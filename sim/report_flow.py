#!/usr/bin/env python3
"""Produce a small, source-labeled Markdown report from v1.0 flow summaries.
No inference about real markets; all inputs must be synthetic flow summaries.
"""
import argparse
import json
from pathlib import Path


def load(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if (data.get("schema") != "micro_exchange_flow_v1"
            or data.get("data_kind") != "synthetic_order_event_trace"):
        raise ValueError(f"not a v1.0 synthetic flow summary: {path}")
    return data


def fmt(value, digits=2):
    return "n/a" if value is None else f"{value:,.{digits}f}"


def build(paths):
    blocks = [
        "# Micro Exchange v1.0 — Synthetic Order-Flow Research",
        "",
        "**Input type:** Generated synthetic order-event CSV replay summaries only",
        "**Important:** The generated datasets are synthetic; outputs are NOT empirical market metrics.",
        "The C++ trace replayer does not infer individual order IDs from aggregate L2 snapshots.",
        "",
    ]
    for path in paths:
        data = load(path)
        events = data["event_counts"]
        x = data["executions"]
        blocks += [
            f"## {data['profile']} (seed {data['seed']}, {data['generated_events']:,} simulated events)",
            "",
            f"- **Event count:** {data['total_events']:,} including {data['seed_events']:,} preload events",
            f"- **Operations:** {events['new']:,} NEW, {events['cancel']:,} CANCEL, "
            f"{events['replace']:,} REPLACE; {events['market_ioc']:,} market IOC orders",
            f"- **Executions:** {x['trades']:,} fill records, {x['quantity']:,} executed quantity units",
            f"- **Execution fingerprint:** `{data['execution_fingerprint_fnv1a64']}`",
            "",
            "| Regime | Events | Observed events/s | Aggressive NEW | Trades | Executed qty | Mean spread (ticks) | Mean depth-5 imbalance |",
            "|---|---:|---:|---:|---:|---:|---:|---:|",
        ]
        for regime in ("calm", "volatile", "stress", "seed", "external"):
            if regime not in data["regimes"]:
                continue
            row = data["regimes"][regime]
            blocks.append(
                f"| {regime} | {row['events']:,} | {fmt(row['observed_arrivals_per_second'], 0)} "
                f"| {row['aggressive_new']:,} | {row['trades']:,} | {row['quantity']:,} "
                f"| {fmt(row['quotes']['mean_spread_ticks'])} "
                f"| {fmt(row['quotes']['mean_depth5_imbalance'], 4)} |"
            )
        blocks += ["", "**Interpretation:** Quotes and depth are sampled after each event. "
                    "The fill count is matching-engine execution count, not number of distinct trades at an exchange. "
                    "Arrival rates use phase elapsed simulated time and will vary with the seeded draw.", ""]
    blocks += [
        "## Validity boundaries", "",
        "- The scenario rates, aggressive-order share, price-placement and size distributions are user-defined assumptions.",
        "- No real Kraken order stream or queue-position data were used; aggregate L2 data cannot directly reveal order identities.",
        "- The replayer excludes network latency, WAL synchronization, risk, fees, clearing and account funding.",
        "- Cross-runtime identical traces are not guaranteed because floating-point `log` behavior may differ.",
        "- For defensible research, run several seeds and disclose parameter sensitivity; use normalized empirical observations for calibration later.",
        "",
    ]
    return "\n".join(blocks)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--summaries", nargs="+", required=True, help="v1.0 JSON summary paths")
    p.add_argument("--output", help="optional Markdown output path")
    args = p.parse_args()
    report = build(args.summaries)
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(report, encoding="utf-8")
        print(f"report_written={out}")
    else:
        print(report)


if __name__ == "__main__":
    main()
