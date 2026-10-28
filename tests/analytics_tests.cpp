#include <chronos/analytics/statistics.hpp>
#include <chronos/core/types.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <random>

using namespace chronos::analytics;

TEST(Returns, HandCalculated) {
    auto r = simple_returns({100, 110, 99});
    ASSERT_EQ(r.size(), 2U);
    EXPECT_NEAR(r[0], .1, 1e-15);
    EXPECT_NEAR(r[1], -.1, 1e-15);
    EXPECT_NEAR(cumulative_return(r), -.01, 1e-15);
    auto l = log_returns({100, 110, 99});
    EXPECT_NEAR(l[0] + l[1], std::log(.99), 1e-15);
}
TEST(Returns, InvalidAndExtreme) {
    EXPECT_THROW(simple_returns({1}), std::invalid_argument);
    EXPECT_THROW(simple_returns({1, 0}), std::invalid_argument);
    EXPECT_THROW(log_returns({1, -1}), std::invalid_argument);
    EXPECT_THROW(cumulative_return({-1.01}), std::invalid_argument);
    EXPECT_DOUBLE_EQ(cumulative_return({-.2, -1, .2}), -1);
    EXPECT_THROW(simple_returns({1, std::numeric_limits<double>::infinity()}), std::invalid_argument);
    EXPECT_THROW(simple_returns({std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max()}), std::overflow_error);
}
TEST(Metrics, HandCalculated) {
    const std::vector<double> r{.1, -.1, .1, -.1};
    EXPECT_NEAR(annualized_volatility(r, 12), .4, 1e-15);
    EXPECT_NEAR(sharpe_ratio(r), 0, 1e-15);
    EXPECT_NEAR(sortino_ratio(r), 0, 1e-15);
    EXPECT_NEAR(maximum_drawdown({100, 120, 90, 110, 80}), 1.0 / 3, 1e-15);
    EXPECT_DOUBLE_EQ(maximum_drawdown({100, 0}), 1);
}
TEST(Metrics, DenominatorsAndMissingDataAreExplicit) {
    EXPECT_DOUBLE_EQ(annualized_volatility({.1, .1}), 0);
    EXPECT_THROW(sharpe_ratio({.1, .1}), std::domain_error);
    EXPECT_THROW(sortino_ratio({.1, .2}), std::domain_error);
    EXPECT_THROW(sharpe_ratio({.1, .2}, 252, -1), std::invalid_argument);
    EXPECT_THROW(annualized_volatility({.1, .2}, 0), std::invalid_argument);
    EXPECT_THROW(annualized_volatility({.1, std::numeric_limits<double>::quiet_NaN()}), std::invalid_argument);
    EXPECT_THROW(maximum_drawdown({0, 1}), std::invalid_argument);
    EXPECT_THROW(maximum_drawdown({1, -1}), std::invalid_argument);
}
TEST(Covariance, SingularMatrixAndBeta) {
    Matrix x{{1, 2}, {2, 4}, {3, 6}};
    auto c = covariance(x);
    EXPECT_DOUBLE_EQ(c[0][0], 1);
    EXPECT_DOUBLE_EQ(c[0][1], 2);
    EXPECT_DOUBLE_EQ(c[1][1], 4);
    auto corr = correlation(x);
    EXPECT_DOUBLE_EQ(corr[0][1], 1);
    EXPECT_DOUBLE_EQ(portfolio_volatility({.5, .5}, c, 1), 1.5);
    EXPECT_DOUBLE_EQ(portfolio_volatility({2, -1}, c, 252), 0);
    EXPECT_DOUBLE_EQ(beta({2, 4, 6}, {1, 2, 3}), 2);
}
TEST(Covariance, RejectsInvalidMatrices) {
    EXPECT_THROW(covariance({{1, 2}}), std::invalid_argument);
    EXPECT_THROW(covariance({{1}, {1, 2}}), std::invalid_argument);
    EXPECT_THROW(correlation({{1, 1}, {2, 1}}), std::domain_error);
    EXPECT_THROW(portfolio_volatility({.5, .5}, {{1, 2}, {2, 1}}), std::invalid_argument);
    EXPECT_THROW(portfolio_volatility({.5, .5}, {{1, 2}, {1, 4}}), std::invalid_argument);
    EXPECT_THROW(portfolio_volatility({1}, {{-1}}), std::invalid_argument);
    EXPECT_THROW(beta({1, 2}, {1, 1}), std::domain_error);
}
TEST(Tails, ExactEmpiricalTailMass) {
    std::vector<double> r{-.1, -.2, .1, .2};
    EXPECT_DOUBLE_EQ(historical_var(r, .5), -.1);
    EXPECT_DOUBLE_EQ(historical_var(r, .75), .1);
    EXPECT_NEAR(historical_expected_shortfall(r, .625), (.2 + .5 * .1) / 1.5, 1e-15);
    EXPECT_DOUBLE_EQ(historical_expected_shortfall(r, .95), .2);
    EXPECT_THROW(historical_var(r, 1), std::invalid_argument);
    EXPECT_THROW(historical_var({}, .95), std::invalid_argument);
}
TEST(Risk, Concentration) {
    EXPECT_DOUBLE_EQ(concentration({.5, -.5}), .5);
    EXPECT_DOUBLE_EQ(concentration({0, 0}), 0);
    EXPECT_DOUBLE_EQ(concentration({100, 0}), 1);
}
TEST(Arithmetic, FixedPointBoundaries) {
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    EXPECT_EQ(chronos::checked_mul(lo, 1), lo);
    EXPECT_EQ(chronos::checked_mul(lo, 0), 0);
    EXPECT_EQ(chronos::checked_mul(hi, -1), -hi);
    EXPECT_EQ(chronos::checked_mul(-3, -4), 12);
    EXPECT_EQ(chronos::checked_add(lo, hi), -1);
    EXPECT_THROW(chronos::checked_mul(lo, -1), std::overflow_error);
    EXPECT_THROW(chronos::checked_mul(-1, lo), std::overflow_error);
    EXPECT_THROW(chronos::checked_mul(hi, 2), std::overflow_error);
    EXPECT_THROW(chronos::checked_mul(lo, 2), std::overflow_error);
    EXPECT_THROW(chronos::checked_mul(lo / 2 - 1, -2), std::overflow_error);
    EXPECT_THROW(chronos::checked_add(lo, -1), std::overflow_error);
    EXPECT_THROW(chronos::checked_add(hi, 1), std::overflow_error);
}
#if defined(__SIZEOF_INT128__)
TEST(Arithmetic, MultiplicationAgainstWideIntegerOracle) {
    __extension__ typedef __int128 OracleInteger;
    std::mt19937_64 rng(20261028);
    constexpr auto lo = std::numeric_limits<std::int64_t>::min();
    constexpr auto hi = std::numeric_limits<std::int64_t>::max();
    for (unsigned i = 0; i < 20000; ++i) {
        const auto a = static_cast<std::int64_t>(rng());
        auto b = static_cast<std::int64_t>(rng());
        // Exercise valid products as well as the overwhelmingly overflowing full-width case.
        if (i % 3 == 0) b %= 7;
        const auto oracle = static_cast<OracleInteger>(a) * b;
        if (oracle < lo || oracle > hi) {
            EXPECT_THROW(chronos::checked_mul(a, b), std::overflow_error);
        } else {
            EXPECT_EQ(chronos::checked_mul(a, b), static_cast<std::int64_t>(oracle));
        }
    }
}
#endif
TEST(Metrics, StableLargeOffsetsAndSmallCovariance) {
    const auto c = covariance({{1e12 + 1, 1e12 + 2}, {1e12 + 2, 1e12 + 4}, {1e12 + 3, 1e12 + 6}});
    EXPECT_DOUBLE_EQ(c[0][0], 1);
    EXPECT_DOUBLE_EQ(c[0][1], 2);
    EXPECT_DOUBLE_EQ(c[1][1], 4);
    EXPECT_NEAR(portfolio_volatility({1}, {{1e-200}}, 1), 1e-100, 1e-114);
    EXPECT_DOUBLE_EQ(portfolio_volatility({1, -1}, {{0, 0}, {0, 0}}, 1), 0);
    EXPECT_THROW(portfolio_volatility({1, 0}, {{0, 1}, {1, 0}}, 1), std::invalid_argument);
}
TEST(Metrics, LongConstantSamplesRemainExactlyConstant) {
    for (const auto count : {10U, 1000U}) {
        const std::vector<double> values(count, .01);
        EXPECT_DOUBLE_EQ(annualized_volatility(values), 0);
        EXPECT_THROW(sharpe_ratio(values), std::domain_error);
        EXPECT_THROW(beta(values, values), std::domain_error);
        const Matrix observations(count, std::vector<double>{.01, .07});
        EXPECT_EQ(covariance(observations), (Matrix{{0, 0}, {0, 0}}));
        EXPECT_THROW(correlation(observations), std::domain_error);
    }
}
