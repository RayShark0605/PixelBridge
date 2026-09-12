cbuffer FrameConstants : register(b0)
{
    uint Mode;
    uint TilePixels;
    uint TileCount;
    uint RowTiles;
    uint InterleavePhase;
    uint MetricCount;
    uint Reserved0;
    uint StateLumaMode;
    float OriginX;
    float OriginY;
    float ScaleX;
    float ScaleY;
    float SourceTexelPitchX;
    float SourceTexelPitchY;
    float MinimumEndpointSeparation;
    float MaximumPilotVariance;
    float MaximumPilotSpatialDeviation;
    float MinimumSymbolRms;
    float MaximumSymbolResidual;
    float MinimumSymbolMargin;
    float MinimumFreshnessMetric;
    uint SourceWidth;
    uint SourceHeight;
    uint FreshnessRegionCount;
    float BootstrapBlackLevel;
    float BootstrapWhiteLevel;
    float ReservedFloat0;
    float ReservedFloat1;
};

struct UnifiedTileBinding
{
    uint OriginX;
    uint OriginY;
    // Product binds four luma planes plus two chroma planes; gray v3 binds
    // six mask planes plus the seventh foreground-level plane in LumaBits[4].
    uint LumaBits[5];
    uint ChromaBits[2];
    uint Reserved[3];
};

Texture2D<float4> RoiTexture : register(t0);
StructuredBuffer<float4> Calibration : register(t1);
StructuredBuffer<uint4> UnusedTileMappings : register(t2);
StructuredBuffer<UnifiedTileBinding> UnifiedBindings : register(t3);
StructuredBuffer<uint> SymbolMasks : register(t4);
static const uint GrayMasksBySymbol[64] =
{
    0x1FFFFFF, 0x0AAAAAA, 0x0666666, 0x1333333, 0x01E1E1E, 0x14B4B4B, 0x1878787, 0x0D2D2D2,
    0x001FE01, 0x154AB54, 0x1986798, 0x0CD32CD, 0x1E01FE0, 0x0B54AB5, 0x0798679, 0x12CD32C,
    0x00001FF, 0x15554AA, 0x1999866, 0x0CCCD33, 0x1E1E01E, 0x0B4B54B, 0x0787987, 0x12D2CD2,
    0x1FE0001, 0x0AB5554, 0x0679998, 0x132CCCD, 0x01FE1E0, 0x14AB4B5, 0x1867879, 0x0D32D2C,
    0x0000000, 0x1555555, 0x1999999, 0x0CCCCCC, 0x1E1E1E1, 0x0B4B4B4, 0x0787878, 0x12D2D2D,
    0x1FE01FE, 0x0AB54AB, 0x0679867, 0x132CD32, 0x01FE01F, 0x14AB54A, 0x1867986, 0x0D32CD3,
    0x1FFFE00, 0x0AAAB55, 0x0666799, 0x13332CC, 0x01E1FE1, 0x14B4AB4, 0x1878678, 0x0D2D32D,
    0x001FFFE, 0x154AAAB, 0x1986667, 0x0CD3332, 0x1E01E1F, 0x0B54B4A, 0x0798786, 0x12CD2D3,
};
StructuredBuffer<uint> ExpectedFreshnessBits : register(t5);
StructuredBuffer<float4> StateModes : register(t6);
RWStructuredBuffer<float4> CalibrationOutput : register(u0);
RWStructuredBuffer<float> MetricOutput : register(u1);
RWStructuredBuffer<uint> TileSamplingFailures : register(u2);
RWStructuredBuffer<float4> FreshnessOutput : register(u3);
RWStructuredBuffer<float4> PhaseOutput : register(u4);
RWStructuredBuffer<uint> ForegroundHistogram : register(u5);
RWStructuredBuffer<float4> UnifiedStateModes : register(u6);

static const uint UnifiedMetricCount = 243000;
static const uint UnifiedGrayMetricCount = 291600;
static const uint UnifiedBaseLumaMetricCount = 145800;
static const uint UnifiedLumaMetricCount = 162000;
static const uint2 CalibrationOrigins[4] =
{
    uint2(736, 16), uint2(1696, 16), uint2(96, 1000), uint2(1056, 1000)
};
static const uint2 FreshnessOrigins[9] =
{
    uint2(96, 160), uint2(896, 160), uint2(1696, 160),
    uint2(96, 476), uint2(896, 476), uint2(1696, 476),
    uint2(96, 792), uint2(896, 792), uint2(1696, 792)
};
static const uint2 PhaseOrigins[2] = {uint2(896, 16), uint2(896, 1000)};

float Luma(float3 blueGreenRed)
{
    return dot(blueGreenRed, float3(0.0722, 0.7152, 0.2126));
}

