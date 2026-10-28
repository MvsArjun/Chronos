import unittest

import numpy as np
import pandas as pd

from chronos.research import (factor_snapshot, form_portfolio, rank_factor,
                              run_factor_research, synthetic_research_data)


class ResearchTests(unittest.TestCase):
    def setUp(self):
        self.prices, self.fundamentals = synthetic_research_data(periods=70, instruments=4)
        self.dates = sorted(self.prices.time.unique())

    def test_future_price_and_fundamental_do_not_change_past_snapshot(self):
        asof = self.dates[25]
        expected = factor_snapshot(self.prices, self.fundamentals, asof, lookback=10)
        changed = self.prices.copy()
        changed.loc[changed.time > asof, "close"] *= 1000
        changed_f = self.fundamentals.copy()
        changed_f.loc[changed_f.available_time > asof, "book_equity"] *= 1e6
        actual = factor_snapshot(changed, changed_f, asof, lookback=10)
        pd.testing.assert_frame_equal(expected, actual)

    def test_late_record_is_unavailable_and_publication_is_enforced(self):
        asof = self.dates[25]
        late = self.prices.copy()
        mask = (late.instrument == "S000") & (late.time == asof)
        late.loc[mask, "available_time"] = self.dates[26]
        result = factor_snapshot(late, self.fundamentals, asof, lookback=10)
        self.assertEqual(result.loc["S000", "last_price_time"], self.dates[24])
        broken = self.fundamentals.copy()
        broken.loc[0, "publication_time"] = pd.Timestamp("2010-01-01", tz="UTC")
        with self.assertRaises(ValueError):
            factor_snapshot(self.prices, broken, asof, lookback=10)

    def test_all_factors_and_future_execution(self):
        for factor in ("momentum", "value", "quality", "low_volatility"):
            with self.subTest(factor=factor):
                result = run_factor_research(self.prices, self.fundamentals, factor=factor, lookback=10, cost_bps=10)
                self.assertFalse(result.periods.empty)
                self.assertTrue((result.weights.execution_time > result.weights.formation_time).all())
                self.assertAlmostEqual(result.periods.iloc[0].turnover, 1)
                self.assertAlmostEqual(result.periods.iloc[0].cost, .001)
                self.assertTrue(np.isfinite(result.periods.net_return).all())
                self.assertAlmostEqual(result.periods.iloc[-1].equity,
                                       float(np.prod(1 + result.periods.net_return)))

    def test_turnover_uses_drifted_weights(self):
        result = run_factor_research(self.prices, self.fundamentals, factor="momentum", lookback=10,
                                     rebalance_every=1, top_fraction=1, cost_bps=0)
        panel = self.prices.pivot(index="time", columns="instrument", values="close")
        first_time = result.periods.iloc[0].time
        next_time = result.periods.iloc[0].end_time
        growth = panel.loc[next_time] / panel.loc[first_time]
        expected_drift = growth / growth.sum()
        expected_turnover = float(np.abs(.25 - expected_drift).sum())
        self.assertAlmostEqual(result.periods.iloc[1].turnover, expected_turnover)

    def test_repeatability_and_costs(self):
        a = run_factor_research(self.prices, self.fundamentals, lookback=10, cost_bps=10)
        b = run_factor_research(self.prices, self.fundamentals, lookback=10, cost_bps=10)
        pd.testing.assert_frame_equal(a.periods, b.periods)
        pd.testing.assert_frame_equal(a.weights, b.weights)
        free = run_factor_research(self.prices, self.fundamentals, lookback=10, cost_bps=0)
        self.assertLess(a.periods.iloc[-1].equity, free.periods.iloc[-1].equity)

    def test_stable_ties_and_missing_values(self):
        snapshot = pd.DataFrame({"momentum": [.2, .2, np.nan]}, index=["B", "A", "C"])
        self.assertEqual(rank_factor(snapshot, "momentum").index.tolist(), ["A", "B"])
        self.assertEqual(form_portfolio(snapshot, "momentum", .5).index.tolist(), ["A"])
        with self.assertRaises(ValueError):
            form_portfolio(pd.DataFrame({"value": [np.nan]}, index=["A"]), "value")

    def test_missing_universe_and_naive_timestamps_fail(self):
        missing = self.prices.drop(index=0)
        with self.assertRaises(ValueError):
            run_factor_research(missing, self.fundamentals, lookback=10)
        naive = self.prices.copy()
        naive["time"] = naive.time.dt.tz_localize(None)
        with self.assertRaises(ValueError):
            factor_snapshot(naive, self.fundamentals, self.dates[25], lookback=10)
        bad_price = self.prices.copy()
        bad_price.loc[0, "close"] = np.nan
        with self.assertRaises(ValueError):
            factor_snapshot(bad_price, self.fundamentals, self.dates[25], lookback=10)

    def test_hand_calculated_delayed_factor_return(self):
        dates = pd.date_range("2020-01-01", periods=8, tz="UTC")
        rows = []
        for symbol, closes in (("A", [100, 110, 121, 1000, 50, 100, 100, 100]),
                               ("B", [100, 100, 100, 100, 100, 100, 100, 100])):
            for t, close in zip(dates, closes):
                rows.append({"instrument": symbol, "time": t, "available_time": t, "close": close})
        result = run_factor_research(pd.DataFrame(rows), factor="momentum", lookback=2, skip=0,
                                     rebalance_every=20, cost_bps=0)
        # Formation at t2 chooses A, execution at t3=1000; first held return t3->t4 is -95%.
        self.assertEqual(result.weights.iloc[0].formation_time, dates[2])
        self.assertEqual(result.periods.iloc[0].time, dates[3])
        self.assertAlmostEqual(result.periods.iloc[0].gross_return, -.95)

    def test_factor_overflow_is_an_explicit_error(self):
        extreme = self.prices.copy()
        mask = extreme.instrument == "S000"
        extreme.loc[mask, "close"] = np.geomspace(1e-300, 1e300, mask.sum())
        with self.assertRaises(ValueError):
            factor_snapshot(extreme, self.fundamentals, self.dates[-1], lookback=68, skip=0)


if __name__ == "__main__":
    unittest.main()
