#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace chronos::analytics {
using Matrix = std::vector<std::vector<double>>;

namespace detail {
inline void finite(double x) {
    if (!std::isfinite(x)) throw std::invalid_argument("non-finite numerical input");
}
inline void sample(const std::vector<double>& x, std::size_t minimum = 2) {
    if (x.size() < minimum) throw std::invalid_argument("insufficient observations");
    for (auto v : x) finite(v);
}
inline double output(long double x) {
    const auto result = static_cast<double>(x);
    if (!std::isfinite(result)) throw std::overflow_error("non-finite numerical result");
    return result;
}
inline long double mean(const std::vector<double>& x) {
    // Shift before summing: an exactly constant input must have exactly its
    // constant mean, even when n*x[0] is not exactly representable.
    const long double origin = x.front();
    long double sum = 0;
    for (auto v : x) sum += static_cast<long double>(v) - origin;
    return origin + sum / static_cast<long double>(x.size());
}
inline long double variance(const std::vector<double>& x) {
    const auto m = mean(x);
    long double sum = 0;
    for (auto v : x) { const auto d = static_cast<long double>(v) - m; sum += d * d; }
    return sum / static_cast<long double>(x.size() - 1);
}
inline void periods(double x) {
    finite(x);
    if (x <= 0) throw std::invalid_argument("periods per year must be positive");
}
inline double periodic_rate(double annual, double periods_per_year) {
    finite(annual);
    periods(periods_per_year);
    if (annual <= -1) throw std::invalid_argument("annual rate must exceed -1");
    return std::expm1(std::log1p(annual) / periods_per_year);
}
inline std::vector<double> losses(const std::vector<double>& returns, double confidence) {
    sample(returns, 1);
    finite(confidence);
    if (confidence <= 0 || confidence >= 1) throw std::invalid_argument("confidence must be between zero and one");
    std::vector<double> result;
    result.reserve(returns.size());
    for (auto r : returns) result.push_back(-r);
    std::sort(result.begin(), result.end());
    return result;
}
} // namespace detail

inline std::vector<double> simple_returns(const std::vector<double>& prices) {
    detail::sample(prices);
    for (auto p : prices) if (p <= 0) throw std::invalid_argument("prices must be positive");
    std::vector<double> result;
    result.reserve(prices.size() - 1);
    for (std::size_t i = 1; i < prices.size(); ++i)
        result.push_back(detail::output(static_cast<long double>(prices[i]) / prices[i - 1] - 1));
    return result;
}

inline std::vector<double> log_returns(const std::vector<double>& prices) {
    detail::sample(prices);
    for (auto p : prices) if (p <= 0) throw std::invalid_argument("prices must be positive");
    std::vector<double> result;
    result.reserve(prices.size() - 1);
    for (std::size_t i = 1; i < prices.size(); ++i)
        result.push_back(detail::output(std::log(static_cast<long double>(prices[i])) - std::log(static_cast<long double>(prices[i - 1]))));
    return result;
}

inline double cumulative_return(const std::vector<double>& returns) {
    detail::sample(returns, 1);
    long double growth = 1;
    for (auto r : returns) {
        if (r < -1) throw std::invalid_argument("simple return below -100 percent");
        growth *= 1 + static_cast<long double>(r);
        if (!std::isfinite(growth)) throw std::overflow_error("cumulative return overflow");
    }
    return detail::output(growth - 1);
}

inline double annualized_volatility(const std::vector<double>& returns, double periods_per_year = 252) {
    detail::sample(returns);
    detail::periods(periods_per_year);
    return detail::output(std::sqrt(detail::variance(returns) * periods_per_year));
}

inline double sharpe_ratio(const std::vector<double>& returns, double periods_per_year = 252, double annual_risk_free = 0) {
    detail::sample(returns);
    const auto rf = detail::periodic_rate(annual_risk_free, periods_per_year);
    const auto variance = detail::variance(returns);
    if (variance == 0) throw std::domain_error("Sharpe ratio undefined for zero variance");
    return detail::output((detail::mean(returns) - rf) / std::sqrt(variance) * std::sqrt(periods_per_year));
}

