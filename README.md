# m5-backtest-engine

C++20 backtesting code for replaying crypto market data and simulating order execution.

The replay engine reads timestamped trades and order book updates, processes them in order, and tracks simulated fills and account state. Execution settings control submission delays, cancellation delays, latency jitter, and estimated queue position. Prices and quantities use scaled integers defined for each symbol.

A separate bar backtester takes target-position signals, executes changes at the next bar's open, and applies taker fees. It closes any remaining position at the final close.

This repository contains the source extracted from `M5Research/VolRegime-Engine`. The C++ core has its own build and tests. The Python scripts still need some dependencies and paths from the original project.

## Contents

| Path | Contents |
| --- | --- |
| `core/include/engine/` | Replay loop, event types, execution model, and risk tracking |
| `core/src/` | Order book code, data normalization, and Pybind11 bindings |
| `core/tests/` | Tests for the order book, scheduler, execution model, and replay engine |
| `python/scripts/` | CSV conversion to the engine's binary archive format |
| `stress_tests/` | Synthetic data generation, replay comparisons, and timing scripts |
| `docs/` | Design notes, examples, and validation guides |
| `cli.py`, `download.py`, `strategy1.py` | Scripts carried over from the original project |

## Build the C++ core

Requires CMake 3.20 or later and a C++20 compiler.

```sh
cmake -S core -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON -DBUILD_PYBIND11=OFF
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Use a Debug build when running the tests: their checks use `assert`, which is disabled in Release builds. Python bindings are optional and disabled in the command above.

## Convert CSV data

The converter uses the Python standard library. Run it from the repository root:

```sh
python python/scripts/binance_to_crearchv.py --trades trades.csv --depth depth.csv --output replay.bin --symbol-id 1 --price-scale 100 --qty-scale 1000
```

Trade CSVs need `timestamp`, `price`, `qty`, and `is_buyer_maker` columns. Depth CSVs need `timestamp`, `price`, `qty`, and `side` columns. Timestamps are milliseconds. Depth sides can be `b`, `buy`, or `bid` for bids; other values are treated as asks.

The output uses the `CREARCHV` archive format with 128-byte events. Match the price and quantity scales to the symbol metadata used during replay.

## Documentation

- [Architecture](docs/architecture.md)
- [Running backtests](docs/running-backtests.md)
- [Engine tutorial](docs/engine-tutorial.md)
- [Backtest validation](docs/backtest-guide.md)

## Current limits

- The Python package folder was removed. `python/setup.py` and scripts that import `m5_engine` need adjustment before installation or execution.
- The root CLI imports `backtest` and looks for `_backtest_engine` in a separate `backtest_project` directory. Those components are not included here.
- Data and compiled libraries are not included. Some scripts still use paths from the original project.
- The bar backtester's `sharpe` field is a sign-based score, not a calculated Sharpe ratio.
- Documentation includes examples from the original project. Check their imports and method names against the current source before using them.
