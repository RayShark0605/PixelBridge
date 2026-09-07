#pragma once

#include "pbmodulation/unified_visual_profile.h"

#include <ostream>

namespace pbapp
{

// Read-only build metadata for package sealing, not a new wire descriptor or
// an input mode. Values come from the same compiled manifest as the renderer.
[[nodiscard]] inline bool WriteUnifiedProfileIdentity(std::ostream& stream)
{
    if (!pbmodulation::ValidateUnifiedVisualProfile())
    {
        return false;
    }
    const auto& profile = pbmodulation::kUnifiedVisualProfile;
    stream << "{\"schema\":\"PixelBridge.UnifiedProfile.1\",\"name\":\"" << profile.productProfile.name
        << "\",\"visualProfileId\":" << profile.productProfile.visualProfileId
        << ",\"layoutVersion\":" << static_cast<unsigned int>(profile.productProfile.visualLayoutVersion)
        << ",\"canvasWidth\":" << profile.canvasWidth << ",\"canvasHeight\":" << profile.canvasHeight
        << ",\"pixelFormat\":" << static_cast<unsigned int>(profile.pixelFormat)
        << ",\"alphaCodeValue\":" << static_cast<unsigned int>(profile.alphaCodeValue)
        << ",\"tileWidth\":" << profile.tileWidth << ",\"tileHeight\":" << profile.tileHeight
        << ",\"dataTileCount\":" << profile.dataTileCount << ",\"innerFecProfileId\":" << profile.innerFecProfileId
        << ",\"innerCodewordBits\":" << profile.innerCodewordBits << ",\"innerInformationBits\":" << profile.innerInformationBits
        << ",\"transportOverheadBytes\":" << profile.transportOverheadBytes
        << ",\"dataGrid\":[" << profile.dataGridBounds.x << ',' << profile.dataGridBounds.y << ','
        << profile.dataGridBounds.width << ',' << profile.dataGridBounds.height << "],\"regions\":[";
    for (std::size_t index = 0; index < profile.regions.size(); index++)
    {
        const auto& region = profile.regions[index];
        stream << (index == 0 ? "" : ",") << '[' << static_cast<unsigned int>(region.kind) << ','
            << static_cast<unsigned int>(region.pilotContent) << ',' << region.bounds.x << ',' << region.bounds.y << ','
            << region.bounds.width << ',' << region.bounds.height << ']';
    }
    stream << "],\"carriers\":[";
    for (std::size_t index = 0; index < profile.carriers.size(); index++)
    {
        const auto& carrier = profile.carriers[index];
        stream << (index == 0 ? "" : ",") << '[' << static_cast<unsigned int>(carrier.carrier) << ',' << carrier.bitsPerTile << ']';
    }
    stream << "],\"lanes\":[";
    for (std::size_t index = 0; index < profile.lanes.size(); index++)
    {
        const auto& lane = profile.lanes[index];
        stream << (index == 0 ? "" : ",") << '[' << static_cast<unsigned int>(lane.lane) << ','
            << static_cast<unsigned int>(lane.carrier) << ',' << lane.firstCodewordSlot << ',' << lane.codewordCount << ']';
    }
    const auto& presentation = profile.presentation;
    stream << "],\"mixedSlots\":{\"controlLane\":" << static_cast<unsigned int>(profile.mixedSlots.controlRegion.lane)
        << ",\"firstControlSlot\":" << profile.mixedSlots.controlRegion.firstCodewordSlot
        << ",\"controlSlotCount\":" << profile.mixedSlots.controlRegion.codewordSlotCount
        << ",\"minimumTransportSlots\":" << profile.mixedSlots.minimumTransportSlots
        << "},\"presentation\":{\"minimumScaleNumerator\":" << presentation.minimumScaleNumerator
        << ",\"minimumScaleDenominator\":" << presentation.minimumScaleDenominator
        << ",\"maximumScaleNumerator\":" << presentation.maximumScaleNumerator
        << ",\"maximumScaleDenominator\":" << presentation.maximumScaleDenominator
        << ",\"minimumFps\":" << presentation.minimumLogicalFramesPerSecond
        << ",\"defaultFps\":" << presentation.defaultLogicalFramesPerSecond
        << ",\"maximumFps\":" << presentation.maximumLogicalFramesPerSecond
        << ",\"filter\":" << static_cast<unsigned int>(presentation.filter)
        << ",\"fit\":" << static_cast<unsigned int>(presentation.fit)
        << ",\"placement\":" << static_cast<unsigned int>(presentation.placement)
        << ",\"undersizeBehavior\":" << static_cast<unsigned int>(presentation.undersizeBehavior) << "}}" << std::endl;
    return stream.good();
}

} // namespace pbapp
