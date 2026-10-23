// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#ifndef APUSR_TOKEN_LAYOUT_HLSLI
#define APUSR_TOKEN_LAYOUT_HLSLI

#define APUSR_TOKEN_BYTES 256u
#define APUSR_FSR4_MIDDLE_CHANNELS 16u
#define APUSR_C16_PIXEL_BYTES APUSR_FSR4_MIDDLE_CHANNELS
#define APUSR_C16_PIXELS_PER_TOKEN (APUSR_TOKEN_BYTES / APUSR_C16_PIXEL_BYTES)
#define APUSR_TOKEN_LAYOUT_RASTER_NHWC_C16 0u
#define APUSR_TOKEN_LAYOUT_PACKAGE_MAJOR_C16 1u
#define APUSR_TOKEN_LAYOUT_LANE_MAJOR_PACKAGE_C16 2u
#define APUSR_TOKEN_LAYOUT_LANE_MAJOR_CELL_C16 3u
#define APUSR_TOKEN_LAYOUT_ZERO_GUARDED_RASTER_NHWC_C16 4u
#define APUSR_TOKEN_LAYOUT_CELL_M4X8_C16 5u
#define APUSR_FSR4_CELL_WIDTH 16u
#define APUSR_FSR4_CELL_ROWS 8u
#define APUSR_FSR4_CELL_BYTES (APUSR_FSR4_CELL_WIDTH * APUSR_FSR4_CELL_ROWS * APUSR_C16_PIXEL_BYTES)
#define APUSR_FSR4_PACKAGE_WIDTH 64u
#define APUSR_FSR4_PACKAGE_ROWS 32u
#define APUSR_FSR4_PACKAGE_BYTES (APUSR_FSR4_PACKAGE_WIDTH * APUSR_FSR4_PACKAGE_ROWS * APUSR_C16_PIXEL_BYTES)

#define APUSR_FSR4_PRESET_NATIVEAA 0u
#define APUSR_FSR4_PRESET_QUALITY 1u
#define APUSR_FSR4_PRESET_BALANCED 2u
#define APUSR_FSR4_PRESET_PERFORMANCE 3u
#define APUSR_FSR4_PRESET_DRS 4u
#define APUSR_FSR4_PRESET_ULTRAPERF 5u

#define APUSR_FLAG_HIGH_DYNAMIC_RANGE (1u << 0u)
#define APUSR_FLAG_DISPLAY_RESOLUTION_MOTION_VECTORS (1u << 1u)
#define APUSR_FLAG_MOTION_VECTOR_JITTER_CANCELLATION (1u << 2u)
#define APUSR_FLAG_DEPTH_INVERTED (1u << 3u)
#define APUSR_FLAG_DEPTH_INFINITE (1u << 4u)
#define APUSR_FLAG_AUTO_EXPOSURE (1u << 5u)
#define APUSR_FLAG_DYNAMIC_RESOLUTION (1u << 6u)
#define APUSR_FLAG_NON_LINEAR_COLORSPACE (1u << 8u)
#define APUSR_FLAG_COLORSPACE_SRGB (1u << 9u)
#define APUSR_FLAG_COLORSPACE_PQ (1u << 10u)

struct ApusrTokenBandConstants
{
    uint renderWidth;
    uint renderHeight;
    uint displayWidth;
    uint displayHeight;
    uint tokenLogicalWidth;
    uint tokenPaddedWidth;
    uint tokenHeight;
    uint tokenRowPitchBytes;
    uint bandIndex;
    uint bandCount;
    uint inputFirstRow;
    uint inputRowCount;
    uint outputFirstRow;
    uint outputRowCount;
    uint tokenByteOffsetLow;
    uint tokenByteOffsetHigh;
    float jitterX;
    float jitterY;
    float previousJitterX;
    float previousJitterY;
    float motionVectorScaleX;
    float motionVectorScaleY;
    float renderToDisplayScaleX;
    float renderToDisplayScaleY;
    float displayToRenderScaleX;
    float displayToRenderScaleY;
    float motionVectorJitterCancellationX;
    float motionVectorJitterCancellationY;
    float preExposure;
    float sharpness;
    float frameTimeDeltaMs;
    float viewSpaceToMetersFactor;
    uint featureFlags;
    uint resetHistory;
    uint sharpeningRequested;
    uint hasExposure;
    uint modelPreset;
    uint egressFirstRow;
    uint egressRowCount;
    uint hasReactiveMask;
    uint hasTransparencyMask;
    uint tokenLayout;
    uint tokenTileColumns;
    uint tokenTileRows;
    uint tokenLaneGroups;
};

