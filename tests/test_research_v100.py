"""Black-box integration and fixture tests for the v1.0 research pipeline."""
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXE = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else None
sys.argv[:] = sys.argv[:1]
SIM = Path(__file__).resolve().parents[1] / "sim"
spec = importlib.util.spec_from_file_location("experiment_v10", SIM / "experiment_v10.py")
experiments = importlib.util.module_from_spec(spec)
spec.loader.exec_module(experiments)


class ResearchV100(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix="mex_v100_")
        self.addCleanup(self.dir.cleanup)
        self.root = Path(self.dir.name)

    def cmd(self, *args, ok=True):
        proc = subprocess.run([EXE, *map(str, args)], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=30, text=True)
        if ok:
            self.assertEqual(proc.returncode, 0, proc.stderr)
        else:
            self.assertNotEqual(proc.returncode, 0, proc.stdout)
        return proc

    def test_tuned_metadata_replay_and_legacy_format(self):
        baseline = self.root / "baseline.csv"
        tuned = self.root / "tuned.csv"
        reference = self.root / "tuned.json"
        self.cmd("--generate", baseline, "--events", 250, "--seed", 17)
        self.cmd("--generate", tuned, "--events", 250, "--seed", 17,
                 "--arrival-scale-percent", 150, "--size-scale-percent", 150,
                 "--cancel-shift-points", 10, "--aggression-shift-points", 10,
                 "--summary", reference)
        self.assertTrue(baseline.read_text().startswith("# MEXFLOW2 "))
        self.assertTrue(tuned.read_text().startswith("# MEXFLOW2 "))
        self.assertNotEqual(baseline.read_bytes(), tuned.read_bytes())
        self.cmd("--replay", baseline)
        self.cmd("--replay", tuned, "--expect-summary", reference)
        # v0.9 archived MEXFLOW1 file and old JSON summary must still replay exactly.
        examples = Path(__file__).resolve().parents[1] / "examples"
        self.cmd("--replay", examples / "flow_v090_sample.csv",
                 "--expect-summary", examples / "flow_v090_sample.json")
        data = json.loads(reference.read_text(encoding="utf-8"))
        self.assertEqual(data["synthetic_tuning"]["cancel_shift_points"], 10)
        self.assertIn("mean_best_quote_imbalance", data["quote_metrics"])
        self.assertIn("mean_absolute_midprice_change_bps_per_quote_observation", data["quote_metrics"])

    def test_reject_out_of_range_and_tampered_metadata(self):
        for extra in [
            ("--arrival-scale-percent", 0),
            ("--arrival-scale-percent", 201),
            ("--size-scale-percent", 0),
            ("--cancel-shift-points", 21),
            ("--aggression-shift-points", 21),
        ]:
            with self.subTest(extra=extra):
                self.cmd("--generate", self.root / "bad.csv", "--events", 30,
                         *extra, ok=False)
        trace = self.root / "ok.csv"
        self.cmd("--generate", trace, "--events", 100, "--cancel-shift-points", 10)
        lines = trace.read_text().splitlines()
        lines[0] = lines[0].replace("cancel_shift_points=10", "cancel_shift_points=99")
        trace.write_text("\n".join(lines) + "\n")
        self.cmd("--replay", trace, ok=False)

    def test_multiseed_study_replays_and_reports(self):
        root = self.root / "experiment"
        result = experiments.run_experiment(
            EXE, root, [17, 21, 99], 240, "mixed",
            ["baseline", "arrivals_150pct", "cancels_plus_10pp", "aggression_plus_10pp", "max_size_150pct"],
        )
        self.assertEqual(len(result["runs"]), 15)
        self.assertEqual(result["aggregates"]["baseline"]["runs"], 3)
        self.assertEqual(result["data_kind"], "synthetic_sensitivity_study")
        self.assertIn("Multi-seed Synthetic Sensitivity", (root / "EXPERIMENT_V100.md").read_text())
        self.assertEqual(len((root / "RUNS_V100.csv").read_text().splitlines()), 16)
        self.assertNotEqual(
            result["aggregates"]["baseline"]["metrics"]["cancel_fraction"]["median"],
            result["aggregates"]["cancels_plus_10pp"]["metrics"]["cancel_fraction"]["median"],
        )

    def test_observation_fixture_is_comparison_not_calibration(self):
        fixture = self.root / "observations.json"
        fixture.write_text(json.dumps({
            "schema": "micro_exchange_l2_observations_v1",
            "data_kind": "normalized_aggregate_market_observations",
            "rows": 3, "source_path_basename": "ILLUSTRATIVE_ONLY.csv",
            "spread_ticks": {"mean": 3.5},
            "mean_imbalance_at_best_quote": -0.1,
            "mean_absolute_midprice_change_bps_per_observation": 0.5,
        }), encoding="utf-8")
        result = experiments.run_experiment(
            EXE, self.root / "with_l2", [17, 21], 120, "mixed", ["baseline"], fixture)
        md = (self.root / "with_l2" / "EXPERIMENT_V100.md").read_text()
        self.assertIn("diagnostic side-by-side comparison", md)
        self.assertIn("ILLUSTRATIVE_ONLY.csv", md)
        self.assertEqual(result["l2_reference"]["rows"], 3)
        with self.assertRaises(ValueError):
            experiments.validate_l2({"schema": "micro_exchange_flow_v1"})

    def test_runner_input_validation(self):
        for seeds in ([1, 1], [], [-1]):
            with self.subTest(seeds=seeds), self.assertRaises(ValueError):
                experiments.run_experiment(EXE, self.root / "wrong", seeds, 150,
                                           "mixed", ["baseline"])
        with self.assertRaises(ValueError):
            experiments.run_experiment(EXE, self.root / "wrong", [1], 50,
                                       "mixed", ["baseline"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
