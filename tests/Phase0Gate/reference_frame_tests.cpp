#include "reference_frame_tests.h"
#include "reference_frame_receiver.h"

#include "pbprotocol/protocol_version.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>

namespace phase0gate
{
namespace
{

constexpr pbprotocol::SessionTag kTestSessionTag{0x1122334455667788ULL};

void Check(const bool condition, const std::string_view message)
{
    if (!condition)
    {
        throw std::runtime_error("reference boundary regression: " + std::string(message));
    }
}

// Bitwise test oracle, independent of the production CRC implementation.
[[nodiscard]] std::uint32_t OracleCrc32c(const std::span<const std::byte> bytes)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto value : bytes)
    {
        crc ^= std::to_integer<std::uint8_t>(value);
        for (std::uint32_t bit = 0; bit < 8; bit++)
        {
            crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? 0x82F63B78U : 0U);
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

void PutUint32(const std::span<std::byte> bytes, const std::size_t offset, const std::uint32_t value)
{
    for (std::size_t index = 0; index < 4; index++)
    {
        bytes[offset + index] = static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

void RewriteCrc(const std::span<std::byte> bytes, const std::size_t offset)
{
    PutUint32(bytes, offset, OracleCrc32c(bytes.first(offset)));
}

[[nodiscard]] std::string Hex(const std::span<const std::byte> bytes)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    for (const auto value : bytes)
    {
        const auto numeric = std::to_integer<std::uint8_t>(value);
        result += digits[numeric >> 4U];
        result += digits[numeric & 15U];
    }
    return result;
}

struct FrameFixture
{
    std::array<std::byte, 44> bootstrap{};
    std::array<std::byte, 240> control{};
    std::vector<std::byte> data = std::vector<std::byte>(56168);
    std::vector<std::byte> raster = std::vector<std::byte>(1920U * 1080U * 4U);

    void Reset()
    {
        const pbprotocol::BootstrapRecord record{1, {1, 0}, 1, 0x5042524546524153ULL,
            kTestSessionTag, 0x0102030405060708ULL, 0, 0};
        Check(static_cast<bool>(pbprotocol::SerializeBootstrapRecord(record, bootstrap)), "serialize Bootstrap");
        std::fill(control.begin(), control.end(), std::byte{0});
        std::fill(data.begin(), data.end(), std::byte{0});
    }

    void SetControl(const std::size_t recordBytes = 31)
    {
        const std::vector<std::byte> payload(recordBytes - 30U, std::byte{0x5A});
        const pbprotocol::ControlRecordView record{1, pbprotocol::ControlRecordType::SessionDescriptor,
            0, kTestSessionTag, payload};
        Check(static_cast<bool>(pbprotocol::SerializeControlRecord(record,
            std::span<std::byte>(control).first(recordBytes))), "serialize Control envelope");
    }

    void Render()
    {
        Check(static_cast<bool>(pbmodulation::EncodeReferenceFrame({bootstrap, control, data}, raster)), "render fixture");
    }
};

[[nodiscard]] std::array<std::byte, 1350> MakeInfoBlock(const std::uint16_t payloadBytes = 1,
    const pbprotocol::SessionTag sessionTag = kTestSessionTag)
{
    const std::vector<std::byte> payload(payloadBytes, std::byte{0x5A});
    const pbprotocol::TransportBlockHeader header{1, 0, 0, sessionTag, 7, 73, payloadBytes};
    std::vector<std::byte> transport(pbprotocol::GetTransportSerializedSize(header));
    Check(static_cast<bool>(pbprotocol::SerializeTransportBlock(header, payload, transport)), "serialize Transport");
    std::array<std::byte, 1350> info{};
    Check(static_cast<bool>(pbprotocol::FrameTransportBlockIntoInfoBlock(transport, info.size(), info)), "frame Transport");
    return info;
}

void SetCodeword(FrameFixture& fixture, const std::span<const std::byte> info, const std::size_t slot = 0)
{
    Check(static_cast<bool>(pbinnerfec::EncodeQcLdpcCodeword(pbinnerfec::kInnerFecProfileIdRobust,
        info, std::span<std::byte>(fixture.data).subspan(slot * 2025, 2025))), "encode codeword");
}

void ExpectRejected(ReferenceFrameDecoder& decoder, FrameFixture& fixture,
    const ReferenceFrameErrorCode code, const std::size_t offset, const std::string_view name)
{
    fixture.Render();
    try
    {
        static_cast<void>(decoder.Decode(fixture.raster));
    }
    catch (const ReferenceFrameFailure& failure)
    {
        Check(failure.GetCode() == code && failure.GetOffset() == offset,
            std::string(name) + " wrong error/offset: " + failure.what());
        return;
    }
    Check(false, std::string(name) + " accepted malformed frame");
}

[[nodiscard]] std::string Event(const std::string_view name)
{
    return "{\"case\":\"reference-" + std::string(name) + "\",\"status\":\"pass\"}";
}

} // namespace

std::vector<std::string> RunReferenceFrameTests()
{
    std::vector<std::string> evidence;
    ReferenceFrameDecoder decoder;
    FrameFixture fixture;
    fixture.Reset();
    // Independent literal answer includes CRC; changing serializers or profile
    // constants cannot silently regenerate this oracle or any existing pin.
    Check(Hex(fixture.bootstrap) == "504252470101000153415246455242508877665544332211080706050403020100000000000000008e4f0cee",
        "Bootstrap known answer");
    Check(kReferenceBinding.innerProfileId == 0x36BC661265E826C3ULL &&
        kReferenceBinding.matrixId == 0xB3F9EFAD6196DE85ULL &&
        kReferenceBinding.nBits == 16200 && kReferenceBinding.kBits == 10800 &&
        kReferenceBinding.bitOrder == 1 && kReferenceBinding.interleave == 0 &&
        kReferenceBinding.puncturing == 0 && kReferenceBinding.featureFlags == 0,
        "independent fixed composition answer");
    evidence.push_back(Event("binding-known-answer"));

    for (std::size_t variant = 0; variant < 17; variant++)
    {
        auto binding = kReferenceBinding;
        switch (variant)
        {
        case 0: binding.visualProfileId ^= 1; break;
        case 1: binding.layoutVersion++; break;
        case 2: binding.innerProfileId ^= 1; break;
        case 3: binding.nBits--; break;
        case 4: binding.kBits--; break;
        case 5: binding.matrixId ^= 1; break;
        case 6: binding.matrixDigest[0] ^= std::byte{1}; break;
        case 7: binding.bitOrder = 0; break;
        case 8: binding.puncturing = 1; break;
        case 9: binding.interleave = 1; break;
        case 10: binding.featureFlags = 1; break;
        case 11: binding.width = UINT32_MAX; break;
        case 12: binding.height = UINT32_MAX; break;
        case 13: binding.outerBlockBytes++; break;
        case 14: binding.controlWindowBytes = UINT32_MAX; break;
        case 15: binding.dataRegionBytes = UINT32_MAX; break;
        default: binding.matrixDigest[31] ^= std::byte{1}; break;
        }
        bool rejected = false;
        try
        {
            const ReferenceFrameDecoder invalid(binding);
        }
        catch (const ReferenceFrameFailure& failure)
        {
            rejected = failure.GetCode() == ReferenceFrameErrorCode::BindingMismatch && failure.GetOffset() == 0;
        }
        Check(rejected, "unsupported binding must fail before dimensions are used");
    }
    evidence.push_back("{\"case\":\"reference-binding-negative\",\"status\":\"pass\",\"variants\":17}");

    for (const std::size_t count : {0U, 1U, 27U})
    {
        fixture.Reset();
        if (count != 0)
        {
            fixture.SetControl();
        }
        for (std::size_t slot = 0; slot < count; slot++)
        {
            SetCodeword(fixture, MakeInfoBlock(slot == 0 ? 1 : 1314), slot);
        }
        fixture.Render();
        const auto frame = decoder.Decode(fixture.raster);
        Check(frame.blockCount == count && frame.controlBytes == (count == 0 ? 0 : 31), "self-delimiting count");
        Check(frame.bootstrap.sessionTag == kTestSessionTag && frame.bootstrap.frameSequence == 0x0102030405060708ULL,
            "Bootstrap decoded answer");
        for (std::size_t slot = 0; slot < count; slot++)
        {
            Check(frame.blocks[slot].header.segmentOrdinal == 7 && frame.blocks[slot].header.outerBlockId == 73 &&
                frame.blocks[slot].header.payloadBytes == (slot == 0 ? 1 : 1314) &&
                frame.blocks[slot].paddedPayload[0] == std::byte{0x5A}, "independent Transport answer");
            if (slot == 0)
            {
                Check(frame.blocks[slot].paddedPayload.back() == std::byte{0}, "short payload zero padding");
            }
        }
        evidence.push_back(Event("codewords-" + std::to_string(count)));
    }

    for (const std::size_t recordBytes : {30U, 240U})
    {
        fixture.Reset();
        fixture.SetControl(recordBytes);
        fixture.Render();
        const auto frame = decoder.Decode(fixture.raster);
        Check(frame.controlBytes == recordBytes && frame.blockCount == 0, "Control window exact boundary");
        evidence.push_back(Event("control-window-" + std::to_string(recordBytes)));
    }

    const auto rejected = [&](const std::string_view name, const ReferenceFrameErrorCode code, const std::size_t offset = 0)
    {
        ExpectRejected(decoder, fixture, code, offset, name);
        evidence.push_back(Event(name));
    };
    fixture.Reset();
    fixture.bootstrap[8] ^= std::byte{1};
    RewriteCrc(fixture.bootstrap, 40);
    rejected("unknown-profile-crc-valid", ReferenceFrameErrorCode::UnsupportedProfile, 8);
    fixture.Reset();
    fixture.bootstrap[7] = std::byte{2};
    RewriteCrc(fixture.bootstrap, 40);
    rejected("unknown-layout-crc-valid", ReferenceFrameErrorCode::UnsupportedLayout, 7);
    fixture.Reset();
    fixture.bootstrap[36] = std::byte{1};
    RewriteCrc(fixture.bootstrap, 40);
    rejected("unsupported-feature-crc-valid", ReferenceFrameErrorCode::BootstrapInvalid);
    fixture.Reset();
    fixture.bootstrap[5] = std::byte{2};
    RewriteCrc(fixture.bootstrap, 40);
    rejected("unsupported-major-crc-valid", ReferenceFrameErrorCode::BootstrapInvalid);
    fixture.Reset();
    fixture.bootstrap[40] ^= std::byte{1};
    rejected("bootstrap-crc", ReferenceFrameErrorCode::BootstrapInvalid);

    for (const std::uint32_t length : {0U, 29U, 241U, UINT32_MAX})
    {
        fixture.Reset();
        fixture.SetControl();
        PutUint32(fixture.control, 22, length);
        rejected("control-length-" + std::to_string(length), ReferenceFrameErrorCode::ControlLength, 22);
    }
    fixture.Reset();
    fixture.SetControl();
    PutUint32(fixture.control, 22, 30);
    rejected("control-truncated-record", ReferenceFrameErrorCode::ControlInvalid);
    fixture.Reset();
    fixture.SetControl();
    fixture.control[30] ^= std::byte{1};
    rejected("control-crc", ReferenceFrameErrorCode::ControlInvalid);
    fixture.Reset();
    fixture.SetControl();
    fixture.control[31] = std::byte{1};
    rejected("control-trailing", ReferenceFrameErrorCode::NonCanonicalPadding, 31);
    fixture.Reset();
    fixture.SetControl();
    fixture.control[14] ^= std::byte{1};
    RewriteCrc(fixture.control, 27);
    rejected("control-session-mismatch", ReferenceFrameErrorCode::SessionTagMismatch, 14);
    fixture.Reset();
    SetCodeword(fixture, MakeInfoBlock(), 1);
    rejected("zero-window-followed-by-codeword", ReferenceFrameErrorCode::NonCanonicalPadding);
    fixture.Reset();
    for (std::size_t slot = 0; slot < 27; slot++)
    {
        SetCodeword(fixture, MakeInfoBlock(), slot);
    }
    fixture.data.back() = std::byte{1};
    rejected("nonzero-partial-window", ReferenceFrameErrorCode::NonCanonicalPadding, 27U * 2025U);

    for (const std::size_t byteOffset : {28U, 33U, 37U})
    {
        fixture.Reset();
        auto info = MakeInfoBlock();
        info[byteOffset] ^= std::byte{1};
        SetCodeword(fixture, info);
        rejected("transport-reencoded-invalid-" + std::to_string(byteOffset), ReferenceFrameErrorCode::TransportInvalid);
    }
    fixture.Reset();
    SetCodeword(fixture, MakeInfoBlock(1, pbprotocol::SessionTag{kTestSessionTag.value ^ 1U}));
    rejected("transport-session-mismatch", ReferenceFrameErrorCode::SessionTagMismatch);
    fixture.Reset();
    SetCodeword(fixture, MakeInfoBlock());
    fixture.data[100] ^= std::byte{1};
    rejected("syndrome-corruption", ReferenceFrameErrorCode::SyndromeFailure);
    fixture.Reset();
    SetCodeword(fixture, MakeInfoBlock());
    for (auto& value : std::span<std::byte>(fixture.data).first(2025))
    {
        auto numeric = std::to_integer<std::uint8_t>(value);
        std::uint8_t reversed = 0;
        for (std::size_t bit = 0; bit < 8; bit++)
        {
            reversed = static_cast<std::uint8_t>((reversed << 1U) | (numeric & 1U));
            numeric >>= 1U;
        }
        value = static_cast<std::byte>(reversed);
    }
    rejected("wrong-packed-bit-order", ReferenceFrameErrorCode::SyndromeFailure);

    // Fixed, bounded input mutation for this newly exposed receiver boundary.
    // Every generated case has an independent expected rejection. This is not
    // coverage-guided fuzzing and does not replace the existing 830000 budget.
    std::uint64_t state = 20260827;
    for (std::size_t iteration = 0; iteration < 128; iteration++)
    {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        fixture.Reset();
        const auto nonzero = static_cast<std::byte>(1U + state % 255U);
        switch (iteration % 4)
        {
        case 0:
            fixture.bootstrap[8 + state % 8U] ^= nonzero;
            RewriteCrc(fixture.bootstrap, 40);
            ExpectRejected(decoder, fixture, ReferenceFrameErrorCode::UnsupportedProfile, 8, "mutation profile");
            break;
        case 1:
            fixture.SetControl();
            PutUint32(fixture.control, 22, 241U + static_cast<std::uint32_t>(state % 100000U));
            ExpectRejected(decoder, fixture, ReferenceFrameErrorCode::ControlLength, 22, "mutation length");
            break;
        case 2:
            fixture.data[2025 + state % (56168U - 2025U)] = nonzero;
            ExpectRejected(decoder, fixture, ReferenceFrameErrorCode::NonCanonicalPadding, 0, "mutation after zero");
            break;
        default:
            fixture.SetControl();
            fixture.control[31 + state % (240U - 31U)] = nonzero;
            ExpectRejected(decoder, fixture, ReferenceFrameErrorCode::NonCanonicalPadding, 31, "mutation control tail");
            break;
        }
    }
    fixture.Reset();
    fixture.Render();
    Check(decoder.Decode(fixture.raster).blockCount == 0, "decoder reusable after rejection");
    evidence.push_back("{\"case\":\"reference-receiver-mutation\",\"status\":\"pass\",\"seed\":20260827,"
        "\"iterations\":128,\"rejected\":128,\"completion_marker\":\"REFERENCE_RECEIVER_MUTATION_COMPLETED\"}");
    return evidence;
}

} // namespace phase0gate
