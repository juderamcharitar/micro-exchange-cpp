# Historical Windows durability A/B investigation (v0.7 vs v0.8.1)

The user collected five short synthetic trials at 16 local TCP clients per mode and version on Windows 11, with `TCP_NODELAY` enabled and the same scripted workload. The results were **not** recorded by v0.9's synthetic flow replayer.

| Version | Off median cmd/s | Sync median cmd/s | Group median cmd/s |
|---|---:|---:|---:|
| 0.7.0 | 29,124 | 638 | 1,675 |
| 0.8.1 | 29,006 | 2,410 | 6,626 |

Caution: v0.7 strict-sync trials initially reported ~2.1k–2.3k cmd/s, then dropped to ~630 cmd/s for the final three. Its group trials were also consistently much slower than the version 0.8.1 group trials. Both had 1,560 commands and 1,560 syncs per strict-sync trial, with roughly 500–570 syncs per group trial. v0.8.1 replay checks passed; the historical A/B included passing recovery checks for both versions. The observed variance could arise from environment, storage flush cost, scheduler interference or other factors, and the data do **not** prove either version is universally faster.

These numbers are retained as **historical provenance**, not v0.9 performance results. Repeated randomized A/B ordering, direct sync latency instrumentation and recorded hardware/antivirus/storage conditions are needed before attributing causality.
