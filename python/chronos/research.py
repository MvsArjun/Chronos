"""Point-in-time cross-sectional research with delayed close execution.

This module models fractional target weights, not exchange orders. Prices must
be consistently adjusted for corporate actions; execution is one close after
formation. The C++ simulator provides whole-lot order and ledger accounting.
"""
from __future__ import annotations

from dataclasses import dataclass
import math

import numpy as np
import pandas as pd


def _utc(value) -> pd.Timestamp:
    result = pd.Timestamp(value)
    if pd.isna(result) or result.tzinfo is None:
        raise ValueError("timestamps must be finite and timezone-aware")
    return result.tz_convert("UTC")


def _timestamps(frame: pd.DataFrame, columns: tuple[str, ...]) -> None:
    for column in columns:
        frame[column] = pd.DatetimeIndex([_utc(x) for x in frame[column]])


def _prices(prices: pd.DataFrame) -> pd.DataFrame:
    required = {"time", "available_time", "instrument", "close"}
    if not required.issubset(prices.columns) or prices.empty:
        raise ValueError(f"prices require nonempty columns {sorted(required)}")
    result = prices.copy()
    _timestamps(result, ("time", "available_time"))
    if result["instrument"].isna().any():
        raise ValueError("missing instrument")
    result["instrument"] = result["instrument"].astype(str)
    result["close"] = pd.to_numeric(result["close"], errors="raise").astype(float)
    if not np.isfinite(result["close"]).all() or (result["close"] <= 0).any():
        raise ValueError("prices must be positive and finite")
    if (result["available_time"] < result["time"]).any():
        raise ValueError("price available before its event time")
    if result.duplicated(["instrument", "time", "available_time"]).any():
        raise ValueError("ambiguous duplicate price revision")
    return result.sort_values(["instrument", "time", "available_time"], kind="stable")


def _fundamentals(fundamentals: pd.DataFrame | None) -> pd.DataFrame | None:
    if fundamentals is None:
        return None
    required = {"instrument", "period_end", "publication_time", "available_time", "book_equity", "net_income", "shares"}
    if not required.issubset(fundamentals.columns):
        raise ValueError(f"fundamentals require columns {sorted(required)}")
    result = fundamentals.copy()
    if result.empty:
        return result
    _timestamps(result, ("period_end", "publication_time", "available_time"))
    if result["instrument"].isna().any():
        raise ValueError("missing fundamental instrument")
    result["instrument"] = result["instrument"].astype(str)
    if ((result["publication_time"] < result["period_end"]) | (result["available_time"] < result["publication_time"])).any():
        raise ValueError("fundamental dates violate period <= publication <= availability")
    for column in ("book_equity", "net_income", "shares"):
        result[column] = pd.to_numeric(result[column], errors="raise").astype(float)
        if not np.isfinite(result[column]).all():
            raise ValueError("fundamentals must be finite; omit unavailable records")
    if (result["shares"] <= 0).any():
        raise ValueError("shares must be positive")
    if result.duplicated(["instrument", "period_end", "available_time"]).any():
        raise ValueError("ambiguous duplicate fundamental revision")
    return result.sort_values(["instrument", "period_end", "available_time"], kind="stable")


def _snapshot(prices: pd.DataFrame, fundamentals: pd.DataFrame | None, asof: pd.Timestamp,
              lookback: int, skip: int) -> pd.DataFrame:
    visible = prices[(prices["time"] <= asof) & (prices["available_time"] <= asof)]
    # At each historical time use the last revision actually known at this cutoff.
    visible = visible.drop_duplicates(["instrument", "time"], keep="last")
    visible_f = None
    if fundamentals is not None and not fundamentals.empty:
        visible_f = fundamentals[(fundamentals["publication_time"] <= asof) & (fundamentals["available_time"] <= asof)]
        visible_f = visible_f.drop_duplicates(["instrument", "period_end"], keep="last")
        visible_f = visible_f.groupby("instrument", sort=False).tail(1).set_index("instrument")
    rows = []
    for instrument, group in visible.groupby("instrument", sort=True):
        closes = group["close"].to_numpy()
        momentum = volatility = np.nan
        # Momentum spans lookback returns and deliberately skips the most recent skip observations.
        endpoint = len(closes) - 1 - skip
        try:
            with np.errstate(over="raise", divide="raise", invalid="raise"):
                if endpoint >= lookback:
                    momentum = closes[endpoint] / closes[endpoint - lookback] - 1
                if len(closes) > lookback:
                    window = closes[-lookback - 1:]
                    volatility = float(np.std(window[1:] / window[:-1] - 1, ddof=1))
                value = quality = np.nan
                if visible_f is not None and instrument in visible_f.index:
                    record = visible_f.loc[instrument]
                    # Negative book equity has no meaningful book-to-price/ROE rank here.
                    if record["book_equity"] > 0:
                        value = record["book_equity"] / (closes[-1] * record["shares"])
                        quality = record["net_income"] / record["book_equity"]
        except FloatingPointError as error:
            raise ValueError(f"unrepresentable factor calculation for {instrument}") from error
        rows.append({"instrument": instrument, "momentum": momentum, "value": value,
                     "quality": quality, "low_volatility": -volatility,
                     "price_observations": len(closes), "last_price_time": group["time"].iloc[-1]})
    if not rows:
        return pd.DataFrame(columns=["momentum", "value", "quality", "low_volatility", "price_observations", "last_price_time"]).rename_axis("instrument")
    return pd.DataFrame(rows).set_index("instrument")


