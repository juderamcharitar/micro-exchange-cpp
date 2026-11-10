# Micro Exchange v0.6.0 — C++20 Trading Engine & Exchange Simulator

A **local-only, single-instrument, research simulator** with a deterministic price-time-priority matching engine, a nonblocking TCP gateway, an append-only checksummed journal, a fill-driven position/P&L ledger, conservative pre-trade risk checks, and new **self-trade prevention plus reconnectable research accounts**.

**Not a production exchange, real broker, or secure internet-facing trading service.** Listen address is hardcoded to `127.0.0.1`. No live execution, deposited funds, production risk margin, transport encryption, durable outbound fill delivery, or formal security audit.

## v0.6 — Synthetic market workloads and performance research

**New in v0.6:** a C++ book-depth benchmark (`exchange_depth_bench`), a concurrent Python-to-C++ TCP benchmark (`client/bench_scaling.py`), a self-contained durable-vs-nonjournal comparison, optional **TCP_NODELAY** on server and benchmark clients, repeated independent trial runs, and a Markdown reporting tool (`bench/report_v06.py`). **Existing matching, risk, accounting, journal, identities and self-trade rules are preserved.** The server greeting now reports `version=0.6.0` (instead of the stale `0.5.0` from v0.5.1).

All new datasets are **synthetically constructed**, with no live trading, real-world market queue reconstruction, or exchange venue connectivity. A large number like millions of in-memory operations/second must never be described as TCP matching throughput. All lab results and limitations are documented in [`benchmarks/BENCHMARK_PROTOCOL.md`](benchmarks/BENCHMARK_PROTOCOL.md).

### Windows quick start: two explicitly labeled windows

**Window 1: x64 Native Tools Command Prompt for VS 2022** (required to build).

```cmd
cd /d "C:\Users\YOUR_USERNAME\Downloads\micro-exchange-cpp-v0.6.0\micro-exchange-cpp"
powershell -ExecutionPolicy Bypass -File .\run_windows.ps1
```

Expect **8/8 CTest suites** to pass. This is verified on Linux GCC 14; Windows v0.6 testing remains pending until performed on a Windows machine.

Then run the native C++ depth experiment in that **same x64 window**:

```cmd
.\build\Release\exchange_depth_bench.exe --levels 10,100,1000,5000 --cycles 3000 --repetitions 3 --csv .\benchmarks\depth_windows.csv
```

**Window 2: Regular Windows PowerShell** (no Developer Prompt required; Python 3 is required).

```powershell
cd "C:\Users\YOUR_USERNAME\Downloads\micro-exchange-cpp-v0.6.0\micro-exchange-cpp"
py -3 .\client\bench_scaling.py --pairs 1,2,4,8 --rounds 75 --repeats 3 --mode both --tcp-nodelay --json-output .\benchmarks\scaling_windows.json
py -3 .\bench\report_v06.py --depth .\benchmarks\depth_windows.csv --scaling .\benchmarks\scaling_windows.json --output .\benchmarks\WINDOWS_V060_REPORT.md
```

**Important:** the scaling script starts and stops its **own temporary localhost server processes**. You do **not** need a third terminal or a running server. It uses temporary journals and will not touch `data/commands.journal` or your Alice/Bob credentials. `--tcp-nodelay` suppresses Nagle on both benchmark clients and server; omit it for a separate TCP-default comparison, saving to a distinct file.

### What v0.6 proves and does not prove

- Checks **correct counts, partial matching, empty ending book, invariant integrity, and ledger reconciliation**. Durable trials also verify the recovered state in a restarted server process.
- Provides sample Linux results (not Windows results) under `benchmarks/LINUX_V060_*`, including a generated Markdown report. Each saved result includes runtime environment and methodology; remeasure on Windows before putting numbers on a resume.
- Benchmarks are **synthetic API/loopback measurements**, **not** production latency, actual market flow, or exchange-grade resilience. Repeated trials are exploratory; they do not establish statistically robust platform comparisons by themselves.

### Optional: Interactive v0.6 server

If you want the manual server/client workflow instead of a benchmark, run `start_server_windows.ps1` in **Window 1** and `py -3 .\client\exchange_client.py` in **Window 2**. The launcher optionally accepts `-TcpNoDelay`. Use a fresh v0.6 journal; older WAL cross-version migration is not certified.

## Retained v0.5 / v0.5.1 features

- **Self-trade prevention (STP):** `NEW` and crossing `REPLACE` requests are preflighted against matching-eligible FIFO book liquidity. If the order would match the same owner, the **entire request is rejected before WAL append** with `reason=self_trade_prevention`, without changing book priority or balances. Other-owner fills still work. Rejected requests do not consume an order ID.
- **Registered local accounts:** create a named account with a client-generated 256-bit random capability using `REGISTER <alias> <hex-token>`. Its journal entry stores only the account name and the **SHA-256 digest** of the token. The secret itself is never written into the WAL. `AUTH <alias> <hex-token>` rebinds a new connection to the historical owner, permitting old orders to be cancelled and old P&L to be queried after restart.
- **Session safeguards:** an account can have only one live session; `REGISTER`/`AUTH` must happen before any order mutation in the new connection. Anonymous, one-off client IDs remain supported for backward-compatible research, but **cannot be recovered**.
- **Failure testing:** new tests cover registrations and secrets, same-owner crossing and replacements, denied account takeover, repeated abrupt termination/restart, journal truncation/corruption, concurrent client submissions and reconciliation. SHA-256 is checked against published reference vectors.

