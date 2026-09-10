#!/usr/bin/env python3
"""Independent oracle for the supplemental control band raster; never imports production code.

Re-implements the frozen band semantics from the 2026-09-09/10 experiments in
plain Python: PB-Control-1 record bytes are pinned as a literal (the protocol
serialization itself is covered by PBProtocol tests), the 223-byte sealed
message (magic "PBB1", version, length, session tag, frame sequence, CRC32C)
is packed here, parity comes from an independent LFSR division over the
RS(255,223) generator built in ascending coefficient order, and the codeword
is rendered as 4x4 cells (high 224 / low 32) on a mid-gray 608x64 BGRA patch.

Generation is create-only. --check compares without modifying the fixture.
"""

from __future__ import annotations

import argparse
from pathlib import Path

RECORD_HEX = "5042435201014d0000000000000000010203040506072700000001030507090b0d0f118d8d383e"
SESSION_TAG = 0x0706050403020100
FRAME_SEQUENCE = 42
PATCH_WIDTH, PATCH_HEIGHT = 608, 64
CELL_COLUMNS, CELL_OFFSET_X, CELL_OFFSET_Y, CELL_STRIDE = 148, 8, 4, 4
LUMA_HIGH, LUMA_LOW, MATTE = 224, 32, 128

FIELD_POLYNOMIAL = 0x11D


def BuildField() -> tuple[list[int], list[int]]:
    powers: list[int] = []
    logarithms = [0] * 256
    value = 1
    for exponent in range(255):
        powers.append(value)
        logarithms[value] = exponent
        value <<= 1
        if value & 0x100:
            value ^= FIELD_POLYNOMIAL
    for exponent in range(255, 510):
        powers.append(powers[exponent - 255])
    return powers, logarithms


POWERS, LOGS = BuildField()


def Multiply(left: int, right: int) -> int:
    if left == 0 or right == 0:
        return 0
    return POWERS[LOGS[left] + LOGS[right]]


def BuildGenerator() -> list[int]:
    generator = [1]
    for root in range(32):
        ascending = [0] * (len(generator) + 1)
        for index, coefficient in enumerate(generator):
            ascending[index] ^= coefficient
            ascending[index + 1] ^= Multiply(coefficient, POWERS[root])
        generator = ascending
    return generator


GENERATOR = BuildGenerator()


def RsParity(message: bytes) -> bytes:
    parity = [0] * 32
    for symbol in message:
        factor = symbol ^ parity[0]
        parity = parity[1:] + [0]
        if factor:
            for index in range(32):
                parity[index] ^= Multiply(factor, GENERATOR[index + 1])
    return bytes(parity)


def Crc32c(data: bytes) -> int:
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def PackMessage(record: bytes, session_tag: int, frame_sequence: int) -> bytes:
    message = bytearray(223)
    message[0:4] = b"PBB1"
    message[4] = 1
    message[5] = 0
    message[6:8] = len(record).to_bytes(2, "little")
    message[8:16] = session_tag.to_bytes(8, "little")
    message[16:24] = frame_sequence.to_bytes(8, "little")
    message[28:28 + len(record)] = record
    message[24:28] = Crc32c(bytes(message)).to_bytes(4, "little")
    return bytes(message)


def RenderPatch(codeword: bytes) -> bytes:
    patch = bytearray(PATCH_WIDTH * PATCH_HEIGHT * 4)
    for offset in range(0, len(patch), 4):
        patch[offset] = patch[offset + 1] = patch[offset + 2] = MATTE
        patch[offset + 3] = 255
    for bit in range(len(codeword) * 8):
        cell_x = CELL_OFFSET_X + (bit % CELL_COLUMNS) * CELL_STRIDE
        cell_y = CELL_OFFSET_Y + (bit // CELL_COLUMNS) * CELL_STRIDE
        level = LUMA_HIGH if (codeword[bit // 8] >> (bit % 8)) & 1 else LUMA_LOW
        for row in range(4):
            for column in range(4):
                offset = ((cell_y + row) * PATCH_WIDTH + cell_x + column) * 4
                patch[offset] = patch[offset + 1] = patch[offset + 2] = level
    return bytes(patch)


def BuildFixture() -> bytes:
    record = bytes.fromhex(RECORD_HEX)
    assert len(record) <= 195
    message = PackMessage(record, SESSION_TAG, FRAME_SEQUENCE)
    codeword = message + RsParity(message)
    assert len(codeword) == 255
    return RenderPatch(codeword)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    fixture = BuildFixture()
    target = Path(__file__).resolve().parent.parent / "golden" / "supplemental-band" / "render-session.bin"
    if arguments.check:
        if not target.is_file():
            print(f"missing fixture: {target}")
            return 1
        if target.read_bytes() != fixture:
            print("fixture bytes differ from the independent oracle")
            return 1
        print("supplemental band golden fixture matches the independent oracle")
        return 0
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists():
        print(f"fixture already exists (create-only): {target}")
        return 1
    target.write_bytes(fixture)
    print(f"wrote {target} ({len(fixture)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
