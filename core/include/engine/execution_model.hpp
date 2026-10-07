#pragma once

#include <unordered_map>
#include <map>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <random>

#include "engine/types.hpp"
#include "engine/events.hpp"
#include "engine/scheduler.hpp"
#include "engine/book.hpp"
#include "engine/symbol_metadata.hpp"

namespace engine {
// Latency Model
// Submitted orders are delayed by a simulated
// network + exchange processing latency before entering the matching pool.
struct LatencyConfig {
    int64_t submission_ns   = 50'000'000;  // 50ms in nanoseconds
    int64_t cancellation_ns = 30'000'000;  // 30ms
    int64_t jitter_ns       = 10'000'000;  // 10ms uniform jitter
    uint64_t rng_seed       = 42;          // Deterministic PRNG seed
};
// Queue Depletion Config
// Queue position is reduced by a
// configured volume multiplier and per-event cancellation decay.
struct QueueConfig {
    double depletion_rate           = 0.7;   // Fraction of trade volume that depletes queue
    double cancel_decay_per_event   = 0.995; // Per-event multiplicative decay
    double initial_depth_fraction   = 1.0;   // Fraction of displayed volume ahead of the order
};
// Simulated Order
struct SimulatedOrder {
    uint64_t   order_id;
    uint16_t   symbol_id;
    Side       side;
    FixedPoint price;
    FixedPoint limit_price     = 0;
    FixedPoint qty;
    FixedPoint filled_qty      = 0;
    FixedPoint pending_fill    = 0;  // Qty to report in next tick_fills
    FixedPoint queue_position  = 0;  // Volume ahead of us in the queue
    int64_t    submit_ts       = 0;  // When strategy submitted
    int64_t    activate_ts     = 0;  // When order "arrives" at exchange
    int64_t    cancel_activate_ts = 0;
    bool       is_active       = false;
    bool       is_filled       = false;
    bool       is_cancelled    = false;
    bool       pending_cancel  = false;
    bool       is_maker        = true;
    OrderType  type            = OrderType::LIMIT;
    TimeInForce tif            = TimeInForce::GTC;
};
// Symbol Execution State
struct SymbolExecutionState {
    FixedPoint recent_buy_volume  = 0;
    FixedPoint recent_sell_volume = 0;
    int64_t    last_update_ts     = 0;
    double     current_ofi        = 0.0; // [-1.0, 1.0] where >0 is buy-heavy
};
// Execution Model
// Implements the execution simulator with the following behavior:
//   1. Latency queue: orders delayed before entering matching pool
//   2. Queue position estimation from displayed L2 volume
//   3. Probabilistic queue depletion on public trades
//   4. Fill generation when queue_position <= 0 (with partial fills)
//   5. Market orders walk the L2 book with slippage
//   6. Fees computed from SymbolMeta
//   7. Latency jitter from deterministic PRNG
class ExecutionModel {
public:
    ExecutionModel() = default;

    explicit ExecutionModel(LatencyConfig lat, QueueConfig queue,
                            const SymbolRegistry* registry = nullptr)
        : latency_(lat), queue_cfg_(queue), registry_(registry)
        , rng_(lat.rng_seed)
        , jitter_dist_(-lat.jitter_ns, lat.jitter_ns)
    {}

    // Submit an order from the strategy. It enters the latency queue.
    void submit_order(const OrderSubmitPayload& order, uint16_t symbol_id, int64_t current_ts) {
        SimulatedOrder sim;
        sim.order_id    = order.order_id;
        sim.symbol_id   = symbol_id;
        sim.side        = order.side;
        sim.price       = order.price;
        sim.limit_price = order.price;
        sim.qty         = order.qty;
        sim.type        = order.type;
        sim.tif         = order.tif;
        sim.is_maker    = (order.type != OrderType::MARKET);
        sim.submit_ts   = current_ts;

        // Apply latency jitter for realistic timing
        int64_t jitter = 0;
        if (latency_.jitter_ns > 0) {
            jitter = jitter_dist_(rng_);
        }
        int64_t total_latency = std::max(int64_t(0), latency_.submission_ns + jitter);
        sim.activate_ts = current_ts + total_latency;

        sim.is_active   = false;
        sim.filled_qty  = 0;
        sim.pending_fill = 0;

        active_orders_[order.order_id] = sim;
        ++stats_.total_orders;
    }

    // Cancel an active order.
    bool cancel_order(uint64_t order_id, int64_t current_ts = 0) {
        auto it = active_orders_.find(order_id);
        if (it == active_orders_.end()) return false;
        if (it->second.is_cancelled || it->second.is_filled) return false;

        if (current_ts == 0) {
            // Instant cancellation (e.g. for testing/compatibility)
            it->second.is_cancelled = true;
            ++stats_.total_cancels;
        } else {
            // Apply cancellation latency
            it->second.pending_cancel = true;
            it->second.cancel_activate_ts = current_ts + latency_.cancellation_ns;
            ++stats_.total_cancels;
        }
        return true;
    }

