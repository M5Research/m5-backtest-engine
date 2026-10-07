"""Strategy1: An example trading strategy replayed using the C++ engine.

Execute directly via:
    python -m backtest.strategy1 3m
"""

from __future__ import annotations

import argparse
import logging
import os
import struct
import sys
import time
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import pandas as pd

# Add the src folder and scripts folder to the python path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

try:
    import _bootstrap  # noqa: F401
except ModuleNotFoundError:
    pass

import m5_engine as qe

from backtesting.data_downloader import download_binance_klines

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s: %(message)s")
logger = logging.getLogger(__name__)

class Strategy1(qe.Strategy):
    """
    Submit responder orders when leader price and volume changes cross a threshold.
    """

    def __init__(
        self,
        engine: qe.ReplayEngine,
        leader_id: int,
        responder_id: int,
        return_window: int = 5,
        volume_window: int = 20,
        shock_threshold: float = 1.0,
        qty_per_trade: float = 1000.0,
    ):
        super().__init__()
        self.engine = engine
        self.leader_id = leader_id
        self.responder_id = responder_id
        self.return_window = return_window
        self.volume_window = volume_window
        self.shock_threshold = shock_threshold
        self.qty_per_trade = qty_per_trade

        # Queues to hold leader price & volume history
        from collections import deque
        self.leader_prices: deque[float] = deque(maxlen=return_window)
        self.leader_volumes: deque[float] = deque(maxlen=volume_window)
        self.current_order_id = 1

    def on_init(self, registry: Any):
        self.registry = registry

    def on_market_event(self, event: Any, book: Any):
        """Called for every trade and depth update chronologically."""
        if event.type == qe.EventType.PUBLIC_TRADE:
            trade = event.trade
            if event.symbol_id == self.leader_id:
                self._update_leader(trade)

    def _update_leader(self, trade: Any):
        if trade.price <= 0:
            return

        # Check if queues are warmed up
        if (
            len(self.leader_prices) == self.return_window
            and len(self.leader_volumes) == self.volume_window
        ):
            ref_price = self.leader_prices[0]
            avg_vol = sum(self.leader_volumes) / self.volume_window

            if avg_vol > 0:
                import math
                volume_shock = (trade.qty / avg_vol) - 1.0
                trailing_return = math.log(trade.price / ref_price) if ref_price > 0 else 0

                # Scaled shock score
                shock_score = (trailing_return / 0.01) * (1.0 + max(0.0, volume_shock))

                if abs(shock_score) >= self.shock_threshold:
                    self._fire_responder_order(shock_score)

        self.leader_prices.append(trade.price)
        self.leader_volumes.append(trade.qty)

    def _fire_responder_order(self, shock_score: float):
        """Submit a market IOC order for the responder asset."""
        order = qe.OrderSubmitPayload()
        order.order_id = self.current_order_id
        self.current_order_id += 1

        order.qty = int(self.qty_per_trade)
        order.side = qe.Side.BUY if shock_score > 0 else qe.Side.SELL
        order.type = qe.OrderType.MARKET
        order.tif = qe.TimeInForce.IOC
        order.price = 0  # Market orders use 0 limit price fallback in our engine

        self.engine.submit_order(self.responder_id, order)

    def on_fill(self, fill: Any, symbol_id: int):
        """Called automatically when an order is filled."""
        logger.info(
            "[FILL] Symbol: %d, Side: %s, Qty: %.2f, Price: %.2f",
            symbol_id,
            fill.side.name,
            fill.fill_qty / 1000.0,  # unscale qty
            fill.fill_price / 100.0,  # unscale price
        )

def parse_duration(duration_str: str) -> timedelta:
    duration_str = duration_str.strip().lower()
    unit = duration_str[-1]
    value = int(duration_str[:-1])
    if unit == "m":
        return timedelta(days=value * 30)
    elif unit == "y":
        return timedelta(days=value * 365)
    elif unit == "d":
        return timedelta(days=value)
    else:
        raise ValueError(f"Unsupported duration: {duration_str}")

def pack_event(exchange_ts: int, sequence_id: int, symbol_id: int, event_type: int, payload: bytes) -> bytes:
    header = struct.pack("<qqQHBB", exchange_ts, exchange_ts, sequence_id, symbol_id, event_type, 0)
    return header + payload + b"\x00" * (100 - len(payload))

def convert_klines_to_binary(df: pd.DataFrame, output_path: str, symbol_id: int) -> int:
    events = []
    seq = 0
    for row in df.itertuples(index=False):
        ts_ns = int(pd.to_datetime(row.timestamp).timestamp() * 1_000_000_000)
        price = int(float(row.close) * 100)
        qty = int(float(row.volume) * 1000)
        payload = struct.pack("<qqB", price, qty, 0)
        event = pack_event(ts_ns, seq, symbol_id, 2, payload)
        events.append((ts_ns, event))
        seq += 1
    events.sort(key=lambda x: x[0])
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(struct.pack("<8sIIHH", b"CREARCHV", 1, len(events), 1, 0))
        for _, event_bytes in events:
            f.write(event_bytes)
    return len(events)

