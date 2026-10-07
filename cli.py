"""Run strategies and download historical data from the command line."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import sys
import time
from datetime import UTC, datetime
from pathlib import Path

from backtest.download import data_covers_range, run_download_cli
from backtest.utils import (
    DEFAULT_CONFIG_PATH,
    DEFAULT_DATA_PATH,
    PROJECT_ROOT,
    dt_to_ms,
    parse_lookback,
    resolve_config_path,
    resolve_strategy_path,
)

_ENGINE = None

def _add_msys_dll_path() -> None:
    if sys.platform == "win32":
        msys_bin = Path(r"C:\msys64\ucrt64\bin")
        if msys_bin.is_dir():
            os.add_dll_directory(str(msys_bin))

def _engine_search_paths() -> list[Path]:
    paths: list[Path] = []
    build_root = PROJECT_ROOT / "backtest_project" / "build"
    if build_root.is_dir():
        for child in sorted(build_root.iterdir()):
            if child.is_dir() and child.name.startswith("lib."):
                if list(child.glob("_backtest_engine*.pyd")) or list(child.glob("_backtest_engine*.so")):
                    paths.append(child)
    inplace_dir = PROJECT_ROOT / "backtest_project"
    if list(inplace_dir.glob("_backtest_engine*.pyd")) or list(inplace_dir.glob("_backtest_engine*.so")):
        paths.append(inplace_dir)
    return paths

def import_engine():
    global _ENGINE
    if _ENGINE is not None:
        return _ENGINE

    _add_msys_dll_path()
    for engine_dir in _engine_search_paths():
        path_str = str(engine_dir)
        if path_str not in sys.path:
            sys.path.insert(0, path_str)

    try:
        import _backtest_engine as engine  # noqa: PLC0415
    except ImportError as exc:
        raise SystemExit(
            "Cannot import _backtest_engine. Build first:\n"
            "  cd backtest_project && python setup.py build_ext --inplace\n"
            f"  ({exc})"
        ) from exc

    _ENGINE = engine
    return engine

def load_strategy_module(path: Path):
    module_name = f"strategy_{path.stem}"
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load strategy module from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    if not hasattr(module, "generate_signals"):
        raise AttributeError(f"{path} missing generate_signals(data) function")
    return module

def load_config(config_path: Path | None, symbol: str, data_path: str, timeframe: str) -> dict:
    config = {
        "data_path": data_path,
        "symbol": symbol,
        "timeframe": timeframe,
        "initial_capital": 10000.0,
        "slippage_pct": 0.0001,
        "taker_fee_pct": 0.0004,
        "max_gap_allowed_mins": 60,
        "position_size_type": "leverage",
        "position_size_value": 1.0,
    }
    if config_path and config_path.is_file():
        with config_path.open(encoding="utf-8") as fh:
            config.update(json.load(fh))
    config["symbol"] = symbol
    config["data_path"] = data_path
    return config

def print_dashboard(report: dict, meta: dict) -> None:
    cyan, green, red, yellow, bold, reset = "\033[96m", "\033[92m", "\033[91m", "\033[93m", "\033[1m", "\033[0m"
    ret_pct = report["total_return"] * 100
    ret_color = green if ret_pct >= 0 else red
    trades = report.get("trades", [])
    wins = [t for t in trades if t["pnl"] > 0]
    win_rate = (len(wins) / len(trades) * 100) if trades else 0.0

    print(f"\n{cyan}{bold}{'=' * 65}{reset}")
    print(f" {cyan}{bold}HIGH-FIDELITY EVENT-DRIVEN C++ SIMULATION REPORT{reset}")
    print(f"{cyan}{bold}{'=' * 65}{reset}")
    print(f" {bold}Asset/Symbol:{reset}      {meta['symbol']}")
    print(f" {bold}Timeframe:{reset}         {meta.get('timeframe', '1m')}")
    print(f" {bold}Backtest Period:{reset}   {meta['start_time'][:10]} to {meta['end_time'][:10]}")
    print(f" {bold}Strategy:{reset}          {meta['strategy_name']}")
    print(f" {bold}Strategy Hash:{reset}     {meta['strategy_hash'][:16]}")
    print(f" {bold}Sim Run Date:{reset}      {meta['run_timestamp'][:19]}")
    print(f"{cyan}{'-' * 65}{reset}")
    print(f" {bold}Initial Capital:{reset}   ${meta['config']['initial_capital']:.2f}")
    print(f" {bold}Ending Equity:{reset}     ${report['final_equity']:.2f}")
    print(f" {bold}Total Return:{reset}      {ret_color}{bold}{ret_pct:+.2f}%{reset}")
    sharpe = report.get("sharpe_ratio", 0.0)
    sharpe_color = green if sharpe > 1.0 else yellow
    print(f" {bold}Annualized Sharpe:{reset} {sharpe_color}{sharpe:.3f}{reset}")
    print(f" {bold}Max Drawdown:{reset}      {red}{report['max_drawdown'] * 100:.2f}%{reset}")
    print(f"{cyan}{'-' * 65}{reset}")
    print(f" {bold}Total Trades:{reset}      {report['total_trades']}")
    print(f" {bold}Win Rate:{reset}          {win_rate:.2f}% ({len(wins)} won, {len(trades) - len(wins)} lost)")
    if trades:
        pnls = [t["pnl"] for t in trades]
        avg = sum(pnls) / len(pnls)
        print(f" {bold}Avg Trade PnL:{reset}     ${green if avg >= 0 else red}{avg:+.2f}{reset}")
        print(f" {bold}Best Trade PnL:{reset}    ${green}{max(pnls):+.2f}{reset}")
        print(f" {bold}Worst Trade PnL:{reset}   ${red}{min(pnls):+.2f}{reset}")
    sim_s = meta.get("simulation_time_seconds")
    if sim_s is not None:
        print(f" {bold}Sim Wall Time:{reset}     {sim_s:.3f}s")
    print(f"{cyan}{bold}{'=' * 65}{reset}\n")

class _NumpyEncoder(json.JSONEncoder):
    def default(self, obj):
        try:
            import numpy as np

            if isinstance(obj, np.ndarray):
                return obj.tolist()
        except ImportError:
            pass
        return super().default(obj)

def cmd_backtest(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        prog="backtest",
        description="Run a strategy on the C++ event-driven backtest engine.",
    )
    parser.add_argument("strategy", help="Path to strategy .py (e.g. strategies/mean_reversion.py)")
    parser.add_argument("lookback", help="Window: 3y, 6m, 90d, or YYYY-MM-DD/YYYY-MM-DD")
    parser.add_argument("--symbol", default="BTCUSDT", help="Symbol (default: BTCUSDT)")
    parser.add_argument("--tf", default="1m", help="Bar timeframe (default: 1m; only 1m supported)")
    parser.add_argument("--config", default=None, help="JSON config path")
    parser.add_argument(
        "--data-path",
        default=None,
        help=f"Parquet root (default: {DEFAULT_DATA_PATH})",
    )
    parser.add_argument(
        "--download",
        action="store_true",
        help="Download missing Binance futures 1m data before run",
    )
    args = parser.parse_args(argv)

    if args.tf != "1m":
        print(f"[!] Warning: engine only supports 1m bars; ignoring --tf {args.tf}")

    try:
        start_dt, end_dt = parse_lookback(args.lookback)
    except ValueError as exc:
        print(f"[!] {exc}")
        return 1

    start_ms = dt_to_ms(start_dt)
    end_ms = dt_to_ms(end_dt)
    data_path = str(args.data_path or DEFAULT_DATA_PATH)

    has_data = data_covers_range(data_path, args.symbol, start_ms, end_ms)
    if not has_data:
        if not args.download:
            print("[*] Data missing or incomplete. Re-run with --download.")
            return 1
        days = max(1, int((end_dt - start_dt).total_seconds() / 86400) + 1)
        print(f"[*] Downloading {args.symbol} ({days} days) -> {data_path}")
        run_download_cli([args.symbol], days=days, output=data_path)

    try:
        strategy_path = resolve_strategy_path(args.strategy)
        strategy_module = load_strategy_module(strategy_path)
    except (FileNotFoundError, ImportError, AttributeError) as exc:
        print(f"[!] Strategy error: {exc}")
        return 1

    config_path = resolve_config_path(args.config)
    config = load_config(config_path, args.symbol, data_path, args.tf)
    if config_path:
        print(f"[*] Config: {config_path}")
    else:
        print(f"[*] Config: defaults ({DEFAULT_CONFIG_PATH.name} not found)")

    code_bytes = strategy_path.read_bytes()
    strategy_hash = hashlib.sha256(code_bytes).hexdigest()

    engine_mod = import_engine()
    print(f"[*] Engine load {config['symbol']} [{start_dt.date()} .. {end_dt.date()}]")
    try:
        engine = engine_mod.BacktestEngine(
            config["data_path"],
            config["symbol"],
            start_ms,
            end_ms,
            config,
        )
        print("[*] Running simulation...")
        t0 = time.perf_counter()
        report = engine.run(engine, strategy_module)
        elapsed = time.perf_counter() - t0
    except Exception as exc:
        print(f"[!] Engine failed: {exc}")
        return 1

    meta = {
        "strategy_path": str(strategy_path),
        "strategy_name": strategy_path.name,
        "strategy_hash": strategy_hash,
        "symbol": config["symbol"],
        "timeframe": args.tf,
        "start_time": start_dt.isoformat(),
        "end_time": end_dt.isoformat(),
        "run_timestamp": datetime.now(UTC).isoformat(),
        "config": config,
        "simulation_time_seconds": elapsed,
    }
    report["metadata"] = meta
    print_dashboard(report, meta)

    out_dir = PROJECT_ROOT / "backtest_results"
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_file = out_dir / f"report_{strategy_path.stem}_{config['symbol']}_{stamp}.json"
    try:
        with out_file.open("w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2, cls=_NumpyEncoder)
        print(f"[*] Report saved: {out_file}")
    except Exception as exc:
        print(f"[!] Could not save report: {exc}")

    return 0

def cmd_download(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(prog="backtest download", description="Download Binance futures 1m OHLCV.")
    parser.add_argument("--symbols", nargs="+", default=["BTCUSDT"], help="Symbols to download")
    parser.add_argument("--days", type=int, default=5 * 365, help="History length in days")
    parser.add_argument("--start", dest="start_date", default=None, help="Start YYYY-MM-DD")
    parser.add_argument("--end", dest="end_date", default=None, help="End YYYY-MM-DD")
    parser.add_argument(
        "--output",
        default=None,
        help=f"Parquet output root (default: {DEFAULT_DATA_PATH})",
    )
    args = parser.parse_args(argv)
    run_download_cli(
        args.symbols,
        days=args.days,
        output=args.output,
        start_date=args.start_date,
        end_date=args.end_date,
    )
    return 0

def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv:
        print("Usage: backtest <strategy.py> <lookback>  |  backtest download --symbols BTCUSDT --days 1825")
        return 1
    if argv[0] == "download":
        return cmd_download(argv[1:])
    return cmd_backtest(argv)

if __name__ == "__main__":
    raise SystemExit(main())
