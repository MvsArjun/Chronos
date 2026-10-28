"""Execute the GCC/Clang release and separate sanitizer verification matrix."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def source_digest(root):
    files = []
    for folder in ("include", "src", "python", "tests", "tools", ".github"):
        files.extend(p for p in (root / folder).rglob("*")
                     if p.is_file() and "__pycache__" not in p.parts
                     and p.suffix not in {".so", ".pyd", ".pyc"})
    files.extend(root / name for name in ("CMakeLists.txt", "CMakePresets.json", "requirements.txt", "pyproject.toml"))
    digest = hashlib.sha256()
    for path in sorted(set(files)):
        digest.update(path.relative_to(root).as_posix().encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def main():
    if platform.system() != "Linux":
        raise SystemExit("This matrix requires a Linux host or guest")
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=root / "docs/results/pre-upload/linux")
    parser.add_argument("--deps", type=Path, help="Optional extracted pinned dependency directory")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, PYTHONPATH=str(root / "python"),
               ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1", TSAN_OPTIONS="halt_on_error=1")
    rows = []
    evidence = {"source_sha256": source_digest(root), "system": platform.platform(),
                "python": platform.python_version(), "matrix": rows}

    def run(name, command, destination):
        destination.mkdir(parents=True, exist_ok=True)
        try:
            process = subprocess.run(list(map(str, command)), cwd=root, env=env, text=True,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     timeout=900 if name == "build" else 300)
            code, log = process.returncode, process.stdout
        except subprocess.TimeoutExpired as error:
            partial = error.stdout or ""
            log = (partial.decode(errors="replace") if isinstance(partial, bytes) else partial) + "\nStep timed out.\n"
            code = 124
        (destination / (name + ".txt")).write_text(log, encoding="utf-8")
        print(f"{destination.name}/{name}: {'PASS' if code == 0 else 'FAIL'}", flush=True)
        if code:
            print(log[-12000:], flush=True)
        return code

    def save():
        (out / "matrix.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")

    for label, compiler in (("gcc", "g++"), ("clang", "clang++")):
        resolved = shutil.which(compiler)
        for preset, sanitizer in (("release", "none"), ("asan-ubsan", "address-undefined"), ("tsan", "thread")):
            entry = {"compiler": label, "configuration": preset, "passed": False}
            rows.append(entry)
            destination = out / (label + "-" + preset)
            if not resolved:
                entry["error"] = f"{compiler} unavailable"
                save()
                continue
            env["CXX"] = resolved
            entry["version"] = subprocess.check_output([resolved, "--version"], text=True).splitlines()[0]
            build = root / "build" / ("verify-" + label + "-" + preset)
            release = preset == "release"
            command = ["cmake", "-S", root, "-B", build, "-G", "Ninja",
                       "-DCMAKE_CXX_COMPILER=" + resolved,
                       "-DCMAKE_BUILD_TYPE=" + ("Release" if release else "Debug"),
                       "-DCHRONOS_SANITIZER=" + sanitizer,
                       "-DCHRONOS_PYTHON=" + ("ON" if release else "OFF"),
                       "-DCHRONOS_BENCHMARKS=" + ("ON" if release else "OFF"),
                       "-DPython_EXECUTABLE=" + sys.executable]
            if args.deps:
                command += ["-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=" + str(args.deps.resolve() / "googletest-1.16.0"),
                            "-DFETCHCONTENT_SOURCE_DIR_GOOGLEBENCHMARK=" + str(args.deps.resolve() / "benchmark-1.9.2")]
            entry["configure_exit"] = run("configure", command, destination)
            if entry["configure_exit"]:
                save()
                continue
            entry["build_exit"] = run("build", ["cmake", "--build", build, "-j", "2"], destination)
            if entry["build_exit"]:
                save()
                continue
            if release:
                command = [sys.executable, root / "tools/run_checks.py", "--build", build, "--output", destination]
            else:
                env["GTEST_OUTPUT"] = "xml:" + str(destination / "cpp-tests.xml")
                command = ["ctest", "--test-dir", build, "--output-on-failure"]
            entry["tests_exit"] = run("verification", command, destination)
            env.pop("GTEST_OUTPUT", None)
            if release and entry["tests_exit"] == 0:
                entry["benchmark_exit"] = run("measure", [sys.executable, root / "tools/measure.py", "--build", build,
                                                           "--output", destination / "performance"], destination)
            entry["passed"] = entry["tests_exit"] == 0 and entry.get("benchmark_exit", 0) == 0
            save()
    evidence["source_unchanged"] = evidence["source_sha256"] == source_digest(root)
    evidence["passed"] = len(rows) == 6 and all(row["passed"] for row in rows) and evidence["source_unchanged"]
    save()
    raise SystemExit(0 if evidence["passed"] else 1)


if __name__ == "__main__":
    main()
