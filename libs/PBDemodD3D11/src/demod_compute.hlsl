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
};

Texture2D<float4> RoiTexture : register(t0);
StructuredBuffer<float4> Calibration : register(t1);
RWStructuredBuffer<float4> CalibrationOutput : register(u0);
RWStructuredBuffer<float> MetricOutput : register(u1);

static const uint ShapeMasks[16] =
{
    0x00FF, 0xFF00, 0x3333, 0xCCCC, 0x0FF0, 0xF00F, 0x6666, 0x9999,
    0x7331, 0x8CCE, 0x1337, 0xECC8, 0x8C73, 0x738C, 0x13EC, 0xEC13
};
static const uint ChromaLabels[4] = {0, 1, 3, 2};
static const uint LevelLabels[4] = {0, 1, 3, 2};
static const uint2 PilotOrigins[4] = {uint2(736, 16), uint2(1696, 16), uint2(96, 1000), uint2(1056, 1000)};
static const uint BandHeights[7] = {64, 128, 188, 128, 188, 128, 64};

float3 LoadBgr(uint2 position)
{
    return RoiTexture.Load(int3(position, 0)).rgb * 255.0;
}

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

uint ToLogical(uint physical)
{
    const uint shift = InterleavePhase * RowTiles;
    const uint unshifted = (physical + TileCount - shift) % TileCount;
    uint result = 0;
    uint value = unshifted;
    [unroll]
    for (uint multiplierBit = 0; multiplierBit < 15; multiplierBit++)
    {
        if ((29825 & (1u << multiplierBit)) != 0)
        {
            result = result >= TileCount - value ? result - (TileCount - value) : result + value;
        }
        value = value >= TileCount - value ? value - (TileCount - value) : value + value;
    }
    return result;
}

uint2 TileOrigin(uint physical)
{
    uint y = 96;
    for (uint band = 0; band < 7; band++)
    {
        const bool hasTiming = (band & 1) != 0;
        const uint tilesPerRow = (hasTiming ? 1344 : 1728) / TilePixels;
        const uint count = tilesPerRow * (BandHeights[band] / TilePixels);
        if (physical < count)
        {
            const uint column = physical % tilesPerRow;
            uint x = 96 + column * TilePixels;
            if (hasTiming)
            {
                x += column < 672 / TilePixels ? 128 : 256;
            }
            return uint2(x, y + (physical / tilesPerRow) * TilePixels);
        }
        physical -= count;
        y += BandHeights[band];
    }
    return uint2(0, 0);
}

[numthreads(16, 1, 1)]
void CalibrateChromaCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint entry = dispatchThreadId.x;
    if (entry >= 16)
    {
        return;
    }
    const uint pilot = entry / 4;
    const uint state = entry % 4;
    float2 sum = 0.0;
    float2 squares = 0.0;
    uint clipped = 0;
    [loop]
    for (uint row = 0; row < 64; row++)
    {
        [loop]
        for (uint column = 0; column < 32; column++)
        {
            const float3 sample = LoadBgr(PilotOrigins[pilot] + uint2(state * 32 + column, row));
            const float2 opponent = Opponent(sample);
            sum += opponent;
            squares += opponent * opponent;
            clipped += IsClipped(sample) ? 1 : 0;
        }
    }
    const float count = 2048.0;
    const float2 mean = sum / count;
    const float2 variance = max(0.0, squares / count - mean * mean);
    CalibrationOutput[entry] = float4(mean, variance.x + variance.y, (float)clipped);
}

[numthreads(16, 1, 1)]
void CalibrateLevelsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint entry = dispatchThreadId.x;
    if (entry >= 16)
    {
        return;
    }
    const uint pilot = entry / 4;
    const uint level = entry % 4;
    float sum = 0.0;
    float squares = 0.0;
    uint clipped = 0;
    [loop]
    for (uint row = 0; row < 64; row++)
    {
        [loop]
        for (uint column = 0; column < 32; column++)
        {
            const float3 sample = LoadBgr(PilotOrigins[pilot] + uint2(level * 32 + column, row));
            const float value = Luma(sample);
            sum += value;
            squares += value * value;
            clipped += IsClipped(sample) ? 1 : 0;
        }
    }
    const float count = 2048.0;
    const float mean = sum / count;
    CalibrationOutput[entry] = float4(mean, max(0.0, squares / count - mean * mean), (float)clipped, 0.0);
}

