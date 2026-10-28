"""Run from the repository root with PYTHONPATH=python."""
import argparse
from pathlib import Path
import chronos as c
from chronos.parquet import save_parquet, load_parquet
from chronos.report import export_report
from chronos.research import synthetic_research_data, run_factor_research

parser = argparse.ArgumentParser()
parser.add_argument("--output", type=Path, default=Path("build/example-report"))
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
batch = c.synthetic_bars(10_000, seed=7)
save_parquet(args.output / "bars.parquet", batch)
batch = load_parquet(args.output / "bars.parquet")
config = c.ExecutionConfig()
config.bar_duration_ns = 60_000_000_000  # synthetic_bars produces minute intervals
config.commission_per_unit = 10
config.slippage_bps = 1
result = c.run_backtest(batch, c.MovingAverageCross(1, 10), config=config)
summary = export_report(result, args.output / "backtest")
prices, fundamentals = synthetic_research_data()
for factor in ("momentum", "value", "quality", "low_volatility"):
    research = run_factor_research(prices, fundamentals, factor=factor, lookback=20, rebalance_every=10, cost_bps=5)
    research.periods.to_csv(args.output / f"{factor}-returns.csv", index=False)
    research.weights.to_csv(args.output / f"{factor}-weights.csv", index=False)
print(summary)
