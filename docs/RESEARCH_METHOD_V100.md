# Micro Exchange v1.0 — Research Design and Limitations

## Research question

How do prescribed changes to the stochastic order-flow *generator* affect observable output metrics of a deterministic FIFO continuous matching engine? The experiment is a sensitivity demonstration, **not an estimated or identified model of Kraken or any other venue**.

## Generator

The original seeded generator produces one of three regimes (`calm`, `volatile`, `stress`) in contiguous segments for `mixed`. Arrival gaps are exponential with regime-dependent mean, converted to integer microseconds. It generates limit/market orders, IOC/GTC instructions, cancels and quantity/price replacements. Matching and quote sampling are deterministic given the exact trace. Seeding adds 16 noncrossing resting orders.

Version 1.0 retains the legacy RNG and adds bounded tuning:

- `arrival_scale_percent`: multiplies each regime's exogenous arrival rate; affects **only timestamps** in this event-driven model. Consequently a fixed seed produces identical event types and fills under arrival-only scaling, which is an intentional limitation.
- `size_scale_percent`: scales max order quantity in each regime, rounded to the nearest integer.
- `aggression_shift_points`: transfers percentage points from passive adds to aggressive NEW events; it does not make cancellations more likely.
- `cancel_shift_points`: transfers percentage points from passive adds to cancels; it does not change the nominal aggressive-NEW probability.

The generator handles live-order sampling so cancellation and replacement IDs correspond to existing orders. Percentages are scenario assumptions, and the actually realized fractions fluctuate from RNG and live-order availability. In particular the optional market IOC/limit IOC split is not fitted to a real venue.

## Reproducibility

`MEXFLOW1` legacy traces contain seed, profile and event-count metadata; their original v0.9 summary format is preserved. All v1.0 generated traces use `MEXFLOW2`, recording all four tuning parameters, including default values. The replay accepts both and verifies the deterministic matching outputs and fill tape. The experiment runner records per-run summaries, fingerprints, SHA-256 hashes of fill tapes and the executable, machine information, seed list, and min–max ranges across seeds. A Windows or other runtime may produce slightly different generated arrival gaps due to floating-point library differences; **replaying the saved trace on the same runtime** is the strong reproducibility check.

Use paired seeds to compare scenarios. Changing a non-timestamp parameter changes RNG consumption paths, so differences are descriptive scenario effects, **not tightly controlled causal estimates**. Five seeds are a smoke-study sample, not the basis for a confidence interval or hypothesis test.

## Quote statistics

The simulator samples the top five levels after every generated event. It reports average spread only when both sides exist, plus depths and imbalance over all events (including one-sided books, with an empty-sided depth of zero). The additional v1.0 **best-quote imbalance** is computed on two-sided books using only aggregate size at the best bid and ask. The absolute midpoint-change statistic is computed between consecutive valid two-sided quote samples and resets after a one-sided book.

An optional L2 inspector expects independent normalized *top-of-book* observations, not reconstructed order events. For any meaningful comparison the tick size, instrument, units, data filtering, and sampling interval must match. Even then, real data samples occur at externally defined observation points while simulated samples follow every generated event, so the comparison is **diagnostic**, not a calibrated fit. Observed quote updates cannot identify per-order cancellations, arrival intensities, or queue positions.

## What is NOT measured

The in-process study excludes TCP, WAL synchronization, idempotency checks, crashes, and risk/ledger checks. Its artificially scheduled event rate is **not throughput or latency**. A single aggressive order can match multiple makers, so `executions.trades` counts execution records, not submitted market orders. No fees, impact model, clearing, funded balances, profits or external venue fill probabilities are derived.

## Appropriate use

This release is useful to discuss queueing and priority semantics, reproducibility, failure testing, persistence tradeoffs, model assumptions and how to *avoid inventing information missing from L2 data*. It is not evidence of a trading strategy's historical performance, true exchange load, or readiness to execute real customer orders.