float2 Opponent(float3 blueGreenRed)
{
    const float luma = Luma(blueGreenRed);
    return float2(blueGreenRed.x - luma, blueGreenRed.z - luma);
}

bool ReadSample(float logicalX, float logicalY, out float3 blueGreenRed)
{
    const float physicalX = OriginX + ScaleX * (logicalX + 0.5) - 0.5;
    const float physicalY = OriginY + ScaleY * (logicalY + 0.5) - 0.5;
    if (physicalX < 0.0 || physicalY < 0.0 || physicalX > (float)(SourceWidth - 1) ||
        physicalY > (float)(SourceHeight - 1))
    {
        blueGreenRed = 0.0;
        return false;
    }
    const bool horizontalDownscale = ScaleX < 1.0;
    const bool verticalDownscale = ScaleY < 1.0;
    const uint left = horizontalDownscale ? min((uint)floor(physicalX + 0.5), SourceWidth - 1) :
        (uint)floor(physicalX);
    const uint top = verticalDownscale ? min((uint)floor(physicalY + 0.5), SourceHeight - 1) :
        (uint)floor(physicalY);
    const float horizontal = horizontalDownscale ? 0.0 : physicalX - (float)left;
    const float vertical = verticalDownscale ? 0.0 : physicalY - (float)top;
    const uint right = horizontal == 0.0 ? left : left + 1;
    const uint bottom = vertical == 0.0 ? top : top + 1;
    // A B8G8R8A8 SRV exposes logical RGB components. Normalize them back to
    // the BGRA byte-order contract used by the CPU oracle before applying the
    // shared B,G,R luma and opponent coefficients.
    const float3 topLeft = RoiTexture.Load(int3(left, top, 0)).bgr * 255.0;
    const float3 topRight = right == left ? topLeft : RoiTexture.Load(int3(right, top, 0)).bgr * 255.0;
    const float3 bottomLeft = bottom == top ? topLeft : RoiTexture.Load(int3(left, bottom, 0)).bgr * 255.0;
    const float3 bottomRight = bottom == top ? topRight :
        (right == left ? bottomLeft : RoiTexture.Load(int3(right, bottom, 0)).bgr * 255.0);
    blueGreenRed = lerp(lerp(topLeft, topRight, horizontal), lerp(bottomLeft, bottomRight, horizontal), vertical);
    return true;
}

bool ReadSharpenedLuma(float logicalX, float logicalY, out float luma)
{
    float3 sample = 0.0;
    float3 left = 0.0;
    float3 right = 0.0;
    float3 top = 0.0;
    float3 bottom = 0.0;
    const bool valid = ReadSample(logicalX, logicalY, sample) &&
        ReadSample(logicalX - 1.0, logicalY, left) && ReadSample(logicalX + 1.0, logicalY, right) &&
        ReadSample(logicalX, logicalY - 1.0, top) && ReadSample(logicalX, logicalY + 1.0, bottom);
    luma = clamp(5.0 * Luma(sample) - Luma(left) - Luma(right) - Luma(top) - Luma(bottom), 0.0, 255.0);
    return valid;
}

uint ProjectLumaChip(uint2 tileOrigin, uint chip, uint model)
{
    if (model == 0)
    {
        return chip;
    }
    const float2 logical = (float2)(tileOrigin + uint2(chip % 5, chip / 5));
    const float2 origin = float2(OriginX, OriginY);
    const float2 scale = float2(ScaleX, ScaleY);
    const float2 pixel = floor(origin + scale * (logical + 0.5));
    const float2 source = (pixel + 0.5 - origin) / scale;
    // Same bounded point-sampler tie models as the CPU reference. Phase pilots
    // select the model; no provider, adapter or expected payload is consulted.
    const float tieTolerance = 1.0 / 4096.0;
    const float2 lower = ceil(source - tieTolerance) - 1.0;
    const float2 upper = floor(source + tieTolerance);
    const float column = (ScaleX >= 1.0 ? logical.x : (((model - 1) & 1) != 0 ? upper.x : lower.x)) - (float)tileOrigin.x;
    const float row = (ScaleY >= 1.0 ? logical.y : (((model - 1) & 2) != 0 ? upper.y : lower.y)) - (float)tileOrigin.y;
    return column >= 0.0 && column < 5.0 && row >= 0.0 && row < 5.0 ? (uint)row * 5 + (uint)column : 25;
}

float GetLumaCentroid(uint label)
{
    float result = 0.0;
    [unroll]
    for (uint pilot = 0; pilot < 4; pilot++)
    {
        result += Calibration[pilot * 4 + label].x * 0.25;
    }
    return result;
}