[numthreads(64, 1, 1)]
void DemodShapeChromaCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint physical = dispatchThreadId.x;
    if (physical >= TileCount)
    {
        return;
    }
    const uint2 origin = TileOrigin(physical);
    float3 samples[16];
    float luma[16];
    float meanLuma = 0.0;
    float2 meanOpponent = 0.0;
    bool clipped = false;
    [unroll]
    for (uint samplePixel = 0; samplePixel < 16; samplePixel++)
    {
        samples[samplePixel] = LoadBgr(origin + uint2(samplePixel & 3, samplePixel >> 2));
        luma[samplePixel] = Luma(samples[samplePixel]);
        meanLuma += luma[samplePixel] / 16.0;
        meanOpponent += Opponent(samples[samplePixel]) / 16.0;
        clipped = clipped || IsClipped(samples[samplePixel]);
    }
    float energy = 0.0;
    [unroll]
    for (uint centeredPixel = 0; centeredPixel < 16; centeredPixel++)
    {
        luma[centeredPixel] -= meanLuma;
        energy += luma[centeredPixel] * luma[centeredPixel];
    }
    float distances[16];
    float amplitudes[16];
    [unroll]
    for (uint candidateShape = 0; candidateShape < 16; candidateShape++)
    {
        float correlation = 0.0;
        [unroll]
        for (uint correlationPixel = 0; correlationPixel < 16; correlationPixel++)
        {
            correlation += luma[correlationPixel] * (((ShapeMasks[candidateShape] >> correlationPixel) & 1) != 0 ? 1.0 : -1.0);
        }
        const float amplitude = max(0.0, correlation / 16.0);
        amplitudes[candidateShape] = amplitude;
        float residual = 0.0;
        [unroll]
        for (uint residualPixel = 0; residualPixel < 16; residualPixel++)
        {
            const float expected = ((ShapeMasks[candidateShape] >> residualPixel) & 1) != 0 ? amplitude : -amplitude;
            const float difference = luma[residualPixel] - expected;
            residual += difference * difference;
        }
        distances[candidateShape] = residual / max(energy, 1.0);
    }
    uint bestShape = 0;
    [unroll]
    for (uint searchShape = 1; searchShape < 16; searchShape++)
    {
        bestShape = distances[searchShape] < distances[bestShape] ? searchShape : bestShape;
    }
    const bool shapeUnreliable = clipped || amplitudes[bestShape] < 24.0 || distances[bestShape] > 0.40;
    const uint logical = ToLogical(physical);
    const uint firstMetric = logical * 6;
    [unroll]
    for (uint shapeBit = 0; shapeBit < 4; shapeBit++)
    {
        float zeroDistance = 3.402823466e+38F;
        float oneDistance = zeroDistance;
        [unroll]
        for (uint labeledShape = 0; labeledShape < 16; labeledShape++)
        {
            if ((labeledShape & (1 << shapeBit)) != 0)
            {
                oneDistance = min(oneDistance, distances[labeledShape]);
            }
            else
            {
                zeroDistance = min(zeroDistance, distances[labeledShape]);
            }
        }
        MetricOutput[firstMetric + shapeBit] = shapeUnreliable ? 0.0 : oneDistance - zeroDistance;
    }

    float chromaDistances[4];
    float2 chromaCentroids[4];
    [unroll]
    for (uint centroidState = 0; centroidState < 4; centroidState++)
    {
        chromaCentroids[centroidState] = 0.0;
        [unroll]
        for (uint centroidPilot = 0; centroidPilot < 4; centroidPilot++)
        {
            chromaCentroids[centroidState] += Calibration[centroidPilot * 4 + centroidState].xy / 4.0;
        }
    }
    float minimumSeparationSquared = 3.402823466e+38F;
    [unroll]
    for (uint separationState = 0; separationState < 4; separationState++)
    {
        [unroll]
        for (uint otherState = 0; otherState < separationState; otherState++)
        {
            const float2 separation = chromaCentroids[separationState] - chromaCentroids[otherState];
            minimumSeparationSquared = min(minimumSeparationSquared, dot(separation, separation));
        }
    }
    [unroll]
    for (uint distanceState = 0; distanceState < 4; distanceState++)
    {
        const float2 difference = meanOpponent - chromaCentroids[distanceState];
        chromaDistances[distanceState] = dot(difference, difference) / minimumSeparationSquared;
    }
    float minimumDistance = chromaDistances[0];
    [unroll]
    for (uint minimumState = 1; minimumState < 4; minimumState++)
    {
        minimumDistance = min(minimumDistance, chromaDistances[minimumState]);
    }
    const bool chromaUnreliable = clipped || sqrt(minimumDistance) > 1.5;
    [unroll]
    for (uint chromaBit = 0; chromaBit < 2; chromaBit++)
    {
        float zeroDistance = 3.402823466e+38F;
        float oneDistance = zeroDistance;
        [unroll]
        for (uint labeledState = 0; labeledState < 4; labeledState++)
        {
            if ((ChromaLabels[labeledState] & (1 << chromaBit)) != 0)
            {
                oneDistance = min(oneDistance, chromaDistances[labeledState]);
            }
            else
            {
                zeroDistance = min(zeroDistance, chromaDistances[labeledState]);
            }
        }
        MetricOutput[firstMetric + 4 + chromaBit] = chromaUnreliable ? 0.0 : oneDistance - zeroDistance;
    }
}

