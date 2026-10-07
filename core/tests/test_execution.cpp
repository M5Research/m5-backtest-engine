// Test: ExecutionModel
// Queue depletion, latency, fill generation.
#include "engine/execution_model.hpp"
#include "engine/scheduler.hpp"
#include "engine/book.hpp"
#include "engine/symbol_metadata.hpp"

#include <cassert>
#include <iostream>
#include <cmath>

using namespace engine;

// Helper: create a registry with one BTC symbol
static SymbolRegistry make_registry() {
    SymbolRegistry reg;
    SymbolMeta btc{};
    btc.symbol_id     = 1;
    btc.symbol_name   = "BTCUSDT";
    btc.price_scale   = 100;
    btc.qty_scale     = 1000;
    btc.tick_size     = 1;
    btc.lot_size      = 1;
    btc.maker_fee_bps = 20;   // 2 bps = 0.02%
    btc.taker_fee_bps = 50;   // 5 bps = 0.05%
    reg.register_symbol(btc);
    return reg;
}

void test_order_latency() {
    LatencyConfig lat;
    lat.submission_ns = 50'000'000; // 50ms
    QueueConfig queue;
    auto reg = make_registry();

    ExecutionModel exec(lat, queue, &reg);
    EventScheduler sched;

    // Submit a limit order at t=0
    OrderSubmitPayload order{};
    order.order_id = 1;
    order.side     = Side::BUY;
    order.price    = 50000;
    order.qty      = 100;
    order.type     = OrderType::LIMIT;
    order.tif      = TimeInForce::GTC;

    exec.submit_order(order, 1, 0);

    // Trade at t=10ms; order should NOT be active yet
    PublicTradePayload trade{};
    trade.price      = 50000;
    trade.qty        = 10000;
    trade.taker_side = Side::SELL;

    OrderBookL2 book(1, 1);
    exec.process_public_trade(trade, 1, 10'000'000, book); // 10ms, use symbol_id=1

    // Check: order should still be resting (not active, not filled)
    const auto& orders = exec.active_orders();
    assert(orders.count(1));
    assert(!orders.at(1).is_filled);
    assert(!orders.at(1).is_active);
    (void)orders;

    std::cout << "  [PASS] order_latency\n";
}

void test_queue_depletion_fill() {
    LatencyConfig lat;
    lat.submission_ns = 0; // No latency for this test
    lat.jitter_ns     = 0; // No jitter

    QueueConfig queue;
    queue.depletion_rate = 1.0;        // Full depletion
    queue.cancel_decay_per_event = 1.0; // No decay
    queue.initial_depth_fraction = 1.0;

    auto reg = make_registry();
    ExecutionModel exec(lat, queue, &reg);

    // Submit a buy limit order
    OrderSubmitPayload order{};
    order.order_id = 42;
    order.side     = Side::BUY;
    order.price    = 50000;
    order.qty      = 100;
    order.type     = OrderType::LIMIT;
    order.tif      = TimeInForce::GTC;

    exec.submit_order(order, 1, 0);

    // Manually set queue depth to 1000 (larger to account for OFI multiplier)
    exec.set_queue_depth(42, 1000);

    // Process trades that partially deplete the queue
    // OFI: all SELL taker trades -> OFI ~ -1.0 -> multiplier=1.5 for BUY orders
    // Effective depletion per 200 trade: 200 * 1.0 * 1.5 = 300
    PublicTradePayload trade{};
    trade.price      = 50000;
    trade.taker_side = Side::SELL;

    OrderBookL2 book(1, 1);
    // Trade 200 units -> depletes ~300 from queue (1000->700)
    trade.qty = 200;
    exec.process_public_trade(trade, 1, 1'000'000, book);
    assert(!exec.active_orders().at(42).is_filled);

    // Trade 200 more -> depletes ~300 (700->400)
    trade.qty = 200;
    exec.process_public_trade(trade, 1, 2'000'000, book);
    assert(!exec.active_orders().at(42).is_filled);

    // Trade 200 more -> depletes ~300 (400->100)
    trade.qty = 200;
    exec.process_public_trade(trade, 1, 3'000'000, book);
    assert(!exec.active_orders().at(42).is_filled);

    // Trade 200 more -> depletes ~300 (100->-200), fills
    trade.qty = 200;
    exec.process_public_trade(trade, 1, 4'000'000, book);
    assert(exec.active_orders().at(42).filled_qty > 0);

    std::cout << "  [PASS] queue_depletion_fill\n";
}

