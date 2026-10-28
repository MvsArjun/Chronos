# Execution, causality and portfolio accounting

## Event order and information availability

`run_backtest` accepts a fully validated vector ordered by
`(available_time, event_time, sequence)` with globally unique sequence numbers.
It validates the full vector before calling a strategy. It performs each step in
this order:

1. Apply a dividend or split. A split cancels pending orders for that instrument.
2. Match orders accepted at a strictly earlier availability time. A matching
   event's exchange timestamp must also be strictly later than acceptance.
3. Deliver execution callbacks and update cash/positions. Mark the instrument
   using the current trade price, bar close, or quote midpoint (rounded down one
   price tick if necessary).
4. Apply strategy cancellations, then call `on_event` and accept new orders.
5. Assert the integer ledger invariant and append an equity snapshot.

Strategies receive the current event and a const ledger. Future vector elements
are never provided. Callers must avoid leaking future information through their
own strategy state. `Strategy` objects are stateful: use a fresh instance for each
repeatable run. The supplied examples are `BuyAndHold` and
`MovingAverageCross` (trailing means, long/flat target; no return claims).

The simulation has no wall-clock dependency. At equal availability timestamps,
orders cannot execute until a later timestamp, even when sequence is later.
This conservative rule also prevents zero-time response to another event in the
same timestamp batch. In addition, each instrument must have strictly increasing
`(event_time, sequence)` keys in availability order. The watermark includes
corporate actions and book updates. A stale BBO cannot replace a newer quote or
supply a fill, and an old pre-split price cannot restore the old share-unit scale.
The complete backtest is rejected before any strategy or execution callback when
this condition fails. Generic storage and replay continue to support late data;
callers must normalize or otherwise reconcile it before using this execution model.

## Bar model

Bars use end-of-interval exchange timestamps. Set
`ExecutionConfig.bar_duration_ns` to the dataset's actual interval duration;
the default of **1 nanosecond is for the synthetic integer-time examples**.
For irregular sessions, daylight-saving changes or bars with variable duration,
split the dataset/configuration appropriately; this version does not carry a
per-event bar start timestamp or exchange calendar.

The bar backtest has a deliberately restricted supported input contract:

- One instrument and only `Bar` payloads. Quote/trade/book and corporate-action
  events must use the tick model or a separate research workflow.
- Immediate bar delivery: `event_time == receive_time == available_time`.
  Delayed bars are rejected rather than allowing a historical open to spend cash
  obtained from a later economic event.
- One fixed duration and phase: all end timestamps lie on the first bar's grid,
  every interval is distinct, and intervals do not overlap. The first interval
  may not begin before Unix epoch zero. Gaps of whole intervals are allowed, with
  no execution assumed inside the gap; callers must distinguish closed sessions
  from missing data before simulation.

Full-input validation rejects unsupported mixed, multi-instrument, delayed,
duplicate, overlapping or off-grid bar datasets before any callbacks. This avoids
exposing one instrument's close before matching another instrument's earlier open,
and avoids retroactive cancellations or corporate-action entitlements within an
outstanding bar. Atomic multi-asset bar batches and intrabar actions are not
implemented. The tick simulator remains multi-instrument and supports the timely
corporate actions described below.

A pending order can execute at a later bar's open only if
`bar.event_time - bar_duration_ns >= order.acceptance_time`. The full bar is delivered at
its availability time and reports are delivered at that simulation time;
`ExecutionReport.time` is **report availability**, not a claim of an observed
exchange execution timestamp. This is an interval-step simulation, with equity
recorded after the close, rather than an intrabar message simulator.

Market orders use the eligible open plus adverse slippage. Limit orders use only
an eligible open with its adverse adjustment inside the limit. Highs/lows never
justify an intrabar fill, and a strategy cannot trade the close of the same bar
that created its signal. `bar_liquidity` is an explicit assumed integer quantity
per bar, shared FIFO by both directions; it does not infer open liquidity from a
bar's later volume. The default of 1,000,000 units is a simulation assumption,
not measured liquidity. Choose and report this limit for each experiment.

## Tick model

The tick model matches against quotes only: buys cross the ask, sells cross the
bid. It shares the quoted ask/bid sizes among eligible orders in FIFO acceptance
order. Partial fills remain pending. Every subsequent quote is assumed to
refresh displayed liquidity; the model cannot infer whether an unchanged quote
represents replenished or already consumed size. Trade events mark positions
but cannot fill orders because a print alone does not reveal executable liquidity.
Book updates are transported/replayed but not used for depth matching.

Queue priority, hidden liquidity, exchange latency, adverse selection, trade
classification and market depth are unknown. This model is useful for testing
research infrastructure, not exchange-quality fill prediction.

## Orders, risk and costs

Market/limit orders have globally unique nonzero IDs, an instrument, a side and
positive whole-unit quantity. Invalid requests produce `Rejected` reports.
Maximum order quantity is configurable. Pending orders persist until filled,
explicitly cancelled or rejected; `pending_orders` reports any outstanding count
at the dataset end. Orders accepted on the final event have no opportunity to fill.
No synthetic final liquidation is performed.