[numthreads(64, 1, 1)]
void DemodDesktopLevelsCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint physical = dispatchThreadId.x;
    if (physical >= TileCount)
    {
        return;
    }
    const uint2 origin = TileOrigin(physical);
    float sum = 0.0;
    float squares = 0.0;
    bool clipped = false;
    [loop]
    for (uint row = 0; row < TilePixels; row++)
    {
        [loop]
        for (uint column = 0; column < TilePixels; column++)
        {
            const float3 sample = LoadBgr(origin + uint2(column, row));
            const float value = Luma(sample);
            sum += value;
            squares += value * value;
            clipped = clipped || IsClipped(sample);
        }
    }
    const float count = (float)(TilePixels * TilePixels);
    const float mean = sum / count;
    const float variance = max(0.0, squares / count - mean * mean);
    float distances[4];
    float levelCentroids[4];
    [unroll]
    for (uint centroidLevel = 0; centroidLevel < 4; centroidLevel++)
    {
        levelCentroids[centroidLevel] = 0.0;
        [unroll]
        for (uint centroidPilot = 0; centroidPilot < 4; centroidPilot++)
        {
            levelCentroids[centroidLevel] += Calibration[centroidPilot * 4 + centroidLevel].x / 4.0;
        }
    }
    float minimumGap = 255.0;
    [unroll]
    for (uint distanceLevel = 0; distanceLevel < 4; distanceLevel++)
    {
        const float difference = mean - levelCentroids[distanceLevel];
        distances[distanceLevel] = difference * difference;
        if (distanceLevel != 0)
        {
            minimumGap = min(minimumGap, levelCentroids[distanceLevel] - levelCentroids[distanceLevel - 1]);
        }
    }
    const float gapSquared = minimumGap * minimumGap;
    const bool unreliable = clipped || variance > gapSquared / 16.0;
    const uint firstMetric = ToLogical(physical) * 2;
    [unroll]
    for (uint levelBit = 0; levelBit < 2; levelBit++)
    {
        float zeroDistance = 3.402823466e+38F;
        float oneDistance = zeroDistance;
        [unroll]
        for (uint labeledLevel = 0; labeledLevel < 4; labeledLevel++)
        {
            if ((LevelLabels[labeledLevel] & (1 << levelBit)) != 0)
            {
                oneDistance = min(oneDistance, distances[labeledLevel]);
            }
            else
            {
                zeroDistance = min(zeroDistance, distances[labeledLevel]);
            }
        }
        MetricOutput[firstMetric + levelBit] = unreliable ? 0.0 : (oneDistance - zeroDistance) / gapSquared;
    }
}
