# Micro Exchange v0.7 — Journal Strategy Benchmark

**Collected (UTC):** 2026-11-09T15:55:09.626358+00:00
**Host:** Linux-6.18.57-x86_64-with-glibc2.41
**Python:** 3.13.5
**TCP_NODELAY:** True
**Repetitions:** 3

## Matched workload measurements

| Mode | Clients | Trials | Median cmd/s | Min–max cmd/s | Median trial p50 ACK (µs) | Median trial p99 ACK (µs) | Syncs across trials | Largest group | Recovery |
|---|---:|---:|---:|---|---:|---:|---|---:|---|
| off | 2 | 3 | 25,134 | 21,761–26,860 | 28.16 | 237.89 | 0, 0, 0 | 0 | not_applicable |
| off | 4 | 3 | 35,871 | 35,375–39,269 | 31.29 | 268.80 | 0, 0, 0 | 0 | not_applicable |
| off | 8 | 3 | 28,717 | 28,032–32,265 | 79.57 | 541.41 | 0, 0, 0 | 0 | not_applicable |
| off | 16 | 3 | 21,852 | 19,413–22,445 | 261.74 | 1,453.21 | 0, 0, 0 | 0 | not_applicable |
| sync | 2 | 3 | 2,081 | 2,051–2,090 | 435.67 | 1,302.64 | 195, 195, 195 | 1 | passed |
| sync | 4 | 3 | 2,084 | 1,708–2,235 | 809.34 | 2,656.29 | 390, 390, 390 | 1 | passed |
| sync | 8 | 3 | 2,290 | 2,141–2,373 | 1,563.15 | 4,440.68 | 780, 780, 780 | 1 | passed |
| sync | 16 | 3 | 2,640 | 2,072–2,916 | 2,714.39 | 7,962.87 | 1560, 1560, 1560 | 1 | passed |
| group | 2 | 3 | 2,537 | 2,429–2,538 | 362.18 | 846.03 | 195, 195, 195 | 1 | passed |
| group | 4 | 3 | 3,008 | 2,805–3,248 | 650.73 | 1,172.77 | 335, 348, 343 | 2 | passed |
| group | 8 | 3 | 4,256 | 4,134–4,545 | 852.92 | 2,192.46 | 465, 432, 437 | 4 | passed |
| group | 16 | 3 | 6,618 | 6,281–6,782 | 1,156.44 | 2,287.12 | 581, 553, 569 | 7 | passed |

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
