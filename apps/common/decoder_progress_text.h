#pragma once

// Qt presentation only; all counters and estimates come from the runtime.
#include "application_model.h"

#include <QString>

#include <algorithm>
#include <cmath>

namespace pbgui
{

struct DecoderProgressText
{
    QString percent = QStringLiteral("—");
    QString speed = QStringLiteral("0 KB/s");
    QString remaining = QStringLiteral("估算中");
};

[[nodiscard]] inline bool IsVerifiedCompletion(const pbapp::DecoderSnapshot& snapshot)
{
    return snapshot.state == pbapp::DecoderState::Completed && snapshot.wholeFileDigestVerified &&
        snapshot.finalPublishSucceeded && snapshot.finalReopenVerified.value_or(false) && !snapshot.outputPath.empty();
}

[[nodiscard]] inline DecoderProgressText FormatDecoderProgress(const pbapp::DecoderSnapshot& snapshot)
{
    DecoderProgressText result;
    const bool complete = IsVerifiedCompletion(snapshot);
    if (complete)
    {
        result.percent = QStringLiteral("100%");
        result.remaining = QStringLiteral("00:00:00");
    }
    else if (snapshot.descriptorKnown && snapshot.recoveryProgress && std::isfinite(*snapshot.recoveryProgress) &&
        *snapshot.recoveryProgress >= 0 && *snapshot.recoveryProgress <= 1 && snapshot.verifiedRawBytes <= snapshot.originalFileBytes)
    {
        // Verified Segments are not yet an accepted file. Never round to 100%
        // until whole digest, safe publish and final reopen have all succeeded.
        result.percent = QStringLiteral("%1%").arg((std::min)(99.9, *snapshot.recoveryProgress * 100.0), 0, 'f', 1);
    }
    const bool active = pbapp::IsDecoderStateActive(snapshot.state) && snapshot.state != pbapp::DecoderState::Stopping;
    const bool stalled = snapshot.captureStallActive || snapshot.visualStallActive;
    const double speed = complete ? snapshot.averageVerifiedRawGoodputBytesPerSecond : snapshot.smoothedVerifiedRawGoodputBytesPerSecond;
    if ((complete || (active && !stalled)) && snapshot.verifiedRawBytes > 0 && std::isfinite(speed) && speed > 0)
    {
        result.speed = QStringLiteral("%1 KB/s").arg(speed / 1024.0, 0, 'f', 1);
        if (!complete && snapshot.etaMilliseconds)
        {
            const std::uint64_t milliseconds = *snapshot.etaMilliseconds;
            const std::uint64_t seconds = milliseconds / 1000 + (milliseconds % 1000 != 0 ? 1U : 0U);
            const std::uint64_t days = seconds / 86400;
            const QString clock = QStringLiteral("%1:%2:%3").arg((seconds / 3600) % 24, 2, 10, QLatin1Char('0'))
                .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
            result.remaining = days == 0 ? clock : QStringLiteral("%1 天 %2").arg(days).arg(clock);
        }
    }
    if (!active && !complete)
    {
        result.remaining = QStringLiteral("—");
    }
    return result;
}

} // namespace pbgui
