# Windows v0.3.1 benchmark — user-observed baseline

Source: interactive Windows test results supplied by the user on November 9, 2026.
Environment: Visual Studio 2022 Build Tools, MSVC 19.44, Windows x64. Hardware details, disk model, frequency, power profile, CPU affinity and background load not recorded. All results are observed in *different* benchmark scripts and are not directly substitutable.

## In-process synthetic engine benchmark (Release)

- `messages=200000`, `trades=100000`
- `elapsed_seconds=0.0377533`
- `throughput_messages_per_second=5.29755e+06`
- `p50_operation_ns=100`, `p99_operation_ns=300`
- Workload: single-threaded in-memory synthetic add+immediate-execute.

## v0.2 localhost TCP PING/PONG (not matching) — earlier version

- `messages=1000`
- `mean_rtt_us=24.88`, `p50_rtt_us=22.80`, `p99_rtt_us=46.60`
- Workload: sequential blocking Python localhost TCP PING/PONG.

## v0.3.1 localhost order submission (synthetic; WAL possibly enabled)

| Response | mean µs | p50 µs | p99 µs |
|---|---:|---:|---:|
| Resting sell ACK | 312.77 | 288.30 | 683.90 |
| Crossing buy ACK | 299.64 | 276.30 | 611.90 |
| Crossing buy FILL | 300.21 | 276.80 | 613.20 |

Workload: sequential one-client synthetic order round trips; Python client/socket/scheduling and any journal-sync costs included. Journal/path/system settings were not captured in the supplied excerpt, so persistence costs cannot be isolated. This does not measure matching engine-only latency, production throughput, or remote exchange wire-to-wire latency.

**Benchmark comparability:** no v0.4 Windows results exist in this baseline. Rerun equivalent v0.4 commands on the same machine, with comparable journal state and power/network conditions, before claiming improvement or regression.
