"""Parse time ranges and resolve project paths."""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_DATA_PATH = PROJECT_ROOT / "data" / "binance_futures"
DEFAULT_CONFIG_PATH = PROJECT_ROOT / "backtest_project" / "config.example.json"

def parse_lookback(lookback: str) -> tuple[datetime, datetime]:
    """Parse lookback string into UTC (start, end) datetimes.

    Supports: Ny, Nm, Nd, or YYYY-MM-DD/YYYY-MM-DD.
    """
    now = datetime.now(UTC)
    lookback = lookback.strip()

    if lookback.endswith("y"):
        years = int(lookback[:-1])
        return now - timedelta(days=365 * years), now
    if lookback.endswith("m"):
        months = int(lookback[:-1])
        return now - timedelta(days=30 * months), now
    if lookback.endswith("d"):
        days = int(lookback[:-1])
        return now - timedelta(days=days), now
    if "/" in lookback:
        start_s, end_s = lookback.split("/", 1)
        start = datetime.strptime(start_s.strip(), "%Y-%m-%d").replace(tzinfo=UTC)
        end = datetime.strptime(end_s.strip(), "%Y-%m-%d").replace(
            tzinfo=UTC,
            hour=23,
            minute=59,
            second=59,
            microsecond=999999,
        )
        return start, end

    raise ValueError(
        f"Invalid lookback '{lookback}'. Use 3y, 1y, 6m, 90d, or YYYY-MM-DD/YYYY-MM-DD."
    )

def dt_to_ms(dt: datetime) -> int:
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=UTC)
    return int(dt.timestamp() * 1000)

def resolve_strategy_path(strategy_arg: str) -> Path:
    """Resolve strategy file from cwd, project root, or backtest_project/strategies."""
    raw = Path(strategy_arg)
    candidates = [
        raw,
        Path.cwd() / raw,
        PROJECT_ROOT / raw,
        PROJECT_ROOT / "backtest_project" / raw,
        PROJECT_ROOT / "backtest_project" / "strategies" / raw.name,
    ]
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise FileNotFoundError(f"Strategy file not found: {strategy_arg}")

def resolve_config_path(config_arg: str | None) -> Path | None:
    if config_arg is None:
        return DEFAULT_CONFIG_PATH if DEFAULT_CONFIG_PATH.is_file() else None
    path = Path(config_arg)
    if not path.is_file():
        path = PROJECT_ROOT / config_arg
    if path.is_file():
        return path.resolve()
    return None