    void activate_order(SimulatedOrder& order, const OrderBookL2& book, int64_t current_ts) {
        (void)current_ts; // Suppress unused parameter warning
        order.is_active = true;

        if (order.type == OrderType::MARKET) {
            // Market order walks the L2 book
            WalkResult res = walk_book(book, order.side, order.qty);
            if (res.filled_qty > 0) {
                order.pending_fill = res.filled_qty;
                order.filled_qty = res.filled_qty;
                order.price = res.vwap;
                order.is_maker = false;
                if (order.filled_qty >= order.qty) {
                    order.is_filled = true;
                } else {
                    // Remaining is cancelled
                    order.is_filled = true;
                }
                ++stats_.total_fills;
                ++stats_.taker_fills;
            } else {
                order.is_cancelled = true;
            }
            return;
        }

        // Limit order
        // Check if crossed
        FixedPoint best_bid = book.get_best_bid();
        FixedPoint best_ask = book.get_best_ask();
        bool crossed = false;
        if (order.side == Side::BUY && best_ask > 0 && order.price >= best_ask) {
            crossed = true;
        } else if (order.side == Side::SELL && best_bid > 0 && order.price <= best_bid) {
            crossed = true;
        }

        if (crossed) {
            if (order.type == OrderType::LIMIT_MAKER) {
                // Post-Only rejection
                order.is_cancelled = true;
                return;
            }

            // Crosses the spread, walk asks up or bids down up to order.price
            WalkResult res = book.walk_levels(order.side, order.qty, order.price);

            if (order.tif == TimeInForce::FOK) {
                if (res.filled_qty < order.qty) {
                    // FOK failed
                    order.is_cancelled = true;
                } else {
                    // FOK succeeded
                    order.pending_fill = res.filled_qty;
                    order.filled_qty = res.filled_qty;
                    order.price = res.vwap;
                    order.is_filled = true;
                    order.is_maker = false;
                    ++stats_.total_fills;
                    ++stats_.taker_fills;
                }
            } else if (order.tif == TimeInForce::IOC) {
                if (res.filled_qty > 0) {
                    order.pending_fill = res.filled_qty;
                    order.filled_qty = res.filled_qty;
                    order.price = res.vwap;
                    order.is_filled = true;
                    order.is_maker = false;
                    ++stats_.total_fills;
                    ++stats_.taker_fills;
                } else {
                    order.is_cancelled = true;
                }
            } else { // GTC
                if (res.filled_qty > 0) {
                    order.pending_fill = res.filled_qty;
                    order.filled_qty += res.filled_qty;
                    order.price = res.vwap; // Temporary store match price
                    order.is_maker = false;
                    if (order.filled_qty >= order.qty) {
                        order.is_filled = true;
                    } else {
                        order.queue_position = 0; // Front of queue
                    }
                    ++stats_.total_fills;
                    ++stats_.taker_fills;
                } else {
                    order.queue_position = 0; // Front of queue
                }
            }
        } else {
            // Not crossed
            if (order.tif == TimeInForce::IOC || order.tif == TimeInForce::FOK) {
                order.is_cancelled = true;
                return;
            }

            // Normal rest in queue
            FixedPoint depth_ahead = 0;
            if (order.side == Side::BUY && best_bid > 0 && order.price <= best_bid) {
                depth_ahead = book.get_qty_at_price(order.price, Side::BUY);
            } else if (order.side == Side::SELL && best_ask > 0 && order.price >= best_ask) {
                depth_ahead = book.get_qty_at_price(order.price, Side::SELL);
            }
            if (order.queue_position == 0) {
                order.queue_position = static_cast<FixedPoint>(
                    static_cast<double>(depth_ahead) * queue_cfg_.initial_depth_fraction
                );
            }
        }
    }

