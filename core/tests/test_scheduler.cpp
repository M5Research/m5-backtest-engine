// Test: EventScheduler
// K-way merge with internal event heap.
#include "engine/scheduler.hpp"
#include "engine/events.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace engine;

// Helper to create a simple trade event
static MarketEvent make_event(int64_t ts, uint64_t seq, uint16_t sym = 0) {
    MarketEvent e{};
    e.exchange_ts = ts;
    e.local_ts    = ts;
    e.sequence_id = seq;
    e.symbol_id   = sym;
    e.type        = EventType::PUBLIC_TRADE;
    e.flags       = 0;
    e.payload.trade.price = 50000;
    e.payload.trade.qty   = 100;
    e.payload.trade.taker_side = Side::BUY;
    return e;
}

void test_single_stream() {
    std::vector<MarketEvent> stream = {
        make_event(100, 0),
        make_event(200, 1),
        make_event(300, 2),
    };

    EventScheduler sched;
    sched.add_stream(stream.data(), stream.size());

    MarketEvent out{};
    assert(sched.pop(out));
    assert(out.exchange_ts == 100);

    assert(sched.pop(out));
    assert(out.exchange_ts == 200);

    assert(sched.pop(out));
    assert(out.exchange_ts == 300);

    assert(!sched.pop(out)); // Exhausted
    (void)out;

    std::cout << "  [PASS] single_stream\n";
}

void test_kway_merge() {
    // Two interleaved streams; should merge chronologically
    std::vector<MarketEvent> btc_stream = {
        make_event(100, 0, 0),
        make_event(300, 2, 0),
        make_event(500, 4, 0),
    };

    std::vector<MarketEvent> eth_stream = {
        make_event(150, 1, 1),
        make_event(250, 3, 1),
        make_event(450, 5, 1),
    };

    EventScheduler sched;
    sched.add_stream(btc_stream.data(), btc_stream.size());
    sched.add_stream(eth_stream.data(), eth_stream.size());

    int64_t expected_ts[] = {100, 150, 250, 300, 450, 500};
    MarketEvent out{};
    for (int i = 0; i < 6; ++i) {
        assert(sched.pop(out));
        assert(out.exchange_ts == expected_ts[i]);
    }
    assert(!sched.pop(out));
    (void)out;
    (void)expected_ts;

    std::cout << "  [PASS] kway_merge\n";
}

void test_internal_events_interleave() {
    // Historical stream + internally generated events
    std::vector<MarketEvent> stream = {
        make_event(100, 0),
        make_event(300, 2),
        make_event(500, 4),
    };

    EventScheduler sched;
    sched.add_stream(stream.data(), stream.size());

    // Push an internal event at ts=200 (between stream events)
    sched.push_internal(make_event(200, 10));
    // And one at ts=400
    sched.push_internal(make_event(400, 11));

    int64_t expected[] = {100, 200, 300, 400, 500};
    MarketEvent out{};
    for (int i = 0; i < 5; ++i) {
        assert(sched.pop(out));
        assert(out.exchange_ts == expected[i]);
    }
    assert(!sched.pop(out));
    (void)out;
    (void)expected;

    std::cout << "  [PASS] internal_events_interleave\n";
}

void test_empty_scheduler() {
    EventScheduler sched;
    assert(sched.empty());

    MarketEvent out{};
    assert(!sched.pop(out));
    (void)out;

    std::cout << "  [PASS] empty_scheduler\n";
}

int main() {
    std::cout << "Running Scheduler tests...\n";
    test_single_stream();
    test_kway_merge();
    test_internal_events_interleave();
    test_empty_scheduler();
    std::cout << "All Scheduler tests passed.\n";
    return 0;
}
