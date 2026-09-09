"""Generate isolated tool-only reference extensions without editing production sources."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path


def write_once(path, data):
    encoded = data.encode("utf-8")
    if path.exists():
        if path.read_bytes() != encoded:
            raise RuntimeError(f"Generated source changed; use a new build directory: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        output.write(encoded)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    identity = json.loads((args.repo / "artifacts/remote-step2-20260908-run01/identity/FINAL_SOURCE_IDENTITY.json").read_text(encoding="utf-8-sig"))
    hashes = {entry["path"]: entry["sha256"] for entry in identity["files"]}
    paths = ["libs/PBModulation/include/pbmodulation/unified_visual.h", "libs/PBModulation/src/unified_visual.cpp", "libs/PBModulation/src/local_desktop_decode.cpp"]
    original = {}
    for relative in paths:
        data = (args.repo / relative).read_bytes()
        if hashlib.sha256(data).hexdigest() != hashes[relative]:
            raise RuntimeError(f"Sealed production source identity changed: {relative}")
        original[relative] = data.decode("utf-8-sig").replace("\r\n", "\n")

    header, unified, local = [original[path] for path in paths]
    declaration = """    // Generated G1 tool-only entry; absent from the production public header.
    [[nodiscard]] UnifiedVisualObservation DecodeG1CanonicalFrame(const LumaView& view,
        const UnifiedExpectedFrameIdentity& expectedIdentity = {}, const UnifiedVisualDecodePolicy& policy = {}) noexcept;
"""
    marker = "    // Product GPU handoff."
    assert header.count(marker) == 1
    generated_header = header.replace(marker, declaration + marker)

    start = unified.index("UnifiedVisualObservation UnifiedVisualCpuOracle::DecodeInternal(")
    end = unified.index("UnifiedVisualObservation UnifiedVisualCpuOracle::FinalizeDecodedMetrics(", start)
    method = unified[start:end]
    body_start = method.index("{\n")
    candidate_method = """UnifiedVisualObservation UnifiedVisualCpuOracle::DecodeG1CanonicalFrame(const LumaView& view,
    const UnifiedExpectedFrameIdentity& expectedIdentity, const UnifiedVisualDecodePolicy& policy) noexcept
{
    const std::span<const UnifiedSlotAssignment> slotPlan{};
    constexpr bool inferSlotKinds = true;
""" + method[body_start + 2:]
    selection_start = candidate_method.index("    const LocalDesktopBootstrapBinding binding{")
    selection_end = candidate_method.index("    if (!observation.bootstrap.IsAccepted())\n    {\n        switch", selection_start)
    candidate_method = candidate_method[:selection_start] + """    const LocalDesktopDecodePolicy locatorPolicy = GetUnifiedLocatorPolicy(policy);
    observation.bootstrap = pbg1::ValidateCanonicalCandidate(view, locatorPolicy).bootstrap;
""" + candidate_method[selection_end:]
    # The original entry is retained byte-for-byte after newline normalization.
    generated_unified = '#include "candidate.h"\n' + unified + "\nnamespace pbmodulation\n{\n" + candidate_method + "}\n"

    evaluation_start = local.index("LocalDesktopObservation EvaluateGeometry(")
    evaluation_end = local.index("bool SameGeometry(", evaluation_start)
    evaluation = local[evaluation_start:evaluation_end]
    calibration_start = evaluation.index("    observation.erasure = Calibrate(")
    candidate_evaluation = """LocalDesktopObservation EvaluateG1ValidatedGeometry(LumaReader& reader, const LocalDesktopDecodePolicy& policy,
    const LocalDesktopGeometry& geometry, const detail::LocalDesktopBinding binding) noexcept
{
    LocalDesktopObservation observation;
    observation.geometry = geometry;
""" + evaluation[calibration_start:]
    generated_local = '#include "candidate.h"\n' + local + "\nnamespace pbmodulation\n{\nnamespace\n{\n" + candidate_evaluation + '\n}\n}\n#include "candidate_impl.inc"\n'
    outputs = {"include/pbmodulation/unified_visual.h": generated_header, "unified_visual_g1.cpp": generated_unified, "local_desktop_decode_g1.cpp": generated_local}
    originals = {"include/pbmodulation/unified_visual.h": header, "unified_visual_g1.cpp": unified, "local_desktop_decode_g1.cpp": local}
    for name, contents in outputs.items():
        write_once(args.output / name, contents)
        diff = "".join(difflib.unified_diff(originals[name].splitlines(True), contents.splitlines(True), fromfile="sealed/" + name, tofile="tool-only/" + name))
        write_once(args.output / (Path(name).name + ".diff"), diff)
    write_once(args.output / "generation.json", json.dumps({
        "schema": "PixelBridge.G1.ReferenceExtension.1", "sourceFingerprint": identity["sourceFingerprint"],
        "originalSources": [{"path": path, "sha256": hashes[path]} for path in paths],
        "outputs": [{"path": name, "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest()} for name, text in outputs.items()],
        "preservedOriginalFunctions": True,
        "candidateDelta": "One new tool-only oracle method replaces only Bootstrap/geometry selection; it reuses the unchanged downstream calibration/freshness/FEC/CRC backend. Candidate Bootstrap reuses the unchanged post-geometry validation body.",
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()
