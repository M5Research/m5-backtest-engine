"""Convert trade and depth CSV rows into CREARCHV events."""

import argparse
import csv
import os
import struct

# Constants matching C++ types
EVENT_TYPE_DEPTH_DIFF = 0
EVENT_TYPE_PUBLIC_TRADE = 2
SIDE_BUY = 0
SIDE_SELL = 1

def pack_event(
    exchange_ts: int, sequence_id: int, symbol_id: int, event_type: int, payload: bytes
) -> bytes:
    """
    Packs a MarketEvent to exactly 128 bytes.
    MarketEvent structure (28 byte header):
      int64_t exchange_ts
      int64_t local_ts
      uint64_t sequence_id
      uint16_t symbol_id
      uint8_t type
      uint8_t flags
    """
    header = struct.pack("<qqQHBB", exchange_ts, exchange_ts, sequence_id, symbol_id, event_type, 0)

    if len(payload) > 100:
        raise ValueError("Payload exceeds 100 bytes padding limit")

    full_event = header + payload + b"\x00" * (100 - len(payload))
    assert len(full_event) == 128
    return full_event

def parse_binance_csvs(
    trades_path: str,
    depth_path: str,
    output_path: str,
    symbol_id: int,
    price_scale: int,
    qty_scale: int,
):
    events: list[tuple[int, bytes]] = []  # list of (timestamp, packed_event)

    print(f"Reading depth from {depth_path}")
    if os.path.exists(depth_path):
        with open(depth_path) as f:
            reader = csv.DictReader(f)
            seq = 0
            for row in reader:
                ts = int(row["timestamp"]) * 1_000_000  # Convert ms to ns
                price = int(float(row["price"]) * price_scale)
                qty = int(float(row["qty"]) * qty_scale)
                side = SIDE_BUY if row["side"].lower() in ["b", "buy", "bid"] else SIDE_SELL

                # DepthUpdatePayload: price(8), qty(8), side(1)
                payload = struct.pack("<qqB", price, qty, side)
                events.append((ts, pack_event(ts, seq, symbol_id, EVENT_TYPE_DEPTH_DIFF, payload)))
                seq += 1

    print(f"Reading trades from {trades_path}")
    if os.path.exists(trades_path):
        with open(trades_path) as f:
            reader = csv.DictReader(f)
            seq = 1_000_000_000  # Offset trade sequence IDs from depth sequence IDs.
            for row in reader:
                ts = int(row["timestamp"]) * 1_000_000
                price = int(float(row["price"]) * price_scale)
                qty = int(float(row["qty"]) * qty_scale)
                taker_side = SIDE_BUY if row["is_buyer_maker"].lower() == "false" else SIDE_SELL

                # PublicTradePayload: price(8), qty(8), taker_side(1)
                payload = struct.pack("<qqB", price, qty, taker_side)
                events.append(
                    (
                        ts,
                        pack_event(ts, seq, symbol_id, EVENT_TYPE_PUBLIC_TRADE, payload),
                    )
                )
                seq += 1

    print("Sorting events by timestamp...")
    events.sort(key=lambda x: x[0])

    print(f"Writing {len(events)} events to {output_path}")
    with open(output_path, "wb") as f:
        # CREARCHV Header
        f.write(struct.pack("<8sIIHH", b"CREARCHV", 1, len(events), 1, 0))
        for _, event_bytes in events:
            f.write(event_bytes)

    print("Conversion complete.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert Binance CSVs to CREARCHV binary")
    parser.add_argument("--trades", type=str, default="trades.csv")
    parser.add_argument("--depth", type=str, default="depth.csv")
    parser.add_argument("--output", type=str, default="output.bin")
    parser.add_argument("--symbol-id", type=int, default=1)
    parser.add_argument("--price-scale", type=int, default=100)  # e.g. 100 for 2 decimal places
    parser.add_argument("--qty-scale", type=int, default=1000)  # e.g. 1000 for 3 decimal places
    args = parser.parse_args()

    parse_binance_csvs(
        args.trades,
        args.depth,
        args.output,
        args.symbol_id,
        args.price_scale,
        args.qty_scale,
    )
