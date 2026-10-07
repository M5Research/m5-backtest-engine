import struct

import m5_engine as qe

def test_liquidation():
    print("Testing Liquidation Scenario...")
    registry = qe.SymbolRegistry()
    meta = qe.SymbolMeta()
    meta.symbol_id = 1
    meta.symbol_name = "BTCUSDT"
    meta.price_scale = 100
    meta.qty_scale = 1000
    meta.tick_size = 1
    meta.lot_size = 1
    registry.register_symbol(meta)

    # Initial balance $1000
    engine = qe.ReplayEngine(
        registry, qe.LatencyConfig(), qe.QueueConfig(), qe.RiskLimits(), 1000_00
    )

    # A 0.2 BTC position at $50,000 has $10,000 notional value.

    order = qe.OrderSubmitPayload()
    order.order_id = 1
    order.price = 50000_00
    order.qty = 200  # 0.2 BTC
    order.side = qe.Side.BUY
    order.type = qe.OrderType.MARKET

    # Encode the order, fill, and price drop as a synthetic stream.
    def make_event(ts, type, payload):
        header = struct.pack("<qqQHBB", ts, ts, 0, 1, type, 0)
        return header + payload + b"\x00" * (128 - len(header) - len(payload))

    events = []
    # Event 0: Order Submit
    submit_payload = struct.pack("<QqqBBB", 1, 50000_00, 200, 0, 1, 0)
    events.append(make_event(1000, 3, submit_payload))

    # Event 1: Execution Report (Fill at 50,000)
    # fill_id, price, qty, leaves, fee, is_maker, is_full
    fill_payload = struct.pack("<QqqqB?B", 1, 50000_00, 200, 0, 1_00, False, True)
    events.append(make_event(2000, 5, fill_payload))

    # Event 2: Flash Crash (Price goes to $40,000)
    # 0.2 BTC * ($40,000 - $50,000) = -$2,000 PnL.
    # The $2,000 loss exceeds the $1,000 starting balance.
    crash_payload = struct.pack("<qqB", 40000_00, 500, 0)
    events.append(make_event(3000, 2, crash_payload))

    path = "backtest/stress_tests/crash.bin"
    with open(path, "wb") as f:
        f.write(struct.pack("<8sIIHH", b"CREARCHV", 1, len(events), 1, 0))
        for e in events:
            f.write(e)

    reader = qe.BinaryArchiveReader(path)
    engine.add_stream(reader)
    engine.run()

    risk = engine.risk()
    print(f"Final Equity after crash: {risk.equity()}")
    print(f"Maintenance Margin:       {risk.account().maintenance_margin}")

    if risk.equity() < risk.account().maintenance_margin:
        print("SUCCESS: Engine detected margin breach/negative equity.")
    else:
        print("FAILURE: Engine failed to detect liquidation state.")

if __name__ == "__main__":
    test_liquidation()
