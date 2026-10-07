"""Compare summary results from two replays of the same archive."""

import m5_engine as qe

def run_once(archive_path):
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
    engine.add_stream(reader)
    engine.run()

    return {
        "equity": engine.risk().equity(),
        "events": engine.stats().events_processed,
        "trades": engine.stats().trade_events,
        "pnl": engine.risk().total_realized_pnl(),
    }

def test_determinism():
    print("Testing Determinism...")
    archive_path = "backtest/stress_tests/test_data.bin"

    res1 = run_once(archive_path)
    res2 = run_once(archive_path)

    print(f"Run 1: {res1}")
    print(f"Run 2: {res2}")

    if res1 == res2:
        print("SUCCESS: Both runs produced matching summary results.")
    else:
        print("FAILURE: Replay summary results differ.")
        for k in res1:
            if res1[k] != res2[k]:
                print(f"  Mismatch in {k}: {res1[k]} vs {res2[k]}")

if __name__ == "__main__":
    test_determinism()
