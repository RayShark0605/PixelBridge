#pragma once

// Qt presentation only; all counters and estimates come from the runtime.
#include "application_model.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pbgui
{

struct DecoderProgressText
{
    QString percent = QStringLiteral("—");
    QString received = QStringLiteral("—");
    QString elapsed = QStringLiteral("—");
    QString speed = QStringLiteral("0 KB/s");
    QString remaining = QStringLiteral("估算中");
};

[[nodiscard]] inline bool IsVerifiedCompletion(const pbapp::DecoderSnapshot& snapshot)
{
    return snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified &&
        snapshot.finalPublishSucceeded && snapshot.finalReopenVerified.value_or(false) && !snapshot.outputPath.empty();
}

// Windows binary units (1 KB = 1024 B); one fractional digit above bytes.
[[nodiscard]] inline QString FormatByteSize(const std::uint64_t bytes)
{
    const double value = static_cast<double>(bytes);
    if (bytes < 1024ULL)
    {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024ULL * 1024ULL)
    {
        return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL)
    {
        return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 1);
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL * 1024ULL)
    {
        return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }
    return QStringLiteral("%1 TB").arg(value / (1024.0 * 1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

[[nodiscard]] inline QString FormatDurationClock(const std::uint64_t seconds)
{
    const std::uint64_t days = seconds / 86400;
    const QString clock = QStringLiteral("%1:%2:%3").arg((seconds / 3600) % 24, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return days == 0 ? clock : QStringLiteral("%1 天 %2").arg(days).arg(clock);
}

[[nodiscard]] inline QString FormatSpeed(const double bytesPerSecond)
{
    return QStringLiteral("%1 KB/s").arg(bytesPerSecond / 1024.0, 0, 'f', 1);
}

// nowUnixMilliseconds lets the elapsed clock tick between runtime snapshots;
// pass 0 (or a past timestamp) to suppress the elapsed display.
[[nodiscard]] inline DecoderProgressText FormatDecoderProgress(const pbapp::DecoderSnapshot& snapshot,
    const std::int64_t nowUnixMilliseconds = 0)
{
    DecoderProgressText result;
    const bool complete = IsVerifiedCompletion(snapshot);
    const bool active = pbapp::IsDecoderStateActive(snapshot.state) && snapshot.state != pbapp::DecoderState::Stopping;
    const bool stalled = snapshot.captureStallActive || snapshot.visualStallActive;

    // Received-size basis: the continuous estimate when the runtime provides
    // it, otherwise the verified bytes (e.g. a resumed session before the
    // first admission). Never trusts an estimate beyond the declared file.
    std::uint64_t received = snapshot.estimatedReceivedRawBytes;
    if (received < snapshot.verifiedRawBytes)
    {
        received = snapshot.verifiedRawBytes;
    }
    if (snapshot.originalFileBytes != 0 && received > snapshot.originalFileBytes)
    {
        received = snapshot.originalFileBytes;
    }

    const bool sizeKnown = snapshot.descriptorKnown && snapshot.originalFileBytes != 0;

    if (complete)
    {
        result.percent = QStringLiteral("100%");
        result.received = FormatByteSize(snapshot.originalFileBytes);
        result.remaining = QStringLiteral("00:00:00");
    }
    else if (sizeKnown)
    {
        // Verified Segments are not yet an accepted file. Never round to 100%
        // until whole digest, safe publish and final reopen have all succeeded.
        result.percent = QStringLiteral("%1%").arg(
            (std::min)(99.9, static_cast<double>(received) * 100.0 / static_cast<double>(snapshot.originalFileBytes)),
            0, 'f', 1);
        result.received = FormatByteSize(received);
        if (received > snapshot.verifiedRawBytes && snapshot.verifiedRawBytes != 0)
        {
            result.received += QStringLiteral("（已验证 %1）").arg(FormatByteSize(snapshot.verifiedRawBytes));
        }
        else if (received > snapshot.verifiedRawBytes)
        {
            result.received += QStringLiteral("（接收中）");
        }
    }

    // Elapsed wall time of this run; ticks from the GUI clock between
    // snapshots and freezes on the terminal timestamp afterwards. Without a
    // usable clock (or terminal timestamp) the field stays unavailable.
    if (snapshot.runStartedUnixMilliseconds != 0)
    {
        std::uint64_t elapsedMilliseconds = 0;
        bool elapsedKnown = false;
        if (complete && snapshot.runEndedUnixMilliseconds && *snapshot.runEndedUnixMilliseconds >= snapshot.runStartedUnixMilliseconds)
        {
            elapsedMilliseconds = *snapshot.runEndedUnixMilliseconds - snapshot.runStartedUnixMilliseconds;
            elapsedKnown = true;
        }
        else if (nowUnixMilliseconds > static_cast<std::int64_t>(snapshot.runStartedUnixMilliseconds))
        {
            elapsedMilliseconds = static_cast<std::uint64_t>(nowUnixMilliseconds) - snapshot.runStartedUnixMilliseconds;
            elapsedKnown = true;
        }
        if (elapsedKnown)
        {
            result.elapsed = FormatDurationClock(elapsedMilliseconds / 1000);
        }
    }
    else if (active)
    {
        result.elapsed = QStringLiteral("00:00:00");
    }

    if (complete)
    {
        result.speed = snapshot.averageVerifiedRawGoodputBytesPerSecond > 0 ?
            FormatSpeed(snapshot.averageVerifiedRawGoodputBytesPerSecond) : QStringLiteral("0 KB/s");
        return result;
    }

    if (!active)
    {
        result.speed = snapshot.averageVerifiedRawGoodputBytesPerSecond > 0 ?
            FormatSpeed(snapshot.averageVerifiedRawGoodputBytesPerSecond) : QStringLiteral("0 KB/s");
        result.remaining = QStringLiteral("—");
        return result;
    }

    if (stalled)
    {
        result.speed = QStringLiteral("0 KB/s");
        result.remaining = QStringLiteral("画面停滞");
        return result;
    }

    // Average speed over the elapsed window from the received estimate; the
    // first second stays quiet so the startup burst cannot show as ∞.
    std::uint64_t elapsedMilliseconds = 0;
    if (nowUnixMilliseconds > static_cast<std::int64_t>(snapshot.runStartedUnixMilliseconds) &&
        snapshot.runStartedUnixMilliseconds != 0)
    {
        elapsedMilliseconds = static_cast<std::uint64_t>(nowUnixMilliseconds) - snapshot.runStartedUnixMilliseconds;
    }
    double averageBytesPerSecond = 0.0;
    if (elapsedMilliseconds >= 1000 && received != 0)
    {
        averageBytesPerSecond = static_cast<double>(received) * 1000.0 / static_cast<double>(elapsedMilliseconds);
        if (!(averageBytesPerSecond >= 0.0) || !std::isfinite(averageBytesPerSecond))
        {
            averageBytesPerSecond = 0.0;
        }
    }
    result.speed = averageBytesPerSecond > 0.0 ? FormatSpeed(averageBytesPerSecond) : QStringLiteral("0 KB/s");
    if (sizeKnown && averageBytesPerSecond > 0.0 && received < snapshot.originalFileBytes)
    {
        const double remainingSeconds = static_cast<double>(snapshot.originalFileBytes - received) / averageBytesPerSecond;
        if (std::isfinite(remainingSeconds) && remainingSeconds >= 0.0 && remainingSeconds < 1.0e12)
        {
            result.remaining = FormatDurationClock(static_cast<std::uint64_t>(std::ceil(remainingSeconds)));
        }
    }
    return result;
}

} // namespace pbgui
