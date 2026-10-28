#pragma once
#include <chronos/execution/backtest.hpp>
#include <deque>

namespace chronos {
// A deliberately simple research example: long target quantity when the fast
// trailing arithmetic mean exceeds the slow mean, otherwise flat. It does not
// make a profitability claim. Mean calculations are long-double indicators;
// orders and the ledger remain checked integer arithmetic.
class MovingAverageCross final : public Strategy {
public:
    MovingAverageCross(InstrumentId instrument, Quantity target_quantity,
                       std::size_t fast_window = 5, std::size_t slow_window = 20)
        : instrument_(instrument), target_quantity_(target_quantity), fast_window_(fast_window), slow_window_(slow_window) {
        if (!instrument || target_quantity <= 0 || !fast_window || slow_window <= fast_window)
            throw std::invalid_argument("invalid moving-average strategy configuration");
    }
    std::vector<OrderRequest> on_event(const Event& event, const Portfolio& portfolio) override {
        if (event.header.instrument != instrument_) return {};
        if (std::holds_alternative<Split>(event.payload)) {
            prices_.clear(); // old unadjusted prices use the pre-split unit scale
            return {};
        }
        Price observation = 0;
        if (const auto* p = std::get_if<Bar>(&event.payload)) observation = p->close;
        if (const auto* p = std::get_if<Trade>(&event.payload)) observation = p->price;
        if (const auto* p = std::get_if<Quote>(&event.payload)) observation = checked_add(p->bid, (p->ask - p->bid) / 2);
        if (!observation) return {};
        prices_.push_back(observation);
        if (prices_.size() > slow_window_) prices_.pop_front();
        if (prices_.size() < slow_window_ || pending_id_) return {};
        long double slow = 0, fast = 0;
        for (std::size_t i = 0; i < prices_.size(); ++i) {
            slow += prices_[i];
            if (i >= prices_.size() - fast_window_) fast += prices_[i];
        }
        const auto target = fast / fast_window_ > slow / slow_window_ ? target_quantity_ : 0;
        const auto delta = checked_add(target, checked_negate(portfolio.quantity(instrument_)));
        if (!delta) return {};
        if (next_id_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("strategy order identifiers exhausted");
        pending_id_ = next_id_++;
        return {{pending_id_, instrument_, delta > 0 ? Side::Buy : Side::Sell, OrderType::Market, accounting_detail::magnitude(delta), 0}};
    }
    void on_execution(const ExecutionReport& report, const Portfolio&) override {
        if (report.order_id == pending_id_ && report.status != ExecutionStatus::Partial) pending_id_ = 0;
    }
private:
    InstrumentId instrument_;
    Quantity target_quantity_;
    std::size_t fast_window_, slow_window_;
    std::deque<Price> prices_;
    std::uint64_t next_id_{1}, pending_id_{};
};
} // namespace chronos
