cbuffer FrameConstants : register(b0)
{
    uint Mode;
    uint TilePixels;
    uint TileCount;
    uint RowTiles;
    uint InterleavePhase;
    uint MetricCount;
    uint Reserved0;
    uint Reserved1;
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
};

Texture2D<float4> RoiTexture : register(t0);
StructuredBuffer<float4> Calibration : register(t1);
StructuredBuffer<uint4> TileMappings : register(t2);
StructuredBuffer<uint4> FrameBindings : register(t3);
StructuredBuffer<uint> SymbolMasks : register(t4);
RWStructuredBuffer<float4> CalibrationOutput : register(u0);
RWStructuredBuffer<float> MetricOutput : register(u1);
RWStructuredBuffer<uint4> FreshnessSummary : register(u2);
SamplerState LinearSampler : register(s0);

static const uint RoleFreshness = 1;
static const uint RoleData = 2;
static const uint CodedBits = 64800;
static const uint2 PilotOrigins[4] = {uint2(736, 16), uint2(1696, 16), uint2(96, 1000), uint2(1056, 1000)};
static const float2 SampleOffsets[5] =
{
    float2(0.0, 0.0), float2(-0.45, 0.0), float2(0.45, 0.0), float2(0.0, -0.45), float2(0.0, 0.45)
};

float Luma(float3 blueGreenRed)
{
    return dot(blueGreenRed, float3(0.0722, 0.7152, 0.2126));
}

bool SampleLuma(float2 logicalPosition, out float value, out bool clipped)
{
    const float2 physicalPosition = float2(OriginX, OriginY) + float2(ScaleX, ScaleY) * logicalPosition - 0.5;
    if (any(physicalPosition < 0.0) || physicalPosition.x > (float)(SourceWidth - 1) ||
        physicalPosition.y > (float)(SourceHeight - 1))
    {
        value = 0.0;
        clipped = true;
        return false;
    }
    const float2 texturePosition = (physicalPosition + 0.5) * float2(SourceTexelPitchX, SourceTexelPitchY);
    value = Luma(RoiTexture.SampleLevel(LinearSampler, texturePosition, 0).rgb * 255.0);
    clipped = value <= 0.0 || value >= 255.0;
    return true;
}

bool SampleChips(uint2 tileOrigin, out float chips[16], out bool clipped)
{
    clipped = false;
    [unroll]
    for (uint chip = 0; chip < 16; chip++)
    {
        const uint chipRow = chip >> 2;
        const uint chipColumn = chip & 3;
        float sum = 0.0;
        [unroll]
        for (uint sampleIndex = 0; sampleIndex < 5; sampleIndex++)
        {
            float sample = 0.0;
            bool sampleClipped = false;
            const float2 logicalPosition = float2(tileOrigin) + float2(chipColumn * 2 + 1, chipRow * 2 + 1) +
                SampleOffsets[sampleIndex];
            if (!SampleLuma(logicalPosition, sample, sampleClipped))
            {
                return false;
            }
            sum += sample;
            clipped = clipped || sampleClipped;
        }
        chips[chip] = sum / 5.0;
    }
    return true;
}

void GetCentroids(out float zeroCentroid, out float oneCentroid)
{
    zeroCentroid = 0.0;
    oneCentroid = 0.0;
    [unroll]
    for (uint ladder = 0; ladder < 4; ladder++)
    {
        zeroCentroid += Calibration[ladder * 4].x / 4.0;
        oneCentroid += Calibration[ladder * 4 + 3].x / 4.0;
    }
}

[numthreads(16, 1, 1)]
void CalibrateRemoteVisualLowFpsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint entry = dispatchThreadId.x;
    if (entry >= 16)
    {
        return;
    }
    const uint ladder = entry / 4;
    const uint level = entry % 4;
    if (level != 0 && level != 3)
    {
        CalibrationOutput[entry] = 0.0;
        return;
    }
    float sum = 0.0;
    float squares = 0.0;
    uint clippedSamples = 0;
    [unroll]
    for (uint rowIndex = 0; rowIndex < 8; rowIndex++)
    {
        [unroll]
        for (uint columnIndex = 0; columnIndex < 8; columnIndex++)
        {
            const uint row = 4 + rowIndex * 7;
            const uint column = 4 + columnIndex * 3;
            const float2 logicalPosition = float2(PilotOrigins[ladder]) + float2(level * 32 + column + 0.5, row + 0.5);
            float sample = 0.0;
            bool clipped = false;
            if (!SampleLuma(logicalPosition, sample, clipped))
            {
                CalibrationOutput[entry] = float4(0.0, 0.0, 1.0, 0.0);
                return;
            }
            sum += sample;
            squares += sample * sample;
            clippedSamples += clipped ? 1 : 0;
        }
    }
    const float mean = sum / 64.0;
    CalibrationOutput[entry] = float4(mean, max(0.0, squares / 64.0 - mean * mean), (float)clippedSamples, 0.0);
}

