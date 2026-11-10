# Changelog

## v1.0.0 — Synthetic sensitivity research and release preparation

- Configurable, bounded synthetic generation parameters: arrival rate scale, maximum order size scale, aggressive-NEW share shift, and cancellation share shift.
- Backward-compatible `MEXFLOW1` replay (including exact legacy summary) and self-describing `MEXFLOW2` traces for all v1.0 configurations.
- Added best-quote imbalance and midprice-change metrics, separate from depth-five imbalance.
- Added `sim/experiment_v10.py`: 5 named scenarios, paired seeds, exact generated/replayed summary and fill-tape verification, executable/tape fingerprints, JSON/CSV/Markdown exports, and per-seed dispersion.
- Optional diagnostics against separately normalized aggregate best-quote observations; no claim of empirical order-level reconstruction or parameter fitting.
- Added 13th CTest suite, clearer Windows commands and a GitHub-ready CI/release methodology.
- Retained existing matching-engine, TCP gateway, account, WAL/group-commit, risk, ledger and fault-injection features and prior sample results.


## v0.9.0 — Synthetic order-flow generator and event replay

- Added independent C++ `exchange_flow_sim` executable with seeded stochastic event arrivals and configurable calm / volatile / stress / mixed scenarios.
- Added strict MEXFLOW1 CSV replayer, optional fill tape, per-regime JSON summary, execution fingerprint and exact expected-summary verification.
- Added separate normalized aggregate L2 observation reader and Markdown synthetic-flow report script; neither fabricates real order events.
- Added two cross-platform CTest suites for deterministic replay, malformed input, model summaries and aggregate-L2 schema validation.
- Preserved all v0.8.1 matching/WAL/identity/ledger/fault-tolerance functionality; no changes to trading service protocol or WAL format.
- Archived Windows v0.7 vs v0.8.1 variability investigation with explicit limitations.
- Linux Release and sanitizer tests are part of the release gate; Windows verification remains a separate user run.


## v0.8.1 — November 9, 2026

- Fixed a Windows-only fault-test cleanup error (`WinError 32`): the second
  subtest previously tried to delete the test WAL while the preceding test
  server still had it open. The test now closes client sockets, terminates and
  waits for the server process, and only then unlinks the WAL between sync and
  group-commit scenarios.
- No changes to matching, WAL serialization, trading semantics, account logic,
  or fault-injection behavior. Updated the version string to `0.8.1` consistently.
- Retest Windows on the user's MSVC environment; Linux CTest and sanitizers
  are revalidated for this patch.

## v0.8.0 — November 9, 2026

- Added opt-in `RID <request_id> NEW|CANCEL|REPLACE ...` for registered owners, journaling the request ID with the command and replaying stored receipts after restart. Exact retry returns original ACK/FILL with `duplicate=1`; conflicting command reuses are rejected.
- Added deterministic, test-only WAL faults at before-write, before-sync and after-sync checkpoints, plus injected write/sync error paths. No success reply is transmitted when persistence becomes uncertain.
- Hardened server fail-stop handling on journal append/sync errors to avoid accidental destructor-triggered flushes after a failed operation.
- Added `exchange_fault_idempotency_integration` as the 10th CTest suite: post-sync/pre-ACK crash in both modes, repeated order and fill receipts, write/sync error fail-stop, checksum/truncation recovery tests, duplicate conflicts.
- Archived the user's completed v0.7 Windows comparison report in `benchmarks/WINDOWS_V070_REPORT.md`, retaining measured Windows throughput/latency as historical v0.7 results (not v0.8 measurements).
- Verified 10/10 tests on Linux Release and sanitizer-enabled builds; Windows testing pending.

## v0.7.0 — November 9, 2026

- Introduced experimental event-loop group commit; measured its throughput effect against strict per-command sync in matched Linux and Windows synthetic tests.
- Added `JOURNAL_STATS` and journal-mode scaling reports; retained the earlier matching and replay functionality.

Historical versions: [`docs/CHANGELOG_PRE_V070.md`](docs/CHANGELOG_PRE_V070.md).