float2 GetChromaCentroid(uint label)
{
    float2 result = 0.0;
    [unroll]
    for (uint pilot = 0; pilot < 4; pilot++)
    {
        result += Calibration[16 + pilot * 4 + label].xy * 0.25;
    }
    return result;
}

// Gray-state mode helpers: the per-frame tile foreground-mean histogram feeds
// a deterministic four-peak mode estimate (mirroring the CPU oracle's
// ComputeUnifiedStateModes exactly), because remote links remap glyph
// foreground luma differently from flat calibration stripes.
void AccumulateTileForegroundMean(uint tileOrdinal, uint2 tileOrigin, float foregroundThreshold)
{
    float lumaSum = 0.0;
    uint foregroundChips = 0;
    [loop]
    for (uint chip = 0; chip < 25; chip++)
    {
        float3 sample = 0.0;
        if (ReadSample((float)(tileOrigin.x + chip % 5), (float)(tileOrigin.y + chip / 5), sample) &&
            Luma(sample) >= foregroundThreshold)
        {
            lumaSum += Luma(sample);
            foregroundChips++;
        }
    }
    if (foregroundChips == 0)
    {
        return;
    }
    const uint bin = (uint)clamp(lumaSum / (float)foregroundChips, 0.0, 255.0);
    InterlockedAdd(ForegroundHistogram[bin], 1u);
}

[numthreads(64, 1, 1)]
void BinUnifiedForegroundCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint tileOrdinal = dispatchThreadId.x;
    if (StateLumaMode == 0 || tileOrdinal >= TileCount)
    {
        return;
    }
    const UnifiedTileBinding binding = UnifiedBindings[tileOrdinal];
    const float low = GetLumaCentroid(0);
    const float ladderMid = GetLumaCentroid(1);
    const float foregroundThreshold = (low + ladderMid) * 0.5;
    AccumulateTileForegroundMean(tileOrdinal, uint2(binding.OriginX, binding.OriginY), foregroundThreshold);
}

[numthreads(1, 1, 1)]
void FindUnifiedModesCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // Single-thread deterministic pass: reads the binned means, mirrors the CPU
    // peak selection (smoothed local maxima, greedy by mass, separation >= 32,
    // each peak >= 0.5% of tiles, combined >= 50%, +/-6-bin refinement), then
    // clears the histogram for the next frame.
    uint histogram[256];
    uint total = 0;
    [unroll]
    for (uint load = 0; load < 256; load++)
    {
        histogram[load] = ForegroundHistogram[load];
        total += histogram[load];
    }
    bool valid = total >= 1024;
    uint peaks[4];
    uint peakCount = 0;
    bool taken[256];
    [unroll]
    for (uint clear = 0; clear < 256; clear++)
    {
        taken[clear] = false;
    }
    while (valid && peakCount < 4)
    {
        uint best = 256;
        uint bestMass = 0;
        [loop]
        for (uint index = 1; index < 255; index++)
        {
            // Mirrors the CPU smoothed array whose 0/255 entries stay zero.
            const uint smoothedHere = histogram[index - 1] + 2 * histogram[index] + histogram[index + 1];
            const uint below = index >= 2 ? index - 2 : 0;
            const uint above = index + 2 <= 255 ? index + 2 : 255;
            const uint smoothedPrev = index > 1 ? histogram[below] + 2 * histogram[index - 1] + histogram[index] : 0;
            const uint smoothedNext = index < 254 ? histogram[index] + 2 * histogram[index + 1] + histogram[above] : 0;
            const bool isLocalMax = smoothedHere > smoothedPrev && smoothedHere >= smoothedNext;
            if (taken[index] || bestMass >= smoothedHere || !isLocalMax)
            {
                continue;
            }
            best = index;
            bestMass = smoothedHere;
        }
        if (best == 256)
        {
            break;
        }
        bool separated = true;
        [unroll]
        for (uint existing = 0; existing < 4; existing++)
        {
            if (existing < peakCount)
            {
                const uint distance = best > peaks[existing] ? best - peaks[existing] : peaks[existing] - best;
                separated = separated && distance >= 32;
            }
        }
        if (!separated)
        {
            taken[best] = true;
            continue;
        }
        peaks[peakCount] = best;
        peakCount++;
        taken[best] = true;
    }
    float centers[4];
    float coveredMass = 0.0;
    [unroll]
    for (uint mode = 0; mode < 4; mode++)
    {
        centers[mode] = -1.0;
    }
    if (valid && peakCount == 4)
    {
        [unroll]
        for (uint mode = 0; mode < 4; mode++)
        {
            const uint smoothedPeak = histogram[peaks[mode] - 1] + 2 * histogram[peaks[mode]] + histogram[peaks[mode] + 1];
            if (smoothedPeak * 200u < total)
            {
                valid = false;
                break;
            }
            const uint low = peaks[mode] > 6 ? peaks[mode] - 6 : 0;
            const uint high = peaks[mode] + 7 < 256 ? peaks[mode] + 7 : 256;
            // Peak bin is the robust center (field sigma 15..20 luma makes
            // tail-chasing refinement displace class boundaries); the +/-6
            // window only feeds the combined-coverage validity gate.
            [loop]
            for (uint index = low; index < high; index++)
            {
                coveredMass += (float)histogram[index];
            }
            centers[mode] = (float)peaks[mode];
        }
        if (valid)
        {
            [unroll]
            for (uint mode = 1; mode < 4; mode++)
            {
                if (centers[mode] - centers[mode - 1] < 32.0)
                {
                    valid = false;
                }
            }
            if (coveredMass < (float)total * 0.5)
            {
                valid = false;
            }
        }
    }
    else
    {
        valid = false;
    }
    UnifiedStateModes[0] = float4(valid ? centers[0] : -1.0, valid ? centers[1] : -1.0,
        valid ? centers[2] : -1.0, valid ? centers[3] : -1.0);
    UnifiedStateModes[1] = float4(valid ? 1.0 : 0.0, 0.0, 0.0, 0.0);
    [unroll]
    for (uint zero = 0; zero < 256; zero++)
    {
        ForegroundHistogram[zero] = 0u;
    }
}

