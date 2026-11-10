#!/usr/bin/env python3
"""Create a research-facing Markdown report from actual measured v0.6 CSV/JSON.

No benchmark is run by this script, and no missing results are fabricated.
"""
from __future__ import annotations
import argparse
import csv
import json
from pathlib import Path


def fmt(value):
    return f'{float(value):,.2f}'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scaling', type=Path, required=True)
    parser.add_argument('--depth', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args=parser.parse_args()
    scaling=json.loads(args.scaling.read_text(encoding='utf-8'))
    depths=list(csv.DictReader(args.depth.open(encoding='utf-8', newline='')))
    if scaling.get('schema')!='micro_exchange_scaling_v1' or not scaling.get('results') or not depths:
        parser.error('invalid or empty v0.6 benchmark artifacts')
    lines=[
        '# Micro Exchange v0.6 — Measured Benchmark Report',
        '',
        f"**UTC collection timestamp:** {scaling.get('date_utc', 'unavailable')}",
        f"**Platform:** {scaling.get('platform', 'unavailable')}",
        f"**Python:** {scaling.get('python', 'unavailable')}",
        f"**TCP_NODELAY (both client and server):** {scaling.get('tcp_nodelay', 'unavailable')}",
        '',
        '## 1. C++ in-memory engine vs resting-book depth',
        '',
        '| Book levels per side | Preloaded resting orders | Timed operations | Median operations/s | p50 (ns) | p99 (ns) |',
        '|---:|---:|---:|---:|---:|---:|',
    ]
    for row in depths:
        lines.append('| '+ ' | '.join([
            row['levels'], row['preloaded_orders'], row['operations'],
            fmt(row['median_throughput_ops_per_sec']),row['p50_op_ns'],row['p99_op_ns']])+' |')
    lines.extend([
        '',
        'This engine benchmark preloads a noncrossing book, then repeats a **scripted** '
        'six-operation cycle: a passive sell, two partial fills, a passive bid, '
        'a same-price quantity decrease, and cancellation. It checks invariants '
        'and exact trade counts. It excludes TCP, risk checks, and journaling. '
        'Per-operation timings include steady-clock overhead. No actual exchange event stream is replayed.',
        '',
        '## 2. Python-to-C++ loopback TCP execution and replay',
        '',
        '| Journal | Repeat | Clients | Mutations | Trades | Mutations/s | ACK p50 (µs) | ACK p99 (µs) | Post-restart replay |',
        '|---|---:|---:|---:|---:|---:|---:|---:|---|',
    ])
    for row in scaling['results']:
        lines.append('| '+ ' | '.join([
            row['journal_mode'], str(row.get('repetition', 1)), str(row['concurrent_clients']), str(row['measured_mutations']),
            str(row['trades']), fmt(row['throughput_mutations_per_second']),
            fmt(row['response_ack_rtt']['p50_us']),fmt(row['response_ack_rtt']['p99_us']),
            row['correctness']['post_restart_recovery']])+' |')
    lines.extend([
        '',
        'The TCP benchmark uses two clients per pair (one passive seller, one buyer). '
        'Each pair alternates successful crossing trades with occasional passive '
        'add/reduce/cancel operations. Concurrent clients have distinct identities. '
        'The timing window excludes setup, server startup, restart, journal replay, '
        'and post-run checks. Each trial uses its own fresh process and optional journal. '
        'Every trial verifies exact trades, an empty final book, engine invariants, '
        'and ledger reconciliation; durable trials also reverify after server restart.',
        '',
        '## 3. Interpretation and validity boundaries',
        '',
        '- **Not wire latency:** Python, loopback TCP, parsing, system scheduling, '
        'and (in durable mode) per-mutation flush/sync are included.',
        '- **Not a production throughput claim:** this is a bounded 32-connection '
        'single-process simulator; the tested client counts may be lower.',
        '- **Not empirical market data:** traffic is synthetic, with no order IDs, '
        'queue estimates, or exchange execution timestamps from an actual market.',
        '- **Not independent inference:** one run per scenario is a baseline, not '
        'statistical evidence of a stable performance difference. Repeat trials, '
        'record hardware/environment, and report dispersion before making claims.',
        '- **Accounting differs from a broker:** cash flows use tick-units, no funded '
        'balances, margin, clearing, fees, or real venue connectivity.',
        '- **Transport comparison requires matched settings:** compare runs with '
        'Nagle/delayed ACK behavior versus TCP_NODELAY independently; do not mix '
        'them to attribute journaling effects.',
        '',
        '## 4. Suggested next measurements',
        '',
        'Repeat each row across warm/cold runs; archive clock, CPU/OS build, '
        'SSD and power-plan settings; add deeper resting orders per level, bursts, '
        'backpressure, disconnected makers and explicit durability failure injection. '
        'Future empirical replay requires appropriate order-level data or transparent '
        'queue models; these results are deliberately synthetic.',
        '',
    ])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(lines),encoding='utf-8')
    print(f'report_written={args.output}')

if __name__=='__main__':
    main()
