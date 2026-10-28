# Market data contract

Chronos normalizes sources into the `Event` schema in `core/types.hpp`. Data adapters
must convert exchange/source identifiers into nonzero 32-bit instrument IDs and
record that mapping in dataset metadata. One run has one reporting currency; the
engine does not perform foreign exchange conversion.

## Units and validity

Timestamps are signed 64-bit Unix nanoseconds in UTC, restricted to nonnegative
values. `event_time` is exchange/economic event time; `receive_time` is when the
collector received it; `available_time` is when a strategy could first consume it.
Receipt must be at least event time and availability must be at least receipt.
They are separate fields because downstream strategy availability can lag receipt.
Unavailable values are rejected rather than represented by zero/NaN sentinels.
Zero is a valid Unix timestamp and zero-volume quote/book/bar values are allowed.

Prices and cash use signed 64-bit units of 1/10,000 currency unit. Quantities are
whole shares/contracts. Exchange prices must be positive, quotes cannot cross,
and OHLC extrema must enclose open and close. Dividend amounts may be zero. Split
ratios are positive numerator/denominator integers. Fractions of shares are not
part of the schema. `sequence` is a nonzero, globally unique dataset sequence,
not a venue-local sequence. A source adapter merging venues must create a stable
mapping and retain venue provenance separately. Event schema version is 1.

For bars, event time and availability describe the **end** of the interval, when
the complete close/high/low/volume became known. Adapters must never mark a full
bar available at its opening time. Sample bars are synthetic completed daily
intervals; sample ticks are synthetic and carry a 100 ns receipt/availability lag.
Neither sample represents licensed exchange data.

## Canonical CSV schema

The exact header is:

```text
type,event_time,receive_time,available_time,sequence,instrument,p1,p2,p3,p4,quantity,quantity2,aux
```

| Type | p1 | p2 | p3 | p4 | quantity | quantity2 | aux |
| --- | --- | --- | --- | --- | --- | --- | --- |
| trade | price | 0 | 0 | 0 | size | 0 | 0 |
| quote | bid | ask | 0 | 0 | bid size | ask size | 0 |
| book | price | side: 1 buy / 2 sell | 0 | 0 | size; 0 deletes | 0 | level |
| bar | open | high | low | close | volume | 0 | 0 |
| dividend | cash per share | 0 | 0 | 0 | 0 | 0 | 0 |
| split | numerator | denominator | 0 | 0 | 0 | 0 | 0 |

Fields use base-ten integers. CSV output uses the classic numeric locale so
machine locale cannot insert thousands separators. The parser rejects floating point, missing values,
unknown types, extra columns, inactive nonzero fields, crossed quotes and invalid
OHLC ranges. LF and CRLF are supported. Quoted fields, BOMs and blank rows are not
supported because this is a canonical interchange format, not a general CSV
spreadsheet parser. Import vendor files into this contract first. The parser
reports the failing line number. Read counts are capped at ten million events;
the optional `max_events` argument can tighten that cap. A row longer than 1024
bytes is rejected by a fixed-size line buffer before dynamic growth can occur.

## Binary dataset v1

Files are endian-independent and never serialize C++ object memory or padding.
All integers are little endian; signed integers use their two's-complement bit
representation. The format is:

| Byte offset | Width | Meaning |
| --- | ---: | --- |
| 0 | 8 | ASCII `CHRONOS` followed by NUL |
| 8 | 2 | Schema version, 1 |
| 10 | 2 | Literal bytes `LE` |
| 12 | 4 | Record width, 89 |
| 16 | 4 | Metadata byte count, at most 65,536 |
| 20 | 8 | Record count, at most 10,000,000 |
| 28 | metadata length | Opaque dataset metadata; conventionally UTF-8 JSON |
| following | count x 89 | Fixed-width records |
| final | 4 | CRC32/IEEE checksum of every preceding byte |

