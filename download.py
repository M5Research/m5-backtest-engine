"""Download Binance USD-M futures one-minute klines into partitioned Parquet files."""

from __future__ import annotations

import asyncio
import logging
import sys
from datetime import UTC, datetime, timedelta

import aiohttp
import numpy as np
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

from backtest.utils import DEFAULT_DATA_PATH, dt_to_ms

logger = logging.getLogger("backtest.download")

BINANCE_FAPI_URL = "https://fapi.binance.com/fapi/v1/klines"
LIMIT_PER_REQUEST = 1000
MS_PER_MIN = 60_000
CONCURRENCY_LIMIT = 5
RATE_LIMIT_DELAY = 0.1
OHLCV_COLUMNS = ["timestamp", "open", "high", "low", "close", "volume"]

def _read_partition(path) -> pd.DataFrame:
    """Read one part-0.parquet (avoids dataset merge issues on mixed schemas)."""
    from pathlib import Path

    path = Path(path)
    pf = pq.ParquetFile(path)
    names = set(pf.schema_arrow.names)
    cols = [c for c in OHLCV_COLUMNS if c in names]
    if not cols:
        raise ValueError(f"No OHLCV columns in {path}")
    df = pf.read(columns=cols).to_pandas()
    return df[OHLCV_COLUMNS]

def _write_partition(path, df: pd.DataFrame) -> None:
    clean = df[OHLCV_COLUMNS].copy()
    for col in ("open", "high", "low", "close", "volume"):
        clean[col] = clean[col].astype(np.float64)
    clean["timestamp"] = clean["timestamp"].astype(np.int64)
    pq.write_table(pa.Table.from_pandas(clean, preserve_index=False), path, compression="snappy")

async def fetch_klines_batch(
    session: aiohttp.ClientSession,
    symbol: str,
    start_ms: int,
    end_ms: int,
    semaphore: asyncio.Semaphore,
) -> list:
    params: dict[str, str | int] = {
        "symbol": symbol,
        "interval": "1m",
        "startTime": start_ms,
        "endTime": end_ms,
        "limit": LIMIT_PER_REQUEST,
    }
    async with semaphore:
        for attempt in range(5):
            try:
                async with session.get(BINANCE_FAPI_URL, params=params) as response:
                    if response.status == 200:
                        return await response.json()
                    if response.status in (429, 418):
                        retry_after = int(response.headers.get("Retry-After", 10))
                        logger.warning(
                            "Rate limited (%s) on %s. Sleep %ss.",
                            response.status,
                            symbol,
                            retry_after,
                        )
                        await asyncio.sleep(retry_after)
                    else:
                        logger.error(
                            "Fetch %s failed status %s (attempt %s).",
                            symbol,
                            response.status,
                            attempt + 1,
                        )
                        await asyncio.sleep(2**attempt)
            except Exception as exc:
                logger.error("Fetch %s at %s: %s", symbol, start_ms, exc)
                await asyncio.sleep(2**attempt)
    return []

async def download_symbol_range(
    session: aiohttp.ClientSession,
    symbol: str,
    start_ms: int,
    end_ms: int,
    semaphore: asyncio.Semaphore,
) -> pd.DataFrame:
    current_ts = start_ms
    all_raw: list = []

    logger.info(
        "Download %s %s -> %s",
        symbol,
        datetime.fromtimestamp(start_ms / 1000, tz=UTC),
        datetime.fromtimestamp(end_ms / 1000, tz=UTC),
    )

    while current_ts < end_ms:
        batch = await fetch_klines_batch(session, symbol, current_ts, end_ms, semaphore)
        if not batch:
            logger.warning("Empty batch %s at %s; advance 12h.", symbol, current_ts)
            current_ts += 12 * 60 * MS_PER_MIN
            await asyncio.sleep(RATE_LIMIT_DELAY)
            continue

        all_raw.extend(batch)
        last_fetched = batch[-1][0]
        next_ts = last_fetched + MS_PER_MIN
        if next_ts <= current_ts:
            next_ts = current_ts + LIMIT_PER_REQUEST * MS_PER_MIN
        current_ts = next_ts
        await asyncio.sleep(RATE_LIMIT_DELAY)

    if not all_raw:
        return pd.DataFrame()

    df = pd.DataFrame(all_raw).iloc[:, [0, 1, 2, 3, 4, 5]]
    df.columns = ["timestamp", "open", "high", "low", "close", "volume"]
    df["timestamp"] = df["timestamp"].astype(np.int64)
    for col in ("open", "high", "low", "close", "volume"):
        df[col] = df[col].astype(np.float64)
    return df

