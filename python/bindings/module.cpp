#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <chronos/analytics/statistics.hpp>
#include <chronos/execution/backtest.hpp>
#include <chronos/execution/strategies.hpp>
#include <chronos/market_data/data.hpp>
#include <chronos/replay/replay.hpp>

namespace py = pybind11;
using namespace chronos;
struct EventBatch { std::vector<Event> events; };
struct LoadedDataset { std::string metadata; EventBatch events; };
struct PyStrategy : Strategy {
    using Strategy::Strategy;
    std::vector<OrderRequest> on_event(const Event& e, const Portfolio& p) override {
        py::gil_scoped_acquire gil;
        auto method=py::get_override(static_cast<const Strategy*>(this),"on_event");
        if(!method) throw std::runtime_error("Strategy.on_event must be implemented");
        return method(py::cast(Event(e)),py::cast(Portfolio(p))).cast<std::vector<OrderRequest>>();
    }
    void on_execution(const ExecutionReport& e, const Portfolio& p) override {
        py::gil_scoped_acquire gil;
        if(auto method=py::get_override(static_cast<const Strategy*>(this),"on_execution"))
            method(py::cast(ExecutionReport(e)),py::cast(Portfolio(p)));
    }
    std::vector<std::uint64_t> cancel_orders(const Event& e, const Portfolio& p) override {
        py::gil_scoped_acquire gil;
        if(auto method=py::get_override(static_cast<const Strategy*>(this),"cancel_orders"))
            return method(py::cast(Event(e)),py::cast(Portfolio(p))).cast<std::vector<std::uint64_t>>();
        return {};
    }
};
PYBIND11_MODULE(_core, m) {
    m.doc() = "Chronos C++20 deterministic event simulation";
    m.attr("PRICE_SCALE") = price_scale;
    m.attr("SCHEMA_VERSION") = schema_version;
    py::enum_<Side>(m, "Side").value("BUY", Side::Buy).value("SELL", Side::Sell);
    py::enum_<OrderType>(m, "OrderType").value("MARKET", OrderType::Market).value("LIMIT", OrderType::Limit);
    py::enum_<ExecutionStatus>(m, "ExecutionStatus")
        .value("FILLED", ExecutionStatus::Filled).value("PARTIAL", ExecutionStatus::Partial)
        .value("CANCELLED", ExecutionStatus::Cancelled).value("REJECTED", ExecutionStatus::Rejected);
    py::enum_<ExecutionModel>(m, "ExecutionModel").value("BAR", ExecutionModel::Bar).value("TICK", ExecutionModel::Tick);
    py::class_<EventHeader>(m,"EventHeader").def(py::init<Timestamp,Timestamp,Timestamp,std::uint64_t,InstrumentId>())
        .def_readwrite("event_time", &EventHeader::event_time).def_readwrite("receive_time", &EventHeader::receive_time)
        .def_readwrite("available_time", &EventHeader::available_time).def_readwrite("sequence", &EventHeader::sequence)
        .def_readwrite("instrument", &EventHeader::instrument);
    py::class_<Trade>(m,"Trade").def(py::init<Price,Quantity>()).def_readwrite("price", &Trade::price).def_readwrite("quantity", &Trade::quantity);
    py::class_<Quote>(m,"Quote").def(py::init<Price,Price,Quantity,Quantity>()).def_readwrite("bid", &Quote::bid).def_readwrite("ask", &Quote::ask).def_readwrite("bid_size", &Quote::bid_size).def_readwrite("ask_size", &Quote::ask_size);
    py::class_<BookUpdate>(m,"BookUpdate").def(py::init<Side,Price,Quantity,std::uint32_t>()).def_readwrite("side", &BookUpdate::side).def_readwrite("price", &BookUpdate::price).def_readwrite("quantity", &BookUpdate::quantity).def_readwrite("level", &BookUpdate::level);
    py::class_<Bar>(m,"Bar").def(py::init<Price,Price,Price,Price,Quantity>()).def_readwrite("open", &Bar::open).def_readwrite("high", &Bar::high).def_readwrite("low", &Bar::low).def_readwrite("close", &Bar::close).def_readwrite("volume", &Bar::volume);
    py::class_<Dividend>(m,"Dividend").def(py::init<Price>()).def_readwrite("amount_per_share", &Dividend::amount_per_share);
    py::class_<Split>(m,"Split").def(py::init<std::int64_t,std::int64_t>()).def_readwrite("numerator", &Split::numerator).def_readwrite("denominator", &Split::denominator);
    py::class_<Event>(m,"Event").def(py::init<EventHeader,Payload>()).def_readwrite("header", &Event::header).def_readwrite("payload", &Event::payload);
    py::class_<EventBatch>(m,"EventBatch")
        .def(py::init<std::vector<Event>>())
        .def("__len__", [](const EventBatch& b){ return b.events.size(); })
        .def("__getitem__", [](const EventBatch& b, py::ssize_t i) {
            if(i < 0) i += static_cast<py::ssize_t>(b.events.size());
            if(i < 0 || static_cast<std::size_t>(i) >= b.events.size()) throw py::index_error();
            return b.events[static_cast<std::size_t>(i)]; // copy preserves immutable batch ownership
        });
    py::class_<LoadedDataset>(m,"Dataset").def_readonly("metadata", &LoadedDataset::metadata).def_readonly("events", &LoadedDataset::events);
    m.def("load_csv", [](const std::string& p) { return EventBatch{market_data::load_csv(p)}; }, py::call_guard<py::gil_scoped_release>());
    m.def("save_csv", [](const std::string& p,const EventBatch& b){ market_data::save_csv(p,b.events); }, py::call_guard<py::gil_scoped_release>());
    m.def("load_binary", [](const std::string& p){ auto d=market_data::load_binary(p); return LoadedDataset{std::move(d.metadata),{std::move(d.events)}}; }, py::call_guard<py::gil_scoped_release>());
    m.def("save_binary", [](const std::string& p,const EventBatch& b,const std::string& meta){market_data::save_binary(p,b.events,meta);},py::arg("path"),py::arg("events"),py::arg("metadata")="{}",py::call_guard<py::gil_scoped_release>());
    m.def("synthetic_trades", [](std::size_t n,std::uint64_t seed){return EventBatch{market_data::synthetic_trades(n,seed)};},py::arg("count"),py::arg("seed")=1,py::call_guard<py::gil_scoped_release>());
    m.def("synthetic_bars", [](std::size_t n,std::uint64_t seed){return EventBatch{market_data::synthetic_bars(n,seed)};},py::arg("count"),py::arg("seed")=1,py::call_guard<py::gil_scoped_release>());
    m.def("ordered_events", [](const EventBatch& b){return EventBatch{replay::ordered_events(b.events)};},py::call_guard<py::gil_scoped_release>());
    m.def("replay", [](const EventBatch& b,py::function f) { replay::Replay r(b.events); r.run([&](const Event& e){f(py::cast(Event(e)));}); });
    m.def("replay_checksum", [](const EventBatch& b){std::uint64_t sum=0; replay::Replay r(b.events);r.run([&](const Event& e){sum=sum*1099511628211ULL+e.header.sequence;});return sum;},py::call_guard<py::gil_scoped_release>());
    py::class_<OrderRequest>(m,"OrderRequest").def(py::init<std::uint64_t,InstrumentId,Side,OrderType,Quantity,Price>(),py::arg("id"),py::arg("instrument"),py::arg("side"),py::arg("type"),py::arg("quantity"),py::arg("limit_price")=0)
        .def_readwrite("id",&OrderRequest::id).def_readwrite("instrument",&OrderRequest::instrument).def_readwrite("side",&OrderRequest::side).def_readwrite("type",&OrderRequest::type).def_readwrite("quantity",&OrderRequest::quantity).def_readwrite("limit_price",&OrderRequest::limit_price);
    py::class_<ExecutionReport>(m,"ExecutionReport").def_readonly("order_id",&ExecutionReport::order_id).def_readonly("instrument",&ExecutionReport::instrument).def_readonly("time",&ExecutionReport::time).def_readonly("side",&ExecutionReport::side).def_readonly("quantity",&ExecutionReport::quantity).def_readonly("price",&ExecutionReport::price).def_readonly("fees",&ExecutionReport::fees).def_readonly("status",&ExecutionReport::status);
    py::class_<Position>(m,"Position").def_readonly("quantity",&Position::quantity).def_readonly("cost_basis",&Position::cost_basis).def_readonly("last_price",&Position::last_price).def_readonly("realized_pnl",&Position::realized_pnl).def_property_readonly("average_cost",&Position::average_cost).def_property_readonly("unrealized_pnl",&Position::unrealized_pnl);
    py::class_<Portfolio>(m,"Portfolio").def_property_readonly("cash",&Portfolio::cash).def_property_readonly("equity",&Portfolio::equity).def_property_readonly("fees",&Portfolio::fees).def_property_readonly("realized_pnl",&Portfolio::realized_pnl).def_property_readonly("unrealized_pnl",&Portfolio::unrealized_pnl).def_property_readonly("income",&Portfolio::income).def_property_readonly("gross_exposure",&Portfolio::gross_exposure).def_property_readonly("net_exposure",&Portfolio::net_exposure).def_property_readonly("positions",[](const Portfolio& p){return p.positions();});
    py::class_<ExecutionConfig>(m,"ExecutionConfig").def(py::init<>())
        .def_readwrite("bar_duration_ns",&ExecutionConfig::bar_duration_ns)
        .def_readwrite("model",&ExecutionConfig::model).def_readwrite("commission_per_unit",&ExecutionConfig::commission_per_unit).def_readwrite("exchange_fee_per_unit",&ExecutionConfig::exchange_fee_per_unit).def_readwrite("fixed_fee",&ExecutionConfig::fixed_fee).def_readwrite("slippage_bps",&ExecutionConfig::slippage_bps).def_readwrite("impact_bps",&ExecutionConfig::impact_bps).def_readwrite("max_order_quantity",&ExecutionConfig::max_order_quantity).def_readwrite("bar_liquidity",&ExecutionConfig::bar_liquidity).def_readwrite("allow_short",&ExecutionConfig::allow_short);
    py::class_<Strategy,PyStrategy>(m,"Strategy").def(py::init<>());
    py::class_<BuyAndHold,Strategy>(m,"BuyAndHold").def(py::init<InstrumentId,Quantity>(),py::arg("instrument")=1,py::arg("quantity")=10);
    py::class_<MovingAverageCross,Strategy>(m,"MovingAverageCross").def(py::init<InstrumentId,Quantity,std::size_t,std::size_t>(),py::arg("instrument")=1,py::arg("quantity")=10,py::arg("fast_window")=5,py::arg("slow_window")=20);
    py::class_<EquityPoint>(m,"EquityPoint").def_readonly("time",&EquityPoint::time).def_readonly("equity",&EquityPoint::equity).def_readonly("cash",&EquityPoint::cash).def_readonly("gross_exposure",&EquityPoint::gross_exposure).def_readonly("net_exposure",&EquityPoint::net_exposure);
    py::class_<BacktestResult>(m,"BacktestResult").def_readonly("portfolio",&BacktestResult::portfolio).def_readonly("reports",&BacktestResult::reports).def_readonly("equity_curve",&BacktestResult::equity_curve).def_readonly("pending_orders",&BacktestResult::pending_orders);
    // GIL stays held for Python strategies. Native-only convenience call releases it.
    m.def("run_backtest",[](const EventBatch& b,Strategy& s,Money cash,const ExecutionConfig& c){const auto config=c;return run_backtest(b.events,s,cash,config);},py::arg("events"),py::arg("strategy"),py::arg("initial_cash")=1'000'000*price_scale,py::arg("config")=ExecutionConfig{});
    m.def("backtest_buy_hold",[](const EventBatch& b,Quantity quantity,Money cash,const ExecutionConfig& c){const auto config=c;py::gil_scoped_release gil;BuyAndHold s(1,quantity);return run_backtest(b.events,s,cash,config);},py::arg("events"),py::arg("quantity")=10,py::arg("initial_cash")=1'000'000*price_scale,py::arg("config")=ExecutionConfig{});
    using namespace chronos::analytics;
    m.def("simple_returns",&simple_returns); m.def("log_returns",&log_returns); m.def("cumulative_return",&cumulative_return);
    m.def("annualized_volatility",&annualized_volatility,py::arg("returns"),py::arg("periods")=252);
    m.def("sharpe_ratio",&sharpe_ratio,py::arg("returns"),py::arg("periods")=252,py::arg("annual_risk_free")=0);
    m.def("sortino_ratio",&sortino_ratio,py::arg("returns"),py::arg("periods")=252,py::arg("annual_target")=0);
    m.def("maximum_drawdown",&maximum_drawdown);m.def("covariance",&covariance);m.def("correlation",&correlation);
    m.def("portfolio_volatility",&portfolio_volatility,py::arg("weights"),py::arg("covariance"),py::arg("periods")=252);
    m.def("beta",&beta);m.def("historical_var",&historical_var,py::arg("returns"),py::arg("confidence")=.95);
    m.def("historical_expected_shortfall",&historical_expected_shortfall,py::arg("returns"),py::arg("confidence")=.95);m.def("concentration",&concentration);
}
