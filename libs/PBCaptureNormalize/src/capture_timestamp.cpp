#include "pbcapturenormalize/capture_types.h"
#include "pbprotocol/checked_integer.h"

#include <array>
#include <limits>

namespace pbcapturenormalize
{
CaptureFrameAgeResult ClassifyFrameAge(const std::int64_t now100ns, const std::int64_t timestamp100ns, const std::uint32_t maximumFrameAgeMilliseconds) noexcept
{
    if (maximumFrameAgeMilliseconds == 0)
    {
        return {CaptureFrameAgeDisposition::Current, 0};
    }
    if (now100ns < 0 || timestamp100ns < 0 || timestamp100ns > now100ns)
    {
        return {};
    }
    const auto age = static_cast<std::uint64_t>(now100ns - timestamp100ns);
    const auto limit100ns = static_cast<std::uint64_t>(maximumFrameAgeMilliseconds) * 10000;
    return {age > limit100ns ? CaptureFrameAgeDisposition::Expired : CaptureFrameAgeDisposition::Current, age};
}

std::int64_t ResolveEffectiveCaptureTime100ns(const std::int64_t claimed100ns, const std::int64_t arrivalQpc100ns) noexcept
{
    const std::array<std::int64_t, 2> candidates = {claimed100ns, arrivalQpc100ns};
    std::int64_t effective = -1;
    for (const auto candidate : candidates)
    {
        if (candidate >= 0 && (effective < 0 || candidate < effective))
        {
            effective = candidate;
        }
    }
    return effective;
}

bool ConvertQpcTo100ns(const std::int64_t ticks, const std::int64_t frequency, std::int64_t& output) noexcept
{
    if (ticks < 0 || frequency <= 0)
    {
        return false;
    }
    constexpr std::uint64_t unitsPerSecond = 10000000;
    const auto unsignedTicks = static_cast<std::uint64_t>(ticks);
    const auto unsignedFrequency = static_cast<std::uint64_t>(frequency);
    const auto whole = pbprotocol::CheckedMultiplyUint64(unsignedTicks / unsignedFrequency, unitsPerSecond);
    if (!whole)
    {
        return false;
    }
    // Binary long division avoids overflowing remainder * 10^7. The divisor
    // is at most INT64_MAX, so twice a reduced remainder fits uint64_t.
    std::uint64_t remainder = 0;
    std::uint64_t fraction = 0;
    const std::uint64_t originalRemainder = unsignedTicks % unsignedFrequency;
    for (int bit = 23; bit >= 0; bit--)
    {
        fraction *= 2;
        remainder *= 2;
        if (remainder >= unsignedFrequency)
        {
            remainder -= unsignedFrequency;
            fraction++;
        }
        if ((unitsPerSecond & (std::uint64_t{1} << bit)) != 0)
        {
            if (remainder >= unsignedFrequency - originalRemainder)
            {
                remainder -= unsignedFrequency - originalRemainder;
                fraction++;
            }
            else
            {
                remainder += originalRemainder;
            }
        }
    }
    const auto converted = pbprotocol::CheckedAddUint64(whole.Value(), fraction);
    if (!converted || converted.Value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return false;
    }
    output = static_cast<std::int64_t>(converted.Value());
    return true;
}
}