## Historical v0.5.1 golden-test patch

The earlier v0.5.1 patch only changed the golden-replay test harness after the first Windows v0.5.0 run captured an empty CLI transcript (5/6 passing). The revised harness is retained in v0.6 and captures exit code, stderr and a transcript diff on failure.

## Manual server/client workflow on Windows

Prerequisites: Visual Studio 2022 Build Tools (Desktop development with C++), CMake, Python 3. From **x64 Native Tools Command Prompt for VS 2022**:

```cmd
cd /d "C:\Users\YOUR_USERNAME\Downloads\micro-exchange-cpp-v0.6.0\micro-exchange-cpp"
powershell -ExecutionPolicy Bypass -File .\run_windows.ps1
```

This builds Release and runs **eight CTest suites**: the six retained matching/network/replay/integrity tests plus the two new workload suites. Linux Release and sanitizer test suites passed in this v0.6 release; Windows must be verified on your machine.

### Start the server

Stop any older v0.2–0.4 server on port 8765 using `Ctrl+C`. Then:

```cmd
powershell -ExecutionPolicy Bypass -File .\start_server_windows.ps1
```

The default WAL is `data/commands.journal` in **this v0.6 project folder**. Use a **fresh journal** for v0.6; do not copy v0.4 or v0.5 WALs in without testing migrations (not certified). An existing, valid v0.6 journal is recovered on restart. To run a separate independent scenario, pass `-Journal .\data\scenario2.journal`.

### Register and log back into an account

Open a **second** PowerShell window, navigate to the v0.6 project folder, then:

```powershell
py -3 .\client\identity.py register alice
```

The helper writes a secret token **before** sending registration, into `data/identities/alice.json` (excluded from Git). Keep it private; on Windows check the folder's user ACL if your PC is shared. Do not copy or paste tokens into GitHub, logs, chat, or screenshots.

Log in to the account and place simulated orders:

```powershell
py -3 .\client\exchange_client.py --credentials .\data\identities\alice.json
```

At the `>` prompt:

```text
NEW 100 SELL LIMIT 10100 5 GTC
ACCOUNT 10200
BOOK
CHECK
QUIT
```

Create a second account in a separate terminal (e.g., `register bob`, then connect using `--credentials .\data\identities\bob.json`) to match opposite-side orders. **One account cannot execute against its own resting orders** in v0.6.

Stop the server with `Ctrl+C`, restart it **with the same journal**, reconnect using Alice's same JSON credentials and run `BOOK`, `ACCOUNT 10200` and `CANCEL 100` (assuming the order still rests). The previously registered owner and remaining inventory will be restored.

The ordinary client still works without `--credentials`; such connections are anonymous test clients that cannot reclaim a previous ID or its orders.

## Project architecture

```text
Python client(s) on 127.0.0.1
    -> TCP message parser / sequencing
    -> optional REGISTER / AUTH (local research capability, journaled digest)
    -> ownership + risk + self-trade preview
    -> write-ahead log: checked, exclusively locked, synced BEFORE mutation
    -> C++ price-time matching engine
    -> fill-driven ledger + accounting reconciliation
    -> ACK / FILL / BOOK / ACCOUNT / LEDGER_CHECK / CHECK responses
```

The journal uses strict ordered replay of accepted mutations. Upon corruption, checksum failure or an incomplete tail the server refuses to start rather than silently truncating history. Only one server can own the journal at a time. Rejected STP and identity login attempts are not committed. Mutations journaled before an ACK can leave an uncertain client outcome on unexpected disconnection; no exactly-once or durable outbound fill-delivery guarantee exists.

**Research units:** all prices are integer ticks, quantities are integer units, and cash flow/P&L is in *tick × quantity*, not dollars. `ACCOUNT 10200` specifies a hypothetical mark, not a live market quote. Positions are allowed to be long or short and have no funded-account margin backing.

## Known limits

- STP is an all-or-nothing preflight, rather than cancel-resting/cancel-aggressor or partial cross policies. It scans potentially matched resting FIFO quantities, adding workload-dependent overhead; v0.6 Windows latency measurements have not been collected yet.
- Reconnect tokens are capability secrets intended for a **trusted local machine only**. No TLS, user password policy, login lockout, encrypted secrets-at-rest, MFA, or multi-user security boundary. A token is effectively account control.
- Only one instrument, one process, max 32 clients; no exchange-grade high availability, snapshots, durable push delivery, idempotent order-request IDs, or account-credential rotation/revocation.
- Historic v0.4 self-trades are deliberately replayable (legacy compatibility) but newly submitted v0.5 self-trades are rejected. Cross-version WAL migration, especially policy-version semantics, is **not independently certified**; use a fresh v0.6 WAL.
- A checksum protects against accidental journal corruption, not malicious modification; producer/protocol behavior and security require further scrutiny.
- The existing performance baseline in `benchmarks/WINDOWS_V031_BASELINE.md` is from an earlier build, **not a v0.6 measurement**.

## Testing and release status

`ctest --test-dir build -C Release --output-on-failure` checks core invariants, live network behavior, journal/replay/risk, registered ownership/reconnect/STP and failure injection, accounting, and golden replay. Code under `tests/` includes randomized deterministic operations (15,000 steps) and new 80-command concurrent-client stress runs. These are small research checks, not exhaustive fuzz testing or formal verification.

Keep the signed-off v0.4 Windows results as an independent milestone; v0.6 results should be measured and reported separately from old Windows baselines.
