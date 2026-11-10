"""v0.8 black-box fault/duplicate tests; localhost only; no external dependencies.

A post-fsync, pre-ACK crash is the core ambiguous-outcome case. The RID
receipt must survive restart and a replay of the same RID must NOT trade twice.
Injected process exits do not simulate true power loss/storage-controller faults.
"""
from __future__ import annotations
import os
import re
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

EXE = sys.argv[1]
sys.argv = [sys.argv[0]]
TOKEN_A = 'ab' * 32  # Synthetic token only; never use it for a real account.
TOKEN_B = 'cd' * 32


class Server:
    def __init__(self, journal, *, stage=None, at=None, mode='group'):
        args = [EXE, '--port', '0', '--journal', str(journal), '--journal-mode',
                mode, '--tcp-nodelay']
        if stage:
            args += ['--fault-stage', stage, '--fault-at', str(at)]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, bufsize=1)
        ready = self.proc.stdout.readline().strip()
        m = re.fullmatch(r'READY host=127\.0\.0\.1 port=(\d+) version=1\.0\.0', ready)
        if not m:
            out, err = self.proc.communicate(timeout=5)
            raise AssertionError(f'Server did not start: exit={self.proc.returncode}, '
                                 f'stdout={ready!r} {out!r}, stderr={err!r}')
        self.port = int(m.group(1))

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.communicate(timeout=5)


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=4)
        self.sock.settimeout(4)
        self.buf = b''
        assert self.line().startswith('HELLO ')

    def line(self):
        while b'\n' not in self.buf:
            block = self.sock.recv(65536)
            if not block:
                raise EOFError('Connection closed without reply')
            self.buf += block
        line, self.buf = self.buf.split(b'\n', 1)
        return line.decode('ascii')

    def send(self, line):
        self.sock.sendall((line + '\n').encode('ascii'))

    def cmd(self, line):
        self.send(line)
        return self.line()

    def close(self):
        self.sock.close()


def fields(line):
    return dict(part.split('=', 1) for part in line.split() if '=' in part)


