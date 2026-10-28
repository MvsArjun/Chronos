#pragma once
#include <chronos/core/types.hpp>
#include <chronos/portfolio/portfolio.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace chronos {
enum class ExecutionModel { Bar, Tick };
struct ExecutionConfig {
    ExecutionModel model{ExecutionModel::Bar};
    Money commission_per_unit{};
    Money exchange_fee_per_unit{};
    Money fixed_fee{}; // per fill, including each partial fill
    std::int64_t slippage_bps{};
    std::int64_t impact_bps{}; // fixed adverse-price approximation, not calibrated
    Quantity max_order_quantity{1'000'000};
    Quantity bar_liquidity{1'000'000}; // assumed units per event, shared FIFO
    Timestamp bar_duration_ns{1}; // synthetic default; set actual interval explicitly
    bool allow_short{false}; // no margin or borrow model
};
class Strategy {
public:
    virtual ~Strategy() = default;
    virtual std::vector<OrderRequest> on_event(const Event&, const Portfolio&) = 0;
    virtual void on_execution(const ExecutionReport&, const Portfolio&) {}
    virtual std::vector<std::uint64_t> cancel_orders(const Event&, const Portfolio&) { return {}; }
};
class BuyAndHold final : public Strategy {
public:
    explicit BuyAndHold(InstrumentId instrument = 1, Quantity quantity = 10)
        : instrument_(instrument), quantity_(quantity) {
        if (!instrument || quantity <= 0) throw std::invalid_argument("invalid buy-and-hold configuration");
    }
    std::vector<OrderRequest> on_event(const Event& event, const Portfolio&) override {
        if (sent_ || event.header.instrument != instrument_ ||
            (!std::holds_alternative<Bar>(event.payload) && !std::holds_alternative<Quote>(event.payload))) return {};
        sent_ = true;
        return {{1, instrument_, Side::Buy, OrderType::Market, quantity_, 0}};
    }
private:
    InstrumentId instrument_;
    Quantity quantity_;
    bool sent_{};
};
struct EquityPoint {
    Timestamp time{};
    Money equity{};
    Money cash{};
    Money gross_exposure{};
    Money net_exposure{};
};
struct BacktestResult {
    Portfolio portfolio;
    std::vector<ExecutionReport> reports;
    std::vector<EquityPoint> equity_curve;
    std::size_t pending_orders{};
};

namespace execution_detail {
struct Pending { OrderRequest request; Quantity remaining{}; Timestamp accepted_at{}; };
inline void validate_config(const ExecutionConfig& config) {
    if ((config.model != ExecutionModel::Bar && config.model != ExecutionModel::Tick) ||
        config.commission_per_unit < 0 || config.exchange_fee_per_unit < 0 || config.fixed_fee < 0 ||
        config.slippage_bps < 0 || config.impact_bps < 0 ||
        checked_add(config.slippage_bps, config.impact_bps) >= price_scale ||
        config.max_order_quantity <= 0 || config.bar_liquidity <= 0 || config.bar_duration_ns <= 0)
        throw std::invalid_argument("invalid execution configuration");
}
inline Money fee(const ExecutionConfig& config, Quantity quantity) {
    return checked_add(checked_mul(checked_add(config.commission_per_unit, config.exchange_fee_per_unit), quantity), config.fixed_fee);
}
inline Price execution_price(Price price, Side side, const ExecutionConfig& config) {
    const auto bps = checked_add(config.slippage_bps, config.impact_bps);
    // Quotient/remainder decomposition avoids overflowing price*bps.
    const auto whole = checked_mul(price / price_scale, bps);
    const auto remainder = checked_mul(price % price_scale, bps);
    auto adjustment = checked_add(whole, remainder / price_scale);
    if (remainder % price_scale) adjustment = checked_add(adjustment, 1);
    return checked_add(price, side == Side::Buy ? adjustment : checked_negate(adjustment));
}
inline void mark_event(Portfolio& portfolio, const Event& event) {
    const auto id = event.header.instrument;
    if (const auto* p = std::get_if<Trade>(&event.payload)) portfolio.mark(id, p->price);
    if (const auto* p = std::get_if<Bar>(&event.payload)) portfolio.mark(id, p->close);
    if (const auto* p = std::get_if<Quote>(&event.payload)) portfolio.mark(id, checked_add(p->bid, (p->ask - p->bid) / 2));
}
} // namespace execution_detail