Each 89-byte record contains event/receipt/availability timestamps (three i64),
sequence (u64), instrument (u32), type tag (u8, CSV table order starting at zero),
p1/p2/p3/p4/quantity/quantity2 (six i64), and aux (u32). There are no implicit
padding bytes. CRC uses reflected polynomial `0xedb88320`, initial state all ones
and final bitwise inversion, matching Python `zlib.crc32`. CRC detects accidental
corruption; it is not authentication or a cryptographic signature.

Readers reject unsupported versions, endianness markers and record widths,
oversized metadata/counts, physical-size mismatches, malformed records, CRC
failures and trailing garbage. The physical file size is checked before reserving
event memory. Read/write is sequential after this initial size check. The C++
API materializes events into a vector; replay also owns a vector, so this is an
offline capped-memory implementation rather than an unlimited streaming store.
The writer preserves caller event order and emits identical bytes for identical
input and metadata. Failed writes can leave incomplete files; publish datasets
by writing a temporary path and renaming after success if atomic publication is
needed. Metadata is opaque and not JSON-validated by C++.

Recommended metadata includes source, source file hash, instrument map, currency,
price scale, timezone, bar duration, timestamp interpretation, adjustment policy,
download time, license/provenance and generator seed. Store unadjusted events if
the accounting engine will process dividends and splits. Combining adjusted
prices with separate corporate actions double-counts adjustments.

## Replay and late data

`replay::ordered_events` validates every event and rejects duplicate global
sequences. It sorts by `(available_time, event_time, sequence)`. The unique final
key makes tie-breaking total and deterministic. Economic timestamps may move
backward after late data arrives; the simulation clock follows availability and
never moves backward. The clock is reset at each `Replay::run` and advances only
from input data. Wall-clock timing and thread scheduling cannot change replay
ordering. Handler exceptions propagate; a subsequent run starts from the first
event again. Reentrant calls on the same Replay object and concurrent mutation
are unsupported.

Sorting is an explicit offline normalization step. Live queues must receive
already ordered events or reject late events according to their own ingestion
policy. This code does not silently revise historical strategy decisions when
late data arrives. Replay does not model historical universe membership,
delistings, exchange calendars or vendor correction streams automatically.

## Synthetic generation

`synthetic_trades` and `synthetic_bars` use a specified xorshift64 sequence with a
nonzero seed. Results use integer arithmetic and are stable across supported
compilers. Trades are spaced one millisecond apart; completed bars are spaced one
minute apart. Generation is capped at ten million events. They are suitable for
repeatable correctness and throughput experiments, not realistic microstructure
or evidence of strategy profitability.

## Parquet interchange

`chronos.parquet` provides `load_parquet(path)`, `save_parquet(path, EventBatch)`,
`csv_to_parquet(csv_path, parquet_path)` and `parquet_to_csv(parquet_path, csv_path)`.
The columns have the same names/order as canonical CSV. Times, p1-p4 and quantities
are Arrow int64; sequence is uint64; instrument and aux are uint32; type is string.
Nulls, implicit floating-point coercions and wrong column order are rejected.
Required schema metadata is `chronos.schema_version=1`,
`chronos.price_scale=10000`, and `chronos.timestamp_unit=unix_ns_utc`. Event semantics
are validated by the C++ CSV parser as part of conversion. Tests preserve sequences
up to UINT64_MAX and prices above the exact integer range of IEEE double.

Parquet writes use format 2.6 with Zstd compression and stage output beside the
destination before atomic replacement. Existing output is preserved if validation
fails. The adapter materializes Arrow tables and uses temporary CSV to construct
owning C++ batches. It is intentionally a copying, offline ingestion path and not
a zero-copy transport. The ten-million-event cap is checked before materializing
Parquet rows. Other vendor Parquet schemas must be normalized explicitly first.

CSV and binary APIs are declared in `market_data/data.hpp`; replay is in
`replay/replay.hpp`. Arrow is deliberately not a dependency of the C++ hot path.
