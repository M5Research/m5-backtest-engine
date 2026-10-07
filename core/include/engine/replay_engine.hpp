#pragma once

#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <iostream>

#include "engine/types.hpp"
#include "engine/events.hpp"
#include "engine/scheduler.hpp"
#include "engine/book.hpp"
#include "engine/feed.hpp"
#include "engine/execution_model.hpp"
#include "engine/risk.hpp"
#include "engine/symbol_metadata.hpp"

namespace engine {
// Strategy Callback Interface
// The C++ engine calls these methods for market events and fills.
// Pybind11 trampoline classes will override these with Python methods.
class StrategyCallback {
public:
    virtual ~StrategyCallback() = default;

    // Called once before the replay loop starts
    virtual void on_init(const SymbolRegistry& registry) { (void)registry; }

    // Called on depth diff or public trade events
    // strategy_->on_market_event(event)
    virtual void on_market_event(const MarketEvent& event,
                                 const OrderBookL2& book) {
        (void)event; (void)book;
    }

    // Called with a batch of events to reduce Python GIL overhead
    virtual void on_market_event_batch(const std::vector<MarketEvent>& events) {
        (void)events;
    }

    // Called when a simulated fill is generated
    virtual void on_fill(const ExecutionReportPayload& fill,
                         uint16_t symbol_id) {
        (void)fill; (void)symbol_id;
    }
};
// Engine Statistics
struct EngineStats {
    uint64_t events_processed    = 0;
    uint64_t depth_events        = 0;
    uint64_t trade_events        = 0;
    uint64_t order_events        = 0;
    uint64_t fill_events         = 0;
    uint64_t sequence_gaps       = 0;
    int64_t  first_event_ts      = 0;
    int64_t  last_event_ts       = 0;
};
// Replay Engine
// Dispatch events to the order book, execution model, risk manager, and strategy.
class ReplayEngine {
public:
    ReplayEngine(SymbolRegistry registry,
                 LatencyConfig latency = {},
                 QueueConfig queue = {},
                 RiskLimits risk_limits = {},
                 FixedPoint initial_balance = 0)
        : registry_(std::move(registry))
        , execution_(latency, queue, &registry_) // pass registry for fee lookup
        , risk_(risk_limits, registry_, initial_balance)
    {
        // Pre-allocate books for all registered symbols
        // (Using sparse map for now; CompactLadder upgrade once snapshots arrive)
    }

    // Set the strategy callback (Python or C++ test strategy)
    void set_strategy(StrategyCallback* strategy) {
        strategy_ = strategy;
    }

    void set_batch_mode(bool batch_mode) {
        batch_mode_ = batch_mode;
    }

    // Add a pre-sorted event stream to the scheduler
    void add_stream(const MarketEvent* data, size_t count) {
        scheduler_.add_stream(data, count);
    }

    void submit_order(uint16_t symbol_id, const OrderSubmitPayload& payload) {
        MarketEvent event{};
        event.exchange_ts = stats_.last_event_ts;
        event.local_ts    = stats_.last_event_ts;
        event.sequence_id = ++internal_seq_;
        event.symbol_id   = symbol_id;
        event.type        = EventType::ORDER_SUBMIT;
        event.payload.order_submit = payload;
        scheduler_.push_internal(event);
    }

    void cancel_order(uint16_t symbol_id, uint64_t order_id) {
        MarketEvent event{};
        event.exchange_ts = stats_.last_event_ts;
        event.local_ts    = stats_.last_event_ts;
        event.sequence_id = ++internal_seq_;
        event.symbol_id   = symbol_id;
        event.type        = EventType::ORDER_CANCEL;
        event.payload.order_submit.order_id = order_id;
        scheduler_.push_internal(event);
    }

    // Process events in scheduler order.
    void run() {
        if (strategy_) {
            strategy_->on_init(registry_);
        }

        MarketEvent event{};
        std::vector<MarketEvent> batch_buffer;
        batch_buffer.reserve(1024);

        while (scheduler_.pop(event)) {
            process_single_event(event, batch_buffer);
        }

        if (strategy_ && !batch_buffer.empty()) {
            strategy_->on_market_event_batch(batch_buffer);
        }
    }

