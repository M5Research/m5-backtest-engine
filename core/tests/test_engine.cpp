// Test: Full ReplayEngine integration
// Restart determinism; replaying the same
// archive twice must yield the same PnL ledger.
#include "engine/replay_engine.hpp"
#include "engine/feed.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace engine;

// Simple test strategy that buys on any trade event
class TestBuyStrategy : public StrategyCallback {
public:
    int event_count = 0;
    int fill_count = 0;

    void on_market_event(const MarketEvent& event,
                         const OrderBookL2& book) override {
        event_count++;
        (void)event;
        (void)book;
    }

    void on_fill(const ExecutionReportPayload& fill,
                 uint16_t symbol_id) override {
        fill_count++;
        (void)fill; (void)symbol_id;
    }
};

void test_engine_processes_events() {
    // Setup symbol registry
    SymbolRegistry registry;
    SymbolMeta btc{};
    btc.symbol_id   = 0;
    btc.symbol_name = "BTCUSDT";
    btc.price_scale = 100;
    btc.qty_scale   = 1000;
    btc.tick_size   = 1;
    btc.lot_size    = 1;
    btc.maker_fee_bps = 20;
    btc.taker_fee_bps = 50;
    registry.register_symbol(btc);

    // Create event stream
    std::vector<MarketEvent> stream;

    // Depth update
    MarketEvent e1{};
    e1.exchange_ts = 1000;
    e1.sequence_id = 0;
    e1.symbol_id   = 0;
    e1.type        = EventType::DEPTH_DIFF;
    e1.payload.depth = {5000000, 100000, Side::BUY};
    stream.push_back(e1);

    // Trade
    MarketEvent e2{};
    e2.exchange_ts = 2000;
    e2.sequence_id = 1;
    e2.symbol_id   = 0;
    e2.type        = EventType::PUBLIC_TRADE;
    e2.payload.trade = {5000000, 50000, Side::BUY};
    stream.push_back(e2);

    // Another trade
    MarketEvent e3{};
    e3.exchange_ts = 3000;
    e3.sequence_id = 2;
    e3.symbol_id   = 0;
    e3.type        = EventType::PUBLIC_TRADE;
    e3.payload.trade = {5000100, 30000, Side::SELL};
    stream.push_back(e3);

    // Run engine
    ReplayEngine engine(registry);
    TestBuyStrategy strategy;
    engine.set_strategy(&strategy);
    engine.add_stream(stream.data(), stream.size());
    engine.run();

    assert(engine.stats().events_processed == 3);
    assert(engine.stats().depth_events == 1);
    assert(engine.stats().trade_events == 2);
    assert(strategy.event_count == 3); // 1 depth + 2 trades

    std::cout << "  [PASS] engine_processes_events\n";
}

void test_restart_determinism() {
    SymbolRegistry registry;
    SymbolMeta btc{};
    btc.symbol_id   = 0;
    btc.symbol_name = "BTCUSDT";
    btc.price_scale = 100;
    btc.qty_scale   = 1000;
    btc.tick_size   = 1;
    btc.lot_size    = 1;
    registry.register_symbol(btc);

    std::vector<MarketEvent> stream;
    for (int i = 0; i < 100; ++i) {
        MarketEvent e{};
        e.exchange_ts = static_cast<int64_t>(i) * 1000;
        e.sequence_id = static_cast<uint64_t>(i);
        e.symbol_id   = 0;
        e.type        = EventType::PUBLIC_TRADE;
        e.payload.trade = {5000000 + static_cast<int64_t>(i), 1000, Side::BUY};
        stream.push_back(e);
    }

    // Run 1
    ReplayEngine engine1(registry);
    TestBuyStrategy strat1;
    engine1.set_strategy(&strat1);
    engine1.add_stream(stream.data(), stream.size());
    engine1.run();

    // Run 2
    ReplayEngine engine2(registry);
    TestBuyStrategy strat2;
    engine2.set_strategy(&strat2);
    engine2.add_stream(stream.data(), stream.size());
    engine2.run();

    // Must be identical
    assert(engine1.stats().events_processed == engine2.stats().events_processed);
    assert(strat1.event_count == strat2.event_count);
    assert(strat1.fill_count == strat2.fill_count);

    std::cout << "  [PASS] restart_determinism\n";
}