class FaultIdempotency(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix='mex_fault_')
        self.journal = Path(self.folder.name) / 'log.journal'
        self.server = None
        self.peers = []

    def tearDown(self):
        for p in self.peers:
            p.close()
        if self.server:
            self.server.stop()
        self.folder.cleanup()

    def start(self, **opts):
        if self.server:
            self.server.stop()
        self.server = Server(self.journal, **opts)
        return self.server.port

    def reset_journal_for_subtest(self):
        """Release all open handles before removing the WAL between modes.

        POSIX permits unlinking an open file. Windows instead raises WinError 32
        while the previous server still has the journal open. Waiting for the
        server process to exit also prevents races with pending WAL flushes.
        """
        for peer in self.peers:
            peer.close()
        self.peers.clear()
        if self.server is not None:
            self.server.stop()  # kill (if necessary) and wait for process exit
            self.server = None
        self.journal.unlink(missing_ok=True)

    def peer(self, port):
        p = Peer(port)
        self.peers.append(p)
        return p

    def register(self, p, name='alice', token=TOKEN_A):
        response = p.cmd(f'REGISTER {name} {token}')
        self.assertIn('status=accepted', response)
        return response

    def auth(self, p, name='alice', token=TOKEN_A):
        response = p.cmd(f'AUTH {name} {token}')
        self.assertIn('status=accepted', response)
        return response

    def verify_exit_without_ack(self, peer, command, expected_status):
        peer.send(command)
        with self.assertRaises((EOFError, ConnectionResetError, BrokenPipeError, OSError)):
            peer.line()
        status = self.server.proc.wait(timeout=5)
        self.assertEqual(status, expected_status)

    def test_retry_post_fsync_crash_does_not_duplicate_sync(self):
        for mode in ('sync', 'group'):
            with self.subTest(mode=mode):
                self.reset_journal_for_subtest()
                p = self.peer(self.start(stage='after-sync', at=2, mode=mode))
                self.register(p)
                self.verify_exit_without_ack(
                    p, 'RID 500 NEW 800 SELL LIMIT 10100 5 GTC', 83)
                recovered = self.peer(self.start(mode=mode))
                self.auth(recovered)
                self.assertIn('asks=10100:5:1', recovered.cmd('BOOK'))
                receipt = recovered.cmd('RID 500 NEW 800 SELL LIMIT 10100 5 GTC')
                self.assertIn('status=accepted', receipt)
                self.assertIn('duplicate=1', receipt)
                self.assertIn('accepted=1', recovered.cmd('STATS'))
                self.assertIn('status=ok', recovered.cmd('CHECK'))
                self.assertIn('status=accepted', recovered.cmd('RID 501 CANCEL 800'))
                self.assertIn('asks=none', recovered.cmd('BOOK'))

    def test_before_write_crash_has_no_record_or_effect(self):
        p = self.peer(self.start(stage='before-write', at=2))
        self.register(p)
        self.verify_exit_without_ack(p, 'RID 10 NEW 400 BUY LIMIT 100 1 GTC', 81)
        restored = self.peer(self.start())
        self.auth(restored)
        self.assertIn('bids=none', restored.cmd('BOOK'))
        self.assertIn('status=accepted', restored.cmd('RID 10 NEW 400 BUY LIMIT 100 1 GTC'))
        self.assertIn('bids=100:1:1', restored.cmd('BOOK'))

    def test_before_sync_crash_has_ambiguous_outcome_and_no_ack(self):
        p = self.peer(self.start(stage='before-sync', at=2))
        self.register(p)
        self.verify_exit_without_ack(p, 'RID 11 NEW 600 SELL LIMIT 11000 1 GTC', 82)
        # Platform C stdio buffering determines whether the final full record
        # reached the kernel; on a valid replay RID deduplicates either case.
        p2 = self.peer(self.start())
        self.auth(p2)
        before = p2.cmd('STATS')
        self.assertIn('status=accepted', p2.cmd('RID 11 NEW 600 SELL LIMIT 11000 1 GTC'))
        self.assertIn('asks=11000:1:1', p2.cmd('BOOK'))
        self.assertIn('accepted=1', p2.cmd('STATS'))
        self.assertIn('status=ok', p2.cmd('CHECK'))
        self.assertIn('accepted=', before)

    def test_duplicate_conflict_not_journaled_and_no_additional_trade(self):
        p = self.peer(self.start(mode='sync'))
        self.register(p)
        self.assertIn('status=accepted', p.cmd('RID 101 NEW 100 SELL LIMIT 10500 5 GTC'))
        old = self.journal.read_bytes()
        self.assertIn('idempotency_conflict', p.cmd('RID 101 CANCEL 100'))
        self.assertEqual(old, self.journal.read_bytes())
        self.assertIn('duplicate=1', p.cmd('RID 101 NEW 100 SELL LIMIT 10500 5 GTC'))
        self.assertEqual(old, self.journal.read_bytes())
        self.assertIn('asks=10500:5:1', p.cmd('BOOK'))

    def test_replace_cancel_retries_and_account_scoping(self):
        port = self.start(mode='group')
        a, b = self.peer(port), self.peer(port)
        self.register(a, 'alice', TOKEN_A)
        self.register(b, 'bob', TOKEN_B)
        self.assertIn('status=accepted', a.cmd('RID 1 NEW 101 SELL LIMIT 11000 5 GTC'))
        self.assertIn('status=accepted', a.cmd('RID 2 REPLACE 101 11200 4'))
        self.assertIn('duplicate=1', a.cmd('RID 2 REPLACE 101 11200 4'))
        self.assertIn('asks=11200:4:1', a.cmd('BOOK'))
        self.assertIn('status=accepted', a.cmd('RID 3 CANCEL 101'))
        self.assertIn('duplicate=1', a.cmd('RID 3 CANCEL 101'))
        self.assertIn('asks=none', a.cmd('BOOK'))
        # Bob can use request ID 1 for a distinct command on a distinct owner.
        self.assertIn('status=accepted', b.cmd('RID 1 NEW 202 BUY LIMIT 10000 2 GTC'))
        self.assertIn('bids=10000:2:1', b.cmd('BOOK'))
        port = self.start(mode='sync')
        recovered_a = self.peer(port)
        self.auth(recovered_a, 'alice', TOKEN_A)
        self.assertIn('duplicate=1', recovered_a.cmd('RID 3 CANCEL 101'))
        self.assertIn('status=ok', recovered_a.cmd('CHECK'))
        self.assertIn('bids=10000:2:1', recovered_a.cmd('BOOK'))

    def test_duplicate_fill_receipt_after_restart_does_not_retrade(self):
        port = self.start()
        a, b = self.peer(port), self.peer(port)
        self.register(a, 'alice', TOKEN_A)
        self.register(b, 'bob', TOKEN_B)
        self.assertIn('status=accepted', a.cmd('RID 1 NEW 10 SELL LIMIT 100 2 GTC'))
        self.assertIn('status=accepted', b.cmd('RID 2 NEW 11 BUY LIMIT 100 2 IOC'))
        fill = b.line()
        self.assertIn('trade_seq=1', fill)
        self.assertIn('trade_seq=1', a.line())
        port = self.start()
        b = self.peer(port)
        self.auth(b, 'bob', TOKEN_B)
        first = b.cmd('RID 2 NEW 11 BUY LIMIT 100 2 IOC')
        second = b.line()
        self.assertIn('duplicate=1', first)
        self.assertEqual(second, fill)
        self.assertIn('trades=1', b.cmd('STATS'))
        self.assertIn('status=ok', b.cmd('LEDGER_CHECK'))

    def test_truncated_record_and_checksum_corruption_fail_closed(self):
        p = self.peer(self.start())
        self.register(p)
        self.assertIn('status=accepted', p.cmd('RID 1 NEW 800 SELL LIMIT 10100 1 GTC'))
        self.server.stop()
        self.server = None
        intact = self.journal.read_bytes()
        for mode in ('truncate', 'checksum'):
            with self.subTest(mode=mode):
                if mode == 'truncate':
                    data = intact[:-3]
                else:
                    data = bytearray(intact)
                    pos = data.index(b'MEX1|', 1)
                    data[pos + 12] ^= 1
                self.journal.write_bytes(data)
                proc = subprocess.run([EXE, '--port', '0', '--journal', str(self.journal)],
                                      capture_output=True, text=True, timeout=5)
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn('Journal recovery failed', proc.stderr)

    def test_injected_write_error_fail_stops_without_ack(self):
        for mode in ('sync', 'group'):
            with self.subTest(mode=mode):
                self.reset_journal_for_subtest()
                p = self.peer(self.start(stage='write-error', at=2, mode=mode))
                self.register(p)
                self.verify_exit_without_ack(
                    p, 'RID 100 NEW 700 SELL LIMIT 10100 2 GTC', 4)
                self.assertIn('fatal journal mutation failure',
                              self.server.proc.stderr.read())
                p2 = self.peer(self.start(mode=mode))
                self.auth(p2)
                self.assertIn('asks=none', p2.cmd('BOOK'))
                self.assertIn('status=accepted',
                              p2.cmd('RID 100 NEW 700 SELL LIMIT 10100 2 GTC'))

    def test_injected_sync_error_fail_stops_without_ack(self):
        for mode in ('sync', 'group'):
            with self.subTest(mode=mode):
                self.reset_journal_for_subtest()
                p = self.peer(self.start(stage='sync-error', at=2, mode=mode))
                self.register(p)
                self.verify_exit_without_ack(
                    p, 'RID 200 NEW 750 SELL LIMIT 10200 3 GTC', 4)
                self.assertIn('fatal journal', self.server.proc.stderr.read())
                # The record may or may not have reached a kernel buffer before
                # a real sync error. On normal replay, either outcome must be
                # retry-safe; corrupted tails must fail closed.
                p2 = self.peer(self.start(mode=mode))
                self.auth(p2)
                self.assertIn('status=accepted',
                              p2.cmd('RID 200 NEW 750 SELL LIMIT 10200 3 GTC'))
                self.assertIn('asks=10200:3:1', p2.cmd('BOOK'))
                self.assertIn('accepted=1', p2.cmd('STATS'))

    def test_unregistered_idempotent_request_is_rejected(self):
        p = self.peer(self.start())
        self.assertIn('registered_durable_account_required',
                      p.cmd('RID 42 NEW 100 SELL LIMIT 10100 1 GTC'))
        self.assertIn('asks=none', p.cmd('BOOK'))


if __name__ == '__main__':
    unittest.main()
