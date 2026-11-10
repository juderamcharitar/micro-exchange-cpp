"""Black-box double-entry, position/P&L and replay tests maintained through v0.5.0."""
import re
import socket
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SERVER = sys.argv[1]
sys.argv = [sys.argv[0]]


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=4)
        self.sock.settimeout(4)
        self.pending = b""
        self.line("HELLO ")

    def line(self, prefix):
        while b"\n" not in self.pending:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise AssertionError("server closed before reply")
            self.pending += chunk
        value, self.pending = self.pending.split(b"\n", 1)
        msg = value.decode("ascii")
        if not msg.startswith(prefix):
            raise AssertionError(f"wanted {prefix!r}, got {msg!r}")
        return msg

    def cmd(self, msg, prefix="ACK "):
        self.sock.sendall((msg + "\n").encode("ascii"))
        return self.line(prefix)

    def close(self):
        self.sock.close()


class LedgerIntegration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mex_ledger_")
        self.wal = Path(self.temp.name) / "account.journal"
        self.proc = None
        self.peers = []

    def tearDown(self):
        self.stop()
        self.temp.cleanup()

    def start(self):
        self.proc = subprocess.Popen(
            [SERVER, "--port", "0", "--journal", str(self.wal)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1)
        ready = self.proc.stdout.readline().strip()
        if not ready:
            _, error = self.proc.communicate(timeout=3)
            self.fail(f"server exited before READY: {error}")
        self.assertRegex(ready, r"^READY host=127\.0\.0\.1 port=\d+ version=1\.0\.0$")
        return int(re.search(r"port=(\d+)", ready).group(1))

    def connect(self, port):
        peer = Peer(port)
        self.peers.append(peer)
        return peer

    def stop(self):
        for peer in self.peers:
            peer.close()
        self.peers.clear()
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.communicate(timeout=3)
            self.proc = None

    def test_cash_position_mark_and_realized_pnl(self):
        port = self.start()
        a = self.connect(port)
        b = self.connect(port)
        self.assertIn("accepted", a.cmd("NEW 10 SELL LIMIT 100 6 GTC"))
        self.assertIn("accepted", b.cmd("NEW 11 BUY LIMIT 100 6 IOC"))
        self.assertIn("trade_seq=1", b.line("FILL "))
        a.line("FILL ")
        aa = a.cmd("ACCOUNT 110", "ACCOUNT ")
        bb = b.cmd("ACCOUNT 110", "ACCOUNT ")
        for k, v in {"position": "-6", "cash_flow_tick_units": "600.00",
                     "avg_entry_ticks": "100.00", "realized_pnl_tick_units": "0.00",
                     "unrealized_pnl_tick_units": "-60.00", "equity_tick_units": "-60.00"}.items():
            self.assertIn(f"{k}={v}", aa)
        self.assertIn("position=6", bb)
        self.assertIn("cash_flow_tick_units=-600.00", bb)
        self.assertIn("equity_tick_units=60.00", bb)

        self.assertIn("accepted", b.cmd("NEW 12 SELL LIMIT 110 6 GTC"))
        self.assertIn("accepted", a.cmd("NEW 13 BUY LIMIT 110 6 IOC"))
        a.line("FILL ")
        b.line("FILL ")
        aa = a.cmd("ACCOUNT", "ACCOUNT ")
        bb = b.cmd("ACCOUNT", "ACCOUNT ")
        self.assertIn("position=0", aa)
        self.assertIn("cash_flow_tick_units=-60.00", aa)
        self.assertIn("realized_pnl_tick_units=-60.00", aa)
        self.assertIn("avg_entry_ticks=0.00", aa)
        self.assertIn("position=0", bb)
        self.assertIn("cash_flow_tick_units=60.00", bb)
        self.assertIn("realized_pnl_tick_units=60.00", bb)
        self.assertIn("status=ok trades=2 accounts=2", a.cmd("LEDGER_CHECK", "LEDGER_CHECK "))
        self.assertIn("status=ok", b.cmd("CHECK", "CHECK "))

    def test_weighted_cost_basis_partial_close(self):
        port = self.start()
        seller = self.connect(port)
        buyer = self.connect(port)
        seller.cmd("NEW 100 SELL LIMIT 101 5 GTC")
        buyer.cmd("NEW 101 BUY LIMIT 101 5 IOC")
        buyer.line("FILL "); seller.line("FILL ")
        seller.cmd("NEW 102 SELL LIMIT 103 5 GTC")
        buyer.cmd("NEW 103 BUY LIMIT 103 5 IOC")
        buyer.line("FILL "); seller.line("FILL ")
        buyer.cmd("NEW 104 SELL LIMIT 104 4 GTC")
        seller.cmd("NEW 105 BUY LIMIT 104 4 IOC")
        seller.line("FILL "); buyer.line("FILL ")
        a = buyer.cmd("ACCOUNT 105", "ACCOUNT ")
        self.assertIn("position=6", a)
        self.assertIn("avg_entry_ticks=102.00", a)
        self.assertIn("realized_pnl_tick_units=8.00", a)
        self.assertIn("unrealized_pnl_tick_units=18.00", a)
        self.assertIn("equity_tick_units=26.00", a)
        self.assertIn("status=ok trades=3 accounts=2", seller.cmd("LEDGER_CHECK", "LEDGER_CHECK "))

    def test_replay_restores_account_conservation(self):
        port = self.start()
        x = self.connect(port)
        y = self.connect(port)
        x.cmd("NEW 1 SELL LIMIT 600 3 GTC")
        y.cmd("NEW 2 BUY LIMIT 600 3 IOC")
        y.line("FILL "); x.line("FILL ")
        self.assertIn("status=ok trades=1 accounts=2", y.cmd("LEDGER_CHECK", "LEDGER_CHECK "))
        self.stop()
        port = self.start()
        z = self.connect(port)
        self.assertIn("status=ok trades=1 accounts=2", z.cmd("LEDGER_CHECK", "LEDGER_CHECK "))
        self.assertIn("trades=1", z.cmd("STATS", "STATS "))
        self.assertIn("status=ok", z.cmd("CHECK", "CHECK "))
        # No reconnectable accounts yet: fresh client starts with a new identity.
        self.assertIn("position=0", z.cmd("ACCOUNT", "ACCOUNT "))

    def test_self_cross_rejected_without_ledger_mutation(self):
        port = self.start()
        p = self.connect(port)
        self.assertIn("accepted", p.cmd("NEW 50 SELL LIMIT 100 3 GTC"))
        self.assertIn("self_trade_prevention", p.cmd("NEW 51 BUY LIMIT 100 3 IOC"))
        self.assertIn("asks=100:3:1", p.cmd("BOOK", "BOOK "))
        self.assertIn("position=0", p.cmd("ACCOUNT", "ACCOUNT "))
        self.assertIn("status=ok trades=0 accounts=0", p.cmd("LEDGER_CHECK", "LEDGER_CHECK "))

    def test_position_risk_includes_filled_inventory(self):
        port = self.start()
        a = self.connect(port)
        b = self.connect(port)
        for i in range(10):
            self.assertIn("accepted", a.cmd(f"NEW {1000+i} SELL LIMIT 100 10000 GTC"))
            self.assertIn("accepted", b.cmd(f"NEW {2000+i} BUY LIMIT 100 10000 IOC"))
            b.line("FILL "); a.line("FILL ")
        self.assertIn("position=-100000", a.cmd("ACCOUNT", "ACCOUNT "))
        self.assertIn("position=100000", b.cmd("ACCOUNT", "ACCOUNT "))
        self.assertIn("risk_position_long", b.cmd("NEW 9999 BUY LIMIT 100 1 GTC"))
        self.assertIn("risk_position_short", a.cmd("NEW 9998 SELL LIMIT 100 1 GTC"))
        # Reducing exposure is still permitted.
        self.assertIn("accepted", b.cmd("NEW 9000 SELL LIMIT 101 1 GTC"))
        self.assertIn("status=ok trades=10 accounts=2", a.cmd("LEDGER_CHECK", "LEDGER_CHECK "))

    def test_bad_mark_is_nonmutating(self):
        port = self.start()
        p = self.connect(port)
        self.assertIn("bad_mark_price", p.cmd("ACCOUNT -1"))
        self.assertIn("bad_mark_price", p.cmd("ACCOUNT 0"))
        self.assertIn("status=ok trades=0 accounts=0", p.cmd("LEDGER_CHECK", "LEDGER_CHECK "))


if __name__ == "__main__":
    unittest.main()
