# Chronos engineering audit â€” 2026-10-28

## Disposition

The implemented workflows passed pre-upload verification on Windows GCC 16.2 and
Ubuntu 24.04.5 with GCC 13.3 and Clang 18.1. Separate Address/UndefinedBehaviorSanitizer
and ThreadSanitizer builds passed under both Linux compilers. Each of eight final
build configurations passed 57 C++ cases; the three Release environments also
passed 26 Python tests and 47 independent numerical checks each.

The code/configuration hash and raw evidence are in
[pre-upload-verification.md](pre-upload-verification.md). The publication gate
passed before source upload. This verifies the supported model described below;
it does not establish production readiness, profitability, authenticated vendor
history, or equivalence to a firm's private infrastructure.

## Implemented and exercised

| Acceptance item | Status and evidence |
|---|---|
| C++20 build | PASS: Windows GCC 16.2 Release/Debug; Ubuntu GCC 13.3 and Clang 18.1 Release plus separate sanitizer builds. |
| Historical loading/validation | PASS: strict CSV, explicit-endian binary/CRC, exact-schema Parquet, synthetic bars/trades and trade/quote samples. |
| Deterministic replay | PASS: availability/event/sequence total order, duplicate rejection, logical clock, equal serial/pipeline checksums and repeated ledger/report tests. |
| SPSC correctness/concurrency | PASS: FIFO/lifetime/saturation/stop tests and 500,000-item stress; both Linux TSan builds passed without race reports. Canary controls verify the runtime detects races. |
| Pipeline measurement | PASS: Google Benchmark repetitions, single-thread baselines, separate preparation/dispatch, end-to-end bounded transport and sampled residence distributions. |
| Event-driven backtest | PASS within explicit contracts: timely single-instrument pure bars; multi-instrument ticks; market/limit/cancel, partial fills, costs and fill-time risk checks. Late economic regressions are rejected. |
| Accounting independent validation | PASS: exact hand-calculated reversal/fee examples, random transitions versus a separate fractional-average reference, overflow and ledger identity tests. |
| Risk/return independent validation | PASS: 47 NumPy/SciPy checks per Release environment, including 13 explicit error cases and VaR/singular-covariance boundary regressions. |
| Factor research | PASS on synthetic suitable inputs: momentum, value, quality and low volatility; publication-aware formation, delayed close execution, rebalancing, drifted turnover and costs. Historical vendor PIT coverage is unverified. |
| Python bindings | PASS: owning batches, strategy/report APIs, Parquet, immutable shared-batch native runs, configuration snapshot, retained callback snapshots and an installable CPython 3.12 Windows wheel smoke test. |
| CLI/examples | PASS: generation, binary packing, serial replay, pipeline, bar and tick backtest, all four factors and report/Parquet workflow. |
| Unit/integration/property/regression | PASS: 57 C++ cases in every final configuration; 26 Python cases per Release environment; fixed seeds and independent references. |
| Sanitizers | PASS: GCC and Clang ASan+UBSan and separate TSan builds, 57 C++ cases each. Guest/ephemeral CI mmap setting is documented in sanitizers.md. |
| Reproducible benchmarks | PASS within documented synthetic scope: source, baseline implementation, raw repetitions, compiler/CPU/flags/data/method and measurement scripts saved. Desktop noise and timer resolution limit interpretation. |
| Documentation/audit | PASS: architecture, data, financial definitions, numerical results, performance, integration boundaries and outstanding limitations recorded. |

## Executed checks and outcomes

`tools/run_checks.py` completed with every command returning zero: CTest, Python
unittest discovery, independent numerical validation, generation, pack, replay,
pipeline, both execution examples and the full research/report example. Raw logs
are in [results/checks.json](results/checks.json) and adjacent text files.

* [Release C++ XML](results/cpp-tests.xml): 57 tests, zero failures/errors.
* [Debug C++ XML](results/cpp-debug-tests.xml): 57 tests, zero failures/errors.
* [Python log](results/python-tests.txt): 26 tests, zero failures, zero skipped.
* [Linux matrix](results/pre-upload/linux/matrix.json): all six configurations
  passed with an unchanged code/configuration hash.
