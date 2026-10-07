// Pybind11 Module: m5_engine
// C++ handles routing, book tracking, risk, and matching.
// Python callbacks handle strategy decisions and fill notifications.
//
// This module exposes:
//   - m5_engine.Side, .EventType, .OrderType, .TimeInForce
//   - m5_engine.SymbolMeta, .SymbolRegistry
//   - m5_engine.MarketEvent, .OrderBookL2
//   - m5_engine.Strategy (trampoline for Python subclassing)
//   - m5_engine.ReplayEngine
//   - m5_engine.BinaryArchiveReader/Writer
#ifdef BUILD_PYBIND11

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>
#include <pybind11/numpy.h>

#include "engine/types.hpp"
#include "engine/events.hpp"
#include "engine/book.hpp"
#include "engine/scheduler.hpp"
#include "engine/execution_model.hpp"
#include "engine/risk.hpp"
#include "engine/feed.hpp"
#include "engine/symbol_metadata.hpp"
#include "engine/replay_engine.hpp"
#include "engine/vector_engine.hpp"

namespace py = pybind11;
using namespace engine;
// Strategy Trampoline
// Allows Python classes to override the C++ StrategyCallback virtual methods.
// Python strategies implement on_market_event and
// on_fill, and submit orders back through the context.
class PyStrategy : public StrategyCallback {
public:
    using StrategyCallback::StrategyCallback;

    void on_init(const SymbolRegistry& registry) override {
        PYBIND11_OVERRIDE(void, StrategyCallback, on_init, registry);
    }

    void on_market_event(const MarketEvent& event,
                         const OrderBookL2& book) override {
        PYBIND11_OVERRIDE(void, StrategyCallback, on_market_event, event, book);
    }

    void on_market_event_batch(const std::vector<MarketEvent>& events) override {
        PYBIND11_OVERRIDE(void, StrategyCallback, on_market_event_batch, events);
    }

    void on_fill(const ExecutionReportPayload& fill,
                 uint16_t symbol_id) override {
        PYBIND11_OVERRIDE(void, StrategyCallback, on_fill, fill, symbol_id);
    }
};