[numthreads(36, 1, 1)]
void CalibrateUnifiedCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint entry = dispatchThreadId.x;
    if (entry >= 36)
    {
        return;
    }
    const uint pilot = entry < 32 ? (entry & 15) / 4 : entry - 32;
    const uint label = entry & 3;
    const uint2 origin = CalibrationOrigins[pilot];
    float2 sum = 0.0;
    float2 squareSum = 0.0;
    uint samples = 0;
    bool valid = true;
    if (entry < 16)
    {
        for (uint row = 4; row < 20; row++)
        {
            for (uint column = label * 32 + 4; column < label * 32 + 28; column++)
            {
                float3 sample = 0.0;
                valid = ReadSample((float)(origin.x + column), (float)(origin.y + row), sample) && valid;
                const float value = Luma(sample);
                sum.x += value;
                squareSum.x += value * value;
                samples++;
            }
        }
        const float mean = samples == 0 ? 0.0 : sum.x / (float)samples;
        const float variance = samples == 0 ? 0.0 : max(0.0, squareSum.x / (float)samples - mean * mean);
        CalibrationOutput[entry] = float4(mean, variance, valid ? 1.0 : 0.0, 0.0);
        return;
    }
    if (entry < 32)
    {
        for (uint row = 36; row < 60; row++)
        {
            for (uint column = label * 32 + 4; column < label * 32 + 28; column++)
            {
                float3 sample = 0.0;
                valid = ReadSample((float)(origin.x + column), (float)(origin.y + row), sample) && valid;
                if (StateLumaMode != 0)
                {
                    // Gray-state stripes calibrate the second carrier on luma;
                    // the opponent coordinate stays exactly zero so the CPU
                    // readback keeps its existing centroid/variance form.
                    const float value = Luma(sample);
                    sum.x += value;
                    squareSum.x += value * value;
                }
                else
                {
                    const float2 value = Opponent(sample);
                    sum += value;
                    squareSum += value * value;
                }
                samples++;
            }
        }
        if (StateLumaMode != 0)
        {
            const float mean = samples == 0 ? 0.0 : sum.x / (float)samples;
            const float variance = samples == 0 ? 0.0 : max(0.0, squareSum.x / (float)samples - mean * mean);
            CalibrationOutput[entry] = float4(mean, 0.0, variance, 0.0);
        }
        else
        {
            const float2 mean = samples == 0 ? 0.0 : sum / (float)samples;
            const float2 variance = samples == 0 ? 0.0 : max(0.0, squareSum / (float)samples - mean * mean);
            CalibrationOutput[entry] = float4(mean, variance);
        }
        if (!valid)
        {
            CalibrationOutput[entry].w = -1.0;
        }
        return;
    }
    for (uint row = 25; row < 31; row++)
    {
        for (uint column = 4; column < 124; column++)
        {
            float3 sample = 0.0;
            valid = ReadSample((float)(origin.x + column), (float)(origin.y + row), sample) && valid;
            sum += Opponent(sample);
            samples++;
        }
    }
    CalibrationOutput[entry] = float4(samples == 0 ? 0.0 : sum / (float)samples,
        valid ? 1.0 : 0.0, 0.0);
}