* [Independent numerical results](results/numerical.json): 47 passed; largest
  observed absolute comparison error 3.25e-15 within specified tolerances.
* Binary bytes were independently decoded with Python struct and checked against
  zlib.crc32; all payloads and large uint64 sequences round-trip through Parquet.
* The Python wheel was built and imported from a separate installation directory;
  native synthetic backtest returned the expected position. It is platform/ABI
  specific and does not establish Linux wheel compatibility.

Additional development checks included warning-enabled C++ compilation and a
PowerShell syntax parse of the Windows bootstrap script. The complete bootstrap
script was not run from a clean machine; its equivalent manual dependency/build
workflow was run. clang-format, clang-tidy, perf, Valgrind, real exchange adapters
were not executed. At the pre-upload checkpoint, GitHub Actions had not yet
executed; the complete local Linux matrix did pass. A first example run exposed a factor-name
mismatch (`volatility` versus `low_volatility`); it was corrected and the entire
workflow rerun successfully. No failed run is presented as a successful result.

## Measurements

See [performance.md](performance.md) for the current exact scope, timings and raw
links. The final Windows medians per 10,000 synthetic events were
1.451 ms for bounded transport,
3.477 ms for the validated backtest and
4.384 ms for checked binary reading. Background
load and scheduling were not controlled; differing scopes must not be compared
as equivalent production operations. Both Linux Release configurations also ran
measurements; those results are explicitly VMware guest results.

The retained CRC baseline and current table implementation produce identical
canonical bytes. Container experiments made 13/1/0 upstream allocations for
growing/reserved/arena storage; no custom allocator was adopted. Observed Windows
benchmark peak RSS was 9.86 MiB,
which is not a large-dataset scalability bound. Event size was 88 bytes/alignment8.
Direct queue-pair latency remained below the Windows timer's resolving power;
it is reported as unresolved, not zero or exchange latency.

## Review findings resolved

1. **Constant-series numerical denominators:** shifted accumulation now preserves
   exactly zero variance; longer constant samples test Sharpe, beta and correlation.
2. **Mutable Python configuration:** native runs copy settings while holding the
   GIL before releasing it. Python strategies also see a fixed configuration.
3. **Callback ownership:** explicit owned snapshots and retained-object tests make
   lifetimes reviewable. Detailed review confirmed pybind11's original const
   argument conversion already copied; a dangling-reference defect was not
   established in that conversion.
4. **Bar timing:** matching checks the modeled opening time against acceptance.
   The CLI requires explicit bar duration; daily sample/minute synthetic examples
   provide their actual intervals.
5. **Corporate-action entitlement:** simulation rejects delayed actions and actions
   ordered after same-time same-instrument market events. It does not apply stale
   ex-date actions to present holdings.
6. **Cash risk/report accuracy:** sell-side fees receive a cash check; rejected or
   cancelled reports do not inflate CLI fill counts; unknown CSV currency and
   adjustments are no longer labeled USD/unadjusted by the pack command.
7. **Data parser bounds/compatibility:** fixed row limits, record/count/size caps,
   inactive-field validation, classic output locale and independent CRC/layout
   tests protect deterministic interchange.
8. **Measured bottleneck:** bitwise CRC became a portable table implementation with
   preserved bytes. Arena experiments did not justify allocator complexity.

9. **Stale state and bar causality:** economic watermarks reject stale quote/
   pre-split state before callbacks. The bar contract rejects unsupported delayed,
   mixed, asynchronous/multi-asset and overlapping grids instead of funding an
   earlier modeled open with later proceeds.
10. **Numerical/build portability:** decimal VaR boundaries match the double
    confidence convention; pivoted covariance validation handles singular
    roundoff while preserving small positive variance. Preset schema5 agrees
    with the declared CMake minimum. Compiler metadata comes from CMake cache.

