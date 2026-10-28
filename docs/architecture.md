# Architecture and design decisions

Chronos is a compact C++20 systems and quantitative-research project. It is not
exchange connectivity, a broker, a matching engine, or a claim of equivalence to
any firm's proprietary infrastructure. Linux GCC/Clang are the primary intended
targets; Windows GCC is the locally verified target. Audit status is explicit.

```text
CSV / local ticks / Python Parquet / synthetic generator
                         |
                  validated typed Event
                         |
              canonical binary + metadata + CRC
                         |
       offline availability ordering and sequence validation
                    /                         \
       single-thread Replay             bounded SPSC transport
                    \                         /
                  deterministic handlers
                         |
          execution assumptions + Strategy callbacks
                         |
       checked integer Portfolio -> reports/equity/exposures
                         |
       pybind11 API -> NumPy/SciPy validation / factor research
```

## Event and storage boundary

`std::variant` makes trade, quote, book update, bar, dividend and split payloads
distinct types. The measured Event is 88 bytes, alignment 8, with a 40-byte header
on this GCC x64 ABI. These are measurements, not a portable ABI promise. Disk
records are explicitly encoded 89-byte little-endian sequences, independent of
compiler padding. Signal, order request and execution report schemas share the
same units. Invalid values raise errors; zero instrument/sequence/price is not a
missing-value sentinel. Research factors use explicit missing observations.

Prices and money are signed 64-bit integers scaled by 10,000; quantities are whole
units. Overflow is checked instead of wrapping. A run has one currency and unit
convention. Instrument IDs are supplied by the caller; there is no security master
or FX conversion. Metadata must describe those conventions and adjustments.

Replay preprocessing validates and sorts by (available_time,event_time,sequence).
Availability must be at/after receipt, which must be at/after exchange/event time.
It is an offline batch operation with O(n) auxiliary memory and O(n log n) sort;
it is excluded from dispatch-only measurements and benchmarked separately.
Global sequence identifiers are unique, but gaps are allowed. They are not a
claim of exchange-feed gap recovery. Late events become visible when available;
generic replay preserves them, while backtests reject per-instrument economic-time
regressions before callbacks so stale prices cannot replace current state or fill
orders. Clock progression is logical,
without wall-clock pacing. For bars the exchange timestamp is interval end;
the configured duration defines earliest possible next-open execution.

## Ownership and concurrency

The single-thread baseline establishes deterministic results. The transport path
has one producer that copies already ordered immutable input and one caller-thread
consumer that owns handler execution. Accepted events stay FIFO. The queue has a
fixed power-of-two capacity; full policies block/yield, drop newest with a count,
or fail after draining accepted events. Blocking is the lossless default. A stop
request stops ingestion and drains accepted events. Handler exceptions cancel and
join the producer before propagating. Destruction requires no active queue users.

Each SPSC slot is an optional in a contiguous preallocated array. Producer release
publishes a constructed value; consumer acquire observes it. Consumer release
publishes slot reclamation; producer acquire prevents reuse before destruction.
Owner-local index reads are relaxed. Separate 64-byte-aligned cursors avoid typical
x64 cache-line sharing; other hardware can have different cache-line sizes.
Unsigned cursor arithmetic handles wrap provided capacity is less than half the
index range. Compile-time lock-free size_t atomics are required, and the tested
x64 target satisfies this. Lock-free queue operations do not make the entire
pipeline lock-free: yielding, thread creation and user callbacks are outside that
claim. Generic stored types may allocate in their constructors; Event does not.

There is no MPMC queue or custom pool. PMR experiments compare growing, reserved
and arena-backed containers. Reservation removes repeated allocations without
introducing another ownership mechanism; the measured arena gain is too small
and noisy to justify adopting it. The simulator intentionally uses conventional
maps/vectors and allocates state/reports. It is not an allocation-free hot loop.

## Financial assumptions and Python boundary

Execution and portfolio logic are separated from transport. Bar execution uses a
later eligible open with explicit assumed liquidity. It accepts timely, pure Bar
input for one instrument on a fixed nonoverlapping grid. Unsupported multi-asset,
mixed or delayed bar input is rejected; atomic multi-asset bar batches are not
implemented. Quote execution uses later
displayed sides/sizes shared FIFO; trades mark but do not fabricate liquidity.
Limits include adverse slippage/impact, and risk checks run at fill time. There is
no borrow, margin, market-depth queue position or intrabar path model. See
[execution-model.md](execution-model.md) for exact rules and ledger identities.

`EventBatch` owns its C++ vector and exposes copies of individual events. Native
backtests read that vector without a second vector conversion. Native convenience
runs copy mutable configuration while holding the GIL, then release the GIL.
Python strategies hold the GIL and receive owned event/portfolio/report snapshots;
retaining callback arguments is tested. Fresh strategies are required per run.
Individual Strategy objects must not be used concurrently. Native independent
runs can share an immutable batch. Result properties retain parent ownership or
return copies. Analytics accept Python sequences/NumPy sequences through STL
conversion; these calls copy into vectors. They are not advertised as zero-copy
NumPy kernels. Parquet conversion materializes an Arrow table and temporary CSV,
which is suitable for offline ingestion rather than the measured transport path.

Factor research is a fractional target-weight model with publication/availability
gates, one-close delayed execution, drifted weights and linear transaction costs.
It shares unit/time principles and analytics, while the C++ simulator models
whole-lot orders. The two models have different execution assumptions; factor
returns are not presented as C++ order simulation results.

## Alternatives and next steps

A packed union could reduce representation overhead but loses variant safety;
raw struct serialization was rejected as ABI-dependent. Mmap/chunked streaming
would reduce batch memory but require index/CRC/lifetime design. A merged live
feed needs watermarks and source-specific sequence-gap rules; offline sorting is
not a substitute. A real tick simulator needs venue, queue and depth information.
Intrinsics/SIMD, shared-memory rings and custom allocators require profiles before
adoption. The measured CRC bottleneck was addressed with a portable 1KiB table,
preserving canonical bytes and independently checked CRC compatibility.
