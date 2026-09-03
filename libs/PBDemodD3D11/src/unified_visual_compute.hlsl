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
    float BootstrapBlackLevel;
    float BootstrapWhiteLevel;
    float ReservedFloat0;
    float ReservedFloat1;
};

struct UnifiedTileBinding
{
    uint OriginX;
    uint OriginY;
    uint LumaBit0;
    uint LumaBit1;
    uint LumaBit2;
    uint LumaBit3;
    uint ChromaBit0;
    uint ChromaBit1;
};

Texture2D<float4> RoiTexture : register(t0);
StructuredBuffer<float4> Calibration : register(t1);
StructuredBuffer<uint4> UnusedTileMappings : register(t2);
StructuredBuffer<UnifiedTileBinding> UnifiedBindings : register(t3);
StructuredBuffer<uint> SymbolMasks : register(t4);
StructuredBuffer<uint> ExpectedFreshnessBits : register(t5);
RWStructuredBuffer<float4> CalibrationOutput : register(u0);
RWStructuredBuffer<float> MetricOutput : register(u1);
RWStructuredBuffer<uint> TileSamplingFailures : register(u2);
RWStructuredBuffer<float4> FreshnessOutput : register(u3);
RWStructuredBuffer<float4> PhaseOutput : register(u4);

static const uint UnifiedMetricCount = 502200;
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

bool IsClipped(float3 blueGreenRed)
{
    return any(blueGreenRed <= 0.0) || any(blueGreenRed >= 255.0);
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
    const float3 topLeft = RoiTexture.Load(int3(left, top, 0)).rgb * 255.0;
    const float3 topRight = right == left ? topLeft : RoiTexture.Load(int3(right, top, 0)).rgb * 255.0;
    const float3 bottomLeft = bottom == top ? topLeft : RoiTexture.Load(int3(left, bottom, 0)).rgb * 255.0;
    const float3 bottomRight = bottom == top ? topRight :
        (right == left ? bottomLeft : RoiTexture.Load(int3(right, bottom, 0)).rgb * 255.0);
    blueGreenRed = lerp(lerp(topLeft, topRight, horizontal), lerp(bottomLeft, bottomRight, horizontal), vertical);
    return true;
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
    bool clipped = false;
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
                const float2 value = Opponent(sample);
                sum += value;
                squareSum += value * value;
                clipped = clipped || IsClipped(sample);
                samples++;
            }
        }
        const float2 mean = samples == 0 ? 0.0 : sum / (float)samples;
        const float2 variance = samples == 0 ? 0.0 : max(0.0, squareSum / (float)samples - mean * mean);
        CalibrationOutput[entry] = float4(mean, variance);
        if (!valid || clipped)
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

[numthreads(16, 1, 1)]
void EvaluateUnifiedPhaseCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint entry = dispatchThreadId.x;
    if (entry >= 16)
    {
        return;
    }
    const uint pilot = entry >> 3;
    const uint phase = entry & 7;
    const uint2 origin = PhaseOrigins[pilot];
    const float low = GetLumaCentroid(1);
    const float high = GetLumaCentroid(2);
    float distance = 0.0;
    bool valid = true;
    for (uint tileOrdinal = 0; tileOrdinal < 512; tileOrdinal++)
    {
        const uint2 tileOrigin = origin + uint2((tileOrdinal & 31) * 4, (tileOrdinal >> 5) * 4);
        const uint mask = SymbolMasks[GetPhaseLabel(pilot, tileOrdinal, phase)];
        [unroll]
        for (uint chip = 0; chip < 16; chip++)
        {
            float3 sample = 0.0;
            valid = ReadSample((float)(tileOrigin.x + (chip & 3)),
                (float)(tileOrigin.y + (chip >> 2)), sample) && valid;
            const float expected = ((mask >> chip) & 1) != 0 ? high : low;
            const float difference = Luma(sample) - expected;
            distance += difference * difference;
        }
    }
    PhaseOutput[entry] = float4(distance, 512.0, valid ? 1.0 : 0.0, 0.0);
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
    float3 samples[16];
    bool valid = true;
    bool clipped = false;
    [unroll]
    for (uint chip = 0; chip < 16; chip++)
    {
        valid = ReadSample((float)(binding.OriginX + (chip & 3)),
            (float)(binding.OriginY + (chip >> 2)), samples[chip]) && valid;
        clipped = clipped || IsClipped(samples[chip]);
    }
    TileSamplingFailures[tileOrdinal] = valid && !clipped ? 0 : 1;
    if (!valid)
    {
        return;
    }
    const float low = GetLumaCentroid(1);
    const float high = GetLumaCentroid(2);
    const float lumaGap = high - low;
    float lumaDistances[16];
    [unroll]
    for (uint label = 0; label < 16; label++)
    {
        float distance = 0.0;
        const uint mask = SymbolMasks[label];
        [unroll]
        for (uint chipIndex = 0; chipIndex < 16; chipIndex++)
        {
            const float expected = ((mask >> chipIndex) & 1) != 0 ? high : low;
            const float difference = Luma(samples[chipIndex]) - expected;
            distance += difference * difference;
        }
        lumaDistances[label] = distance;
    }
    const uint lumaBits[4] = {binding.LumaBit0, binding.LumaBit1, binding.LumaBit2, binding.LumaBit3};
    [unroll]
    for (uint bitPlane = 0; bitPlane < 4; bitPlane++)
    {
        float zeroDistance = 3.402823466e+38;
        float oneDistance = 3.402823466e+38;
        [unroll]
        for (uint labelIndex = 0; labelIndex < 16; labelIndex++)
        {
            if (((labelIndex >> bitPlane) & 1) != 0)
            {
                oneDistance = min(oneDistance, lumaDistances[labelIndex]);
            }
            else
            {
                zeroDistance = min(zeroDistance, lumaDistances[labelIndex]);
            }
        }
        if (lumaBits[bitPlane] < UnifiedMetricCount)
        {
            MetricOutput[lumaBits[bitPlane]] = lumaGap > 0.0 ?
                (oneDistance - zeroDistance) * 2048.0 / (lumaGap * lumaGap) : 0.0;
        }
    }
    float2 averageOpponent = 0.0;
    [unroll]
    for (uint opponentChip = 0; opponentChip < 16; opponentChip++)
    {
        averageOpponent += Opponent(samples[opponentChip]) / 16.0;
    }
    float chromaDistances[4];
    [unroll]
    for (uint chromaLabel = 0; chromaLabel < 4; chromaLabel++)
    {
        const float2 difference = averageOpponent - GetChromaCentroid(chromaLabel);
        chromaDistances[chromaLabel] = dot(difference, difference);
    }
    const uint chromaBits[2] = {binding.ChromaBit0, binding.ChromaBit1};
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
            MetricOutput[chromaBits[chromaPlane]] = (oneDistance - zeroDistance) * 4.0;
        }
    }
}
