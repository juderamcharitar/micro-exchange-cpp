# Quant Developer Resume & Interview Notes — v1.0.0

## High-confidence two-bullet version

**Micro Exchange | C++20, TCP/IP, CMake, Python, Microstructure**

- Engineered a C++20 price-time-priority limit-order matching engine and multi-client TCP exchange simulator with IOC/GTC execution, partial fills, cancel/replace, account-level risk checks, self-trade prevention, and position/ledger reconciliation.
- Added checksummed write-ahead journaling, strict-sync/group-commit persistence, replay recovery, and request-ID deduplication; validated failure scenarios through automated integration tests and 25/25 deterministic multi-seed synthetic-trace replays on Windows.

## If there is room for a third bullet

- Built a configurable synthetic market-flow research lab (5 scenarios × 5 seeds), measuring spread, quote imbalance, fill counts, and executed quantity; documented sensitivity to cancellation/aggression assumptions and limitations of inferring order-level behavior from aggregate L2 data.

## Optional carefully attributed performance bullet

- Measured **7,220 vs 2,984 commands/s** for group commit versus strict-sync WAL at 16 localhost clients on the historical v0.7 Windows synthetic benchmark (2.42× median throughput, three short runs per mode); documented environmental disk-sync variability in later A/B tests.

Only use the optional performance bullet with its workload/version and caveat. Do **not** claim 7,220 commands/s for v1.0 or claim the separate in-process simulator measures network performance. Windows 13/13 CTest pass is not independently evidenced in the supplied v1.0 materials; the 25/25 Windows research replays are documented.

## Interview prompts to rehearse

1. Explain FIFO time priority, when replacing an order resets queue position, and why IOC partially fills or cancels its residual.
2. Explain durability-before-ACK; the difference between a process crash and power loss; why a lost ACK leaves uncertain client state and why idempotent request IDs help.
3. Explain group commit's batching/latency tradeoff and why its throughput benefit changes with concurrency and storage sync costs.
4. Explain precisely why synthetic event arrival timestamps changed while fills did not under the +50% arrival scenario.
5. Explain what aggregate Level 2 snapshots reveal (quoted prices and sizes) and what they cannot identify (individual order IDs, hidden queue priority, order intent).
