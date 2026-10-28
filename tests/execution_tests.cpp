#include <chronos/execution/backtest.hpp>
#include <chronos/execution/strategies.hpp>
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <random>

namespace {
using namespace chronos;
ExecutionReport fill(std::uint64_t id, Side side, Quantity quantity, Price price, Money fee = 0) {
    return {id, 1, static_cast<Timestamp>(id), side, quantity, price, fee, ExecutionStatus::Filled};
}
Event bar(std::uint64_t seq, Price open, Price close) {
    return {{static_cast<Timestamp>(seq), static_cast<Timestamp>(seq), static_cast<Timestamp>(seq), seq, 1},
            Bar{open, std::max(open, close), std::min(open, close), close, 100}};
}
Event quote(std::uint64_t seq, Price bid, Price ask, Quantity size = 10) {
    return {{static_cast<Timestamp>(seq), static_cast<Timestamp>(seq), static_cast<Timestamp>(seq), seq, 1}, Quote{bid, ask, size, size}};
}
class Plan final : public Strategy {
public:
    std::map<std::uint64_t, std::vector<OrderRequest>> orders;
    std::map<std::uint64_t, std::vector<std::uint64_t>> cancellations;
    std::vector<OrderRequest> on_event(const Event& event, const Portfolio&) override { return orders[event.header.sequence]; }
    std::vector<std::uint64_t> cancel_orders(const Event& event, const Portfolio&) override { return cancellations[event.header.sequence]; }
};
TEST(Portfolio, IndependentlyCalculatedReversalAndFeeLedger) {
    Portfolio p(1000 * price_scale);
    p.apply_fill(fill(1, Side::Buy, 10, 10 * price_scale, price_scale));
    p.apply_fill(fill(2, Side::Buy, 5, 12 * price_scale, 5000));
    EXPECT_EQ(p.cash(), 8'385'000);
    p.apply_fill(fill(3, Side::Sell, 8, 15 * price_scale, 8000));
    EXPECT_EQ(p.positions().at(1).cost_basis, 746'667);
    EXPECT_EQ(p.realized_pnl(), 346'667);
    p.apply_fill(fill(4, Side::Sell, 10, 9 * price_scale, 10'000));
    EXPECT_EQ(p.quantity(1), -3);
    EXPECT_EQ(p.cash(), 10'467'000);
    EXPECT_EQ(p.realized_pnl(), 230'000);
    EXPECT_EQ(p.positions().at(1).cost_basis, 270'000);
    p.apply_fill(fill(5, Side::Buy, 1, 8 * price_scale, 1000));
    EXPECT_EQ(p.cash(), 10'386'000);
    EXPECT_EQ(p.quantity(1), -2);
    EXPECT_EQ(p.realized_pnl(), 240'000);
    EXPECT_EQ(p.unrealized_pnl(), 20'000);
    EXPECT_EQ(p.fees(), 34'000);
    EXPECT_EQ(p.equity(), 10'226'000);
    EXPECT_TRUE(p.invariant());
}
TEST(Portfolio, DividendAndExactSplitPreserveLedgerIdentity) {
    Portfolio p(1000 * price_scale);
    p.apply_fill(fill(1, Side::Buy, 5, 20 * price_scale));
    p.dividend(1, 2 * price_scale);
    EXPECT_EQ(p.income(), 10 * price_scale);
    const auto before = p.equity();
    p.split(1, 2, 1);
    EXPECT_EQ(p.quantity(1), 10);
    EXPECT_EQ(p.positions().at(1).last_price, 10 * price_scale);
    EXPECT_EQ(p.equity(), before);
    EXPECT_THROW(p.split(1, 1, 3), std::invalid_argument);
    EXPECT_EQ(p.quantity(1), 10);
    EXPECT_TRUE(p.invariant());
}
TEST(Portfolio, OverflowAndInvalidFillDoNotMutateLedger) {
    Portfolio p(1000);
    EXPECT_THROW(p.apply_fill(fill(1, Side::Buy, std::numeric_limits<Quantity>::max(), 2)), std::overflow_error);
    EXPECT_EQ(p.cash(), 1000);
    EXPECT_TRUE(p.positions().empty());
    EXPECT_THROW(p.apply_fill(fill(1, Side::Buy, 0, 10)), std::invalid_argument);
    EXPECT_THROW(p.apply_fill(fill(1, static_cast<Side>(3), 1, 10)), std::invalid_argument);
    EXPECT_TRUE(p.invariant());
}
TEST(Portfolio, ExactCostAllocationSupportsLargeNotionalWithoutWideIntegers) {
    const auto maximum = std::numeric_limits<Money>::max();
    EXPECT_EQ(accounting_detail::allocate(maximum, maximum, maximum), maximum);
    EXPECT_EQ(accounting_detail::allocate(maximum, maximum - 1, maximum), maximum - 1);
    EXPECT_EQ(accounting_detail::allocate(10, 2, 3), 6);
}
TEST(Portfolio, RandomTransitionsMatchIndependentFractionalAverageReference) {
    Portfolio p(10'000'000);
    std::mt19937 generator(47);
    long double cash = 10'000'000, reference_basis = 0, reference_realized = 0;
    Quantity position = 0;
    for (std::uint64_t i = 1; i <= 2000; ++i) {
        const Quantity signed_quantity = (generator() % 2 ? 1 : -1) * static_cast<Quantity>(generator() % 10 + 1);
        const Price price = generator() % 10000 + 1;
        const Money fee = generator() % 5;
        const auto absolute = std::abs(signed_quantity);
        const bool increase = !position || (position > 0) == (signed_quantity > 0);
        if (increase) reference_basis += static_cast<long double>(price) * absolute;
        else {
            const auto closed = std::min(std::abs(position), absolute);
            const auto allocation = reference_basis * closed / std::abs(position);
            reference_realized += (position > 0 ? 1 : -1) * (static_cast<long double>(closed) * price - allocation);
            reference_basis -= allocation;
            if (absolute > std::abs(position)) reference_basis = static_cast<long double>(absolute - std::abs(position)) * price;
        }
        position += signed_quantity;
        cash -= static_cast<long double>(signed_quantity) * price + fee;
        p.apply_fill(fill(i, signed_quantity > 0 ? Side::Buy : Side::Sell, absolute, price, fee));
        EXPECT_EQ(p.quantity(1), position);
        EXPECT_EQ(p.cash(), cash);
        EXPECT_NEAR(static_cast<long double>(p.realized_pnl()), reference_realized, static_cast<long double>(i));
        EXPECT_TRUE(p.invariant());
    }
}
TEST(Execution, BarOrdersUseOnlyLaterOpenAndSharedAssumedLiquidity) {
    ExecutionConfig config;
    config.bar_liquidity = 3;
    config.commission_per_unit = 2;
    config.fixed_fee = 1;
    BuyAndHold strategy(1, 5);
    const auto r = run_backtest({bar(1, 100, 1000), bar(2, 200, 250), bar(3, 300, 350)}, strategy, 10'000, config);
    ASSERT_EQ(r.reports.size(), 2U);
    EXPECT_EQ(r.reports[0].time, 2);
    EXPECT_EQ(r.reports[0].price, 200);
    EXPECT_EQ(r.reports[0].quantity, 3);
    EXPECT_EQ(r.reports[0].fees, 7);
    EXPECT_EQ(r.reports[0].status, ExecutionStatus::Partial);
    EXPECT_EQ(r.reports[1].quantity, 2);
    EXPECT_EQ(r.reports[1].price, 300);
    EXPECT_EQ(r.portfolio.cash(), 8788);
    EXPECT_EQ(r.portfolio.equity(), 10'538);
    EXPECT_EQ(r.pending_orders, 0U);
}
TEST(Execution, LimitCannotFillFromBarHighLowOrAdverseSlipBeyondLimit) {
    Plan plan;
    plan.orders[1] = {{1, 1, Side::Buy, OrderType::Limit, 1, 90}};
    auto second = bar(2, 100, 100);
    std::get<Bar>(second.payload).low = 50;
    auto r = run_backtest({bar(1, 100, 100), second}, plan, 1000);
    EXPECT_TRUE(r.reports.empty());
    EXPECT_EQ(r.pending_orders, 1U);
    Plan slipped;
    slipped.orders[1] = {{1, 1, Side::Buy, OrderType::Limit, 1, 100}};
    ExecutionConfig config;
    config.slippage_bps = 1;
    r = run_backtest({bar(1, 100, 100), bar(2, 100, 100)}, slipped, 1000, config);
    EXPECT_TRUE(r.reports.empty());
}
TEST(Execution, QuoteSpreadAndPartialLiquidityAreSharedFifo) {
    Plan plan;
    plan.orders[1] = {{1, 1, Side::Buy, OrderType::Market, 4, 0}, {2, 1, Side::Buy, OrderType::Market, 4, 0}};
    ExecutionConfig config;
    config.model = ExecutionModel::Tick;
    const auto r = run_backtest({quote(1, 90, 110), quote(2, 95, 115, 5), quote(3, 95, 115, 5)}, plan, 10000, config);
    ASSERT_EQ(r.reports.size(), 3U);
    EXPECT_EQ(r.reports[0].quantity, 4);
    EXPECT_EQ(r.reports[1].quantity, 1);
    EXPECT_EQ(r.reports[2].quantity, 3);
    EXPECT_EQ(r.portfolio.quantity(1), 8);
    EXPECT_EQ(r.portfolio.cash(), 9080);
    EXPECT_EQ(r.portfolio.unrealized_pnl(), -80); // mark midpoint=105, execution ask=115
}
TEST(Execution, RejectsInsufficientCashAndUncoveredSell) {
    Plan plan;
    plan.orders[1] = {{1, 1, Side::Buy, OrderType::Market, 20, 0}, {2, 1, Side::Sell, OrderType::Market, 1, 0}};
    const auto r = run_backtest({bar(1, 100, 100), bar(2, 100, 100)}, plan, 1000);
    ASSERT_EQ(r.reports.size(), 2U);
    EXPECT_EQ(r.reports[0].status, ExecutionStatus::Rejected);
    EXPECT_EQ(r.reports[1].status, ExecutionStatus::Rejected);
    EXPECT_EQ(r.portfolio.cash(), 1000);
}
TEST(Execution, CashRiskAlsoCoversFeesOnSell) {
    Plan plan;
    plan.orders[1] = {{1, 1, Side::Buy, OrderType::Market, 8, 0}};
    plan.orders[2] = {{2, 1, Side::Sell, OrderType::Market, 1, 0}};
    ExecutionConfig config;
    config.fixed_fee = 100;
    const auto r = run_backtest({bar(1, 100, 100), bar(2, 100, 100), bar(3, 1, 1)}, plan, 901, config);
    ASSERT_EQ(r.reports.size(), 2U);
    EXPECT_EQ(r.reports[0].status, ExecutionStatus::Filled);
    EXPECT_EQ(r.reports[1].status, ExecutionStatus::Rejected);
    EXPECT_EQ(r.portfolio.cash(), 1);
    EXPECT_EQ(r.portfolio.quantity(1), 8);
}
TEST(Execution, CancelAfterObservationAndCancelOnSplit) {
    Plan plan;
    plan.orders[1] = {{1, 1, Side::Buy, OrderType::Limit, 1, 50}};
    plan.cancellations[2] = {1};
    auto r = run_backtest({bar(1, 100, 100), bar(2, 100, 100), bar(3, 40, 40)}, plan, 1000);
    ASSERT_EQ(r.reports.size(), 1U);
    EXPECT_EQ(r.reports[0].status, ExecutionStatus::Cancelled);
    Plan split_plan;
    split_plan.orders[1] = {{1, 1, Side::Buy, OrderType::Market, 1, 0}};
    Event split{{2, 2, 2, 2, 1}, Split{2, 1}};
    r = run_backtest({bar(1, 100, 100), split, bar(3, 50, 50)}, split_plan, 1000);
    ASSERT_EQ(r.reports.size(), 1U);
    EXPECT_EQ(r.reports[0].status, ExecutionStatus::Cancelled);
}
TEST(Execution, RejectsDelayedCorporateActionsBeforeStrategyCallbacks) {
    class CountingStrategy final : public Strategy {
    public:
        int calls{};
        std::vector<OrderRequest> on_event(const Event&, const Portfolio&) override { ++calls; return {}; }
    } strategy;
    const Event delayed_dividend{{1, 3, 3, 3, 1}, Dividend{10}};
    const Event delayed_split{{1, 3, 3, 3, 1}, Split{2, 1}};
    EXPECT_THROW(run_backtest({bar(1, 100, 100), bar(2, 100, 100), delayed_dividend}, strategy, 1000), std::invalid_argument);
    EXPECT_EQ(strategy.calls, 0);
    EXPECT_THROW(run_backtest({bar(1, 100, 100), bar(2, 100, 100), delayed_split}, strategy, 1000), std::invalid_argument);
    EXPECT_EQ(strategy.calls, 0);
}
TEST(Execution, CorporateActionsPrecedeSameInstrumentMarketEventsAtEffectiveTime) {
    auto market = bar(1, 100, 100);
    market.header.event_time = market.header.receive_time = market.header.available_time = 2;
    const Event dividend{{2, 2, 2, 2, 1}, Dividend{10}};
    BuyAndHold invalid;
    EXPECT_THROW(run_backtest({market, dividend}, invalid, 1000), std::invalid_argument);
    // Explicit dataset sequence provides action priority without changing the
    // shared replay tie-break convention.
    const Event first_dividend{{2, 2, 2, 1, 1}, Dividend{10}};
    market.header.sequence = 2;
    BuyAndHold valid;
    EXPECT_NO_THROW(run_backtest({first_dividend, market}, valid, 1000));
    // Equal times for a different instrument do not constrain action ordering.
    market.header.sequence = 1;
    market.header.instrument = 2;
    BuyAndHold other_instrument;
    EXPECT_NO_THROW(run_backtest({market, dividend}, other_instrument, 1000));
}
TEST(Execution, NoFillOnLateTickOrTradeAndStrictInputValidation) {
    ExecutionConfig config;
    config.model = ExecutionModel::Tick;
    BuyAndHold strategy(1, 2);
    auto late = quote(2, 90, 110);
    late.header.event_time = 0;
    Event trade{{3, 3, 3, 3, 1}, Trade{100, 1000}};
    const auto r = run_backtest({quote(1, 90, 110), late, trade, quote(4, 90, 110)}, strategy, 1000, config);
    ASSERT_EQ(r.reports.size(), 1U);
    EXPECT_EQ(r.reports[0].time, 4);
    BuyAndHold invalid;
    EXPECT_THROW(run_backtest({bar(2, 100, 100), bar(1, 100, 100)}, invalid), std::invalid_argument);
    EXPECT_THROW(run_backtest({bar(1, 100, 100), bar(1, 100, 100)}, invalid), std::invalid_argument);
}
TEST(Execution, DelayedBarSignalCannotFillAnOpenThatAlreadyHappened) {
    ExecutionConfig config;
    config.bar_duration_ns = 2;
    auto first = bar(1, 100, 100);
    first.header.event_time = 2;
    first.header.receive_time = 3;
    first.header.available_time = 3;
    auto second = bar(2, 110, 110);
    second.header.event_time = second.header.receive_time = second.header.available_time = 4;
    auto third = bar(3, 120, 120);
    third.header.event_time = third.header.receive_time = third.header.available_time = 6;
    BuyAndHold strategy(1, 1);
    const auto r = run_backtest({first, second, third}, strategy, 1000, config);
    ASSERT_EQ(r.reports.size(), 1U);
    EXPECT_EQ(r.reports[0].time, 6);
    EXPECT_EQ(r.reports[0].price, 120);
}
TEST(Execution, IdenticalInputAndFreshStrategiesProduceIdenticalLedger) {
    const std::vector<Event> events{bar(1, 100, 110), bar(2, 120, 130), bar(3, 140, 150)};
    BuyAndHold a(1, 3), b(1, 3);
    const auto ra = run_backtest(events, a, 1000);
    const auto rb = run_backtest(events, b, 1000);
    EXPECT_EQ(ra.portfolio.cash(), rb.portfolio.cash());
    EXPECT_EQ(ra.portfolio.equity(), rb.portfolio.equity());
    EXPECT_EQ(ra.portfolio.positions().at(1).cost_basis, rb.portfolio.positions().at(1).cost_basis);
    ASSERT_EQ(ra.reports.size(), rb.reports.size());
    EXPECT_EQ(ra.reports[0].price, rb.reports[0].price);
    EXPECT_EQ(ra.reports[0].quantity, rb.reports[0].quantity);
    ASSERT_EQ(ra.equity_curve.size(), rb.equity_curve.size());
    for (std::size_t i = 0; i < ra.equity_curve.size(); ++i) EXPECT_EQ(ra.equity_curve[i].equity, rb.equity_curve[i].equity);
}
TEST(Strategies, MovingAverageObservesTrailingClosesAndTradesLaterOpens) {
    MovingAverageCross strategy(1, 2, 2, 3);
    const std::vector<Event> events{bar(1, 100, 100), bar(2, 110, 110), bar(3, 120, 120),
                                    bar(4, 130, 80), bar(5, 90, 70), bar(6, 70, 60)};
    const auto r = run_backtest(events, strategy, 1000);
    ASSERT_EQ(r.reports.size(), 2U);
    EXPECT_EQ(r.reports[0].time, 4);
    EXPECT_EQ(r.reports[0].side, Side::Buy);
    EXPECT_EQ(r.reports[0].price, 130);
    EXPECT_EQ(r.reports[1].time, 5);
    EXPECT_EQ(r.reports[1].side, Side::Sell);
    EXPECT_EQ(r.reports[1].price, 90);
    EXPECT_EQ(r.portfolio.quantity(1), 0);
    EXPECT_EQ(r.portfolio.cash(), 920);
    EXPECT_EQ(r.portfolio.realized_pnl(), -80);
    EXPECT_THROW(MovingAverageCross(1, 2, 3, 3), std::invalid_argument);
}
TEST(Strategies, MovingAverageResetsOnSplit) {
    MovingAverageCross strategy(1, 2, 2, 3);
    const Event split{{3, 3, 3, 3, 1}, Split{2, 1}};
    const auto r = run_backtest({bar(1, 100, 100), bar(2, 110, 110), split, bar(4, 60, 60)}, strategy, 1000);
    EXPECT_TRUE(r.reports.empty());
    EXPECT_EQ(r.pending_orders, 0U);
}
} // namespace
