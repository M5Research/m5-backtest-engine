"""Measure replay throughput with strategy callbacks."""

import os
import time

import m5_engine as qe

class PerformanceStrategy(qe.Strategy):
    def __init__(self):
        super().__init__()
        self.events = 0
        self.fills = 0

    def on_init(self, registry):
        pass

    def on_market_event(self, event, book):
        self.events += 1

    def on_market_event_batch(self, events):
        self.events += len(events)

    def on_fill(self, fill, symbol_id):
        self.fills += 1

def run_performance_test(archive_path):
    if not os.path.exists(archive_path):
        print(f"Error: Archive {archive_path} not found.")
        return

    print(f"Loading archive {archive_path}...")
    reader = qe.BinaryArchiveReader(archive_path)

    registry = qe.SymbolRegistry()
    meta = qe.SymbolMeta()
    meta.symbol_id = 1
    meta.symbol_name = "BTCUSDT"
    meta.price_scale = 100
    meta.qty_scale = 1000
    meta.tick_size = 1
    meta.lot_size = 1
    registry.register_symbol(meta)

    engine = qe.ReplayEngine(
        registry, qe.LatencyConfig(), qe.QueueConfig(), qe.RiskLimits(), 1_000_000_00
    )
    engine.set_batch_mode(True)
    strategy = PerformanceStrategy()
    engine.set_strategy(strategy)
    engine.add_stream(reader)

    print("Starting replay...")
    start_time = time.perf_counter()
    engine.run()
    end_time = time.perf_counter()

    duration = end_time - start_time
    stats = engine.stats()

    print("-" * 40)
    print("Stress Test Results:")
    print(f"Events Processed: {stats.events_processed}")
    print(f"Time Taken:       {duration:.4f} seconds")
    print(f"Throughput:       {stats.events_processed / duration:.0f} events/sec")
    print(f"Strategy Events:  {strategy.events}")
    print(f"Final Equity:     {engine.risk().equity()}")
    print("-" * 40)

if __name__ == "__main__":
    archive_path = "backtest/stress_tests/test_data.bin"
    run_performance_test(archive_path)
