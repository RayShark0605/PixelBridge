"""Append read-only edge instrumentation; preserve every production function."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path


def write_once(path, contents):
    data = contents.encode("utf-8")
    if path.exists():
        if path.read_bytes() != data:
            raise RuntimeError(f"Different generated source exists; use a new build: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as stream:
        stream.write(data)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    identity = json.loads((args.repo / "artifacts/remote-step2-20260908-run01/identity/FINAL_SOURCE_IDENTITY.json").read_text(encoding="utf-8-sig"))
    relative = "libs/PBModulation/src/local_desktop_decode.cpp"
    expected = next(item["sha256"] for item in identity["files"] if item["path"] == relative)
    data = (args.repo / relative).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError("Sealed production locator source changed")
    original = data.decode("utf-8-sig").replace("\r\n", "\n")
    generated = original + '\n#include "edge_probe.h"\n#include "edge_probe_impl.inc"\n'
    write_once(args.output / "local_desktop_decode_g1b.cpp", generated)
    diff = "".join(difflib.unified_diff(original.splitlines(True), generated.splitlines(True), fromfile=relative, tofile="tool-only/local_desktop_decode_g1b.cpp"))
    write_once(args.output / "local_desktop_decode_g1b.cpp.diff", diff)
    write_once(args.output / "generation.json", json.dumps({
        "schema": "PixelBridge.G1B.ReferenceInstrumentation.1", "originalPath": relative, "originalSha256": expected,
        "generatedSha256": hashlib.sha256(generated.encode("utf-8")).hexdigest(),
        "originalIsUnmodifiedPrefix": generated.startswith(original), "publicHeaderChanged": False,
        "admissionCandidateImplemented": False,
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
