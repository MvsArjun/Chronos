# Numerical definitions and validation

`include/chronos/analytics/statistics.hpp` is the C++ source of the public
analytics API. `tools/validate_numerics.py` computes independent references with
NumPy and SciPy against the compiled Python extension. A report produced by a
successful run contains individual measured absolute errors, tolerances, package
versions, and the deterministic random seed. No precomputed pass result is used.

## Return conventions

Prices must be finite and strictly positive. Simple return is
`r[t] = P[t]/P[t-1] - 1`; log return is `log(P[t]) - log(P[t-1])`.
Cumulative return is `product(1+r[t])-1`. Cumulative compounding accepts returns
at -100% (bankruptcy) and rejects returns below -100%. Statistical estimators
accept any finite numerical returns, including leveraged losses below -100%,
because volatility and tail statistics do not themselves compound wealth.

The default annualization frequency is 252 equally spaced observations per year.
The caller must choose the frequency to match the data; the routines do not infer
time spacing or remove missing observations. Sample variance divides by `n-1`.
Annualized volatility is `sqrt(sample_variance(r) * periods_per_year)`.
Means are accumulated after subtracting the first observation. This preserves
exact zero variance for constant floating-point arrays and avoids cancellation
from a large common level in otherwise small differences.

For an annual effective risk-free rate `R_f`, the observation rate is
`r_f = expm1(log1p(R_f)/periods_per_year)`. Sharpe is
`sqrt(periods_per_year) * mean(r-r_f) / sample_stddev(r)`. Sortino uses the same
annual-to-period conversion for its target, with denominator
`sqrt(mean(min(r-target, 0)^2))` over **all** observations. A zero denominator
throws a domain error. Zero volatility itself is a valid result. Ratios require
at least two observations; compounding and tail risk require one.

Maximum drawdown is the positive loss fraction
`max_t(1 - equity[t]/max_{s<=t} equity[s])`. Input equity starts strictly positive
and may reach zero, but cannot become negative. Include initial equity in the
input if the first holding-period loss must be measured against it.

## Portfolio estimators

Covariance takes rows as observations and columns as assets; it uses `n-1`.
Inputs must be complete and finite. There is no implicit missing-value deletion
or pairwise covariance construction. Correlation divides covariance by marginal
standard deviations and rejects constant columns. Singular covariance is valid;
the implementation never inverts it.

Portfolio volatility is `sqrt(periods_per_year * w^T C w)`. Weights may be signed
and need not sum to one. The covariance matrix must be finite, square, symmetric,
and positive semidefinite within the declared rounding tolerance
`delta = 128 * epsilon(double) * max_abs_entry * dimension`. The dimension term
bounds accumulation across a row; the factor 128 allows double-input rounding
and subsequent factorization error. A subnormal floor prevents delta underflow
on platforms with narrow `long double`. A zero matrix returns zero directly.

Validation factors the symmetrized `C + delta*I` with diagonal pivoting, selecting
the largest remaining diagonal. This validation-only shift prevents a nearly
zero pivot in a rank-deficient covariance from amplifying roundoff. A materially
negative eigenvalue cannot be made positive by this small shift and is rejected.
This is an approximate PSD check, not proof that every accepted double matrix is
exactly PSD: negative eigenvalues within delta are treated as input rounding.

Reported variance comes from a compensated sum of the quadratic form on the
**original symmetrized C**, without the validation shift. No positive variance is
thresholded to zero; for example, a portfolio entirely in a `1e-20` variance
direction still has volatility `1e-10` even alongside a `1e12` variance direction.
A negative quadratic form is clamped to zero only when its magnitude is at most
`delta * sum(w^2)`, the admitted matrix error in that portfolio direction;
otherwise it fails explicitly. Computation uses long-double accumulators where
the compiler provides them. Platforms where `long double` equals `double` have
correspondingly less intermediate precision. Extremely ill-conditioned hedges
remain limited by the accuracy of the supplied double covariance itself.

Beta is `sample_covariance(asset, market)/sample_variance(market)` on aligned
equally spaced observations. Constant market returns are undefined. Position
concentration is the Herfindahl index `sum((abs(w)/sum(abs(w)))^2)`; an all-cash
zero-exposure vector has concentration zero. Gross and net monetary exposure
come from the portfolio ledger rather than this weight-based estimator.

## Historical loss tails

Loss observations are `L=-r`; positive losses indicate adverse returns. For
`0 < confidence < 1`, VaR is the empirical inverse CDF:
`sorted_losses[ceil(n*confidence)-1]`. No interpolation and no zero floor are
applied. An all-gain sample can therefore have negative VaR and ES.
The confidence product is evaluated in double precision, matching the public
input type and NumPy's `inverted_cdf` boundary convention. In particular, 90%
of ten observations selects rank nine and 80% of five selects rank four;
promoting the binary confidence to extended precision before multiplication
would give platform-dependent ranks. One representable confidence step above
90% selects rank ten in the ten-observation regression fixture.

Expected Shortfall averages the worst `(1-confidence)*n` observations, including
only the necessary fractional mass at the boundary. For example, with losses
`[-.2, -.1, .1, .2]` and confidence `.625`, tail mass is `1.5` and
ES is `(.2 + .5*.1)/1.5`. This definition is the upper empirical quantile
integral. It avoids the inconsistent effective confidence that can result from
averaging all observations at or above an interpolated VaR. These are historical
estimators, not forward loss guarantees; small samples have poor tail resolution.