void test_through_price_fill() {
    LatencyConfig lat;
    lat.submission_ns = 0;
    lat.jitter_ns     = 0;
    QueueConfig queue;
    auto reg = make_registry();

    ExecutionModel exec(lat, queue, &reg);

    // Buy limit at 50000
    OrderSubmitPayload order{};
    order.order_id = 99;
    order.side     = Side::BUY;
    order.price    = 50000;
    order.qty      = 100;
    order.type     = OrderType::LIMIT;
    order.tif      = TimeInForce::GTC;

    exec.submit_order(order, 1, 0);

    // Price trades THROUGH our level (much lower)
    PublicTradePayload trade{};
    trade.price      = 49900; // Below our 50000 limit
    trade.qty        = 50;
    trade.taker_side = Side::SELL;

    OrderBookL2 book(1, 1);
    exec.process_public_trade(trade, 1, 1'000'000, book);

    // Should be filled because price went through
    assert(exec.active_orders().at(99).is_filled);

    std::cout << "  [PASS] through_price_fill\n";
}

void test_cancel_order() {
    LatencyConfig lat;
    QueueConfig queue;
    auto reg = make_registry();
    ExecutionModel exec(lat, queue, &reg);

    OrderSubmitPayload order{};
    order.order_id = 7;
    order.side     = Side::BUY;
    order.price    = 50000;
    order.qty      = 100;
    order.type     = OrderType::LIMIT;

    exec.submit_order(order, 1, 0);
    assert(exec.active_orders().count(7));

    assert(exec.cancel_order(7));
    assert(exec.active_orders().at(7).is_cancelled);

    // Can't cancel a non-existent order
    assert(!exec.cancel_order(999));

    std::cout << "  [PASS] cancel_order\n";
}

void test_fill_event_generation() {
    LatencyConfig lat;
    lat.submission_ns = 0;
    lat.jitter_ns     = 0;
    QueueConfig queue;
    queue.depletion_rate = 1.0;
    queue.cancel_decay_per_event = 1.0;

    auto reg = make_registry();
    ExecutionModel exec(lat, queue, &reg);
    EventScheduler sched;

    // Submit and immediately fill via through-price
    OrderSubmitPayload order{};
    order.order_id = 55;
    order.side     = Side::SELL;
    order.price    = 50000;
    order.qty      = 100;
    order.type     = OrderType::LIMIT;

    exec.submit_order(order, 1, 0);

    PublicTradePayload trade{};
    trade.price      = 50100; // Above our sell limit
    trade.qty        = 50;
    trade.taker_side = Side::BUY;

    OrderBookL2 book(1, 1);
    exec.process_public_trade(trade, 1, 1'000'000, book);
    assert(exec.active_orders().at(55).is_filled);

    // tick_fills should generate an EXECUTION_REPORT in the scheduler
    exec.tick_fills(1'000'000, sched);

    MarketEvent out{};
    assert(sched.pop(out));
    assert(out.type == EventType::EXECUTION_REPORT);
    assert(out.payload.exec_report.order_id == 55);
    assert(out.payload.exec_report.is_fully_filled);
    // verify fill carries the correct side
    assert(out.payload.exec_report.side == Side::SELL);
    (void)out;

    std::cout << "  [PASS] fill_event_generation\n";
}

// Verify fee computation
void test_fee_computation() {
    LatencyConfig lat;
    lat.submission_ns = 0;
    lat.jitter_ns     = 0;
    QueueConfig queue;

    auto reg = make_registry();
    // maker_fee_bps = 20 (2bps), taker_fee_bps = 50 (5bps)

    ExecutionModel exec(lat, queue, &reg);
    EventScheduler sched;

    // Limit order -> maker fill
    OrderSubmitPayload order{};
    order.order_id = 100;
    order.side     = Side::BUY;
    order.price    = 5000000; // $50,000 * price_scale(100)
    order.qty      = 1000;    // 1.0 BTC * qty_scale(1000)
    order.type     = OrderType::LIMIT;

    exec.submit_order(order, 1, 0);

    // Through-price fill
    PublicTradePayload trade{};
    trade.price      = 4990000;
    trade.qty        = 500;
    trade.taker_side = Side::SELL;

    OrderBookL2 book(1, 1);
    exec.process_public_trade(trade, 1, 1'000'000, book);
    exec.tick_fills(1'000'000, sched);

    MarketEvent out{};
    assert(sched.pop(out));
    assert(out.payload.exec_report.is_maker == true);

    // Fee calculation:
    // notional = 5,000,000 * 1000 / 1000(qty_scale) = 5,000,000 (= $50,000 * 100)
    // fee = 5,000,000 * 20(maker_bps) / 100,000 = 1000
    // $1000/100 = $10 fee for $50k notional at 2bps = 0.02% -> $50000 * 0.0002 = $10
    assert(out.payload.exec_report.fee == 1000);
    (void)out;

    std::cout << "  [PASS] fee_computation\n";
}

