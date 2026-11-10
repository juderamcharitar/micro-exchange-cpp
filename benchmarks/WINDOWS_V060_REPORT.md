# Micro Exchange v0.6 — Measured Benchmark Report

**UTC collection timestamp:** 2026-11-09T15:46:49.826100+00:00
**Platform:** Windows-11-10.0.26200-SP0
**Python:** 3.13.7
**TCP_NODELAY (both client and server):** True

## 1. C++ in-memory engine vs resting-book depth

| Book levels per side | Preloaded resting orders | Timed operations | Median operations/s | p50 (ns) | p99 (ns) |
|---:|---:|---:|---:|---:|---:|
| 10 | 20 | 54000 | 5,850,043.88 | 100 | 300 |
| 100 | 200 | 54000 | 6,421,920.15 | 100 | 200 |
| 1000 | 2000 | 54000 | 6,521,739.13 | 100 | 200 |
| 5000 | 10000 | 54000 | 6,615,701.26 | 100 | 200 |

This engine benchmark preloads a noncrossing book, then repeats a **scripted** six-operation cycle: a passive sell, two partial fills, a passive bid, a same-price quantity decrease, and cancellation. It checks invariants and exact trade counts. It excludes TCP, risk checks, and journaling. Per-operation timings include steady-clock overhead. No actual exchange event stream is replayed.

## 2. Python-to-C++ loopback TCP execution and replay

| Journal | Repeat | Clients | Mutations | Trades | Mutations/s | ACK p50 (µs) | ACK p99 (µs) | Post-restart replay |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| off | 1 | 2 | 195 | 75 | 22,861.56 | 29.00 | 158.20 | not_applicable |
| off | 2 | 2 | 195 | 75 | 4,956.04 | 28.20 | 166.30 | not_applicable |
| off | 3 | 2 | 195 | 75 | 4,916.56 | 26.90 | 164.40 | not_applicable |
| off | 1 | 4 | 390 | 150 | 10,468.31 | 29.30 | 111.30 | not_applicable |
| off | 2 | 4 | 390 | 150 | 8,478.54 | 32.00 | 113.00 | not_applicable |
| off | 3 | 4 | 390 | 150 | 8,538.01 | 30.30 | 147.70 | not_applicable |
| off | 1 | 8 | 780 | 300 | 17,194.25 | 53.00 | 202.50 | not_applicable |
| off | 2 | 8 | 780 | 300 | 24,144.72 | 54.70 | 195.00 | not_applicable |
| off | 3 | 8 | 780 | 300 | 18,454.64 | 52.40 | 201.00 | not_applicable |
| off | 1 | 16 | 1560 | 600 | 47,044.77 | 142.30 | 418.80 | not_applicable |
| off | 2 | 16 | 1560 | 600 | 32,658.53 | 162.70 | 455.30 | not_applicable |
| off | 3 | 16 | 1560 | 600 | 33,827.08 | 137.70 | 422.20 | not_applicable |
| durable | 1 | 2 | 195 | 75 | 1,872.46 | 297.70 | 710.30 | passed |
| durable | 2 | 2 | 195 | 75 | 1,903.75 | 328.10 | 628.10 | passed |
| durable | 3 | 2 | 195 | 75 | 2,444.77 | 281.70 | 818.00 | passed |
| durable | 1 | 4 | 390 | 150 | 2,553.51 | 564.50 | 1,220.20 | passed |
| durable | 2 | 4 | 390 | 150 | 3,064.51 | 579.10 | 1,380.00 | passed |
| durable | 3 | 4 | 390 | 150 | 2,615.49 | 568.30 | 1,399.50 | passed |
| durable | 1 | 8 | 780 | 300 | 3,003.06 | 1,176.50 | 2,076.50 | passed |
| durable | 2 | 8 | 780 | 300 | 3,054.71 | 1,188.00 | 2,196.50 | passed |
| durable | 3 | 8 | 780 | 300 | 2,936.18 | 1,230.90 | 2,214.80 | passed |
| durable | 1 | 16 | 1560 | 600 | 2,933.59 | 2,486.60 | 5,447.80 | passed |
| durable | 2 | 16 | 1560 | 600 | 3,009.27 | 2,499.70 | 4,638.10 | passed |
| durable | 3 | 16 | 1560 | 600 | 2,589.12 | 2,662.90 | 5,404.20 | passed |

The TCP benchmark uses two clients per pair (one passive seller, one buyer). Each pair alternates successful crossing trades with occasional passive add/reduce/cancel operations. Concurrent clients have distinct identities. The timing window excludes setup, server startup, restart, journal replay, and post-run checks. Each trial uses its own fresh process and optional journal. Every trial verifies exact trades, an empty final book, engine invariants, and ledger reconciliation; durable trials also reverify after server restart.

## 3. Interpretation and validity boundaries

- **Not wire latency:** Python, loopback TCP, parsing, system scheduling, and (in durable mode) per-mutation flush/sync are included.
- **Not a production throughput claim:** this is a bounded 32-connection single-process simulator; the tested client counts may be lower.
- **Not empirical market data:** traffic is synthetic, with no order IDs, queue estimates, or exchange execution timestamps from an actual market.
- **Not independent inference:** three independent trials per scenario provide an exploratory baseline, not statistical evidence of a stable performance difference. Repeat trials, record hardware/environment, and report dispersion before making claims.
- **Accounting differs from a broker:** cash flows use tick-units, no funded balances, margin, clearing, fees, or real venue connectivity.
- **Transport comparison requires matched settings:** compare runs with Nagle/delayed ACK behavior versus TCP_NODELAY independently; do not mix them to attribute journaling effects.

## 4. Suggested next measurements

Repeat each row across warm/cold runs; archive clock, CPU/OS build, SSD and power-plan settings; add deeper resting orders per level, bursts, backpressure, disconnected makers and explicit durability failure injection. Future empirical replay requires appropriate order-level data or transparent queue models; these results are deliberately synthetic.
