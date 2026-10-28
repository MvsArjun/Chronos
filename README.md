# Chronos

C++20 market-data transport, deterministic replay, event-driven backtesting and
quantitative research. Built to demonstrate inspectable engineering and financial
correctness. This is not a forecasting model, a production trading system, or an
equivalent of any firm's private infrastructure.

## Working features

* Typed trades, quotes, book updates, OHLCV bars and corporate actions; exact
  fixed-point prices/money and checked integer arithmetic.
* Strict CSV, schema-checked Parquet, canonical versioned binary records with CRC,
  deterministic synthetic data and availability-aware replay.
* Bounded SPSC queue with verified lock-free index atomics, explicit backpressure,
  graceful shutdown and concurrent FIFO stress tests.
* Distinct next-open bar and displayed-quote execution, market/limit/cancel,
  partial fills, costs and fill-time cash/position checks.
* Position reversals, proportional integer cost allocation, realized/unrealized
  P&L, cash/equity, fees, dividends, exact integral splits and exposure metrics.
* Buy-and-hold and moving-average strategies; extensible C++/Python callbacks.
* Return/risk statistics, covariance/correlation, beta, historical VaR/ES and
  publication-aware momentum/value/quality/low-volatility research.
* GoogleTest, Google Benchmark, independent NumPy/SciPy references, reproducible
  CLI/Python workflows and saved audit/measurement evidence.

The local verified environment is **Windows 11, GCC 16.2.0, Python 3.12.8**. Linux
GCC/Clang and sanitizer presets/CI are supplied but have **not been executed** in
this Windows-only session. See [audit](docs/audit-report.md) for acceptance status.

## Build on Linux (primary intended target)

Requires a C++20 GCC/Clang toolchain, Git, Python 3.11+ and internet for the first
dependency download. GoogleTest 1.16.0 and Google Benchmark 1.9.2 archives are
version-pinned and SHA256-checked; Python version ranges are in requirements.txt.
The exact locally installed versions are recorded in requirements-lock.txt.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
cmake --preset release
cmake --build --preset release -j 4
ctest --preset release
export PYTHONPATH="$PWD/python"
python tools/run_checks.py
```

Use `CC=clang CXX=clang++ cmake --preset release` in a fresh build directory for
Clang. The `debug`, `asan` (AddressSanitizer + UndefinedBehaviorSanitizer) and `tsan`
presets are separate; Python and benchmarks are disabled in sanitizer presets.

```sh
cmake --preset asan
cmake --build --preset asan -j 4
ctest --preset asan
cmake --preset tsan
cmake --build --preset tsan -j 4
ctest --preset tsan
```

TSan requires a compatible Linux compiler/runtime/kernel. Failures or unavailable
runtimes must not be counted as successful race checks. To build only the C++ CLI,
configure with `-DCHRONOS_TESTS=OFF -DCHRONOS_BENCHMARKS=OFF -DCHRONOS_PYTHON=OFF`.
The core has no external runtime dependency beyond the C++ standard library.

## Build on Windows

Use a modern MinGW-w64 compiler and Ninja; the old MinGW GCC 6.3 cannot build this
C++20 project. `tools/build-windows.ps1` downloads the exact compiler/dependency
archives used here, verifies their hashes and builds/tests with a local venv.
Python must be installed and available as `python`. Run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build-windows.ps1
$env:PYTHONPATH = "$PWD\python"
.venv\Scripts\python.exe tools/run_checks.py
```

No global compiler or Python installation is modified. The initial downloads
require network access. To use your own compiler, set PATH and configure CMake
normally. GCC portability is locally verified; MSVC is not a verified target.

## CLI and Python

```sh
build/release/chronos_cli generate 10000 build/bars.csv
build/release/chronos_cli pack build/bars.csv build/bars.chr
build/release/chronos_cli replay build/bars.chr
build/release/chronos_cli pipeline build/bars.chr
build/release/chronos_cli backtest build/bars.chr 60000000000
build/release/chronos_cli backtest-tick data/samples/ticks.csv
PYTHONPATH=python python examples/research.py --output build/example-report
```

