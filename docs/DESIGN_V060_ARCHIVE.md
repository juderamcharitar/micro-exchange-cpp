# Micro Exchange v0.6.0 — Architecture and invariants

## Ownership model

Each TCP connection starts with a newly allocated anonymous owner ID. It can remain anonymous, or **before trading**:

- `REGISTER <alias> <64-hex-secret>` binds a new persistent owner. Python's `identity.py` uses `secrets.token_hex(32)` and writes the secret to a local, Git-ignored credential file **before** sending REGISTER. The WAL writes only `REGISTERED <alias> <SHA256(secret)>` under that owner's ID.
- `AUTH <alias> <64-hex-secret>` verifies SHA-256 digest and binds a fresh TCP connection to the original owner ID. It cannot take over an owner that currently has a live connection; cannot occur after trades/orders have begun on the new connection; not a WAL mutation.

`client_sockets_` maps authenticated owners to connected maker sockets, so fills on reattached sessions can be delivered. The historical book and ledger already reference the original owner ID, allowing `CANCEL` and `ACCOUNT` to work after restart. A stolen plaintext capability grants access to the local account; neither the TCP protocol nor the filesystem is a production-grade authentication boundary.

## STP and side effects

Before appending to WAL, gateway calls `MatchingEngine::would_self_trade()` on incoming `NEW` or `REPLACE`, excluding no liquidity from opposite side. The preview scans sorted opposite-side levels and same-price FIFO entries, decrementing remaining quantity against other-owner orders and rejecting the whole order if the incoming order would reach the owner's own maker liquidity. A replacement that retains queue position cannot cross a valid, uncrossed book; crossing replacements use the same preflight. Rejections preserve book/ledger/WAL, and don't reserve the incoming order ID.

Replaying a legacy v0.4 journal bypasses the new STP filter to avoid changing the historical execution record; v0.5-origin STP-rejected requests are excluded, but legacy engine-level rejection paths can still be journaled and deterministically replayed. Migration of legacy data remains uncertified.

## Durable sequencing

A request that passes the gateway prechecks is written to a checksummed MEX1 record and `fsync` / `_commit` completed **before** book mutation. WAL writer uses an OS single-writer file lock. Recovery validates monotonic record sequence, checksum, registered account uniqueness, book invariants and ledger reconciliation. The sequence includes read-only commands and failed requests in memory but only accepted mutations in WAL, so WAL sequence numbers can have gaps. A truncated/corrupt record fails closed.

Crashing after fsync but before client ACK yields an uncertain client outcome, not an exactly-once guarantee. The client needs a request-id/idempotency and durable execution-report outbox for stronger claims. A recorded `REGISTERED` entry includes digest but never the plaintext token; registration helper pre-saves the capability to reduce credential-loss risk across a crash/ACK gap.

## Accounting and risk

Price/time matching remains single-instrument and integer tick-based. Ledger posts two offsetting legs per execution. Mark-to-market is simulated, in tick×quantity units, via weighted-average cost; no real cash settlement. Existing worst-case owner position ±100,000 units, order quantity, gross open quantity and notional controls remain; no margin or financing constraints. `LEDGER_CHECK` verifies conservation and one ledger trade per matching execution.

## Test boundaries

Six CTest suites including SHA-256 FIPS-known vectors, existing randomized engine and network tests, WAL/risk, owner recovery, STP preview, repeated process kill/restart, corrupted/truncated records, concurrent clients, accounting and golden replay. Windows compilation/runtime must be checked by user. Synthetic in-memory throughput and localhost RTT are different metrics and earlier results do not represent v0.5 performance.

## Golden replay on Windows (v0.5.1)

The original `cmake/check_replay.cmake` uses `execute_process` output capture. The first MSVC v0.5.0 run reported an empty captured transcript despite a zero reported exit status. The underlying cause is not established. The test now preferentially invokes `tests/test_replay_golden.py` for explicit stdout/stderr and exit-code capture, preserving byte-normalized exact transcript comparison. No matching logic changed.

## v0.6 bounded performance experiments

**C++ engine-only depth test:** an independently constructed, noncrossing book at 10–5,000 price levels per side, and repeated cycles comprising insert, two one-unit partial fills, insert/reduce/cancel. Capture per-operation latency and throughput under `steady_clock` with correctness assertions. Only the matching engine is timed; **no TCP, journal, account ownership, or risk checks**. Changes in timing here cannot be interpreted as full gateway performance.

**Multi-client gateway test:** two independent TCP connections per worker pair. Each pair sends sell makers and crossing IOC buys plus a periodic passive bid amendment/cancel. Distinct owners avoid inadvertent self-trades. Pairs execute concurrently; each connection uses a serial request-response pattern. Every run asserts exact trade count and mutation counters, zero book remainder, engine and ledger checks. WAL-on trials also terminate the process, reopen the same journal, and recheck the same state.

**Durability comparison:** no journal vs synchronous per-command `fsync` / `_commit` to a fresh isolated journal, rather than misleadingly comparing only `PING/PONG` latency. Nonjournal mode is **not durable** and should never be used for meaningful persisted sessions. Fresh processes keep trials independent.

**TCP transport option:** `--tcp-nodelay` sets `TCP_NODELAY` on each accepted connection (the Python stress benchmark sets the same on its client sockets). This suppresses Nagle buffering, but does **not** disable all OS scheduling delay or guarantee low latency. Default server setting remains unchanged. The benchmark records the choice.

**Caveats:** A single-threaded `select()` server supports at most 32 clients. Anonymous benchmark owners are ephemeral, but journals replay their original owner IDs. Synthetic event sequences intentionally avoid real order arrival patterns, price discovery, queue estimates, market events, spreads, stochastic liquidity provision, margin and real P&L. Results are local engineering evidence only. See `benchmarks/BENCHMARK_PROTOCOL.md`.
