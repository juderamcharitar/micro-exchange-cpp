# Changelog

## v0.6.0 — Performance experiment framework

- Added `exchange_depth_bench`: six-operation synthetic maker, partial fill, passive bid, replace and cancel benchmark across independently preseeded book-depth scenarios; invariants and execution counts checked per trial. CSV output and three-repetition median throughput reporting.
- Added `client/bench_scaling.py`: parallel pairs of real Python TCP clients trading against the localhost C++ server, with isolated server processes, optional durable WAL, repeated trials, ACK/FILL timings, exact trade and account reconciliation checks, and recovery assertions after process restart.
- Added optional server/client `TCP_NODELAY` for controlled investigation of delayed small-message responses; server default behavior unchanged.
- Added reproducible report generator, Linux sample measurements, benchmark protocol and Windows commands (x64 compiler vs regular PowerShell clearly separated).
- Updated server protocol greeting from the old stale `0.5.0` string to `0.6.0` and updated associated test expectations. Matching engine, original WAL semantics, risk and ledger algorithms remain unchanged.
- Linux GCC 14 Release: eight CTest suites passed, including new depth workload and concurrent network/journal/replay tests. Windows v0.6 verification pending.
- Results are exploratory synthetic controlled workloads, not actual venue performance, market-data replay or live trading.

## v0.5.1 — Windows golden replay test harness diagnostics

- In the first Windows MSVC v0.5.0 test, core, TCP, WAL/risk, identity/STP, and ledger passed (5/6), but the golden replay test returned an empty stdout capture.
- Switch the golden replay CTest to an explicit Python subprocess-based assertion when Python is available (fallback to original CMake for environments without Python).
- Assert nonempty input, check exit code, and report stderr, actual output length, and diff if there is a mismatch.
- No trading engine or protocol code changed. Windows confirmation remains outstanding.

## v0.5.0 — Exchange integrity and recoverable local research accounts

- Added pre-commit, FIFO-aware self-trade prevention for `NEW` and crossing `REPLACE`.
- Added registered account alias + 256-bit local capability credentials, stored as SHA-256 digest in the checksummed WAL; registration helper pre-saves the token in a Git-ignored JSON file.
- Added reconnectable account owner IDs, preventing simultaneous active sessions per owner and requiring authentication before trading.
- Added `exchange_identity_stp_integration` suite: restart/recovery, ownership reclaim, account queries/cancel, corrupted journal, repeated SIGKILL/restart cycles, concurrent client traffic, and negative cases.
- Added SHA-256 golden vectors to core test suite and updated previous self-cross ledger expectations.
- Retained older matching, network, risk, accounting, and golden replay suites.
- Linux GCC 14 Release: 6/6 CTest suites passed. Windows MSVC compilation/testing **pending**, not inferred from Linux results.
- Earlier Windows v0.3.1 performance baseline retained unchanged; do **not** present as v0.5 metrics.

## Previous versions

v0.1.0: in-memory matching engine, deterministic replay, unit tests and benchmark.

v0.2.0: localhost TCP gateway, multi-client messages, Python client and TCP tests.

v0.3.1: durable mutation WAL, strict recovery and simulated pre-trade risk; Windows append hotfix.

v0.4.0: fill-driven ledger, inventory and realized/unrealized P&L accounting, conservative position bound and reconciliation.
