"""Export deterministic ledger values without silently converting to binary floats."""
import csv
import json
from pathlib import Path
from . import _core


def export_report(result, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    p = result.portfolio
    summary = {"schema_version": 1, "money_scale": _core.PRICE_SCALE,
               "cash_fixed": p.cash, "equity_fixed": p.equity, "fees_fixed": p.fees,
               "realized_pnl_fixed": p.realized_pnl, "unrealized_pnl_fixed": p.unrealized_pnl,
               "income_fixed": p.income, "pending_orders": result.pending_orders,
               "positions": {str(k): {"quantity": v.quantity, "cost_basis_fixed": v.cost_basis,
                                     "last_price_fixed": v.last_price}
                             for k, v in p.positions.items()}}
    (directory / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    names = ("time", "equity", "cash", "gross_exposure", "net_exposure")
    with (directory / "equity.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(names)
        writer.writerows([[getattr(point, name) for name in names] for point in result.equity_curve])
    return summary
