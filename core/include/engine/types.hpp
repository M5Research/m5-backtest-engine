#pragma once

#include <cstdint>

namespace engine {

// Store prices and quantities as scaled integers.
// Symbol metadata will define the scaling factor (e.g., 100 for 2 decimal places).
using FixedPoint = int64_t;

// Standard sides for orders and trades
enum class Side : uint8_t {
    BUY = 0,
    SELL = 1,
    UNKNOWN = 2
};

// Event types representing the normalized stream
enum class EventType : uint8_t {
    DEPTH_DIFF = 0,
    DEPTH_SNAPSHOT = 1,
    PUBLIC_TRADE = 2,
    ORDER_SUBMIT = 3,
    ORDER_CANCEL = 4,
    EXECUTION_REPORT = 5,
    FUNDING_RATE = 6
};

// Order types
enum class OrderType : uint8_t {
    LIMIT = 0,
    MARKET = 1,
    LIMIT_MAKER = 2 // Post-only
};

// Time in Force
enum class TimeInForce : uint8_t {
    GTC = 0, // Good Till Canceled
    IOC = 1, // Immediate Or Cancel
    FOK = 2  // Fill Or Kill
};

enum class MarginMode : uint8_t {
    CROSS = 0,
    ISOLATED = 1
};

} // namespace engine