// ByteAddressBuffer offsets are 32-bit. The DX12 recorder rejects token
// ranges that would require tokenByteOffsetHigh.
uint ApusrRasterTokenOffsetBytes(uint tokenByteOffsetLow, uint pixelX, uint localRow, uint channel, uint rowPitchBytes)
{
    return tokenByteOffsetLow + (localRow * rowPitchBytes) + (pixelX * APUSR_C16_PIXEL_BYTES) + channel;
}

uint ApusrPackageMajorTokenOffsetBytes(uint tokenByteOffsetLow, uint pixelX, uint absoluteRow, uint channel, uint packageColumns)
{
    const uint packageX = pixelX / APUSR_FSR4_PACKAGE_WIDTH;
    const uint packageY = absoluteRow / APUSR_FSR4_PACKAGE_ROWS;
    const uint localX = pixelX - (packageX * APUSR_FSR4_PACKAGE_WIDTH);
    const uint localY = absoluteRow - (packageY * APUSR_FSR4_PACKAGE_ROWS);
    const uint packageIndex = packageY * packageColumns + packageX;
    const uint packageByteOffset = packageIndex * APUSR_FSR4_PACKAGE_BYTES;
    const uint localByteOffset = ((localY * APUSR_FSR4_PACKAGE_WIDTH) + localX) * APUSR_C16_PIXEL_BYTES;
    return tokenByteOffsetLow + packageByteOffset + localByteOffset + channel;
}

uint ApusrLaneStartPackageColumn(uint laneIndex, uint packageColumns, uint laneGroups)
{
    const uint safeLaneGroups = max(laneGroups, 1u);
    const uint baseColumns = packageColumns / safeLaneGroups;
    const uint extraColumns = packageColumns - (baseColumns * safeLaneGroups);
    return laneIndex * baseColumns + min(laneIndex, extraColumns);
}

uint ApusrLanePackageColumnCount(uint laneIndex, uint packageColumns, uint laneGroups)
{
    const uint safeLaneGroups = max(laneGroups, 1u);
    const uint baseColumns = packageColumns / safeLaneGroups;
    const uint extraColumns = packageColumns - (baseColumns * safeLaneGroups);
    return baseColumns + ((laneIndex < extraColumns) ? 1u : 0u);
}

uint ApusrLaneMajorPackageTokenOffsetBytes(
    uint tokenByteOffsetLow,
    uint pixelX,
    uint absoluteRow,
    uint channel,
    uint packageColumns,
    uint packageRows,
    uint laneGroups)
{
    const uint packageX = pixelX / APUSR_FSR4_PACKAGE_WIDTH;
    const uint packageY = absoluteRow / APUSR_FSR4_PACKAGE_ROWS;
    const uint localX = pixelX - (packageX * APUSR_FSR4_PACKAGE_WIDTH);
    const uint localY = absoluteRow - (packageY * APUSR_FSR4_PACKAGE_ROWS);

    uint laneIndex = 0u;
    uint laneStartColumn = 0u;
    uint laneColumnCount = packageColumns;
    const uint safeLaneGroups = max(laneGroups, 1u);
    [unroll]
    for (uint candidateLane = 0u; candidateLane < 8u; ++candidateLane) {
        if (candidateLane >= safeLaneGroups) {
            break;
        }
        const uint candidateStart = ApusrLaneStartPackageColumn(candidateLane, packageColumns, safeLaneGroups);
        const uint candidateColumns = ApusrLanePackageColumnCount(candidateLane, packageColumns, safeLaneGroups);
        if (packageX >= candidateStart && packageX < candidateStart + candidateColumns) {
            laneIndex = candidateLane;
            laneStartColumn = candidateStart;
            laneColumnCount = candidateColumns;
            break;
        }
    }

    const uint lanePrefixPackages = laneStartColumn * packageRows;
    const uint packageIndexInLane = packageY * laneColumnCount + (packageX - laneStartColumn);
    const uint packageByteOffset = (lanePrefixPackages + packageIndexInLane) * APUSR_FSR4_PACKAGE_BYTES;
    const uint localByteOffset = ((localY * APUSR_FSR4_PACKAGE_WIDTH) + localX) * APUSR_C16_PIXEL_BYTES;
    return tokenByteOffsetLow + packageByteOffset + localByteOffset + channel;
}


