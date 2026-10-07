#pragma once

#include <vector>
#include <map>
#include <cstdint>
#include <stdexcept>
#include <iostream>

#include "engine/types.hpp"
#include "engine/events.hpp"
#include <algorithm>

namespace engine {
// Compact Ladder
// Represents a dense section of the order book (e.g., top 10,000 ticks).
// Provides O(1) lookups and updates.
class CompactLadder {
public:
    CompactLadder(FixedPoint start_price, size_t size, FixedPoint tick_size, Side side);

    // Updates the quantity at a given price level. Returns true if within ladder bounds.
    bool update(FixedPoint price, FixedPoint qty);

    // Retrieves quantity at a price level. Returns 0 if outside bounds.
    FixedPoint get_qty(FixedPoint price) const;

    // Fast lookup for the best price in this dense ladder.
    // Returns -1 (or appropriate invalid marker) if ladder is empty.
    FixedPoint get_best_price() const;

    // Resets the ladder state.
    void clear();

    // Check if price falls within the pre-allocated ladder range.
    bool in_bounds(FixedPoint price) const;

private:
    std::vector<FixedPoint> quantities_;
    FixedPoint start_price_;
    FixedPoint tick_size_;
    size_t size_;
    Side side_;

    // Caches the index of the best price to avoid full scans
    mutable int64_t best_idx_cache_;

    inline size_t price_to_index(FixedPoint price) const;
    inline FixedPoint index_to_price(size_t index) const;
    void invalidate_cache();
};
// Walk Result; returned by walk_levels for market order slippage
struct WalkResult {
    FixedPoint vwap        = 0; // Volume-weighted average fill price
    FixedPoint filled_qty  = 0; // Total qty actually filled
    int        levels_hit  = 0; // Number of price levels consumed
};
// L2 Order Book
// Hybrid structure: CompactLadder for the active trading zone, and std::map
// for deep/outlier levels.
class OrderBookL2 {
public:
    // Default constructor for container compatibility
    OrderBookL2() : symbol_id_(0), tick_size_(1), is_initialized_(false) {}

    // Initialize the order book with metadata for fixed-point scaling
    OrderBookL2(uint16_t symbol_id, FixedPoint tick_size);

    // Process a depth difference event
    void apply_diff(const DepthUpdatePayload& diff);

    // Get the current best bid (highest price someone is willing to buy at)
    FixedPoint get_best_bid() const;

    // Get the current best ask (lowest price someone is willing to sell at)
    FixedPoint get_best_ask() const;

    // Get the quantity at a specific price level
    FixedPoint get_qty_at_price(FixedPoint price, Side side) const;

    // Walk price levels for market order slippage and limit crossing.
    // taker_side=BUY walks asks up, SELL walks bids down.
    // If limit_price > 0, halts walk if price crosses limit_price.
    WalkResult walk_levels(Side taker_side, FixedPoint target_qty, FixedPoint limit_price = 0) const;

    FixedPoint tick_size() const { return tick_size_; }

    // Check if the order book is crossed
    bool is_crossed() const;

    // Flush the entire book (used when a sequence gap is detected)
    void clear();

private:
    uint16_t symbol_id_;
    FixedPoint tick_size_;

    // To properly initialize ladders, we need an anchor price.
    // In a real system, the first snapshot initializes the ladder boundaries.
    bool is_initialized_;

    // Pointers so we can dynamically allocate the ladders once the first snapshot arrives.
    // For now, statically allocate based on an assumed anchor or defer until snapshot.
    // std::unique_ptr<CompactLadder> active_bids_;
    // std::unique_ptr<CompactLadder> active_asks_;

    // Sparse representation for deep book or uninitialized state
    std::map<FixedPoint, FixedPoint, std::greater<FixedPoint>> sparse_bids_; // Highest first
    std::map<FixedPoint, FixedPoint, std::less<FixedPoint>> sparse_asks_;    // Lowest first

    void update_sparse(FixedPoint price, FixedPoint qty, Side side);
};

} // namespace engine