## Factor research conventions

`python/chronos/research.py` expects timezone-aware UTC-convertible price timestamps
and explicit availability timestamps. Fundamentals additionally require period
end and publication timestamps with `period_end <= publication <= availability`.
No forward filling across an unknown publication date is performed. At cutoff
`t`, only records with event/publication time and availability no later than `t`
are visible. For historical revisions, the latest revision actually known by the
cutoff is selected. The helper cannot turn a current, restated database into a
point-in-time database: source provenance remains the caller's responsibility.

Momentum is the lookback simple return ending `skip` observed closes ago. Value
is book equity divided by latest visible close times shares. Quality is net
income divided by book equity. Low volatility is negative sample standard
deviation of the last lookback simple returns. Value/quality are unavailable when
book equity is nonpositive; insufficient histories yield explicit NaN factors.
Ranking excludes missing factors, sorts descending, and breaks ties by instrument
identifier. A formation with no eligible observations raises an error.
Visible prices can be stale if delivery is delayed; the prototype reports the
last visible price time and does not impose a maximum age filter. Lookback means
observed records, not calendar days. Cross-sectional comparability therefore
depends on the supplied trading calendars and currency/adjustment conventions.
Quality uses the supplied income period as-is; the helper does not manufacture
trailing-twelve-month income from incomplete statements. Factor arithmetic
overflow raises an error, while NaN is reserved for explicitly unavailable factors.

Portfolio formation selects a configurable top fraction and equal-weights it,
long-only. Formation occurs at a known close; target weights execute at the
**next** close and earn returns only afterward. This prevents a signal from
earning its own formation-to-execution return. The research simulation uses
fractional post-cost target weights, unlike the C++ whole-lot execution engine.
Costs are a stated linear approximation: `cost_bps/10000 * sum(abs(target -
drifted_weights))`. Initial deployment has turnover one. Costs reduce equity
before the next interval, so `net_return = (1-cost)*(1+gross_return)-1`.
Between rebalances weights drift with asset returns; cash earns zero.

Research requires a fixed rectangular execution panel, positive finite prices,
and consistent corporate-action adjustment. Missing/delisted prices fail rather
than silently removing an asset. Universe selection can still introduce
survivorship bias. There are no profitability claims.

## Executable checks

After building the Python extension, from the repository root run:

```sh
PYTHONPATH=python python tools/validate_numerics.py --output reports/numerical.json
PYTHONPATH=python python -m unittest discover -s tests -p 'test_research.py' -v
ctest --test-dir build/release --output-on-failure
```

The numerical script uses seed `20261028`, checks nontrivial random samples,
singular covariance with unequal scales and axis permutations, small positive
portfolio variance beside a large variance, decimal VaR boundaries, constant
series, negative compounding, bankruptcy, extreme
returns, one-observation tails, and expected exceptions for undefined or invalid
inputs. Default comparison tolerance is absolute `2e-12` plus relative `2e-11`.
Those tolerances cover floating-point roundoff in differing evaluation orders;
they do not relax exact fixed-point ledger accounting. The C++ tests include
hand-calculated references independent of the Python implementations. Python
tests perturb future price/fundamental records and verify that past factors do
not change; they also verify delayed fills, drifted turnover, transaction costs,
determinism, and missing-data rejection.

Actual execution outcomes belong in the generated report and final audit.
This document defines the checks and does not assert that an unexecuted build
or unavailable platform has passed.

## Actual local results â€” 2026-10-28

The final GCC 16.2.0 / Python 3.12.8 compiled-extension run passed **47 checks**:
34 numerical comparisons and 13 expected input/domain-error cases. NumPy 2.5.4
and SciPy 1.18.1 provide independent references. The largest absolute difference
among these comparisons was `3.25e-15`; each comparison passed its documented
absolute `2e-12` plus relative `2e-11` criterion. Full per-calculation errors and
exception outcomes are in [results/numerical.json](results/numerical.json).

Both Release and Debug C++ suites passed 57 tests, including hand-calculated
portfolio reversals/fees, a separate fractional-average accounting reference,
large exact integer allocation, 20,000 multiplication-oracle cases, constant
series, singular/indefinite matrices and late corporate-action rejection. Python
passed 26 tests, including factor publication/availability perturbations, delayed
execution, turnover, overflow, Parquet precision, binary layout/CRC, callback
ownership, immutable batch sharing and CLI assumption/reporting regressions.

The initial review found that ordinary summation could give a tiny nonzero
variance for long constant-return arrays. Shifted accumulation around the first
observation now preserves exactly zero variance; length-10 and length-1000
constant series have explicit volatility/correlation/Sharpe/beta regressions.

These outcomes cover the committed fixtures and synthetic observations. They do
not verify vendor point-in-time history or cross-platform floating-point identity.
Factors also require prices and fundamental share counts in consistent split/
adjustment units. The framework cannot infer or reconcile those units.

The complete pre-upload GCC/Clang/sanitizer matrix also passed; see
[pre-upload-verification.md](pre-upload-verification.md) for current per-build evidence.
