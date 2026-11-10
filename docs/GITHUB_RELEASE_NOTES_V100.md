# Micro Exchange v1.0.0 — Research Release

**C++20 matching engine, local TCP gateway, and reproducible synthetic microstructure research lab.**

This release combines price-time FIFO matching (market/limit, IOC/GTC, partial fills, cancel/replace), multi-client local TCP order entry, registered-account controls, self-trade prevention, a checksummed write-ahead journal, strict-sync and event-loop group-commit modes, failure injection, and request-ID idempotency for retry safety.

**New in v1.0:** bounded synthetic order-flow tuning, versioned event traces, multi-seed sensitivity experiments, exact generated/replayed summary and trade-tape checks, and diagnostics for normalized top-of-book observations. The synthetic flow runner is separate from TCP and WAL performance measurements.

**Verified evidence:** 13/13 Linux Release and sanitizer test suites (release preparation); a Windows 11 experiment with 5 scenarios × 5 seeds (25/25 replay-verified), 3,000 generated events per run. Windows 13-suite CTest transcript has not yet been archived. The Windows study's data are in `benchmarks/WINDOWS_V100_SYNTHETIC_STUDY.md` and `.json`.

**Illustrative synthetic results:** the median per-run mean spread rose from 1.789 to 4.054 ticks when the cancellation scenario shifted +10 percentage points, while fills/event fell from 0.464 to 0.294. Changing the simulated arrival rate by +50% did not affect fills: the model currently treats interarrival timestamps as exogenous and has no elapsed-time-dependent book response.

**Scope:** research/education only. No actual Kraken orders, market-data calibration, alpha claim, real-money execution, or production-grade power-loss guarantees. Earlier Linux/Windows TCP/WAL benchmarks are historical and not measures of this in-memory research study. Use the methodology and reproducibility notes in `docs/RESEARCH_METHOD_V100.md`.

**Reproduce (Windows):** build via `run_windows.ps1` in an x64 Visual Studio developer prompt; then in PowerShell run:

```powershell
py -3 .\sim\experiment_v10.py --executable .\build\Release\exchange_flow_sim.exe --output-dir .\benchmarks\windows_v100 --seeds 11,23,37,41,53 --events 3000 --profile mixed
```

**Licensing:** choose and add an appropriate license before release. None has been selected by default.
