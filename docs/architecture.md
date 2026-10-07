# Crypto Market Replay Engine Architecture

**Target Infrastructure**: C++20 Core, Pybind11, Python 3.11+
**Status**: Initial Implementation Specification

## 1. System Architecture

The replay engine processes market events in timestamp order. Feed handling, order book updates, event scheduling, and execution simulation run in C++20. Strategy and research code run in Python.

### Two-Tier Data Representation Invariant
The design separates raw data from replay state:
1. **Raw Event Archive**: The immutable source of truth. Contains the exact, unmodified payload retrieved from Binance (e.g., exact JSON blobs or WebSocket binary frames, exchange timestamps, and sequence IDs).
2. **Normalized Replay State**: A fixed-point, memory-optimized binary stream. All exchange symbols, prices, and quantities are normalized into `int64_t` fixed-point representations using symbol-specific scaling factors (metadata). Fixed-point scaling makes price and quantity conversions explicit.

### Python / C++ Boundary
The engine loop executes exclusively in C++. Python callbacks (e.g., `on_depth`, `on_trade`) are sparse and boundary-based. C++ handles routing, full depth tracking, risk-limit evaluation, and matching simulator logic. Python is invoked *only* to make trading decisions, passing order intents back to the C++ core.

## 2. Folder Structure

```text
crypto-replay-engine/
|-- core/                       # C++20 Core Engine
|   |-- CMakeLists.txt
|   |-- src/
|   |   |-- engine/             # Event scheduler, core loop
|   |   |-- feed/               # Raw ingestion and normalization parsers
|   |   |-- book/               # L2 Order book reconstruction
|   |   |-- execution/          # Execution simulator (queue & latency models)
|   |   |-- risk/               # Pre-trade risk tracking
|   |   +-- bindings/           # Pybind11 trampoline classes
|   +-- include/                # Public C++ headers
|-- python/                     # Python Research Interface
|   |-- m5_engine/
|   |   |-- __init__.py
|   |   |-- strategy.py         # Python strategy base class
|   |   +-- research/           # OFI, microprice, and toxicity analysis tools
|-- data/
|   |-- raw/                    # Immutable raw exchange archives
|   +-- normalized/             # Deterministic binary replay state
|-- scripts/                    # Entrypoints for ingestion, normalization, and backtesting
+-- docs/
```

## 3. C++ Class Design

```cpp
namespace engine {

// Represents the normalized fixed-point event payload.
struct MarketEvent {
    int64_t exchange_ts;
    int64_t local_ts;
    uint64_t sequence_id;
    int64_t price;           // Fixed-point scaled
    int64_t qty;             // Fixed-point scaled
    uint16_t symbol_id;
    uint8_t event_type;      // e.g., DEPTH_DIFF, TRADE
    uint8_t flags;           // e.g., IS_BUYER_MAKER
} __attribute__((packed));

class EventScheduler {
public:
    void push(const MarketEvent& e);
    void run();
private:
    // Depending on optimization, either a min-heap or a segmented time-wheel.
    std::priority_queue<MarketEvent, std::vector<MarketEvent>, EventComparator> pq_;
};

class OrderBookL2 {
public:
    void apply_snapshot(const Snapshot& snap);
    void apply_diff(const DepthUpdate& diff);
    int64_t get_best_bid() const;
private:
    // Symbol-specific price-to-index ladder for fast active level updates,
    // combined with a sparse structure (e.g., flat_map) for outliers.
    CompactLadder bids_;
    CompactLadder asks_;
};

class ExecutionModel {
public:
    void submit_order(const OrderIntent& order);
    void process_public_trade(const MarketEvent& trade);
private:
    LatencyModel latency_model_;
    std::unordered_map<uint64_t, SimulatedOrder> active_orders_;
};

} // namespace engine
```

## 4. Python Strategy API

The Python API limits the GIL impact by minimizing sub-event noise.

