#!/usr/bin/env python3
"""Convert STM32 flash CAN log dump to per-sequence CSV files.

This script parses 24-byte log entries written by flash_writer.c and
splits them into sequences based on the LOGA magic word.
"""

import argparse
import csv
import os
import re
import struct
from typing import Iterable, List, Optional, Tuple

FLASH_LOG_MAGIC_A = 0x4C4F4741  # 'LOGA'
FLASH_LOG_MAGIC_B = 0x4C4F4742  # 'LOGB'
ENTRY_SIZE = 24

CSV_HEADER = [
    "Time Stamp",
    "ID",
    "Extended",
    "Dir",
    "Bus",
    "LEN",
    "D1",
    "D2",
    "D3",
    "D4",
    "D5",
    "D6",
    "D7",
    "D8",
]


def iter_entries(
    data: bytes, start_offset: int = 0
) -> Iterable[Tuple[int, int, int, int, int, List[int]]]:
    """Yield parsed entries from raw flash data.

    Returns tuples: (magic, timestamp, id, is_tx, dlc, data_bytes)
    """
    offset = start_offset
    max_offset = len(data) - ENTRY_SIZE
    while offset <= max_offset:
        w0, w1, w2, w3, w4, w5 = struct.unpack_from("<6I", data, offset)
        if w0 in (FLASH_LOG_MAGIC_A, FLASH_LOG_MAGIC_B):
            timestamp = w1
            idf = w2
            is_tx = 1 if (idf & 0x80000000) else 0
            can_id = idf & 0x7FFFFFFF
            dlc = w3 & 0xFF
            data_bytes = [
                (w3 >> 8) & 0xFF,
                (w3 >> 16) & 0xFF,
                (w3 >> 24) & 0xFF,
                w4 & 0xFF,
                (w4 >> 8) & 0xFF,
                (w4 >> 16) & 0xFF,
                (w4 >> 24) & 0xFF,
                w5 & 0xFF,
            ]
            yield (w0, timestamp, can_id, is_tx, dlc, data_bytes)
            offset += ENTRY_SIZE
            continue
        if w0 == 0xFFFFFFFF:
            offset += ENTRY_SIZE
            continue
        offset += 4


def write_sequence_csv(
    out_path: str, entries: List[Tuple[int, int, int, int, int, List[int]]]
) -> None:
    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(CSV_HEADER)
        for _, timestamp, can_id, is_tx, dlc, data_bytes in entries:
            row = [
                str(timestamp),
                f"{can_id:08X}",
                "true",
                "Tx" if is_tx else "Rx",
                "0",
                str(dlc),
            ]
            row.extend(f"{b:02X}" for b in data_bytes)
            writer.writerow(row)


def split_sequences(
    entries: Iterable[Tuple[int, int, int, int, int, List[int]]]
) -> List[List[Tuple[int, int, int, int, int, List[int]]]]:
    sequences: List[List[Tuple[int, int, int, int, int, List[int]]]] = []
    current: List[Tuple[int, int, int, int, int, List[int]]] = []

    for entry in entries:
        magic = entry[0]
        if magic == FLASH_LOG_MAGIC_A:
            if current:
                sequences.append(current)
                current = []
        current.append(entry)

    if current:
        sequences.append(current)

    return sequences


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Split STM32 flash CAN log dump into per-sequence CSV files."
    )
    parser.add_argument("input", help="Path to flash dump (e.g. stm_dump.bin)")
    parser.add_argument(
        "--out-dir",
        default=None,
        help="Output directory (default: alongside input file)",
    )
    parser.add_argument(
        "--prefix",
        default="sequence_",
        help="Output filename prefix (default: sequence_)",
    )
    parser.add_argument(
        "--start-offset",
        default=None,
        help="Optional start offset (hex or decimal), e.g. 0x20000",
    )
    return parser.parse_args()


def extract_hex_bytes(text: str) -> bytes:
    """Extract bytes from a C-style hex dump (e.g. 0x41, 0x47, ...)."""
    tokens = re.findall(r"0x([0-9A-Fa-f]{2})", text)
    if not tokens:
        return b""
    return bytes(int(t, 16) for t in tokens)


def read_input_bytes(path: str) -> bytes:
    """Read raw bytes, with fallback to parse text hex dumps."""
    with open(path, "rb") as f:
        data = f.read()
    entries = list(iter_entries(data))
    if entries:
        return data

    try:
        text = data.decode("utf-8", errors="ignore")
    except UnicodeDecodeError:
        return data

    hex_bytes = extract_hex_bytes(text)
    if hex_bytes:
        return hex_bytes
    return data


def parse_int(value: str) -> int:
    if value.lower().startswith("0x"):
        return int(value, 16)
    return int(value, 10)


def find_start_offset(data: bytes) -> int:
    """Find first occurrence of LOGA/LOGB magic in raw data."""
    magic_a = struct.pack("<I", FLASH_LOG_MAGIC_A)
    magic_b = struct.pack("<I", FLASH_LOG_MAGIC_B)
    idx_a = data.find(magic_a)
    idx_b = data.find(magic_b)
    if idx_a == -1 and idx_b == -1:
        return 0
    if idx_a == -1:
        return idx_b
    if idx_b == -1:
        return idx_a
    return min(idx_a, idx_b)


def main() -> int:
    args = parse_args()
    in_path = args.input
    out_dir = args.out_dir or os.path.dirname(os.path.abspath(in_path))
    os.makedirs(out_dir, exist_ok=True)

    data = read_input_bytes(in_path)
    start_offset = (
        parse_int(args.start_offset) if args.start_offset is not None else None
    )
    if start_offset is None:
        start_offset = find_start_offset(data)

    entries = list(iter_entries(data, start_offset=start_offset))
    sequences = split_sequences(entries)

    for i, seq in enumerate(sequences):
        out_path = os.path.join(out_dir, f"{args.prefix}{i:03d}.csv")
        write_sequence_csv(out_path, seq)

    print(f"Sequences written: {len(sequences)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