PYBIND11_MODULE(_core, m) {
    m.doc() = "High-fidelity crypto market replay engine (C++20 core)";

    // Enums
    py::enum_<Side>(m, "Side")
        .value("BUY", Side::BUY)
        .value("SELL", Side::SELL)
        .value("UNKNOWN", Side::UNKNOWN);

    py::enum_<EventType>(m, "EventType")
        .value("DEPTH_DIFF", EventType::DEPTH_DIFF)
        .value("DEPTH_SNAPSHOT", EventType::DEPTH_SNAPSHOT)
        .value("PUBLIC_TRADE", EventType::PUBLIC_TRADE)
        .value("ORDER_SUBMIT", EventType::ORDER_SUBMIT)
        .value("ORDER_CANCEL", EventType::ORDER_CANCEL)
        .value("EXECUTION_REPORT", EventType::EXECUTION_REPORT);

    py::enum_<OrderType>(m, "OrderType")
        .value("LIMIT", OrderType::LIMIT)
        .value("MARKET", OrderType::MARKET)
        .value("LIMIT_MAKER", OrderType::LIMIT_MAKER);

    py::enum_<TimeInForce>(m, "TimeInForce")
        .value("GTC", TimeInForce::GTC)
        .value("IOC", TimeInForce::IOC)
        .value("FOK", TimeInForce::FOK);

    // Symbol Metadata
    py::class_<SymbolMeta>(m, "SymbolMeta")
        .def(py::init<>())
        .def_readwrite("symbol_id", &SymbolMeta::symbol_id)
        .def_readwrite("symbol_name", &SymbolMeta::symbol_name)
        .def_readwrite("price_scale", &SymbolMeta::price_scale)
        .def_readwrite("qty_scale", &SymbolMeta::qty_scale)
        .def_readwrite("tick_size", &SymbolMeta::tick_size)
        .def_readwrite("lot_size", &SymbolMeta::lot_size)
        .def_readwrite("maker_fee_bps", &SymbolMeta::maker_fee_bps)
        .def_readwrite("taker_fee_bps", &SymbolMeta::taker_fee_bps)
        .def("price_to_ticks", &SymbolMeta::price_to_ticks)
        .def("ticks_to_price", &SymbolMeta::ticks_to_price)
        .def("qty_to_ticks", &SymbolMeta::qty_to_ticks)
        .def("ticks_to_qty", &SymbolMeta::ticks_to_qty);

    py::class_<SymbolRegistry>(m, "SymbolRegistry")
        .def(py::init<>())
        .def("register_symbol", &SymbolRegistry::register_symbol)
        .def("get", &SymbolRegistry::get, py::return_value_policy::reference)
        .def("resolve", &SymbolRegistry::resolve)
        .def("size", &SymbolRegistry::size);

    // Payloads
    py::class_<DepthUpdatePayload>(m, "DepthUpdatePayload")
        .def(py::init<>())
        .def_readwrite("price", &DepthUpdatePayload::price)
        .def_readwrite("qty", &DepthUpdatePayload::qty)
        .def_readwrite("side", &DepthUpdatePayload::side);

    py::class_<PublicTradePayload>(m, "PublicTradePayload")
        .def(py::init<>())
        .def_readwrite("price", &PublicTradePayload::price)
        .def_readwrite("qty", &PublicTradePayload::qty)
        .def_readwrite("taker_side", &PublicTradePayload::taker_side);

    py::class_<OrderSubmitPayload>(m, "OrderSubmitPayload")
        .def(py::init<>())
        .def_readwrite("order_id", &OrderSubmitPayload::order_id)
        .def_readwrite("price", &OrderSubmitPayload::price)
        .def_readwrite("qty", &OrderSubmitPayload::qty)
        .def_readwrite("side", &OrderSubmitPayload::side)
        .def_readwrite("type", &OrderSubmitPayload::type)
        .def_readwrite("tif", &OrderSubmitPayload::tif);

    py::class_<ExecutionReportPayload>(m, "ExecutionReportPayload")
        .def(py::init<>())
        .def_readwrite("order_id", &ExecutionReportPayload::order_id)
        .def_readwrite("fill_price", &ExecutionReportPayload::fill_price)
        .def_readwrite("fill_qty", &ExecutionReportPayload::fill_qty)
        .def_readwrite("leaves_qty", &ExecutionReportPayload::leaves_qty)
        .def_readwrite("fee", &ExecutionReportPayload::fee)
        .def_readwrite("side", &ExecutionReportPayload::side)
        .def_readwrite("is_maker", &ExecutionReportPayload::is_maker)
        .def_readwrite("is_fully_filled", &ExecutionReportPayload::is_fully_filled);

    py::class_<FundingRatePayload>(m, "FundingRatePayload")
        .def(py::init<>())
        .def_readwrite("rate", &FundingRatePayload::rate);

    // Market Event
    py::class_<MarketEvent>(m, "MarketEvent")
        .def(py::init<>())
        .def_readwrite("exchange_ts", &MarketEvent::exchange_ts)
        .def_readwrite("local_ts", &MarketEvent::local_ts)
        .def_readwrite("sequence_id", &MarketEvent::sequence_id)
        .def_readwrite("symbol_id", &MarketEvent::symbol_id)
        .def_readwrite("type", &MarketEvent::type)
        .def_readwrite("flags", &MarketEvent::flags)
        .def_property("depth",
            [](const MarketEvent& self) { return self.payload.depth; },
            [](MarketEvent& self, const DepthUpdatePayload& p) { self.payload.depth = p; })
        .def_property("trade",
            [](const MarketEvent& self) { return self.payload.trade; },
            [](MarketEvent& self, const PublicTradePayload& p) { self.payload.trade = p; })
        .def_property("order_submit",
            [](const MarketEvent& self) { return self.payload.order_submit; },
            [](MarketEvent& self, const OrderSubmitPayload& p) { self.payload.order_submit = p; })
        .def_property("exec_report",
            [](const MarketEvent& self) { return self.payload.exec_report; },
            [](MarketEvent& self, const ExecutionReportPayload& p) { self.payload.exec_report = p; })
        .def_property("funding",
            [](const MarketEvent& self) { return self.payload.funding; },
            [](MarketEvent& self, const FundingRatePayload& p) { self.payload.funding = p; });

    // Order Book
    py::class_<OrderBookL2>(m, "OrderBookL2")
        .def(py::init<uint16_t, FixedPoint>())
        .def("get_best_bid", &OrderBookL2::get_best_bid)
        .def("get_best_ask", &OrderBookL2::get_best_ask)
        .def("clear", &OrderBookL2::clear);

    // Strategy (trampoline for Python subclassing)
    py::class_<StrategyCallback, PyStrategy>(m, "Strategy")
        .def(py::init<>())
        .def("on_init", &StrategyCallback::on_init)
        .def("on_market_event", &StrategyCallback::on_market_event)
        .def("on_market_event_batch", &StrategyCallback::on_market_event_batch)
        .def("on_fill", &StrategyCallback::on_fill);

    // Config structs
    py::class_<LatencyConfig>(m, "LatencyConfig")
        .def(py::init<>())
        .def_readwrite("submission_ns", &LatencyConfig::submission_ns)
        .def_readwrite("cancellation_ns", &LatencyConfig::cancellation_ns)
        .def_readwrite("jitter_ns", &LatencyConfig::jitter_ns)
        .def_readwrite("rng_seed", &LatencyConfig::rng_seed);

    py::class_<QueueConfig>(m, "QueueConfig")
        .def(py::init<>())
        .def_readwrite("depletion_rate", &QueueConfig::depletion_rate)
        .def_readwrite("cancel_decay_per_event", &QueueConfig::cancel_decay_per_event)
        .def_readwrite("initial_depth_fraction", &QueueConfig::initial_depth_fraction);

    py::class_<RiskLimits>(m, "RiskLimits")
        .def(py::init<>())
        .def_readwrite("max_gross_exposure", &RiskLimits::max_gross_exposure)
        .def_readwrite("max_net_exposure", &RiskLimits::max_net_exposure)
        .def_readwrite("max_order_size", &RiskLimits::max_order_size)
        .def_readwrite("max_open_orders", &RiskLimits::max_open_orders)
        .def_readwrite("max_position_per_sym", &RiskLimits::max_position_per_sym);

    // Engine Stats
    py::class_<EngineStats>(m, "EngineStats")
        .def_readonly("events_processed", &EngineStats::events_processed)
        .def_readonly("depth_events", &EngineStats::depth_events)
        .def_readonly("trade_events", &EngineStats::trade_events)
        .def_readonly("order_events", &EngineStats::order_events)
        .def_readonly("fill_events", &EngineStats::fill_events)
        .def_readonly("sequence_gaps", &EngineStats::sequence_gaps)
        .def_readonly("first_event_ts", &EngineStats::first_event_ts)
        .def_readonly("last_event_ts", &EngineStats::last_event_ts);

    py::class_<PositionState>(m, "PositionState")
        .def_readonly("quantity", &PositionState::quantity)
        .def_readonly("cost_basis", &PositionState::cost_basis)
        .def_readonly("realized_pnl", &PositionState::realized_pnl)
        .def_readonly("unrealized_pnl", &PositionState::unrealized_pnl)
        .def_readonly("total_fees", &PositionState::total_fees)
        .def_readonly("total_funding", &PositionState::total_funding)
        .def_readonly("num_trades", &PositionState::num_trades);

    py::class_<RiskManager>(m, "RiskManager")
        .def("positions", &RiskManager::positions, py::return_value_policy::reference)
        .def("account", &RiskManager::account, py::return_value_policy::reference)
        .def("equity", &RiskManager::equity)
        .def("total_realized_pnl", &RiskManager::total_realized_pnl)
        .def("total_unrealized_pnl", &RiskManager::total_unrealized_pnl);

    py::class_<AccountState>(m, "AccountState")
        .def_readonly("balance", &AccountState::balance)
        .def_readonly("maintenance_margin", &AccountState::maintenance_margin)
        .def_readonly("initial_margin", &AccountState::initial_margin)
        .def_readonly("margin_mode", &AccountState::margin_mode);

    // Replay Engine
    py::class_<ReplayEngine>(m, "ReplayEngine")
        .def(py::init<SymbolRegistry, LatencyConfig, QueueConfig, RiskLimits, FixedPoint>(),
             py::arg("registry"),
             py::arg("latency") = LatencyConfig{},
             py::arg("queue") = QueueConfig{},
             py::arg("risk_limits") = RiskLimits{},
             py::arg("initial_balance") = 0)
        .def("set_strategy", &ReplayEngine::set_strategy, py::keep_alive<1, 2>())
        .def("set_batch_mode", &ReplayEngine::set_batch_mode)
        .def("add_stream", [](ReplayEngine& self, const BinaryArchiveReader& reader) {
            self.add_stream(reader.data(), reader.size());
        }, py::keep_alive<1, 2>())
        .def("inject_event", &ReplayEngine::inject_event)
        .def("submit_order", &ReplayEngine::submit_order)
        .def("cancel_order", &ReplayEngine::cancel_order)
        .def("run", &ReplayEngine::run, py::call_guard<py::gil_scoped_release>())
        .def("stats", &ReplayEngine::stats, py::return_value_policy::reference)
        .def("registry", &ReplayEngine::registry, py::return_value_policy::reference)
        .def("risk", &ReplayEngine::risk, py::return_value_policy::reference);

    // Archive I/O
    py::class_<BinaryArchiveReader>(m, "BinaryArchiveReader")
        .def(py::init<const std::string&>())
        .def("size", &BinaryArchiveReader::size)
        .def("num_symbols", &BinaryArchiveReader::num_symbols);

    // Vectorized Engine
    py::class_<BacktestConfig>(m, "BacktestConfig")
        .def(py::init<>())
        .def_readwrite("symbol", &BacktestConfig::symbol)
        .def_readwrite("timeframe", &BacktestConfig::timeframe)
        .def_readwrite("start_ts", &BacktestConfig::start_ts)
        .def_readwrite("end_ts", &BacktestConfig::end_ts)
        .def_readwrite("initial_capital", &BacktestConfig::initial_capital)
        .def_readwrite("strategy_name", &BacktestConfig::strategy_name)
        .def_readwrite("maker_fee_bps", &BacktestConfig::maker_fee_bps)
        .def_readwrite("taker_fee_bps", &BacktestConfig::taker_fee_bps);

    py::class_<BacktestResult>(m, "BacktestResult")
        .def_readonly("sharpe", &BacktestResult::sharpe)
        .def_readonly("pnl", &BacktestResult::pnl)
        .def_readonly("final_capital", &BacktestResult::final_capital)
        .def_readonly("total_trades", &BacktestResult::total_trades);

    m.def("run_backtest", [](
        py::array_t<int64_t> timestamps,
        py::array_t<double> open,
        py::array_t<double> high,
        py::array_t<double> low,
        py::array_t<double> close,
        py::array_t<double> volume,
        py::array_t<double> signals,
        const BacktestConfig& config
    ) {
        py::buffer_info ts_buf = timestamps.request();
        py::buffer_info open_buf = open.request();
        py::buffer_info high_buf = high.request();
        py::buffer_info low_buf = low.request();
        py::buffer_info close_buf = close.request();
        py::buffer_info vol_buf = volume.request();
        py::buffer_info sig_buf = signals.request();

        size_t length = ts_buf.shape[0];

        return run_vectorized_backtest(
            static_cast<int64_t*>(ts_buf.ptr),
            static_cast<double*>(open_buf.ptr),
            static_cast<double*>(high_buf.ptr),
            static_cast<double*>(low_buf.ptr),
            static_cast<double*>(close_buf.ptr),
            static_cast<double*>(vol_buf.ptr),
            static_cast<double*>(sig_buf.ptr),
            length,
            config
        );
    }, "Run vectorized backtest");
}

#endif // BUILD_PYBIND11
