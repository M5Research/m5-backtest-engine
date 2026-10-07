#pragma once

#include <cstdint>
#include <unordered_map>
#include <algorithm>

#include "engine/types.hpp"
#include "engine/events.hpp"
#include "engine/symbol_metadata.hpp"

namespace engine {
// Risk Limits Configuration
// Risk tracking is pushed completely into C++.
struct RiskLimits {
    FixedPoint max_gross_exposure   = 0; // Maximum total |long| + |short| notional
    FixedPoint max_net_exposure     = 0; // Maximum |net| notional
    FixedPoint max_order_size       = 0; // Maximum single order size in base qty
    int32_t    max_open_orders      = 100;
    FixedPoint max_position_per_sym = 0; // Maximum position per symbol
};
// Position Tracker
// VWAP cost-basis position tracking.
struct PositionState {
    FixedPoint quantity       = 0; // Signed: positive=long, negative=short
    FixedPoint cost_basis     = 0; // VWAP entry price (fixed-point)
    FixedPoint realized_pnl   = 0;
    FixedPoint unrealized_pnl = 0;
    FixedPoint total_fees     = 0;
    FixedPoint total_funding  = 0; // Accumulated funding payments
    uint32_t   num_trades     = 0;
};
// Account State
struct AccountState {
    FixedPoint balance         = 0; // Total collateral
    FixedPoint maintenance_margin = 0;
    FixedPoint initial_margin     = 0;
    MarginMode margin_mode        = MarginMode::CROSS;
    bool       is_liquidated      = false;
};
// Risk Manager
class RiskManager {
public:
    RiskManager() = default;

    explicit RiskManager(RiskLimits limits, const SymbolRegistry& registry, FixedPoint initial_balance = 0)
        : limits_(limits), registry_(&registry) {
        account_.balance = initial_balance;
    }

    // Evaluate whether an order should be accepted.
    // Returns true if the order passes all risk checks.
    [[nodiscard]] bool check_order(const OrderSubmitPayload& order, uint16_t symbol_id) const {
        if (account_.is_liquidated) {
            return false;
        }

        // Check 1: Maximum order size
        if (limits_.max_order_size > 0 && order.qty > limits_.max_order_size) {
            return false;
        }

        // Check 2: Maximum open orders
        int32_t open_count = 0;
        for (const auto& [sym, pos] : positions_) {
            if (pos.quantity != 0) ++open_count;
        }
        if (open_count >= limits_.max_open_orders) {
            return false;
        }

        // Check 3: Margin Check; use initial_margin for new orders
        // Maintenance margin is for liquidation, initial margin is for order acceptance.
        FixedPoint order_notional = order.price * order.qty; // Simplified
        FixedPoint current_equity = account_.balance + total_realized_pnl() + total_unrealized_pnl();
        if (current_equity < account_.initial_margin) {
            return false;
        }

        // Check 4: Maximum position per symbol
        if (limits_.max_position_per_sym > 0) {
            auto it = positions_.find(symbol_id);
            if (it != positions_.end()) {
                FixedPoint projected = it->second.quantity;
                if (order.side == Side::BUY) {
                    projected += order.qty;
                } else {
                    projected -= order.qty;
                }
                if (std::abs(projected) > limits_.max_position_per_sym) {
                    return false;
                }
            }
        }

        // Check 5: Gross exposure limit
        if (limits_.max_gross_exposure > 0) {
            FixedPoint gross = compute_gross_exposure();
            if (gross + order_notional > limits_.max_gross_exposure) {
                return false;
            }
        }

        return true;
    }

    // Update position on fill. Implements VWAP cost basis.
    void on_fill(const ExecutionReportPayload& fill, uint16_t symbol_id, Side side) {
        auto& pos = positions_[symbol_id];
        pos.num_trades++;
        pos.total_fees += fill.fee;
        account_.balance -= fill.fee; // Deduct fees from balance

        if (side == Side::BUY) {
            if (pos.quantity >= 0) {
                // Adding to long: update VWAP
                FixedPoint total_cost = pos.cost_basis * pos.quantity + fill.fill_price * fill.fill_qty;
                pos.quantity += fill.fill_qty;
                pos.cost_basis = (pos.quantity != 0) ? total_cost / pos.quantity : 0;
            } else {
                // Covering short
                FixedPoint closed = std::min(fill.fill_qty, -pos.quantity);
                pos.realized_pnl += closed * (pos.cost_basis - fill.fill_price);
                pos.quantity += fill.fill_qty;
                if (pos.quantity > 0) {
                    pos.cost_basis = fill.fill_price;
                }
            }
        } else {
            if (pos.quantity <= 0) {
                // Adding to short: update VWAP
                FixedPoint total_cost = std::abs(pos.cost_basis * pos.quantity) + fill.fill_price * fill.fill_qty;
                pos.quantity -= fill.fill_qty;
                pos.cost_basis = (pos.quantity != 0) ? total_cost / std::abs(pos.quantity) : 0;
            } else {
                // Closing long
                FixedPoint closed = std::min(fill.fill_qty, pos.quantity);
                pos.realized_pnl += closed * (fill.fill_price - pos.cost_basis);
                pos.quantity -= fill.fill_qty;
                if (pos.quantity < 0) {
                    pos.cost_basis = fill.fill_price;
                }
            }
        }
        update_margin();
    }

