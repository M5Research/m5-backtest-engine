# Backtest Data and Execution Validation

This guide covers data quality, execution assumptions, costs, risk limits, and comparison with paper trading.

> Data errors and optimistic fill assumptions can overstate backtest performance.

## Table of Contents

1. [Phase 0: Get Real Data](#phase-0-get-real-data)
2. [Phase 1: Make Fills Realistic](#phase-1-make-fills-realistic)
3. [Phase 2: Make Costs Accurate](#phase-2-make-costs-accurate)
4. [Phase 3: Make the Book Trustworthy](#phase-3-make-the-book-trustworthy)
5. [Phase 4: Make Risk Match Live](#phase-4-make-risk-match-live)
6. [Phase 5: Validate Against Reality](#phase-5-validate-against-reality)
7. [Phase 6: Speed Without Sacrificing Accuracy](#phase-6-speed-without-sacrificing-accuracy)
8. [Anti-Patterns to Avoid](#anti-patterns-to-avoid)
9. [Checklist: Before You Trust a Backtest](#checklist-before-you-trust-a-backtest)

## Phase 0: Get Real Data

Validate data before tuning the execution model. Hourly bars do not capture order book changes or order arrival timing.

### Step 0.1: Collect Tick-Level Trade Data

Download Binance historical trade data (aggTrades). These are real, tick-by-tick records of every trade that happened on the exchange.

```bash
# Binance provides free historical data downloads:
# https://data.binance.vision/
# Example: Download BTC/USDT aggTrades for January 2025
wget https://data.binance.vision/data/futures/um/daily/aggTrades/BTCUSDT/BTCUSDT-aggTrades-2025-01-15.zip
```

Each row contains:
| Field | What It Means |
|-------|---------------|
| `agg_trade_id` | Unique trade ID |
| `price` | Exact execution price |
| `quantity` | Trade size |
| `first_trade_id` | First trade in aggregation |
| `last_trade_id` | Last trade in aggregation |
| `timestamp` | Millisecond timestamp |
| `is_buyer_maker` | `true` = seller was the taker (sell aggressor) |

### Step 0.2: Collect L2 Depth Snapshots + Diffs

For strategies that depend on order book state (market making, spread trading), you need depth data:

```bash
# Binance depth snapshots (REST API, poll every ~60s)
# and depth diff stream (WebSocket, real-time updates)
#
# For historical data, use the T_DEPTH files from Binance Vision:
wget https://data.binance.vision/data/futures/um/daily/bookDepth/BTCUSDT/BTCUSDT-bookDepth-2025-01-15.zip
wget https://data.binance.vision/data/futures/um/daily/bookTicker/BTCUSDT/BTCUSDT-bookTicker-2025-01-15.zip
```

### Step 0.3: Normalize Into Binary Archive

Convert the raw CSVs into the engine's `CREARCHV` binary format using the normalizer script.

```bash
python backtest/python/scripts/binance_to_crearchv.py \
    --trades data/raw/BTCUSDT-aggTrades-2025-01-15.csv \
    --depth data/raw/BTCUSDT-bookDepth-2025-01-15.csv \
    --output data/normalized/BTCUSDT-2025-01-15.bin \
    --symbol-id 1 \
    --price-scale 100 \
    --qty-scale 1000
```

> The kline normalizer in `feed.cpp` synthesizes depth and trade events from bars. These events can support signal checks, but they do not reproduce observed spreads, order flow, or execution timing.

### Step 0.4: Validate the Normalized Data

Before running any strategy, verify your data pipeline:

```python
import m5_engine as qe

reader = qe.BinaryArchiveReader("data/normalized/BTCUSDT-2025-01-15.bin")
print(f"Events: {reader.size()}")
print(f"Symbols: {reader.num_symbols()}")

# Run with no strategy to check book state
registry = qe.SymbolRegistry()
meta = qe.SymbolMeta()
meta.symbol_id = 1
meta.symbol_name = "BTCUSDT"
meta.price_scale = 100
meta.qty_scale = 1000
meta.tick_size = 1
meta.lot_size = 1
meta.maker_fee_bps = 20  # 2 bps
meta.taker_fee_bps = 50  # 5 bps
registry.register_symbol(meta)

engine = qe.ReplayEngine(registry)
engine.run()

stats = engine.stats()
print(f"Depth events: {stats.depth_events}")
print(f"Trade events: {stats.trade_events}")
print(f"Sequence gaps: {stats.sequence_gaps}")  # Must be 0
```

**Pass criteria**:
- `sequence_gaps == 0`
- Trade count roughly matches Binance's reported 24h trade count
- First/last timestamps span the expected date range

## Phase 1: Make Fills Realistic

Compare simulated fills with observed execution. Optimistic fills can overstate performance.

### Step 1.1: Configure Latency to Match Your Real Infrastructure

Measure your actual round-trip latency to Binance and configure accordingly:

```yaml
# configs/backtest.yaml
execution:
  # Measure your real latency: ping api.binance.com from your server
  # Add ~5-10ms for exchange matching engine processing
  submission_latency_ms: 50.0    # Typical for cloud VPS
  cancellation_latency_ms: 30.0
  latency_jitter_ms: 10.0        # Variance you observe in production
```

```python
# Python setup
lat = qe.LatencyConfig()
lat.submission_ns = 50_000_000    # 50ms
lat.cancellation_ns = 30_000_000  # 30ms
lat.jitter_ns = 10_000_000        # +/-10ms uniform jitter
lat.rng_seed = 42                  # Deterministic for reproducibility
```

**How to measure your real latency**:
```python
import time, requests

times = []
for _ in range(100):
    start = time.perf_counter_ns()
    requests.get("https://fapi.binance.com/fapi/v1/time")
    times.append(time.perf_counter_ns() - start)

print(f"Median RTT: {sorted(times)[50] / 1e6:.1f} ms")
print(f"P95 RTT: {sorted(times)[95] / 1e6:.1f} ms")
# Use the P95 value as submission_latency_ns
```

### Step 1.2: Configure Queue Position Model

The queue model estimates the volume ahead of a limit order and how it depletes.

```python
queue = qe.QueueConfig()
queue.depletion_rate = 0.7           # 70% of trade volume depletes queue
                                      # (30% is cancellations, not real fills)
queue.cancel_decay_per_event = 0.995 # Queue shrinks 0.5% per event from cancellations
queue.initial_depth_fraction = 1.0   # You're behind 100% of displayed volume
```

**Tuning guidance**:

| Parameter | Conservative (pessimistic) | Moderate | Aggressive (optimistic) |
|-----------|---------------------------|----------|------------------------|
| `depletion_rate` | 0.5 | 0.7 | 0.9 |
| `cancel_decay` | 0.999 | 0.995 | 0.990 |
| `initial_depth_fraction` | 1.0 | 0.8 | 0.5 |

> Compare results across queue settings. Profitability under one setting does not establish live profitability.

### Step 1.3: Understand What the Engine Does With Your Orders

The execution model simulates the following cases:

| Scenario | Simulated behavior | Assumption |
|----------|-------------|---------------|
| Limit order at best bid | Queued behind displayed volume | Model estimate |
| Limit order improving price | Queue position = 0 (front) | Model estimate |
| Market order | Walks L2 book with slippage (VWAP) | Model estimate |
| Price trades through limit | Immediate full fill | Model estimate |
| Queue depleted | Partial fill based on overshoot | Model estimate |
| Order cancelled | Delayed by configured cancellation latency | Model estimate |
| IOC/FOK time-in-force | Cancel remainder or reject incomplete execution | Model estimate |

## Phase 2: Make Costs Accurate

### Step 2.1: Set Correct Fee Tiers

Match your actual Binance VIP tier:

| VIP Tier | Maker Fee | Taker Fee | `maker_fee_bps` | `taker_fee_bps` |
|----------|-----------|-----------|-----------------|-----------------|
| Regular | 0.0200% | 0.0500% | 20 | 50 |
| VIP 1 | 0.0160% | 0.0400% | 16 | 40 |
| VIP 2 | 0.0140% | 0.0350% | 14 | 35 |
| VIP 3 | 0.0120% | 0.0320% | 12 | 32 |
| BNB Discount | -25% off above | -25% off above | * 0.75 | * 0.75 |

```python
meta.maker_fee_bps = 20  # Regular tier, no BNB discount
meta.taker_fee_bps = 50  # Match the account fee schedule
```

### Step 2.2: Include Funding Rate Costs (Perpetuals)

If you hold positions across funding intervals (every 8 hours on Binance), funding payments directly impact P&L:

1. Download historical funding rate data from Binance
2. Inject `FUNDING_RATE` events into your archive at each 8-hour mark
3. The engine's `RiskManager::on_funding()` will compute the payment

```python
# Funding rate events should be injected at 00:00, 08:00, 16:00 UTC
# Rate is typically +/-0.01% but can spike to +/-0.5% in volatile markets
```

> Include historical funding rates when positions span funding intervals. Funding can change net P&L.

### Step 2.3: Account for Slippage on Market Orders

The engine now walks the L2 book for market orders. But the slippage depends on having accurate book depth data. If you only have trade data (no depth), add a manual slippage buffer:

```yaml
# Conservative slippage estimate when no depth data is available
spread:
  model: volume_adaptive
  base_spread_bps: 1.0    # 1 bps = 0.01% one-way slippage for BTC
  min_spread_bps: 0.5
  max_spread_bps: 30.0    # Widen during low liquidity / high vol
```

## Phase 3: Make the Book Trustworthy

### Step 3.1: Use Snapshot + Diff Alignment

The Binance depth stream requires careful synchronization (see `architecture.md` section 7):

1. **Buffer** incoming WebSocket depth diffs
2. **Fetch** REST depth snapshot (get `lastUpdateId`)
3. **Drop** diffs where `u <= lastUpdateId`
4. **Apply** first diff where `U <= lastUpdateId+1 && u >= lastUpdateId+1`
5. **Sequential** diffs: each `U` must equal previous `u + 1`
6. **Gap recovery**: if `U != prev_u + 1`, flush book and restart

The `SequenceValidator` class in `feed.hpp` implements this logic. **You must call it during normalization**, not just have it available.

### Step 3.2: Detect Crossed Books

Add a sanity check after each depth update:

```cpp
// After apply_diff(), verify book isn't crossed
if (best_bid >= best_ask && best_bid > 0 && best_ask > 0) {
    // Data error; log and skip this update
    stats_.crossed_book_count++;
}
```

A crossed book (bid >= ask) can indicate stale, incomplete, or incorrectly ordered updates. Check the data before using that book state.

### Step 3.3: Don't Trust Stale Book State

If your last depth update was >5 seconds ago, the book is stale. Don't make trading decisions on stale data:

```python
def on_market_event(self, event, book):
    # Skip if book hasn't been updated recently
    if event.exchange_ts - self.last_depth_ts > 5_000_000_000:  # 5 seconds
        return  # Book is stale, don't trade
```

## Phase 4: Make Risk Match Live

### Step 4.1: Mirror Your Live Risk Parameters

```python
risk = qe.RiskLimits()
risk.max_gross_exposure = 100_000_00   # $100k in fixed-point (scale 100)
risk.max_net_exposure = 50_000_00      # $50k
risk.max_order_size = 5_000            # 5.0 BTC (scale 1000)
risk.max_open_orders = 20
risk.max_position_per_sym = 2_000      # 2.0 BTC per symbol
```

> **These must match your live risk limits exactly.** If your backtest uses 10x leverage but live uses 5x, the backtest will show trades that live would reject.

### Step 4.2: Set Initial Balance to Match Live Account

```python
# If your live account has $10,000 USDT
engine = qe.ReplayEngine(
    registry,
    latency_config,
    queue_config,
    risk_limits,
    initial_balance=10_000_00  # $10,000 * price_scale(100)
)
```

### Step 4.3: Verify Margin Calculations

The engine uses:
- **10% Initial Margin** (10x leverage) for order acceptance
- **5% Maintenance Margin** for liquidation checks

If your Binance account uses different leverage, modify `RiskManager::update_margin()`:

```cpp
// For 20x leverage:
total_im += notional / 20;  // 5% Initial Margin
total_mm += notional / 50;  // 2% Maintenance Margin
```

## Phase 5: Validate Against Reality

Compare replay results with paper trading to assess execution assumptions.

### Step 5.1: Paper Trade Comparison

Run your strategy simultaneously in:
1. **Backtest** on historical data for a known period
2. **Binance Testnet** live paper trading for the same period

Compare:
- Total number of fills
- Average fill price vs. intended price
- Total fees paid
- Final P&L

Set comparison tolerances before the test. Compare fills, fees, and P&L separately.

### Step 5.2: Fill Rate Analysis

Track submitted orders and fills:

```python
class FillTracker(qe.Strategy):
    def __init__(self, engine):
        super().__init__()
        self.engine = engine
        self.orders_submitted = 0
        self.orders_filled = 0

    def on_market_event(self, event, book):
        # Your strategy logic...
        if should_trade:
            self.engine.submit_order(symbol_id, order)
            self.orders_submitted += 1

    def on_fill(self, fill, symbol_id):
        self.orders_filled += 1

# After run:
fill_rate = tracker.orders_filled / tracker.orders_submitted
print(f"Fill rate: {fill_rate:.1%}")
# Compare fill rates with observations for the same market and order placement.
```

### Step 5.3: Markout Analysis

Measure how the market moves **after** your fills. This reveals adverse selection:

```python
class MarkoutTracker(qe.Strategy):
    def __init__(self, engine):
        super().__init__()
        self.engine = engine
        self.fills = []  # (timestamp, price, side, symbol_id)
        self.prices = {}  # symbol_id -> latest price

    def on_market_event(self, event, book):
        if event.type == qe.EventType.PUBLIC_TRADE:
            self.prices[event.symbol_id] = event.trade.price

    def on_fill(self, fill, symbol_id):
        self.fills.append((fill, symbol_id, dict(self.prices)))

    def compute_markouts(self):
        # For each fill, check where the price went 1s, 5s, 60s later
        # If markouts are consistently negative, you're being adversely selected
        pass
```

**What to expect**:
- Compare markouts across replay and paper trading at the same horizons.
- Persistent differences can indicate mismatched fill assumptions or adverse selection.

### Step 5.4: Determinism Check

Run the same backtest twice and compare the reported summary values:

```python
# Already implemented in stress_tests/determinism_test.py
python backtest/stress_tests/determinism_test.py
# Must print: "SUCCESS: Both runs produced matching summary results."
```

### Step 5.5: Walk-Forward Validation

Never optimize parameters on the full dataset. Use walk-forward:

```
[====== Train (90 days) ======][== Test (30 days) ==]
                 [====== Train (90 days) ======][== Test (30 days) ==]
                                  [====== Train (90 days) ======][== Test (30 days) ==]
```

Only trust P&L numbers from the **out-of-sample test windows**.

## Phase 6: Speed Without Sacrificing Accuracy

### Step 6.1: Use Batch Mode for Signal-Only Strategies

If your strategy doesn't need per-event book state (e.g., it only looks at trade prices):

```python
engine.set_batch_mode(True)  # Batches 1024 events per Python callback
# Measure throughput with and without batch callbacks.
```

### Step 6.2: Use the Binary Archive Format

The `CREARCHV` binary format is `mmap`-able and avoids all parsing overhead:
- 128 bytes per event, cache-line aligned
- Zero-copy feeding into the scheduler
- Measure throughput on the target hardware.

### Step 6.3: Minimize Python Callback Work

The GIL is released during C++ event processing and reacquired for each Python callback. Keep callbacks lean:

```python
def on_market_event(self, event, book):
    # FAST: simple comparisons and order submission
    if event.trade.price > self.threshold:
        self.engine.submit_order(self.symbol_id, self.order)

    # SLOW: avoid NumPy/Pandas inside callbacks
    # Instead, accumulate data and process in batches
```

### Step 6.4: Profile Before Optimizing

```python
import time

start = time.perf_counter()
engine.run()
elapsed = time.perf_counter() - start

print(f"Events: {engine.stats().events_processed:,}")
print(f"Time: {elapsed:.2f}s")
print(f"Throughput: {engine.stats().events_processed / elapsed:,.0f} events/sec")
```

Report throughput together with hardware, build settings, data size, and callback workload.

## Anti-Patterns to Avoid

### "I'll just use close prices for fills"
Market orders don't fill at the close. They fill at whatever the book looks like when your order arrives, 50ms later.

### "My strategy has a 95% win rate"
A high win rate alone does not validate execution assumptions. Check fill prices, costs, and markouts.

### "I'll optimize parameters on the full dataset and then backtest on the full dataset"
This is textbook overfitting. Use walk-forward validation.

### "Fees don't matter, they're only 0.05%"
At 100 trades/day * $10k notional * 0.05% * 365 days, annual fees would total $182,500 under these assumptions.

### "I don't need depth data, trade data is enough"
For momentum/trend strategies, trade data is sufficient. For anything involving limit orders, spreads, or market making, you need depth data.

### "My backtest Sharpe is 5.0, this must be good"
Check out-of-sample results and parameter sensitivity. An in-sample Sharpe ratio alone does not establish expected live performance.

## Checklist: Before You Trust a Backtest

Use this checklist before deploying any strategy to live trading:

- [ ] **Data**: Using tick-level trade data (not klines/candles)
- [ ] **Data**: Depth data included (if strategy uses limit orders)
- [ ] **Data**: Sequence gaps = 0 in replay
- [ ] **Data**: No crossed books detected
- [ ] **Fees**: Maker/taker fees match my actual Binance VIP tier
- [ ] **Fees**: Funding rates included (if holding positions >8 hours)
- [ ] **Fills**: Latency configured to match my real server latency
- [ ] **Fills**: Queue depletion rate is conservative (<= 0.7)
- [ ] **Fills**: Fill rates checked against observed execution
- [ ] **Fills**: Market orders show slippage (not filling at best price)
- [ ] **Risk**: Max position, max exposure, leverage match live account
- [ ] **Risk**: Initial balance matches live account
- [ ] **Validation**: Walk-forward validation used (not in-sample only)
- [ ] **Validation**: Determinism test passes (identical results on replay)
- [ ] **Validation**: Markout analysis shows no excessive adverse selection
- [ ] **Validation**: Paper trading comparison meets predefined tolerances

## Quick-Start: Running Your First Realistic Backtest

```python
import m5_engine as qe

# 1. Registry with accurate fees
registry = qe.SymbolRegistry()
meta = qe.SymbolMeta()
meta.symbol_id = 1
meta.symbol_name = "BTCUSDT"
meta.price_scale = 100
meta.qty_scale = 1000
meta.tick_size = 1
meta.lot_size = 1
meta.maker_fee_bps = 20   # Account fee schedule
meta.taker_fee_bps = 50
registry.register_symbol(meta)

# 2. Conservative execution model
lat = qe.LatencyConfig()
lat.submission_ns = 50_000_000     # Measured submission latency
lat.jitter_ns = 10_000_000
lat.rng_seed = 42

queue = qe.QueueConfig()
queue.depletion_rate = 0.7         # Conservative
queue.cancel_decay_per_event = 0.995
queue.initial_depth_fraction = 1.0  # Pessimistic

# 3. Risk limits matching live
risk = qe.RiskLimits()
risk.max_gross_exposure = 100_000_00
risk.max_order_size = 1_000
risk.max_position_per_sym = 500
risk.max_open_orders = 20

# 4. Engine with the account balance
engine = qe.ReplayEngine(registry, lat, queue, risk, 10_000_00)

# 5. Load tick-level data
reader = qe.BinaryArchiveReader("data/normalized/BTCUSDT-2025-01-15.bin")
engine.add_stream(reader)

# 6. Attach strategy and run
strategy = MyStrategy(engine)
engine.set_strategy(strategy)
engine.run()

# 7. Inspect results
r = engine.risk()
print(f"Realized PnL: ${r.total_realized_pnl() / 100:.2f}")
print(f"Unrealized PnL: ${r.total_unrealized_pnl() / 100:.2f}")
print(f"Total Fees: ${sum(p.total_fees for _, p in r.positions().items()) / 100:.2f}")
print(f"Equity: ${r.equity() / 100:.2f}")

s = engine.stats()
print(f"Events processed: {s.events_processed:,}")
print(f"Fills: {s.fill_events}")
```

Examples cover the C++ replay engine and its execution and risk models.
