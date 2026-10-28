import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from chronos import _core as c

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("CHRONOS_BUILD", ROOT / "build/release"))
CLI = BUILD / ("chronos_cli.exe" if os.name == "nt" else "chronos_cli")


@unittest.skipUnless(CLI.exists(), "Build the C++ CLI to run CLI regression tests")
class CliTests(unittest.TestCase):
    def test_bar_backtest_requires_explicit_duration(self):
        result = subprocess.run([str(CLI), "backtest", "unused.csv"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("BAR_DURATION_NS", result.stderr)

    def test_rejected_order_is_not_reported_as_fill_and_metadata_is_unspecified(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            price = 10**12
            batch = c.EventBatch([c.Event(c.EventHeader(i,i,i,i,1), c.Bar(price,price,price,price,100)) for i in (1,2,3)])
            c.save_csv(str(path / "data.csv"), batch)
            result = subprocess.run([str(CLI), "backtest", str(path / "data.csv"), "1"], capture_output=True, text=True, check=True)
            report = json.loads(result.stdout)
            self.assertEqual(report["fills"], 0)
            self.assertEqual(report["execution_reports"], 1)
            subprocess.run([str(CLI), "pack", str(path / "data.csv"), str(path / "data.chr")], capture_output=True, check=True)
            meta = json.loads(c.load_binary(str(path / "data.chr")).metadata)
            self.assertEqual(meta["currency"], "unspecified")
            self.assertEqual(meta["adjustment"], "unspecified")

if __name__ == "__main__": unittest.main()
