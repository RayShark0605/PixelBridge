#!/usr/bin/env python3
"""Independent DesktopLevels-X1 oracle. No C++ headers/tools are read or run.

Reuses the independent Bootstrap RS/CRC oracle, not the production codec.
All outputs are create-only or read-only --check; old Golden files are untouched.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys
from blake3 import blake3
sys.dont_write_bytecode = True
import generate_local_desktop_golden as scaffold

ROOT = Path(__file__).resolve().parents[1] / "golden" / "desktop-levels"
LEVELS = (32, 96, 160, 224)
LABELS = (0, 1, 3, 2)
LADDERS = ((736, 16), (1696, 16), (96, 1000), (1056, 1000))
PHASE_PILOTS = ((896, 16), (896, 1000))

# Independently transcribed frozen Robust DVB-S2 short matrix parameters.
# Provenance: the already-validated ETSI Table 5b/aff3ct baseline recorded in
# PBInnerFec. This oracle reads no C++ source and does not invoke its encoder.
ROBUST_SHIFTS = (
    (0,2084,1613,1548,1286,1460,3196,4297,2481,3369,3451,4620,2622),
    (1,122,1516,3448,2880,1407,1847,3799,3529,373,971,4358,3108),
    (2,259,3399,929,2650,864,3996,3833,107,5287,164,3125,2350),
    (3,342,3529),(4,4198,2147),(5,1880,4836),(6,3864,4910),(7,243,1542),
    (8,3011,1436),(9,2167,2512),(10,4606,1003),(11,2835,705),(12,3426,2365),
    (13,3848,2474),(14,1360,1743),(0,163,2536),(1,2583,1180),(2,1542,509),
    (3,4418,1005),(4,5212,5117),(5,2155,2922),(6,347,2696),(7,226,4296),
    (8,1560,487),(9,3926,1640),(10,149,2928),(11,2364,563),(12,635,688),
    (13,231,1684),(14,1129,3894))


def DiagnosticData(record: bytes, capacity: int,
                   domain: bytes = b"PB-DesktopLevels-X1-Data") -> bytes:
    assert domain and b"\x00" not in domain
    result = bytearray()
    tag, sequence = struct.unpack_from("<QQ", record, 16)
    for slot in range(capacity // 2025):
        payload = b"".join(blake3(domain + record + struct.pack("<II", slot, chunk)).digest()
                           for chunk in range(42))[:1314]
        header = struct.pack("<BBHQQIHH", 1, 0, 0, tag, sequence, slot, len(payload), 0)
        info = header + struct.pack("<I", scaffold.Crc32C(header)) + payload + struct.pack("<I", scaffold.Crc32C(payload))
        assert len(info) == 1350
        # XOR columns as Python integers, then solve the staircase with a
        # prefix XOR. This does not share the production matrix workspace.
        parity = 0
        for bit in range(10800):
            if info[bit // 8] & (1 << (bit % 8)):
                for shift in ROBUST_SHIFTS[bit // 360]:
                    parity ^= 1 << ((shift + (bit % 360) * 15) % 5400)
        carry = 0
        packed = bytearray(675)
        for bit in range(5400):
            carry ^= (parity >> bit) & 1
            packed[bit // 8] |= carry << (bit % 8)
        result += info + packed
    return bytes(result) + bytes(capacity - len(result))


def Record(tile: int, sequence: int) -> bytes:
    name = f"PixelBridge/VisualProfile/DesktopLevels-X1-Luma4-Tile{tile}x{tile}"
    profile = int.from_bytes(blake3(name.encode()).digest()[:8], "little")
    content = struct.pack("<4sBBBBQQQII", b"PBRG", 1, 1, 0, 3, profile,
                          0x1122334455667788, sequence, 0x21222324, 0)
    return content + struct.pack("<I", scaffold.Crc32C(content))


def Coordinates(tile: int) -> list[tuple[int, int]]:
    return [(x, y) for y in range(96, 984, tile) for x in range(96, 1824, tile)
            if not any(left <= x < left + 128 and top <= y < top + 128
                       for left, top in scaffold.PILOTS)]


def LogicalData(tile_count: int, sequence: int) -> bytes:
    capacity = tile_count // 4
    coded = capacity // 2025 * 2025
    return bytes((index * 37 + sequence * 11 + 5) & 255 for index in range(coded)) + bytes(capacity - coded)


def Raster(tile: int, sequence: int, record: bytes, coordinates: list[tuple[int, int]]) -> bytes:
    raw = scaffold.Raster(scaffold.EncodeRs(record), scaffold.TimingBits(record), scaffold.MarkerBits())
    gray = bytearray(raw[0::4])

    def Fill(x: int, y: int, width: int, height: int, level: int) -> None:
        row = bytes([level]) * width
        for offset_y in range(y, y + height):
            offset = offset_y * 1920 + x
            gray[offset:offset + width] = row

    for left, top in LADDERS:
        for index, level in enumerate(LEVELS):
            Fill(left + index * 32, top, 32, 64, level)
    for left, top in PHASE_PILOTS:
        for row in range(64):
            for column in range(128):
                gray[(top + row) * 1920 + left + column] = LEVELS[3 if (column if column < 64 else row) % 2 else 0]
    data = LogicalData(len(coordinates), sequence)
    # Forward scatter is deliberately different from the production renderer's
    # inverse gather and seven-band analytical coordinate mapping.
    for logical in range(len(coordinates)):
        physical = (logical * 65537 + (sequence % 16) * (1728 // tile)) % len(coordinates)
        left, top = coordinates[physical]
        label = (data[logical // 4] >> (2 * (logical % 4))) & 3
        Fill(left, top, tile, tile, LEVELS[LABELS.index(label)])
    result = bytearray(1920 * 1080 * 4)
    for channel in range(3):
        result[channel::4] = gray
    result[3::4] = bytes([255]) * len(gray)
    return bytes(result)


def Main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--raster-output", type=Path)
    args = parser.parse_args()
    files = {"marker-bits.bin": scaffold.MarkerBits()}
    manifest = ["tile phase profile_id tile_count data_bytes codewords padding_bytes data_blake3 bgra_blake3"]
    for tile in (2, 4):
        coordinates = Coordinates(tile)
        expected_count = 346752 if tile == 2 else 86688
        assert len(coordinates) == expected_count and len(set(coordinates)) == expected_count
        files[f"tile{tile}-diagnostic.bin"] = DiagnosticData(Record(tile, 0), len(coordinates) // 4)
        for sequence in range(16):
            record = Record(tile, sequence)
            stem = f"tile{tile}-phase{sequence}"
            files[stem + "-record.bin"] = record
            files[stem + "-rs76.bin"] = scaffold.EncodeRs(record)
            files[stem + "-timing.bin"] = scaffold.TimingBits(record)
            pixels = Raster(tile, sequence, record, coordinates)
            data = LogicalData(len(coordinates), sequence)
            manifest.append(f"{tile} {sequence} {int.from_bytes(record[8:16], 'little'):016X} {len(coordinates)} {len(data)} {len(data)//2025} {len(data)%2025} {blake3(data).hexdigest()} {blake3(pixels).hexdigest()}")
            if args.raster_output is not None and sequence == 0:
                args.raster_output.mkdir(parents=True, exist_ok=True)
                with (args.raster_output / (stem + ".pbrw")).open("xb") as output:
                    output.write(scaffold.Pbrw(pixels))
    files["manifest.txt"] = ("\n".join(manifest) + "\n").encode()
    if not args.check:
        ROOT.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        path = ROOT / name
        if path.exists():
            if path.read_bytes() != content:
                raise SystemExit(f"Golden mismatch, never re-pin: {path}")
        elif args.check:
            raise SystemExit(f"Missing Golden: {path}")
        else:
            with path.open("xb") as output:
                output.write(content)
    print(f"DESKTOP_LEVELS_GOLDEN_PASS files={len(files)} frames=32")


if __name__ == "__main__":
    Main()