    // Called on every PUBLIC_TRADE event to deplete queues.
    // queue_position decremented by
    // trade_volume * depletion_rate, with probabilistic decay.
    void process_public_trade(const PublicTradePayload& trade,
                              uint16_t symbol_id,
                              int64_t current_ts,
                              const OrderBookL2& book) {
        // Track OFI (Order Flow Toxicity)
        auto& state = sym_state_[symbol_id];
        // 1-second rolling window for OFI
        if (current_ts - state.last_update_ts > 1'000'000'000) {
            state.recent_buy_volume = 0;
            state.recent_sell_volume = 0;
        }
        if (trade.taker_side == Side::BUY) {
            state.recent_buy_volume += trade.qty;
        } else {
            state.recent_sell_volume += trade.qty;
        }
        state.last_update_ts = current_ts;

        FixedPoint total_vol = state.recent_buy_volume + state.recent_sell_volume;
        if (total_vol > 0) {
            state.current_ofi = static_cast<double>(state.recent_buy_volume - state.recent_sell_volume) / static_cast<double>(total_vol);
        }

        // Determine if it's a Large-Tick or Small-Tick regime based on spread
        FixedPoint best_bid = book.get_best_bid();
        FixedPoint best_ask = book.get_best_ask();
        bool is_small_tick = false;
        if (best_bid > 0 && best_ask > 0) {
            // If spread > 1 tick, it's typically a small-tick regime
            is_small_tick = (best_ask - best_bid) > book.tick_size();
        }

        for (auto& [id, order] : active_orders_) {
            if (order.symbol_id != symbol_id) continue;

            // Check pending cancellation
            if (order.pending_cancel && current_ts >= order.cancel_activate_ts) {
                order.is_cancelled = true;
            }
            if (order.is_filled || order.is_cancelled) continue;

            // Activation if latency window passed
            if (!order.is_active) {
                if (current_ts >= order.activate_ts) {
                    activate_order(order, book, current_ts);
                    if (order.is_filled || order.is_cancelled) continue;
                } else {
                    continue;
                }
            }

            // Only deplete queue if trade is at our price level
            bool price_match = (order.price == trade.price);

            if (price_match) {
                // High-fidelity depletion: only if trade side matches
                bool side_match = (order.side == Side::BUY && trade.taker_side == Side::SELL) ||
                                  (order.side == Side::SELL && trade.taker_side == Side::BUY);

                if (side_match) {
                    // OFI adjustments
                    double ofi_multiplier = 1.0;
                    if (order.side == Side::BUY) {
                        if (state.current_ofi < -0.5) ofi_multiplier = 1.5;
                        else if (state.current_ofi > 0.5) ofi_multiplier = 0.5;
                    } else {
                        if (state.current_ofi > 0.5) ofi_multiplier = 1.5;
                        else if (state.current_ofi < -0.5) ofi_multiplier = 0.5;
                    }

                    // In small-tick regimes, queue position is less reliable
                    if (is_small_tick) {
                        ofi_multiplier *= 1.2;
                    }

                    FixedPoint depletion = static_cast<FixedPoint>(
                        static_cast<double>(trade.qty) * queue_cfg_.depletion_rate * ofi_multiplier
                    );
                    order.queue_position -= depletion;
                }

                order.queue_position = static_cast<FixedPoint>(
                    static_cast<double>(order.queue_position) * queue_cfg_.cancel_decay_per_event
                );
            }

            // Check for through-price fills
            bool through_price = false;
            if (order.side == Side::BUY && trade.price < order.price) {
                through_price = true;
            } else if (order.side == Side::SELL && trade.price > order.price) {
                through_price = true;
            }

            // Partial fills instead of binary all-or-nothing
            if (through_price) {
                // Price traded through our level; fill everything remaining
                FixedPoint remaining = order.qty - order.filled_qty;
                order.pending_fill = remaining;
                order.filled_qty = order.qty;
                order.is_filled = true;
                ++stats_.total_fills;
                ++stats_.maker_fills;
            } else if (price_match && order.queue_position <= 0) {
                // Queue depleted; fill based on volume that passed us
                FixedPoint remaining = order.qty - order.filled_qty;
                FixedPoint overshoot = -order.queue_position;
                FixedPoint fill_amount = std::min(remaining, std::max(overshoot, FixedPoint(1)));

                order.pending_fill = fill_amount;
                order.filled_qty += fill_amount;

                if (order.filled_qty >= order.qty) {
                    order.is_filled = true;
                } else {
                    // Not fully filled yet; reset queue to 0 (we're at front)
                    order.queue_position = 0;
                }
                ++stats_.total_fills;
                ++stats_.maker_fills;
            }
        }
    }

    // Set initial queue position for a newly activated limit order.
    // Called from the engine when we have access to the L2 book.
    void set_queue_depth(uint64_t order_id, FixedPoint depth) {
        auto it = active_orders_.find(order_id);
        if (it != active_orders_.end()) {
            it->second.queue_position = static_cast<FixedPoint>(
                static_cast<double>(depth) * queue_cfg_.initial_depth_fraction
            );
        }
    }

