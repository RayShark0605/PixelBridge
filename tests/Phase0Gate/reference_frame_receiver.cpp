#include "reference_frame_receiver.h"

#include "pbprotocol/blake3_digest.h"
#include "pbprotocol/byte_io.h"

#include <algorithm>
#include <string>
#include <utility>

namespace phase0gate
{
namespace
{

void RequireFrame(const bool condition, const ReferenceFrameErrorCode code, const std::size_t offset = 0)
{
    if (!condition)
    {
        throw ReferenceFrameFailure(code, offset);
    }
}

[[nodiscard]] bool IsZero(const std::span<const std::byte> bytes) noexcept
{
    return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value)
    {
        return value == std::byte{0};
    });
}

} // namespace

ReferenceFrameFailure::ReferenceFrameFailure(const ReferenceFrameErrorCode errorCode, const std::size_t errorOffset)
    : std::runtime_error("reference frame rejected: code=" + std::to_string(static_cast<int>(errorCode)) +
          " offset=" + std::to_string(errorOffset)), errorCode_(errorCode), errorOffset_(errorOffset)
{
}

ReferenceFrameErrorCode ReferenceFrameFailure::GetCode() const noexcept
{
    return errorCode_;
}

std::size_t ReferenceFrameFailure::GetOffset() const noexcept
{
    return errorOffset_;
}

ReferenceFrameDecoder::ReferenceFrameDecoder(const ReferenceBinding& binding)
    : binding_(binding)
{
    // Validate the whole binding before using any supplied size or profile.
    RequireFrame(binding_ == kReferenceBinding, ReferenceFrameErrorCode::BindingMismatch);
    const auto* const profile = pbinnerfec::GetInnerFecProfile(binding_.innerProfileId);
    RequireFrame(profile != nullptr && pbinnerfec::ValidateInnerFecProfile(*profile) &&
        profile->nBits == binding_.nBits && profile->kBits == binding_.kBits &&
        profile->matrixId == binding_.matrixId && profile->matrixDigest == binding_.matrixDigest &&
        profile->systematicBitOrder == binding_.bitOrder && profile->puncturingRule == binding_.puncturing &&
        pbmodulation::ValidateReferenceVisualProfile(), ReferenceFrameErrorCode::BindingMismatch);
    const auto manifest = pbmodulation::SerializeReferenceRegionManifest();
    // Existing PBVM Golden digest; do not derive the expected pin at runtime.
    constexpr std::array<std::byte, 32> manifestDigest{
        std::byte{0xa7}, std::byte{0xe3}, std::byte{0x1c}, std::byte{0xbd}, std::byte{0x7c}, std::byte{0xfa}, std::byte{0x68}, std::byte{0x65},
        std::byte{0xf8}, std::byte{0xbc}, std::byte{0x02}, std::byte{0x9a}, std::byte{0x78}, std::byte{0xd5}, std::byte{0x8d}, std::byte{0x06},
        std::byte{0x59}, std::byte{0x90}, std::byte{0xe7}, std::byte{0x32}, std::byte{0xf6}, std::byte{0x38}, std::byte{0x66}, std::byte{0x13},
        std::byte{0x32}, std::byte{0x9f}, std::byte{0x2d}, std::byte{0xd0}, std::byte{0x76}, std::byte{0x02}, std::byte{0x00}, std::byte{0x3b}};
    RequireFrame(pbprotocol::ComputeBlake3Digest(manifest) == manifestDigest, ReferenceFrameErrorCode::BindingMismatch);
    auto decoderResult = pbinnerfec::QcLdpcDecoder::Create(binding_.innerProfileId);
    RequireFrame(static_cast<bool>(decoderResult), ReferenceFrameErrorCode::BindingMismatch);
    decoder_ = std::move(decoderResult).Value();
    data_.resize(pbmodulation::kReferenceDataRegionBytes);
    llr_.resize(pbinnerfec::kDvbS2ShortFrameNBits);
    decodeOptions_.maxIterations = 48;
    decodeOptions_.syndromeCheckInterval = 1;
    decodeOptions_.offset = 0;
    decodeOptions_.scaleNum = 1;
    decodeOptions_.scaleDen = 1;
}

