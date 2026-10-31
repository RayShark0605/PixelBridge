#include "qt_raster_paint.h"
#include "pbprotocol/checked_integer.h"

#include <QImage>
#include <QPaintEngine>
#include <QPainter>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace pbencoder
{

pbrenderd3d::PresentationStatus PaintQtRasterFrame(QPainter& painter, const pbrenderd3d::DataWindowConfig& config,
    const pbrenderd3d::CanonicalBgraFrameView& frame, const std::uint32_t physicalWidth, const std::uint32_t physicalHeight,
    const double devicePixelRatio, QImage& physicalScratch) noexcept
{
    using namespace pbrenderd3d;
    static_assert(std::endian::native == std::endian::little);
    const auto validation = ValidateCanonicalBgraPixels(config.width, config.height, config.maximumFrameBytes, frame);
    if (!validation)
    {
        return validation;
    }
    if (config.presentationModel != PresentationModel::QtRaster || physicalWidth == 0 || physicalHeight == 0 ||
        physicalWidth > 16384 || physicalHeight > 16384 || !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0)
    {
        return PresentationStatus::Failure(PresentationErrorCode::InvalidConfiguration, PresentationStage::Present);
    }
    const auto pixelCount = pbprotocol::CheckedMultiplyUint64(physicalWidth, physicalHeight);
    const auto outputBytes = pixelCount ? pbprotocol::CheckedMultiplyUint64(pixelCount.Value(), 4) : pixelCount;
    if (!outputBytes || outputBytes.Value() > config.maximumFrameBytes || !pbprotocol::CheckedUint64ToSize(outputBytes.Value()))
    {
        return PresentationStatus::Failure(PresentationErrorCode::ResourceLimit, PresentationStage::Present);
    }
    if (!painter.isActive() || painter.device() == nullptr || painter.paintEngine() == nullptr ||
        painter.paintEngine()->type() != QPaintEngine::Raster ||
        std::abs(painter.device()->devicePixelRatioF() - devicePixelRatio) > 0.000001)
    {
        return PresentationStatus::Failure(PresentationErrorCode::ContractViolation, PresentationStage::Present);
    }
    bool painterSaved = false;
    try
    {
        if (physicalScratch.width() != static_cast<int>(physicalWidth) || physicalScratch.height() != static_cast<int>(physicalHeight) ||
            physicalScratch.format() != QImage::Format_RGB32)
        {
            // Drop the old canvas before allocating the replacement; a resize
            // never transiently owns two full physical output canvases.
            physicalScratch = {};
            physicalScratch = QImage(static_cast<int>(physicalWidth), static_cast<int>(physicalHeight), QImage::Format_RGB32);
        }
        if (physicalScratch.isNull())
        {
            return PresentationStatus::Failure(PresentationErrorCode::OutOfMemory, PresentationStage::Present);
        }
        physicalScratch.setDevicePixelRatio(devicePixelRatio);
        const auto viewport = ResolvePresentationViewport(config, physicalWidth, physicalHeight);
        if (physicalWidth == frame.width && physicalHeight == frame.height)
        {
            const auto rowBytes = static_cast<std::size_t>(frame.width) * 4;
            for (std::uint32_t y = 0; y < physicalHeight; y++)
            {
                auto* const output = physicalScratch.scanLine(static_cast<int>(y));
                std::memcpy(output, frame.pixels.data() + static_cast<std::size_t>(y) * frame.rowPitch, rowBytes);
                for (std::uint32_t x = 0; x < physicalWidth; x++)
                {
                    output[static_cast<std::size_t>(x) * 4 + 3] = 255;
                }
            }
        }
        else
        {
            physicalScratch.fill(0xff808080U);
            if (CanPresentData(viewport.disposition))
            {
                for (std::uint32_t y = 0; y < physicalHeight; y++)
                {
                    const double sampleY = y + 0.5;
                    if (sampleY < viewport.originY || sampleY >= viewport.originY + viewport.height)
                    {
                        continue;
                    }
                    const auto sourceY = (std::min)(frame.height - 1, static_cast<std::uint32_t>(
                        std::floor((sampleY - viewport.originY) * frame.height / viewport.height)));
                    auto* const output = physicalScratch.scanLine(static_cast<int>(y));
                    const auto* const input = frame.pixels.data() + static_cast<std::size_t>(sourceY) * frame.rowPitch;
                    for (std::uint32_t x = 0; x < physicalWidth; x++)
                    {
                        const double sampleX = x + 0.5;
                        if (sampleX < viewport.originX || sampleX >= viewport.originX + viewport.width)
                        {
                            continue;
                        }
                        const auto sourceX = (std::min)(frame.width - 1, static_cast<std::uint32_t>(
                            std::floor((sampleX - viewport.originX) * frame.width / viewport.width)));
                        const auto offset = static_cast<std::size_t>(x) * 4;
                        std::memcpy(output + offset, input + static_cast<std::size_t>(sourceX) * 4, 3);
                        output[offset + 3] = 255;
                    }
                }
            }
        }
        // All resampling is already defined in physical pixels. Matching DPR
        // metadata makes this last draw a 1:1 backing-store copy, not a second
        // Qt scaling pass or alpha composition with the desktop.
        painter.save();
        painterSaved = true;
        painter.resetTransform();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.setOpacity(1.0);
        painter.drawImage(QPointF(0, 0), physicalScratch);
        painter.restore();
        return {};
    }
    catch (const std::bad_alloc&)
    {
        if (painterSaved)
        {
            painter.restore();
        }
        return PresentationStatus::Failure(PresentationErrorCode::OutOfMemory, PresentationStage::Present);
    }
    catch (...)
    {
        if (painterSaved)
        {
            painter.restore();
        }
        return PresentationStatus::Failure(PresentationErrorCode::InternalError, PresentationStage::Present);
    }
}

}