def get_min_timestamp_in_partition(base_path: str, symbol: str) -> int:
    from pathlib import Path

    root = Path(f"{base_path}/symbol={symbol}")
    if not root.is_dir():
        return 0

    min_ts = 0
    for year_dir in root.iterdir():
        if not year_dir.name.startswith("year="):
            continue
        part_path = year_dir / "part-0.parquet"
        if part_path.is_file():
            try:
                ts = _read_partition(part_path)["timestamp"]
                if len(ts):
                    local_min = int(ts.min())
                    min_ts = local_min if min_ts == 0 else min(min_ts, local_min)
            except Exception as exc:
                logger.error("Read %s: %s", part_path, exc)
    return min_ts

def get_max_timestamp_in_partition(base_path: str, symbol: str) -> int:
    symbol_dir = f"{base_path}/symbol={symbol}"
    from pathlib import Path

    root = Path(symbol_dir)
    if not root.is_dir():
        return 0

    max_ts = 0
    for year_dir in root.iterdir():
        if not year_dir.name.startswith("year="):
            continue
        part_path = year_dir / "part-0.parquet"
        if part_path.is_file():
            try:
                ts = _read_partition(part_path)["timestamp"]
                if len(ts):
                    max_ts = max(max_ts, int(ts.max()))
            except Exception as exc:
                logger.error("Read %s: %s", part_path, exc)
    return max_ts

def prepare_raw_df(df: pd.DataFrame, symbol: str) -> pd.DataFrame:
    """Dedupe and sort only ; gaps left for C++ engine forward-fill at load."""
    if df.empty:
        return df
    initial = len(df)
    df = df.drop_duplicates(subset=["timestamp"], keep="first")
    if len(df) < initial:
        logger.warning("[%s] Dropped %s duplicate rows.", symbol, initial - len(df))
    df = df.sort_values("timestamp").reset_index(drop=True)
    diffs = df["timestamp"].diff().dropna()
    if (diffs < 0).any():
        raise ValueError(f"[{symbol}] Timestamps not strictly increasing.")
    return df

def verify_continuity(df: pd.DataFrame, symbol: str) -> None:
    if df.empty or len(df) < 2:
        return
    min_ts = int(df["timestamp"].iloc[0])
    max_ts = int(df["timestamp"].iloc[-1])
    expected = int((max_ts - min_ts) / MS_PER_MIN) + 1
    if len(df) < expected:
        missing = expected - len(df)
        logger.warning(
            "[%s] %s gap minute(s) in stored raw data (engine will forward-fill <=60m).",
            symbol,
            missing,
        )
    else:
        logger.info("[%s] Partition continuous (%s rows).", symbol, len(df))

def save_partitioned_data(df: pd.DataFrame, symbol: str, base_path: str) -> None:
    if df.empty:
        return

    df = prepare_raw_df(df, symbol)
    df = df.copy()
    df["year"] = pd.to_datetime(df["timestamp"], unit="ms", utc=True).dt.year

    for year, group in df.groupby("year"):
        year_dir = f"{base_path}/symbol={symbol}/year={year}"
        from pathlib import Path

        Path(year_dir).mkdir(parents=True, exist_ok=True)
        part_path = f"{year_dir}/part-0.parquet"
        chunk = group.drop(columns=["year"])

        chunk = chunk[OHLCV_COLUMNS]
        if Path(part_path).is_file():
            existing = _read_partition(part_path)
            merged = pd.concat([existing, chunk], ignore_index=True)
            merged = prepare_raw_df(merged, symbol)
            _write_partition(part_path, merged)
            logger.info("[%s] Merged year %s -> %s rows.", symbol, year, len(merged))
        else:
            clean = prepare_raw_df(chunk, symbol)
            _write_partition(part_path, clean)
            logger.info("[%s] Wrote year %s -> %s rows.", symbol, year, len(clean))