ReferenceDecodedFrame ReferenceFrameDecoder::Decode(const std::span<const std::byte> frameBgra)
{
    ReferenceDecodedFrame frame;
    std::array<std::byte, pbmodulation::kReferenceBootstrapRecordBytes> bootstrap{};
    const auto demodulated = pbmodulation::DecodeReferenceFrameInto(frameBgra, bootstrap, frame.control, data_);
    RequireFrame(static_cast<bool>(demodulated), ReferenceFrameErrorCode::RasterInvalid);
    const auto parsedBootstrap = pbprotocol::ParseBootstrapRecord(bootstrap);
    RequireFrame(static_cast<bool>(parsedBootstrap), ReferenceFrameErrorCode::BootstrapInvalid);
    frame.bootstrap = parsedBootstrap.Value();
    RequireFrame(frame.bootstrap.visualProfileId == binding_.visualProfileId,
        ReferenceFrameErrorCode::UnsupportedProfile, 8);
    RequireFrame(frame.bootstrap.visualLayoutVersion == binding_.layoutVersion,
        ReferenceFrameErrorCode::UnsupportedLayout, 7);

    if (!IsZero(frame.control))
    {
        // PB-Control-1 RecordBytes is the existing LE u32 at offset 22.
        // A single complete record plus canonical zeros is this composition's
        // fixed control mode; no sender length and no fragment/type guessing.
        pbprotocol::ByteReader lengthReader(std::span<const std::byte>(frame.control).subspan(22, 4));
        const auto length = lengthReader.ReadUint32();
        RequireFrame(static_cast<bool>(length) && length.Value() >= pbprotocol::kMinimumControlRecordBytes &&
            length.Value() <= frame.control.size(), ReferenceFrameErrorCode::ControlLength, 22);
        frame.controlBytes = length.Value();
        const auto parsedControl = pbprotocol::ParseControlRecord(
            std::span<const std::byte>(frame.control).first(frame.controlBytes));
        RequireFrame(static_cast<bool>(parsedControl), ReferenceFrameErrorCode::ControlInvalid);
        RequireFrame(parsedControl.Value().sessionTag == frame.bootstrap.sessionTag,
            ReferenceFrameErrorCode::SessionTagMismatch, 14);
        RequireFrame(IsZero(std::span<const std::byte>(frame.control).subspan(frame.controlBytes)),
            ReferenceFrameErrorCode::NonCanonicalPadding, frame.controlBytes);
    }

    for (std::size_t offset = 0; offset < data_.size(); offset += kReferenceCodewordBytes)
    {
        const auto remaining = std::span<const std::byte>(data_).subspan(offset);
        const auto codeword = remaining.first(std::min(kReferenceCodewordBytes, remaining.size()));
        if (IsZero(codeword))
        {
            RequireFrame(IsZero(remaining), ReferenceFrameErrorCode::NonCanonicalPadding, offset);
            break;
        }
        RequireFrame(codeword.size() == kReferenceCodewordBytes && frame.blockCount < frame.blocks.size(),
            ReferenceFrameErrorCode::NonCanonicalPadding, offset);
        const auto syndrome = pbinnerfec::ComputeQcLdpcSyndrome(binding_.innerProfileId, codeword);
        RequireFrame(syndrome && syndrome.Value(), ReferenceFrameErrorCode::SyndromeFailure, offset);
        for (std::size_t bitIndex = 0; bitIndex < llr_.size(); bitIndex++)
        {
            const auto mask = static_cast<std::byte>(static_cast<std::uint8_t>(1U << (bitIndex % 8U)));
            llr_[bitIndex] = (codeword[bitIndex / 8U] & mask) == std::byte{0} ? 30000 : -30000;
        }
        const auto decoded = decoder_.Decode(llr_, decodeOptions_, decodedCodeword_);
        RequireFrame(decoded && std::ranges::equal(codeword, decodedCodeword_),
            ReferenceFrameErrorCode::InnerDecodeFailure, offset);
        const auto extracted = pbprotocol::ExtractTransportBlockFromInfoBlock(
            std::span<const std::byte>(decodedCodeword_).first(kReferenceInfoBytes));
        RequireFrame(static_cast<bool>(extracted), ReferenceFrameErrorCode::TransportInvalid, offset);
        const auto parsed = pbprotocol::ParseTransportBlock(extracted.Value());
        RequireFrame(parsed && parsed.Value().payload.size() <= binding_.outerBlockBytes,
            ReferenceFrameErrorCode::TransportInvalid, offset);
        RequireFrame(parsed.Value().header.sessionTag == frame.bootstrap.sessionTag,
            ReferenceFrameErrorCode::SessionTagMismatch, offset);
        auto& block = frame.blocks[frame.blockCount];
        block.header = parsed.Value().header;
        std::copy(parsed.Value().payload.begin(), parsed.Value().payload.end(), block.paddedPayload.begin());
        frame.blockCount++;
    }
    return frame;
}

std::size_t ReferenceFrameDecoder::GetWorkingBytes() const noexcept
{
    return data_.size() + llr_.size() * sizeof(std::int16_t) + decodedCodeword_.size() +
        sizeof(ReferenceDecodedFrame) + pbmodulation::kReferenceBootstrapRecordBytes;
}

} // namespace phase0gate
