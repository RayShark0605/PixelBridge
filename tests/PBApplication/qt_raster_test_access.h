#pragma once

#include "qt_raster_presentation.h"
#include <QImage>

namespace pbencoder
{

// Only the privately compiled test clone defines these entry points. Product
// configuration has no hidden-window, synthetic-monitor or pixel-readback flag.
class QtRasterPresentationTestAccess
{
public:
    static void UseHiddenWindow(QtRasterPresentationHost& host);
    static void UseNativeDpiScaleSimulation(QtRasterPresentationHost& host);
    static void SetEnvironment(QtRasterPresentationHost& host, const pbrenderd3d::WindowEnvironment& environment);
    static bool CreationQueued(const QtRasterPresentationHost& host);
    static std::size_t PendingWakeups(const QtRasterPresentationHost& host);
    static std::size_t OwnedFrameBytes(const QtRasterPresentationHost& host);
    static std::uintptr_t WindowHandle(const QtRasterPresentationHost& host);
    static QString DescribeGeometry(const QtRasterPresentationHost& host);
    static QImage Paint(QtRasterPresentationHost& host);
    static QImage PaintRegion(QtRasterPresentationHost& host, const QRect& region);
    static void CloseWindow(QtRasterPresentationHost& host);
};

}
