import struct

def generate_synthetic_archive(path, num_events=1_000_000):
    """
    Generates a binary archive with synthetic market data.
    """
    print(f"Generating {num_events} synthetic events...")

    # Header: magic[8], version[4], num_events[4], num_symbols[2], reserved[2]
    header = struct.pack("<8sIIHH", b"CREARCHV", 1, num_events, 1, 0)

    with open(path, "wb") as f:
        f.write(header)

        base_price = 50_000_00
        start_ts = 1625097600000000000  # Nanoseconds

        for i in range(num_events):
            ts = start_ts + i * 1000  # 1us between events

            event_type = 0  # DEPTH_DIFF
            if i % 100 == 0:
                event_type = 2  # PUBLIC_TRADE

            symbol_id = 1

            # MarketEvent structure:
            # int64_t exchange_ts (8)
            # int64_t local_ts (8)
            # uint64_t sequence_id (8)
            # uint16_t symbol_id (2)
            # uint8_t type (1)
            # uint8_t flags (1)
            # The 28-byte header leaves 100 bytes for payload and padding.

            header_part = struct.pack("<qqQHBB", ts, ts, i, symbol_id, event_type, 0)

            if event_type == 0:
                # DepthUpdatePayload: price(8), qty(8), side(1)
                payload = struct.pack(
                    "<qqB", base_price + (i % 100), 1000, 0 if (i % 2 == 0) else 1
                )
            else:
                # PublicTradePayload: price(8), qty(8), taker_side(1)
                payload = struct.pack("<qqB", base_price + (i % 100), 500, 0)

            # Pad the payload to 100 bytes to reach 128 total
            full_event = header_part + payload + b"\x00" * (128 - len(header_part) - len(payload))
            f.write(full_event)

if __name__ == "__main__":
    archive_path = "backtest/stress_tests/test_data.bin"
    generate_synthetic_archive(archive_path, 1_000_000)
    print(f"Archive generated at {archive_path}")
