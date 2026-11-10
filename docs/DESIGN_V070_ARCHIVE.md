# Micro Exchange v0.7 engineering design — Journal Commit Modes

## Synchronous mode (default)

`accept command -> validate risk and owner -> WAL append + fflush + fsync/_commit -> modify matching engine -> update fill ledger -> queue ACK/FILL`.

This preserves v0.6 order of journal durability and engine mutations. Every accepted mutation requires its own disk synchronization.

## Experimental event-loop group commit

1. `select` returns a set of sockets ready for reading. Responses from previously committed iterations can also be ready for sending.
2. For each readable client socket, parse complete incoming lines and sequentially validate/append accepted mutations to the *locked* journal; update the matching engine/ledger; accumulate its outgoing responses **in memory**. Monotonic journal request sequence and checksum format are unchanged.
3. After reading **all ready sockets**, call `CommandJournal::sync()` **once** if there are pending journal records. The implementation flushes the buffered stream and issues OS-level `fsync`/`_commit`; records share a sync boundary.
4. Only if sync succeeds may the socket writer send queued responses. On failure, the event loop exits, closes sockets and does not transmit the new group's ACKs or FILL messages. Server status is fatal.
5. The next server process validates and replays all journal records (including a batch if complete). Incomplete or corrupt records cause startup to fail closed.

A lost connection without ACK is ambiguous: journal entries can reach storage despite a client not receiving its response. There is no idempotent request nonce, fencing epoch, distributed failover or exactly-once guarantee. Synchronization depends on OS/storage promises; tests cannot simulate every hardware failure.

## Batch boundaries

- This is **not** a timer-based group commit. There is no artificial wait or background IO worker. A one-command iteration has the same number of syncs as strict mode.
- Batching is opportunistic: synchronized multi-client bursts and pipelined commands can share one sync. The journal statistics report `max_group` and `syncs` for transparency.
- Single-writer OS locking is retained. Response generation is buffered in the existing per-client output queue (bounded to 65,536 bytes); slow clients can be disconnected.
- The parser's existing maximum input and message lengths bound per-iteration buffering. The server remains single-threaded, with a 32-connection cap.
- WAL append-before-engine-update is relaxed **within** the unacknowledged group; no external response is sent until commit. On a sync failure the process fails stop rather than serving other commands against uncertain state.

## Pending work

Storage-failure injection under Linux and Windows, journal-tail repair policy, idempotency keys, protocol-level client resumption, write amplification measurement, power-cut testing, snapshotting and separately measured journal-writer thread design. Neither production-grade risk controls nor HA is implied.

The prior v0.6 design is archived at [docs/DESIGN_V060_ARCHIVE.md](docs/DESIGN_V060_ARCHIVE.md).
