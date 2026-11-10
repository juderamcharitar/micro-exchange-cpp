# Micro Exchange v0.8.1 — Fault-Tolerant Replay and Idempotent Requests

**C++20 exchange research simulator** with price-time matching, localhost TCP gateway, risk checks, simulated fill accounting, registered account recovery, checksum-validated WAL, event-loop group commit, and deterministic failure testing. **Research software only — not a production exchange or broker.**

## Windows v0.8.1 fault-test hotfix

In v0.8.0, the fault integration suite attempted to delete its temporary
journal between `sync` and `group` subtests *before stopping the previous
server*. Windows correctly rejected deletion with `WinError 32`; Linux allows
unlinking an open file and did not expose the test bug. This patch closes
previous client sockets and stops/waits for the previous server before removing
that disposable journal. **No matching or durability engine changes.**

## What's new in v0.8

1. **Persistent idempotency for registered accounts.** `RID <request_id> NEW ...`, `RID <request_id> CANCEL ...`, and `RID <request_id> REPLACE ...` record the request ID in the MEX1 journal envelope. Replaying a committed request after server restart reconstructs its original response. Sending the exact same `RID` again returns the original ACK/FILL report with `duplicate=1` added to the ACK **without placing another order or executing another trade**. A different command with a previously committed ID returns `reason=idempotency_conflict`. Request IDs are scoped to the registered account. This is opt-in: plain `NEW`/`CANCEL`/`REPLACE` are not retry-deduplicated.
2. **Deterministic failure injection for tests only** with `--fault-stage before-write|before-sync|after-sync|write-error|sync-error --fault-at N`. Crash checkpoints terminate the server at a defined point; `write-error`/`sync-error` exercise fatal storage-error code paths. **These are software failure simulations, not actual power loss or defective hardware.**
3. **Fail-stop hardening.** Both strict and group-commit modes now stop before sending queued success responses on an append/sync error, without an ordinary C++ destructor flushing a failed journal after the fact.
4. **Black-box test suite for uncertain outcomes.** Covers retrying after a committed-but-unacknowledged request, unchanged replay of a fill receipt, pre-write failures, injected sync/write errors, checksum corruption, truncated journal tails, and conflicting request IDs.
5. Archived the user's **v0.7 Windows benchmark report** as `benchmarks/WINDOWS_V070_REPORT.md`. The v0.7 Windows 16-client synthetic group-commit baseline was **7,220 mutations/s** vs **2,984/s** strict sync (three runs per configuration). **This is a v0.7 baseline, not a newly measured v0.8 result.**

## Build and test on Windows

**Window 1 — x64 Native Tools Command Prompt for VS 2022** (where MSVC/CMake work):

```cmd
cd /d "C:\Users\YOUR_USERNAME\Downloads\micro-exchange-cpp-v0.8.1\micro-exchange-cpp"
powershell -ExecutionPolicy Bypass -File .\run_windows.ps1
```

Expect **10/10 CTest suites passed**. The new suite is `exchange_fault_idempotency_integration`. All 10 suites passed on Linux Release and AddressSanitizer/UndefinedBehaviorSanitizer builds. Windows needs validation on your machine.

Do **not** manually start the server while CTest is running. Tests create temporary localhost servers and separate journals.

## Manual registered-account test

**Window 1 — x64 Native Tools Command Prompt** (after the build):

```cmd
powershell -ExecutionPolicy Bypass -File .\start_server_windows.ps1 -Journal .\data\v08_demo.journal -JournalMode group -TcpNoDelay
```

Leave the server window open. If it reports `journal_already_locked_or_unavailable`, stop your old server with `Ctrl+C` rather than deleting the journal.

**Window 2 — regular Windows PowerShell**:

```powershell
cd "C:\Users\YOUR_USERNAME\Downloads\micro-exchange-cpp-v0.8.1\micro-exchange-cpp"
py -3 .\client\identity.py register alice
py -3 .\client\exchange_client.py --credentials .\data\identities\alice.json
```

Inside the connected client, type commands **one at a time**:

```text
RID 100 NEW 900 SELL LIMIT 10100 5 GTC
RID 100 NEW 900 SELL LIMIT 10100 5 GTC
BOOK
RID 101 CANCEL 900
LEDGER_CHECK
QUIT
```

The second identical `RID 100` should return `duplicate=1`, not add a second sell order. `BOOK` should show one resting order of 5 units before cancellation. To verify persistence, create another `RID` order, stop the server using `Ctrl+C`, restart with **the same journal path**, reconnect with Alice's saved credentials, then retry the same `RID`; the original reply should be replayed without another mutation. Never paste or publish the `alice.json` token.

A fresh journal is strongly recommended for each isolated experiment. Account aliases persist in a journal, so `REGISTER alice` will fail with `alias_taken` if Alice is already registered in the chosen journal. Use the existing `alice.json` with that journal instead.

## Dedicated fault test (no manual server)

You can run just the v0.8 new black-box suite after building:

**Window 1 — x64 Native Tools Command Prompt**:

```cmd
ctest --test-dir build -C Release -R exchange_fault_idempotency_integration --output-on-failure
```

The tests themselves start their own servers and create disposable journals. **Do not run `--fault-stage` against your normal journal**: a deliberate process kill can interrupt requests and leave the outcome uncertain.

## What the results mean (and do not mean)

- A successfully acknowledged **journaled mutation** is intended to survive a **process crash**, subject to OS/storage sync semantics. Group commit still withholds all current-group replies until sync succeeds.
- A disconnected or missing ACK means the outcome is **unknown**, even if the journal contains a complete record. Registered accounts can retry the **exact same `RID`** after recovering the server and account. If the journal itself is corrupt, the server refuses startup rather than silently discarding records.
- A request without `RID` does **not** have retry deduplication. Risk rejection before WAL append is not preserved as a committed receipt; the same `RID` may be submitted again after correcting the rejected request.
- Duplicate replay returns the **original** ACK sequence and any recorded taker FILL lines, with `duplicate=1` on the ACK. It is a retransmission of a stored receipt, not a new execution; maker notifications are not re-emitted.
- Receipt storage is currently **in-memory plus full WAL replay**; journal growth, retention, snapshots, and dedup cache limits need further work.
- The fault stages exercise controlled process exits and injected error-return branches, **not** torn sector writes, actual disk-full conditions, power cuts, controller caches, or full filesystem corruption. No guarantee of production-grade exactly-once processing or distributed consensus is implied.
- There are no real funds, broker connections, clearing, margin, fees, or market data feeds. Account cash/P&L is in synthetic tick-units.

## Benchmarks and documentation

The previous Windows performance results are archived in [`benchmarks/WINDOWS_V070_REPORT.md`](benchmarks/WINDOWS_V070_REPORT.md). You can rerun the v0.7-style performance matrix on v0.8 to measure the **new implementation separately**:

**Window 2 — regular Windows PowerShell**, after build and with no manually started server:

```powershell
py -3 .\client\bench_scaling.py --pairs 1,2,4,8 --rounds 75 --repeats 3 --mode all --tcp-nodelay --json-output .\benchmarks\scaling_windows_v08.json
py -3 .\bench\report_v07.py --scaling .\benchmarks\scaling_windows_v08.json --output .\benchmarks\WINDOWS_V080_SCALING_REPORT.md
```

The old workload does not use `RID`, so it measures compatibility/overhead of the revised server, **not** the deduplication protocol's throughput. See [`docs/FAULT_MODEL_V080.md`](docs/FAULT_MODEL_V080.md), [`DESIGN.md`](DESIGN.md), and [`CHANGELOG.md`](CHANGELOG.md).
