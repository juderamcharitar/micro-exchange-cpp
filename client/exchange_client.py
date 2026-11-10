#!/usr/bin/env python3
"""Standard-library example client for Micro Exchange v0.8 (localhost only).

Examples:
  python client/exchange_client.py
  python client/exchange_client.py --script examples/network_orders.txt
"""
import argparse
import json
import socket
import threading
import time
from pathlib import Path


def receive_forever(sock: socket.socket) -> None:
    try:
        with sock.makefile("r", encoding="ascii", newline="\n") as stream:
            for line in stream:
                print(f"< {line.rstrip()}", flush=True)
    except (OSError, UnicodeError):
        pass


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--script", type=Path)
    ap.add_argument("--credentials", type=Path, help="JSON from identity.py; AUTH before trading")
    args = ap.parse_args()
    with socket.create_connection(("127.0.0.1", args.port), timeout=5) as sock:
        sock.settimeout(None)
        if args.credentials:
            credentials = json.loads(args.credentials.read_text(encoding="utf-8"))
            alias = credentials["account"]
            token = credentials["token"]
            with sock.makefile("r", encoding="ascii", newline="\n") as greeting:
                hello = greeting.readline().strip()
                if not hello.startswith("HELLO "):
                    raise RuntimeError(f"Bad server greeting: {hello}")
                print(f"< {hello}")
                sock.sendall(f"AUTH {alias} {token}\n".encode("ascii"))
                response = greeting.readline().strip()
                print(f"< {response}")
                if not response.startswith("AUTH ") or "status=accepted" not in response:
                    raise RuntimeError("Account login failed; no trading commands sent")
            print(f"Authenticated account {alias} (token not displayed)")
        reader = threading.Thread(target=receive_forever, args=(sock,), daemon=True)
        reader.start()
        if args.script:
            lines = args.script.read_text(encoding="utf-8").splitlines()
            for line in lines:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                print(f"> {line}")
                sock.sendall((line + "\n").encode("ascii"))
                time.sleep(0.05)  # readability only; not needed for correctness
            time.sleep(0.25)
        else:
            print("Micro Exchange client. Type REGISTER/AUTH/RID/NEW/CANCEL/REPLACE/BOOK/STATS/CHECK/ACCOUNT/LEDGER_CHECK/JOURNAL_STATS/PING/QUIT")
            while True:
                try:
                    line = input("> ").strip()
                except (KeyboardInterrupt, EOFError):
                    break
                if not line:
                    continue
                if len(line.encode("ascii")) > 512:
                    print("Command too long (512-byte limit)")
                    continue
                sock.sendall((line + "\n").encode("ascii"))
                if line.upper() == "QUIT":
                    break
        try:
            sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        reader.join(timeout=0.5)


if __name__ == "__main__":
    main()