def factor_snapshot(prices: pd.DataFrame, fundamentals: pd.DataFrame | None, asof,
                    lookback: int = 20, skip: int = 1) -> pd.DataFrame:
    """Return factors using only observations available at ``asof``.

    Momentum is a lookback simple return ending ``skip`` observations ago.
    Value is book equity / (latest close * shares); quality is net income / book
    equity; low volatility is negative sample volatility of lookback returns.
    Missing histories or nonpositive book equity produce explicit NaN factors.
    """
    if not isinstance(lookback, int) or lookback < 2 or not isinstance(skip, int) or skip < 0:
        raise ValueError("lookback >= 2 and skip >= 0 must be integers")
    return _snapshot(_prices(prices), _fundamentals(fundamentals), _utc(asof), lookback, skip)


def rank_factor(snapshot: pd.DataFrame, factor: str) -> pd.Series:
    """Descending ranks with deterministic instrument tie-breaking; missing omitted."""
    if factor not in {"momentum", "value", "quality", "low_volatility"}:
        raise ValueError("unknown factor")
    values = snapshot[factor].dropna()
    if not np.isfinite(values).all():
        raise ValueError("nonfinite factor")
    order = sorted(values.index, key=lambda instrument: (-values.loc[instrument], str(instrument)))
    return pd.Series(range(1, len(order) + 1), index=order, name="rank", dtype="int64")


def form_portfolio(snapshot: pd.DataFrame, factor: str, top_fraction: float = .5) -> pd.Series:
    """Equal-weight long-only top bucket, gross weight one; empty universe fails."""
    if not math.isfinite(top_fraction) or not 0 < top_fraction <= 1:
        raise ValueError("top_fraction must be in (0, 1]")
    ranks = rank_factor(snapshot, factor)
    if ranks.empty:
        raise ValueError("no eligible factor observations at formation")
    count = max(1, math.ceil(len(ranks) * top_fraction))
    selected = ranks.index[:count]
    return pd.Series(1 / count, index=selected, name="weight", dtype=float)


@dataclass
class ResearchResult:
    periods: pd.DataFrame
    weights: pd.DataFrame