def run_strategy1_backtest(duration: str, interval: str = "1h") -> int:
    delta = parse_duration(duration)
    end_time = datetime.now(tz=UTC)
    start_time = end_time - delta
    five_years_ago = end_time - timedelta(days=5 * 365)

    # 1. Acquire cache
    btc_master = download_binance_klines("BTCUSDT", interval, five_years_ago, end_time, "data/study")
    eth_master = download_binance_klines("ETHUSDT", interval, five_years_ago, end_time, "data/study")

    # 2. Filter data
    btc_df = pd.read_csv(btc_master)
    eth_df = pd.read_csv(eth_master)
    btc_df["dt"] = pd.to_datetime(btc_df["timestamp"], utc=True)
    eth_df["dt"] = pd.to_datetime(eth_df["timestamp"], utc=True)
    btc_filtered = btc_df[(btc_df["dt"] >= start_time) & (btc_df["dt"] <= end_time)]
    eth_filtered = eth_df[(eth_df["dt"] >= start_time) & (eth_df["dt"] <= end_time)]

    # 3. Convert to binary crearchv
    btc_bin = "data/study/BTCUSDT_strategy1.bin"
    eth_bin = "data/study/ETHUSDT_strategy1.bin"
    convert_klines_to_binary(btc_filtered, btc_bin, symbol_id=1)
    convert_klines_to_binary(eth_filtered, eth_bin, symbol_id=2)

    # 4. Engine setup
    registry = qe.SymbolRegistry()

    # BTC
    btc_meta = qe.SymbolMeta()
    btc_meta.symbol_id = 1
    btc_meta.symbol_name = "BTCUSDT"
    btc_meta.price_scale = 100
    btc_meta.qty_scale = 1000
    btc_meta.tick_size = 1
    btc_meta.lot_size = 1
    btc_meta.maker_fee_bps = 2
    btc_meta.taker_fee_bps = 5
    registry.register_symbol(btc_meta)

    # ETH
    eth_meta = qe.SymbolMeta()
    eth_meta.symbol_id = 2
    eth_meta.symbol_name = "ETHUSDT"
    eth_meta.price_scale = 100
    eth_meta.qty_scale = 1000
    eth_meta.tick_size = 1
    eth_meta.lot_size = 1
    eth_meta.maker_fee_bps = 2
    eth_meta.taker_fee_bps = 5
    registry.register_symbol(eth_meta)

    lat = qe.LatencyConfig()
    lat.submission_ns = 35_000_000
    lat.cancellation_ns = 30_000_000

    engine = qe.ReplayEngine(registry, lat, qe.QueueConfig(), qe.RiskLimits(), 100_000_00)

    # Attach the strategy.
    # Use five price observations, twenty volume observations, and a 0.1 shock threshold.
    strategy = Strategy1(engine, leader_id=1, responder_id=2, return_window=5, volume_window=20, shock_threshold=0.1)
    engine.set_strategy(strategy)

    # 6. Stream readers
    btc_reader = qe.BinaryArchiveReader(btc_bin)
    eth_reader = qe.BinaryArchiveReader(eth_bin)
    engine.add_stream(btc_reader)
    engine.add_stream(eth_reader)

    logger.info("Executing C++ Backtest for Strategy1...")
    start_perf = time.perf_counter()
    engine.run()
    end_perf = time.perf_counter()

    stats = engine.stats()
    risk = engine.risk()

    print("\n" + "=" * 45)
    print("      C++ STRATEGY1 BACKTEST SIMULATION      ")
    print("=" * 45)
    print(f"Target lookback:   {duration}")
    print(f"Time Range:        {start_time.strftime('%Y-%m-%d')} to {end_time.strftime('%Y-%m-%d')}")
    print(f"Engine Replay time: {end_perf - start_perf:.4f}s")
    print(f"Events Replayed:   {stats.events_processed:,}")
    print(f"Order Fills:       {stats.fill_events}")
    print("Initial Capital:   $100,000.00")
    print(f"Ending Balance:    ${risk.account().balance / 100:.2f}")
    print(f"Realized P&L:      ${risk.total_realized_pnl() / 100:.2f}")
    print(f"Unrealized P&L:    ${risk.total_unrealized_pnl() / 100:.2f}")
    print("=" * 45 + "\n")
    return 0

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("duration", nargs="?", default="3m")
    args = parser.parse_args()
    sys.exit(run_strategy1_backtest(args.duration))
