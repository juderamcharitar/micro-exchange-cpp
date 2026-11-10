#!/usr/bin/env python3
"""Simple sequential PING->PONG localhost TCP RTT benchmark (not engine latency)."""
import argparse
import socket
import statistics
import time


def percentile(sorted_values, p):
    return sorted_values[min(len(sorted_values) - 1, int((len(sorted_values) - 1) * p))]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--messages", type=int, default=1000)
    args = ap.parse_args()
    if args.messages < 1 or args.messages > 1_000_000:
        ap.error("--messages must be between 1 and 1000000")

    ns = []
    with socket.create_connection(("127.0.0.1", args.port), timeout=5) as s:
        s.settimeout(5)
        with s.makefile("rb") as f:
            hello = f.readline()
            if not hello.startswith(b"HELLO "):
                raise RuntimeError("Unexpected server greeting")
            for _ in range(args.messages):
                start = time.perf_counter_ns()
                s.sendall(b"PING\n")
                response = f.readline()
                end = time.perf_counter_ns()
                if not response.startswith(b"PONG seq="):
                    raise RuntimeError("Unexpected server response: " + repr(response))
                ns.append(end - start)
    ns.sort()
    print("workload=sequential blocking localhost TCP PING/PONG")
    print(f"messages={len(ns)}")
    print(f"mean_rtt_us={statistics.mean(ns)/1000:.2f}")
    print(f"p50_rtt_us={percentile(ns,0.50)/1000:.2f}")
    print(f"p99_rtt_us={percentile(ns,0.99)/1000:.2f}")
    print("NOTE: Includes Python timing, socket stack, request parsing and scheduling;")
    print("      NOT matching engine latency or production throughput.")


if __name__ == "__main__":
    main()
