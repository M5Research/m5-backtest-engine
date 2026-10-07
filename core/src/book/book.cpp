#include "engine/book.hpp"

namespace engine {
// CompactLadder Implementation
CompactLadder::CompactLadder(FixedPoint start_price, size_t size, FixedPoint tick_size, Side side)
    : quantities_(size, 0), start_price_(start_price), tick_size_(tick_size), size_(size), side_(side), best_idx_cache_(-1) {
    if (tick_size_ <= 0) {
        throw std::invalid_argument("Tick size must be strictly positive.");
    }
}

bool CompactLadder::in_bounds(FixedPoint price) const {
    if (side_ == Side::BUY) {
        // Bids go down from start_price
        return price <= start_price_ && price > start_price_ - static_cast<FixedPoint>(size_) * tick_size_;
    } else {
        // Asks go up from start_price
        return price >= start_price_ && price < start_price_ + static_cast<FixedPoint>(size_) * tick_size_;
    }
}

inline size_t CompactLadder::price_to_index(FixedPoint price) const {
    if (side_ == Side::BUY) {
        return static_cast<size_t>((start_price_ - price) / tick_size_);
    } else {
        return static_cast<size_t>((price - start_price_) / tick_size_);
    }
}

inline FixedPoint CompactLadder::index_to_price(size_t index) const {
    if (side_ == Side::BUY) {
        return start_price_ - static_cast<FixedPoint>(index) * tick_size_;
    } else {
        return start_price_ + static_cast<FixedPoint>(index) * tick_size_;
    }
}

void CompactLadder::invalidate_cache() {
    best_idx_cache_ = -1;
}

bool CompactLadder::update(FixedPoint price, FixedPoint qty) {
    if (!in_bounds(price)) {
        return false;
    }

    size_t idx = price_to_index(price);
    if (idx >= size_) return false; // Sanity check

    quantities_[idx] = qty;
    invalidate_cache(); // Invalidate on every update for now. Can be optimized.
    return true;
}

FixedPoint CompactLadder::get_qty(FixedPoint price) const {
    if (!in_bounds(price)) {
        return 0;
    }
    size_t idx = price_to_index(price);
    return quantities_[idx];
}

FixedPoint CompactLadder::get_best_price() const {
    if (best_idx_cache_ != -1) {
        return index_to_price(static_cast<size_t>(best_idx_cache_));
    }

    for (size_t i = 0; i < size_; ++i) {
        if (quantities_[i] > 0) {
            best_idx_cache_ = static_cast<int64_t>(i);
            return index_to_price(i);
        }
    }
    return -1; // Indicates empty
}

void CompactLadder::clear() {
    std::fill(quantities_.begin(), quantities_.end(), 0);
    invalidate_cache();
}
// OrderBookL2 Implementation
OrderBookL2::OrderBookL2(uint16_t symbol_id, FixedPoint tick_size)
    : symbol_id_(symbol_id), tick_size_(tick_size), is_initialized_(false) {
}

void OrderBookL2::update_sparse(FixedPoint price, FixedPoint qty, Side side) {
    if (side == Side::BUY) {
        if (qty == 0) {
            sparse_bids_.erase(price);
        } else {
            sparse_bids_[price] = qty;
        }
    } else {
        if (qty == 0) {
            sparse_asks_.erase(price);
        } else {
            sparse_asks_[price] = qty;
        }
    }
}

void OrderBookL2::apply_diff(const DepthUpdatePayload& diff) {
    // Depth updates go directly to the sparse price maps.
    update_sparse(diff.price, diff.qty, diff.side);
}

FixedPoint OrderBookL2::get_best_bid() const {
    if (sparse_bids_.empty()) {
        return -1;
    }
    return sparse_bids_.begin()->first;
}

FixedPoint OrderBookL2::get_best_ask() const {
    if (sparse_asks_.empty()) {
        return -1;
    }
    return sparse_asks_.begin()->first;
}

FixedPoint OrderBookL2::get_qty_at_price(FixedPoint price, Side side) const {
    if (side == Side::BUY) {
        auto it = sparse_bids_.find(price);
        if (it != sparse_bids_.end()) {
            return it->second;
        }
    } else {
        auto it = sparse_asks_.find(price);
        if (it != sparse_asks_.end()) {
            return it->second;
        }
    }
    return 0;
}

WalkResult OrderBookL2::walk_levels(Side taker_side, FixedPoint target_qty, FixedPoint limit_price) const {
    WalkResult result{};
    FixedPoint remaining = target_qty;
    FixedPoint total_cost = 0;

    if (taker_side == Side::BUY) {
        // Walk asks from lowest (best) upward
        for (const auto& [price, qty] : sparse_asks_) {
            if (remaining <= 0) break;
            if (limit_price > 0 && price > limit_price) break; // Exceeded limit price
            FixedPoint fill = std::min(remaining, qty);
            total_cost += price * fill;
            remaining -= fill;
            result.filled_qty += fill;
            result.levels_hit++;
        }
    } else {
        // Walk bids from highest (best) downward
        for (const auto& [price, qty] : sparse_bids_) {
            if (remaining <= 0) break;
            if (limit_price > 0 && price < limit_price) break; // Below limit price
            FixedPoint fill = std::min(remaining, qty);
            total_cost += price * fill;
            remaining -= fill;
            result.filled_qty += fill;
            result.levels_hit++;
        }
    }

    if (result.filled_qty > 0) {
        result.vwap = total_cost / result.filled_qty;
    }
    return result;
}

bool OrderBookL2::is_crossed() const {
    FixedPoint bid = get_best_bid();
    FixedPoint ask = get_best_ask();
    return (bid > 0 && ask > 0 && bid >= ask);
}

void OrderBookL2::clear() {
    sparse_bids_.clear();
    sparse_asks_.clear();
    is_initialized_ = false;
}

} // namespace engine
