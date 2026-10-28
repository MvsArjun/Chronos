"""Generate the checked-in Markdown table from captured benchmark evidence."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
results = root / "docs/results"
environment = json.loads((results / "environment.json").read_text())
bench = json.loads((results / "benchmark.json").read_text())
baseline = json.loads((results / "baseline/benchmark.json").read_text())
latency = json.loads((results / "latency.json").read_text())
interface = json.loads((results / "python-interface.json").read_text())
memory = json.loads((results / "memory.json").read_text())
medians = {b["run_name"]: b for b in bench["benchmarks"] if b.get("aggregate_name") == "median"}
previous = {b["run_name"]: b for b in baseline["benchmarks"] if b.get("aggregate_name") == "median"}
lines = ["# Performance measurements — 2026-10-28", "", "These are actual synthetic local measurements, not live exchange performance.", "",
         f"Host: {environment['cpu']}; {environment['physical_cores']} physical / {environment['logical_cores']} logical cores; {environment['ram_bytes']/2**30:.2f} GiB RAM; {environment['os']}.",
         f"Compiler: {environment['compiler']}. {environment['flags']}. Python {environment['python']}.", "",
         "## Method", "", "Google Benchmark 1.9.2, 0.05-second warm-up, minimum 0.1-second measurement, three repetitions per case. Table uses the median **wall time** (real_time), deriving throughput from that duration rather than noisy CPU-time counters. Workload: 10,000 deterministic synthetic bars, seed 1, one instrument. Ring round-trip measures one push/pop pair per iteration. Allocation cases process 4,096 Event values. Tests completed before the final timed run. No affinity isolation, fixed governor, frequency lock or background-load control was applied. File reads use the local filesystem with warm caches, not raw disk bandwidth.", "",
         "## Throughput", "", "| Case | Median wall time per iteration | Derived throughput |", "|---|---:|---:|"]
for name,b in medians.items():
    count = 4096 if name.startswith("EventAllocation") else (1 if name == "SpscRoundTrip" else 10000)
    duration = b["real_time"]
    lines.append(f"| {name} | {duration/1000:,.3f} µs | {count*1000/duration:,.3f} million events or pairs/s |")
binary_speedup = previous["BinaryRead"]["real_time"] / medians["BinaryRead"]["real_time"]
lines += ["", "Dispatch/ReplayDispatch are a minimal sequential checksum loop over already prepared memory. They demonstrate dispatch overhead only. ReplayPrepare includes copy, validation, global sequence tracking and sorting. Pipeline includes producer-thread creation/join, bounded transport and checksum over 10,000 events. Backtest includes input validation, the buy-and-hold strategy, execution, ledger and equity collection. Parsing cases include file I/O, validation and vector materialization. These scopes differ and must not be compared as equivalent production operations.", "",
          "## Measured optimization and allocation experiment", "",
          f"BinaryRead changed from {previous['BinaryRead']['real_time']/1e6:.3f} ms to {medians['BinaryRead']['real_time']/1e6:.3f} ms per 10,000 records ({binary_speedup:.2f}× in this run) after replacing bit-at-a-time CRC32 with a constexpr 256-entry IEEE CRC table. The table occupies 1 KiB. Canonical output and CRC were independently checked against Python struct/zlib; no format changed. Baseline raw files are preserved in results/baseline/. This before/after microbenchmark is evidence for this host and workload, not a universal speedup. Linux perf was unavailable, so no sampled CPU profile is claimed.", "",
          "Growing PMR vector (case 0) makes 13 container allocations / 720,808 requested bytes per batch; reserved vector (case 1) makes 1 / 360,448 bytes; the fixed-buffer monotonic arena (case 2) makes 0 upstream allocations. Counts come from an instrumented memory_resource and exclude unrelated process allocations. The extra arena benefit varies across observed runs and has not been established as an engine bottleneck. Chronos retains ordinary preallocated storage instead of introducing a custom allocator into the engine.", "",
          "## Latency distributions and timer limits", "",
          f"100,000 samples after 10,000 warm-ups. On this Windows host the sampler uses QueryPerformanceCounter at {latency['timer_frequency_hz']:,} Hz ({latency['timer_tick_ns']} ns per tick); Linux uses steady_clock. Clock pairs are measured as an instrumentation baseline. Reported nanosecond units are timer conversions, not claims of that timing precision.", "",
          "| Observation | p50 | p90 | p99 | p99.9 |", "|---|---:|---:|---:|---:|"]
for name in ("clock_pair", "spsc_pair", "queue_residence"):
    item=latency[name]
    lines.append(f"| {name} | {item['p50_ns']:g} ns | {item['p90_ns']:g} ns | {item['p99_ns']:g} ns | {item['p999_ns']:g} ns |")
lines += ["", "The clock-pair and single-thread SPSC-pair distributions are indistinguishable at this timer resolution. **Their per-operation latency is unresolved**, including zero-tick observations; zero is not zero execution time. Google Benchmark's batch average is not an independently resolved individual-event latency. Queue residence includes a timestamp call, publication, backlog, OS scheduling and consumer timestamp. Producer retry refreshes the timestamp, so it excludes preceding full-queue wait. It is neither isolated enqueue/dequeue service time nor wire-to-strategy latency. A 100,000-sample p99.9 describes roughly 100 observations in the tail and remains sensitive to desktop scheduling.", "",
          "## Python interface and memory", "",
          f"Empty native backtest convenience call: median {interface['p50_ns_per_call']/1000:.3f} µs/call, p90 {interface['p90_ns_per_call']/1000:.3f} µs/call and p99 {interface['p99_ns_per_call']/1000:.3f} µs/call across 200 batches of 100 calls, after 1,000 warm-ups. This includes Python invocation, GIL transition, configuration snapshot and empty result construction. These are batch-average quantiles, not individual-call latency tails or pure ABI crossing cost.", "",
          f"Benchmark-process observed peak RSS: {memory['benchmark_process_peak_rss_bytes']:,} bytes ({memory['benchmark_process_peak_rss_bytes']/2**20:.2f} MiB). Method: {memory['method']}. It includes Google Benchmark, datasets, experiments and threads. It is not a process-wide allocation count or a large-dataset memory scalability result.", "",
          "## Reproduce", "", "```sh", "PYTHONPATH=python python tools/measure.py --output build/measurements", "```", "",
          "To reproduce the original CRC implementation, configure a separate Release build with `-DCHRONOS_CRC_BASELINE=ON -DCHRONOS_PYTHON=OFF` and run its BinaryRead benchmark. This selects the retained bitwise reference without changing the format. Default builds use the lookup table.", "",
          "Raw context, repetitions, CPU-time values, clock quantiles, Python batches and memory method are preserved under docs/results/. Exact numbers will change with CPU frequency, thermals, compiler, timer, operating system and contention. Large tick datasets, live feed bursts, CPU-pinned cross-core comparisons, perf/Valgrind analysis and hardware-counter studies remain unverified.", ""]
(root / "docs/performance.md").write_text("\n".join(lines),encoding="utf-8",newline="\n")
print("Wrote docs/performance.md from real measurements")
