"""Independent NumPy/SciPy checks of the compiled C++ implementation.

Run with PYTHONPATH=python and the built chronos._core extension installed there.
The optional --output path stores measured errors as JSON; no static pass fixture.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import platform

import numpy as np
import scipy
from scipy import stats

from chronos import _core as cpp


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    rng = np.random.default_rng(20261028)
    r = rng.normal(.0004, .013, 1000)
    market = rng.normal(.0003, .009, 1000)
    prices = 100 * np.exp(np.r_[0, np.cumsum(r)])
    observations = rng.normal(0, .01, (1000, 4))
    covariance = np.cov(observations, rowvar=False, ddof=1)
    weights = np.array([.3, .2, .4, .1])
    checks = []

    def compare(name, actual, expected, *, atol=2e-12, rtol=2e-11):
        actual, expected = np.asarray(actual, dtype=float), np.asarray(expected, dtype=float)
        np.testing.assert_allclose(actual, expected, atol=atol, rtol=rtol, err_msg=name)
        checks.append({"name": name, "max_abs_error": float(np.max(np.abs(actual - expected))),
                       "atol": atol, "rtol": rtol})

    compare("simple_returns", cpp.simple_returns(prices.tolist()), np.diff(prices) / prices[:-1])
    compare("log_returns", cpp.log_returns(prices.tolist()), np.diff(np.log(prices)))
    compare("cumulative_return", cpp.cumulative_return(r.tolist()), np.prod(1 + r) - 1)
    # SciPy independently computes unbiased sample variance.
    variance = stats.describe(r, ddof=1).variance
    compare("annualized_volatility", cpp.annualized_volatility(r.tolist(), 252), np.sqrt(variance * 252))
    rf_period = np.expm1(np.log1p(.03) / 252)
    compare("sharpe_ratio", cpp.sharpe_ratio(r.tolist(), 252, .03),
            np.mean(r - rf_period) / np.sqrt(variance) * np.sqrt(252))
    target = np.expm1(np.log1p(.01) / 252)
    compare("sortino_ratio", cpp.sortino_ratio(r.tolist(), 252, .01),
            np.mean(r - target) / np.sqrt(np.mean(np.minimum(r - target, 0) ** 2)) * np.sqrt(252))
    equity = 100 * np.cumprod(1 + r)
    compare("maximum_drawdown", cpp.maximum_drawdown(equity.tolist()),
            np.max(1 - equity / np.maximum.accumulate(equity)))
    compare("covariance", cpp.covariance(observations.tolist()), covariance)
    compare("correlation", cpp.correlation(observations.tolist()), np.corrcoef(observations, rowvar=False))
    compare("portfolio_volatility", cpp.portfolio_volatility(weights.tolist(), covariance.tolist(), 252),
            np.sqrt(weights @ covariance @ weights * 252))
    compare("beta", cpp.beta(r.tolist(), market.tolist()), np.cov(r, market, ddof=1)[0, 1] / np.var(market, ddof=1))
    compare("concentration", cpp.concentration(weights.tolist()), np.sum((np.abs(weights) / np.abs(weights).sum()) ** 2))
    for confidence in (.5, .95, .975, .999):
        compare(f"VaR_{confidence}", cpp.historical_var(r.tolist(), confidence),
                np.quantile(-r, confidence, method="inverted_cdf"))
        # Integrate the empirical survival quantile with exact fractional tail mass.
        loss = np.sort(-r)[::-1]
        mass = len(loss) * (1 - confidence)
        included = np.clip(mass - np.arange(len(loss)), 0, 1)
        expected_es = np.dot(included, loss) / mass
        compare(f"ES_{confidence}", cpp.historical_expected_shortfall(r.tolist(), confidence), expected_es)
    singular = np.column_stack((r, 2 * r))
    compare("singular_covariance", cpp.covariance(singular.tolist()), np.cov(singular, rowvar=False))
    compare("singular_portfolio", cpp.portfolio_volatility([1, 1], np.cov(singular, rowvar=False).tolist(), 1),
            3 * np.std(r, ddof=1))
    rank_one = np.array([0.0003427698826407296, 0.40292055875370403,
                         -0.0014391140288242679, -767.7286890404523,
                         0.0031402142979679243, 80.66934754730025])
    for rotation in range(3):
        vector = np.roll(rank_one, rotation)
        compare(f"unequal_scale_rank_one_{rotation}",
                cpp.portfolio_volatility([1 / len(vector)] * len(vector), np.outer(vector, vector).tolist(), 1),
                abs(np.mean(vector)))
    compare("small_positive_portfolio_variance",
            cpp.portfolio_volatility([0, 1], [[1e12, 0], [0, 1e-20]], 1), 1e-10,
            atol=1e-24, rtol=2e-13)
    for count, confidence in ((10, .9), (5, .8), (10, float(np.nextafter(.9, 1)))):
        losses = np.arange(1, count + 1, dtype=float)
        compare(f"decimal_boundary_VaR_{count}_{confidence!r}",
                cpp.historical_var((-losses).tolist(), confidence),
                np.quantile(losses, confidence, method="inverted_cdf"), atol=0, rtol=0)
    compare("constant_volatility", cpp.annualized_volatility([.01] * 10, 252), 0)
    compare("bankruptcy_drawdown", cpp.maximum_drawdown([100, 0]), 1)
    compare("negative_cumulative_return", cpp.cumulative_return([-.2, -.3]), -.44)
    compare("one_observation_tail", cpp.historical_expected_shortfall([-.2], .95), .2)
    compare("large_return", cpp.cumulative_return([1e100, -0.5]), .5 * (1 + 1e100) - 1)
    exception_checks = {
        "zero_variance_sharpe": lambda: cpp.sharpe_ratio([.1, .1], 252, 0),
        "long_constant_sharpe": lambda: cpp.sharpe_ratio([.01] * 1000, 252, 0),
        "constant_correlation": lambda: cpp.correlation([[.01, .07]] * 1000),
        "long_constant_beta": lambda: cpp.beta([.01] * 1000, [.01] * 1000),
        "no_downside_sortino": lambda: cpp.sortino_ratio([.1, .2], 252, 0),
        "missing_nan": lambda: cpp.annualized_volatility([.1, float("nan")], 252),
        "nonfinite": lambda: cpp.simple_returns([1, float("inf")]),
        "small_sample": lambda: cpp.annualized_volatility([.1], 252),
        "zero_price": lambda: cpp.simple_returns([1, 0]),
        "indefinite_covariance": lambda: cpp.portfolio_volatility([.5, .5], [[1, 2], [2, 1]], 1),
        "unheld_negative_covariance_direction": lambda: cpp.portfolio_volatility([0, 1], [[-.001, 0], [0, 1]], 1),
        "zero_market_variance": lambda: cpp.beta([1, 2], [1, 1]),
        "invalid_return": lambda: cpp.cumulative_return([-1.01]),
    }
    for name, function in exception_checks.items():
        try:
            function()
        except (ValueError, RuntimeError, OverflowError):
            checks.append({"name": name, "expected_exception": True})
        else:
            raise AssertionError(f"{name}: expected a numerical input/domain error")
    result = {"passed": len(checks), "python": platform.python_version(), "numpy": np.__version__,
              "scipy": scipy.__version__, "seed": 20261028, "checks": checks}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
