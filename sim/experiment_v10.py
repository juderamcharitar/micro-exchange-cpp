#!/usr/bin/env python3
"""Reproducible multi-seed sensitivity experiment for the synthetic C++ flow generator.

This is simulation diagnostics, NOT empirical calibration. Optional normalized
L2 observation summaries are compared only on *comparable* quote-level fields.
No order IDs, venue cancellation rates or queue position are inferred from L2.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
from hashlib import sha256
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys

SCENARIOS = {
    "baseline": {},
    "arrivals_150pct": {"--arrival-scale-percent": 150},
    "cancels_plus_10pp": {"--cancel-shift-points": 10},
    "aggression_plus_10pp": {"--aggression-shift-points": 10},
    "max_size_150pct": {"--size-scale-percent": 150},
}
METRICS = (
    "simulated_event_rate_per_second", "executions_per_event", "executed_quantity_per_event", "mean_spread_ticks",
    "mean_depth5_imbalance", "mean_best_quote_imbalance",
    "mean_absolute_midprice_change_bps_per_quote_observation",
    "cancel_fraction", "aggressive_new_fraction",
)


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def run_command(argv):
    completed = subprocess.run([str(x) for x in argv], capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=120)
    if completed.returncode:
        raise RuntimeError(f"command failed ({completed.returncode}): {argv}\n"
                           f"stderr: {completed.stderr[-2500:]}\nstdout: {completed.stdout[-2500:]}")


def extract(data):
    if data.get("schema") != "micro_exchange_flow_v1" or data.get("data_kind") != "synthetic_order_event_trace":
        raise ValueError("expected synthetic micro_exchange_flow_v1 summary")
    n = data["generated_events"]
    if not isinstance(n, int) or n <= 0:
        raise ValueError("invalid generated event count")
    q = data["quote_metrics"]
    counts = data["event_counts"]
    # Approximate simulated arrival rate from phase spans (boundary gaps omitted).
    phase_span = sum(v["span_us"] for k, v in data["regimes"].items()
                     if k in ("calm", "volatile", "stress"))
    return {
        "simulated_event_rate_per_second": n * 1e6 / phase_span if phase_span > 0 else None,
        "executions_per_event": data["executions"]["trades"] / n,
        "executed_quantity_per_event": data["executions"]["quantity"] / n,
        "mean_spread_ticks": q["mean_spread_ticks"],
        "mean_depth5_imbalance": q["mean_depth5_imbalance"],
        "mean_best_quote_imbalance": q.get("mean_best_quote_imbalance"),
        "mean_absolute_midprice_change_bps_per_quote_observation": q.get(
            "mean_absolute_midprice_change_bps_per_quote_observation"),
        "cancel_fraction": counts["cancel"] / n,
        "aggressive_new_fraction": (counts["market_ioc"] + counts["limit_ioc"]) / n,
    }


def summarise(records):
    result = {}
    for scenario in SCENARIOS:
        sample = [x for x in records if x["scenario"] == scenario]
        if not sample:
            continue
        result[scenario] = {"runs": len(sample), "metrics": {}}
        for name in METRICS:
            values = [r["metrics"][name] for r in sample if r["metrics"][name] is not None]
            result[scenario]["metrics"][name] = (
                {"median": statistics.median(values), "min": min(values), "max": max(values)}
                if values else None
            )
    return result


def validate_l2(data):
    if data.get("schema") != "micro_exchange_l2_observations_v1" or \
            data.get("data_kind") != "normalized_aggregate_market_observations":
        raise ValueError("--l2-summary must come from inspect_l2.py normalized observation tool")
    if data.get("rows", 0) < 2:
        raise ValueError("L2 summary requires at least two observations")
    if data["spread_ticks"]["mean"] is None:
        raise ValueError("L2 summary missing spread mean")
    return {
        "source_basename": data.get("source_path_basename", "unavailable"),
        "rows": data["rows"],
        "mean_spread_ticks": data["spread_ticks"]["mean"],
        "mean_best_quote_imbalance": data["mean_imbalance_at_best_quote"],
        "mean_absolute_midprice_change_bps_per_quote_observation":
            data["mean_absolute_midprice_change_bps_per_observation"],
    }


def fmt(val, digits=3):
    return "n/a" if val is None else f"{val:,.{digits}f}"


def report(payload):
    aggregates = payload["aggregates"]
    lines = [
        "# Micro Exchange v1.0 — Multi-seed Synthetic Sensitivity Study", "",
        f"- **Host:** {payload['host']}",
        f"- **Events per run:** {payload['events_per_run']:,} (plus 16 preload events)",
        f"- **Regime profile:** {payload['profile']}",
        f"- **Seeds:** {', '.join(str(s) for s in payload['seeds'])}",
        f"- **Executable SHA-256:** `{payload['executable_sha256']}`",
        f"- **Replay:** exact generated/replayed summaries and trade tapes verified for {len(payload['runs'])} runs", "",
        "All inputs are **synthetic**. Each number below is the median across independent seeded",
        "runs, not a confidence interval or a production performance benchmark. The scenario",
        "changes are deliberately imposed assumptions, not parameters fitted from market data.", "",
        "## Scenario comparisons", "",
        "| Scenario | Simulated events/s | Fill records / event | Executed qty / event | Mean spread (ticks) | Mean L1 imbalance | Cancellation share | Aggressive NEW share |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for scenario, row in aggregates.items():
        m = row["metrics"]
        def md(key, digits=3):
            return fmt(m[key]["median"] if m[key] else None, digits)
        lines.append(f"| {scenario.replace('_', ' ')} | {md('simulated_event_rate_per_second',0)} | {md('executions_per_event')} | "
                     f"{md('executed_quantity_per_event')} | {md('mean_spread_ticks')} | "
                     f"{md('mean_best_quote_imbalance',4)} | {md('cancel_fraction',4)} | "
                     f"{md('aggressive_new_fraction',4)} |")
    lines += ["", "**Simulated events/s** is derived from generated timestamp spans, not timed engine throughput.",
              "Because arrivals scale only event timestamps, fixed-event-count matching and fill metrics",
              "remain unchanged under the arrivals-only scenario for a given seed.", "",
              "### Dispersion across seeds", "",
              "Ranges below are observed **minimum–maximum across seeds**, not statistical confidence intervals.", "",
              "| Scenario | Fill/event range | Spread mean range (ticks) | Mean depth-5 imbalance range |",
              "|---|---:|---:|---:|"]
    for scenario, row in aggregates.items():
        m = row["metrics"]
        def rr(key, digits=3):
            v = m[key]
            return "n/a" if v is None else f"{fmt(v['min'],digits)}–{fmt(v['max'],digits)}"
        lines.append(f"| {scenario.replace('_', ' ')} | {rr('executions_per_event')} | "
                     f"{rr('mean_spread_ticks')} | {rr('mean_depth5_imbalance',4)} |")

    if payload["l2_reference"]:
        ref = payload["l2_reference"]
        lines += ["", "## Optional aggregate-market observation comparison", "",
                  f"Observation source: `{ref['source_basename']}` ({ref['rows']:,} normalized rows).",
                  "**This is a diagnostic side-by-side comparison, not a fitted calibration.**",
                  "Comparable definitions require the same tick size, instrument, units, sampling",
                  "method and market time window; those conditions are NOT verified automatically.",
                  "The simulator samples quotes after individual synthetic events, whereas the",
                  "reference samples external quote observations. Differences reflect that as well",
                  "as differences in price-placement and event-flow assumptions.", "",
                  "| Observable | Normalized L2 | Baseline synthetic median | Difference (synthetic − L2) |",
                  "|---|---:|---:|---:|"]
        for display, key in [
            ("Mean quoted spread (ticks)", "mean_spread_ticks"),
            ("Mean imbalance at best quote", "mean_best_quote_imbalance"),
            ("Mean absolute mid-change (bps / observation)", "mean_absolute_midprice_change_bps_per_quote_observation")]:
            orig = ref[key]
            synthetic = aggregates["baseline"]["metrics"][key]
            val = None if synthetic is None else synthetic["median"]
            diff = val-orig if val is not None and orig is not None else None
            lines.append(f"| {display} | {fmt(orig,4)} | {fmt(val,4)} | {fmt(diff,4)} |")
        lines += ["", "**Never inferred:** order IDs, queue positions, individual cancellations,",
                  "true aggressive-order fractions, or market-event arrival rates from aggregate L2."]
    lines += ["", "## Interpretation and scope", "",
              "- Fill records are individual matching-engine executions; several can arise from one taker order.",
              "- Generated event timestamps are simulated interarrival times; throughput is NOT measured here.",
              "- The matching engine's in-memory simulator omits WAL, TCP, margin, fees and live venue execution.",
              "- The replay check establishes exact trace reproducibility for each generated run on this runtime.",
              "- Results depend on seeded simulation and a deliberately simple event-generation process.",
              "- Observational calibration would require separately normalized, source-verified data",
              "  and matched observation rules; no real Kraken data is bundled with this release.", ""]
    return "\n".join(lines)


def run_experiment(executable, output_dir, seeds, events, profile, scenarios, l2_summary=None):
    executable = Path(executable).resolve()
    if not executable.is_file():
        raise ValueError(f"compiled exchange_flow_sim executable not found: {executable}")
    if not seeds or len(seeds) != len(set(seeds)) or any(s < 0 or s > (1 << 64)-1 for s in seeds):
        raise ValueError("seeds must be distinct unsigned 64-bit integers")
    if not (100 <= events <= 250000):
        raise ValueError("--events must be 100..250000")
    if profile not in ("mixed", "calm", "volatile", "stress"):
        raise ValueError("unknown profile")
    if not scenarios or "baseline" not in scenarios or any(s not in SCENARIOS for s in scenarios):
        raise ValueError("scenarios must include baseline and use known names")
    out = Path(output_dir).resolve()
    out.mkdir(parents=True, exist_ok=True)
    l2 = validate_l2(read_json(l2_summary)) if l2_summary else None
    records = []
    for scenario in scenarios:
        subdir = out / scenario
        subdir.mkdir(exist_ok=True)
        extra = [z for flag, value in SCENARIOS[scenario].items() for z in (flag, str(value))]
        for seed in seeds:
            base = f"seed_{seed}"
            trace = subdir / f"{base}.csv"
            generated = subdir / f"{base}.json"
            tape = subdir / f"{base}_trades.csv"
            replay_tape = subdir / f"{base}_replayed_trades.csv"
            run_command([executable, "--generate", trace, "--seed", seed,
                         "--events", events, "--profile", profile,
                         "--summary", generated, "--trades", tape, *extra])
            run_command([executable, "--replay", trace, "--expect-summary", generated,
                         "--trades", replay_tape])
            if tape.read_bytes() != replay_tape.read_bytes():
                raise RuntimeError(f"trade tape differs after replay for {scenario}/{base}")
            summary = read_json(generated)
            if summary["generated_events"] != events or summary["seed"] != seed:
                raise RuntimeError("summary metadata does not match requested run")
            metrics = extract(summary)
            records.append({"scenario": scenario, "seed": seed, "metrics": metrics,
                            "fingerprint": summary["execution_fingerprint_fnv1a64"],
                            "tape_sha256": sha256(tape.read_bytes()).hexdigest(),
                            "journal_mode": "not_applicable_in_memory"})
            print(f"verified {scenario} seed={seed} fills={summary['executions']['trades']} "
                  f"mean_spread={metrics['mean_spread_ticks']}", flush=True)
    payload = {
        "schema": "micro_exchange_experiment_v1", "data_kind": "synthetic_sensitivity_study",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "host": f"{platform.system()} {platform.release()} ({platform.machine()})",
        "python": platform.python_version(), "executable_basename": executable.name,
        "executable_sha256": sha256(executable.read_bytes()).hexdigest(),
        "events_per_run": events, "profile": profile, "seeds": seeds,
        "scenarios": {name: SCENARIOS[name] for name in scenarios},
        "runs": records, "aggregates": summarise(records), "l2_reference": l2,
    }
    (out / "EXPERIMENT_V100.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    (out / "EXPERIMENT_V100.md").write_text(report(payload), encoding="utf-8")
    with (out / "RUNS_V100.csv").open("w", newline="", encoding="utf-8") as fp:
        writer = csv.DictWriter(fp, fieldnames=["scenario", "seed", *METRICS, "fingerprint", "tape_sha256"])
        writer.writeheader()
        for r in records:
            writer.writerow({"scenario": r["scenario"], "seed": r["seed"],
                             **r["metrics"], "fingerprint": r["fingerprint"],
                             "tape_sha256": r["tape_sha256"]})
    return payload


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--executable", required=True, help="path to built exchange_flow_sim(.exe)")
    p.add_argument("--output-dir", required=True)
    p.add_argument("--seeds", default="11,23,37,41,53", help="comma-separated independent seeds")
    p.add_argument("--events", type=int, default=3000)
    p.add_argument("--profile", choices=["mixed", "calm", "volatile", "stress"], default="mixed")
    p.add_argument("--scenarios", default=",".join(SCENARIOS),
                   help="comma-separated scenario names including baseline")
    p.add_argument("--l2-summary", help="optional JSON from inspect_l2.py; comparison only, no parameter fit")
    args = p.parse_args()
    seeds = [int(v.strip()) for v in args.seeds.split(",")]
    scenarios = [x.strip() for x in args.scenarios.split(",")]
    run_experiment(args.executable, args.output_dir, seeds, args.events,
                   args.profile, scenarios, args.l2_summary)
    print(f"report={Path(args.output_dir).resolve() / 'EXPERIMENT_V100.md'}")


if __name__ == "__main__":
    main()