    // Push reports for pending fills into the scheduler.
    void tick_fills(int64_t current_ts, EventScheduler& scheduler, const std::unordered_map<uint16_t, OrderBookL2>& books) {
        // First, check for activations and cancellations that have triggered in this time step
        for (auto& [id, order] : active_orders_) {
            // Check pending cancellation
            if (order.pending_cancel && current_ts >= order.cancel_activate_ts) {
                order.is_cancelled = true;
            }
            if (order.is_filled || order.is_cancelled) continue;

            // Check pending activation
            if (!order.is_active && current_ts >= order.activate_ts) {
                auto book_it = books.find(order.symbol_id);
                if (book_it != books.end()) {
                    activate_order(order, book_it->second, current_ts);
                } else {
                    order.is_active = true; // Fallback
                }
            }
        }

        std::vector<uint64_t> to_remove;

        for (auto& [id, order] : active_orders_) {
            if (order.is_cancelled) {
                to_remove.push_back(id);
                continue;
            }

            // Only generate fill if there's a pending fill amount
            if (order.pending_fill > 0) {
                MarketEvent fill_event{};
                fill_event.exchange_ts = current_ts;
                fill_event.local_ts    = current_ts;
                fill_event.sequence_id = id;
                fill_event.symbol_id   = order.symbol_id;
                fill_event.type        = EventType::EXECUTION_REPORT;
                fill_event.flags       = 0;

                auto& rpt = fill_event.payload.exec_report;
                rpt.order_id       = order.order_id;
                rpt.fill_price     = order.price;
                rpt.fill_qty       = order.pending_fill;
                rpt.leaves_qty     = order.qty - order.filled_qty;
                rpt.side           = order.side; // Embed side
                rpt.is_maker       = order.is_maker; // Enforce maker/taker flag correctly
                rpt.is_fully_filled = (order.filled_qty >= order.qty);

                // Compute fees from SymbolMeta
                rpt.fee = compute_fee(order.symbol_id, rpt.fill_price,
                                      rpt.fill_qty, rpt.is_maker);

                scheduler.push_internal(fill_event);
                order.pending_fill = 0; // Clear pending

                // Restore original limit price for subsequent matches if GTC and partially filled
                if (!order.is_filled) {
                    order.price = order.limit_price;
                }

                if (order.is_filled) {
                    to_remove.push_back(id);
                }
            }
        }

        for (auto id : to_remove) {
            active_orders_.erase(id);
        }
    }

    void tick_fills(int64_t current_ts, EventScheduler& scheduler) {
        std::unordered_map<uint16_t, OrderBookL2> empty_books;
        tick_fills(current_ts, scheduler, empty_books);
    }

    void cancel_all_orders() {
        for (auto& [id, order] : active_orders_) {
            if (!order.is_filled && !order.is_cancelled) {
                order.is_cancelled = true;
                ++stats_.total_cancels;
            }
        }
    }

    // Walk the L2 book for a market order and compute average fill price.
    // Properly walks multiple price levels with slippage.
    WalkResult walk_book(const OrderBookL2& book, Side side, FixedPoint target_qty) const {
        return book.walk_levels(side, target_qty);
    }

    [[nodiscard]] const auto& active_orders() const { return active_orders_; }

    struct Stats {
        uint64_t total_orders = 0;
        uint64_t total_fills  = 0;
        uint64_t total_cancels = 0;
        uint64_t maker_fills  = 0;
        uint64_t taker_fills  = 0;
    };
    [[nodiscard]] const Stats& stats() const { return stats_; }

private:
    // Compute fee from SymbolMeta maker/taker bps.
    // SymbolMeta convention: fee_bps where 10 = 1 basis point.
    // Fee in price_scale units = (price * qty * fee_bps) / (qty_scale * 100000)
    FixedPoint compute_fee(uint16_t symbol_id, FixedPoint fill_price,
                           FixedPoint fill_qty, bool is_maker) const {
        if (!registry_) return 0;
        try {
            const auto& meta = registry_->get(symbol_id);
            FixedPoint fee_bps = is_maker ? meta.maker_fee_bps : meta.taker_fee_bps;
            if (fee_bps == 0) return 0;
            // notional = fill_price * fill_qty / qty_scale (in price_scale units)
            // fee = notional * fee_bps / 100000
            // Combined: fee = fill_price * fill_qty * fee_bps / (qty_scale * 100000)
            // Split to avoid overflow: (fill_price * fee_bps / 100000) * (fill_qty / qty_scale)
            // But this loses precision. Use two-step with intermediate:
            FixedPoint notional = fill_price * fill_qty / meta.qty_scale;
            return notional * fee_bps / 100000;
        } catch (...) {
            return 0;
        }
    }

    LatencyConfig latency_;
    QueueConfig   queue_cfg_;
    const SymbolRegistry* registry_ = nullptr;
    std::map<uint64_t, SimulatedOrder> active_orders_;
    std::unordered_map<uint16_t, SymbolExecutionState> sym_state_;
    Stats stats_;

    // Deterministic PRNG for latency jitter
    std::mt19937_64 rng_{42};
    std::uniform_int_distribution<int64_t> jitter_dist_{0, 0};
};

} // namespace engine
