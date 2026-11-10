"""Black-box integration suite against real localhost TCP sockets and server process."""
import re
import socket
import subprocess
import sys
import time
import unittest

SERVER_PATH = sys.argv[1]
sys.argv = [sys.argv[0]]


class Peer:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.sock.settimeout(2)
        self.pending = b""
        self.expect("HELLO ")

    def close(self):
        self.sock.close()

    def send(self, line):
        self.sock.sendall((line + "\n").encode("ascii"))

    def expect(self, prefix):
        limit = time.monotonic() + 2
        while True:
            nl = self.pending.find(b"\n")
            if nl >= 0:
                line = self.pending[:nl].decode("ascii")
                self.pending = self.pending[nl+1:]
                assert line.startswith(prefix), f"expected {prefix!r}, got {line!r}"
                return line
            if time.monotonic() >= limit:
                raise AssertionError(f"timeout waiting for {prefix}")
            chunk = self.sock.recv(8192)
            if not chunk:
                raise AssertionError("connection closed unexpectedly")
            self.pending += chunk


class TcpTest(unittest.TestCase):
    def setUp(self):
        self.proc = subprocess.Popen(
            [SERVER_PATH, "--port", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        first = self.proc.stdout.readline().strip()
        self.assertRegex(first, r"^READY host=127\.0\.0\.1 port=\d+ version=1\.0\.0$")
        self.port = int(re.search(r"port=(\d+)", first).group(1))
        self.peers = []

    def tearDown(self):
        for peer in self.peers:
            peer.close()
        self.proc.terminate()
        try:
            self.proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.communicate(timeout=3)

    def peer(self):
        p = Peer(self.port)
        self.peers.append(p)
        return p

    def test_matching_and_maker_reports(self):
        maker = self.peer()
        taker = self.peer()
        maker.send("NEW 101 SELL LIMIT 10000 5 GTC")
        self.assertIn("status=accepted", maker.expect("ACK "))
        taker.send("NEW 201 BUY LIMIT 10000 3 IOC")
        self.assertIn("status=accepted", taker.expect("ACK "))
        fill = taker.expect("FILL ")
        self.assertIn("maker=101 taker=201 price_ticks=10000 qty=3", fill)
        self.assertEqual(fill, maker.expect("FILL "))
        maker.send("BOOK")
        self.assertIn("asks=10000:2:1", maker.expect("BOOK "))
        taker.send("CHECK")
        self.assertIn("status=ok", taker.expect("CHECK "))

    def test_reject_cross_client_cancel_and_replace(self):
        a = self.peer()
        b = self.peer()
        a.send("NEW 1 BUY LIMIT 123 3 GTC")
        self.assertIn("accepted", a.expect("ACK "))
        b.send("CANCEL 1")
        self.assertIn("reason=not_order_owner", b.expect("ACK "))
        b.send("REPLACE 1 124 3")
        self.assertIn("reason=not_order_owner", b.expect("ACK "))
        a.send("REPLACE 1 123 2")
        self.assertIn("status=accepted", a.expect("ACK "))
        a.send("CANCEL 1")
        self.assertIn("removed=2", a.expect("ACK "))
        a.send("CHECK")
        self.assertIn("status=ok", a.expect("CHECK "))

    def test_partial_tcp_frames_and_sequencing(self):
        a = self.peer()
        a.sock.sendall(b"PI")
        time.sleep(0.015)
        a.sock.sendall(b"NG\nPING\n")
        first = a.expect("PONG ")
        second = a.expect("PONG ")
        self.assertLess(int(first.split("seq=")[1]), int(second.split("seq=")[1]))

    def test_input_validation_and_reused_order_ids(self):
        a = self.peer()
        a.send("NEW x BUY LIMIT 101 2 GTC")
        self.assertIn("reason=bad_syntax", a.expect("ACK "))
        a.send("NEW 10 BUY LIMIT 101 2 GTC")
        self.assertIn("accepted", a.expect("ACK "))
        a.send("NEW 10 BUY LIMIT 101 2 GTC")
        self.assertIn("rejected", a.expect("ACK "))
        a.send("BOOK 101")
        self.assertIn("reason=bad_depth", a.expect("ACK "))

    def test_long_line_disconnect_does_not_break_server(self):
        a = self.peer()
        a.sock.sendall(b"X" * 600 + b"\n")
        a.sock.settimeout(2)
        self.assertEqual(a.sock.recv(128), b"")
        b = self.peer()
        b.send("PING")
        self.assertTrue(b.expect("PONG "))

    def test_disconnected_owner_cannot_be_spoofed_by_reused_fd(self):
        a = self.peer()
        a.send("NEW 77 SELL LIMIT 10000 1 GTC")
        self.assertIn("accepted", a.expect("ACK "))
        a.close()
        self.peers.remove(a)
        # Server reads EOF on a subsequent select iteration and removes the peer.
        time.sleep(0.08)
        b = self.peer()
        b.send("CANCEL 77")
        self.assertIn("reason=not_order_owner", b.expect("ACK "))


if __name__ == "__main__":
    unittest.main()