inline double sortino_ratio(const std::vector<double>& returns, double periods_per_year = 252, double annual_target = 0) {
    detail::sample(returns);
    const auto target = detail::periodic_rate(annual_target, periods_per_year);
    long double downside = 0;
    for (auto r : returns) { const auto d = std::min(static_cast<long double>(r) - target, 0.0L); downside += d * d; }
    downside /= static_cast<long double>(returns.size());
    if (downside == 0) throw std::domain_error("Sortino ratio undefined without downside observations");
    return detail::output((detail::mean(returns) - target) / std::sqrt(downside) * std::sqrt(periods_per_year));
}

inline double maximum_drawdown(const std::vector<double>& equity) {
    detail::sample(equity, 1);
    if (equity.front() <= 0) throw std::invalid_argument("initial equity must be positive");
    long double peak = equity.front(), worst = 0;
    for (auto e : equity) {
        if (e < 0) throw std::invalid_argument("drawdown equity must be nonnegative");
        peak = std::max(peak, static_cast<long double>(e));
        worst = std::max(worst, 1 - static_cast<long double>(e) / peak);
    }
    return detail::output(worst);
}

inline Matrix covariance(const Matrix& observations) {
    if (observations.size() < 2 || observations.front().empty()) throw std::invalid_argument("covariance needs two rows and one column");
    const auto d = observations.front().size();
    std::vector<long double> means(d);
    for (const auto& row : observations) {
        if (row.size() != d) throw std::invalid_argument("ragged observations");
        for (std::size_t j = 0; j < d; ++j) {
            detail::finite(row[j]);
            means[j] += static_cast<long double>(row[j]) - observations.front()[j];
        }
    }
    for (std::size_t j = 0; j < d; ++j)
        means[j] = observations.front()[j] + means[j] / static_cast<long double>(observations.size());
    Matrix result(d, std::vector<double>(d));
    for (std::size_t j = 0; j < d; ++j) {
        for (std::size_t k = 0; k <= j; ++k) {
            long double sum = 0;
            for (const auto& row : observations) sum += (row[j] - means[j]) * (row[k] - means[k]);
            result[j][k] = result[k][j] = detail::output(sum / static_cast<long double>(observations.size() - 1));
        }
    }
    return result;
}

inline Matrix correlation(const Matrix& observations) {
    auto result = covariance(observations);
    std::vector<double> sd(result.size());
    for (std::size_t j = 0; j < result.size(); ++j) {
        if (result[j][j] == 0) throw std::domain_error("correlation undefined for constant column");
        sd[j] = std::sqrt(result[j][j]);
    }
    for (std::size_t j = 0; j < result.size(); ++j)
        for (std::size_t k = 0; k < result.size(); ++k)
            result[j][k] = std::clamp(detail::output(static_cast<long double>(result[j][k]) / sd[j] / sd[k]), -1.0, 1.0);
    return result;
}