[numthreads(9, 1, 1)]
void EvaluateUnifiedFreshnessCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint region = dispatchThreadId.x;
    if (region >= 9)
    {
        return;
    }
    const uint2 origin = FreshnessOrigins[region];
    const float contrast = BootstrapWhiteLevel - BootstrapBlackLevel;
    uint errors = 0;
    float residual = 0.0;
    bool valid = contrast > 0.0;
    for (uint bitIndex = 0; bitIndex < 256; bitIndex++)
    {
        const uint bitColumn = bitIndex & 15;
        const uint bitRow = bitIndex >> 4;
        float sum = 0.0;
        for (uint sampleRow = 2; sampleRow < 6; sampleRow++)
        {
            for (uint sampleColumn = 2; sampleColumn < 6; sampleColumn++)
            {
                float3 sample = 0.0;
                valid = ReadSample((float)(origin.x + bitColumn * 8 + sampleColumn),
                    (float)(origin.y + bitRow * 8 + sampleRow), sample) && valid;
                sum += Luma(sample);
            }
        }
        const float normalized = contrast > 0.0 ? (sum / 16.0 - BootstrapBlackLevel) / contrast : 0.0;
        const uint expected = ExpectedFreshnessBits[region * 256 + bitIndex];
        residual += abs(normalized - (float)expected);
        errors += (uint)((normalized >= 0.5) != (expected != 0));
    }
    FreshnessOutput[region] = float4((float)errors, residual / 256.0, valid ? 1.0 : 0.0, 0.0);
}

uint GetPhaseLabel(uint pilot, uint tileOrdinal, uint phase)
{
    if (pilot == 0)
    {
        return (tileOrdinal + phase) & 7;
    }
    return (((tileOrdinal >> 1) + phase) & 7) | (((tileOrdinal + phase) & 1) << 3);
}

groupshared float CanonicalPhaseDistances[16];

[numthreads(16, 1, 1)]
void EvaluateUnifiedPhaseCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint entry = dispatchThreadId.x;
    // Exactly one sixteen-thread group is dispatched; every thread reaches
    // the barrier before a lane chooses its same-frame sampling model.
    const uint pilot = entry >> 3;
    const uint phase = entry & 7;
    const uint2 origin = PhaseOrigins[pilot];
    const float low = GetLumaCentroid(0);
    const float high = GetLumaCentroid(2);
    float distance = 0.0;
    float sharpenedDistance = 0.0;
    bool valid = true;
    for (uint tileOrdinal = 0; tileOrdinal < 210; tileOrdinal++)
    {
        const uint2 tileOrigin = origin + uint2((tileOrdinal % 21) * 6, (tileOrdinal / 21) * 6);
        const uint mask = SymbolMasks[GetPhaseLabel(pilot, tileOrdinal, phase)];
        // Keep the bounded sampling loop compact: expanding ReadSample twenty-five
        // times makes shader initialization dominate the first-frame deadline.
        [loop]
        for (uint chip = 0; chip < 25; chip++)
        {
            float3 sample = 0.0;
            float sharpenedLuma = 0.0;
            const float logicalX = (float)(tileOrigin.x + chip % 5);
            const float logicalY = (float)(tileOrigin.y + chip / 5);
            valid = ReadSample(logicalX, logicalY, sample) && valid;
            valid = ReadSharpenedLuma(logicalX, logicalY, sharpenedLuma) && valid;
            const float expected = ((mask >> chip) & 1) != 0 ? high : low;
            const float difference = Luma(sample) - expected;
            const float sharpenedDifference = sharpenedLuma - expected;
            distance += difference * difference;
            sharpenedDistance += sharpenedDifference * sharpenedDifference;
        }
    }
    distance = min(distance, sharpenedDistance);
    CanonicalPhaseDistances[entry] = distance;
    GroupMemoryBarrierWithGroupSync();
    const uint expectedPhase = InterleavePhase & 7;
    const float expectedDistance = CanonicalPhaseDistances[pilot * 8 + expectedPhase];
    float nearestOther = 3.402823466e+38;
    [unroll]
    for (uint otherPhase = 0; otherPhase < 8; otherPhase++)
    {
        if (otherPhase != expectedPhase)
        {
            nearestOther = min(nearestOther, CanonicalPhaseDistances[pilot * 8 + otherPhase]);
        }
    }
    const float gap = high - low;
    const bool canonicalAccepted = valid && gap > 0.0 && expectedDistance < nearestOther &&
        expectedDistance / (5250.0 * gap * gap) <= MinimumSymbolRms;
    uint selectedModel = 0;
    if (!canonicalAccepted && (ScaleX < 1.0 || ScaleY < 1.0))
    {
        [loop]
        for (uint model = 1; model <= 4; model++)
        {
            float candidateDistance = 0.0;
            float candidateSharpenedDistance = 0.0;
            bool candidateValid = true;
            for (uint tileOrdinal = 0; tileOrdinal < 210; tileOrdinal++)
            {
                const uint2 tileOrigin = origin + uint2((tileOrdinal % 21) * 6, (tileOrdinal / 21) * 6);
                const uint mask = SymbolMasks[GetPhaseLabel(pilot, tileOrdinal, phase)];
                [loop]
                for (uint chip = 0; chip < 25; chip++)
                {
                    float3 sample = 0.0;
                    float sharpenedLuma = 0.0;
                    const uint projected = ProjectLumaChip(tileOrigin, chip, model);
                    candidateValid = projected < 25 && candidateValid;
                    const float logicalX = (float)(tileOrigin.x + chip % 5);
                    const float logicalY = (float)(tileOrigin.y + chip / 5);
                    candidateValid = ReadSample(logicalX, logicalY, sample) && candidateValid;
                    candidateValid = ReadSharpenedLuma(logicalX, logicalY, sharpenedLuma) && candidateValid;
                    const float expected = ((mask >> min(projected, 24)) & 1) != 0 ? high : low;
                    const float difference = Luma(sample) - expected;
                    const float sharpenedDifference = sharpenedLuma - expected;
                    candidateDistance += difference * difference;
                    candidateSharpenedDistance += sharpenedDifference * sharpenedDifference;
                }
            }
            candidateDistance = min(candidateDistance, candidateSharpenedDistance);
            if (candidateValid && candidateDistance < distance)
            {
                distance = candidateDistance;
                selectedModel = model;
            }
        }
    }
    PhaseOutput[entry] = float4(distance, 210.0, valid ? 1.0 : 0.0, (float)selectedModel);
}

