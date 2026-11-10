# v0.8: Failure model, reproducible tests, and research boundaries

## Properties tested

**P1 — no false durable ACK.** A process that fails before an intended commit does not send a successful reply from the failing event-loop iteration.

**P2 — acknowledged writes replay.** Previously acknowledged commands survive deliberate process stops under the tested filesystems.

**P3 — commit/ACK ambiguity can be reconciled.** A durable mutation with an injected crash after sync but before ACK can be retried using the same `RID` after restart. The order/ledger/trade count stays unchanged and the original receipt is returned with `duplicate=1`.

**P4 — conflicting retry is rejected.** Reuse of an already committed `(owner, request_id)` with different raw command returns `idempotency_conflict` and does not append to the WAL.

**P5 — fail-closed replay.** Checksum changes and truncated final records make recovery fail rather than silently discard data.

**P6 — fill receipts are idempotent.** Re-delivering the original taker fill report after recovery does not re-execute matching or re-post the ledger.

## Run isolated failure tests

```cmd
ctest --test-dir build -C Release -R exchange_fault_idempotency_integration --output-on-failure
```

This invokes `tests/test_fault_idempotency.py`, which automatically creates disposable journals, connects localhost Python peers, registers synthetic account identities, injects stop/error stages and restarts C++ subprocesses. It additionally runs both strict and group modes for several cases. The complete test suite count is ten.

## What is NOT established

- Hardware power removal, lost writes despite successful flush, drive/controller write-cache honesty, sector-level atomicity, torn journal records during real device failure.
- Database durability or exactly-once semantics under distributed failover and multiple writers; this is an exclusive-writer local journal.
- Unbounded receipt retention and snapshot/compaction correctness. Replaying the entire journal reconstructs receipts and currently scales in memory with committed RID count.
- Absence of duplicate logical economic intent when an application uses **different** RID values for the same economic request.
- Recovery in the presence of arbitrary untrusted or tampered journals; the checksum is non-cryptographic FNV-1a and is an accidental-corruption detector, **not an authentication MAC**.
- Correct behavior for every OS scheduling pattern or real network failure (tests are finite, localhost, and deterministic). Linux Release and sanitizer test outcomes do not imply Windows qualification.

## Experimental next steps

1. Run v0.8 on Windows and archive exact test results, OS/compiler and storage information.
2. Benchmark the RID pathway with repeated original/dedup calls separately from ordinary commands; preserve v0.7 metrics as the baseline.
3. Test page-sized writes, journal truncation at each byte offset, storage capacity exhaustion, and snapshots/compaction with retained ID receipts.
4. Consider a versioned WAL format with authenticated integrity if the simulated environment needs adversarial tamper detection.
