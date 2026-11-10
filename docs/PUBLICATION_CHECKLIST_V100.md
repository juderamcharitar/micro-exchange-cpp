# GitHub publication checklist — Micro Exchange v1.0.0

## Verified in this publication package

- [x] C++20 source, Windows build script, CMake configuration and Linux/Windows CI workflow are present.
- [x] 13/13 Linux Release and sanitizer test suites reported in the release validation record.
- [x] Windows synthetic 5-scenario × 5-seed study archived in `benchmarks/WINDOWS_V100_SYNTHETIC_STUDY.md` and `.json`.
- [x] The raw JSON medians, minima and maxima reconcile with its 25 run-level records.
- [x] Each generated/replayed trace and fill tape was verified by the research runner (25/25); the program's reported replay status is not independent external certification.
- [x] No live trading, Kraken calibration, or production performance claims in the README.
- [x] Generated `.exe`, local account credentials, and WAL state are not bundled.

## Before publishing or calling the release fully verified

- [ ] Choose a license (none added without permission); without a license, public visibility does **not** grant broad reuse rights.
- [ ] Provide/upload the v1.0 Windows `ctest -C Release` transcript showing 13/13 passes if you wish to claim Windows CI-equivalent test coverage.
- [ ] Initialize the repo, inspect `git status`, and review the diff and ignored files for private data.
- [ ] Push to the chosen GitHub repository; check that Actions actually pass for Ubuntu and Windows runners.
- [ ] Create GitHub release tag `v1.0.0`, attach this source bundle if desired, and use the notes in `docs/GITHUB_RELEASE_NOTES_V100.md`.
- [ ] Preserve historical benchmark provenance and disclose Windows sync-latency variability if using throughput numbers in a resume/interview.
- [ ] Calibrate to actual Kraken observations **only after** obtaining consented, normalized and methodologically comparable L2 data.

Do not promote this research simulator as an operational exchange, deployed trading strategy, source of alpha, or real-money product.
