# Micro Exchange v1.0 — Multi-seed Synthetic Sensitivity Study

- **Host:** Windows 11 (AMD64)
- **Events per run:** 3,000 (plus 16 preload events)
- **Regime profile:** mixed
- **Seeds:** 11, 23, 37, 41, 53
- **Executable SHA-256:** `48afa40f1de5a34cb2b325093882766f8339bb8068d918f6a49ac8ec7e3b0100`
- **Replay:** exact generated/replayed summaries and trade tapes verified for 25 runs

All inputs are **synthetic**. Each number below is the median across independent seeded
runs, not a confidence interval or a production performance benchmark. The scenario
changes are deliberately imposed assumptions, not parameters fitted from market data.

## Scenario comparisons

| Scenario | Simulated events/s | Fill records / event | Executed qty / event | Mean spread (ticks) | Mean L1 imbalance | Cancellation share | Aggressive NEW share |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline | 4,093 | 0.464 | 3.526 | 1.789 | -0.1231 | 0.1937 | 0.2090 |
| arrivals 150pct | 6,139 | 0.464 | 3.526 | 1.789 | -0.1231 | 0.1937 | 0.2090 |
| cancels plus 10pp | 4,086 | 0.294 | 2.266 | 4.054 | -0.0538 | 0.2873 | 0.2003 |
| aggression plus 10pp | 4,182 | 0.452 | 3.256 | 5.032 | -0.0035 | 0.1903 | 0.2993 |
| max size 150pct | 4,093 | 0.471 | 5.297 | 1.626 | -0.0542 | 0.1937 | 0.2090 |

**Simulated events/s** is derived from generated timestamp spans, not timed engine throughput.
Because arrivals scale only event timestamps, fixed-event-count matching and fill metrics
remain unchanged under the arrivals-only scenario for a given seed.

### Dispersion across seeds

Ranges below are observed **minimum–maximum across seeds**, not statistical confidence intervals.

| Scenario | Fill/event range | Spread mean range (ticks) | Mean depth-5 imbalance range |
|---|---:|---:|---:|
| baseline | 0.443–0.487 | 1.556–2.679 | -0.1169–0.0755 |
| arrivals 150pct | 0.443–0.487 | 1.556–2.679 | -0.1169–0.0755 |
| cancels plus 10pp | 0.276–0.320 | 3.431–4.460 | -0.0884–0.0339 |
| aggression plus 10pp | 0.436–0.458 | 4.611–5.936 | -0.1010–0.0187 |
| max size 150pct | 0.437–0.498 | 1.545–2.745 | -0.0892–0.0904 |

## Interpretation and scope

- Fill records are individual matching-engine executions; several can arise from one taker order.
- Generated event timestamps are simulated interarrival times; throughput is NOT measured here.
- The matching engine's in-memory simulator omits WAL, TCP, margin, fees and live venue execution.
- The replay check establishes exact trace reproducibility for each generated run on this runtime.
- Results depend on seeded simulation and a deliberately simple event-generation process.
- Observational calibration would require separately normalized, source-verified data
  and matched observation rules; no real Kraken data is bundled with this release.