def run_factor_research(prices: pd.DataFrame, fundamentals: pd.DataFrame | None = None,
                        *, factor: str = "momentum", lookback: int = 20, skip: int = 1,
                        rebalance_every: int = 5, top_fraction: float = .5,
                        cost_bps: float = 10, start=None) -> ResearchResult:
    """Form at a known close, execute at the next close, then earn future returns.

    Fixed, rectangular universe and unique final prices are required. Delisted
    assets/missing prices fail explicitly rather than disappearing. The supplied
    universe can still have survivorship bias; this routine cannot infer listings.
    Cost is a linear approximation: bps * sum(abs(target - drifted_weights)),
    charged before the next holding interval. Fractional post-cost target weights
    are assumed attainable. Cash earns zero. All timestamps must be timezone aware.
    """
    if not isinstance(lookback, int) or lookback < 2 or not isinstance(skip, int) or skip < 0:
        raise ValueError("lookback >= 2 and skip >= 0 must be integers")
    if not isinstance(rebalance_every, int) or rebalance_every < 1:
        raise ValueError("rebalance_every must be a positive integer")
    if not math.isfinite(cost_bps) or cost_bps < 0 or cost_bps >= 5000:
        raise ValueError("cost_bps must lie in [0, 5000)")
    validated = _prices(prices)
    fundamental_data = _fundamentals(fundamentals)
    # The fill-price panel must have one final observation per asset/date. Factor
    # snapshots still respect each record's original availability timestamp.
    if validated.duplicated(["instrument", "time"]).any():
        raise ValueError("research execution panel requires one price per instrument/time")
    panel = validated.pivot(index="time", columns="instrument", values="close").sort_index()
    if panel.isna().any().any():
        raise ValueError("research requires rectangular universe; missing/delisted prices need an explicit policy")
    dates = panel.index
    if len(dates) < lookback + skip + 3:
        raise ValueError("insufficient data for formation, delayed execution and holding")
    first_formation = lookback + skip
    if start is not None:
        first_formation = max(first_formation, int(dates.searchsorted(_utc(start))))
    if first_formation + 2 >= len(dates):
        raise ValueError("no holding period remains after delayed execution")
    formation_indices = set(range(first_formation, len(dates) - 2, rebalance_every))
    current = pd.Series(0.0, index=panel.columns)
    equity = 1.0
    rows, weight_rows = [], []
    for k in range(first_formation + 1, len(dates) - 1):
        turnover = cost = 0.0
        formation_time = pd.NaT
        eligible = 0
        if k - 1 in formation_indices:
            formation_time = dates[k - 1]
            snapshot = _snapshot(validated, fundamental_data, formation_time, lookback, skip)
            target = form_portfolio(snapshot, factor, top_fraction).reindex(panel.columns, fill_value=0.0)
            eligible = len(rank_factor(snapshot, factor))
            turnover = float(np.abs(target - current).sum())
            cost = turnover * cost_bps / 10_000
            if cost >= 1:
                raise ValueError("transaction costs exhaust portfolio equity")
            current = target
            for instrument, weight in target.items():
                weight_rows.append({"formation_time": formation_time, "execution_time": dates[k],
                                    "instrument": instrument, "weight": weight})
        with np.errstate(over="raise", divide="raise", invalid="raise"):
            try:
                asset_returns = panel.iloc[k + 1] / panel.iloc[k] - 1
            except FloatingPointError as error:
                raise ValueError("unrepresentable holding-period return") from error
        gross = float(current.dot(asset_returns))
        net = (1 - cost) * (1 + gross) - 1
        if not math.isfinite(net) or 1 + gross <= 0:
            raise ValueError("invalid portfolio growth")
        equity *= 1 + net
        if not math.isfinite(equity):
            raise ValueError("portfolio equity overflow")
        rows.append({"time": dates[k], "end_time": dates[k + 1], "formation_time": formation_time,
                     "gross_return": gross, "turnover": turnover, "cost": cost,
                     "net_return": net, "equity": equity, "eligible_count": eligible})
        current = current * (1 + asset_returns) / (1 + gross)
    return ResearchResult(pd.DataFrame(rows), pd.DataFrame(weight_rows))


def synthetic_research_data(periods: int = 100, instruments: int = 6, seed: int = 7) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Deterministic synthetic fixtures, with explicit publication and availability."""
    if periods < 5 or instruments < 1:
        raise ValueError("need at least five periods and one instrument")
    rng = np.random.default_rng(seed)
    dates = pd.date_range("2020-01-02 16:00:00", periods=periods, freq="B", tz="UTC")
    price_rows, fundamental_rows = [], []
    for j in range(instruments):
        instrument = f"S{j:03d}"
        values = (40 + j * 10) * np.exp(np.cumsum(rng.normal(.0003 * (j + 1), .01 + j * .001, periods)))
        for date, close in zip(dates, values):
            price_rows.append({"instrument": instrument, "time": date, "available_time": date, "close": close})
        for period_end, publication_time in ((pd.Timestamp("2019-09-30", tz="UTC"), dates[0]),
                                              (pd.Timestamp("2019-12-31", tz="UTC"), dates[min(40, periods - 1)])):
            fundamental_rows.append({"instrument": instrument, "period_end": period_end,
                                     "publication_time": publication_time, "available_time": publication_time,
                                     "book_equity": 1_000_000 * (j + 1), "net_income": 80_000 * (j + 1) ** 1.1,
                                     "shares": 100_000 + 5_000 * j})
    return pd.DataFrame(price_rows), pd.DataFrame(fundamental_rows)
