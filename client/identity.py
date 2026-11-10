#!/usr/bin/env python3
"""Provision a persistent LOCAL RESEARCH account capability before sending REGISTER.

Usage: py -3 client/identity.py register alice
The credential JSON is intentionally not kept in Git or the ZIP.
"""
import argparse
import json
import os
import secrets
import socket
from pathlib import Path


def read_one(stream):
    text = stream.readline()
    if not text:
        raise RuntimeError('server disconnected')
    return text.rstrip('\r\n')


def register(alias: str, path: Path, port: int) -> None:
    if not (1 <= len(alias) <= 24 and all(x.isascii() and (x.isalnum() or x == '_') for x in alias)):
        raise SystemExit('Alias must be 1–24 ASCII letters, digits or underscores.')
    token = secrets.token_hex(32)  # OS CSPRNG
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    # Create before sending; a crashed client never loses the generated token.
    with os.fdopen(os.open(str(path), flags, 0o600), 'w', encoding='utf-8') as output:
        json.dump({'account': alias, 'token': token}, output, indent=2)
        output.write('\n')
    try:
        os.chmod(path, 0o600)  # On Windows rely on the current filesystem ACL.
    except OSError:
        pass
    print(f'Credential saved locally to {path} (keep this file private).')
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=5) as sock:
            with sock.makefile('r', encoding='ascii') as r:
                hello = read_one(r)
                if not hello.startswith('HELLO '): raise RuntimeError(hello)
                sock.sendall(f'REGISTER {alias} {token}\n'.encode('ascii'))
                result = read_one(r)
                print(result)
                if 'status=accepted' not in result:
                    print('Registration rejected. Credential file retained; inspect the server response.')
    except OSError as exc:
        print(f'Connection error: {exc}. Keep the credentials until you know registration status.')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('operation', choices=['register'])
    ap.add_argument('alias')
    ap.add_argument('--credentials', type=Path)
    ap.add_argument('--port', type=int, default=8765)
    args = ap.parse_args()
    path = args.credentials or (Path('data') / 'identities' / (args.alias + '.json'))
    register(args.alias, path, args.port)


if __name__ == '__main__': main()
