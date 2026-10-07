#pragma once

#include "engine/types.hpp"

// Cross-platform packed struct support
#ifdef _MSC_VER
    #define PACKED_STRUCT
    #define PACKED_END
    #pragma pack(push, 1)
#else
    #define PACKED_STRUCT __attribute__((packed))
    #define PACKED_END
#endif

namespace engine {
// Payload Definitions
struct DepthUpdatePayload {
    FixedPoint price;
    FixedPoint qty; // 0 indicates the price level should be removed
    Side side;
} PACKED_STRUCT;

struct PublicTradePayload {
    FixedPoint price;
    FixedPoint qty;
    Side taker_side; // The side of the active order that crossed the book
} PACKED_STRUCT;

struct FundingRatePayload {
    FixedPoint rate; // Fixed-point percentage
} PACKED_STRUCT;

struct OrderSubmitPayload {
    uint64_t order_id;
    FixedPoint price;
    FixedPoint qty;
    Side side;
    OrderType type;
    TimeInForce tif;
} PACKED_STRUCT;

struct ExecutionReportPayload {
    uint64_t order_id;
    FixedPoint fill_price;
    FixedPoint fill_qty;
    FixedPoint leaves_qty;
    FixedPoint fee;      // Negative for maker rebates, positive for taker fees
    Side side;           // Side of the original order (fixes erasure bug #6.1)
    bool is_maker;
    bool is_fully_filled;
} PACKED_STRUCT;
// Core Market Event
// Designed to fit neatly into cache lines. Uses a discriminated union to
// avoid virtual function dispatch and memory fragmentation.
struct MarketEvent {
    int64_t exchange_ts;     // 8 bytes
    int64_t local_ts;        // 8 bytes
    uint64_t sequence_id;    // 8 bytes
    uint16_t symbol_id;      // 2 bytes
    EventType type;          // 1 byte
    uint8_t flags;           // 1 byte (padding/custom flags)

    // Payload union; the discriminated union tag is `type`
    union Payload {
        DepthUpdatePayload depth;
        PublicTradePayload trade;
        OrderSubmitPayload order_submit;
        ExecutionReportPayload exec_report;
        FundingRatePayload funding;
        uint8_t padding[100]; // Explicitly pad to 100 bytes
    } payload;

} PACKED_STRUCT;

static_assert(sizeof(MarketEvent) == 128, "MarketEvent must be exactly 128 bytes");

#ifdef _MSC_VER
    #pragma pack(pop)
#endif

// Static assertions to guarantee size invariants
static_assert(sizeof(DepthUpdatePayload) <= 48, "Payload too large");
static_assert(sizeof(PublicTradePayload) <= 48, "Payload too large");
static_assert(sizeof(OrderSubmitPayload) <= 48, "Payload too large");
static_assert(sizeof(ExecutionReportPayload) <= 48, "Payload too large");
static_assert(sizeof(MarketEvent) <= 128, "MarketEvent must fit in two 64-byte cache lines");

} // namespace engine

#undef PACKED_STRUCT
#undef PACKED_END
