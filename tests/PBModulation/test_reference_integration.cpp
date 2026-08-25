#include "modulation_test_helpers.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

using namespace pbmodtest;
using namespace pbmodulation;

namespace {

// Fills one 4x4 data tile at (x, y) with the given luma value.
void FillWholeDataTile(
    const std::span<std::byte> bgra,
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint8_t level)
{
    for (std::uint32_t row = 0; row < 4; row++)
    {
        for (std::uint32_t col = 0; col < 4; col++)
        {
            SetFramePixel(bgra, x + col, y + row, level, level, level,
                kReferenceAlphaValue);
        }
    }
}

} // namespace

TEST_CASE("End-to-end clean channel: payload to container and back",
    "[pbmodulation][integration][clean]")
{
    const GoldenFramePayload payload = MakeCanonicalGoldenPayload();
    const std::vector<std::byte> frame =
        EncodeFrameOrDie(payload.MakeInput());

    // Raw channel.
    const auto rawResult = EncodeRawFrame(frame, 1920, 1080);
    REQUIRE(rawResult);
    std::vector<std::byte> rawFrame(frame.size());
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    const auto rawStatus = DecodeRawFrame(
        std::span<const std::byte>(rawResult.Value()),
        std::span<std::byte>(rawFrame), width, height);
    REQUIRE(rawStatus);
    CHECK(rawFrame == frame);
    CHECK(DecodeFrameOrDie(rawFrame) ==
        DecodedReferenceFrame{
            payload.bootstrap,
            payload.control,
            payload.data});

    // PNG channel.
    const auto pngResult = EncodePngFrame(frame, 1920, 1080);
    REQUIRE(pngResult);
    std::vector<std::byte> pngFrame(frame.size());
    const auto pngStatus = DecodePngFrame(
        std::span<const std::byte>(pngResult.Value()), 1920, 1080,
        std::span<std::byte>(pngFrame));
    REQUIRE(pngStatus);
    CHECK(pngFrame == frame);
    CHECK(DecodeFrameOrDie(pngFrame) ==
        DecodedReferenceFrame{
            payload.bootstrap,
            payload.control,
            payload.data});
}

TEST_CASE("A single-symbol pixel error recovers through Inner-FEC",
    "[pbmodulation][integration][fec-recovery]")
{
    const GoldenFramePayload payload = MakeCanonicalGoldenPayload();
    const std::vector<std::byte> codeword(
        payload.data.begin(),
        payload.data.begin() + 2025);
    const std::vector<std::byte> info(
        payload.data.begin(),
        payload.data.begin() + 1350);

    // Find the first even codeword tile (4x4 block) whose 4-bit symbol is
    // 0 (level 8). Tampering that whole tile to level 24 flips exactly
    // stream bit 4*tileIndex; with an even tileIndex that bit is bit 0 of
    // byte targetTile/2, producing a single-bit codeword error at a
    // precisely pinned position.
    std::size_t targetTile = static_cast<std::size_t>(-1);
    for (std::size_t tileIndex = 0; tileIndex < 4050; tileIndex += 2)
    {
        const std::uint8_t byteValue =
            std::to_integer<std::uint8_t>(payload.data[tileIndex / 2]);
        const std::uint8_t symbol = byteValue & 0x0Fu;
        if (symbol == 0)
        {
            targetTile = tileIndex;
            break;
        }
    }
    REQUIRE(targetTile != static_cast<std::size_t>(-1));

    const std::vector<std::byte> cleanFrame =
        EncodeFrameOrDie(payload.MakeInput());
    std::vector<std::byte> frame = cleanFrame;
    const auto [tileX, tileY] = GetDataTileOrigin(
        static_cast<std::uint32_t>(targetTile % kReferenceDataGridColumns),
        static_cast<std::uint32_t>(
            targetTile / kReferenceDataGridColumns));
    FillWholeDataTile(std::span<std::byte>(frame), tileX, tileY, 24);

    const DecodedReferenceFrame decoded = DecodeFrameOrDie(frame);

    // Exactly one bit of the codeword region is wrong, at 4*targetTile.
    std::size_t errorBits = 0;
    for (std::size_t i = 0; i < 2025; i++)
    {
        const std::uint8_t a =
            std::to_integer<std::uint8_t>(codeword[i]);
        const std::uint8_t b =
            std::to_integer<std::uint8_t>(decoded.data[i]);
        for (std::uint8_t bit = 0; bit < 8; bit++)
        {
            if (((a >> bit) & 1u) != ((b >> bit) & 1u))
            {
                CHECK(bit == 0);
                CHECK(i == targetTile / 2);
                errorBits++;
            }
        }
    }
    CHECK(errorBits == 1);
    CHECK(static_cast<std::size_t>(4u * targetTile) < 16200u);

    // Hard-decision LLRs from the demodulated bits (llr > 0 -> bit 0).
    std::vector<std::int16_t> llr(16200);
    for (std::size_t bitIndex = 0; bitIndex < 16200; bitIndex++)
    {
        const bool bit =
            (decoded.data[bitIndex / 8] &
                std::byte{static_cast<std::uint8_t>(
                    1u << (bitIndex % 8u))}) !=
            std::byte{0};
        llr[bitIndex] = bit ? -1000 : 1000;
    }

    auto decoderResult = pbinnerfec::QcLdpcDecoder::Create(
        pbinnerfec::kInnerFecProfileIdRobust);
    REQUIRE(decoderResult);
    pbinnerfec::QcLdpcDecoder& decoder = decoderResult.Value();
    // Strong hard-decision channel: the default 2048 min-sum offset would
    // clamp every |1000| internal message to zero and freeze the decoder on
    // the received bits, so the exact min-sum (offset 0, scale 1/1) is the
    // deterministic reference setting for this recovery proof.
    pbinnerfec::InnerFecDecodeOptions decodeOptions;
    decodeOptions.maxIterations = 48;
    decodeOptions.syndromeCheckInterval = 1;
    decodeOptions.offset = 0;
    decodeOptions.scaleNum = 1;
    decodeOptions.scaleDen = 1;
    std::vector<std::byte> recovered(2025);
    const auto decodeResult = decoder.Decode(
        std::span<const std::int16_t>(llr),
        decodeOptions,
        std::span<std::byte>(recovered));
    REQUIRE(decodeResult);
    CHECK(recovered == codeword);
    CHECK(std::equal(recovered.begin(), recovered.begin() + 1350,
        info.begin()));
}