[numthreads(64, 1, 1)]
void DemodUnifiedCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint tileOrdinal = dispatchThreadId.x;
    if (tileOrdinal >= TileCount)
    {
        return;
    }
    const UnifiedTileBinding binding = UnifiedBindings[tileOrdinal];
    float3 samples[25];
    float lumaSamples[25];
    bool valid = true;
    [loop]
    for (uint chip = 0; chip < 25; chip++)
    {
        valid = ReadSample((float)(binding.OriginX + chip % 5),
            (float)(binding.OriginY + chip / 5), samples[chip]) && valid;
        const float logicalX = (float)(binding.OriginX + chip % 5);
        const float logicalY = (float)(binding.OriginY + chip / 5);
        float3 left = 0.0;
        float3 right = 0.0;
        float3 top = 0.0;
        float3 bottom = 0.0;
        valid = ReadSample(logicalX - 1.0, logicalY, left) && valid;
        valid = ReadSample(logicalX + 1.0, logicalY, right) && valid;
        valid = ReadSample(logicalX, logicalY - 1.0, top) && valid;
        valid = ReadSample(logicalX, logicalY + 1.0, bottom) && valid;
        // This sum-one cross kernel preserves flat calibration levels while counteracting provider low-pass blur
        // before shape scoring. The original RGB samples remain authoritative for the chroma decision below.
        lumaSamples[chip] = clamp(5.0 * Luma(samples[chip]) - Luma(left) - Luma(right) - Luma(top) - Luma(bottom), 0.0, 255.0);
    }
    TileSamplingFailures[tileOrdinal] = valid ? 0 : 1;
    if (!valid)
    {
        return;
    }
    const float low = GetLumaCentroid(0);
    const float high = GetLumaCentroid(2);
    const float lumaGap = high - low;
    if (StateLumaMode != 0)
    {
        // Gray carrier v3: six punctured-Hadamard mask planes plus one
        // foreground-level plane per tile. The mask planes classify against
        // the 64-symbol book using the tile's own measured foreground mean,
        // so a level bit of either value never disturbs the mask distances.
        const float background = GetLumaCentroid(0);
        const float ladderLow = GetLumaCentroid(1);
        const float ladderHigh = GetLumaCentroid(3);
        const float foregroundGate = (background + ladderLow) * 0.5;
        uint foregroundSamples = 0;
        float foregroundLumaSum = 0.0;
        [loop]
        for (uint gateChip = 0; gateChip < 25; gateChip++)
        {
            const float foregroundValue = Luma(samples[gateChip]);
            if (foregroundValue >= foregroundGate)
            {
                foregroundSamples++;
                foregroundLumaSum += foregroundValue;
            }
        }
        const uint grayPlanes[7] = {binding.LumaBits[0], binding.LumaBits[1], binding.LumaBits[2],
            binding.LumaBits[3], binding.ChromaBits[0], binding.ChromaBits[1], binding.LumaBits[4]};
        if (foregroundSamples == 0)
        {
            // An all-background tile uniquely identifies its mask: symbol 32
            // is the only zero-foreground symbol, so all six mask bits are
            // decided at full confidence and only the level plane stays
            // erased (metric 0). A pure 0-LLR erasure would zero every
            // min-sum message of its check rows and stall the decoder.
            [unroll]
            for (uint plane = 0; plane < 7; plane++)
            {
                if (grayPlanes[plane] < UnifiedGrayMetricCount)
                {
                    MetricOutput[grayPlanes[plane]] = plane == 6 ? 0.0 :
                        (((32u >> plane) & 1u) != 0 ? -32767.0 : 32767.0);
                }
            }
            return;
        }
        const float tileForeground = foregroundLumaSum / (float)foregroundSamples;
        const uint baseModel = (uint)PhaseOutput[InterleavePhase & 7].w;
        uint projected[25];
        bool symbolModelValid = true;
        [loop]
        for (uint projectionChip = 0; projectionChip < 25; projectionChip++)
        {
            projected[projectionChip] = ProjectLumaChip(uint2(binding.OriginX, binding.OriginY),
                projectionChip, baseModel);
            symbolModelValid = symbolModelValid && projected[projectionChip] < 25;
        }
        float symbolDistances[64];
        [unroll]
        for (uint symbol = 0; symbol < 64; symbol++)
        {
            float distance = 0.0;
            if (symbolModelValid)
            {
                const uint mask = GrayMasksBySymbol[symbol];
                [loop]
                for (uint chip = 0; chip < 25; chip++)
                {
                    const float expected = ((mask >> min(projected[chip], 24)) & 1) != 0 ?
                        tileForeground : background;
                    const float difference = Luma(samples[chip]) - expected;
                    distance += difference * difference;
                }
            }
            symbolDistances[symbol] = distance;
        }
        // Normalize by the tile's own foreground contrast: a nine-chip mask
        // flip at any level then yields 9 x 2048 = 18432, the magnitude the
        // frozen min-sum offset (2048) and message accumulation proved safe
        // in the SC6 product path. A fixed ladder-gap scale would leave
        // LOW-level tiles at a magnitude mid-decode accumulation can flip.
        const float tileContrast = tileForeground - background;
        const float maskScale = tileContrast > 0.0 ? 2048.0 / (tileContrast * tileContrast) : 0.0;
        const float levelGap = ladderHigh - ladderLow;
        [unroll]
        for (uint plane = 0; plane < 7; plane++)
        {
            float metric = 0.0;
            if (symbolModelValid && plane == 6)
            {
                const float lowDistance = tileForeground - ladderLow;
                const float highDistance = tileForeground - ladderHigh;
                metric = levelGap > 0.0 ? (highDistance * highDistance - lowDistance * lowDistance) *
                    8192.0 / (levelGap * levelGap) : 0.0;
            }
            else if (symbolModelValid)
            {
                float zeroDistance = 3.402823466e+38;
                float oneDistance = 3.402823466e+38;
                [unroll]
                for (uint symbol = 0; symbol < 64; symbol++)
                {
                    if (((symbol >> plane) & 1) != 0)
                    {
                        oneDistance = min(oneDistance, symbolDistances[symbol]);
                    }
                    else
                    {
                        zeroDistance = min(zeroDistance, symbolDistances[symbol]);
                    }
                }
                metric = (oneDistance - zeroDistance) * maskScale;
            }
            if (grayPlanes[plane] < UnifiedGrayMetricCount)
            {
                MetricOutput[grayPlanes[plane]] = metric;
            }
        }
        return;
    }
    const float foregroundThreshold = (low + high) * 0.5;
    uint foregroundSamples = 0;
    float foregroundLumaSum = 0.0;
    [loop]
    for (uint foregroundChip = 0; foregroundChip < 25; foregroundChip++)
    {
        const float foregroundValue = Luma(samples[foregroundChip]);
        if (foregroundValue >= foregroundThreshold)
        {
            foregroundSamples++;
            foregroundLumaSum += foregroundValue;
        }
    }
    TileSamplingFailures[tileOrdinal] = valid && foregroundSamples != 0 ? 0 : 1;
    const float tileExpectedForeground = high;
    const uint baseModel = (uint)PhaseOutput[InterleavePhase & 7].w;
    const uint fineModel = (uint)PhaseOutput[8 + (InterleavePhase & 7)].w;
    uint baseChips[25];
    uint fineChips[25];
    bool baseValid = true;
    bool fineValid = true;
    [loop]
    for (uint sourceChip = 0; sourceChip < 25; sourceChip++)
    {
        baseChips[sourceChip] = ProjectLumaChip(uint2(binding.OriginX, binding.OriginY), sourceChip, baseModel);
        fineChips[sourceChip] = baseModel == fineModel ? baseChips[sourceChip] :
            ProjectLumaChip(uint2(binding.OriginX, binding.OriginY), sourceChip, fineModel);
        baseValid = baseValid && baseChips[sourceChip] < 25;
        fineValid = fineValid && fineChips[sourceChip] < 25;
    }
    float baseDistances[16];
    float fineDistances[16];
    [unroll]
    for (uint label = 0; label < 16; label++)
    {
        float baseDistance = 0.0;
        float fineDistance = 0.0;
        const uint mask = SymbolMasks[label];
        [loop]
        for (uint chipIndex = 0; chipIndex < 25; chipIndex++)
        {
            const float value = lumaSamples[chipIndex];
            const float baseExpected = ((mask >> min(baseChips[chipIndex], 24)) & 1) != 0 ?
                tileExpectedForeground : low;
            const float baseDifference = value - baseExpected;
            baseDistance += baseDifference * baseDifference;
            if (baseModel != fineModel)
            {
                const float fineExpected = ((mask >> min(fineChips[chipIndex], 24)) & 1) != 0 ?
                    tileExpectedForeground : low;
                const float fineDifference = value - fineExpected;
                fineDistance += fineDifference * fineDifference;
            }
        }
        baseDistances[label] = baseValid ? baseDistance : 0.0;
        fineDistances[label] = fineValid ? (baseModel == fineModel ? baseDistance : fineDistance) : 0.0;
    }
    const uint lumaBits[4] = {binding.LumaBits[0], binding.LumaBits[1], binding.LumaBits[2], binding.LumaBits[3]};
    [unroll]
    for (uint bitPlane = 0; bitPlane < 4; bitPlane++)
    {
        const bool fineLane = lumaBits[bitPlane] >= UnifiedBaseLumaMetricCount &&
            lumaBits[bitPlane] < UnifiedLumaMetricCount;
        float zeroDistance = 3.402823466e+38;
        float oneDistance = 3.402823466e+38;
        [unroll]
        for (uint labelIndex = 0; labelIndex < 16; labelIndex++)
        {
            if (((labelIndex >> bitPlane) & 1) != 0)
            {
                oneDistance = min(oneDistance, fineLane ? fineDistances[labelIndex] : baseDistances[labelIndex]);
            }
            else
            {
                zeroDistance = min(zeroDistance, fineLane ? fineDistances[labelIndex] : baseDistances[labelIndex]);
            }
        }
        if (lumaBits[bitPlane] < UnifiedMetricCount)
        {
            MetricOutput[lumaBits[bitPlane]] = lumaGap > 0.0 ?
                (oneDistance - zeroDistance) * 2048.0 / (lumaGap * lumaGap) : 0.0;
        }
    }
    float chromaDistances[4];
    {
    float2 averageOpponent = 0.0;
    [loop]
    for (uint opponentChip = 0; opponentChip < 25; opponentChip++)
    {
        if (Luma(samples[opponentChip]) >= foregroundThreshold)
        {
            averageOpponent += Opponent(samples[opponentChip]);
        }
    }
    if (foregroundSamples != 0)
    {
        averageOpponent /= (float)foregroundSamples;
    }
    [unroll]
    for (uint chromaLabel = 0; chromaLabel < 4; chromaLabel++)
    {
        const float2 difference = averageOpponent - GetChromaCentroid(chromaLabel);
        chromaDistances[chromaLabel] = dot(difference, difference);
    }

    }
    const uint chromaBits[2] = {binding.ChromaBits[0], binding.ChromaBits[1]};
    [unroll]
    for (uint chromaPlane = 0; chromaPlane < 2; chromaPlane++)
    {
        float zeroDistance = 3.402823466e+38;
        float oneDistance = 3.402823466e+38;
        [unroll]
        for (uint labelValue = 0; labelValue < 4; labelValue++)
        {
            if (((labelValue >> chromaPlane) & 1) != 0)
            {
                oneDistance = min(oneDistance, chromaDistances[labelValue]);
            }
            else
            {
                zeroDistance = min(zeroDistance, chromaDistances[labelValue]);
            }
        }
        if (chromaBits[chromaPlane] < UnifiedMetricCount)
        {
            MetricOutput[chromaBits[chromaPlane]] = foregroundSamples == 0 ? 0.0 :
                (oneDistance - zeroDistance) * 4.0;
        }
    }
}
