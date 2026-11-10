"""Black-box, platform-neutral tests for v0.9 synthetic trace generator/replayer."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXE = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else None
sys.argv[:] = sys.argv[:1]


class FlowReplayTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="mex_flow_")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def command(self, *args, expect_ok=True):
        result = subprocess.run(
            [EXE, *map(str, args)], capture_output=True, text=True,
            timeout=30, encoding="utf-8", errors="replace",
        )
        if expect_ok:
            self.assertEqual(result.returncode, 0, msg=result.stderr + result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, msg=result.stdout)
        return result

    def generate(self, label, profile="mixed", seed=42, events=1400):
        trace = self.root / f"{label}.csv"
        summary = self.root / f"{label}.json"
        tape = self.root / f"{label}_trades.csv"
        self.command("--generate", trace, "--seed", seed, "--events", events,
                     "--profile", profile, "--summary", summary, "--trades", tape)
        return trace, summary, tape

    def test_mixed_roundtrip_and_exact_tape(self):
        trace, generated, tape = self.generate("mixed")
        replayed = self.root / "replayed.json"
        replay_tape = self.root / "replayed_tape.csv"
        self.command("--replay", trace, "--summary", replayed, "--trades", replay_tape,
                     "--expect-summary", generated)
        self.assertEqual(generated.read_bytes(), replayed.read_bytes())
        self.assertEqual(tape.read_bytes(), replay_tape.read_bytes())
        data = json.loads(generated.read_text(encoding="utf-8"))
        self.assertEqual(data["data_kind"], "synthetic_order_event_trace")
        self.assertEqual(data["total_events"], 1416)
        self.assertEqual(data["event_counts"]["new"] +
                         data["event_counts"]["cancel"] +
                         data["event_counts"]["replace"], 1416)
        self.assertGreater(data["executions"]["trades"], 0)
        self.assertEqual(set(data["regimes"]), {"seed", "calm", "volatile", "stress"})
        self.assertGreater(data["regimes"]["stress"]["aggressive_new"], 0)
        self.assertGreater(data["regimes"]["stress"]["observed_arrivals_per_second"],
                           data["regimes"]["calm"]["observed_arrivals_per_second"])
        self.assertIn("execution_fingerprint_fnv1a64", data)

    def test_determinism_and_different_seed(self):
        one = self.generate("one", seed=918)
        two = self.generate("two", seed=918)
        three = self.generate("three", seed=919)
        for x, y in zip(one, two):
            self.assertEqual(x.read_bytes(), y.read_bytes())
        self.assertNotEqual(one[0].read_bytes(), three[0].read_bytes())
        self.assertNotEqual(one[1].read_bytes(), three[1].read_bytes())

    def test_all_scenarios(self):
        for profile in ("calm", "volatile", "stress"):
            with self.subTest(profile=profile):
                trace, summary, _ = self.generate(profile, profile=profile, seed=7, events=1800)
                data = json.loads(summary.read_text(encoding="utf-8"))
                self.assertEqual(set(data["regimes"]), {"seed", profile})
                self.assertEqual(data["generated_events"], 1800)
                self.command("--replay", trace)

    def test_handwritten_external_trace(self):
        trace = self.root / "external.csv"
        trace.write_text(
            "# MEXFLOW1 seed=0 profile=external synthetic_events=4 seed_events=0\n"
            "ts_us,regime,op,order_id,side,type,price_ticks,quantity,tif\n"
            "0,calm,NEW,1,SELL,LIMIT,10010,5,GTC\n"
            "1,calm,NEW,2,BUY,LIMIT,10010,3,IOC\n"
            "2,calm,REPLACE,1,,,10011,2,\n"
            "3,calm,CANCEL,1,,,,,\n", encoding="utf-8")
        result = self.command("--replay", trace)
        data = json.loads(result.stdout)
        self.assertEqual(data["executions"], {"trades": 1, "quantity": 3})
        self.assertEqual(data["final_book"]["resting_orders"], 0)

    def test_malformed_or_inconsistent_trace_fails(self):
        trace, _, _ = self.generate("valid", seed=3, events=70)
        original = trace.read_text(encoding="utf-8").splitlines(keepends=True)
        # The following corruptions must each produce nonzero exit, never silent acceptance.
        damaged = {
            "header": ["# MEXFLOW999 bogus\n", *original[1:]],
            "missing": original[:-1],
            "extra": [*original, original[-1]],
            "column": [*original[:2], original[2].replace(",NEW,", ",BOGUS,"), *original[3:]],
            "duplicate_id": [*original[:3], original[2], *original[4:]],
            "nonmonotonic": [*original[:19], original[19].replace(original[19].split(',')[0], "0", 1), *original[20:]],
        }
        for name, content in damaged.items():
            with self.subTest(failure=name):
                path = self.root / f"bad_{name}.csv"
                path.write_text("".join(content), encoding="utf-8")
                self.command("--replay", path, expect_ok=False)

    def test_changed_but_valid_trace_detected_by_expected_summary(self):
        trace, summary, _ = self.generate("original", events=60)
        mutated = self.root / "mutated.csv"
        lines = trace.read_text(encoding="utf-8").splitlines()
        # Modify a non-crossing preload price. Trace remains syntactically valid.
        lines[2] = lines[2].replace("9999", "9997", 1)
        mutated.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.command("--replay", mutated, "--expect-summary", summary, expect_ok=False)

    def test_invalid_generate_arguments_rejected(self):
        for args in [
            ("--generate", self.root / "bad.csv", "--events", 0),
            ("--generate", self.root / "bad.csv", "--events", 250001),
            ("--generate", self.root / "bad.csv", "--profile", "real_kraken"),
        ]:
            with self.subTest(args=args):
                self.command(*args, expect_ok=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