On Windows executables have an `.exe` suffix. CLI outputs ending `_fixed` are
integers in 1/10,000 currency units; 1000000 means 100 currency units.
Bar backtests require an explicit duration in nanoseconds. The generated bars
above use one-minute intervals; use the actual interval duration for other data.
The pack command leaves currency and adjustment metadata unspecified; provide
verified dataset metadata through the C++/Python binary writer for research use.

```python
import chronos as c
from chronos.report import export_report

events = c.ordered_events(c.load_csv("data/samples/bars.csv"))
config = c.ExecutionConfig()
config.bar_duration_ns = 86_400_000_000_000  # supplied sample bars are daily
config.commission_per_unit = 10  # 0.001 currency units per share
config.slippage_bps = 1
result = c.run_backtest(events, c.BuyAndHold(1, 10), config=config)
print(result.portfolio.cash, result.portfolio.equity)
export_report(result, "build/report")
```

`Strategy.on_event(event, portfolio)` returns OrderRequest objects;
`on_execution` and `cancel_orders` are optional callbacks. Orders cannot fill on
the observation that created them. For real daily bars configure duration/session
semantics explicitly; a daily label alone is insufficient. See
[execution rules](docs/execution-model.md) and
[API ownership/thread safety](docs/architecture.md).

Parquet helpers are in `chronos.parquet`. Tick CSV uses the same schema as other
events; vendor-specific columns must be normalized explicitly. The optional
Eulerpool client requires `EULERPOOL_API_KEY` and returns only raw, confirmed
endpoint responses. [Integration review](docs/eulerpool.md) records unresolved
timestamp, adjustment and publication contracts. Paid APIs are unnecessary.

## Tests, measurements and reproducibility

```sh
PYTHONPATH=python python -m unittest discover -s tests -p 'test_*.py' -v
PYTHONPATH=python python tools/validate_numerics.py --output build/numerical.json
PYTHONPATH=python python tools/measure.py --output build/measurements
build/release/chronos_bench --benchmark_min_time=0.1s
build/release/chronos_latency
```

`run_checks.py` runs CTest, Python, independent numerical references, serial versus
concurrent replay, CLI execution and the full research/report example. Unit,
integration, randomized invariant, concurrency and regression tests live in
`tests/`; they use fixed seeds and hand-computed fixtures. Their execution does not
require credentials. Reports under [docs/results](docs/results/) are actual local
outputs; do not confuse supplied CI configuration with an executed CI run.

With clang tools installed: `clang-format -i include/chronos/**/*.hpp src/*.cpp`
(expand paths as appropriate), and `clang-tidy -p build/release src/main.cpp`.
For Linux profiling: `perf record -g build/release/chronos_bench
--benchmark_filter=Backtest` then `perf report`; optionally run Valgrind on a Debug
CLI fixture. These profiling tools were unavailable locally and are not claimed
as executed. Build configuration/dependencies, dataset seed, CPU, flags, sampling
method and raw measurements are saved with the performance report.

## Documentation and limits

* [Architecture](docs/architecture.md), [data schema/storage](docs/market-data.md)
* [Execution/accounting](docs/execution-model.md), [numerical definitions](docs/numerical-validation.md)
* [Measured performance](docs/performance.md), [engineering audit](docs/audit-report.md)
* [Phases and gates](docs/implementation-plan.md), [Eulerpool review](docs/eulerpool.md)

Datasets are batch-materialized with resource caps, not streamed from a live
venue. No full order-book reconstruction, margin/borrow/FX model, fractional
corporate-action settlement, survivorship-free instrument master or verified
live vendor history is supplied. Research price adjustments/universe quality are
the caller's responsibility. Simulated execution and synthetic benchmark results
do not establish profitability or live exchange performance. MIT licensed.
