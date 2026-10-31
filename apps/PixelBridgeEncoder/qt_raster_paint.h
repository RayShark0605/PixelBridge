#pragma once

#include "pbrenderd3d/data_window.h"

class QImage;
class QPainter;

namespace pbencoder
{

[[nodiscard]] pbrenderd3d::PresentationStatus PaintQtRasterFrame(QPainter& painter, const pbrenderd3d::DataWindowConfig& config,
    const pbrenderd3d::CanonicalBgraFrameView& frame, std::uint32_t physicalWidth, std::uint32_t physicalHeight,
    double devicePixelRatio, QImage& physicalScratch) noexcept;

}
