#pragma once
// PBFrameInspector core: deterministic reference-frame inspector.
//
// Accepts one frame container (PBRW raw or PNG) and reports, in
// deterministic output (no timestamps):
//
//   - container identity and validation status (magic, version, reserved
//     bytes, declared geometry, pixel-byte count, exact content length);
//   - the frozen 14-region table with pixel offsets;
//   - the demodulated bootstrap record (fields + CRC recompute + the
//     authoritative pbprotocol parse);
//   - the 240-byte control window parsed as a PBCR record sequence with
//     DIAGNOSTIC semantics: a bad record is reported but never fails the
//     frame;
//   - the 56,168-byte data region digest and trailing-zero length;
//   - with --recovery: a fail-closed scan of 2,025-byte Robust LDPC
//     windows (zero window == padding stop; a non-zero window must pass
//     the syndrome, a bounded perfect-LLR decode (magnitude 100, a
//     documented receiver-local reference choice, not a wire field),
//     info-block extraction and the strict Transport parse), with a
//     decoded-SessionTag vs bootstrap-SessionTag cross-check whose
//     mismatch fails the inspection.
//
// Zero-allocation fail-closed contract: both container paths validate the
// declared geometry against the frozen 1920x1080 canvas BEFORE any canvas
// allocation. A hostile PBRW header (e.g. 16384x16384) or a non-expected
// PNG IHDR is rejected with FrameGeometryMismatch and canvasAllocationBytes
// stays 0, so tests can assert that no canvas buffer was ever allocated.
// The 8,294,400-byte canvas is allocated only for inputs that already
// declared the exact frozen geometry.
//
// Error offset semantics: frame-level errors carry the canvas linear pixel
// index (y * 1920 + x); container/region-level errors carry byte offsets;
// recovery errors carry the window byte offset inside the 56,168-byte data
// region. All protocol decisions are fail-closed; exception containment is
// described on InspectFrameContainer below.

#include "pbmodulation/frame_io.h"
#include "pbmodulation/modulation_result.h"
#include "pbmodulation/reference_raster.h"
#include "pbmodulation/reference_visual_profile.h"
#include "pbprotocol/protocol_result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pbinspect {

// Frozen reference canvas (mirrors pbmodulation constants; kept local so
// the report contract is self-documenting).
inline constexpr std::uint32_t kExpectedCanvasWidth =
    pbmodulation::kReferenceCanvasWidth; // 1920
inline constexpr std::uint32_t kExpectedCanvasHeight =
    pbmodulation::kReferenceCanvasHeight; // 1080
inline constexpr std::size_t kCanvasAllocationBytes =
    pbmodulation::kReferenceFrameBgraBytes; // 8,294,400

// Recovery chain constants (Robust DVB-S2 Short profile).
inline constexpr std::size_t kRobustCodewordBytes = 2025;
inline constexpr std::size_t kRobustInfoBlockBytes = 1350;
inline constexpr std::size_t kDataRegionBytes =
    pbmodulation::kReferenceDataRegionBytes; // 56,168
// Perfect-LLR magnitude for the clean-channel reference decode. Receiver-
// local reference choice (int16 headroom), not a wire field.
inline constexpr std::int16_t kPerfectLlrMagnitude = 100;

struct RegionRow
{
    std::size_t index = 0;
    std::string typeName;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Canvas linear pixel index of the region origin (y * 1920 + x).
    std::size_t pixelOffset = 0;
    std::size_t pixelBytes = 0; // width * height * 4
};

struct RecoveredBlock
{
    std::size_t windowIndex = 0;
    // Byte offset of the codeword window inside the data region.
    std::size_t windowByteOffset = 0;
    std::uint64_t sessionTag = 0;
    std::uint64_t segmentOrdinal = 0;
    std::uint32_t outerBlockId = 0;
    std::size_t payloadBytes = 0;
    std::uint32_t payloadCrcStored = 0;
    std::uint32_t payloadCrcRecomputed = 0;
    std::array<std::byte, 32> payloadBlake3{};
};

