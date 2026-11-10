# v0.9.0 release validation — November 9, 2026

## Verified in the provided Linux execution environment

- CMake Release (`GCC 14.2.0`): **12/12 CTest suites passed**, including all v0.8.1 recovery and identity tests.
- Debug build with AddressSanitizer + UndefinedBehaviorSanitizer (`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`, `UBSAN_OPTIONS=halt_on_error=1`): **12/12 CTest suites passed**.
- Deterministic generation of a 300-event example trace and exact replay verification against the generated summary and fill tape.
- Generated a separate 12,000-event three-regime synthetic Linux research sample with seed 42 and generated a Markdown report from the JSON summary.
- A local executable smoke test and report generation succeeded. **Windows v0.9 binary compatibility and tests have not yet been checked by the user.**

## Synthetic 12,000-event Linux sample

Inputs: `--seed 42 --events 12000 --profile mixed`. The output includes a separate 16-order liquidity initialization. See `benchmarks/LINUX_V090_SYNTHETIC_TRACE.csv`, `benchmarks/LINUX_V090_SYNTHETIC_SAMPLE.json` and `benchmarks/LINUX_V090_SYNTHETIC_SAMPLE_REPORT.md`.

| Phase | Synthetic events | Observed simulated event rate/s | Engine fill records | Mean post-event spread (ticks) |
|---|---:|---:|---:|---:|
| calm | 4,000 | 2,245.6 | 762 | 1.0047 |
| volatile | 4,000 | 5,390.2 | 1,489 | 1.0000 |
| stress | 4,000 | 11,978.5 | 3,159 | 2.8630 |

Totals: **12,016** records including seed orders, **5,410** fill records, **40,056** executed synthetic quantity units. These are **generated scenario outcomes**, not trading venue throughput or empirical market data.

## Remaining validation and research gaps

- Verify the build and 12/12 suites with Windows MSVC on the target machine.
- Repeat synthetic traces across a grid of seeds for distribution sensitivity, not just seed 42.
- Measure market-state differences against **actual** normalized Kraken observed quotes/trades; explicitly keep unknown order queue states unknown.
- Validate replay timestamps and realistic event arrivals from documented exchange venue semantics before claiming empirical event replay.
- Evaluate storage durability and TCP throughput with the separate v0.8.1 gateway benchmarks; v0.9's in-process replay bypasses both.
