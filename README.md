# m5-backtest-engine

C++20 market replay and bar backtesting code with Python scripts.

## Contents

- `core/`: event scheduling, order books, simulated execution, risk tracking, and C++ tests.
- `python/`: Python build configuration and a CSV conversion script.
- `stress_tests/`: archive generation, replay comparisons, and performance scripts.
- `docs/`: architecture, running instructions, and validation guides.

## Build the C++ core

Requires CMake 3.20 or later and a C++20 compiler.

```sh
cmake -S core -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## Documentation

- [Architecture](docs/architecture.md)
- [Running backtests](docs/running-backtests.md)
- [Engine tutorial](docs/engine-tutorial.md)
- [Backtest validation](docs/backtest-guide.md)

## Repository scope

This code was copied from the `backtest/` folder in `M5Research/VolRegime-Engine`. Some CLI, strategy, and stress-test scripts still refer to modules, configuration, or data paths from that parent project. Those scripts need their dependencies and paths configured before use. The documentation also includes examples from the parent project.

The Python package folder was removed. Existing packaging configuration and scripts that import `m5_engine` need adjustment before installation or execution.

The vectorized engine's `sharpe` field currently contains a sign-based score rather than a calculated Sharpe ratio.
