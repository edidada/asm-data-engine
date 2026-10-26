#!/usr/bin/env python3
"""
convert.py - Prepare input data for asm-data-engine.

Two modes:

  1. Real NYC TLC trip data (requires `pip install pyarrow`):
     python scripts/convert.py --parquet yellow_tripdata_2024-01.parquet amounts.bin

     Reads a single double column (default: total_amount), keeps only
     positive values, and writes raw little-endian doubles (8 bytes/row).

  2. Synthetic mode (no dependencies):
     python scripts/convert.py --synthetic 1000000 amounts.bin
"""
import argparse
import struct
import sys


def convert_parquet(path: str, out: str, column: str, keep_positive: bool) -> int:
    try:
        import pyarrow.parquet as pq
    except ImportError:
        sys.exit("error: pyarrow is required for --parquet mode (pip install pyarrow)")

    table = pq.read_table(path, columns=[column])
    values = table.column(column).to_pylist()

    count = 0
    with open(out, "wb") as f:
        buf = bytearray()
        for v in values:
            if v is None:
                continue
            if keep_positive and v <= 0:
                continue
            buf += struct.pack("<d", float(v))
            count += 1
            if len(buf) >= 1 << 20:  # flush every 1 MiB
                f.write(buf)
                buf.clear()
        if buf:
            f.write(buf)
    return count


def convert_synthetic(n: int, out: str, seed: int) -> int:
    import random

    rng = random.Random(seed)
    count = 0
    with open(out, "wb") as f:
        buf = bytearray()
        for _ in range(n):
            # taxi-fare-like values in [2.0, 250.0), quantized to cents
            buf += struct.pack("<d", round(rng.uniform(2.0, 250.0), 2))
            count += 1
            if len(buf) >= 1 << 20:
                f.write(buf)
                buf.clear()
        if buf:
            f.write(buf)
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert data to raw doubles for asm-data-engine")
    parser.add_argument("output", help="output .bin file (raw little-endian doubles)")
    parser.add_argument("--parquet", help="input NYC TLC .parquet file")
    parser.add_argument("--column", default="total_amount", help="parquet column to use")
    parser.add_argument("--synthetic", type=int, metavar="N",
                        help="generate N deterministic synthetic values instead")
    parser.add_argument("--seed", type=int, default=42, help="synthetic generator seed")
    parser.add_argument("--all", action="store_true",
                        help="keep non-positive values too (parquet mode)")
    args = parser.parse_args()

    if args.synthetic is None and not args.parquet:
        parser.error("choose one source: --parquet FILE or --synthetic N")

    if args.synthetic is not None:
        count = convert_synthetic(args.synthetic, args.output, args.seed)
    else:
        count = convert_parquet(args.parquet, args.output, args.column,
                                keep_positive=not args.all)

    size_mb = count * 8 / (1024.0 * 1024.0)
    print(f"wrote {count} doubles ({size_mb:.2f} MB) -> {args.output}")


if __name__ == "__main__":
    main()
