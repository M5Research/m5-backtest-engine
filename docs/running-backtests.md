# How to Backtest Strategies Using the C++ Engine

This guide covers backtest commands, data conversion, and Python strategies for the C++ engine (`m5_engine`).

## Using `backtest.py`

A backtest runner is located at the root of the project: **`backtest.py`**. It handles downloading data, caching it, converting it, and executing the C++ backtest in one command.

### Running a Backtest from the Terminal

1. **Open your terminal** and navigate to the project directory:
   ```powershell
   cd "D:\Coding\M5 Research\lin-regression"
   ```

2. **Execute the script** with the lookback window you want (e.g., `3m` for 3 months, `1y` for 1 year, `5y` for 5 years):
   ```powershell
   # Backtest the last 3 months
   python backtest.py 3m

   # Backtest the last 1 year
   python backtest.py 1y

   # Backtest the last 5 years
   python backtest.py 5y
   ```

### Runner workflow
1. **Cache Master Data:** The first time you run it, it downloads 5 years of historical klines from the Binance API and saves them to `data/study/`. Later runs can reuse cached data.
2. **Filter Window:** It extracts only the requested timeframe (e.g. the last 3 months).
3. **Compile Binary:** It packs the selected bars into standard C++ trade events (`.bin` format).
4. **C++ Replay:** Replays the events chronologically inside the C++ engine and prints your metrics.

## Ingesting Custom Tick & L2 Depth Data

For high-frequency trading (HFT) simulations that require full order book order queues, you can convert raw Binance L2 CSV files (aggTrades and bookDepth) to the engine's binary format:

```powershell
python backtest/python/scripts/binance_to_crearchv.py `
    --trades path/to/BTCUSDT-trades.csv `
    --depth path/to/BTCUSDT-depth.csv `
    --output data/study/BTCUSDT-custom.bin `
    --symbol-id 1 `
    --price-scale 100 `
    --qty-scale 1000
```
 `--price-scale 100` converts floats to integers (x100 to keep 2 decimals). `--qty-scale 1000` preserves 3 decimal places for quantities.

## Writing Your Strategy in Python

To build a strategy, subclass `m5_engine.Strategy` and implement the event hooks:

```python
import m5_engine as qe

class MySimpleStrategy(qe.Strategy):
    def __init__(self, engine, symbol_id):
        super().__init__()
        self.engine = engine
        self.symbol_id = symbol_id
        self.next_order_id = 1

    def on_market_event(self, event, book):
        """Called chronologically for every trade and order book update."""
        if event.type != qe.EventType.PUBLIC_TRADE:
            return

        # Access trade details directly on the event structure
        trade = event.trade

        # Simple Logic: If trade price is greater than 100, submit a buy order
        if trade.price > 10000:  # scaled price ($100.00 * 100)
            self.submit_order(qe.Side.BUY, trade.price)

    def submit_order(self, side, price):
        o = qe.OrderSubmitPayload()
        o.order_id = self.next_order_id
        self.next_order_id += 1
        o.side = side
        o.qty = 1000  # 1.0 quantity (scaled x1000, must be an int)
        o.price = price
        o.type = qe.OrderType.LIMIT
        o.tif = qe.TimeInForce.GTC

        self.engine.submit_order(self.symbol_id, o)

    def on_fill(self, fill, symbol_id):
        """Called automatically when your order gets filled."""
        print(f"[FILL] {fill.side.name} filled {fill.fill_qty} @ {fill.fill_price}")
```

## Executing Your Custom Strategy

You can bundle your strategy and backtest runner into a single executable script. We have provided `backtest/strategy1.py` as a ready-to-run template.

You can execute it using two methods:

### Method A: Direct Script Path (Recommended)
You can run it directly by specifying the path to the script file:
```powershell
python backtest/strategy1.py 3m
```

### Method B: Python Module Syntax
If you want to run it as an imported python module, use dot notation (do not use slashes after the `-m` flag):
```powershell
python -m backtest.strategy1 3m
```

*(Note: `python -m backtest/strategy1` will fail because the `-m` flag expects a standard Python dot-separated package name, not a file path).*

## Python and C++ bindings

Pybind11 connects the Python strategy to the C++ replay loop:
1. **The Core Module:** The C++ engine compiles into a native Windows library (`_core.cp311-win_amd64.pyd`) placed inside the Python `m5_engine` package.
2. **Subclassing C++ in Python:** When you write `class Strategy1(qe.Strategy)`, you are inheriting from a C++ class defined in C++. Pybind11 allows method overrides (`on_market_event`, `on_fill`) to cross the Python/C++ boundary.
3. **The Simulation Loop:**
   - When you call `engine.run()`, execution enters the C++ event loop.
   - The C++ `ReplayEngine` reads binary events from the `.bin` streams.
   - For every trade, C++ invokes the python method `on_market_event(...)` on your strategy.
   - If your strategy decides to place an order, it calls `self.engine.submit_order(...)`. The C++ engine schedules the order using the configured simulated latency.
   - When a fill occurs, the C++ engine fires `on_fill(...)` back in your Python code.

> **Pybind11 Memory & Garbage Collection Tip:**
> When adding stream readers to the C++ engine, you **must** assign the reader objects to local variables before calling `add_stream`.
>
> * **Avoid dropping the reader reference:**
>   ```python
>   engine.add_stream(qe.BinaryArchiveReader(btc_bin))  # No local reference to the reader
>   ```
> * **Correct (Keeps the object alive during execution):**
>   ```python
>   btc_reader = qe.BinaryArchiveReader(btc_bin)
>   engine.add_stream(btc_reader)
>   ```

## Advanced: Writing a Custom Replay Script

To write a custom backtest runner, use the following setup:

```python
import m5_engine as qe
from my_strategy import MySimpleStrategy

# 1. Configure symbol registries
registry = qe.SymbolRegistry()

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

# 2. Define latencies and margins
lat = qe.LatencyConfig()
lat.submission_ns = 35_000_000  # 35ms submission delay
lat.cancellation_ns = 30_000_000

# 3. Create the ReplayEngine
engine = qe.ReplayEngine(registry, lat, qe.QueueConfig(), qe.RiskLimits(), 100_000_00)

# 4. Attach strategy & add streams
strategy = MySimpleStrategy(engine, symbol_id=1)
engine.set_strategy(strategy)

# Keep readers alive until replay finishes.
btc_stream = qe.BinaryArchiveReader("data/study/BTCUSDT-custom.bin")
engine.add_stream(btc_stream)

# 5. Run simulation
engine.run()

# 6. Query results
risk = engine.risk()
print(f"Final Balance: ${risk.account().balance / 100:.2f}")
```