inline double portfolio_volatility(const std::vector<double>& weights, const Matrix& covariance_matrix, double periods_per_year = 252) {
    detail::sample(weights, 1);
    detail::periods(periods_per_year);
    const auto d = weights.size();
    if (covariance_matrix.size() != d) throw std::invalid_argument("covariance dimension mismatch");
    long double scale = 0;
    for (const auto& row : covariance_matrix) {
        if (row.size() != d) throw std::invalid_argument("covariance must be square");
        for (auto v : row) { detail::finite(v); scale = std::max(scale, std::abs(static_cast<long double>(v))); }
    }
    if (scale == 0) return 0;
    const auto tolerance = std::max(
        128 * std::numeric_limits<double>::epsilon() * scale * static_cast<long double>(d),
        std::numeric_limits<long double>::denorm_min());
    for (std::size_t i = 0; i < d; ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (std::abs(static_cast<long double>(covariance_matrix[i][j]) - covariance_matrix[j][i]) > tolerance)
                throw std::invalid_argument("covariance must be symmetric");
    // Validate C + tolerance*I, using the largest remaining diagonal as pivot.
    // The validation-only shift bounds the admitted negative eigenvalue error
    // and prevents rank-deficient roundoff from becoming a tiny divisor. The
    // original matrix below still determines the reported portfolio variance.
    std::vector<std::vector<long double>> residual(d, std::vector<long double>(d));
    for (std::size_t i = 0; i < d; ++i) {
        for (std::size_t j = 0; j < d; ++j) {
            residual[i][j] = static_cast<long double>(covariance_matrix[i][j]) / 2
                             + static_cast<long double>(covariance_matrix[j][i]) / 2;
        }
        residual[i][i] += tolerance;
    }
    std::vector<long double> column(d);
    for (std::size_t k = 0; k < d; ++k) {
        auto pivot = k;
        for (std::size_t i = k + 1; i < d; ++i)
            if (residual[i][i] > residual[pivot][pivot]) pivot = i;
        if (pivot != k) {
            std::swap(residual[k], residual[pivot]);
            for (auto& row : residual) std::swap(row[k], row[pivot]);
        }
        const auto diagonal = residual[k][k];
        if (!std::isfinite(diagonal) || diagonal <= 0)
            throw std::invalid_argument("covariance is not positive semidefinite within tolerance");
        const auto root = std::sqrt(diagonal);
        for (std::size_t i = k + 1; i < d; ++i) column[i] = residual[i][k] / root;
        for (std::size_t i = k + 1; i < d; ++i) {
            for (std::size_t j = i; j < d; ++j) {
                residual[j][i] -= column[j] * column[i];
                residual[i][j] = residual[j][i];
            }
        }
    }

    // Compensated quadratic form on C, not its validation factor: a genuine
    // small positive variance is retained even below the matrix-wide tolerance.
    long double sum = 0, correction = 0, weight_norm_squared = 0;
    const auto accumulate = [&](long double term) {
        const auto next = sum + term;
        correction += std::abs(sum) >= std::abs(term) ? (sum - next) + term : (term - next) + sum;
        sum = next;
    };
    for (std::size_t i = 0; i < d; ++i) {
        const auto wi = static_cast<long double>(weights[i]);
        weight_norm_squared += wi * wi;
        accumulate(wi * covariance_matrix[i][i] * wi);
        for (std::size_t j = 0; j < i; ++j) {
            const auto symmetric = static_cast<long double>(covariance_matrix[i][j]) / 2
                                   + static_cast<long double>(covariance_matrix[j][i]) / 2;
            accumulate(2 * wi * symmetric * weights[j]);
        }
    }
    auto variance = sum + correction;
    if (!std::isfinite(variance) || !std::isfinite(weight_norm_squared))
        throw std::overflow_error("portfolio quadratic form overflow");
    if (variance < 0) {
        if (-variance > tolerance * weight_norm_squared)
            throw std::invalid_argument("negative portfolio variance exceeds covariance tolerance");
        variance = 0;
    }
    return detail::output(std::sqrt(variance * periods_per_year));
}

inline double beta(const std::vector<double>& asset, const std::vector<double>& market) {
    detail::sample(asset);
    detail::sample(market);
    if (asset.size() != market.size()) throw std::invalid_argument("beta dimension mismatch");
    const auto ma = detail::mean(asset), mm = detail::mean(market);
    long double cross = 0, denominator = 0;
    for (std::size_t i = 0; i < asset.size(); ++i) {
        const auto dm = static_cast<long double>(market[i]) - mm;
        cross += (static_cast<long double>(asset[i]) - ma) * dm;
        denominator += dm * dm;
    }
    if (denominator == 0) throw std::domain_error("beta undefined for constant market");
    return detail::output(cross / denominator);
}

inline double historical_var(const std::vector<double>& returns, double confidence = .95) {
    auto losses = detail::losses(returns, confidence);
    // Match the public double-precision confidence contract and NumPy's empirical
    // inverse-CDF boundary behavior. Widening 0.9 before multiplying by 10 can
    // incorrectly move a user-specified 90% quantile to the tenth observation.
    const auto rank = static_cast<std::size_t>(std::ceil(confidence * static_cast<double>(losses.size())));
    return losses[std::min(rank - 1, losses.size() - 1)];
}

inline double historical_expected_shortfall(const std::vector<double>& returns, double confidence = .95) {
    auto losses = detail::losses(returns, confidence);
    const auto mass = (1 - static_cast<long double>(confidence)) * losses.size();
    long double remaining = mass, weighted = 0;
    for (auto i = losses.rbegin(); i != losses.rend() && remaining > 0; ++i) {
        const auto take = std::min(remaining, 1.0L);
        weighted += take * *i;
        remaining -= take;
    }
    return detail::output(weighted / mass);
}

inline double concentration(const std::vector<double>& weights) {
    detail::sample(weights, 1);
    long double gross = 0;
    for (auto w : weights) gross += std::abs(static_cast<long double>(w));
    if (gross == 0) return 0;
    long double hhi = 0;
    for (auto w : weights) { const auto share = static_cast<long double>(w) / gross; hhi += share * share; }
    return detail::output(hhi);
}
} // namespace chronos::analytics
