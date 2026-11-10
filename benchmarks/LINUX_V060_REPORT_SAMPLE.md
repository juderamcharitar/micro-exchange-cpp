# Micro Exchange v0.6 — Measured Benchmark Report

**UTC collection timestamp:** 2026-11-09T15:34:30.152930+00:00
**Platform:** Linux-6.18.57-x86_64-with-glibc2.41
**Python:** 3.13.5
**TCP_NODELAY (both client and server):** True

## 1. C++ in-memory engine vs resting-book depth

| Book levels per side | Preloaded resting orders | Timed operations | Median operations/s | p50 (ns) | p99 (ns) |
|---:|---:|---:|---:|---:|---:|
| 10 | 20 | 54000 | 8,532,225.03 | 60 | 140 |
| 100 | 200 | 54000 | 7,946,100.72 | 61 | 131 |
| 1000 | 2000 | 54000 | 8,372,953.69 | 60 | 120 |
| 5000 | 10000 | 54000 | 7,177,188.03 | 60 | 210 |

This engine benchmark preloads a noncrossing book, then repeats a **scripted** six-operation cycle: a passive sell, two partial fills, a passive bid, a same-price quantity decrease, and cancellation. It checks invariants and exact trade counts. It excludes TCP, risk checks, and journaling. Per-operation timings include steady-clock overhead. No actual exchange event stream is replayed.

## 2. Python-to-C++ loopback TCP execution and replay

| Journal | Repeat | Clients | Mutations | Trades | Mutations/s | ACK p50 (µs) | ACK p99 (µs) | Post-restart replay |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| off | 1 | 2 | 195 | 75 | 19,279.33 | 33.13 | 155.49 | not_applicable |
| off | 2 | 2 | 195 | 75 | 26,377.34 | 29.32 | 152.56 | not_applicable |
| off | 3 | 2 | 195 | 75 | 16,555.93 | 21.49 | 211.88 | not_applicable |
| off | 1 | 4 | 390 | 150 | 47,517.23 | 25.34 | 191.63 | not_applicable |
| off | 2 | 4 | 390 | 150 | 50,237.44 | 28.81 | 136.12 | not_applicable |
| off | 3 | 4 | 390 | 150 | 38,755.97 | 33.11 | 232.37 | not_applicable |
| off | 1 | 8 | 780 | 300 | 40,903.24 | 71.80 | 313.72 | not_applicable |
| off | 2 | 8 | 780 | 300 | 39,816.70 | 74.78 | 285.64 | not_applicable |
| off | 3 | 8 | 780 | 300 | 43,879.25 | 66.07 | 287.60 | not_applicable |
| off | 1 | 16 | 1560 | 600 | 29,146.40 | 214.76 | 857.17 | not_applicable |
| off | 2 | 16 | 1560 | 600 | 27,586.99 | 210.57 | 964.02 | not_applicable |
| off | 3 | 16 | 1560 | 600 | 25,460.38 | 243.65 | 959.12 | not_applicable |
| durable | 1 | 2 | 195 | 75 | 2,629.45 | 342.50 | 829.24 | passed |
| durable | 2 | 2 | 195 | 75 | 2,971.15 | 299.01 | 831.66 | passed |
| durable | 3 | 2 | 195 | 75 | 2,893.64 | 315.62 | 634.11 | passed |
| durable | 1 | 4 | 390 | 150 | 2,712.28 | 665.41 | 1,602.55 | passed |
| durable | 2 | 4 | 390 | 150 | 2,840.96 | 579.51 | 1,516.35 | passed |
| durable | 3 | 4 | 390 | 150 | 3,350.51 | 547.58 | 1,590.34 | passed |
| durable | 1 | 8 | 780 | 300 | 3,376.33 | 1,111.74 | 2,559.58 | passed |
| durable | 2 | 8 | 780 | 300 | 2,949.86 | 1,199.56 | 3,519.60 | passed |
| durable | 3 | 8 | 780 | 300 | 2,993.75 | 1,209.45 | 2,924.78 | passed |
| durable | 1 | 16 | 1560 | 600 | 3,154.88 | 2,408.12 | 4,621.26 | passed |
| durable | 2 | 16 | 1560 | 600 | 3,143.72 | 2,366.18 | 5,661.84 | passed |
| durable | 3 | 16 | 1560 | 600 | 3,224.10 | 2,326.83 | 4,510.59 | passed |

The TCP benchmark uses two clients per pair (one passive seller, one buyer). Each pair alternates successful crossing trades with occasional passive add/reduce/cancel operations. Concurrent clients have distinct identities. The timing window excludes setup, server startup, restart, journal replay, and post-run checks. Each trial uses its own fresh process and optional journal. Every trial verifies exact trades, an empty final book, engine invariants, and ledger reconciliation; durable trials also reverify after server restart.

## 3. Interpretation and validity boundaries

- **Not wire latency:** Python, loopback TCP, parsing, system scheduling, and (in durable mode) per-mutation flush/sync are included.
- **Not a production throughput claim:** this is a bounded 32-connection single-process simulator; the tested client counts may be lower.
- **Not empirical market data:** traffic is synthetic, with no order IDs, queue estimates, or exchange execution timestamps from an actual market.
- **Not independent inference:** one run per scenario is a baseline, not statistical evidence of a stable performance difference. Repeat trials, record hardware/environment, and report dispersion before making claims.
- **Accounting differs from a broker:** cash flows use tick-units, no funded balances, margin, clearing, fees, or real venue connectivity.
- **Transport comparison requires matched settings:** compare runs with Nagle/delayed ACK behavior versus TCP_NODELAY independently; do not mix them to attribute journaling effects.

## 4. Suggested next measurements

Repeat each row across warm/cold runs; archive clock, CPU/OS build, SSD and power-plan settings; add deeper resting orders per level, bursts, backpressure, disconnected makers and explicit durability failure injection. Future empirical replay requires appropriate order-level data or transparent queue models; these results are deliberately synthetic.