    void on_funding(uint16_t symbol_id, FixedPoint rate) {
        auto& pos = positions_[symbol_id];
        if (pos.quantity == 0) return;

        FixedPoint notional = 0;
        if (registry_) {
            try {
                const auto& meta = registry_->get(symbol_id);
                notional = pos.quantity * pos.cost_basis / meta.qty_scale;
            } catch (...) {
                notional = pos.quantity * pos.cost_basis;
            }
        } else {
            notional = pos.quantity * pos.cost_basis;
        }

        // Funding rate is typically in 1e6 scale, meaning 0.01% = 0.0001 = 100.
        // payment = notional * rate / 1000000.
        FixedPoint payment = (notional * rate) / 1000000;
        pos.total_funding -= payment; // Payment is deducted if positive and long
        pos.realized_pnl -= payment;
        account_.balance -= payment;
        update_margin();
    }

    // Mark-to-market all positions
    void mark_to_market(uint16_t symbol_id, FixedPoint mark_price) {
        auto it = positions_.find(symbol_id);
        if (it != positions_.end() && it->second.quantity != 0) {
            it->second.unrealized_pnl =
                it->second.quantity * (mark_price - it->second.cost_basis);
        }
        update_margin();
    }

    bool should_liquidate() const {
        return !account_.is_liquidated && equity() < account_.maintenance_margin && has_active_positions();
    }

    bool has_active_positions() const {
        for (const auto& [sym, pos] : positions_) {
            if (pos.quantity != 0) return true;
        }
        return false;
    }

    void liquidate_all_positions() {
        if (account_.is_liquidated) return;
        account_.is_liquidated = true;

        for (auto& [sym, pos] : positions_) {
            if (pos.quantity != 0) {
                pos.realized_pnl += pos.unrealized_pnl;
                pos.quantity = 0;
                pos.cost_basis = 0;
                pos.unrealized_pnl = 0;
            }
        }

        account_.balance = equity();
        account_.initial_margin = 0;
        account_.maintenance_margin = 0;
    }

    void update_margin() {
        FixedPoint total_im = 0;
        FixedPoint total_mm = 0;
        for (const auto& [sym, pos] : positions_) {
            FixedPoint notional = std::abs(pos.quantity * pos.cost_basis);
            if (registry_) {
                try {
                    const auto& meta = registry_->get(sym);
                    notional = notional / meta.qty_scale;
                } catch (...) {}
            }
            total_im += notional / 10; // 10% Initial Margin (10x leverage)
            total_mm += notional / 20; // 5% Maintenance Margin
        }
        account_.initial_margin = total_im;
        account_.maintenance_margin = total_mm;
    }

    [[nodiscard]] const auto& positions() const { return positions_; }
    [[nodiscard]] const AccountState& account() const { return account_; }
    [[nodiscard]] const AccountState& account_state() const { return account_; }

    [[nodiscard]] const PositionState& position(uint16_t symbol_id) const {
        auto it = positions_.find(symbol_id);
        if (it != positions_.end()) {
            return it->second;
        }
        static const PositionState empty{};
        return empty;
    }

    [[nodiscard]] FixedPoint equity() const {
        return account_.balance + total_realized_pnl() + total_unrealized_pnl();
    }

    [[nodiscard]] FixedPoint compute_gross_exposure() const {
        FixedPoint gross = 0;
        for (const auto& [sym, pos] : positions_) {
            gross += std::abs(pos.quantity * pos.cost_basis);
        }
        return gross;
    }

    [[nodiscard]] FixedPoint compute_net_exposure() const {
        FixedPoint net = 0;
        for (const auto& [sym, pos] : positions_) {
            net += pos.quantity * pos.cost_basis;
        }
        return net;
    }

    [[nodiscard]] FixedPoint total_realized_pnl() const {
        FixedPoint total = 0;
        for (const auto& [sym, pos] : positions_) {
            total += pos.realized_pnl;
        }
        return total;
    }

    [[nodiscard]] FixedPoint total_unrealized_pnl() const {
        FixedPoint total = 0;
        for (const auto& [sym, pos] : positions_) {
            total += pos.unrealized_pnl;
        }
        return total;
    }

private:
    RiskLimits limits_;
    const SymbolRegistry* registry_ = nullptr;
    std::unordered_map<uint16_t, PositionState> positions_;
    AccountState account_;
};

} // namespace engine
