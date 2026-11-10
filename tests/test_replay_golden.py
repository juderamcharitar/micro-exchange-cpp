"""Exact scripted CLI replay assertion with useful cross-platform diagnostics.

Unlike CMake execute_process OUTPUT_VARIABLE on some Windows configurations,
Python subprocess captures stdout/stderr explicitly and can explain empty output.
"""
from __future__ import annotations

import difflib
import pathlib
import subprocess
import sys


def normalized(s: str) -> str:
    return s.replace("\r\n", "\n").replace("\r", "\n")


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: test_replay_golden.py CLI INPUT EXPECTED", file=sys.stderr)
        return 2
    cli, input_path, expected_path = map(pathlib.Path, sys.argv[1:])
    for kind, path in (("executable", cli), ("input", input_path), ("expected", expected_path)):
        if not path.is_file():
            print(f"FAIL: {kind} not found: {path}", file=sys.stderr)
            return 2
    if input_path.stat().st_size == 0:
        print(f"FAIL: input file is empty: {input_path}", file=sys.stderr)
        return 2
    try:
        result = subprocess.run(
            [str(cli.resolve()), "--script", str(input_path.resolve())],
            cwd=str(input_path.resolve().parent.parent),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=20,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"FAIL: replay executable couldn't run: {exc}", file=sys.stderr)
        return 1
    actual = normalized(result.stdout)
    expected = normalized(expected_path.read_text(encoding="utf-8"))
    if result.returncode != 0 or actual != expected:
        print("FAIL: deterministic replay differs from golden transcript", file=sys.stderr)
        print(f"exe: {cli.resolve()}", file=sys.stderr)
        print(f"input: {input_path.resolve()} ({input_path.stat().st_size} bytes)", file=sys.stderr)
        print(f"exit_code={result.returncode} stdout_chars={len(actual)} stderr_chars={len(result.stderr)}", file=sys.stderr)
        if result.stderr:
            print("--- stderr ---\n" + result.stderr[:4000], file=sys.stderr)
        if not actual:
            print("--- stdout is empty; run the executable manually with --script to isolate redirection ---", file=sys.stderr)
        else:
            diff = list(difflib.unified_diff(
                expected.splitlines(keepends=True), actual.splitlines(keepends=True),
                fromfile="golden", tofile="actual",
            ))
            print("--- transcript diff ---\n" + "".join(diff[:100]), file=sys.stderr)
        return 1
    print(f"PASS: replay matches golden transcript ({len(actual)} chars)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
