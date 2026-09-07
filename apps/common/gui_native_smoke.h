#pragma once

// Explicit bounded native diagnostic only. The no-argument product GUI never
// enters this path; no input event, foreground activation or peer IPC is used.
#include "product_gui_helpers.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>
#include <string_view>

namespace pbgui
{

struct NativeSmokeOptions
{
    pbapp::MonitorSafetySelection safety;
    QString inputPath;
    QString evidenceDirectory;
    int durationSeconds = 0;
};

[[nodiscard]] inline bool IsNativeSmoke(const int argumentCount, wchar_t* arguments[])
{
    return argumentCount > 1 && std::wstring_view(arguments[1]) == L"--gui-native-smoke";
}

[[nodiscard]] inline bool ParseNativeSmoke(const int argumentCount, wchar_t* arguments[], NativeSmokeOptions& options)
{
    if (argumentCount != 7)
    {
        std::cerr << "Usage: --gui-native-smoke EXPERIMENT_MONITOR PROTECTED_MONITOR SOURCE_OR_OUTPUT NEW_EVIDENCE_DIR SECONDS(5..180)\n";
        return false;
    }
    const std::wstring_view secondsText(arguments[6]);
    if (secondsText.empty() || secondsText.size() > 3 || secondsText.find_first_not_of(L"0123456789") != std::wstring_view::npos)
    {
        return false;
    }
    bool converted = false;
    options.durationSeconds = QString::fromWCharArray(arguments[6]).toInt(&converted);
    if (!converted || options.durationSeconds < 5 || options.durationSeconds > 180 ||
        !pbapp::ResolveMonitorSafetySelection(arguments[3], arguments[2], options.safety))
    {
        return false;
    }
    // This fixture deliberately accepts only a distinct monitor wholly to the
    // right. Never guess the experiment display from primary/array ordering.
    const auto& experiment = options.safety.experimentMonitor;
    if (experiment.physicalRect.left < options.safety.protectedMonitor.physicalRect.right ||
        !pbapp::ValidateMonitorSafetyTarget(options.safety, experiment.physicalRect, experiment.monitor))
    {
        return false;
    }
    options.inputPath = QString::fromWCharArray(arguments[4]);
    options.evidenceDirectory = QFileInfo(QString::fromWCharArray(arguments[5])).absoluteFilePath();
    const QFileInfo evidence(options.evidenceDirectory);
    if (!evidence.isAbsolute() || evidence.exists() || !QFileInfo(evidence.absolutePath()).isDir() ||
        !QDir(evidence.absolutePath()).mkdir(evidence.fileName()))
    {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool NativeWindowIsContained(QWidget& window, const NativeSmokeOptions& options, QJsonObject* failure = nullptr)
{
    DWORD foregroundProcess = 0;
    static_cast<void>(GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess));
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    RECT bounds{};
    const bool ownForeground = foregroundProcess == GetCurrentProcessId();
    const auto topology = pbapp::RevalidateMonitorSafetySelection(options.safety);
    const bool hasBounds = GetWindowRect(handle, &bounds) != FALSE;
    const auto containment = pbapp::ValidateMonitorSafetyTarget(options.safety, bounds, MonitorFromWindow(handle, MONITOR_DEFAULTTONULL));
    const bool safe = !ownForeground && topology && hasBounds && containment;
    if (!safe && failure != nullptr)
    {
        *failure = QJsonObject{{"ownProcessBecameForeground", ownForeground},
            {"topologyStatus", pbapp::GetMonitorSafetyErrorName(topology.code)},
            {"catalogStatus", static_cast<int>(topology.catalogStatus.code)}, {"catalogNativeError", topology.catalogStatus.nativeError},
            {"hasWindowBounds", hasBounds}, {"containmentStatus", pbapp::GetMonitorSafetyErrorName(containment.code)},
            {"left", static_cast<int>(bounds.left)}, {"top", static_cast<int>(bounds.top)},
            {"right", static_cast<int>(bounds.right)}, {"bottom", static_cast<int>(bounds.bottom)}};
    }
    return safe;
}

[[nodiscard]] inline bool ShowNativeSmoke(QWidget& window, const NativeSmokeOptions& options)
{
    if (QApplication::platformName() != QStringLiteral("windows") ||
        !PlaceWithoutActivating(window, options.safety.experimentMonitor.deviceName))
    {
        return false;
    }
    window.show();
    QCoreApplication::processEvents();
    return window.isVisible() && NativeWindowIsContained(window, options);
}

[[nodiscard]] inline bool WriteNativeSmokeFile(const NativeSmokeOptions& options, const QString& name, const QByteArray& bytes)
{
    QFile output(QDir(options.evidenceDirectory).filePath(name));
    return output.open(QIODevice::WriteOnly | QIODevice::NewOnly) && output.write(bytes) == bytes.size() && output.flush();
}

[[nodiscard]] inline bool WriteNativeSmokeResult(const NativeSmokeOptions& options, const QString& role,
    const bool passed, const bool safetyHeld, const QJsonObject& checks, const std::string& report)
{
    const auto& rectangle = options.safety.experimentMonitor.physicalRect;
    const QJsonObject result{{"schema", "PixelBridge.GuiNativeSmoke.1"}, {"role", role}, {"passed", passed},
        {"safetyHeld", safetyHeld}, {"gitCommit", PB_GIT_COMMIT}, {"automaticInputEvents", false},
        {"normalGuiTimedStop", false}, {"testDeadlineSeconds", options.durationSeconds},
        {"protectedMonitor", QString::fromStdWString(options.safety.protectedMonitor.deviceName)},
        {"experimentMonitor", QString::fromStdWString(options.safety.experimentMonitor.deviceName)},
        {"physicalRect", QJsonObject{{"left", static_cast<int>(rectangle.left)}, {"top", static_cast<int>(rectangle.top)},
            {"right", static_cast<int>(rectangle.right)}, {"bottom", static_cast<int>(rectangle.bottom)}}},
        {"checks", checks}, {"utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    return WriteNativeSmokeFile(options, QStringLiteral("run-report.json"), QByteArray::fromStdString(report)) &&
        WriteNativeSmokeFile(options, QStringLiteral("native-smoke.json"), QJsonDocument(result).toJson());
}

} // namespace pbgui
