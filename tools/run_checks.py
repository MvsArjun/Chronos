"""Run the supported local verification workflow and preserve real evidence."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, default=root / "build/release")
parser.add_argument("--output", type=Path, default=root / "docs/results")
args = parser.parse_args()
build, output = args.build.resolve(), args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PYTHONPATH=str(root / "python"), CHRONOS_BUILD=str(build), GTEST_OUTPUT="xml:" + str(output / "cpp-tests.xml"))
suffix = ".exe" if os.name == "nt" else ""
records = []


def run(name, command):
    result = subprocess.run(list(map(str, command)), cwd=root, env=env, text=True, capture_output=True)
    (output / (name + ".txt")).write_text(result.stdout + result.stderr, encoding="utf-8")
    records.append({"name": name, "exit_code": result.returncode, "command": list(map(str, command))})
    print(f"{name}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
    (output / "checks.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(result.returncode)


scratch = build / "check-data"
scratch.mkdir(parents=True, exist_ok=True)
cli = build / ("chronos_cli" + suffix)
run("ctest", ["ctest", "--test-dir", build, "--output-on-failure"])
run("python-tests", [sys.executable, "-m", "unittest", "discover", "-s", root / "tests", "-p", "test_*.py", "-v"])
run("numerical", [sys.executable, root / "tools/validate_numerics.py", "--output", output / "numerical.json"])
run("cli-generate", [cli, "generate", "10000", scratch / "bars.csv"])
run("cli-pack", [cli, "pack", scratch / "bars.csv", scratch / "bars.chr"])
run("cli-replay", [cli, "replay", scratch / "bars.chr"])
run("cli-pipeline", [cli, "pipeline", scratch / "bars.chr"])
if (output / "cli-replay.txt").read_bytes() != (output / "cli-pipeline.txt").read_bytes():
    raise AssertionError("serial and concurrent dispatch differ")
run("cli-backtest", [cli, "backtest", scratch / "bars.chr", "60000000000"])
run("cli-tick", [cli, "backtest-tick", root / "data/samples/ticks.csv"])
run("research-example", [sys.executable, root / "examples/research.py", "--output", scratch / "research"])
