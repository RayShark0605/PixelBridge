#!/usr/bin/env python3
"""Independent PB-LocalDesktopBootstrap-X1 oracle; never imports production code.

GF arithmetic is bitwise, the generator is built in ascending coefficient order,
and parity uses an LFSR (the C++ implementation uses tables and long division).
The raster constants below are a second literal implementation of the frozen spec.
Generation is create-only. --check compares without modifying any fixture;
--extend may add fixtures, but cannot change any existing bytes or digest pin.
Requires Python 3.12 and the independently packaged blake3 1.0.9 baseline.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import struct

from blake3 import blake3


WIDTH, HEIGHT = 1920, 1080
MARKERS = ((16, 16), (1840, 16), (16, 1000), (1840, 1000))
COPIES = ((96, 16), (1216, 1000))
PILOTS = tuple((x, y) for y in (160, 476, 792) for x in (96, 896, 1696))
ROLE_BYTES = (0x00, 0x0F, 0x33, 0x55)
ROLE_POSITIONS = ((0, 0), (1, 0), (5, 0), (6, 0), (0, 6), (1, 6), (5, 6), (6, 6))
DOMAIN = b"PB-LDBS-X1-Pilot"


def Multiply(left: int, right: int) -> int:
    result = 0
    while right:
        if right & 1:
            result ^= left
        right >>= 1
        left <<= 1
        if left & 0x100:
            left ^= 0x11D
    return result


def Power(exponent: int) -> int:
    result = 1
    for _ in range(exponent % 255):
        result = Multiply(result, 2)
    return result


def Generator(root_count: int) -> bytes:
    ascending = [1]
    for exponent in range(root_count):
        root = Power(exponent)
        following = [0] * (len(ascending) + 1)
        for index, value in enumerate(ascending):
            following[index] ^= Multiply(value, root)
            following[index + 1] ^= value
        ascending = following
    return bytes(reversed(ascending))


def EncodeRs(message: bytes) -> bytes:
    assert len(message) in (44, 223)
    coefficients = Generator(32)
    parity = [0] * 32
    for value in message:
        feedback = value ^ parity.pop(0)
        parity.append(0)
        for index in range(32):
            parity[index] ^= Multiply(feedback, coefficients[index + 1])
    return message + bytes(parity)


def Syndromes(word: bytes) -> bytes:
    # Explicit polynomial powers rather than the production Horner evaluator.
    values = []
    for root in range(32):
        total = 0
        for index, symbol in enumerate(word):
            total ^= Multiply(symbol, Power(root * (len(word) - index - 1)))
        values.append(total)
    return bytes(values)


def RootlessLocatorNoise() -> bytes:
    # Lambda(x)=1+x+delta*x^2 has no GF(256) root when Tr(delta)=1.
    # The recurrence below has minimal degree exactly two: S0=1,S1=0
    # would force any degree-one coefficient to zero, contradicting S2!=0.
    delta = 0x20
    trace, conjugate = 0, delta
    for _ in range(8):
        trace ^= conjugate
        conjugate = Multiply(conjugate, conjugate)
    assert trace == 1
    assert all(1 ^ value ^ Multiply(delta, Multiply(value, value)) for value in range(256))
    sequence = [1, 0]
    for _ in range(2, 32):
        sequence.append(sequence[-1] ^ Multiply(delta, sequence[-2]))
    assert sequence[2] == delta != 0
    assert bytes(sequence).hex() == "0100202054207206e020710583238e9a8a20b0c447e2eafbf7e16a1bda9ddf95"

    def Inverse(value: int) -> int:
        assert value != 0
        result, factor, exponent = 1, value, 254
        while exponent:
            if exponent & 1:
                result = Multiply(result, factor)
            factor = Multiply(factor, factor)
            exponent >>= 1
        assert Multiply(value, result) == 1
        return result

    # Change only the 32 parity positions. Their distinct evaluation points
    # give an invertible 32x32 Vandermonde system; neither BM nor a production
    # decoder is used to manufacture this exact root-count failure.
    rows = [[Power(row * (31 - column)) for column in range(32)] + [sequence[row]] for row in range(32)]
    for column in range(32):
        pivot = next(row for row in range(column, 32) if rows[row][column])
        rows[column], rows[pivot] = rows[pivot], rows[column]
        inverse = Inverse(rows[column][column])
        rows[column] = [Multiply(value, inverse) for value in rows[column]]
        for row in range(32):
            if row != column:
                factor = rows[row][column]
                rows[row] = [left ^ Multiply(factor, right) for left, right in zip(rows[row], rows[column], strict=True)]
    assert all(rows[row][column] == int(row == column) for row in range(32) for column in range(32))
    noise = bytes(44) + bytes(row[32] for row in rows)
    assert sum(value != 0 for value in noise) == 32
    assert Syndromes(noise) == bytes(sequence)
    return noise


def Crc32C(data: bytes) -> int:
    value = 0xFFFFFFFF
    for symbol in data:
        value ^= symbol
        for _ in range(8):
            value = (value >> 1) ^ (0x82F63B78 if value & 1 else 0)
    return value ^ 0xFFFFFFFF


def Record(sequence: int, session_tag: int, control_epoch: int = 0x21222324, layout: int = 2) -> bytes:
    prefix = b"PBRG" + bytes((1, 1, 0, layout))
    prefix += struct.pack("<QQQII", 0x50424C4442533031, session_tag, sequence, control_epoch, 0)
    assert len(prefix) == 40
    return prefix + struct.pack("<I", Crc32C(prefix))


def MarkerBits() -> bytes:
    result = bytearray()
    for role in ROLE_BYTES:
        modules = []
        for row in range(7):
            for column in range(7):
                black = column in (0, 6) or row in (0, 6) or (2 <= column <= 4 and 2 <= row <= 4)
                modules.append(int(not black))
        for bit, (column, row) in enumerate(ROLE_POSITIONS):
            modules[row * 7 + column] = (role >> bit) & 1
        result.extend(modules)
    return bytes(result)


def TimingBits(record: bytes) -> bytes:
    assert len(DOMAIN) == 16 and len(record) == 44
    result = bytearray()
    for pilot in range(9):
        digest = blake3(DOMAIN + record + bytes((pilot,))).digest()[:16]
        for symbol in digest:
            for bit_index in range(8):
                bit = (symbol >> bit_index) & 1
                result.extend((bit, bit ^ 1))
    return bytes(result)


def Raster(codeword: bytes, timing: bytes, markers: bytes) -> bytes:
    image = bytearray(bytes((128, 128, 128, 255)) * (WIDTH * HEIGHT))

    def Rectangle(x: int, y: int, width: int, height: int, level: int) -> None:
        assert 0 <= x < x + width <= WIDTH and 0 <= y < y + height <= HEIGHT
        pixels = bytes((level, level, level, 255)) * width
        for row in range(y, y + height):
            offset = (row * WIDTH + x) * 4
            image[offset:offset + len(pixels)] = pixels

    def Cells(origin: tuple[int, int], columns: int, bits: bytes) -> None:
        for index, bit in enumerate(bits):
            assert bit in (0, 1)
            Rectangle(origin[0] + (index % columns) * 8, origin[1] + (index // columns) * 8, 8, 8, 224 if bit else 32)

    for index, (x, y) in enumerate(MARKERS):
        Rectangle(x, y, 64, 64, 224)
        Cells((x + 4, y + 4), 7, markers[index * 49:(index + 1) * 49])
    bootstrap_bits = bytes((value >> bit) & 1 for value in codeword for bit in range(8))
    for origin in COPIES:
        Cells(origin, 76, bootstrap_bits)
    for index, origin in enumerate(PILOTS):
        Cells(origin, 16, timing[index * 256:(index + 1) * 256])
    return bytes(image)


def Pbrw(pixels: bytes) -> bytes:
    return b"PBRW" + bytes((1, 0, 0, 0)) + struct.pack("<III", WIDTH, HEIGHT, len(pixels)) + bytes(8) + pixels


def BuildFixtures() -> tuple[dict[str, bytes], dict[str, bytes]]:
    assert blake3(b"").hexdigest() == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
    assert Crc32C(b"123456789") == 0xE3069283
    generator = Generator(32)
    assert generator.hex() == "01744034ae367e10c2a221219db0c5e10c3b37fde4942fb3b9188afd148e37ac58"
    files = {"marker-bits.bin": MarkerBits(), "rs-generator.bin": generator}
    rasters = {}
    entries = {}
    base_sequence, base_session, base_epoch = 0x1112131415161718, 0x81DF204BD997BAD0, 0x21222324
    cases = (("a", base_sequence, base_session, base_epoch, 2),
             ("b", base_sequence + 1, base_session, base_epoch, 2),
             ("c", base_sequence, base_session + 1, base_epoch, 2),
             ("d", base_sequence + 2, base_session, base_epoch, 2),
             ("e", base_sequence ^ (1 << 48), base_session, base_epoch, 2),
             ("f", base_sequence, base_session, base_epoch + 1, 2),
             ("badcrc", base_sequence, base_session, base_epoch, 2),
             ("unsupported", base_sequence, base_session, base_epoch, 3))
    for stem, sequence, session, control_epoch, layout in cases:
        record = Record(sequence, session, control_epoch, layout)
        if stem == "badcrc":
            record = record[:40] + bytes((record[40] ^ 1,)) + record[41:]
        codeword = EncodeRs(record)
        assert codeword == EncodeRs(bytes(179) + record)[179:]
        assert Syndromes(codeword) == bytes(32)
        timing = TimingBits(record)
        files[f"{stem}-record.bin"] = record
        files[f"{stem}-rs76.bin"] = codeword
        files[f"{stem}-timing-bits.bin"] = timing
        raw = Raster(codeword, timing, files["marker-bits.bin"])
        rasters[f"{stem}.pbrw"] = Pbrw(raw)
        entries[stem] = {"session_tag": f"0x{session:016x}", "frame_sequence": sequence,
                         "canonical44_hex": record.hex(), "rs76_hex": codeword.hex(),
                         "raw_bgra_bytes": len(raw), "raw_bgra_blake3": blake3(raw).hexdigest(),
                         "raw_bgra_sha256": hashlib.sha256(raw).hexdigest(),
                         "pbrw_blake3": blake3(rasters[f"{stem}.pbrw"]).hexdigest()}

    # Known canonical protocol bytes are opaque to RS, not the new visual ID.
    protocol_record = bytes.fromhex("50425247010100010807060504030201d0ba97d94b20df8118171615141312112423222100000000eae188d4")
    assert Crc32C(protocol_record[:40]) == int.from_bytes(protocol_record[40:], "little")
    protocol_word = EncodeRs(protocol_record)
    assert protocol_word[44:].hex() == "2cec0537b6ad3444b28866fb3c41477b077094e75bd5653c446cd3d5998cee3d"
    files["protocol-rs76.bin"] = protocol_word

    # A valid *full* RS word with a nonzero omitted prefix is not a legal
    # shortened word. Its last 44 payload bytes still have a valid Bootstrap CRC.
    for label, position in (("first", 0), ("last", 178)):
        full_message = bytearray(bytes(179) + files["a-record.bin"])
        full_message[position] = 0x5A
        full_word = EncodeRs(bytes(full_message))
        assert Syndromes(full_word) == bytes(32)
        truncated = full_word[179:]
        assert truncated[:44] == files["a-record.bin"] and Syndromes(truncated) != bytes(32)
        files[f"shortening-prefix-{label}.bin"] = truncated

    # Exactly 17 parity-symbol errors whose first 16 syndromes vanish. A <=16
    # error solution cannot exist: its first16 equations have an invertible
    # distinct-location Vandermonde matrix. BM must reject degree17.
    noise = bytes(59) + Generator(16)
    noise_syndromes = Syndromes(noise)
    assert sum(value != 0 for value in noise) == 17
    assert noise_syndromes[:16] == bytes(16) and noise_syndromes[16] != 0
    invalid = bytes(left ^ right for left, right in zip(files["a-rs76.bin"], noise, strict=True))
    assert invalid[:44] == files["a-record.bin"]
    files["uncorrectable-17.bin"] = invalid

    rootless_noise = RootlessLocatorNoise()
    rootless = bytes(left ^ right for left, right in zip(files["a-rs76.bin"], rootless_noise, strict=True))
    assert rootless[:44] == files["a-record.bin"]
    assert Crc32C(rootless[:40]) == int.from_bytes(rootless[40:44], "little")
    assert Syndromes(rootless) == Syndromes(rootless_noise)
    assert blake3(rootless).hexdigest() == "2e60801855d17a127f8362e85cdb93486e4f5ad3d7d97e4d4011565b969819bf"
    files["rootless-locator.bin"] = rootless

    manifest = {
        "name": "PB-LocalDesktopBootstrap-X1", "experimental": True,
        "visual_profile_id": "0x50424c4442533031", "visual_layout_version": 2,
        "protocol": "PB-Bootstrap-1, canonical44 unchanged",
        "canvas": [WIDTH, HEIGHT], "pixel_format": "BGRA8, B=G=R, A=255",
        "luma": {"black": 32, "white": 224, "reserved_background": 128},
        "cell_pixels": 8,
        "marker_regions": [[x, y, 64, 64] for x, y in MARKERS],
        "marker_quiet_pixels": 4, "marker_roles_lsb_first": list(ROLE_BYTES),
        "marker_role_positions": ROLE_POSITIONS,
        "bootstrap_regions": [[x, y, 608, 64] for x, y in COPIES],
        "bootstrap_bit_order": "row-major cells; codeword[i//8] bit i%8",
        "pilot_regions": [[x, y, 128, 128] for x, y in PILOTS],
        "pilot_input": "ASCII PB-LDBS-X1-Pilot (no NUL) || canonical44 || uint8 pilotIndex",
        "pilot_bits": "BLAKE3 first16 bytes, LSB-first, each bit b -> [b,b^1]",
        "rs": {"n": 76, "k": 44, "parity": 32, "field_polynomial": "0x11d", "alpha": 2,
               "roots": [0, 31], "full_n": 255, "full_k": 223, "zero_prefix": 179,
               "coefficients": "highest degree first", "errors_only_limit": 16,
               "generator_hex": generator.hex()},
        "oracle": {"script": "tests/PBModulation/generate_local_desktop_golden.py",
                   "python_baseline": "3.12", "blake3_package": importlib.metadata.version("blake3"),
                   "gf": "bitwise polynomial multiply; ascending generator; parity LFSR",
                   "raw": "independent literal region painter; no production executable"},
        "frames": entries,
        "files": {name: {"bytes": len(data), "blake3": blake3(data).hexdigest()} for name, data in sorted(files.items())},
    }
    files["manifest.json"] = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    return files, rasters


def Main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "golden" / "local-desktop-bootstrap")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--check", action="store_true", help="Read-only recomputation of every committed fixture")
    modes.add_argument("--extend", action="store_true", help="Append new fixtures only; reject any change to existing pins")
    parser.add_argument("--raster-output", type=Path, help="Optionally create raw PBRW files in a separate directory")
    args = parser.parse_args()
    files, rasters = BuildFixtures()
    if args.check:
        for name, expected in files.items():
            actual = (args.output / name).read_bytes()
            if actual != expected:
                raise SystemExit(f"MISMATCH {name}; never re-pin from production output")
        print(f"PASS: {len(files)} independently regenerated fixture files")
    elif args.extend:
        manifest_path = args.output / "manifest.json"
        original = json.loads(manifest_path.read_bytes())
        following = json.loads(files["manifest.json"])
        for key, value in original.items():
            if key in ("frames", "files"):
                if any(following[key].get(name) != entry for name, entry in value.items()):
                    raise SystemExit(f"Refusing extension that changes an existing {key} entry")
            elif following.get(key) != value:
                raise SystemExit(f"Refusing extension that changes frozen manifest field {key}")
        for name in original["files"]:
            if name not in files or (args.output / name).read_bytes() != files[name]:
                raise SystemExit(f"Refusing extension that changes or removes {name}")
        additions = []
        for name, data in files.items():
            if name == "manifest.json":
                continue
            destination = args.output / name
            if destination.exists():
                if destination.read_bytes() != data:
                    raise SystemExit(f"Refusing extension that changes existing file {name}")
            else:
                additions.append((destination, data))
        for destination, data in additions:
            with destination.open("xb") as stream:
                stream.write(data)
        temporary = manifest_path.with_suffix(".json.new")
        with temporary.open("xb") as stream:
            stream.write(files["manifest.json"])
        temporary.replace(manifest_path)
        print(f"Added {len(additions)} new files; preserved every existing fixture byte and digest pin")
    else:
        if any((args.output / name).exists() for name in files):
            raise SystemExit("Refusing to overwrite existing Golden files; use --check")
        args.output.mkdir(parents=True, exist_ok=True)
        for name, data in files.items():
            with (args.output / name).open("xb") as stream:
                stream.write(data)
        print(f"Created {len(files)} independent fixture files in {args.output}")
    if args.raster_output:
        if args.raster_output.resolve() == args.output.resolve():
            raise SystemExit("--raster-output must be separate from the small committed fixture directory")
        if any((args.raster_output / name).exists() for name in rasters):
            raise SystemExit("Refusing to overwrite exported PBRW artifacts")
        args.raster_output.mkdir(parents=True, exist_ok=True)
        for name, data in rasters.items():
            with (args.raster_output / name).open("xb") as stream:
                stream.write(data)
        print(f"Created {len(rasters)} independently rendered PBRW files in {args.raster_output}")


if __name__ == "__main__":
    Main()
