# Pre-upload verification — 2026-10-28

The publication gate passed before any GitHub source upload. The verified code/configuration SHA256 is:

`854ef84ed3ffc0efbaeb0325779d47552668640047c4a62897f62ccc6aa2415d`

The hash covers C++/Python implementation, tests, tooling, build configuration and CI. The Linux runner recorded the same hash before and after the matrix. Generated results, native extension binaries and documentation are excluded from that code hash; the Git commit captures the complete deliverable.

| Environment/configuration | Executed checks | Outcome |
|---|---|---|
| Windows GCC 16.2 release | 57 C++ | PASS |
| Windows GCC 16.2 debug | 57 C++ | PASS |
| Ubuntu g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; release | 57 C++; 26 Python; 47 numerical; CLI/research and benchmarks | PASS |
| Ubuntu g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; asan-ubsan | 57 C++ | PASS |
| Ubuntu g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0; tsan | 57 C++ | PASS |
| Ubuntu Ubuntu clang version 18.1.3 (1ubuntu1); release | 57 C++; 26 Python; 47 numerical; CLI/research and benchmarks | PASS |
| Ubuntu Ubuntu clang version 18.1.3 (1ubuntu1); asan-ubsan | 57 C++ | PASS |
| Ubuntu Ubuntu clang version 18.1.3 (1ubuntu1); tsan | 57 C++ | PASS |

Windows Release additionally passed all 26 Python tests, 47 independent NumPy/SciPy checks, CLI workflows and the complete research example. Both Linux Release configurations exercised the same workflows and Google Benchmark/latency measurement tools. Sanitizer configurations are separate Debug builds with the Python extension and benchmarks disabled.

The Windows CPython 3.12 wheel was rebuilt, installed into a separate target directory and imported in isolated Python mode. The smoke check exercised native backtesting, a decimal VaR boundary and the difficult rank-one covariance fixture.

## Evidence

- [Publication gate](results/pre-upload/gate.json)
- [Linux matrix](results/pre-upload/linux/matrix.json), with per-configuration build/test logs and GoogleTest XML
- [Windows Release](results/pre-upload/windows-release/checks.json) and [Debug XML](results/pre-upload/windows-debug/cpp-tests.xml)
- [Wheel smoke](results/pre-upload/wheel-smoke.json)
- [Linux image provenance](results/pre-upload/linux-image.json), [guest environment](results/pre-upload/linux-environment/) and [sanitizer controls](results/pre-upload/linux-runtime.json)
- [Performance report](performance.md) and per-compiler Linux measurement output

## Corrections made before verification

- Empirical VaR now uses the documented double-precision confidence convention, including decimal boundary/nextafter regressions.
- Pivoted covariance validation accepts roundoff in singular matrices while the original matrix supplies compensated portfolio variance; small valid variances are preserved.
- Backtests reject per-instrument economic-time/sequence regressions, so an obsolete quote or pre-split price cannot supply fills or marks.
- Bar execution explicitly accepts one instrument, pure bars, timely delivery and a nonoverlapping fixed grid. Unsupported asynchronous/multi-asset bar portfolios are rejected before callbacks. Multi-instrument tick execution remains available.
- CMake preset schema now agrees with the declared CMake 3.24 minimum.
- Benchmark compiler/flags metadata is read from the actual CMake cache rather than inferred from a shell default.

## Environment and scope

Linux verification ran on an official SHA256-checked Ubuntu 24.04.5 image in a disposable VMware guest (2 vCPU, 4 GiB), with GCC 13.3, Clang 18.1.3, CMake 3.28.3 and Python 3.12.3. The guest was powered off after evidence collection. Windows features were unchanged. Linux measurements are virtual-machine measurements, not bare-metal comparisons or exchange performance.

GCC ThreadSanitizer initially failed to initialize with the guest's 32-bit mmap randomization. The guest and ephemeral TSan CI jobs use 28 bits; both compilers then passed a correct canary and detected a separate intentional race. See [sanitizers.md](sanitizers.md). No sanitizer diagnostics were suppressed.

The test suite is finite evidence for the supported model, not a production-readiness guarantee. Authenticated Eulerpool compatibility and historical vendor point-in-time data remain unverified; local CSV/Parquet/binary workflows require no paid API. Other financial/data/model limitations remain in the [audit](audit-report.md) and [execution model](execution-model.md).