TEST_CASE("A multi-frame stream round-trips frame by frame",
    "[pbmodulation][integration][multi-frame]")
{
    std::vector<GoldenFramePayload> frames;
    frames.push_back(MakeZeroGoldenPayload());
    frames.push_back(MakeCanonicalGoldenPayload());
    {
        GoldenFramePayload random = MakeZeroGoldenPayload();
        SplitMix64 rng(0xF1E1F00Du);
        for (auto& value : random.bootstrap)
        {
            value = Byte(rng.Next() & 0xFFu);
        }
        for (auto& value : random.control)
        {
            value = Byte(rng.Next() & 0xFFu);
        }
        for (auto& value : random.data)
        {
            value = Byte(rng.Next() & 0xFFu);
        }
        frames.push_back(random);
    }

    std::vector<std::vector<std::byte>> encodedFrames;
    for (const auto& payload : frames)
    {
        encodedFrames.push_back(
            EncodeFrameOrDie(payload.MakeInput()));
    }
    // Distinct payloads render distinct rasters.
    CHECK(encodedFrames[0] != encodedFrames[1]);
    CHECK(encodedFrames[1] != encodedFrames[2]);
    CHECK(encodedFrames[0] != encodedFrames[2]);

    for (std::size_t frameIndex = 0; frameIndex < frames.size();
        frameIndex++)
    {
        const DecodedReferenceFrame decoded =
            DecodeFrameOrDie(encodedFrames[frameIndex]);
        CHECK(decoded.bootstrapRecord == frames[frameIndex].bootstrap);
        CHECK(decoded.controlWindow == frames[frameIndex].control);
        CHECK(decoded.data == frames[frameIndex].data);
    }
}

TEST_CASE("The reference raster codec is safe under concurrent use",
    "[pbmodulation][integration][threads]")
{
    constexpr std::size_t kThreadCount = 8;
    std::atomic<std::size_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (std::size_t threadIndex = 0; threadIndex < kThreadCount;
        threadIndex++)
    {
        workers.emplace_back(
            [&failures, threadIndex]()
            {
                // Each worker uses its own deterministic payload; the
                // codec is a stateless pure function with no shared
                // mutable state.
                GoldenFramePayload payload =
                    MakeZeroGoldenPayload();
                SplitMix64 rng(0xC0DE0000u + threadIndex * 0x9E37u);
                for (auto& value : payload.bootstrap)
                {
                    value = Byte(rng.Next() & 0xFFu);
                }
                for (auto& value : payload.control)
                {
                    value = Byte(rng.Next() & 0xFFu);
                }
                for (auto& value : payload.data)
                {
                    value = Byte(rng.Next() & 0xFFu);
                }
                try
                {
                    for (int round = 0; round < 4; round++)
                    {
                        const std::vector<std::byte> frame =
                            EncodeFrameOrDie(
                                payload.MakeInput());
                        const DecodedReferenceFrame decoded =
                            DecodeFrameOrDie(frame);
                        if (decoded.bootstrapRecord !=
                            payload.bootstrap ||
                            decoded.controlWindow !=
                            payload.control ||
                            decoded.data != payload.data)
                        {
                            failures++;
                        }
                    }
                }
                catch (...)
                {
                    failures++;
                }
            });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    CHECK(failures.load() == 0);
}
