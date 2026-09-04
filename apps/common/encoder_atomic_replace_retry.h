#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstdint>

namespace pbapp::detail
{

// Internal scheduling policy, not a persistence format or a public API. Never
// retry the write/flush or allocate a new generation: retry only the same rename.
inline constexpr std::uint32_t encoderReplaceMaximumAttempts = 11;
inline constexpr std::uint64_t encoderReplaceBudgetMilliseconds = 250;
inline constexpr DWORD encoderReplaceDelayMilliseconds = 25;

struct EncoderReplaceResult
{
    DWORD error = ERROR_SUCCESS;
    DWORD firstError = ERROR_SUCCESS;
    std::uint32_t attempts = 0;
    std::uint64_t elapsedMilliseconds = 0;
};

// Callable seams keep deadline/error tests deterministic without production
// hooks. A stalled clock is also bounded by the attempt limit. The budget does
// not bound time spent inside Windows or scheduler oversleep.
template <typename Replace, typename Clock, typename Wait>
[[nodiscard]] EncoderReplaceResult RetryEncoderStateReplace(Replace&& replace, Clock&& clock, Wait&& wait) noexcept
{
    EncoderReplaceResult result;
    const std::uint64_t started = clock();
    while (result.attempts < encoderReplaceMaximumAttempts)
    {
        if (result.attempts != 0 && result.elapsedMilliseconds >= encoderReplaceBudgetMilliseconds)
        {
            break;
        }
        result.error = replace();
        result.attempts++;
        if (result.attempts == 1)
        {
            result.firstError = result.error;
        }
        result.elapsedMilliseconds = clock() - started;
        if (result.error == ERROR_SUCCESS ||
            (result.error != ERROR_ACCESS_DENIED && result.error != ERROR_SHARING_VIOLATION) ||
            result.attempts == encoderReplaceMaximumAttempts ||
            result.elapsedMilliseconds >= encoderReplaceBudgetMilliseconds)
        {
            break;
        }
        wait(static_cast<DWORD>((std::min)(static_cast<std::uint64_t>(encoderReplaceDelayMilliseconds),
            encoderReplaceBudgetMilliseconds - result.elapsedMilliseconds)));
        result.elapsedMilliseconds = clock() - started;
    }
    return result;
}

} // namespace pbapp::detail
