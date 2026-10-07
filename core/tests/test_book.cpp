// Test: OrderBookL2 and CompactLadder
// Fixed-point invariants, snapshot/diff sync.
#include "engine/book.hpp"
#include "engine/events.hpp"
#include "engine/symbol_metadata.hpp"

#include <cassert>
#include <iostream>

using namespace engine;

void test_compact_ladder_basic() {
    // Bid ladder: prices go down from 50000
    CompactLadder bids(50000, 100, 1, Side::BUY);

    assert(bids.in_bounds(50000));
    assert(bids.in_bounds(49950));
    assert(!bids.in_bounds(50001)); // Above start
    assert(!bids.in_bounds(49899)); // Below range

    // Update and read
    assert(bids.update(50000, 1000));
    assert(bids.get_qty(50000) == 1000);
    assert(bids.get_best_price() == 50000);

    // Update deeper level
    assert(bids.update(49990, 500));
    assert(bids.get_best_price() == 50000); // Best is still 50000

    // Remove best level
    assert(bids.update(50000, 0));
    assert(bids.get_best_price() == 49990); // Now best is 49990

    std::cout << "  [PASS] compact_ladder_basic\n";
}

void test_compact_ladder_ask() {
    // Ask ladder: prices go up from 50001
    CompactLadder asks(50001, 100, 1, Side::SELL);

    assert(asks.in_bounds(50001));
    assert(asks.in_bounds(50050));
    assert(!asks.in_bounds(50000)); // Below start

    asks.update(50005, 200);
    asks.update(50001, 100);
    assert(asks.get_best_price() == 50001); // Lowest ask

    asks.update(50001, 0);
    assert(asks.get_best_price() == 50005);

    std::cout << "  [PASS] compact_ladder_ask\n";
}

void test_order_book_l2() {
    OrderBookL2 book(0, 1); // symbol_id=0, tick_size=1

    // Add bid and ask levels
    DepthUpdatePayload bid_update{};
    bid_update.price = 50000;
    bid_update.qty = 1000;
    bid_update.side = Side::BUY;
    book.apply_diff(bid_update);

    DepthUpdatePayload ask_update{};
    ask_update.price = 50001;
    ask_update.qty = 500;
    ask_update.side = Side::SELL;
    book.apply_diff(ask_update);

    assert(book.get_best_bid() == 50000);
    assert(book.get_best_ask() == 50001);

    // Remove bid level -> next bid should appear
    DepthUpdatePayload bid2{};
    bid2.price = 49999;
    bid2.qty = 300;
    bid2.side = Side::BUY;
    book.apply_diff(bid2);

    DepthUpdatePayload remove_bid{};
    remove_bid.price = 50000;
    remove_bid.qty = 0;
    remove_bid.side = Side::BUY;
    book.apply_diff(remove_bid);

    assert(book.get_best_bid() == 49999);

    // Clear
    book.clear();
    assert(book.get_best_bid() == -1);
    assert(book.get_best_ask() == -1);

    std::cout << "  [PASS] order_book_l2\n";
}

void test_fixed_point_invariant() {
    SymbolMeta meta{};
    meta.symbol_id = 0;
    meta.price_scale = 100; // 2 decimal places

    FixedPoint t1 = meta.price_to_ticks(0.300);
    FixedPoint t2 = meta.price_to_ticks(0.3);
    assert(t1 == t2);
    (void)t1; (void)t2;

    // $50,000.50 with scale 100 -> 5000050
    FixedPoint t3 = meta.price_to_ticks(50000.50);
    assert(t3 == 5000050);
    (void)t3;

    std::cout << "  [PASS] fixed_point_invariant\n";
}

int main() {
    std::cout << "Running OrderBook tests...\n";
    test_compact_ladder_basic();
    test_compact_ladder_ask();
    test_order_book_l2();
    test_fixed_point_invariant();
    std::cout << "All OrderBook tests passed.\n";
    return 0;
}
