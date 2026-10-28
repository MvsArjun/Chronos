import tempfile
from pathlib import Path
import struct
import unittest
import zlib

import pyarrow as pa
import pyarrow.parquet as pq

import chronos
from chronos.parquet import (SCHEMA, csv_to_parquet, load_parquet,
                             parquet_to_csv, save_parquet)


class ParquetTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="chronos-parquet-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_all_types_and_extreme_unsigned_sequence_preserve_precision(self):
        header = chronos.EventHeader(1, 2, 3, 2**64 - 1, 2**32 - 1)
        events = chronos.EventBatch([
            chronos.Event(header, chronos.Trade(2**60 + 1, 1)),
            chronos.Event(chronos.EventHeader(4, 4, 4, 2, 1), chronos.Quote(100, 101, 5, 6)),
            chronos.Event(chronos.EventHeader(5, 5, 5, 3, 1), chronos.BookUpdate(chronos.Side.SELL, 101, 0, 2)),
            chronos.Event(chronos.EventHeader(6, 6, 6, 4, 1), chronos.Bar(100, 105, 95, 101, 1000)),
            chronos.Event(chronos.EventHeader(7, 7, 7, 5, 1), chronos.Dividend(10)),
            chronos.Event(chronos.EventHeader(8, 8, 8, 6, 1), chronos.Split(3, 2)),
        ])
        original = self.root / "original.csv"
        chronos.save_csv(str(original), events)
        destination = self.root / "events.parquet"
        save_parquet(destination, events)
        loaded = load_parquet(destination)
        self.assertEqual(len(loaded), 6)
        self.assertEqual(loaded[0].header.sequence, 2**64 - 1)
        self.assertEqual(loaded[0].payload.price, 2**60 + 1)
        self.assertEqual(loaded[0].header.instrument, 2**32 - 1)
        output = self.root / "roundtrip.csv"
        parquet_to_csv(destination, output)
        self.assertEqual(original.read_bytes(), output.read_bytes())

    def test_binary_layout_and_crc_against_independent_python_reference(self):
        path = self.root / "events.chr"
        events = chronos.synthetic_trades(25, 42)
        chronos.save_binary(str(path), events, '{"seed":42}')
        data = path.read_bytes()
        magic, version, endian, width, metadata_length, count = struct.unpack("<8sHHIIQ", data[:28])
        self.assertEqual((magic, version, endian, width, count), (b"CHRONOS\x00", 1, 0x454C, 89, 25))
        self.assertEqual(zlib.crc32(data[:-4]), struct.unpack("<I", data[-4:])[0])
        offset = 28 + metadata_length
        actual = struct.unpack("<qqqQIBqqqqqqI", data[offset:offset + width])
        first = events[0]
        expected = (first.header.event_time, first.header.receive_time, first.header.available_time,
                    first.header.sequence, first.header.instrument, 0, first.payload.price,
                    0, 0, 0, first.payload.quantity, 0, 0)
        self.assertEqual(actual, expected)

    def test_csv_conversion_and_empty_batch(self):
        source = self.root / "source.csv"
        chronos.save_csv(str(source), chronos.synthetic_trades(20, 42))
        destination = self.root / "events.parquet"
        csv_to_parquet(source, destination)
        self.assertEqual(len(load_parquet(destination)), 20)
        save_parquet(destination, chronos.EventBatch([]))
        self.assertEqual(len(load_parquet(destination)), 0)

    def test_missing_version_and_type_coercion_are_rejected(self):
        path = self.root / "bad.parquet"
        table = pa.Table.from_arrays([pa.array([], type=f.type) for f in SCHEMA], schema=SCHEMA)
        pq.write_table(table.replace_schema_metadata(None), path)
        with self.assertRaises(ValueError):
            load_parquet(path)
        columns = list(table.columns)
        columns[4] = pa.array([], type=pa.float64())
        broken = pa.Table.from_arrays(columns, names=SCHEMA.names).replace_schema_metadata(SCHEMA.metadata)
        pq.write_table(broken, path)
        with self.assertRaises(ValueError):
            load_parquet(path)

    def test_nulls_and_invalid_event_values_are_rejected(self):
        source = self.root / "source.csv"
        path = self.root / "events.parquet"
        chronos.save_csv(str(source), chronos.synthetic_trades(1))
        csv_to_parquet(source, path)
        table = pq.read_table(path)
        # A nullable field with an actual null is forbidden, regardless of Arrow type.
        columns = list(table.columns)
        columns[6] = pa.array([None], type=pa.int64())
        nullable = pa.Table.from_arrays(columns, names=SCHEMA.names).replace_schema_metadata(SCHEMA.metadata)
        pq.write_table(nullable, path)
        with self.assertRaises(ValueError):
            load_parquet(path)
        columns[6] = pa.array([-100], type=pa.int64())
        invalid = pa.Table.from_arrays(columns, names=SCHEMA.names).replace_schema_metadata(SCHEMA.metadata)
        pq.write_table(invalid, path)
        output = self.root / "preserved.csv"
        output.write_text("preserve this existing output", encoding="utf-8")
        with self.assertRaises(ValueError):
            parquet_to_csv(path, output)
        self.assertEqual(output.read_text(encoding="utf-8"), "preserve this existing output")


if __name__ == "__main__":
    unittest.main()
