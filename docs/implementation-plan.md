# Implementation plan and acceptance gates

1. **Repository/build**: C++20 CMake library, CLI, GoogleTest, Google Benchmark,
   Python extension, separate ASan+UBSan and TSan presets. Gate: configure and compile.
2. **Data**: typed fixed-point events, validated CSV, canonical little-endian binary
   records with CRC and metadata, deterministic replay. Gate: round-trip, corruption,
   ordering, invalid-input and reproducibility tests.
3. **Transport**: SPSC bounded ring, explicit overflow policy and clean producer
   lifecycle. Gate: FIFO, saturation, concurrent stress and handler-error tests;
   benchmark dispatch and queue against single-thread baseline.
4. **Simulation**: next-event bar and quote execution, costs, cancellation, risk
   checks and checked integer portfolio ledger. Gate: independent ledger examples,
   reversals, partial fills, causality and repeatability regressions.
5. **Research**: return/risk statistics and publication-aware factor formation.
   Gate: NumPy/SciPy comparisons and point-in-time adversarial tests.
6. **Python**: bindings, local Parquet conversion, CLI and research examples.
   Gate: end-to-end example and Python integration tests.
7. **Measurement**: Google Benchmark throughput, sampled tail latencies, allocation
   experiments, environment metadata and interface overhead. Gate: saved raw output.
8. **Audit**: run supported checks, record limitations and unfinished criteria;
   never treat an unrun sanitizer or Linux build as verified.

See audit-report.md for evidence and final status. Linux is the primary intended
target. The authoring host is Windows without WSL; local checks use a modern
portable GCC where available. No production-equivalence or profitability claim.
