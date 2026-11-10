# v0.9 — Synthetic Order-Flow Methodology and Replay Contract

## What is modeled

`sim/flow_replay.cpp` implements **research-only synthetic order-event generation** followed by deterministic price-time matching. The generator and the `--replay` reader use the same underlying `MatchingEngine`, but replay reads a saved event trace; it does not re-run the random generator. In particular:

1. **Initialization:** 16 fixed limit orders (8 levels each side, quantity 12 each) are submitted at timestamp 0, with bids 9999..9992 and asks 10001..10008. These count as `seed_events` and are excluded from scenario event counts.
2. **Arrival clock:** stochastic exponential interarrivals with nominal scenario rate in events/second, rounded to positive integer microseconds. This is a Poisson-process assumption, not an estimated exchange arrival process.
3. **Selection of actions:** a regime-specific categorical draw chooses a passive limit add, cancel of a uniformly sampled *currently resting* order, replace of a currently resting order, or an aggressive IOC NEW.
4. **Price and size placement:** integer limit ticks, mostly passive placement around the current top-of-book midpoint and a scenario spread/jitter; aggressive arrivals are 70% market IOC and 30% crossing-limit IOC. Remaining quantities and referenced live IDs are derived from the actual C++ order book state, avoiding stale generated cancels. The model does not emulate real maker IDs, venue order size distributions or strategic agent behavior.
5. **Accounting:** engine fills, quantity, quote spread, top-five depth and imbalance are computed after each event, with invariant checks periodically and at completion. There is no journal, network, risk, market-impact model, fee schedule, or financed portfolio in this executable.

All parameter values are **assumptions** in `sim/flow_replay.cpp`, not results from any market data:

| Scenario | Nominal arrivals/s | Passive NEW | CANCEL | REPLACE | Aggressive IOC NEW | Nominal spread ticks | Quote jitter ticks | Maximum order qty |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| calm | 2,200 | 61% | 22% | 7% | 10% | 4 | 3 | 9 |
| volatile | 5,400 | 54% | 20% | 8% | 18% | 9 | 11 | 22 |
| stress | 12,000 | 43% | 17% | 7% | 33% | 17 | 30 | 45 |

`mixed` runs three contiguous phases with equal event counts, not equal simulated durations. Realized spread and aggression will differ from those inputs because the matching engine changes the book endogenously. E.g., a simulated "calm" spread may narrow to one tick despite a larger nominal placement spread.

## CSV wire format

Exact prefix and column header:

```text
# MEXFLOW1 seed=42 profile=mixed synthetic_events=3 seed_events=2
ts_us,regime,op,order_id,side,type,price_ticks,quantity,tif
0,seed,NEW,1,BUY,LIMIT,9999,12,GTC
0,seed,NEW,2,SELL,LIMIT,10001,12,GTC
1,calm,NEW,3,BUY,MARKET,0,4,IOC
2,calm,CANCEL,1,,,,,
3,calm,REPLACE,2,,,10002,8,
```

The excerpt is a **schema illustration**, not a complete realistic data set. Event counts must match metadata. `NEW` requires `side`, `type`, `price_ticks`, `quantity` and `tif`. `CANCEL` requires only the order ID; other columns must be empty. `REPLACE` requires order ID, new price and new **remaining** quantity; other columns must be empty. Valid regimes are `seed`, `calm`, `volatile`, `stress`, `external`.

The replay parser fails closed on malformed rows, unexpected records, invalid order IDs, duplicate used IDs, rejected mutations, out-of-order timestamps, and mismatched event counts. **It does not support quoting/escaping fields**, as every field is a known ASCII token or integer. Timestamps must be monotonic nondecreasing microseconds; they are **event-clock timestamps**, not network arrival or execution latency measurements.

For a manually assembled order-event trace use `profile=external`, `seed_events=0` unless you explicitly include `seed` rows, and `synthetic_events` equal to the total non-seed event rows. The label `external` is metadata only: the software does not authenticate the provenance or correctness of user-supplied event records.

## Replay validation

Run generator with `--summary original.json --trades original.csv`. Later run replayer with `--expect-summary original.json --trades replayed.csv`. The summary includes:

- final best bid/ask and resting-order count
- accepted operation counts, total fill records and executed quantity
- per-regime two-sided spread, depth-5 size, imbalance, observed simulated event frequency, and trades
- an FNV-1a 64-bit **regression fingerprint** of the canonical trace records plus resulting fill rows

A matching summary establishes exact deterministic agreement with the recorded summary on that build. **FNV-1a is not cryptographically secure** and cannot prove tamper resistance. The optional trade tape supplies independent human-inspectable fill evidence. For cross-compiler comparisons use numerical and structural comparisons; C++ floating-point math (notably the exponential clock) can round differently across runtimes, even though the PRNG's integer state transitions are fixed.

## What aggregate L2 can and cannot tell us

A best bid/ask feed or aggregated order book generally provides quoted prices, sizes and timestamps. It does **not** uniquely identify limit-order IDs, cancellations (versus trades), the invisible queue at a venue, order-owner identity, or the exact queue position of an external order. Different underlying event streams can produce the same aggregate depth snapshots.

`sim/inspect_l2.py` deliberately reads a **different** normalized CSV and reports aggregate observables, without constructing fictitious orders. Inputs must use integer tick prices, integer microsecond timestamps, best-quote quantity, and (if known) aggressor-side trade annotations. It is not an order-event generator or automatic parameter fitter. A defensible calibration would fit *observable* distributions from your existing Kraken data, disclose unobservable queue and cancellation assumptions, and compare synthetic and observed feature distributions over multiple independent windows.

## Measurement boundaries

- Simulation summary means are computed over **post-event observations**, which gives events (not clock time) equal sampling weight.
- Spread is measured only when both quote sides exist; depth-5 imbalance is `(bid_qty - ask_qty)/(bid_qty + ask_qty)` for the observed five levels, with zero assigned when both sides are empty.
- The match volume is sum of executions returned by the in-memory engine, not dollars traded or realized portfolio P&L.
- The `--seed` value controls repeatability for the *same implementation/runtime*. Changing assumptions, compiler libraries, or `std::log` can change event details.
- There is currently no event-queue latency, packet loss, burst clustering, self-exciting Hawkes process, correlated trader-agent model, or actual market data replay.
- The scenario data alone cannot validate market-making profitability, forecast market movements, or establish real-time exchange throughput.
