"""Reproducible synthetic measurements. No live exchange latency is inferred."""
import argparse
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
import psutil

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, default=root / "build/release")
parser.add_argument("--output", type=Path, default=root / "docs/results")
args = parser.parse_args()
build, output = args.build.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
suffix = ".exe" if os.name == "nt" else ""
compiler = os.environ.get("CXX", "g++")
model = platform.processor()
if os.name == "nt":
    model = subprocess.check_output(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_Processor).Name"], text=True).strip()
elif Path("/proc/cpuinfo").exists():
    model = next((line.split(":",1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")), model)
environment = {"cpu": model, "physical_cores": psutil.cpu_count(logical=False), "logical_cores": psutil.cpu_count(),
               "ram_bytes": psutil.virtual_memory().total, "os": platform.platform(),
               "compiler": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
               "flags": "CMake Release: -O3 -DNDEBUG; C++20; no native CPU flag",
               "python": platform.python_version(), "dataset_events": 10000,
               "dataset": "deterministic synthetic bars, seed=1, one instrument",
               "clock": "steady_clock/perf_counter; no affinity pinning; desktop background load",
               "memory_method": "10ms RSS sampling; Windows peak_wset if exposed; lower bound otherwise"}
(output / "environment.json").write_text(json.dumps(environment, indent=2) + "\n", encoding="utf-8")
command = [str(build / ("chronos_bench" + suffix)), "--benchmark_min_time=0.1s", "--benchmark_min_warmup_time=0.05", "--benchmark_repetitions=3", "--benchmark_out_format=json", "--benchmark_out=" + str(output / "benchmark.json")]
peak = 0
with (output / "benchmark-console.txt").open("w", encoding="utf-8") as log:
    child = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, cwd=root)
    proc = psutil.Process(child.pid)
    while child.poll() is None:
        try:
            memory = proc.memory_info()
            peak = max(peak, memory.rss, getattr(memory, "peak_wset", 0))
        except psutil.NoSuchProcess:
            pass
        time.sleep(.01)
    if child.returncode:
        raise SystemExit(child.returncode)
(output / "memory.json").write_text(json.dumps({"benchmark_process_peak_rss_bytes": peak, "includes": "Google Benchmark, datasets, allocator experiments, transport threads", "method": environment["memory_method"]}, indent=2) + "\n", encoding="utf-8")
latency = subprocess.check_output([str(build / ("chronos_latency" + suffix))], text=True, cwd=root)
json.loads(latency)
(output / "latency.json").write_text(latency, encoding="utf-8")
sys.path.insert(0, str(root / "python"))
from chronos import _core as c
batch = c.synthetic_bars(0)
function = lambda: c.backtest_buy_hold(batch)
for _ in range(1000):
    function()
samples = []
for _ in range(200):
    start = time.perf_counter_ns()
    for _ in range(100):
        function()
    samples.append((time.perf_counter_ns() - start) / 100)
samples.sort()
interface = {"operation": "empty native backtest call, includes Python call/GIL transition/config copy/result creation", "samples": 200, "calls_per_sample": 100, "p50_ns_per_call": statistics.median(samples), "p90_ns_per_call": samples[179], "p99_ns_per_call": samples[197], "note": "batch averages; not individual-call tail latency or pure ABI cost"}
(output / "python-interface.json").write_text(json.dumps(interface, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"peak_rss_bytes": peak, "python_interface": interface}, indent=2))
