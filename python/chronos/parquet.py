"""Strict offline Parquet interchange for the Chronos integer event schema.

PyArrow and temporary CSV are intentionally outside the C++ processing path.
Conversions materialize tables and event batches; they are not zero-copy APIs.
"""
from __future__ import annotations

import csv
import os
from pathlib import Path
import tempfile

import pyarrow as pa
import pyarrow.csv as arrow_csv
import pyarrow.parquet as pq

from . import _core

MAX_EVENTS = 10_000_000
COLUMNS = (
    "type", "event_time", "receive_time", "available_time", "sequence",
    "instrument", "p1", "p2", "p3", "p4", "quantity", "quantity2", "aux",
)
SCHEMA = pa.schema([
    pa.field("type", pa.string(), nullable=False),
    pa.field("event_time", pa.int64(), nullable=False),
    pa.field("receive_time", pa.int64(), nullable=False),
    pa.field("available_time", pa.int64(), nullable=False),
    pa.field("sequence", pa.uint64(), nullable=False),
    pa.field("instrument", pa.uint32(), nullable=False),
    *[pa.field(name, pa.int64(), nullable=False)
      for name in ("p1", "p2", "p3", "p4", "quantity", "quantity2")],
    pa.field("aux", pa.uint32(), nullable=False),
], metadata={
    b"chronos.schema_version": b"1",
    b"chronos.price_scale": b"10000",
    b"chronos.timestamp_unit": b"unix_ns_utc",
})


def _check_schema(schema: pa.Schema) -> None:
    if tuple(schema.names) != COLUMNS:
        raise ValueError("unsupported Parquet column names or order")
    for expected, actual in zip(SCHEMA, schema):
        if expected.type != actual.type:
            raise ValueError(f"Parquet type mismatch for {expected.name}")
    metadata = schema.metadata or {}
    for name, expected in SCHEMA.metadata.items():
        if metadata.get(name) != expected:
            raise ValueError(f"unsupported or missing Parquet metadata: {name.decode()}")


def _read_table(path: str | Path) -> pa.Table:
    parquet = pq.ParquetFile(path)
    _check_schema(parquet.schema_arrow)
    if parquet.metadata.num_rows > MAX_EVENTS:
        raise ValueError("Parquet event count exceeds cap")
    table = parquet.read()
    if any(column.null_count for column in table.columns):
        raise ValueError("null Parquet event fields are not supported")
    return table


def _table_to_csv(table: pa.Table, path: Path) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(COLUMNS)
        for batch in table.to_batches(max_chunksize=65_536):
            columns = [column.to_pylist() for column in batch.columns]
            writer.writerows(zip(*columns))


def _csv_table(path: str | Path) -> pa.Table:
    # Validate financial/event semantics with the same C++ parser used by replay.
    _core.load_csv(str(path))
    table = arrow_csv.read_csv(path, convert_options=arrow_csv.ConvertOptions(
        column_types={field.name: field.type for field in SCHEMA},
        strings_can_be_null=False,
    ))
    if tuple(table.schema.names) != COLUMNS:
        raise ValueError("unsupported CSV columns")
    if table.num_rows > MAX_EVENTS or any(column.null_count for column in table.columns):
        raise ValueError("oversized or null CSV data")
    return pa.Table.from_arrays(table.columns, schema=SCHEMA)


def csv_to_parquet(csv_path: str | Path, parquet_path: str | Path) -> None:
    """Validate canonical CSV, preserve integer precision, and write Parquet v2.6."""
    table = _csv_table(csv_path)
    destination = Path(parquet_path)
    with tempfile.TemporaryDirectory(prefix="chronos-parquet-", dir=destination.parent) as temporary:
        staging = Path(temporary) / "events.parquet"
        pq.write_table(table, staging, version="2.6", compression="zstd")
        os.replace(staging, destination)


def parquet_to_csv(parquet_path: str | Path, csv_path: str | Path) -> None:
    """Validate metadata, types, nulls and event semantics before replacing output."""
    table = _read_table(parquet_path)
    destination = Path(csv_path)
    with tempfile.TemporaryDirectory(prefix="chronos-csv-", dir=destination.parent) as temporary:
        staging = Path(temporary) / "events.csv"
        _table_to_csv(table, staging)
        _core.load_csv(str(staging))
        os.replace(staging, destination)


def load_parquet(path: str | Path):
    """Load a strict Parquet dataset into an owning C++ EventBatch."""
    table = _read_table(path)
    with tempfile.TemporaryDirectory(prefix="chronos-parquet-") as temporary:
        staging = Path(temporary) / "events.csv"
        _table_to_csv(table, staging)
        return _core.load_csv(str(staging))


def save_parquet(path: str | Path, events) -> None:
    """Save an EventBatch using the same canonical Parquet schema."""
    with tempfile.TemporaryDirectory(prefix="chronos-parquet-") as temporary:
        staging = Path(temporary) / "events.csv"
        _core.save_csv(str(staging), events)
        csv_to_parquet(staging, path)
