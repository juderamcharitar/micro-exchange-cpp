#!/usr/bin/env python3
"""Controlled local TCP concurrency + WAL experiment with accounting/replay checks.

Uses 2 persistent TCP clients per worker pair: maker SELL GTC, taker BUY IOC,
plus periodic add/reduce/cancel orders. A new server and (when enabled) a new
journal are created for each trial; no credentials or user journal are touched.
The 'clients' count is 2*pairs, limited by the gateway's 32-connection cap.

Not a production exchange benchmark or an empirical market-traffic generator.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import re
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time


class LineClient:
    def __init__(self, port: int, tcp_nodelay: bool = False):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=15)
        self.sock.settimeout(20)
        if tcp_nodelay:
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b''
        hello = self.read_line()
        if not hello.startswith('HELLO '):
            raise AssertionError(f'missing server greeting: {hello!r}')

    def read_line(self) -> str:
        while b'\n' not in self.buffer:
            chunk = self.sock.recv(16384)
            if not chunk:
                raise RuntimeError('server closed connection awaiting response')
            self.buffer += chunk
            if len(self.buffer) > 1_000_000:
                raise RuntimeError('client response buffer exceeded limit')
        line, self.buffer = self.buffer.split(b'\n', 1)
        return line.decode('ascii').rstrip('\r')

    def expect(self, prefix: str) -> str:
        for _ in range(100_000):
            response = self.read_line()
            # The maker receives asynchronous FILL reports from other clients;
            # ignore those when waiting for its own ACK.
            if response.startswith(prefix):
                return response
            if not response.startswith('FILL '):
                raise AssertionError(f'expected {prefix!r}, received {response!r}')
        raise AssertionError('excessively many pending fill notifications')

    def command(self, command: str, prefix: str, accepted: bool = False) -> tuple[str, int]:
        start = time.perf_counter_ns()
        self.sock.sendall((command + '\n').encode('ascii'))
        line = self.expect(prefix)
        elapsed = time.perf_counter_ns() - start
        if accepted and 'status=accepted' not in line:
            raise AssertionError(f'{command!r} not accepted: {line!r}')
        return line, elapsed

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Server:
    def __init__(self, exe: str, journal: Path | None, tcp_nodelay: bool = False,
                 journal_mode: str = "sync"):
        args = [exe, '--port', '0']
        if tcp_nodelay:
            args.append('--tcp-nodelay')
        if journal is not None:
            args.extend(['--journal', str(journal), '--journal-mode', journal_mode])
        self.proc = subprocess.Popen(
            args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        # Output is line buffered by the server's explicit std::endl.
        ready = self.proc.stdout.readline().strip()
        if not ready:
            try:
                _, err = self.proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                _, err = self.proc.communicate(timeout=5)
            raise RuntimeError(f'server exited or did not report READY: {err[:2000]!r}')
        match = re.fullmatch(r'READY host=127\.0\.0\.1 port=(\d+) version=([\w.]+)', ready)
        if not match:
            self.stop()
            raise RuntimeError(f'unexpected server startup: {ready!r}')
        self.port = int(match.group(1))
        self.version = match.group(2)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.communicate(timeout=5)


def percentile_us(values_ns: list[int], p: float) -> float:
    vals = sorted(values_ns)
    if not vals:
        raise ValueError('empty sample')
    return vals[min(len(vals)-1, int((len(vals)-1)*p))] / 1000


def sample(values_ns: list[int]) -> dict:
    return {
        'samples': len(values_ns),
        'mean_us': round(statistics.mean(values_ns)/1000, 3),
        'p50_us': round(percentile_us(values_ns, 0.5), 3),
        'p99_us': round(percentile_us(values_ns, 0.99), 3),
    }


def worker(index: int, port: int, rounds: int, barrier: threading.Barrier, tcp_nodelay: bool) -> dict:
    seller = LineClient(port, tcp_nodelay)
    buyer = LineClient(port, tcp_nodelay)
    ack_latencies: list[int] = []
    fill_latencies: list[int] = []
    sent = 0
    try:
        barrier.wait(timeout=30)
        for n in range(rounds):
            base = 10_000_000 + index*10_000_000 + n*10
            maker, latency = seller.command(
                f'NEW {base} SELL LIMIT 10000 1 GTC', 'ACK ', accepted=True)
            if f'order_id={base}' not in maker:
                raise AssertionError('maker ID mismatch: '+maker)
            ack_latencies.append(latency)
            sent += 1

            tic = time.perf_counter_ns()
            taker, _ = buyer.command(
                f'NEW {base+1} BUY LIMIT 10000 1 IOC', 'ACK ', accepted=True)
            ack_latencies.append(time.perf_counter_ns()-tic)
            fill = buyer.expect('FILL ')
            fill_latencies.append(time.perf_counter_ns()-tic)
            if f'taker={base+1}' not in fill or 'qty=1' not in fill or 'price_ticks=10000' not in fill:
                raise AssertionError('incorrect fill: '+fill)
            sent += 1

            if n % 5 == 0:
                # Passive quote/amend/cancel loop, strictly below all asks.
                quote_id = base + 2
                _, t = buyer.command(f'NEW {quote_id} BUY LIMIT 9000 2 GTC', 'ACK ', accepted=True)
                ack_latencies.append(t)
                _, t = buyer.command(f'REPLACE {quote_id} 9000 1', 'ACK ', accepted=True)
                ack_latencies.append(t)
                result, t = buyer.command(f'CANCEL {quote_id}', 'ACK ', accepted=True)
                if 'removed=1' not in result:
                    raise AssertionError('cancel did not remove remaining quantity')
                ack_latencies.append(t)
                sent += 3
        return {'commands': sent, 'ack_latencies_ns': ack_latencies,
                'fill_latencies_ns': fill_latencies}
    finally:
        seller.close()
        buyer.close()


def fields(line: str) -> dict[str, str]:
    return dict(x.split('=', 1) for x in line.split() if '=' in x)


def assert_book_and_ledger(port: int, pairs: int, rounds: int, tcp_nodelay: bool) -> dict:
    client = LineClient(port, tcp_nodelay)
    try:
        stats, _ = client.command('STATS', 'STATS ')
        check, _ = client.command('CHECK', 'CHECK ')
        ledger, _ = client.command('LEDGER_CHECK', 'LEDGER_CHECK ')
        book, _ = client.command('BOOK', 'BOOK ')
    finally:
        client.close()
    s, c, l, b = map(fields, (stats, check, ledger, book))
    expected_trades = pairs * rounds
    extra_per_pair = (rounds+4)//5
    expected_new = pairs * (2*rounds + extra_per_pair)
    expected_commands = pairs * (2*rounds + 3*extra_per_pair)
    if int(s['trades']) != expected_trades:
        raise AssertionError(f'trade count mismatch {stats!r} vs expected {expected_trades}')
    if (int(s['accepted']) != expected_new or
            int(s['replaced']) != pairs*extra_per_pair or
            int(s['cancelled']) != pairs*extra_per_pair):
        raise AssertionError(f'engine mutation counters do not match workload: {stats!r}')
    if int(s['resting']) != 0 or b.get('bids') != 'none' or b.get('asks') != 'none':
        raise AssertionError(f'book not empty after cycles: {book!r}; {stats!r}')
    if c.get('status') != 'ok' or l.get('status') != 'ok' or int(l['trades']) != expected_trades:
        raise AssertionError(f'invariants or ledger inconsistent: {check!r}; {ledger!r}')
    return {'trades': expected_trades, 'accepted_mutations': expected_commands,
            'checks': {'book': 'empty', 'engine': 'ok', 'ledger': 'ok'}}


def trial(exe: str, pairs: int, rounds: int, mode: str, tcp_nodelay: bool) -> dict:
    with tempfile.TemporaryDirectory(prefix='micro_exchange_v07_') as tmp:
        journal = Path(tmp) / 'commands.journal' if mode != 'off' else None
        server = Server(exe, journal, tcp_nodelay, mode)
        try:
            barrier = threading.Barrier(pairs+1)
            with ThreadPoolExecutor(max_workers=pairs) as pool:
                future = [pool.submit(worker, i, server.port, rounds, barrier, tcp_nodelay) for i in range(pairs)]
                start = time.perf_counter()
                barrier.wait(timeout=30)
                results = [f.result(timeout=180) for f in future]
                elapsed = time.perf_counter()-start
            verified = assert_book_and_ledger(server.port, pairs, rounds, tcp_nodelay)
            stats_client = LineClient(server.port, tcp_nodelay)
            try:
                raw_stats, _ = stats_client.command('JOURNAL_STATS', 'JOURNAL_STATS ')
                journal_stats = fields(raw_stats)
            finally:
                stats_client.close()
        finally:
            server.stop()
        # For durable trials, replay the exact journal in a new process, then
        # reassert book, counts, engine invariants, and accounting from recovery.
        if journal is not None:
            restarted = Server(exe, journal, tcp_nodelay, mode)
            try:
                assert_book_and_ledger(restarted.port, pairs, rounds, tcp_nodelay)
                verified['post_restart_recovery'] = 'passed'
            finally:
                restarted.stop()
            verified['journal_bytes'] = journal.stat().st_size
        else:
            verified['post_restart_recovery'] = 'not_applicable'

        ack = [x for r in results for x in r['ack_latencies_ns']]
        fill = [x for r in results for x in r['fill_latencies_ns']]
        commands = sum(r['commands'] for r in results)
        if commands != verified['accepted_mutations'] or len(ack) != commands:
            raise AssertionError('command count discrepancy')
        return {
            'journal_mode': mode,
            'journal_stats': journal_stats,
            'tcp_nodelay': tcp_nodelay,
            'pairs': pairs,
            'concurrent_clients': pairs*2,
            'rounds_per_pair': rounds,
            'measured_mutations': commands,
            'trades': verified['trades'],
            'elapsed_seconds': round(elapsed, 6),
            'throughput_mutations_per_second': round(commands/elapsed, 2),
            'response_ack_rtt': sample(ack),
            'fill_rtt': sample(fill),
            'correctness': verified,
        }


def find_server() -> Path:
    root = Path(__file__).resolve().parent.parent
    for candidate in (root/'build'/'Release'/'exchange_server.exe',
                      root/'build'/'exchange_server',
                      root/'build'/'exchange_server.exe'):
        if candidate.is_file():
            return candidate
    raise RuntimeError('server executable not found; build the project first, or pass --server path')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, default=None, help='Path to built exchange_server executable')
    parser.add_argument('--pairs', default='1,2,4', help='Comma-separated worker PAIRS; 2 TCP clients per pair, max 12')
    parser.add_argument('--rounds', type=int, default=75, help='Rounds per pair (default: 75)')
    parser.add_argument('--mode', choices=['off', 'durable', 'sync', 'group', 'both', 'all'], default='all')
    parser.add_argument('--repeats', type=int, default=1, help='Independent fresh-server repeats per scenario (1..10)')
    parser.add_argument('--json-output', type=Path, help='Write reproducible machine-readable results')
    parser.add_argument('--tcp-nodelay', action='store_true',
                        help='Set TCP_NODELAY on benchmark connections and server accepted sockets')
    args = parser.parse_args()
    try:
        pairs_list = [int(s) for s in args.pairs.split(',')]
    except ValueError:
        parser.error('--pairs expects e.g. 1,2,4,8')
    if (not pairs_list or len(pairs_list) > 12 or any(x < 1 or x > 12 for x in pairs_list)
            or len(set(pairs_list)) != len(pairs_list)):
        parser.error('--pairs must be unique integers between 1 and 12')
    if args.repeats < 1 or args.repeats > 10:
        parser.error('--repeats must be 1..10')
    if args.rounds < 1 or args.rounds > 500:
        parser.error('--rounds must be 1..500 (controls risk limits and runtime)')
    exe = (args.server or find_server()).resolve()
    if not exe.is_file():
        parser.error(f'executable not found: {exe}')
    modes = {'both': ['off', 'sync'], 'all': ['off', 'sync', 'group']}.get(args.mode, [args.mode])
    output = {
        'schema': 'micro_exchange_scaling_v2',
        'project_version': '0.9.0',
        'date_utc': datetime.now(timezone.utc).isoformat(),
        'platform': platform.platform(),
        'tcp_nodelay': args.tcp_nodelay,
        'repeats': args.repeats,
        'python': platform.python_version(),
        'server_executable': str(exe),
        'workload': '2 clients/pair: passive sell + crossing IOC buy, periodic new/replace/cancel bid',
        'limits': 'max 32 concurrent connections in server; timed region excludes server startup, '
                  'client connections, journal replay and correctness queries',
        'results': [],
    }
    for mode in modes:
        for count in pairs_list:
            for repetition in range(1, args.repeats+1):
                result = trial(str(exe), count, args.rounds, 'sync' if mode == 'durable' else mode,
                               args.tcp_nodelay)
                result['repetition'] = repetition
                output['results'].append(result)
                print(f"mode={mode:7} nodelay={str(args.tcp_nodelay):5} "
                      f"repeat={repetition:2} clients={result['concurrent_clients']:2} "
                      f"commands={result['measured_mutations']:5} "
                      f"trades={result['trades']:5} "
                      f"throughput_cmd_s={result['throughput_mutations_per_second']:9.2f} "
                      f"p50_ack_us={result['response_ack_rtt']['p50_us']:9.2f} "
                      f"p99_ack_us={result['response_ack_rtt']['p99_us']:10.2f} "
                      f"syncs={result['journal_stats'].get('syncs', 'n/a')} "
                      f"max_group={result['journal_stats'].get('max_group', 'n/a')} "
                      f"recovery={result['correctness']['post_restart_recovery']}")
    if args.json_output:
        args.json_output.parent.mkdir(parents=True, exist_ok=True)
        args.json_output.write_text(json.dumps(output, indent=2)+'\n', encoding='utf-8')
        print(f'results_json={args.json_output}')
    print('NOTE: loopback/Python/scheduler/WAL overhead included; synthetic deterministic '
          'two-client workloads, NOT wire latency, real data, or guaranteed production throughput.')


if __name__ == '__main__':
    main()