// Verify jitter produces different activation times
void test_latency_jitter() {
    LatencyConfig lat;
    lat.submission_ns = 50'000'000;
    lat.jitter_ns     = 10'000'000;
    lat.rng_seed      = 42;
    QueueConfig queue;
    auto reg = make_registry();

    ExecutionModel exec(lat, queue, &reg);

    // Submit two orders at the same time
    OrderSubmitPayload o1{};
    o1.order_id = 1; o1.side = Side::BUY; o1.price = 50000;
    o1.qty = 100; o1.type = OrderType::LIMIT;

    OrderSubmitPayload o2{};
    o2.order_id = 2; o2.side = Side::BUY; o2.price = 50000;
    o2.qty = 100; o2.type = OrderType::LIMIT;

    exec.submit_order(o1, 1, 0);
    exec.submit_order(o2, 1, 0);

    // With jitter, they should have different activation times
    auto& orders = exec.active_orders();
    int64_t at1 = orders.at(1).activate_ts;
    int64_t at2 = orders.at(2).activate_ts;

    // Both should be near 50ms but not identical (unless RNG happens to give same value)
    assert(at1 >= 40'000'000 && at1 <= 60'000'000); // 50ms +/- 10ms
    assert(at2 >= 40'000'000 && at2 <= 60'000'000);

    std::cout << "  [PASS] latency_jitter (at1=" << at1 << " at2=" << at2 << ")\n";
}

// Verify sell-side fill carries correct side
void test_fill_side_sell() {
    LatencyConfig lat;
    lat.submission_ns = 0;
    lat.jitter_ns     = 0;
    QueueConfig queue;
    auto reg = make_registry();

    ExecutionModel exec(lat, queue, &reg);
    EventScheduler sched;

    // SELL limit order
    OrderSubmitPayload order{};
    order.order_id = 200;
    order.side     = Side::SELL;
    order.price    = 50000;
    order.qty      = 100;
    order.type     = OrderType::LIMIT;

    exec.submit_order(order, 1, 0);

    PublicTradePayload trade{};
    trade.price      = 50100; // Through our sell price
    trade.qty        = 50;
    trade.taker_side = Side::BUY;

    OrderBookL2 book(1, 1);
    exec.process_public_trade(trade, 1, 1'000'000, book);
    exec.tick_fills(1'000'000, sched);

    MarketEvent out{};
    assert(sched.pop(out));
    // The critical assertion: fill must carry SELL, not default to BUY
    assert(out.payload.exec_report.side == Side::SELL);
    (void)out;

    std::cout << "  [PASS] fill_side_sell\n";
}

void test_cancel_latency() {
    LatencyConfig lat;
    lat.submission_ns = 10'000'000; // 10ms
    lat.cancellation_ns = 30'000'000; // 30ms
    lat.jitter_ns = 0;
    QueueConfig queue;
    auto reg = make_registry();

    ExecutionModel exec(lat, queue, &reg);
    EventScheduler sched;

    OrderSubmitPayload order{};
    order.order_id = 500;
    order.side     = Side::BUY;
    order.price    = 100;
    order.qty      = 10;
    order.type     = OrderType::LIMIT;

    exec.submit_order(order, 1, 10'000'000); // submit at 10ms

    // Check at 15ms (not activated yet, submission latency is 10ms)
    std::unordered_map<uint16_t, OrderBookL2> books;
    books.emplace(1, OrderBookL2(1, 1));
    exec.tick_fills(15'000'000, sched, books);
    assert(!exec.active_orders().at(500).is_active);

    // Check at 25ms (should be activated now)
    exec.tick_fills(25'000'000, sched, books);
    assert(exec.active_orders().at(500).is_active);

    // Cancel at 30ms
    exec.cancel_order(500, 30'000'000);
    // At 30ms, it should have pending_cancel = true and is_cancelled = false
    assert(exec.active_orders().at(500).pending_cancel);
    assert(!exec.active_orders().at(500).is_cancelled);

    // Check at 50ms (not yet cancelled because cancellation takes 30ms, so 30ms + 30ms = 60ms)
    exec.tick_fills(50'000'000, sched, books);
    assert(!exec.active_orders().at(500).is_cancelled);

    // Check at 65ms (should be cancelled now)
    exec.tick_fills(65'000'000, sched, books);
    // After cancellation, the order is removed from active_orders_ in tick_fills
    assert(exec.active_orders().find(500) == exec.active_orders().end());

    std::cout << "  [PASS] cancel_latency\n";
}

