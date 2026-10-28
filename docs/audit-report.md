# Chronos engineering audit — 2026-10-28

## Disposition

A working C++20 repository, CLI, Python extension, data formats/adapters, strategy
and accounting engine, analytics/factor workflows, tests, measurements and
documentation are implemented. The entire original acceptance checklist is
**not complete**: Linux GCC/Clang, ASan+UBSan and TSan have not been executed on
this Windows-only host. Supplied presets/CI do not constitute those verifications.
No claim of production readiness, trading profitability or equivalence to a
quantitative firm's proprietary infrastructure is made.

## Implemented and exercised

| Acceptance item | Status and evidence |
|---|---|
| C++20 build | PASS locally: Windows 11 x64, GCC 16.2.0, CMake 4.4.4, Ninja 1.13.2; Release and Debug. Linux GCC/Clang unverified. |
| Historical loading/validation | PASS: strict CSV, explicit-endian binary/CRC, exact-schema Parquet, synthetic bars/trades and trade/quote samples. |
| Deterministic replay | PASS: availability/event/sequence total order, duplicate rejection, logical clock, equal serial/pipeline checksums and repeated ledger/report tests. |
| SPSC correctness/concurrency | PASS: full/empty, wrap, nontrivial object lifetime, 500,000-item FIFO stress, saturation/backpressure, handler failures and stop/drain. Lock-free index atomic requirement is checked for this x64 target. TSan remains unrun. |
| Pipeline measurement | PASS: Google Benchmark repetitions, single-thread baselines, separate preparation/dispatch, end-to-end bounded transport and sampled residence distributions. |
| Event-driven backtest | PASS: market/limit, cancel, later bar-open/quote matching, partial fills, shared liquidity, costs, risk checks and two example strategies. |
| Accounting independent validation | PASS: exact hand-calculated reversal/fee examples, random transitions versus a separate fractional-average reference, overflow and ledger identity tests. |
| Risk/return independent validation | PASS: 39 NumPy/SciPy checks using compiled C++ outputs, including 12 explicit undefined/invalid-input cases. |
| Factor research | PASS on synthetic suitable inputs: momentum, value, quality and low volatility; publication-aware formation, delayed close execution, rebalancing, drifted turnover and costs. Historical vendor PIT coverage is unverified. |
| Python bindings | PASS: owning batches, strategy/report APIs, Parquet, immutable shared-batch native runs, configuration snapshot, retained callback snapshots and an installable CPython 3.12 Windows wheel smoke test. |
| CLI/examples | PASS: generation, binary packing, serial replay, pipeline, bar and tick backtest, all four factors and report/Parquet workflow. |
| Unit/integration/property/regression | PASS: 48 C++ tests in each of Release and Debug; 26 Python tests; fixed seeds and fixtures. |
| Sanitizers | **INCOMPLETE**: ASan+UBSan and separate TSan presets/CI supplied. Unsupported on this local MinGW verification configuration; no Linux runtime exists here. |
| Reproducible benchmarks | PASS within documented synthetic scope: source, baseline implementation, raw repetitions, compiler/CPU/flags/data/method and measurement scripts saved. Desktop noise and timer resolution limit interpretation. |
| Documentation/audit | PASS: architecture, data, financial definitions, numerical results, performance, integration boundaries and outstanding limitations recorded. |

## Executed checks and outcomes

`tools/run_checks.py` completed with every command returning zero: CTest, Python
unittest discovery, independent numerical validation, generation, pack, replay,
pipeline, both execution examples and the full research/report example. Raw logs
are in [results/checks.json](results/checks.json) and adjacent text files.

* [Release C++ XML](results/cpp-tests.xml): 48 tests, zero failures/errors.
* [Debug C++ XML](results/cpp-debug-tests.xml): 48 tests, zero failures/errors.
* [Python log](results/python-tests.txt): 26 tests, zero failures, zero skipped.
* [Independent numerical results](results/numerical.json): 39 passed; largest
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
and GitHub Actions were not executed. A first example run exposed a factor-name
mismatch (`volatility` versus `low_volatility`); it was corrected and the entire
workflow rerun successfully. No failed run is presented as a successful result.

## Measurements

See [performance.md](performance.md) for exact scope and raw links. On the recorded
Ryzen 5 6600H Windows host, final median wall times per 10,000 synthetic events
were approximately 0.507 ms for bounded transport/dispatch, 2.077 ms for the
buy-and-hold backtest and 3.652 ms for checked binary reading. These correspond
to approximately 19.72M, 4.81M and 2.74M events/second in those distinct workloads.

The CRC table experiment reduced binary-read median from 8.012 to 3.652 ms in
the recorded baseline/final runs. Retained bitwise CRC can be selected with
`CHRONOS_CRC_BASELINE=ON`. Growing/reserved/arena containers made 13/1/0 upstream
allocations per 4,096 events. No custom allocator was adopted into the engine.
Observed process peak RSS was about 8.21 MiB for the benchmark workload; this is
not a large-dataset memory bound. The measured Event is 88 bytes, alignment 8.

Direct SPSC-pair p50 was below the 100 ns timer tick and indistinguishable from
the clock baseline. Its per-operation latency is **unresolved**, not zero. Queue
residence includes backlog/scheduling; it is not exchange latency. Python
interface numbers are batch-average convenience-call timings. All measurements
are synthetic, with no affinity isolation or frequency control.

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

## Known limitations and technical debt

* **Platform verification:** Linux compiler compatibility and all sanitizer runs
  remain the first unfinished acceptance work. CI must actually run and be read.
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
* **Execution:** fixed per-run bar duration, no exchange session calendar or
  variable interval starts. Bar liquidity is assumed, limits only fill at an
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

1. Run the supplied Linux GCC/Clang and sanitizer matrix; fix failures and attach
   actual logs before declaring those acceptance items complete.
2. Add a small licensed real trade/quote corpus with explicit exchange/session,
   instrument, timestamp and corporate-action contracts; compare independent ledgers.
3. Add chunked reading and measure realistic memory/throughput, then profile with
   perf before choosing additional optimizations.
4. Introduce calendar/interval-start and settlement/borrow models only for a clear
   research use case, with new independent accounting/causality fixtures.
5. Add historical universe/adjustment provenance and validated PIT fundamental data.

The implemented local workflows are functional and measured. The unresolved
items above remain explicit engineering work rather than implied production
capabilities.
