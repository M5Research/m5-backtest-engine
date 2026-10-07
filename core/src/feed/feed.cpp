// Feed handler implementation.
// Binary archive I/O and sequence validation are header-only.
// This file provides the JSON/CSV normalization pipeline.
#include "engine/feed.hpp"
#include "engine/symbol_metadata.hpp"

#include <sstream>
#include <algorithm>
#include <cmath>

namespace engine {
// CSV Kline Normalizer
// Converts Binance kline CSV rows into MarketEvent arrays suitable for
// the replay engine. Each kline row generates three events:
//   1. A bid DEPTH_DIFF event
//   2. An ask DEPTH_DIFF event
//   3. A PUBLIC_TRADE event using close price and volume
//
// This is the bridge between OHLCV bar data and the tick-level engine.
// For true tick-level replay, use the binary archive format instead.
struct KlineRow {
    int64_t  open_time_ms;
    double   open;
    double   high;
    double   low;
    double   close;
    double   volume;
    int64_t  close_time_ms;
    double   quote_volume;
    int32_t  num_trades;
    double   taker_buy_base_vol;
    double   taker_buy_quote_vol;
};

// Parse a single CSV line into a KlineRow
bool parse_kline_csv_line(const std::string& line, KlineRow& out) {
    std::istringstream ss(line);
    std::string token;
    std::vector<std::string> tokens;

    while (std::getline(ss, token, ',')) {
        tokens.push_back(token);
    }

    if (tokens.size() < 11) return false;

    try {
        out.open_time_ms       = std::stoll(tokens[0]);
        out.open               = std::stod(tokens[1]);
        out.high               = std::stod(tokens[2]);
        out.low                = std::stod(tokens[3]);
        out.close              = std::stod(tokens[4]);
        out.volume             = std::stod(tokens[5]);
        out.close_time_ms      = std::stoll(tokens[6]);
        out.quote_volume       = std::stod(tokens[7]);
        out.num_trades         = std::stoi(tokens[8]);
        out.taker_buy_base_vol = std::stod(tokens[9]);
        out.taker_buy_quote_vol = std::stod(tokens[10]);
    } catch (...) {
        return false;
    }

    return true;
}

// Convert kline rows to MarketEvent arrays
std::vector<MarketEvent> normalize_klines(
    const std::vector<KlineRow>& klines,
    const SymbolMeta& meta)
{
    std::vector<MarketEvent> events;
    events.reserve(klines.size() * 2);

    uint64_t seq = 0;

    for (const auto& kline : klines) {
        int64_t ts_ns = kline.open_time_ms * 1'000'000; // ms -> ns

        // Estimate spread from high-low range
        double spread = (kline.high - kline.low) * 0.1; // 10% of range as spread est.
        if (spread <= 0) spread = kline.close * 0.0001;  // Fallback: 1 bps

        double mid = kline.close;
        double bid = mid - spread / 2.0;
        double ask = mid + spread / 2.0;

        // Event 1: Bid depth update
        MarketEvent bid_event{};
        bid_event.exchange_ts = ts_ns;
        bid_event.local_ts    = ts_ns;
        bid_event.sequence_id = seq++;
        bid_event.symbol_id   = meta.symbol_id;
        bid_event.type        = EventType::DEPTH_DIFF;
        bid_event.flags       = 0;
        bid_event.payload.depth.price = meta.price_to_ticks(bid);
        bid_event.payload.depth.qty   = meta.qty_to_ticks(kline.volume * 0.5);
        bid_event.payload.depth.side  = Side::BUY;
        events.push_back(bid_event);

        // Event 2: Ask depth update
        MarketEvent ask_event{};
        ask_event.exchange_ts = ts_ns;
        ask_event.local_ts    = ts_ns;
        ask_event.sequence_id = seq++;
        ask_event.symbol_id   = meta.symbol_id;
        ask_event.type        = EventType::DEPTH_DIFF;
        ask_event.flags       = 0;
        ask_event.payload.depth.price = meta.price_to_ticks(ask);
        ask_event.payload.depth.qty   = meta.qty_to_ticks(kline.volume * 0.5);
        ask_event.payload.depth.side  = Side::SELL;
        events.push_back(ask_event);

        // Event 3: Public trade at close price
        MarketEvent trade_event{};
        trade_event.exchange_ts = ts_ns + 1; // Slightly after depth
        trade_event.local_ts    = ts_ns + 1;
        trade_event.sequence_id = seq++;
        trade_event.symbol_id   = meta.symbol_id;
        trade_event.type        = EventType::PUBLIC_TRADE;
        trade_event.flags       = 0;
        trade_event.payload.trade.price      = meta.price_to_ticks(kline.close);
        trade_event.payload.trade.qty        = meta.qty_to_ticks(kline.volume);
        // Determine taker side from price action
        trade_event.payload.trade.taker_side =
            (kline.close >= kline.open) ? Side::BUY : Side::SELL;
        events.push_back(trade_event);
    }

    return events;
}

} // namespace engine