struct FailureDiagnostic
{
    std::string space;
    std::size_t byteOffset = 0;
    std::string expected;
    std::string actual;
    // Optional nested coordinate system for recovery errors.
    std::size_t windowByteOffset = 0;
    std::size_t innerByteOffset = 0;
    std::size_t dataRegionByteOffset = 0;
    bool hasNestedOffsets = false;
};

struct InspectorReport
{
    // Container gate.
    // "pbrw" | "png" | "unrecognized".
    std::string containerType = "unrecognized";
    bool containerOk = false;
    // ModulationErrorCode name when the container gate failed.
    std::string containerError;
    std::size_t containerErrorOffset = 0;
    std::size_t fileBytes = 0;
    // Declared container geometry (0 when not readable).
    std::uint32_t declaredWidth = 0;
    std::uint32_t declaredHeight = 0;
    // kCanvasAllocationBytes exactly when the canvas was allocated
    // (0 when a hostile header was rejected pre-allocation).
    std::size_t canvasAllocationBytes = 0;

    // Frame demod gate (canvas linear pixel index offsets).
    bool rasterDemodulated = false;
    bool frameOk = false;
    std::string frameError;
    std::size_t frameErrorOffset = 0;
    // Frozen 14-region table (always populated).
    std::vector<RegionRow> regions;

    // Bootstrap (present when frameOk).
    bool bootstrapParsed = false;
    std::string bootstrapError;
    std::size_t bootstrapErrorOffset = 0;
    std::uint64_t bootstrapSessionTag = 0;
    std::uint64_t bootstrapFrameSequence = 0;
    std::uint32_t bootstrapControlEpoch = 0;
    std::uint32_t bootstrapFlags = 0;
    std::uint64_t bootstrapVisualProfileId = 0;
    std::uint32_t bootstrapCrcStored = 0;
    std::uint32_t bootstrapCrcRecomputed = 0;

    // Control window (diagnostic semantics: never fails the frame).
    int controlRecordCount = 0;
    // "records-fill-window" | "zero-padding" | "unparseable-tail".
    std::string controlTailStatus = "zero-padding";
    // One deterministic line per observed record or tail condition.
    std::vector<std::string> controlDiagnostics;

    // Data region (present when frameOk).
    std::array<std::byte, 32> dataBlake3{};
    std::size_t dataTrailingZeroBytes = 0;

    // --recovery chain (fail-closed; window offsets inside the data
    // region).
    bool recoveryRequested = false;
    // True only after the recovery chain was entered. A requested recovery
    // remains not-run when the container, raster, or Bootstrap gate failed.
    bool recoveryRan = false;
    bool recoveryOk = true;
    // Non-zero codeword windows scanned before the stop condition.
    int recoveryWindowCount = 0;
    bool recoveryStoppedAtPadding = false;
    std::string recoveryError;
    std::size_t recoveryErrorOffset = 0;
    bool sessionTagCrossCheckOk = true;
    std::vector<RecoveredBlock> blocks;

    // Every byte-attributable failure is rendered in the common
    // offset/expected/actual form. Control-plane entries remain diagnostic
    // only and do not change frameOk.
    std::vector<FailureDiagnostic> diagnostics;
};

// Inspects one frame container. Ordinary allocation/filesystem/codec
// exceptions are converted to a fail-closed report when a bounded diagnostic
// can still be built. The API is intentionally not noexcept: the CLI boundary
// performs the final catch if even diagnostic construction cannot allocate.
// input is borrowed and must outlive the call.
void InspectFrame(const std::span<const std::byte> input,
    const bool recoveryRequested, InspectorReport& outReport);

// Renders the deterministic stdout for a report (byte-stable, no
// timestamps). Not noexcept: builds strings.
[[nodiscard]] std::string FormatReport(const InspectorReport& report);

} // namespace pbinspect