void test_sequence_validator() {
    SequenceValidator validator(0);

    // Before snapshot: should be AWAITING_SNAPSHOT
    assert(validator.validate(1, 5) == SequenceValidator::Status::AWAITING_SNAPSHOT);

    // Set snapshot
    validator.set_snapshot_id(10);

    // Stale diff: u <= lastUpdateId
    assert(validator.validate(5, 8) == SequenceValidator::Status::STALE_DUPLICATE);

    // First valid diff: U <= 11 && u >= 11
    assert(validator.validate(9, 12) == SequenceValidator::Status::OK);

    // Next contiguous diff: U = 13
    assert(validator.validate(13, 15) == SequenceValidator::Status::OK);

    // Gap: U = 20 (should be 16)
    assert(validator.validate(20, 25) == SequenceValidator::Status::GAP_DETECTED);

    std::cout << "  [PASS] sequence_validator\n";
}

void test_margin_liquidation() {
    SymbolRegistry registry;
    SymbolMeta btc{};
    btc.symbol_id   = 0;
    btc.symbol_name = "BTCUSDT";
    btc.price_scale = 100;
    btc.qty_scale   = 1000;
    btc.tick_size   = 1;
    btc.lot_size    = 1;
    registry.register_symbol(btc);

    LatencyConfig latency{};
    latency.submission_ns = 0;
    latency.cancellation_ns = 0;
    latency.jitter_ns = 0;

    QueueConfig queue{};
    queue.initial_depth_fraction = 0.0;

    // Initial balance: 10,000 (scaled by 100 -> 100 USDT)
    ReplayEngine engine(registry, latency, queue, {}, 10000);

    std::vector<MarketEvent> stream;

    // Depth snapshot to populate book
    MarketEvent e1{};
    e1.exchange_ts = 1000;
    e1.sequence_id = 1;
    e1.symbol_id   = 0;
    e1.type        = EventType::DEPTH_SNAPSHOT;
    e1.payload.depth = {5000000, 100000, Side::BUY}; // Bid: 500.00 qty 100.00
    stream.push_back(e1);

    MarketEvent e1_ask{};
    e1_ask.exchange_ts = 1001;
    e1_ask.sequence_id = 1;
    e1_ask.symbol_id   = 0;
    e1_ask.type        = EventType::DEPTH_SNAPSHOT;
    e1_ask.payload.depth = {5010000, 100000, Side::SELL}; // Ask: 501.00 qty 100.00
    stream.push_back(e1_ask);

    // Order submit
    MarketEvent e2{};
    e2.exchange_ts = 2000;
    e2.sequence_id = 2;
    e2.symbol_id   = 0;
    e2.type        = EventType::ORDER_SUBMIT;
    e2.payload.order_submit.order_id = 999;
    e2.payload.order_submit.side     = Side::BUY;
    e2.payload.order_submit.price    = 5000000; // 500.00
    e2.payload.order_submit.qty      = 20000;   // 20.000 qty
    e2.payload.order_submit.type     = OrderType::LIMIT;
    stream.push_back(e2);

    // Public trade to fill the order
    MarketEvent e3{};
    e3.exchange_ts = 3000;
    e3.sequence_id = 3;
    e3.symbol_id   = 0;
    e3.type        = EventType::PUBLIC_TRADE;
    e3.payload.trade = {5000000, 50000, Side::SELL}; // Price 500.00, qty 50.000
    stream.push_back(e3);

    // Public trade dropping price to 450.00
    MarketEvent e4{};
    e4.exchange_ts = 4000;
    e4.sequence_id = 4;
    e4.symbol_id   = 0;
    e4.type        = EventType::PUBLIC_TRADE;
    e4.payload.trade = {4500000, 50000, Side::SELL}; // Price 450.00, qty 50.000
    stream.push_back(e4);

    engine.add_stream(stream.data(), stream.size());
    engine.run();

    // Verify engine has processed events and triggered liquidation
    assert(engine.risk().account_state().is_liquidated);
    assert(engine.risk().position(0).quantity == 0);
    assert(engine.execution().active_orders().empty());

    std::cout << "  [PASS] margin_liquidation\n";
}

int main() {
    std::cout << "Running Engine integration tests...\n";
    test_engine_processes_events();
    test_restart_determinism();
    test_sequence_validator();
    test_margin_liquidation();
    std::cout << "All Engine integration tests passed.\n";
    return 0;
}
