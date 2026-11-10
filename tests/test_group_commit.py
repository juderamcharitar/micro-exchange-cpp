"""v0.7 loopback group-commit tests. Stdlib only, isolated temp journals.

Only successful ACKs constitute a durability promise. Abrupt process kills
are not power-failure or storage-controller fault-injection tests.
"""
from __future__ import annotations
import re
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

EXE = sys.argv[1]
sys.argv = [sys.argv[0]]


class RunningServer:
    def __init__(self, journal: Path, mode: str = 'group'):
        self.proc = subprocess.Popen(
            [EXE, '--port', '0', '--journal', str(journal),
             '--journal-mode', mode, '--tcp-nodelay'],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            encoding='utf8', bufsize=1)
        line = self.proc.stdout.readline().strip()
        m = re.fullmatch(r'READY host=127\.0\.0\.1 port=(\d+) version=1\.0\.0', line)
        if not m:
            self.stop()
            raise AssertionError(f'server did not start: {line!r}')
        self.port = int(m[1])

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.communicate(timeout=5)


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=5)
        self.sock.settimeout(5)
        self.buf = b''
        assert self.read().startswith('HELLO ')

    def read(self):
        while b'\n' not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError('connection closed before ACK')
            self.buf += chunk
        line, self.buf = self.buf.split(b'\n', 1)
        return line.decode('ascii')

    def send(self, data):
        self.sock.sendall(data.encode('ascii'))

    def cmd(self, line, prefix):
        self.send(line + '\n')
        got = self.read()
        assert got.startswith(prefix), (line, got)
        return got

    def close(self):
        self.sock.close()


def parse(line):
    return dict(s.split('=', 1) for s in line.split() if '=' in s)


class GroupCommitTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='micro_exchange_group_')
        self.journal = Path(self.temp.name) / 'commands.journal'
        self.server = None

    def tearDown(self):
        if self.server is not None:
            self.server.stop()
        self.temp.cleanup()

    def start(self, mode='group'):
        self.server = RunningServer(self.journal, mode)
        return self.server.port

    def restart(self, mode='group'):
        self.server.stop()
        return self.start(mode)

    def test_real_batch_single_fsync_and_post_restart_book(self):
        port = self.start()
        peer = Peer(port)
        # Multiple accepted mutations arrive within one recv and one event-loop
        # group. They must all be flushed before any success response is sent.
        peer.send('NEW 900 SELL LIMIT 10100 1 GTC\n'
                  'NEW 901 SELL LIMIT 10100 2 GTC\n'
                  'NEW 902 SELL LIMIT 10100 3 GTC\n')
        for oid in (900, 901, 902):
            got = peer.read()
            self.assertIn(f'order_id={oid}', got)
            self.assertIn('status=accepted', got)
        stats = parse(peer.cmd('JOURNAL_STATS', 'JOURNAL_STATS '))
        self.assertEqual(stats['mode'], 'group')
        self.assertEqual(int(stats['appended']), 3)
        self.assertEqual(int(stats['syncs']), 1)
        self.assertEqual(int(stats['max_group']), 3)
        self.assertEqual(peer.cmd('CHECK', 'CHECK ').split('status=')[1], 'ok')
        peer.close()
        port = self.restart()
        restored = Peer(port)
        self.assertIn('asks=10100:6:3', restored.cmd('BOOK', 'BOOK '))
        self.assertIn('status=ok', restored.cmd('CHECK', 'CHECK '))
        restored.close()

    def test_strict_mode_one_sync_per_mutation_and_cross_mode_replay(self):
        p = Peer(self.start('sync'))
        p.send('NEW 100 SELL LIMIT 10000 1 GTC\nNEW 101 SELL LIMIT 11000 1 GTC\n')
        self.assertIn('status=accepted', p.read())
        self.assertIn('status=accepted', p.read())
        stats = parse(p.cmd('JOURNAL_STATS', 'JOURNAL_STATS '))
        self.assertEqual(stats['mode'], 'sync')
        self.assertEqual(int(stats['syncs']), 2)
        self.assertEqual(int(stats['max_group']), 1)
        p.close()
        p = Peer(self.restart('group'))
        self.assertIn('asks=10000:1:1,11000:1:1', p.cmd('BOOK', 'BOOK '))
        p.close()

    def test_killed_process_retains_all_acknowledged_mutations(self):
        port = self.start('group')
        peer = Peer(port)
        for i in range(30):
            got = peer.cmd(f'NEW {1000+i} SELL LIMIT {12000+i} 1 GTC', 'ACK ')
            self.assertIn('status=accepted', got)
        peer.close()
        # kill rather than graceful shutdown, checking acknowledged commands
        # only; unacknowledged commands can have uncertain outcomes.
        port = self.restart('group')
        peer = Peer(port)
        self.assertIn('resting=30', peer.cmd('STATS', 'STATS '))
        self.assertIn('status=ok', peer.cmd('CHECK', 'CHECK '))
        self.assertIn('status=ok', peer.cmd('LEDGER_CHECK', 'LEDGER_CHECK '))
        peer.close()

    def test_missing_journal_argument_invalid_mode(self):
        p = subprocess.run([EXE, '--journal-mode', 'garbage'],
                           capture_output=True, text=True, timeout=5)
        self.assertNotEqual(p.returncode, 0)


if __name__ == '__main__':
    unittest.main()
