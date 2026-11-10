# v1.0.0 release verification record

## Completed Linux verification

- Platform: Linux x86_64, GCC via CMake; C++20 Release build.
- `ctest --test-dir build --output-on-failure`: **13/13 passed**.
- Debug build with AddressSanitizer and UndefinedBehaviorSanitizer: **13/13 passed**, including the research integration suite.
- New test `exchange_research_v100_integration` exercises non-default settings, format/version compatibility, replay, input rejection, multiple seeds/scenarios, exact fill tapes and a clearly marked synthetic L2 comparison fixture.
- The inherited engine, gateway, WAL, account, fault-injection, matching, scenario and replay suites passed.
- A reproducible 25-run synthetic research sample (5 seeds × 5 configurations, 3,000 events per run) is included under `benchmarks/EXAMPLE_V100_SYNTHETIC_*`.

## Windows verification and remaining gaps

- **v1.0 Windows build:** executable successfully ran the 25-run research experiment. **All 13 Windows CTest suites:** no full v1.0 test transcript supplied, so a Windows 13/13 pass is not independently documented here.
- **v1.0 Windows 25-run multi-seed experiment:** **COMPLETED**; 25/25 successful trace/summary/tape replay checks. Report and complete raw aggregates now archived in `benchmarks/WINDOWS_V100_SYNTHETIC_STUDY.{md,json}`; ran Windows 11 AMD64 with Python 3.13.7 (2026-11-10 UTC).
- **New v1.0 network/WAL benchmark:** not run; prior v0.7/v0.8 results are historical, not v1.0 measurements.
- **Empirical Kraken L2 calibration:** not claimed. No user-observed raw/normalized Kraken L2 data has been bundled with this release.
- **Physical power-fault durability, independent pen test and live venue integration:** not attempted.

Note: CI workflow provides build/test coverage for GitHub's Ubuntu and Windows runners upon repository publication and pushes; a passing workflow has not been observed for the v1.0 branch at packaging time.
