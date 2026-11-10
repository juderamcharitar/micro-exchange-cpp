"""Test v0.9 synthetic report and normalized-L2 observation tools."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

SRC = Path(__file__).resolve().parents[1] / "sim"


def module(name):
    spec = importlib.util.spec_from_file_location(name, SRC / (name + ".py"))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


inspect_l2 = module("inspect_l2")
report_flow = module("report_flow")


class FlowTools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="mex_flow_tools_")
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def test_l2_metrics_and_original_row_count(self):
        source = self.dir / "normalized.csv"
        source.write_text(
            "timestamp_us,best_bid_ticks,best_ask_ticks,bid_qty,ask_qty,trade_side,trade_qty\n"
            "1000000,10000,10002,12,8,NONE,0\n"
            "1001000,10000,10004,8,12,BUY,2.5\n"
            "1002000,10000,10002,12,8,SELL,1.5\n", encoding="utf-8")
        data = inspect_l2.inspect(source)
        self.assertEqual(data["rows"], 3)
        self.assertEqual(data["spread_ticks"]["median"], 2)
        self.assertEqual(data["spread_ticks"]["p99_nearest_rank"], 4)
        self.assertEqual(data["tagged_trade_observations"], 2)
        self.assertEqual(data["buyer_initiated_trade_quantity"], 2.5)
        self.assertEqual(data["seller_initiated_trade_quantity"], 1.5)
        self.assertEqual(data["observation_rate_per_second"], 1000.0)
        self.assertEqual(data["source_path_basename"], "normalized.csv")
        self.assertIn("Aggregate L2", data["limitation"])

    def test_l2_rejects_crossed_invalid_and_nan(self):
        for row in [
            "100,10010,10009,1,1,NONE,0\n",
            "100,10001,10002,nan,1,NONE,0\n",
            "100,10001,10002,1,1,NONE,2\n",
            "100,10001,10002,1,1,BOGUS,2\n",
        ]:
            with self.subTest(row=row):
                path = self.dir / "bad.csv"
                path.write_text(",".join(inspect_l2.FIELDS) + "\n" + row, encoding="utf-8")
                with self.assertRaises(ValueError):
                    inspect_l2.inspect(path)

    def test_l2_rejects_decreasing_time(self):
        path = self.dir / "bad_clock.csv"
        path.write_text(",".join(inspect_l2.FIELDS) + "\n"
                        "200,10001,10002,1,1,NONE,0\n"
                        "100,10001,10002,1,1,NONE,0\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            inspect_l2.inspect(path)

    def test_report_accepts_synthetic_only(self):
        p = self.dir / "summary.json"
        p.write_text(json.dumps({
            "schema": "micro_exchange_flow_v1",
            "data_kind": "synthetic_order_event_trace",
            "profile": "stress", "seed": 10, "seed_events": 16,
            "generated_events": 10, "total_events": 26,
            "event_counts": {"new": 20, "cancel": 4, "replace": 2, "market_ioc": 3},
            "executions": {"trades": 4, "quantity": 15},
            "execution_fingerprint_fnv1a64": "0xdeadbeef",
            "regimes": {"stress": {"events": 10, "observed_arrivals_per_second": 12000,
                                    "aggressive_new": 3, "trades": 4, "quantity": 15,
                                    "quotes": {"mean_spread_ticks": 8,
                                               "mean_depth5_imbalance": 0.1}}}
        }), encoding="utf-8")
        md = report_flow.build([p])
        self.assertIn("synthetic", md.lower())
        self.assertIn("0xdeadbeef", md)
        self.assertIn("12,000", md)
        p.write_text(json.dumps({"schema": "micro_exchange_l2_observations_v1"}))
        with self.assertRaises(ValueError):
            report_flow.build([p])


if __name__ == "__main__":
    unittest.main(verbosity=2)