```python
import m5_engine as qe

class LeadLagStrategy(qe.Strategy):
    def on_init(self, context: qe.Context):
        self.btc = context.resolve_symbol("BTCUSDT")
        self.eth = context.resolve_symbol("ETHUSDT")

    def on_depth_update(self, event: qe.DepthEvent):
        # Memory-view into the C++ L2 order book
        eth_book = self.context.get_l2_book(self.eth)

        # Decision boundary: Check if our normalized signal crosses threshold
        if self.analyze_spread(eth_book) > self.threshold:
            self.context.submit_limit_order(
                symbol=self.eth,
                side=qe.Side.BUY,
                price_ticks=eth_book.best_bid_ticks(), # Fixed-point
                qty_ticks=1000                         # Fixed-point
            )
```

## 5. Event Schemas

To avoid virtual function overhead and memory fragmentation, events are designed as tightly packed C-struct discriminated unions.

*   **Fixed-Point Scaling**: Each symbol has a metadata config indicating the tick size (e.g., $0.01 \rightarrow 100$ scale factor). $50,000.00 \rightarrow 5,000,000$.
*   **Alignment**: Schemas fit cleanly into 32-byte or 64-byte chunks to maximize L1/L2 cache utilization.

```cpp
struct InternalEvent {
    int64_t timestamp;
    uint16_t symbol_id;
    uint8_t type; // Discriminated union tag
    union {
        DepthUpdate depth;
        PublicTrade trade;
        StrategyOrder order;
        ExecutionReport fill;
    } payload;
};
```

## 6. Storage Format

*   **Raw Data (`data/raw/`)**: Zstd-compressed JSON Lines or raw WebSocket binary archives.
*   **Normalized Data (`data/normalized/`)**: Custom binary format or serialized FlatBuffers. The normalizer script parses raw JSON, validates sequence IDs, aligns snapshots, converts floats to fixed-point, and outputs a binary file that can be `mmap`'d directly into the C++ engine memory space.

## 7. Order Book Reconstruction Algorithm

**Public market data supports L2 reconstruction only.** True L3 (order ownership tracking) is unavailable.

**Algorithm:**
1.  **Buffer**: Queue incoming WebSocket depth diffs (`U`, `u` sequence IDs).
2.  **Snapshot**: Fetch REST depth snapshot (`lastUpdateId`).
3.  **Align**: Drop diffs where `u <= lastUpdateId`. Apply the first diff where `U <= lastUpdateId + 1 && u >= lastUpdateId + 1`.
4.  **Chronological Updates**: Apply diffs strictly sequentially.
5.  **Data Structure**: Map the active trading range to a `CompactLadder` (a contiguous array where index = price offset from a base). Outlier prices map to a secondary `std::map`.
6.  **Gap Recovery**: If `next_U != current_u + 1`, a sequence gap is logged. The book is flushed, and the sync process restarts.

## 8. Execution Simulator Design

**Model assumptions**: The execution simulator is a *model*, not a reconstruction of exchange truth. Hidden liquidity, exact exchange latencies, and third-party queue priority are unobservable.

**Simulation Model:**
1.  **Latency Queue**: Submitted orders are delayed by a simulated network + exchange processing latency before entering the matching pool.
2.  **Queue Position Estimation**: When a limit order is placed, its queue position is estimated as the total displayed volume currently residing at that price level.
3.  **Queue Depletion**: As public market trades execute at the limit price, the queue position is decremented based on a probabilistic depletion half-life (not assuming every public trade depletes the queue 1:1, as cancellations occur).
4.  **Fill Generation**: When estimated `queue_position <= 0`, a fill event is generated.
5.  **Slippage**: Market orders walk the L2 book array until the target fixed-point volume is exhausted.

## 9. Risk and Portfolio Module

Risk tracking is pushed completely into C++ to prevent latency when paper/live trading.
*   **Inventory Tracking**: Hard checks on maximum open orders, maximum gross exposure, and max order sizing.
*   **Margin & Fees**: Replay explicitly subtracts taker/maker fees (e.g., 0.05% via fixed-point logic) per fill.
*   **PnL Calculation**: Tracks realized and unrealized PnL using VWAP cost basis.