uint ApusrLaneStartCellColumn(uint laneIndex, uint cellColumns, uint laneGroups)
{
    const uint safeLaneGroups = max(laneGroups, 1u);
    const uint baseColumns = cellColumns / safeLaneGroups;
    const uint extraColumns = cellColumns - (baseColumns * safeLaneGroups);
    return laneIndex * baseColumns + min(laneIndex, extraColumns);
}

uint ApusrLaneCellColumnCount(uint laneIndex, uint cellColumns, uint laneGroups)
{
    const uint safeLaneGroups = max(laneGroups, 1u);
    const uint baseColumns = cellColumns / safeLaneGroups;
    const uint extraColumns = cellColumns - (baseColumns * safeLaneGroups);
    return baseColumns + ((laneIndex < extraColumns) ? 1u : 0u);
}

uint ApusrLaneMajorCellTokenOffsetBytes(
    uint tokenByteOffsetLow,
    uint pixelX,
    uint absoluteRow,
    uint channel,
    uint cellColumns,
    uint cellRows,
    uint laneGroups)
{
    const uint cellX = pixelX / APUSR_FSR4_CELL_WIDTH;
    const uint cellY = absoluteRow / APUSR_FSR4_CELL_ROWS;
    const uint localX = pixelX - (cellX * APUSR_FSR4_CELL_WIDTH);
    const uint localY = absoluteRow - (cellY * APUSR_FSR4_CELL_ROWS);

    uint laneIndex = 0u;
    uint laneStartColumn = 0u;
    uint laneColumnCount = cellColumns;
    const uint safeLaneGroups = max(laneGroups, 1u);
    [unroll]
    for (uint candidateLane = 0u; candidateLane < 8u; ++candidateLane) {
        if (candidateLane >= safeLaneGroups) {
            break;
        }
        const uint candidateStart = ApusrLaneStartCellColumn(candidateLane, cellColumns, safeLaneGroups);
        const uint candidateColumns = ApusrLaneCellColumnCount(candidateLane, cellColumns, safeLaneGroups);
        if (cellX >= candidateStart && cellX < candidateStart + candidateColumns) {
            laneIndex = candidateLane;
            laneStartColumn = candidateStart;
            laneColumnCount = candidateColumns;
            break;
        }
    }

    const uint lanePrefixCells = laneStartColumn * cellRows;
    const uint cellIndexInLane = cellY * laneColumnCount + (cellX - laneStartColumn);
    const uint cellByteOffset = (lanePrefixCells + cellIndexInLane) * APUSR_FSR4_CELL_BYTES;
    const uint localByteOffset = ((localY * APUSR_FSR4_CELL_WIDTH) + localX) * APUSR_C16_PIXEL_BYTES;
    return tokenByteOffsetLow + cellByteOffset + localByteOffset + channel;
}


