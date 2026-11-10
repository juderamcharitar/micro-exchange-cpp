# Micro Exchange Architecture — v0.9 Addition

The core v0.8.1 C++ matching engine, TCP gateway, journal, identity, ledger and fault-tolerance design is unchanged. Version 0.9 introduces **a separate, in-process research program** `sim/flow_replay.cpp` that links only against `micro_exchange` to generate and replay order-level event traces. It does *not* route synthetic flow through the TCP journaled server, so it must not be used as evidence of network or durable execution latency. See [`docs/SYNTHETIC_FLOW_V090.md`](docs/SYNTHETIC_FLOW_V090.md) for the model, trace, assumptions, metrics and reproduction procedure.

---

# Micro Exchange v0.8 engineering design — failure recovery and idempotency

## Design principle

Durability-before-ACK remains mandatory for both `sync` and `group` WAL modes. A lost or undelivered ACK must not be interpreted as a failed order. A registered account can optionally attach a stable request ID and safely retry **that exact request** after a disconnect or server restart.

## The RID envelope

A registered, authenticated durable owner can submit:

```
RID 42 NEW 100 BUY LIMIT 10100 5 GTC
RID 43 CANCEL 100
RID 44 REPLACE 100 10150 3
```

The server parses the envelope, validates the owner, and journals **the entire raw `RID` line** as the command within the existing MEX1 checksum record. The normal order pathway runs after the journal append. A scope-bound reply capture stores the exact taker ACK/FILL response alongside `(owner_id, request_id, original_raw_line)` once the mutation is journaled. During journal replay the same capture reconstructs the receipt from the matching/ledger deterministic state. All normal risk, ownership, and self-trade checks still apply.

- Repeated `(owner, request_id, raw_line)` → send cached original receipt, marking ACK `duplicate=1`, without modifying the book/journal/ledger.
- Reused `(owner, request_id)` with different raw command → `reason=idempotency_conflict`.
- Request ID `0`, unregistered owner, journal disabled, malformed wrapped command → rejected; no receipt created.
- A rejection **before** journal append is not a committed receipt. A rejected command that did reach WAL is captured and replayed unchanged, because the journal contains it.
- The sequence in a duplicate receipt is deliberately the **original sequence**; callers must use their stable `RID` for request correlation rather than expecting a new exchange sequence on retransmission.
- Registered-account receipts are reconstructed by complete journal replay. No snapshots, receipt eviction, distributed fencing or HA failover exist.

## WAL failure response

If an append or sync fails, the server executes a fail-stop exit before socket writes from the uncertain cycle. `std::_Exit` avoids destruction/ordinary stdio `fclose` flushing after a known failed sync; outstanding clients receive a disconnect, not a success ACK. On next startup the complete journal is validated and replayed; an incomplete/corrupt record rejects startup.

The v0.7 opportunistic group-commit protocol remains: one `select()` iteration can append multiple commands, update tentative in-memory state, then issue one synchronization **before** any queued ACK/FILL is transmitted. Strict mode continues to sync before applying each mutation.

## Fault-injection semantics

These are **test-only server CLI options**. They must not be used on a production or valuable journal. A fault stage triggers on the specified 1-based *appended mutation index* since process startup (registration records count); for before-write failures, the selected would-be append is counted for matching the threshold.

| Stage | Action | Expected client outcome |
|---|---|---|
| `before-write` | Force process exit code 81 just before selected journal record write | No ACK; selected record absent |
| `write-error` | Return an injected append error, then server fail-stops with exit 4 | No ACK; selected record absent |
| `before-sync` | Force exit 82 before selected commit boundary's sync | No ACK; current record outcome ambiguous |
| `sync-error` | Inject sync failure return, then fail-stop exit 4 | No ACK; current record outcome ambiguous |
| `after-sync` | Force exit 83 immediately after sync succeeds, before ACK/engine apply in strict mode or before send in group mode | No ACK; complete selected WAL record replayable |

No stage actually corrupts a filesystem, changes storage hardware, simulates an SSD firmware failure, or turns off power. The separate black-box tests deliberately truncate and modify journal bytes while the server is stopped to verify fail-closed recovery.

## Explicit validity limitations

A process return code is not a proof of durable media: `fsync`/`_commit` success relies on operating-system and storage guarantees. A response can still be lost between a successful sync and network delivery. A journal can be valid but contain operations not acknowledged to clients. A malformed/torn last record blocks startup until a separate audited repair procedure exists. Idempotency applies only to explicitly `RID`-wrapped mutations and only within a single recoverable journal for a registered owner; it is not a global exactly-once protocol.

See [`docs/FAULT_MODEL_V080.md`](docs/FAULT_MODEL_V080.md) for test cases and expected invariants. Earlier v0.7 design is archived under [`docs/DESIGN_V070_ARCHIVE.md`](docs/DESIGN_V070_ARCHIVE.md).
