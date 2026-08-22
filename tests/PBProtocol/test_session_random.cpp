#include "pbprotocol/session_random.h"
#include "session_random_internal.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace {

class RecordingBCryptRandomApi final :
    public pbprotocol::detail::BCryptRandomApi
{
public:
    explicit RecordingBCryptRandomApi(
        const NTSTATUS returnStatus,
        const ULONG bytesToWrite = static_cast<ULONG>(
            pbprotocol::kSessionIdBytes)) noexcept
        : returnStatus_(returnStatus),
          bytesToWrite_(bytesToWrite)
    {
    }

    [[nodiscard]] NTSTATUS GenerateRandom(
        const BCRYPT_ALG_HANDLE algorithmHandle,
        PUCHAR const buffer,
        const ULONG bufferByteCount,
        const ULONG flags) noexcept override
    {
        callCount_++;
        algorithmHandle_ = algorithmHandle;
        bufferWasNull_ = buffer == nullptr;
        bufferByteCount_ = bufferByteCount;
        flags_ = flags;

        const ULONG writeByteCount = std::min(
            bytesToWrite_,
            bufferByteCount);
        for (ULONG byteIndex = 0;
             buffer != nullptr && byteIndex < writeByteCount;
             byteIndex++)
        {
            buffer[byteIndex] = static_cast<UCHAR>(byteIndex + 1U);
        }
        return returnStatus_;
    }

    [[nodiscard]] std::size_t CallCount() const noexcept
    {
        return callCount_;
    }

    [[nodiscard]] BCRYPT_ALG_HANDLE AlgorithmHandle() const noexcept
    {
        return algorithmHandle_;
    }

    [[nodiscard]] bool BufferWasNull() const noexcept
    {
        return bufferWasNull_;
    }

    [[nodiscard]] ULONG BufferByteCount() const noexcept
    {
        return bufferByteCount_;
    }

    [[nodiscard]] ULONG Flags() const noexcept
    {
        return flags_;
    }

private:
    NTSTATUS returnStatus_ = 0;
    ULONG bytesToWrite_ = 0;
    std::size_t callCount_ = 0;
    BCRYPT_ALG_HANDLE algorithmHandle_ = nullptr;
    bool bufferWasNull_ = true;
    ULONG bufferByteCount_ = 0;
    ULONG flags_ = 0;
};

void RequireCanonicalBCryptCall(const RecordingBCryptRandomApi& bcryptRandomApi)
{
    REQUIRE(bcryptRandomApi.CallCount() == 1);
    REQUIRE(bcryptRandomApi.AlgorithmHandle() == nullptr);
    REQUIRE_FALSE(bcryptRandomApi.BufferWasNull());
    REQUIRE(
        bcryptRandomApi.BufferByteCount() ==
        static_cast<ULONG>(pbprotocol::kSessionIdBytes));
    REQUIRE(bcryptRandomApi.Flags() == BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

} // namespace

TEST_CASE("GenerateRandomSessionId reaches the Windows CSPRNG",
          "[pbprotocol][session-random][smoke]")
{
    const auto result = pbprotocol::GenerateRandomSessionId();
    REQUIRE(result.HasValue());
}

TEST_CASE("BCrypt adapter maps all 16 bytes and exact call parameters",
          "[pbprotocol][session-random][adapter]")
{
    RecordingBCryptRandomApi bcryptRandomApi(0);
    const auto result = pbprotocol::detail::GenerateRandomSessionIdWithApi(
        bcryptRandomApi);

    REQUIRE(result.HasValue());
    RequireCanonicalBCryptCall(bcryptRandomApi);
    for (std::size_t byteIndex = 0;
         byteIndex < pbprotocol::kSessionIdBytes;
         byteIndex++)
    {
        REQUIRE(
            std::to_integer<std::uint8_t>(result.Value().bytes[byteIndex]) ==
            static_cast<std::uint8_t>(byteIndex + 1U));
    }
}

TEST_CASE("BCrypt status classification uses BCRYPT_SUCCESS",
          "[pbprotocol][session-random][adapter][status]")
{
    SECTION("non-negative informational status is successful")
    {
        constexpr NTSTATUS informationalSuccess = 1;
        RecordingBCryptRandomApi bcryptRandomApi(informationalSuccess);
        const auto result = pbprotocol::detail::GenerateRandomSessionIdWithApi(
            bcryptRandomApi);

        REQUIRE(result.HasValue());
        RequireCanonicalBCryptCall(bcryptRandomApi);
    }

    SECTION("negative status fails closed")
    {
        constexpr NTSTATUS failureStatus = -1;
        RecordingBCryptRandomApi bcryptRandomApi(failureStatus);
        const auto result = pbprotocol::detail::GenerateRandomSessionIdWithApi(
            bcryptRandomApi);

        REQUIRE_FALSE(result);
        RequireCanonicalBCryptCall(bcryptRandomApi);
        REQUIRE(
            result.Error() ==
            pbprotocol::ProtocolError{
                pbprotocol::ProtocolErrorCode::CsprngFailure,
                0});
    }

    SECTION("partial bytes followed by failure never escape")
    {
        constexpr NTSTATUS failureStatus = -1;
        constexpr ULONG partialByteCount = 7;
        RecordingBCryptRandomApi bcryptRandomApi(
            failureStatus,
            partialByteCount);
        const auto result = pbprotocol::detail::GenerateRandomSessionIdWithApi(
            bcryptRandomApi);

        REQUIRE_FALSE(result);
        RequireCanonicalBCryptCall(bcryptRandomApi);
        REQUIRE(
            result.Error().code ==
            pbprotocol::ProtocolErrorCode::CsprngFailure);
    }
}