[numthreads(64, 1, 1)]
void DemodRemoteVisualLowFpsFreshnessCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint physical = dispatchThreadId.x;
    if (physical >= TileCount)
    {
        return;
    }
    const uint4 mapping = TileMappings[physical];
    if (mapping.z != RoleFreshness || mapping.w >= FreshnessRegionCount)
    {
        return;
    }
    float chips[16];
    bool clipped = false;
    if (!SampleChips(mapping.xy, chips, clipped))
    {
        InterlockedAdd(FreshnessSummary[FreshnessRegionCount].x, 1);
        return;
    }
    float mean = 0.0;
    [unroll]
    for (uint chip = 0; chip < 16; chip++)
    {
        mean += chips[chip] / 16.0;
    }
    float zeroCentroid = 0.0;
    float oneCentroid = 0.0;
    GetCentroids(zeroCentroid, oneCentroid);
    const float halfSeparation = (oneCentroid - zeroCentroid) * 0.5;
    if (halfSeparation <= 0.0)
    {
        InterlockedAdd(FreshnessSummary[FreshnessRegionCount].x, 1);
        return;
    }
    const float freshnessMetric = ((zeroCentroid + oneCentroid) * 0.5 - mean) / halfSeparation;
    const bool erased = clipped || abs(freshnessMetric) < MinimumFreshnessMetric;
    const bool expectedOne = FrameBindings[physical].x != 0;
    const bool mismatched = !erased && ((freshnessMetric < 0.0) != expectedOne);
    if (erased)
    {
        InterlockedAdd(FreshnessSummary[mapping.w].y, 1);
        InterlockedOr(FreshnessSummary[mapping.w].z, 1);
    }
    else if (mismatched)
    {
        InterlockedAdd(FreshnessSummary[mapping.w].x, 1);
        InterlockedOr(FreshnessSummary[mapping.w].z, 1);
    }
}

[numthreads(64, 1, 1)]
void DemodRemoteVisualLowFpsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint physical = dispatchThreadId.x;
    if (physical >= TileCount)
    {
        return;
    }
    const uint4 mapping = TileMappings[physical];
    if (mapping.z != RoleData || mapping.w >= FreshnessRegionCount)
    {
        return;
    }
    const uint4 frameBinding = FrameBindings[physical];
    const bool stale = FreshnessSummary[mapping.w].z != 0;
    float chips[16];
    bool clipped = false;
    if (!SampleChips(mapping.xy, chips, clipped))
    {
        InterlockedAdd(FreshnessSummary[FreshnessRegionCount].x, 1);
        return;
    }
    float zeroCentroid = 0.0;
    float oneCentroid = 0.0;
    GetCentroids(zeroCentroid, oneCentroid);
    const float halfSeparation = (oneCentroid - zeroCentroid) * 0.5;
    if (halfSeparation <= 0.0)
    {
        InterlockedAdd(FreshnessSummary[FreshnessRegionCount].x, 1);
        return;
    }
    float normalized[16];
    float normalizedMean = 0.0;
    [unroll]
    for (uint normalizationChip = 0; normalizationChip < 16; normalizationChip++)
    {
        normalized[normalizationChip] = (chips[normalizationChip] - (zeroCentroid + oneCentroid) * 0.5) / halfSeparation;
        normalizedMean += normalized[normalizationChip] / 16.0;
    }
    float energy = 0.0;
    [unroll]
    for (uint centeredChip = 0; centeredChip < 16; centeredChip++)
    {
        normalized[centeredChip] -= normalizedMean;
        energy += normalized[centeredChip] * normalized[centeredChip] / 16.0;
    }
    float distances[16];
    [unroll]
    for (uint candidateSymbol = 0; candidateSymbol < 16; candidateSymbol++)
    {
        float distance = 0.0;
        [unroll]
        for (uint distanceChip = 0; distanceChip < 16; distanceChip++)
        {
            const float expected = (SymbolMasks[candidateSymbol] & (1u << distanceChip)) != 0 ? 1.0 : -1.0;
            const float difference = normalized[distanceChip] - expected;
            distance += difference * difference / 16.0;
        }
        distances[candidateSymbol] = distance;
    }
    float bestDistance = 3.402823466e+38F;
    float secondDistance = bestDistance;
    [unroll]
    for (uint searchSymbol = 0; searchSymbol < 16; searchSymbol++)
    {
        if (distances[searchSymbol] < bestDistance)
        {
            secondDistance = bestDistance;
            bestDistance = distances[searchSymbol];
        }
        else if (distances[searchSymbol] < secondDistance)
        {
            secondDistance = distances[searchSymbol];
        }
    }
    const float symbolMargin = (secondDistance - bestDistance) / max(1e-9, secondDistance + bestDistance);
    const bool unreliable = clipped || sqrt(energy) < MinimumSymbolRms || bestDistance > MaximumSymbolResidual ||
        symbolMargin < MinimumSymbolMargin;
    if (unreliable)
    {
        InterlockedAdd(FreshnessSummary[FreshnessRegionCount].y, 1);
    }
    [unroll]
    for (uint metricPlane = 0; metricPlane < 4; metricPlane++)
    {
        const uint logical = frameBinding[metricPlane];
        if (logical >= CodedBits)
        {
            continue;
        }
        if (stale)
        {
            MetricOutput[logical] = 0.0;
            InterlockedAdd(FreshnessSummary[mapping.w].w, 1);
            continue;
        }
        float zeroDistance = 3.402823466e+38F;
        float oneDistance = zeroDistance;
        [unroll]
        for (uint labeledSymbol = 0; labeledSymbol < 16; labeledSymbol++)
        {
            if ((labeledSymbol & (1u << metricPlane)) == 0)
            {
                zeroDistance = min(zeroDistance, distances[labeledSymbol]);
            }
            else
            {
                oneDistance = min(oneDistance, distances[labeledSymbol]);
            }
        }
        MetricOutput[logical] = unreliable ? 0.0 : (oneDistance - zeroDistance) * 0.5;
    }
}
