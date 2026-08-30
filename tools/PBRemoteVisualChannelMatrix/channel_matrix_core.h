#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pbremotevisualmatrix
{

inline constexpr char kChannelMatrixSchema[] = "PixelBridge.RemoteVisualChannelMatrix.1";
inline constexpr std::uint32_t kChannelMatrixVersion = 1;
inline constexpr std::size_t kChannelMatrixDigestBytes = 32;

enum class ChannelMatrixClassification : std::uint8_t
{
    Verified,
    ErasureNoFalseAccept,
    RejectedNoFalseAccept,
    FalseAcceptance,
    ExecutionFailure
};

struct ChannelMatrixCaseSummary
{
    std::string name;
    std::string impairmentClass;
    std::string expected;
    ChannelMatrixClassification classification = ChannelMatrixClassification::ExecutionFailure;
    bool expectationMatched = false;
    bool modulationAccepted = false;
    bool evaluationVerified = false;
    std::uint32_t falseAcceptedCodewords = 0;
    std::uint32_t acceptedTransportBlocks = 0;
    std::uint32_t staleRegions = 0;
    std::string erasure;
    std::string manifestBlake3;
    std::string outputBlake3;
};

struct ChannelMatrixReport
{
    std::vector<ChannelMatrixCaseSummary> cases;
    bool truthBoundaryValid = false;
    bool expectationsMatched = false;
    std::array<std::byte, kChannelMatrixDigestBytes> payloadBlake3{};
    std::string canonicalJson;
};

// Builds the bounded, deterministic LF4 reference matrix entirely in memory.
// Every case uses the production LF4 modulation, production QC-LDPC and
// Transport truth boundary after applying exactly the transforms recorded in
// its embedded channel manifest. No expected payload is supplied to demod/FEC.
// On failure, no partial report is returned.
[[nodiscard]] bool BuildDefaultChannelMatrix(ChannelMatrixReport& output, std::string& error);

[[nodiscard]] const char* GetChannelMatrixClassificationName(ChannelMatrixClassification classification) noexcept;

} // namespace pbremotevisualmatrix
