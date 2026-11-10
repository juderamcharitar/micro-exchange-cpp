# Micro Exchange v0.7 — Journal Strategy Benchmark

**Collected (UTC):** 2026-11-09T16:08:22.241967+00:00
**Host:** Windows-11-10.0.26200-SP0
**Python:** 3.13.7
**TCP_NODELAY:** True
**Repetitions:** 3

## Matched workload measurements

| Mode | Clients | Trials | Median cmd/s | Min–max cmd/s | Median trial p50 ACK (µs) | Median trial p99 ACK (µs) | Syncs across trials | Largest group | Recovery |
|---|---:|---:|---:|---|---:|---:|---|---:|---|
| off | 2 | 3 | 3,878 | 3,832–4,199 | 26.60 | 130.90 | 0, 0, 0 | 0 | not_applicable |
| off | 4 | 3 | 10,527 | 7,505–17,496 | 30.30 | 79.90 | 0, 0, 0 | 0 | not_applicable |
| off | 8 | 3 | 17,844 | 13,875–49,481 | 51.60 | 174.20 | 0, 0, 0 | 0 | not_applicable |
| off | 16 | 3 | 37,943 | 26,279–38,381 | 123.30 | 359.50 | 0, 0, 0 | 0 | not_applicable |
| sync | 2 | 3 | 1,918 | 1,754–2,063 | 324.10 | 737.90 | 195, 195, 195 | 1 | passed |
| sync | 4 | 3 | 2,459 | 2,330–2,644 | 644.10 | 1,304.90 | 390, 390, 390 | 1 | passed |
| sync | 8 | 3 | 2,887 | 2,632–3,007 | 1,296.20 | 2,207.10 | 780, 780, 780 | 1 | passed |
| sync | 16 | 3 | 2,984 | 2,775–3,112 | 2,532.20 | 3,886.70 | 1560, 1560, 1560 | 1 | passed |
| group | 2 | 3 | 1,938 | 1,730–2,367 | 346.20 | 765.00 | 195, 195, 195 | 1 | passed |
| group | 4 | 3 | 2,332 | 2,296–2,346 | 671.80 | 1,389.10 | 390, 390, 389 | 2 | passed |
| group | 8 | 3 | 4,827 | 3,978–4,994 | 695.00 | 1,414.50 | 398, 453, 391 | 3 | passed |
| group | 16 | 3 | 7,220 | 6,951–7,979 | 964.70 | 1,576.80 | 534, 535, 537 | 7 | passed |

## Definitions and interpretation

- Throughput is accepted order mutation commands per second, not independent matching-engine throughput. Trials include Python clients, localhost TCP, parsing, scheduling and selected journal mode.
- `sync` appends and fsyncs each accepted mutation; `group` batches mutations observed in one server event-loop cycle and fsyncs once before emitting success responses; `off` does not journal.
- Median throughput is the median of independent trial-level throughputs. Latency columns are medians of trial-level percentile estimates, not percentiles recomputed from pooled raw samples.
- Group size varies with concurrent ready sockets and pipelining. No wait-window or background journal thread is used; a group of one offers no synchronization savings.
- A replay pass after process termination is performed for each durable trial. Process termination does not establish resilience to power loss, disk-controller write caching, torn writes or filesystem corruption.
- Durability-before-ACK is the intended property; an abrupt disconnect before an ACK still leaves an ambiguous outcome and no client-side exactly-once retry semantics.
- This is an exploratory synthetic benchmark with only a handful of repetitions, no market-order-flow replay and no independent statistical inference.

## Next verification

Run matching sync/group/off conditions on Windows with the same build and TCP settings, record CPU and storage details, and test explicit sync/write failures before treating this mode as operationally robust.