## 10. Replay Engine Pseudocode & Scheduler Tradeoffs

```cpp
void Engine::run() {
    while (!scheduler_.empty()) {
        auto event = scheduler_.pop();

        switch (event.type) {
            case DEPTH_DIFF:
                book_[event.symbol_id].apply_diff(event.payload.depth);
                break;
            case PUBLIC_TRADE:
                execution_model_.update_queue_depletion(event.payload.trade);
                break;
            case ORDER_SUBMIT:
                execution_model_.submit(event.payload.order);
                break;
        }

        // Trigger Python Strategy at specific signal boundaries
        if (event.type == DEPTH_DIFF || event.type == PUBLIC_TRADE) {
            strategy_->on_market_event(event);
        }

        // Check for simulated fills resulting from public trades
        execution_model_.tick_fills(event.timestamp, scheduler_);
    }
}
```

**Scheduler Tradeoffs**:
*   **Min-Heap (`std::priority_queue`)**: Standard O(log N) operations. Easy to implement, but pointer chasing causes L3 cache misses as it grows.
*   **Calendar Queue / Time Wheel**: O(1) for closely grouped events. Suitable for events grouped into microsecond buckets, but requires careful bucket sizing.
*   **Monotonic Append + Merge (Chosen)**: Since historical data is pre-sorted, we maintain N monotonic cursors (one per data stream) and perform a K-way merge in real-time. This entirely avoids priority queue overhead for historical streams, only using a min-heap for *internally generated* events (like simulated latency timers).

## 11. Testing and Validation Plan

Use these checks to validate replay behavior.

1.  **Fixed-Point Invariants**: Verify that parsing `$0.300` and `$0.3` result in identical tick integers for that symbol.
2.  **Snapshot/Diff Sync Correctness**: Feed the engine a chaotic stream of out-of-order diffs; assert that the book refuses to update until the overlapping snapshot sequence validates.
3.  **Sequence Gap Recovery**: Inject artificial sequence drops into the replay stream. Assert that the engine halts trading, flushes the L2 book, and outputs a sync-recovery log.
4.  **Restart Determinism**: Replaying the same normalized archive twice must yield the *exact* same byte-for-byte PnL ledger.
5.  **Fill Simulation Scenarios**: Test the queue depletion logic to check that an order placed at the back of a 1000 BTC queue is not filled until public trades + probabilistic cancellation models deplete the full 1000 BTC.

## 12. Performance Optimization Plan

1.  **Eliminate the GIL Cost**: Do not pass raw events into Python. Pass aggregated book states, or only invoke Python on periodic timer bounds.
2.  **Object Pooling**: `new` and `delete` are excluded in the proposed design inside the engine loop. Memory arenas pre-allocate all event and order structures.
3.  **mmap Normalized Files**: The binary replay state is memory-mapped directly into the OS page cache, allowing the OS to handle prefetching natively.

## 13. Deployment and Extensibility Roadmap

*   **Deployment**: The core build environment is standard Ubuntu 22.04 LTS via Docker, utilizing CMake, Ninja, and GCC 12/Clang 15. Validate Pybind11 and CPython compatibility on the deployment platform.
*   **Microstructure Research Roadmap**: Implement Python tools reading off the normalized data lake to calculate:
    *   **Order Flow Imbalance (OFI)**: Measuring bid/ask volume pressure.
    *   **Toxicity Proxies (VPIN)**: Volume-synchronized probability of informed trading.
    *   **Queue Depletion Half-Life**: Statistical models analyzing how quickly a displayed queue vanishes post-placement.
    *   **Markouts**: Forward 1s, 5s, 60s price changes measured immediately after a simulated execution.
*   **Extensibility**: The fixed-point C++ core can be extended to Options/Futures by adding funding-rate processing event models and dynamic margin limits.
