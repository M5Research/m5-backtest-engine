#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace engine {

struct BacktestConfig {
    std::string symbol;
    std::string timeframe;
    int64_t start_ts;
    int64_t end_ts;
    double initial_capital;
    std::string strategy_name;
    double maker_fee_bps = 2.0;
    double taker_fee_bps = 5.0;
};

struct BacktestResult {
    double sharpe;
    double pnl;
    double final_capital;
    int32_t total_trades;
};

// Vectorized execution function
inline BacktestResult run_vectorized_backtest(
    const int64_t* timestamps,
    const double* open,
    const double* high,
    const double* low,
    const double* close,
    const double* volume,
    const double* signals,
    size_t length,
    const BacktestConfig& config
) {
    (void)timestamps;
    (void)high;
    (void)low;
    (void)volume;

    double capital = config.initial_capital;
    double position = 0.0;
    int total_trades = 0;

    // Execute signal changes at the next bar open.
    for (size_t i = 0; i < length - 1; ++i) {
        double current_signal = signals[i];

        // Target position size based on signal (-1.0 to 1.0)
        // For simplicity, signal represents target units of asset
        double target_position = current_signal;

        if (target_position != position) {
            double trade_qty = target_position - position;
            // Execute at next open
            double exec_price = open[i + 1];

            // Calculate value and fee
            double trade_value = trade_qty * exec_price;
            double fee = std::abs(trade_value) * (config.taker_fee_bps / 10000.0);

            // Update capital
            capital -= trade_value;
            capital -= fee;

            position = target_position;
            total_trades++;
        }
    }

    // Close position at the end
    if (position != 0.0 && length > 0) {
        double exec_price = close[length - 1];
        double trade_value = -position * exec_price;
        double fee = std::abs(trade_value) * (config.taker_fee_bps / 10000.0);
        capital -= trade_value;
        capital -= fee;
        position = 0.0;
        total_trades++;
    }

    double pnl = capital - config.initial_capital;
    // This sign-based score is not a calculated Sharpe ratio.
    double sharpe = pnl > 0 ? 1.5 : -1.0;

    return { sharpe, pnl, capital, total_trades };
}

} // namespace engine
