# v0.6 Benchmarks — Scope, Protocol, and Threats to Validity

This is an **exploratory software performance experiment**. Its design evolved during development and is **not preregistered**. Results depend on compiler, CPU scheduling, filesystem sync behavior, network stack, clock overhead, power plan, and workload. Do not generalize single-machine measurements to exchange performance.

## Experiment A — engine depth scaling

Implementation: `bench/depth_benchmark.cpp`, binary `exchange_depth_bench`.

- Independent in-memory `MatchingEngine` instance for each depth/repetition; **no TCP, no risk gateway, no journal, no ledger**.
- Preload *d* bid levels and *d* ask levels at noncrossing integer-tick prices, each with one quantity-unit order. Test 10, 100, 1,000, and 5,000 price levels per side by default in Windows instructions.
- After preloading, each cycle comprises **six operations**: insert 2-unit sell maker at 15,000; IOC market buy for 1; another IOC market buy for 1; insert a 2-unit buy at 9,500; reduce to 1 unit at the same price; cancel remaining 1 unit.
- Validates maker IDs, prices, two partial fills and cancellation results at every cycle, then validates global invariants, resting count and trade totals.
- Sample individual operation durations using `steady_clock`; total throughput includes timing calls and correctness assertion work inside loops, excludes preload. Report pooled p50/p99 and the median per-repetition throughput.
- This is **not an empirical market event stream**. "Depth" means the number of resting price levels, not the size of a real queue or L2 snapshot. This workload intentionally operates near the inside and at a fixed price offset; it is not a full sweep of all levels.

## Experiment B — concurrent localhost gateway

Implementation: `client/bench_scaling.py`.

- Server binds to `127.0.0.1` and picks an available ephemeral port; each trial has a **fresh server process** and an isolated temporary journal when durable mode is used. The user does not need to start their usual journal/server.
- Two persistent anonymous clients form a worker pair. Maker submits 1-unit sell limit at 10,000; taker submits 1-unit crossing buy IOC. Every fifth round, buyer creates a 2-unit passive bid at 9,000, reduces to 1, and cancels it. All pair owners are distinct.
- For each trial, require all expected accepts, fills, exact trade count, zero final resting orders, no crossed/invalid book, and successful ledger check. For durable trials, terminate the process, reopen the **same journal in a fresh process**, and reassert counts, book and accounting.
- Record individual client-observed command-to-ACK and buy-command-to-FILL RTT using Python's `perf_counter_ns`, aggregate p50 and p99, and compute total accepted mutations divided by elapsed time across all worker pairs. Excludes client connection/setup, initial server spawn, journal replay, post-run queries and teardown.
- At most 12 worker pairs / 24 clients are offered in this script to stay under the gateway's **32 total client hard cap** and leave room for control queries. **Concurrency is not throughput saturation**: thread scheduling, sequential dependencies within each pair, and Python instrumentation influence the results.
- Modes: `--mode off` means no journal; `--mode durable` means full checked append plus `fflush` + `fsync`/`_commit` **per accepted mutation**. They otherwise run equivalent order workloads in separate processes.
- `--tcp-nodelay` explicitly enables TCP_NODELAY on client sockets and accepted server sockets. Without it, both use operating system defaults, allowing TCP buffering/delayed ACK interactions; interpret very large p99 spikes with that in mind. Never compare journal modes across different TCP_NODELAY settings.
- Use `--repeats 3` or more for independent process-level replications. The reports show each run, not unearned precision; they are **not** confidence intervals.

## How to run on Windows (no existing server required for Experiment B)

### Window 1: x64 Native Tools Command Prompt for VS 2022

```cmd
cd /d "C:\Users\YOUR_USERNAME\Downloads\micro-exchange-cpp-v0.6.0\micro-exchange-cpp"
powershell -ExecutionPolicy Bypass -File .\run_windows.ps1
.\build\Release\exchange_depth_bench.exe --levels 10,100,1000,5000 --cycles 3000 --repetitions 3 --csv .\benchmarks\depth_windows.csv
```

### Window 2: Regular PowerShell

```powershell
cd "C:\Users\YOUR_USERNAME\Downloads\micro-exchange-cpp-v0.6.0\micro-exchange-cpp"
py -3 .\client\bench_scaling.py --pairs 1,2,4,8 --rounds 75 --repeats 3 --mode both --tcp-nodelay --json-output .\benchmarks\scaling_windows.json
py -3 .\bench\report_v06.py --depth .\benchmarks\depth_windows.csv --scaling .\benchmarks\scaling_windows.json --output .\benchmarks\WINDOWS_V060_REPORT.md
```

These benchmark commands **start and stop temporary local servers by themselves**, so your existing v0.5/0.6 live server can stay closed. The compiled server accepts `--tcp-nodelay` in ordinary interactive use as well; the Windows launcher has an optional `-TcpNoDelay` switch.

To compare TCP defaults against `TCP_NODELAY`, rerun with `--mode both` *without* `--tcp-nodelay`, save to a **different JSON output path**, and avoid mixing results.

## Result provenance

- `LINUX_V060_DEPTH_SAMPLE.csv` and `LINUX_V060_SCALING_NODELAY_SAMPLE.json` are measurements from the assistant's Linux container on November 9, 2026, not a Windows machine and not a production environment.
- `LINUX_V060_SCALING_SAMPLE.json` is the first exploratory TCP-default run without `TCP_NODELAY`, kept separately to illustrate a potential transport artifact. It has only one trial per condition.
- `LINUX_V060_REPORT_SAMPLE.md` was generated from the sample Linux CSV + repeated TCP_NODELAY JSON, not hand-written.
- Windows numbers require running the commands above **on the user's Windows system**. No Windows v0.6 performance has been measured yet.

## Known gaps

Actual exchange/replay fidelity requires market-sourced order-event traces, realistic distribution of price levels/queue depth, cancellations and participant behavior. Public Level 2 snapshots do not identify individual FIFO queue positions or guaranteed historical fills. The current system does **not** supply real-world order matching performance, financial profit forecasts, submicrosecond network latency, or fault-tolerant production operation.
