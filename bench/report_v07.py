#!/usr/bin/env python3
"""Summarize v0.7 repeated single-host synthetic benchmarks without unsupported inference."""
from __future__ import annotations
import argparse
import json
import statistics
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--scaling', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    raw = json.loads(args.scaling.read_text(encoding='utf-8'))
    if raw.get('schema') != 'micro_exchange_scaling_v2':
        p.error('expected v0.7 scaling schema')
    results = raw['results']
    modes = ['off', 'sync', 'group']
    clients = sorted({r['concurrent_clients'] for r in results})
    lines = [
        '# Micro Exchange v0.7 — Journal Strategy Benchmark', '',
        f"**Collected (UTC):** {raw.get('date_utc', 'unknown')}",
        f"**Host:** {raw.get('platform', 'unknown')}",
        f"**Python:** {raw.get('python', 'unknown')}",
        f"**TCP_NODELAY:** {raw.get('tcp_nodelay')}",
        f"**Repetitions:** {raw.get('repeats')}", '',
        '## Matched workload measurements', '',
        '| Mode | Clients | Trials | Median cmd/s | Min–max cmd/s | '
        'Median trial p50 ACK (µs) | Median trial p99 ACK (µs) | '
        'Syncs across trials | Largest group | Recovery |',
        '|---|---:|---:|---:|---|---:|---:|---|---:|---|',
    ]
    for mode in modes:
        for c in clients:
            sub = [r for r in results if r['journal_mode'] == mode and r['concurrent_clients'] == c]
            if not sub:
                continue
            tps = [r['throughput_mutations_per_second'] for r in sub]
            p50 = [r['response_ack_rtt']['p50_us'] for r in sub]
            p99 = [r['response_ack_rtt']['p99_us'] for r in sub]
            syncs = [int(r['journal_stats']['syncs']) for r in sub]
            max_group = max(int(r['journal_stats']['max_group']) for r in sub)
            restarts = {r['correctness']['post_restart_recovery'] for r in sub}
            lines.append(
                f"| {mode} | {c} | {len(sub)} | {statistics.median(tps):,.0f} | "
                f"{min(tps):,.0f}–{max(tps):,.0f} | {statistics.median(p50):,.2f} | "
                f"{statistics.median(p99):,.2f} | {', '.join(map(str, syncs))} | "
                f"{max_group} | {', '.join(sorted(restarts))} |"
            )
    lines += [
        '', '## Definitions and interpretation', '',
        '- Throughput is accepted order mutation commands per second, not independent matching-engine throughput. '
        'Trials include Python clients, localhost TCP, parsing, scheduling and selected journal mode.',
        '- `sync` appends and fsyncs each accepted mutation; `group` batches mutations observed in one server event-loop cycle and fsyncs once before emitting success responses; `off` does not journal.',
        '- Median throughput is the median of independent trial-level throughputs. Latency columns are medians of trial-level percentile estimates, not percentiles recomputed from pooled raw samples.',
        '- Group size varies with concurrent ready sockets and pipelining. No wait-window or background journal thread is used; a group of one offers no synchronization savings.',
        '- A replay pass after process termination is performed for each durable trial. Process termination does not establish resilience to power loss, disk-controller write caching, torn writes or filesystem corruption.',
        '- Durability-before-ACK is the intended property; an abrupt disconnect before an ACK still leaves an ambiguous outcome and no client-side exactly-once retry semantics.',
        '- This is an exploratory synthetic benchmark with only a handful of repetitions, no market-order-flow replay and no independent statistical inference.',
        '', '## Next verification', '',
        'Run matching sync/group/off conditions on Windows with the same build and TCP settings, record CPU and storage details, and test explicit sync/write failures before treating this mode as operationally robust.',
    ]
    args.output.parent.mkdir(exist_ok=True, parents=True)
    args.output.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print('report=' + str(args.output))

if __name__ == '__main__':
    main()