`cancel_orders` runs after current-event matching, so a strategy observing that
event cannot retroactively cancel its fill. Unknown cancellation IDs are ignored.
No cash is reserved while orders are pending (`reserved_cash=0`). Immediately
before every fill, the engine checks that resulting cash covers notional and costs.
An unaffordable candidate fill rejects the remaining order; it does not resize it
to cash. Long-only sells fill at most the currently held quantity. An uncovered
sell is rejected; any remaining quantity after a partial liquidation is checked
again at the next eligible event. Accepted pending orders do not guarantee funding.

`allow_short=true` enables short quantities but has **no margin, borrow,
financing, liquidation or collateral model**. Defaults are long-only. Cash can be
negative if external users apply arbitrary fills directly to `Portfolio`; the
execution engine enforces its pre-fill cash check, including fees on sells. Negative equity and
overflows fail explicitly when the relevant ledger/analytics operation cannot
represent the result.

Commissions and exchange fees are nonnegative fixed-point amounts per filled
unit; `fixed_fee` is charged for every fill, including each partial. Slippage and
optional `impact_bps` add a fixed adverse price adjustment. Both are uncalibrated
basis-point assumptions. Price adjustment is rounded adversely to a representable
price tick. A limit that would be breached after costs in the execution price
remains unfilled. Cash fees are separate from price limits. Quote spread is
already included by using bid/ask, so no duplicate spread charge is added.

## Exact monetary ledger

One run uses one currency. Prices, money and fees are signed 64-bit integers
at scale 10,000 per currency unit; quantity is a whole share/contract integer.
Instrument multipliers, FX conversion and fractional lots are unsupported.
Every monetary addition/multiplication is checked. Invalid fills and arithmetic
overflow are rejected before committing a fill's ledger mutation.

Positions track signed quantity `q`, nonnegative remaining cost basis `B`, a mark
`m`, and cumulative gross realized P&L. Fees are expensed separately. Increasing a
position adds `fill_quantity * fill_price` to its basis. Closing `c` units from an
old absolute quantity `n` allocates `floor(B*c/n)` basis. The remaining basis keeps
the indivisible monetary remainder. A full close allocates all remaining basis.
The proportional allocation uses portable unsigned arithmetic and does not
require a compiler-specific 128-bit integer.

For a closing long, realized P&L is proceeds minus allocated basis. For a closing
short, it is allocated opening proceeds minus cover notional. A reversal closes
the old side before opening the remainder at the fill price. Average cost is a
derived floating-point display `B/abs(q)`; it never drives ledger calculations.
Realized/unrealized P&L are gross of fees; net performance follows equity.

```
cash_next = cash - signed_fill_quantity * fill_price - fees
net_exposure = sum(q_i * mark_i)
gross_exposure = sum(abs(q_i) * mark_i)
equity = cash + net_exposure
unrealized_long = abs(q) * mark - B
unrealized_short = B - abs(q) * mark
equity = initial_cash + realized_pnl + unrealized_pnl + dividend_income - fees
```

Dividend events add signed `q * amount_per_share` to cash and income (shorts owe
the amount). They assume unadjusted price data and an ex-date economic cash
credit; payment-date settlement, withholding tax and dividend entitlement trades
are not modeled. Using adjusted prices plus dividend events would double count.
Corporate actions must be available at their effective event timestamp and must
precede all same-instrument market events at that timestamp in the dataset's
sequence order. The engine rejects delayed actions and action-after-market ties
during full-input validation, before any strategy callbacks. It cannot reconstruct
historical entitlement quantities after an effective date has passed. The generic
storage/replay layer may carry late actions, but this accounting model cannot
simulate them safely. If multiple actions share an effective time, their explicit
sequence determines whether a dividend amount is interpreted before or after a
split; the dataset producer must supply the intended share-unit convention.
These action-event semantics are supported by tick execution. The pure-Bar
execution contract rejects corporate-action events; direct `Portfolio` operations
remain available for separately controlled accounting workflows.
Splits change quantity and mark while preserving cost basis. Only ratios producing
an exact whole quantity and exact fixed-point mark are supported. Fractional shares
and cash in lieu raise an error. Splits cancel all outstanding same-instrument
orders rather than guessing broker adjustment rules.

## Validation fixtures

The `Portfolio.IndependentlyCalculatedReversalAndFeeLedger` test starts with 1,000
currency units, buys 10 at 10 (fee 1), buys 5 at 12 (fee 0.5), sells 8 at 15
(fee 0.8), sells 10 at 9 (fee 1), and covers 1 at 8 (fee 0.1). The final independent
ledger is cash 1,038.6, short 2, realized P&L 24, unrealized P&L 2 at mark 8,
fees 3.4, and equity 1,022.6. Intermediate basis remainders are asserted exactly.

A seeded 2,000-fill invariant test independently tracks cash and a long-double
fractional average-cost reference over reductions and reversals. Integer realized
P&L is compared with an explicit cumulative one-price-tick-per-step tolerance,
while the ledger identity is asserted with exact equality. Other tests cover
overflow rollback, exact splits, quote liquidity sharing, partial fees, limit
rules, cancellation, late arrivals and repeated-run determinism. Actual test
execution status is recorded in the final audit, not inferred from this test list.
