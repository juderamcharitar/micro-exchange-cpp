"""Sequential end-to-end synthetic order benchmark (localhost; not HFT measurement).

For each round: submit one resting SELL limit order, then one crossing BUY IOC
for one unit; read ACK and FILL. Reports the *Python client-observed* RTT.
Run against a fresh simulator to avoid user order IDs colliding with the journal.
"""
import argparse
import socket
import statistics
import time


def percentile_sorted(values, quantile):
    if not values:
        return float("nan")
    return values[min(len(values) - 1, int((len(values) - 1) * quantile + 0.5))]


class LineReader:
    def __init__(self, sock):
        self.sock = sock
        self.remaining = b""

    def get(self):
        while b"\n" not in self.remaining:
            data = self.sock.recv(8192)
            if not data:
                raise RuntimeError("TCP connection closed while awaiting response")
            self.remaining += data
        line, self.remaining = self.remaining.split(b"\n", 1)
        return line.decode("ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1", choices=["127.0.0.1"])
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--rounds", type=int, default=1000)
    parser.add_argument("--start-id", type=int, default=1000000000)
    args = parser.parse_args()
    if args.rounds <= 0 or args.rounds > 100000 or args.start_id <= 0:
        parser.error("rounds must be 1..100000, start-id positive")
    maker_ns, taker_ns, fill_ns = [], [], []
    with socket.create_connection((args.host, args.port), timeout=10) as sock:
        sock.settimeout(10)
        reader = LineReader(sock)
        greeting = reader.get()
        if not greeting.startswith("HELLO "):
            raise RuntimeError(f"Expected greeting, got {greeting!r}")
        for i in range(args.rounds):
            a = args.start_id + 2 * i
            b = a + 1
            tic = time.perf_counter_ns()
            sock.sendall(f"NEW {a} SELL LIMIT 10000 1 GTC\n".encode("ascii"))
            ack = reader.get()
            if not (ack.startswith("ACK ") and "status=accepted" in ack):
                raise RuntimeError(f"Maker rejected in round {i}: {ack}")
            maker_ns.append(time.perf_counter_ns() - tic)
            tic = time.perf_counter_ns()
            sock.sendall(f"NEW {b} BUY LIMIT 10000 1 IOC\n".encode("ascii"))
            ack = reader.get()
            a_ns = time.perf_counter_ns()
            fill = reader.get()
            f_ns = time.perf_counter_ns()
            if not (ack.startswith("ACK ") and "status=accepted" in ack
                    and fill.startswith("FILL ") and f"maker={a} taker={b}" in fill
                    and "qty=1" in fill):
                raise RuntimeError(f"Unexpected taker response: {ack!r}; {fill!r}")
            taker_ns.append(a_ns - tic)
            fill_ns.append(f_ns - tic)
    def report(key, vals):
        sorted_values = sorted(vals)
        print(f"{key}: mean_us={statistics.mean(vals)/1000:.2f} "
              f"p50_us={percentile_sorted(sorted_values, 0.5)/1000:.2f} "
              f"p99_us={percentile_sorted(sorted_values, 0.99)/1000:.2f}")
    print(f"workload=sequential Python->localhost TCP->matching->ACK/FILL, rounds={args.rounds}")
    report("resting_sell_ACK", maker_ns)
    report("crossing_buy_ACK", taker_ns)
    report("crossing_buy_FILL", fill_ns)
    print("NOTE: Synthetic single-client order RTT with Python/sockets/scheduling. "
          "Durable journaling adds disk sync overhead if enabled. "
          "Not exchange-wire latency or sustained throughput.")


if __name__ == "__main__":
    main()