## Known limitations and technical debt

* **Platform scope:** the recorded Windows/Ubuntu compiler matrix is verified.
  Other architectures and toolchain/runtime combinations remain outside that
  evidence. TSan needs a compatible address layout; see sanitizers.md.
* **Data integrity/provenance:** CRC detects accidental corruption, not malicious
  modification. Metadata is opaque, so currency, instrument mapping, adjustment
  basis and provenance are caller contracts. C++ writes are not atomic; stage and
  rename if publishing datasets. Parquet output stages before replacement.
* **Memory/scalability:** files are materialized with a ten-million-event cap.
  Replay validation/sorting and backtests add O(n) memory; the cap is not a promise
  that an arbitrary machine can process ten million events without exhaustion.
  No chunked/mmap reader, indexed dataset or live-feed watermark/gap recovery.
* **Replay clock:** logical availability time with reset/repeat is implemented.
  Configurable wall-clock pacing, checkpoint/resume and alternative clock modes
  are not implemented. A different clock must not expose unavailable data.
* **Execution:** one-instrument timely pure-Bar fixed grids; no atomic multi-asset
  bar batches, asynchronous/mixed bar portfolios, exchange session calendar or
  variable interval starts. Tick input rejects per-instrument economic regressions. Bar liquidity is assumed, limits only fill at an
  eligible later open. Quotes have no known queue position or replenishment;
  trades do not fabricate executable liquidity. Book updates are stored/replayed,
  not rebuilt into a full exchange book. No venue/auction/latency/settlement model.
* **Ledger/risk:** whole lots, one currency, proportional integer cost basis and
  fill-time cash checks. No cash reservations, margin, borrow fees, financing,
  FX, external cash flows or exposure-limit engine. Shorting is an optional
  simplified ledger/execution mode; risk controls are not institution-wide risk.
* **Corporate actions:** effective-time ex-date cash convention; actions must be
  timely and ordered first. Splits require exact integral quantities and marks.
  No fractional cash-in-lieu, spin-offs, merger processing or historical late-action
  entitlement reconstruction. Multiple simultaneous actions require declared order.
* **Factor research:** separate fractional delayed-close execution, supplied fixed
  rectangular universe, no delisting/listing membership inference, no automatic
  stale-observation expiry and no order-level parity claim with C++ simulation.
  Value/quality use supplied available statements, not an inferred TTM series.
  Prices and share counts must use consistent point-in-time split/adjustment units;
  adjusted prices combined with stale statement shares can distort value.
* **Python:** batches avoid repeated event-vector conversion in native backtests,
  but analytics use copying STL sequence conversion. Parquet is an offline copying
  adapter. Python callback strategies hold the GIL and snapshots have a cost.
  Strategy instances require fresh/reset state and cannot be shared concurrently.
* **Eulerpool:** official public docs reviewed; confirmed profile/candle client,
  bounded retries and redacted errors implemented. Authenticated compatibility,
  entitlements, complete history, exact timestamp units, adjustments and PIT
  fundamental publication contracts remain unverified. No guessed routes or
  artificial live results; local workflows require no paid service.
* **Performance scope:** no live-feed captures, large real tick corpus, long-running
  thermal study, pinned-core tail study, perf counters or exchange latency evidence.

## Recommended next work

1. Keep the now-verified GCC/Clang/sanitizer matrix green as compilers, kernels
   and datasets change, preserving new logs and source hashes.
2. Add a small licensed real trade/quote corpus with explicit exchange/session,
   instrument, timestamp and corporate-action contracts; compare independent ledgers.
3. Add chunked reading and measure realistic memory/throughput, then profile with
   perf before choosing additional optimizations.
4. Introduce calendar/interval-start and settlement/borrow models only for a clear
   research use case, with new independent accounting/causality fixtures.
5. Add historical universe/adjustment provenance and validated PIT fundamental data.

The supported local workflows passed the recorded pre-upload matrix. The unresolved
items above remain explicit engineering work rather than implied production
capabilities.