def data_covers_range(base_path: str, symbol: str, start_ms: int, end_ms: int) -> bool:
    """True if parquet partitions cover ~99% of [start_ms, end_ms] (allows short live lag)."""
    from pathlib import Path

    sym_dir = Path(base_path) / f"symbol={symbol}"
    if not sym_dir.is_dir():
        return False

    frames = []
    for year_dir in sym_dir.iterdir():
        if not year_dir.name.startswith("year="):
            continue
        part = year_dir / "part-0.parquet"
        if part.is_file():
            frames.append(_read_partition(part))
    if not frames:
        return False

    full = pd.concat(frames, ignore_index=True).sort_values("timestamp")
    in_range = full[(full["timestamp"] >= start_ms) & (full["timestamp"] <= end_ms)]
    if in_range.empty:
        return False
    first = int(in_range["timestamp"].iloc[0])
    last = int(in_range["timestamp"].iloc[-1])
    expected_bars = max(1, int((end_ms - start_ms) / MS_PER_MIN))
    # Allow live lag up to 2h; require ~99% bar coverage in window
    tail_ok = last >= end_ms - (2 * 60 * MS_PER_MIN)
    head_ok = first <= start_ms + MS_PER_MIN
    coverage_ok = len(in_range) >= int(expected_bars * 0.99)
    return head_ok and tail_ok and coverage_ok

async def download_symbols(
    symbols: list[str],
    start_ms: int,
    end_ms: int,
    output: str | None = None,
) -> None:
    base = str(output or DEFAULT_DATA_PATH)
    from pathlib import Path

    Path(base).mkdir(parents=True, exist_ok=True)
    semaphore = asyncio.Semaphore(CONCURRENCY_LIMIT)

    async with aiohttp.ClientSession() as session:
        for symbol in symbols:
            min_existing = get_min_timestamp_in_partition(base, symbol)
            max_existing = get_max_timestamp_in_partition(base, symbol)
            sym_start = start_ms

            if max_existing >= end_ms - MS_PER_MIN and min_existing > 0 and min_existing <= start_ms + MS_PER_MIN:
                logger.info("%s already covers requested window.", symbol)
                continue
            if min_existing > start_ms + MS_PER_MIN:
                logger.info(
                    "%s missing early history (earliest=%s); backfill from %s",
                    symbol,
                    min_existing,
                    start_ms,
                )
                sym_start = start_ms
            elif max_existing > 0:
                sym_start = max(start_ms, max_existing + MS_PER_MIN)
                logger.info("Resume %s tail from %s", symbol, sym_start)

            if sym_start >= end_ms:
                logger.info("%s up to date.", symbol)
                continue

            # Chunk by ~30 days so each slice is saved (resume-safe, bounded RAM)
            chunk_ms = 30 * 24 * 60 * MS_PER_MIN
            cursor = sym_start
            while cursor < end_ms:
                slice_end = min(cursor + chunk_ms, end_ms)
                df = await download_symbol_range(session, symbol, cursor, slice_end, semaphore)
                if not df.empty:
                    save_partitioned_data(df, symbol, base)
                cursor = slice_end + MS_PER_MIN
            logger.info("Finished %s download slices.", symbol)

def run_download_cli(
    symbols: list[str],
    days: int,
    output: str | None = None,
    start_date: str | None = None,
    end_date: str | None = None,
) -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        handlers=[logging.StreamHandler(sys.stdout)],
    )

    if end_date:
        end_dt = datetime.strptime(end_date, "%Y-%m-%d").replace(
            tzinfo=UTC, hour=23, minute=59, second=59
        )
    else:
        end_dt = datetime.now(UTC)

    if start_date:
        start_dt = datetime.strptime(start_date, "%Y-%m-%d").replace(tzinfo=UTC)
    else:
        start_dt = end_dt - timedelta(days=days)

    asyncio.run(download_symbols(symbols, dt_to_ms(start_dt), dt_to_ms(end_dt), output))
