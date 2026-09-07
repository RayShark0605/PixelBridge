#pragma once

// Application-only Qt helpers. This header is not part of PBApplication's
// public sources and must never be included by a core library.
#include "monitor_catalog.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QPixmap>
#include <QTabWidget>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

namespace pbgui
{

[[nodiscard]] inline bool ConfigureFont(const bool offscreen)
{
    if (offscreen)
    {
        // Qt's offscreen font database does not discover the Windows system
        // fonts. Load this host's CJK font explicitly; it is not packaged.
        const QString path = QDir(qEnvironmentVariable("WINDIR")).filePath(QStringLiteral("Fonts/msyh.ttc"));
        if (QFontDatabase::addApplicationFont(path) < 0)
        {
            return false;
        }
    }
    QApplication::setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    return true;
}

[[nodiscard]] inline QString FromUtf8(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

[[nodiscard]] inline QLabel* TextLabel(const QString& text = {})
{
    QLabel* const label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

[[nodiscard]] inline QString HumanBytes(const std::uint64_t bytes)
{
    // UI labels follow Windows: KB/MB/GB use powers of 1024, not 1000.
    const QStringList units{QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB")};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size())
    {
        value /= 1024.0;
        unit++;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit == 0 ? 0 : 2).arg(units[unit]);
}

[[nodiscard]] inline bool PrepareOffscreenPlatform()
{
    std::array<wchar_t, 32768> modulePath{};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
#ifdef QT_DEBUG
    constexpr const wchar_t* plugin = L"qoffscreend.dll";
#else
    constexpr const wchar_t* plugin = L"qoffscreen.dll";
#endif
    std::error_code error;
    if (length == 0 || length >= modulePath.size() || !std::filesystem::is_regular_file(
        std::filesystem::path(modulePath.data()).parent_path() / L"platforms" / plugin, error) || error)
    {
        return false;
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    return true;
}

[[nodiscard]] inline bool PlaceWithoutActivating(QWidget& window, const std::wstring_view deviceName)
{
    std::vector<pbapp::MonitorInfo> monitors;
    if (!pbapp::EnumerateMonitors(monitors))
    {
        return false;
    }
    const auto monitor = std::find_if(monitors.begin(), monitors.end(), [deviceName](const pbapp::MonitorInfo& candidate)
    {
        return candidate.deviceName == deviceName;
    });
    if (monitor == monitors.end())
    {
        return false;
    }
    window.setAttribute(Qt::WA_ShowWithoutActivating);
    const HWND handle = reinterpret_cast<HWND>(window.winId());
    RECT bounds{};
    const std::int64_t availableWidth = static_cast<std::int64_t>(monitor->workRect.right) - monitor->workRect.left;
    const std::int64_t availableHeight = static_cast<std::int64_t>(monitor->workRect.bottom) - monitor->workRect.top;
    if (!GetWindowRect(handle, &bounds) || availableWidth <= 48 || availableHeight <= 48)
    {
        return false;
    }
    const int width = static_cast<int>((std::min)(static_cast<std::int64_t>(bounds.right) - bounds.left, availableWidth - 48));
    const int height = static_cast<int>((std::min)(static_cast<std::int64_t>(bounds.bottom) - bounds.top, availableHeight - 48));
    if (!SetWindowPos(handle, nullptr, monitor->workRect.left + 24, monitor->workRect.top + 24, width, height, SWP_NOACTIVATE | SWP_NOZORDER))
    {
        return false;
    }
    return GetWindowRect(handle, &bounds) && pbapp::RectContains(monitor->workRect, bounds);
}

// Captures only this application's offscreen widgets, never desktop pixels.
// Optional evidence files are create-only and cannot replace user images.
[[nodiscard]] inline bool SaveTabPreviews(QWidget& window, QTabWidget& tabs, const QString& prefix)
{
    const QString directory = qEnvironmentVariable("PB_GUI_SMOKE_EVIDENCE_DIR");
    if (directory.isEmpty())
    {
        return true;
    }
    if (!QFileInfo(directory).isDir())
    {
        return false;
    }
    const int originalTab = tabs.currentIndex();
    window.ensurePolished();
    for (int index = 0; index < tabs.count(); index++)
    {
        const QString path = QDir(directory).filePath(prefix + QStringLiteral("-tab-%1.png").arg(index));
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        {
            tabs.setCurrentIndex(originalTab);
            return false;
        }
        tabs.setCurrentIndex(index);
        QCoreApplication::processEvents();
        if (!window.grab().save(&output, "PNG"))
        {
            tabs.setCurrentIndex(originalTab);
            return false;
        }
    }
    tabs.setCurrentIndex(originalTab);
    return true;
}

} // namespace pbgui