// Input must already be ordered by (availability,event timestamp,sequence).
// Every run owns its ledger; Strategy instances are stateful and must be fresh
// (or explicitly reset by their caller) for repeated experiments.
inline BacktestResult run_backtest(const std::vector<Event>& events, Strategy& strategy,
                                  Money initial_cash = 1'000'000 * price_scale,
                                  const ExecutionConfig& config = {}) {
    execution_detail::validate_config(config);
    BacktestResult result{Portfolio(initial_cash), {}, {}, 0};
    std::vector<execution_detail::Pending> pending;
    std::set<std::uint64_t> event_ids, order_ids;
    std::map<InstrumentId, Timestamp> last_market_time;
    std::map<InstrumentId, std::pair<Timestamp, std::uint64_t>> economic_watermarks;
    InstrumentId bar_instrument = 0;
    Timestamp first_bar_time = 0;
    Timestamp previous_bar_time = 0;
    std::tuple<Timestamp, Timestamp, std::uint64_t> previous{};
    bool first = true;
    // Validate the entire input before invoking user callbacks or changing state.
    for (const auto& event : events) {
        validate_event(event);
        const auto key = std::tuple{event.header.available_time, event.header.event_time, event.header.sequence};
        if ((!first && key <= previous) || !event_ids.insert(event.header.sequence).second)
            throw std::invalid_argument("backtest input must have unique sequences and availability ordering");
        const auto economic_key = std::pair{event.header.event_time, event.header.sequence};
        if (const auto last = economic_watermarks.find(event.header.instrument);
            last != economic_watermarks.end() && economic_key <= last->second)
            throw std::invalid_argument("backtest rejects per-instrument economic-time/sequence regressions; generic replay supports late data");
        economic_watermarks.insert_or_assign(event.header.instrument, economic_key);
        if (config.model == ExecutionModel::Bar) {
            if (!std::holds_alternative<Bar>(event.payload))
                throw std::invalid_argument("bar backtests require pure Bar input; use tick execution for corporate actions");
            if (event.header.event_time != event.header.receive_time || event.header.event_time != event.header.available_time)
                throw std::invalid_argument("bar backtests require immediate end-of-interval availability");
            if (event.header.event_time < config.bar_duration_ns)
                throw std::invalid_argument("bar interval begins before supported Unix epoch");
            if (!bar_instrument) {
                bar_instrument = event.header.instrument;
                first_bar_time = event.header.event_time;
            } else if (event.header.instrument != bar_instrument) {
                throw std::invalid_argument("bar backtests support one instrument; use tick execution for multiple instruments");
            } else if ((event.header.event_time - first_bar_time) % config.bar_duration_ns != 0 ||
                       event.header.event_time - previous_bar_time < config.bar_duration_ns) {
                throw std::invalid_argument("bars must use distinct, nonoverlapping intervals on one fixed-duration grid");
            }
            previous_bar_time = event.header.event_time;
        }
        const bool corporate_action = std::holds_alternative<Dividend>(event.payload) || std::holds_alternative<Split>(event.payload);
        if (corporate_action) {
            if (event.header.available_time != event.header.event_time)
                throw std::invalid_argument("delayed corporate actions require historical entitlement accounting");
            const auto last = last_market_time.find(event.header.instrument);
            if (last != last_market_time.end() && last->second >= event.header.event_time)
                throw std::invalid_argument("corporate actions must precede same-time instrument market events");
        } else {
            auto [last, inserted] = last_market_time.try_emplace(event.header.instrument, event.header.event_time);
            if (!inserted) last->second = std::max(last->second, event.header.event_time);
        }
        first = false;
        previous = key;
    }
    const auto report = [&](const ExecutionReport& execution) {
        result.reports.push_back(execution);
        strategy.on_execution(execution, result.portfolio);
    };
    for (const auto& event : events) {
        const auto time = event.header.available_time;
        const auto instrument = event.header.instrument;
        if (const auto* dividend = std::get_if<Dividend>(&event.payload)) result.portfolio.dividend(instrument, dividend->amount_per_share);
        if (const auto* split = std::get_if<Split>(&event.payload)) {
            result.portfolio.split(instrument, split->numerator, split->denominator);
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->request.instrument == instrument) {
                    report({it->request.id, instrument, time, it->request.side, 0, 0, 0, ExecutionStatus::Cancelled});
                    it = pending.erase(it);
                } else ++it;
            }
        }
        Quantity buy_liquidity = 0, sell_liquidity = 0, bar_remaining = 0;
        const auto* bar = config.model == ExecutionModel::Bar ? std::get_if<Bar>(&event.payload) : nullptr;
        const auto* quote = config.model == ExecutionModel::Tick ? std::get_if<Quote>(&event.payload) : nullptr;
        if (bar) bar_remaining = config.bar_liquidity;
        if (quote) { buy_liquidity = quote->ask_size; sell_liquidity = quote->bid_size; }
        for (auto it = pending.begin(); it != pending.end();) {
            const auto& order = it->request;
            // Later availability alone is insufficient: a late historical tick
            // cannot fill an order issued after that tick's exchange timestamp.
            if (order.instrument != instrument || event.header.event_time <= it->accepted_at || time <= it->accepted_at || (!bar && !quote)) {
                ++it;
                continue;
            }
            // Bars are timestamped at interval end. An order created after the
            // interval opened must wait; knowing the later close cannot move it
            // back to that open. Gaps are represented by the configured duration.
            if (bar && (event.header.event_time < config.bar_duration_ns ||
                        event.header.event_time - config.bar_duration_ns < it->accepted_at)) {
                ++it;
                continue;
            }
            auto& liquidity = bar ? bar_remaining : (order.side == Side::Buy ? buy_liquidity : sell_liquidity);
            if (liquidity <= 0) { ++it; continue; }
            const auto raw_price = bar ? bar->open : (order.side == Side::Buy ? quote->ask : quote->bid);
            const auto price = execution_detail::execution_price(raw_price, order.side, config);
            if (price <= 0) throw std::overflow_error("execution price is nonpositive");
            if (order.type == OrderType::Limit && ((order.side == Side::Buy && price > order.limit_price) ||
                                                   (order.side == Side::Sell && price < order.limit_price))) {
                ++it;
                continue;
            }
            auto quantity = std::min(it->remaining, liquidity);
            if (order.side == Side::Sell && !config.allow_short) quantity = std::min(quantity, std::max<Quantity>(0, result.portfolio.quantity(instrument)));
            auto fees = quantity ? execution_detail::fee(config, quantity) : 0;
            bool rejected = !quantity;
            if (quantity) {
                const auto notional = checked_mul(price, quantity);
                const auto cash_after = checked_add(checked_add(result.portfolio.available_cash(),
                    order.side == Side::Buy ? checked_negate(notional) : notional), checked_negate(fees));
                rejected = cash_after < 0;
            }
            if (rejected) {
                report({order.id, instrument, time, order.side, 0, 0, 0, ExecutionStatus::Rejected});
                it = pending.erase(it);
                continue;
            }
            const auto status = quantity == it->remaining ? ExecutionStatus::Filled : ExecutionStatus::Partial;
            const ExecutionReport fill{order.id, instrument, time, order.side, quantity, price, fees, status};
            result.portfolio.apply_fill(fill);
            liquidity -= quantity;
            it->remaining -= quantity;
            report(fill);
            if (!it->remaining) it = pending.erase(it); else ++it;
        }
        execution_detail::mark_event(result.portfolio, event);
        for (const auto cancel_id : strategy.cancel_orders(event, result.portfolio)) {
            const auto it = std::find_if(pending.begin(), pending.end(), [cancel_id](const auto& p) { return p.request.id == cancel_id; });
            if (it != pending.end()) {
                report({it->request.id, it->request.instrument, time, it->request.side, 0, 0, 0, ExecutionStatus::Cancelled});
                pending.erase(it);
            }
        }
        for (const auto& order : strategy.on_event(event, result.portfolio)) {
            if (!order.id || !order.instrument || order.quantity <= 0 || order.quantity > config.max_order_quantity ||
                (order.side != Side::Buy && order.side != Side::Sell) ||
                (order.type != OrderType::Market && order.type != OrderType::Limit) ||
                (order.type == OrderType::Limit && order.limit_price <= 0) || !order_ids.insert(order.id).second) {
                report({order.id, order.instrument, time, order.side, 0, 0, 0, ExecutionStatus::Rejected});
                continue;
            }
            pending.push_back({order, order.quantity, time});
        }
        if (!result.portfolio.invariant()) throw std::logic_error("portfolio ledger invariant failed");
        result.equity_curve.push_back({time, result.portfolio.equity(), result.portfolio.cash(), result.portfolio.gross_exposure(), result.portfolio.net_exposure()});
    }
    result.pending_orders = pending.size();
    return result;
}
} // namespace chronos
