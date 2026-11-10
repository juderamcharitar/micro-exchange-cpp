"""Identity, account recovery, STP and corruption/failure injection tests (localhost only)."""
import hashlib
import re
import socket
import subprocess
import sys
import tempfile
import time
import threading
import unittest
from pathlib import Path

SERVER = sys.argv[1]
sys.argv = [sys.argv[0]]
TOKEN_A = 'a1' * 32
TOKEN_B = 'b2' * 32


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.sock.settimeout(3)
        self.buf = b''
        self.line('HELLO ')

    def line(self, prefix):
        while b'\n' not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk: raise AssertionError(f'connection closed awaiting {prefix}')
            self.buf += chunk
        line, self.buf = self.buf.split(b'\n', 1)
        val = line.decode('ascii')
        assert val.startswith(prefix), (prefix, val)
        return val

    def cmd(self, text, prefix='ACK '):
        self.sock.sendall((text + '\n').encode('ascii'))
        return self.line(prefix)

    def close(self):
        try: self.sock.close()
        except OSError: pass


class IdentitySTP(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='mex_v05_')
        self.wal = Path(self.temp.name) / 'journal.log'
        self.proc = None
        self.peers = []

    def tearDown(self):
        self.stop()
        self.temp.cleanup()

    def start(self):
        self.proc = subprocess.Popen([SERVER, '--port', '0', '--journal', str(self.wal)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, bufsize=1)
        line = self.proc.stdout.readline().strip()
        if not line:
            _, err = self.proc.communicate(timeout=3)
            self.fail(f'server failed before READY: {err}')
        self.assertRegex(line, r'^READY host=127\.0\.0\.1 port=\d+ version=1\.0\.0$')
        return int(re.search(r'port=(\d+)', line).group(1))

    def peer(self, port):
        peer = Peer(port)
        self.peers.append(peer)
        return peer

    def stop(self):
        for p in self.peers: p.close()
        self.peers = []
        if self.proc:
            self.proc.terminate()
            try: self.proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill(); self.proc.communicate(timeout=3)
            self.proc = None

    def test_register_login_recovery_trade_and_cancel(self):
        port = self.start()
        alice = self.peer(port)
        bob = self.peer(port)
        reg = alice.cmd(f'REGISTER alice {TOKEN_A}', 'REGISTER ')
        self.assertIn('status=accepted', reg)
        alice_owner = int(re.search(r'owner=(\d+)', reg).group(1))
        self.assertIn('accepted', bob.cmd(f'REGISTER bob {TOKEN_B}', 'REGISTER '))
        self.assertIn('accepted', alice.cmd('NEW 100 SELL LIMIT 10100 5 GTC'))
        self.assertIn('accepted', bob.cmd('NEW 200 BUY LIMIT 10100 2 IOC'))
        bob.line('FILL '); alice.line('FILL ')
        self.assertIn('position=-2', alice.cmd('ACCOUNT 10200', 'ACCOUNT '))
        self.assertIn('position=2', bob.cmd('ACCOUNT 10200', 'ACCOUNT '))
        self.assertIn('status=ok', alice.cmd('LEDGER_CHECK', 'LEDGER_CHECK '))
        self.stop()
        raw = self.wal.read_text('ascii')
        self.assertNotIn(TOKEN_A, raw)
        self.assertNotIn(TOKEN_B, raw)
        self.assertIn(hashlib.sha256(TOKEN_A.encode()).hexdigest(), bytes.fromhex(raw.splitlines()[0].split('|')[3]).decode())
        port = self.start()
        random = self.peer(port)
        self.assertIn('not_order_owner', random.cmd('CANCEL 100'))
        self.assertIn('invalid_credentials', random.cmd(f'AUTH alice {TOKEN_B}'))
        self.assertIn('accepted', random.cmd(f'AUTH alice {TOKEN_A}', 'AUTH '))
        self.assertIn(f'owner={alice_owner}', random.cmd('ACCOUNT 10200', 'ACCOUNT '))
        self.assertIn('position=-2', random.cmd('ACCOUNT 10200', 'ACCOUNT '))
        self.assertIn('removed=3', random.cmd('CANCEL 100'))
        self.assertIn('status=ok', random.cmd('LEDGER_CHECK', 'LEDGER_CHECK '))
        self.stop()
        port = self.start()
        again = self.peer(port)
        self.assertIn('accepted', again.cmd(f'AUTH alice {TOKEN_A}', 'AUTH '))
        self.assertIn('asks=none', again.cmd('BOOK', 'BOOK '))
        self.assertIn('trades=1', again.cmd('STATS', 'STATS '))

    def test_takeover_denied_and_late_auth_denied(self):
        port = self.start()
        a = self.peer(port)
        a.cmd(f'REGISTER alice {TOKEN_A}', 'REGISTER ')
        attacker = self.peer(port)
        self.assertIn('account_already_connected', attacker.cmd(f'AUTH alice {TOKEN_A}'))
        self.assertIn('accepted', attacker.cmd('NEW 77 BUY LIMIT 100 1 GTC'))
        self.assertIn('identity_not_pristine', attacker.cmd(f'AUTH alice {TOKEN_A}'))
        self.assertIn('identity_not_pristine', attacker.cmd(f'REGISTER attacker {TOKEN_B}'))
        self.assertIn('alias_taken', self.peer(port).cmd(f'REGISTER alice {TOKEN_B}'))

    def test_stp_partial_fifo_and_replace(self):
        port = self.start()
        a = self.peer(port)
        b = self.peer(port)
        a.cmd('NEW 10 SELL LIMIT 101 3 GTC')
        b.cmd('NEW 11 SELL LIMIT 101 3 GTC')
        wal_size = self.wal.stat().st_size
        self.assertIn('self_trade_prevention', a.cmd('NEW 12 BUY LIMIT 101 5 IOC'))
        self.assertEqual(wal_size, self.wal.stat().st_size)
        # Only the separate owner's 3 lots can fill; own order comes first in FIFO.
        self.assertIn('accepted', b.cmd('NEW 13 BUY LIMIT 101 3 IOC'))
        b.line('FILL '); a.line('FILL ')
        self.assertIn('accepted', b.cmd('CANCEL 11'))
        self.assertIn('accepted', a.cmd('NEW 21 SELL LIMIT 101 2 GTC'))
        a.cmd('NEW 20 BUY LIMIT 100 2 GTC')
        self.assertIn('self_trade_prevention', a.cmd('REPLACE 20 101 2'))
        self.assertIn('bids=100:2:1', b.cmd('BOOK', 'BOOK '))
        self.assertIn('status=ok', b.cmd('LEDGER_CHECK', 'LEDGER_CHECK '))
        self.stop()
        port = self.start()
        p = self.peer(port)
        self.assertIn('status=ok', p.cmd('CHECK', 'CHECK '))

    def test_repeated_kill_restart_and_concurrent_clients(self):
        port = self.start()
        founder = self.peer(port)
        founder.cmd(f'REGISTER alice {TOKEN_A}', 'REGISTER ')
        founder.cmd('NEW 900 SELL LIMIT 200 7 GTC')
        self.proc.kill()  # deliberately abrupt; WAL was fsynced before ACK
        self.proc.communicate(timeout=3)
        self.proc = None
        for cycle in range(3):
            for old in self.peers: old.close()
            self.peers.clear()
            port = self.start()
            p = self.peer(port)
            self.assertIn('accepted', p.cmd(f'AUTH alice {TOKEN_A}', 'AUTH '))
            self.assertIn('asks=200:7:1', p.cmd('BOOK', 'BOOK '))
            self.assertIn('status=ok', p.cmd('CHECK', 'CHECK '))
            if cycle < 2:
                self.proc.kill()
                self.proc.communicate(timeout=3)
                self.proc = None
        # Four simultaneous senders; this tests serialization, not throughput.
        errors = []
        def producer(i):
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=4) as sock:
                    sock.settimeout(4)
                    with sock.makefile('rb') as input_stream:
                        input_stream.readline()  # HELLO
                        for k in range(20):
                            sock.sendall(f'NEW {1000 + i * 100 + k} BUY LIMIT 100 1 GTC\n'.encode())
                            ack = input_stream.readline().decode('ascii')
                            if 'status=accepted' not in ack:
                                raise AssertionError(ack)
            except Exception as exc:
                errors.append(str(exc))
        threads = [threading.Thread(target=producer, args=(i,)) for i in range(4)]
        for t in threads: t.start()
        for t in threads: t.join(timeout=15)
        self.assertFalse(any(t.is_alive() for t in threads))
        self.assertEqual(errors, [])
        self.assertIn('accepted=81', p.cmd('STATS', 'STATS '))
        self.assertIn('status=ok', p.cmd('LEDGER_CHECK', 'LEDGER_CHECK '))
        self.stop()
        port = self.start()
        v = self.peer(port)
        self.assertIn('accepted=81', v.cmd('STATS', 'STATS '))
        self.assertIn('status=ok', v.cmd('CHECK', 'CHECK '))

    def test_no_journal_refuses_registration(self):
        proc = subprocess.Popen([SERVER, '--port', '0'], stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, bufsize=1)
        try:
            ready = proc.stdout.readline().strip()
            port = int(re.search(r'port=(\d+)', ready).group(1))
            x = Peer(port)
            try: self.assertIn('journal_required', x.cmd(f'REGISTER alice {TOKEN_A}'))
            finally: x.close()
        finally:
            proc.terminate(); proc.communicate(timeout=3)

    def test_faults_truncated_and_corrupt_journal_fail_closed(self):
        port = self.start()
        a = self.peer(port)
        a.cmd(f'REGISTER alice {TOKEN_A}', 'REGISTER ')
        a.cmd('NEW 10 SELL LIMIT 101 4 GTC')
        self.stop()
        good = self.wal.read_bytes()
        for bad in [good + b'MEX1|broken', good[:-1], good[:20] + b'0' + good[21:]]:
            self.wal.write_bytes(bad)
            proc = subprocess.run([SERVER, '--port', '0', '--journal', str(self.wal)],
                                  capture_output=True, text=True, timeout=4)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn('Journal recovery failed', proc.stderr)
        self.wal.write_bytes(good)
        port = self.start()
        p = self.peer(port)
        self.assertIn('accepted', p.cmd(f'AUTH alice {TOKEN_A}', 'AUTH '))
        self.assertIn('status=ok', p.cmd('CHECK', 'CHECK '))


if __name__=='__main__': unittest.main()
