# Performance measurements — 2026-10-28

These are actual synthetic local measurements, not live exchange performance.

Host: AMD Ryzen 5 6600H with Radeon Graphics; 6 physical / 12 logical cores; 15.19 GiB RAM; Windows-11-10.0.26300-SP0.
Compiler: g++.exe (GCC) 16.2.0. CMake Release: -O3 -DNDEBUG; C++20. Python 3.12.8.

## Method

Google Benchmark 1.9.2, 0.05-second warm-up, minimum 0.1-second measurement, three repetitions per case. Table uses the median **wall time** (real_time), deriving throughput from that duration rather than noisy CPU-time counters. Workload: 10,000 deterministic synthetic bars, seed 1, one instrument. Ring round-trip measures one push/pop pair per iteration. Allocation cases process 4,096 Event values. Tests completed before the final timed run. No affinity isolation, fixed governor, frequency lock or background-load control was applied. File reads use the local filesystem with warm caches, not raw disk bandwidth.

## Throughput

| Case | Median wall time per iteration | Derived throughput |
|---|---:|---:|
| Dispatch | 6.695 µs | 1,493.700 million events or pairs/s |
| SpscRoundTrip | 0.009 µs | 107.990 million events or pairs/s |
| Pipeline/real_time | 1,450.954 µs | 6.892 million events or pairs/s |
| ReplayDispatch | 10.623 µs | 941.379 million events or pairs/s |
| ReplayPrepare | 1,382.310 µs | 7.234 million events or pairs/s |
| Backtest | 3,477.489 µs | 2.876 million events or pairs/s |
| CsvParse | 5,999.430 µs | 1.667 million events or pairs/s |
| BinaryRead | 4,384.067 µs | 2.281 million events or pairs/s |
| EventAllocation/0 | 35.953 µs | 113.926 million events or pairs/s |
| EventAllocation/1 | 13.206 µs | 310.163 million events or pairs/s |
| EventAllocation/2 | 13.200 µs | 310.308 million events or pairs/s |

Dispatch/ReplayDispatch are a minimal sequential checksum loop over already prepared memory. They demonstrate dispatch overhead only. ReplayPrepare includes copy, validation, global sequence tracking and sorting. Pipeline includes producer-thread creation/join, bounded transport and checksum over 10,000 events. Backtest includes input validation, the buy-and-hold strategy, execution, ledger and equity collection. Parsing cases include file I/O, validation and vector materialization. These scopes differ and must not be compared as equivalent production operations.

## Measured optimization and allocation experiment

BinaryRead changed from 8.012 ms to 4.384 ms per 10,000 records (1.83× in this run) after replacing bit-at-a-time CRC32 with a constexpr 256-entry IEEE CRC table. The table occupies 1 KiB. Canonical output and CRC were independently checked against Python struct/zlib; no format changed. Baseline raw files are preserved in results/baseline/. This before/after microbenchmark is evidence for this host and workload, not a universal speedup. Linux perf was unavailable, so no sampled CPU profile is claimed.

Growing PMR vector (case 0) makes 13 container allocations / 720,808 requested bytes per batch; reserved vector (case 1) makes 1 / 360,448 bytes; the fixed-buffer monotonic arena (case 2) makes 0 upstream allocations. Counts come from an instrumented memory_resource and exclude unrelated process allocations. The extra arena benefit varies across observed runs and has not been established as an engine bottleneck. Chronos retains ordinary preallocated storage instead of introducing a custom allocator into the engine.

## Latency distributions and timer limits

100,000 samples after 10,000 warm-ups. On this Windows host the sampler uses QueryPerformanceCounter at 10,000,000 Hz (100 ns per tick); Linux uses steady_clock. Clock pairs are measured as an instrumentation baseline. Reported nanosecond units are timer conversions, not claims of that timing precision.

| Observation | p50 | p90 | p99 | p99.9 |
|---|---:|---:|---:|---:|
| clock_pair | 0 ns | 100 ns | 100 ns | 100 ns |
| spsc_pair | 0 ns | 100 ns | 100 ns | 100 ns |
| queue_residence | 13300 ns | 42700 ns | 767200 ns | 1.2522e+06 ns |

The clock-pair and single-thread SPSC-pair distributions are indistinguishable at this timer resolution. **Their per-operation latency is unresolved**, including zero-tick observations; zero is not zero execution time. Google Benchmark's batch average is not an independently resolved individual-event latency. Queue residence includes a timestamp call, publication, backlog, OS scheduling and consumer timestamp. Producer retry refreshes the timestamp, so it excludes preceding full-queue wait. It is neither isolated enqueue/dequeue service time nor wire-to-strategy latency. A 100,000-sample p99.9 describes roughly 100 observations in the tail and remains sensitive to desktop scheduling.

## Python interface and memory

Empty native backtest convenience call: median 0.963 µs/call, p90 2.008 µs/call and p99 8.001 µs/call across 200 batches of 100 calls, after 1,000 warm-ups. This includes Python invocation, GIL transition, configuration snapshot and empty result construction. These are batch-average quantiles, not individual-call latency tails or pure ABI crossing cost.

Benchmark-process observed peak RSS: 10,334,208 bytes (9.86 MiB). Method: 10ms RSS sampling; Windows peak_wset if exposed; lower bound otherwise. It includes Google Benchmark, datasets, experiments and threads. It is not a process-wide allocation count or a large-dataset memory scalability result.

## Reproduce

```sh
PYTHONPATH=python python tools/measure.py --output build/measurements
```

To reproduce the original CRC implementation, configure a separate Release build with `-DCHRONOS_CRC_BASELINE=ON -DCHRONOS_PYTHON=OFF` and run its BinaryRead benchmark. This selects the retained bitwise reference without changing the format. Default builds use the lookup table.

Raw context, repetitions, CPU-time values, clock quantiles, Python batches and memory method are preserved under docs/results/. Exact numbers will change with CPU frequency, thermals, compiler, timer, operating system and contention. Large tick datasets, live feed bursts, CPU-pinned cross-core comparisons, perf/Valgrind analysis and hardware-counter studies remain unverified.
