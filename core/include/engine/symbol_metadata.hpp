#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <stdexcept>

#include "engine/types.hpp"

namespace engine {
// Symbol Metadata
// Each symbol has a fixed-point scaling configuration that maps real-world
// float prices/quantities to deterministic int64_t representations.
//
// Example: BTCUSDT with tick_size=0.10 -> scale_factor=10
//   $50,000.50 -> 500005 (in fixed-point ticks)
struct SymbolMeta {
    uint16_t    symbol_id;
    std::string symbol_name;     // e.g. "BTCUSDT"
    FixedPoint  price_scale;     // Multiplier to convert float price -> int64
    FixedPoint  qty_scale;       // Multiplier to convert float qty -> int64
    FixedPoint  tick_size;       // Min price increment in fixed-point
    FixedPoint  lot_size;        // Min qty increment in fixed-point
    FixedPoint  maker_fee_bps;   // Maker fee in basis-point fixed-point (e.g. 20 = 2bps)
    FixedPoint  taker_fee_bps;   // Taker fee in basis-point fixed-point (e.g. 50 = 5bps)

    // Convert a floating-point price to fixed-point
    [[nodiscard]] FixedPoint price_to_ticks(double price) const {
        return static_cast<FixedPoint>(price * static_cast<double>(price_scale));
    }

    // Convert fixed-point back to floating-point price
    [[nodiscard]] double ticks_to_price(FixedPoint ticks) const {
        return static_cast<double>(ticks) / static_cast<double>(price_scale);
    }

    // Convert a floating-point quantity to fixed-point
    [[nodiscard]] FixedPoint qty_to_ticks(double qty) const {
        return static_cast<FixedPoint>(qty * static_cast<double>(qty_scale));
    }

    [[nodiscard]] double ticks_to_qty(FixedPoint ticks) const {
        return static_cast<double>(ticks) / static_cast<double>(qty_scale);
    }
};
// Symbol Registry
// Symbol metadata lookup. Register symbols before replay; mutation is not synchronized.
class SymbolRegistry {
public:
    void register_symbol(const SymbolMeta& meta) {
        if (by_id_.count(meta.symbol_id)) {
            throw std::runtime_error("Duplicate symbol_id: " + std::to_string(meta.symbol_id));
        }
        by_id_[meta.symbol_id] = meta;
        by_name_[meta.symbol_name] = meta.symbol_id;
    }

    [[nodiscard]] const SymbolMeta& get(uint16_t id) const {
        auto it = by_id_.find(id);
        if (it == by_id_.end()) {
            throw std::out_of_range("Unknown symbol_id: " + std::to_string(id));
        }
        return it->second;
    }

    [[nodiscard]] uint16_t resolve(const std::string& name) const {
        auto it = by_name_.find(name);
        if (it == by_name_.end()) {
            throw std::out_of_range("Unknown symbol: " + name);
        }
        return it->second;
    }

    [[nodiscard]] size_t size() const { return by_id_.size(); }

private:
    std::unordered_map<uint16_t, SymbolMeta> by_id_;
    std::unordered_map<std::string, uint16_t> by_name_;
};

} // namespace engine
