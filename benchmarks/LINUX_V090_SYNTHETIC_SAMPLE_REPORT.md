# Micro Exchange v0.9 — Synthetic Order-Flow Research

**Input type:** Generated or externally supplied order-level event CSV replays
**Important:** The generated datasets are synthetic; outputs are NOT empirical market metrics.
The C++ trace replayer does not infer individual order IDs from aggregate L2 snapshots.

## mixed (seed 42, 12,000 simulated events)

- **Event count:** 12,016 including 16 preload events
- **Operations:** 8,779 NEW, 2,368 CANCEL, 869 REPLACE; 1,679 market IOC orders
- **Executions:** 5,410 fill records, 40,056 executed quantity units
- **Execution fingerprint:** `0xb0a625e30bf64fcb`

| Regime | Events | Observed events/s | Aggressive NEW | Trades | Executed qty | Mean spread (ticks) | Mean depth-5 imbalance |
|---|---:|---:|---:|---:|---:|---:|---:|
| calm | 4,000 | 2,246 | 420 | 762 | 2,140 | 1.00 | -0.0469 |
| volatile | 4,000 | 5,390 | 711 | 1,489 | 8,220 | 1.00 | -0.0236 |
| stress | 4,000 | 11,979 | 1,311 | 3,159 | 29,696 | 2.86 | -0.0499 |
| seed | 16 | 0 | 0 | 0 | 0 | 2.00 | 0.1117 |

**Interpretation:** Quotes and depth are sampled after each event. The fill count is matching-engine execution count, not number of distinct trades at an exchange. Arrival rates use phase elapsed simulated time and will vary with the seeded draw.

## Validity boundaries

- The scenario rates, aggressive-order share, price-placement and size distributions are user-defined assumptions.
- No real Kraken order stream or queue-position data were used; aggregate L2 data cannot directly reveal order identities.
- The replayer excludes network latency, WAL synchronization, risk, fees, clearing and account funding.
- Cross-runtime identical traces are not guaranteed because floating-point `log` behavior may differ.
- For defensible research, run several seeds and disclose parameter sensitivity; use normalized empirical observations for calibration later.