uint ApusrCellM4x8C16TokenOffsetBytes(
    uint tokenByteOffsetLow,
    uint pixelX,
    uint absoluteRow,
    uint channel,
    uint cellColumns)
{
    const uint cellX = pixelX / APUSR_FSR4_CELL_WIDTH;
    const uint cellY = absoluteRow / APUSR_FSR4_CELL_ROWS;
    const uint localX = pixelX - (cellX * APUSR_FSR4_CELL_WIDTH);
    const uint localY = absoluteRow - (cellY * APUSR_FSR4_CELL_ROWS);
    const uint pixelGroup = localX >> 2u;
    const uint pixelLane = localX & 3u;
    const uint channelChunk = channel >> 3u;
    const uint channelLane = channel & 7u;

    const uint cellIndex = cellY * cellColumns + cellX;
    const uint m4x8BlockIndex = ((localY * (APUSR_FSR4_CELL_WIDTH / 4u)) + pixelGroup) * 2u + channelChunk;
    const uint localByteOffset = m4x8BlockIndex * 32u + pixelLane * 8u + channelLane;
    return tokenByteOffsetLow + cellIndex * APUSR_FSR4_CELL_BYTES + localByteOffset;
}

uint ApusrTokenOffsetBytes(
    uint tokenLayout,
    uint tokenByteOffsetLow,
    uint pixelX,
    uint absoluteRow,
    uint localRow,
    uint channel,
    uint rowPitchBytes,
    uint tileColumns,
    uint tileRows,
    uint laneGroups)
{
    if (tokenLayout == APUSR_TOKEN_LAYOUT_LANE_MAJOR_CELL_C16) {
        return ApusrLaneMajorCellTokenOffsetBytes(
            tokenByteOffsetLow, pixelX, absoluteRow, channel, tileColumns, tileRows, laneGroups);
    }
    if (tokenLayout == APUSR_TOKEN_LAYOUT_CELL_M4X8_C16) {
        return ApusrCellM4x8C16TokenOffsetBytes(
            tokenByteOffsetLow, pixelX, absoluteRow, channel, tileColumns);
    }
    if (tokenLayout == APUSR_TOKEN_LAYOUT_LANE_MAJOR_PACKAGE_C16) {
        return ApusrLaneMajorPackageTokenOffsetBytes(
            tokenByteOffsetLow, pixelX, absoluteRow, channel, tileColumns, tileRows, laneGroups);
    }
    if (tokenLayout == APUSR_TOKEN_LAYOUT_PACKAGE_MAJOR_C16) {
        return ApusrPackageMajorTokenOffsetBytes(tokenByteOffsetLow, pixelX, absoluteRow, channel, tileColumns);
    }
    return ApusrRasterTokenOffsetBytes(tokenByteOffsetLow, pixelX, localRow, channel, rowPitchBytes);
}

uint ApusrPackSignedInt8Proxy(float value)
{
    const int signedValue = (int)clamp(round(value * 127.0f), -128.0f, 127.0f);
    return (uint)(signedValue & 0xff);
}

uint ApusrPackFourInt8(uint channel0, uint channel1, uint channel2, uint channel3)
{
    return (channel0 & 0xffu) |
        ((channel1 & 0xffu) << 8u) |
        ((channel2 & 0xffu) << 16u) |
        ((channel3 & 0xffu) << 24u);
}

int ApusrUnpackSignedInt8Raw(uint packed, uint byteIndex)
{
    int signedValue = (int)((packed >> (byteIndex * 8u)) & 0xffu);
    if (signedValue >= 128) {
        signedValue -= 256;
    }
    return signedValue;
}

float ApusrUnpackSignedInt8Proxy(uint packed, uint byteIndex)
{
    return (float)ApusrUnpackSignedInt8Raw(packed, byteIndex) / 127.0f;
}

float ApusrUnpackDiagnostic3x3Byte(uint packed, uint byteIndex)
{
    const uint value = (packed >> (byteIndex * 8u)) & 0xffu;
    return saturate((float)value / 63.0f);
}

#endif // APUSR_TOKEN_LAYOUT_HLSLI