void test_tif_ioc_fok() {
    LatencyConfig lat;
    lat.submission_ns = 0;
    lat.cancellation_ns = 0;
    lat.jitter_ns = 0;
    QueueConfig queue;
    auto reg = make_registry();

    // 1. FOK order that fails to fill fully
    {
        ExecutionModel exec(lat, queue, &reg);
        EventScheduler sched;

        OrderBookL2 book(1, 1);
        book.apply_diff({101, 5, Side::SELL}); // Ask: 5 qty at 101

        std::unordered_map<uint16_t, OrderBookL2> books;
        books.emplace(1, book);

        OrderSubmitPayload order{};
        order.order_id = 600;
        order.side     = Side::BUY;
        order.price    = 101;
        order.qty      = 10;
        order.type     = OrderType::LIMIT;
        order.tif      = TimeInForce::FOK;

        exec.submit_order(order, 1, 0);
        exec.tick_fills(1'000'000, sched, books);

        // FOK should fail (qty = 10 > 5 available) and be cancelled immediately
        assert(exec.active_orders().find(600) == exec.active_orders().end());
        assert(exec.stats().total_fills == 0);
    }

    // 2. FOK order that fills fully
    {
        ExecutionModel exec(lat, queue, &reg);
        EventScheduler sched;

        OrderBookL2 book(1, 1);
        book.apply_diff({101, 15, Side::SELL}); // Ask: 15 qty at 101

        std::unordered_map<uint16_t, OrderBookL2> books;
        books.emplace(1, book);

        OrderSubmitPayload order{};
        order.order_id = 601;
        order.side     = Side::BUY;
        order.price    = 101;
        order.qty      = 10;
        order.type     = OrderType::LIMIT;
        order.tif      = TimeInForce::FOK;

        exec.submit_order(order, 1, 0);
        exec.tick_fills(1'000'000, sched, books);

        // Should be fully filled and removed
        assert(exec.active_orders().find(601) == exec.active_orders().end());
        assert(exec.stats().total_fills == 1);
    }

    // 3. IOC order that fills partially
    {
        ExecutionModel exec(lat, queue, &reg);
        EventScheduler sched;

        OrderBookL2 book(1, 1);
        book.apply_diff({101, 5, Side::SELL}); // Ask: 5 qty at 101

        std::unordered_map<uint16_t, OrderBookL2> books;
        books.emplace(1, book);

        OrderSubmitPayload order{};
        order.order_id = 602;
        order.side     = Side::BUY;
        order.price    = 101;
        order.qty      = 10;
        order.type     = OrderType::LIMIT;
        order.tif      = TimeInForce::IOC;

        exec.submit_order(order, 1, 0);
        exec.tick_fills(1'000'000, sched, books);

        // IOC fills 5 qty and remaining 5 is cancelled
        assert(exec.active_orders().find(602) == exec.active_orders().end());
        assert(exec.stats().total_fills == 1);

        MarketEvent fill_event{};
        assert(sched.pop(fill_event));
        assert(fill_event.payload.exec_report.fill_qty == 5);
        assert(fill_event.payload.exec_report.leaves_qty == 5);
        (void)fill_event;
    }

    std::cout << "  [PASS] tif_ioc_fok\n";
}

void test_limit_maker() {
    LatencyConfig lat;
    lat.submission_ns = 0;
    lat.cancellation_ns = 0;
    lat.jitter_ns = 0;
    QueueConfig queue;
    auto reg = make_registry();

    ExecutionModel exec(lat, queue, &reg);
    EventScheduler sched;

    OrderBookL2 book(1, 1);
    book.apply_diff({101, 5, Side::SELL});

    std::unordered_map<uint16_t, OrderBookL2> books;
    books.emplace(1, book);

    // LIMIT_MAKER that crosses ask (101) should be cancelled/rejected
    OrderSubmitPayload order{};
    order.order_id = 700;
    order.side     = Side::BUY;
    order.price    = 101;
    order.qty      = 10;
    order.type     = OrderType::LIMIT_MAKER;

    exec.submit_order(order, 1, 0);
    exec.tick_fills(1'000'000, sched, books);

    assert(exec.active_orders().find(700) == exec.active_orders().end());
    assert(exec.stats().total_fills == 0);

    // LIMIT_MAKER that does not cross (100) should be accepted and active
    order.order_id = 701;
    order.price    = 100;
    exec.submit_order(order, 1, 1'000'000);
    exec.tick_fills(2'000'000, sched, books);

    assert(exec.active_orders().find(701) != exec.active_orders().end());
    assert(exec.active_orders().at(701).is_active);

    std::cout << "  [PASS] limit_maker\n";
}

int main() {
    std::cout << "Running Execution Model tests...\n";
    test_order_latency();
    test_queue_depletion_fill();
    test_through_price_fill();
    test_cancel_order();
    test_fill_event_generation();
    test_fee_computation();
    test_latency_jitter();
    test_fill_side_sell();
    test_cancel_latency();
    test_tif_ioc_fok();
    test_limit_maker();
    std::cout << "All Execution Model tests passed.\n";
    return 0;
}
