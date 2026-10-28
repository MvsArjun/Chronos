import gc
import json
from pathlib import Path
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from chronos import _core as c
from chronos.report import export_report


class IntegrationTests(unittest.TestCase):
    def test_replay_storage_backtest_and_report_repeatability(self):
        batch = c.synthetic_bars(1000, 7)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            c.save_binary(str(path / "data.chr"), batch, '{"seed":7}')
            loaded = c.load_binary(str(path / "data.chr"))
            self.assertEqual(loaded.metadata, '{"seed":7}')
            self.assertEqual(c.replay_checksum(batch), c.replay_checksum(loaded.events))
            a = c.backtest_buy_hold(batch)
            b = c.backtest_buy_hold(loaded.events)
            self.assertEqual(export_report(a, path / "a"), export_report(b, path / "b"))
            self.assertEqual((path / "a/equity.csv").read_bytes(), (path / "b/equity.csv").read_bytes())
            self.assertEqual(a.portfolio.positions[1].quantity, 10)

    def test_python_callback_ownership_survives_backtest_and_replay(self):
        class Strategy(c.Strategy):
            def __init__(self):
                super().__init__()
                self.saved = []
            def on_event(self, event, portfolio):
                self.saved.append((event, portfolio))
                return [c.OrderRequest(1, 1, c.Side.BUY, c.OrderType.MARKET, 1)] if len(self.saved) == 1 else []
        strategy = Strategy()
        result = c.run_backtest(c.synthetic_bars(5), strategy)
        gc.collect()
        self.assertEqual(strategy.saved[0][1].cash, 1_000_000 * c.PRICE_SCALE)
        self.assertLess(strategy.saved[-1][1].cash, strategy.saved[0][1].cash)
        saved = []
        c.replay(c.synthetic_bars(3), saved.append)
        gc.collect()
        self.assertEqual(saved[0].header.sequence, 1)
        self.assertEqual(result.portfolio.positions[1].quantity, 1)

    def test_batch_items_are_copies_and_native_runs_are_independent(self):
        batch = c.synthetic_bars(100)
        item = batch[0]
        item.header.sequence = 999
        self.assertEqual(batch[0].header.sequence, 1)
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(lambda _: c.backtest_buy_hold(batch).portfolio.cash, range(8)))
        self.assertEqual(len(set(results)), 1)

    def test_python_strategy_exceptions_propagate(self):
        class Broken(c.Strategy):
            def on_event(self, event, portfolio):
                raise ValueError("strategy failure")
        with self.assertRaisesRegex(ValueError, "strategy failure"):
            c.run_backtest(c.synthetic_bars(3), Broken())

    def test_configuration_is_snapshotted_before_callbacks(self):
        config = c.ExecutionConfig()
        class Mutating(c.Strategy):
            def __init__(self):
                super().__init__()
                self.sent = False
            def on_event(self, event, portfolio):
                config.fixed_fee = 999
                if self.sent:
                    return []
                self.sent = True
                return [c.OrderRequest(1,1,c.Side.BUY,c.OrderType.MARKET,1)]
        result = c.run_backtest(c.synthetic_bars(3), Mutating(), config=config)
        self.assertEqual(result.portfolio.fees, 0)

if __name__ == "__main__": unittest.main()
