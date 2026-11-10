"""Black-box WAL/restart + pre-trade risk integration tests. No 3rd-party deps."""
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

SERVER = sys.argv[1]
sys.argv = [sys.argv[0]]


class Peer:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=3)
        self.socket.settimeout(3)
        self.pending = b""
        self.line("HELLO ")

    def line(self, prefix):
        while b"\n" not in self.pending:
            data = self.socket.recv(65536)
            if not data:
                raise AssertionError("connection closed")
            self.pending += data
        text, self.pending = self.pending.split(b"\n", 1)
        text = text.decode("ascii")
        if not text.startswith(prefix):
            raise AssertionError((prefix, text))
        return text

    def cmd(self, command, prefix="ACK "):
        self.socket.sendall((command + "\n").encode())
        return self.line(prefix)

    def close(self):
        self.socket.close()


class TestJournalRisk(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory(prefix="micro_exchange_wal_")
        self.journal = Path(self.folder.name) / "records.journal"
        self.proc = None
        self.peers = []

    def tearDown(self):
        self.stop()
        self.folder.cleanup()

    def start(self):
        self.proc = subprocess.Popen(
            [SERVER, "--port", "0", "--journal", str(self.journal)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        ready = self.proc.stdout.readline().strip()
        if not ready:
            # A premature exit was previously hidden by an empty stdout line.
            # Include the server's actual recovery error to diagnose Windows failures.
            _out, err = self.proc.communicate(timeout=3)
            self.fail(f"server exited before READY (exit={self.proc.returncode}): {err.strip()}")
        self.assertRegex(ready, r"^READY host=127\.0\.0\.1 port=\d+ version=1\.0\.0$")
        port = int(re.search(r"port=(\d+)", ready).group(1))
        return port

    def connect(self, port):
        p = Peer(port)
        self.peers.append(p)
        return p

    def stop(self):
        for p in self.peers:
            p.close()
        self.peers = []
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.communicate(timeout=3)
            self.proc = None

    def test_replay_book_stats_and_prevent_owner_reuse(self):
        port = self.start()
        a = self.connect(port)
        self.assertIn("accepted", a.cmd("NEW 11 SELL LIMIT 10100 5 GTC"))
        self.assertIn("accepted", a.cmd("REPLACE 11 10100 4"))
        self.assertIn("asks=10100:4:1", a.cmd("BOOK", "BOOK "))
        baseline = a.cmd("STATS", "STATS ")
        self.assertIn("accepted=1", baseline)
        self.assertIn("replaced=1", baseline)
        self.assertIn("status=ok", a.cmd("CHECK", "CHECK "))
        self.stop()
        self.assertGreater(self.journal.stat().st_size, 0)
        port = self.start()
        b = self.connect(port)
        self.assertIn("asks=10100:4:1", b.cmd("BOOK", "BOOK "))
        restored = b.cmd("STATS", "STATS ")
        self.assertIn("accepted=1", restored)
        self.assertIn("replaced=1", restored)
        self.assertIn("not_order_owner", b.cmd("CANCEL 11"))
        self.assertIn("status=ok", b.cmd("CHECK", "CHECK "))
        self.assertIn("accepted", b.cmd("NEW 20 BUY LIMIT 10000 10 GTC"))
        self.assertIn("bids=10000:10:1", b.cmd("BOOK", "BOOK "))
        self.stop()
        port = self.start()
        c = self.connect(port)
        self.assertIn("asks=10100:4:1", c.cmd("BOOK", "BOOK "))
        self.assertIn("bids=10000:10:1", c.cmd("BOOK", "BOOK "))

    def test_risk_does_not_change_book_or_journal(self):
        p = self.connect(self.start())
        self.assertIn("risk_order_quantity", p.cmd("NEW 1 BUY LIMIT 100 10001 GTC"))
        self.assertIn("risk_order_notional", p.cmd("NEW 2 BUY LIMIT 999999 1000 GTC"))
        self.assertIn("risk_market_quantity", p.cmd("NEW 3 BUY MARKET 0 501 IOC"))
        self.assertIn("accepted", p.cmd("NEW 4 SELL LIMIT 10100 5 GTC"))
        before_size = self.journal.stat().st_size
        self.assertIn("risk_order_quantity", p.cmd("REPLACE 4 10100 10001"))
        self.assertEqual(before_size, self.journal.stat().st_size)
        self.assertIn("asks=10100:5:1", p.cmd("BOOK", "BOOK "))
        self.assertIn("rejected=0", p.cmd("STATS", "STATS "))

    def test_checksum_recovery_fails_closed(self):
        p = self.connect(self.start())
        p.cmd("NEW 9 BUY LIMIT 100 10 GTC")
        self.stop()
        raw = self.journal.read_bytes()
        self.journal.write_bytes(raw[:-18] + b"x" + raw[-17:])
        proc = subprocess.run([SERVER, "--port", "0", "--journal", str(self.journal)],
                              capture_output=True, text=True, timeout=4)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Journal recovery failed", proc.stderr)

    def test_second_server_cannot_write_same_journal(self):
        self.start()
        second = subprocess.run([SERVER, "--port", "0", "--journal", str(self.journal)],
                                capture_output=True, text=True, timeout=4)
        self.assertNotEqual(second.returncode, 0)
        self.assertIn("Journal recovery failed", second.stderr)

    def test_executions_continue_sequence_after_restart(self):
        port = self.start()
        seller = self.connect(port)
        buyer = self.connect(port)
        self.assertIn("accepted", seller.cmd("NEW 400 SELL LIMIT 500 5 GTC"))
        self.assertIn("accepted", buyer.cmd("NEW 401 BUY LIMIT 500 2 IOC"))
        first = buyer.line("FILL ")
        self.assertIn("trade_seq=1", first)
        seller.line("FILL ")
        self.stop()
        port = self.start()
        next_buyer = self.connect(port)
        self.assertIn("accepted", next_buyer.cmd("NEW 402 BUY LIMIT 500 3 IOC"))
        second = next_buyer.line("FILL ")
        self.assertIn("trade_seq=2", second)
        self.assertIn("asks=none", next_buyer.cmd("BOOK", "BOOK "))
        self.assertIn("status=ok", next_buyer.cmd("CHECK", "CHECK "))

    def test_outstanding_quantity_risk(self):
        p = self.connect(self.start())
        for i in range(10):
            self.assertIn("accepted", p.cmd(f"NEW {100+i} BUY LIMIT 100 10000 GTC"))
        self.assertIn("risk_open_quantity", p.cmd("NEW 200 BUY LIMIT 100 1 GTC"))
        self.assertIn("status=ok", p.cmd("CHECK", "CHECK "))
        self.assertIn("accepted", p.cmd("CANCEL 100"))
        self.assertIn("accepted", p.cmd("NEW 201 BUY LIMIT 100 1 GTC"))

    def test_truncated_tail_fails_closed(self):
        p = self.connect(self.start())
        p.cmd("NEW 4 BUY LIMIT 100 10 GTC")
        self.stop()
        with self.journal.open("ab") as f:
            f.write(b"MEX1|123|")
        proc = subprocess.run([SERVER, "--port", "0", "--journal", str(self.journal)],
                              capture_output=True, text=True, timeout=4)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Journal recovery failed", proc.stderr)


if __name__ == "__main__":
    unittest.main()
