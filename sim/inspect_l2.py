#!/usr/bin/env python3
"""Summarize independently normalized aggregate top-of-book observations.

Input CSV: timestamp_us,best_bid_ticks,best_ask_ticks,bid_qty,ask_qty,trade_side,trade_qty
`bid_qty` / `ask_qty` are sizes AT BEST QUOTE, not depth-5 volume.
One record must represent one observation; duplicate quote timestamps are allowed.
Optional trade_side/trade_qty must be NONE/0 or BUY|SELL/positive quantity.
This cannot recover individual NEW/CANCEL/REPLACE events or maker queue priority.
"""
import argparse
import csv
import json
from pathlib import Path
import statistics

FIELDS = ["timestamp_us", "best_bid_ticks", "best_ask_ticks", "bid_qty", "ask_qty", "trade_side", "trade_qty"]


def inspect(path):
    spreads, imbalances, changes = [], [], []
    ts_first = ts_last = previous_mid = None
    rows = trade_rows = 0
    buy_qty = sell_qty = 0.0
    with open(path, newline="", encoding="utf-8-sig") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames != FIELDS:
            raise ValueError("expected columns exactly: " + ",".join(FIELDS))
        for line, record in enumerate(reader, start=2):
            try:
                ts = int(record["timestamp_us"])
                bid = int(record["best_bid_ticks"])
                ask = int(record["best_ask_ticks"])
                bid_qty = float(record["bid_qty"])
                ask_qty = float(record["ask_qty"])
                trade_side = record["trade_side"].strip().upper() or "NONE"
                trade_qty = float(record["trade_qty"] or 0)
            except (TypeError, ValueError, AttributeError) as err:
                raise ValueError(f"row {line}: invalid numeric column") from err
            if (ts < 0 or (ts_last is not None and ts < ts_last)
                    or bid <= 0 or ask <= bid or bid_qty < 0 or ask_qty < 0
                    or trade_qty < 0 or trade_side not in {"NONE", "BUY", "SELL"}):
                raise ValueError(f"row {line}: invalid/non-monotonic observation")
            if not (abs(bid_qty) < float("inf") and abs(ask_qty) < float("inf")
                    and abs(trade_qty) < float("inf")):
                raise ValueError(f"row {line}: non-finite quantity")
            if (trade_side == "NONE") != (trade_qty == 0):
                raise ValueError(f"row {line}: trade side/quantity mismatch")
            if ts_first is None:
                ts_first = ts
            ts_last = ts
            rows += 1
            spreads.append(ask-bid)
            denom = bid_qty + ask_qty
            imbalances.append((bid_qty-ask_qty)/denom if denom else 0.0)
            mid = (bid+ask)/2
            if previous_mid is not None:
                changes.append(abs(mid-previous_mid)/previous_mid * 10000)
            previous_mid = mid
            if trade_qty:
                trade_rows += 1
                if trade_side == "BUY": buy_qty += trade_qty
                else: sell_qty += trade_qty
    if not rows:
        raise ValueError("no observations in normalized L2 input")
    duration = ts_last - ts_first
    sorted_spreads = sorted(spreads)
    return {
        "schema": "micro_exchange_l2_observations_v1",
        "data_kind": "normalized_aggregate_market_observations",
        "source_path_basename": Path(path).name,
        "rows": rows,
        "observed_span_us": duration,
        "observation_rate_per_second": round((rows-1)*1e6/duration, 4) if duration else None,
        "spread_ticks": {
            "median": statistics.median(spreads),
            "mean": round(statistics.mean(spreads), 4),
            "p99_nearest_rank": sorted_spreads[max(0, (99*len(spreads)+99)//100 - 1)],
        },
        "mean_imbalance_at_best_quote": round(statistics.mean(imbalances), 6),
        "mean_absolute_midprice_change_bps_per_observation": round(statistics.mean(changes), 6) if changes else None,
        "tagged_trade_observations": trade_rows,
        "buyer_initiated_trade_quantity": round(buy_qty, 6),
        "seller_initiated_trade_quantity": round(sell_qty, 6),
        "limitation": "Aggregate L2 quotes do not identify orders, cancellations, queue position, or exact order arrival rate.",
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--input", required=True)
    p.add_argument("--output")
    args = p.parse_args()
    result = json.dumps(inspect(args.input), indent=2) + "\n"
    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(result, encoding="utf-8")
        print(f"l2_observations_written={out}")
    else:
        print(result, end="")


if __name__ == "__main__":
    main()