    void process_single_event(const MarketEvent& event, std::vector<MarketEvent>& batch_buffer) {
        stats_.events_processed++;

        if (stats_.first_event_ts == 0) {
            stats_.first_event_ts = event.exchange_ts;
        }
        stats_.last_event_ts = event.exchange_ts;

        try {
            ensure_book(event.symbol_id);
        } catch (const std::exception& e) {
            std::cerr << "Error processing event type: " << static_cast<int>(event.type)
                      << " symbol_id: " << event.symbol_id << " error: " << e.what() << std::endl;
            throw;
        }

        switch (event.type) {
            case EventType::DEPTH_SNAPSHOT: {
                ensure_validator(event.symbol_id);
                auto& last_snap = last_snapshot_seq_[event.symbol_id];
                if (event.sequence_id != last_snap) {
                    books_[event.symbol_id].clear();
                    last_snap = event.sequence_id;
                    sequence_validators_.at(event.symbol_id).set_snapshot_id(event.sequence_id);
                }
                books_[event.symbol_id].apply_diff(event.payload.depth);
                stats_.depth_events++;
                break;
            }

            case EventType::DEPTH_DIFF: {
                ensure_validator(event.symbol_id);
                auto& validator = sequence_validators_.at(event.symbol_id);
                if (validator.is_synced()) {
                    auto status = validator.validate(event.sequence_id, event.sequence_id);
                    if (status == SequenceValidator::Status::GAP_DETECTED) {
                        books_[event.symbol_id].clear();
                        stats_.sequence_gaps++;
                    }
                }
                books_[event.symbol_id].apply_diff(event.payload.depth);
                stats_.depth_events++;
                break;
            }

            case EventType::PUBLIC_TRADE:
                execution_.process_public_trade(
                    event.payload.trade,
                    event.symbol_id,
                    event.exchange_ts,
                    books_[event.symbol_id]
                );
                risk_.mark_to_market(event.symbol_id, event.payload.trade.price);
                if (risk_.should_liquidate()) {
                    risk_.liquidate_all_positions();
                    execution_.cancel_all_orders();
                }
                stats_.trade_events++;
                break;

            case EventType::ORDER_SUBMIT:
                if (risk_.check_order(event.payload.order_submit, event.symbol_id)) {
                    execution_.submit_order(event.payload.order_submit, event.symbol_id, event.exchange_ts);
                    stats_.order_events++;
                }
                break;

            case EventType::ORDER_CANCEL:
                execution_.cancel_order(event.payload.order_submit.order_id, event.exchange_ts);
                break;

            case EventType::EXECUTION_REPORT: {
                const auto& fill = event.payload.exec_report;
                // Use fill.side directly; the order was already
                // erased from active_orders_ by tick_fills() before this event
                // is processed, so the old lookup always defaulted to BUY.
                risk_.on_fill(fill, event.symbol_id, fill.side);

                if (strategy_) {
                    strategy_->on_fill(fill, event.symbol_id);
                }
                stats_.fill_events++;
                break;
            }

            case EventType::FUNDING_RATE: {
                risk_.on_funding(event.symbol_id, event.payload.funding.rate);
                break;
            }
        }

        if (event.type == EventType::DEPTH_DIFF ||
            event.type == EventType::DEPTH_SNAPSHOT ||
            event.type == EventType::PUBLIC_TRADE) {
            if (strategy_) {
                if (batch_mode_) {
                    batch_buffer.push_back(event);
                    if (batch_buffer.size() >= 1024) {
                        strategy_->on_market_event_batch(batch_buffer);
                        batch_buffer.clear();
                    }
                } else {
                    strategy_->on_market_event(event, books_[event.symbol_id]);
                }
            }
        }

        execution_.tick_fills(event.exchange_ts, scheduler_, books_);
    }

    // Process a single event directly (used for Live Trading integration)
    void inject_event(const MarketEvent& event) {
        std::vector<MarketEvent> dummy_buffer; // Batching is generally disabled for live tick-by-tick
        process_single_event(event, dummy_buffer);
        if (strategy_ && !dummy_buffer.empty()) {
            strategy_->on_market_event_batch(dummy_buffer);
        }
    }

    // Accessors
    [[nodiscard]] const EngineStats& stats() const { return stats_; }
    [[nodiscard]] const ExecutionModel& execution() const { return execution_; }
    [[nodiscard]] const RiskManager& risk() const { return risk_; }
    [[nodiscard]] const SymbolRegistry& registry() const { return registry_; }

    [[nodiscard]] const OrderBookL2& book(uint16_t symbol_id) const {
        auto it = books_.find(symbol_id);
        if (it == books_.end()) {
            throw std::out_of_range("No book for symbol_id: " + std::to_string(symbol_id));
        }
        return it->second;
    }

private:
    void ensure_book(uint16_t symbol_id) {
        if (books_.find(symbol_id) == books_.end()) {
            const auto& meta = registry_.get(symbol_id);
            books_.emplace(symbol_id, OrderBookL2(symbol_id, meta.tick_size));
        }
    }

    void ensure_validator(uint16_t symbol_id) {
        if (sequence_validators_.find(symbol_id) == sequence_validators_.end()) {
            sequence_validators_.emplace(symbol_id, SequenceValidator(symbol_id));
        }
    }

    SymbolRegistry registry_;
    EventScheduler scheduler_;
    ExecutionModel execution_;
    RiskManager    risk_;
    std::unordered_map<uint16_t, OrderBookL2> books_;
    std::unordered_map<uint16_t, SequenceValidator> sequence_validators_;
    std::unordered_map<uint16_t, uint64_t> last_snapshot_seq_;
    StrategyCallback* strategy_ = nullptr;
    EngineStats stats_;
    uint64_t internal_seq_ = 0;
    bool batch_mode_ = false;
};

} // namespace engine
