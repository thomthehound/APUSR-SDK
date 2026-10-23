// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstddef>
#include <cstdint>

namespace apusr::kernels::middle_core_spatial {

inline constexpr std::uint32_t kSpatialAbiVersion = 10;

inline constexpr std::uint32_t kC16Channels = 16u;
inline constexpr std::uint32_t kC32Channels = 32u;
inline constexpr std::uint32_t kC64Channels = 64u;
inline constexpr std::uint32_t kC8Channels = 8u;
inline constexpr std::uint32_t kElementBytes = 1u;
inline constexpr std::size_t kC16PixelBytes = kC16Channels * kElementBytes;

inline constexpr std::uint32_t kOwnedCellWidth = 16u;
inline constexpr std::uint32_t kOwnedCellRows = 8u;
inline constexpr std::uint32_t kHaloGuardPixels = 1u;
inline constexpr std::uint32_t kHaloCellWidth = kOwnedCellWidth + 2u * kHaloGuardPixels;
inline constexpr std::uint32_t kHaloCellRows = kOwnedCellRows + 2u * kHaloGuardPixels;

inline constexpr std::size_t kC16CellBytes = kOwnedCellWidth * kOwnedCellRows * kC16Channels * kElementBytes;
inline constexpr std::size_t kC16RowSeamBytes = kOwnedCellRows * kC16Channels * kElementBytes;
inline constexpr std::size_t kC16HaloCellBytes = kHaloCellWidth * kHaloCellRows * kC16Channels * kElementBytes;
inline constexpr std::size_t kC32CellBytes = kOwnedCellWidth * kOwnedCellRows * kC32Channels * kElementBytes;
inline constexpr std::size_t kC32HaloCellBytes = kHaloCellWidth * kHaloCellRows * kC32Channels * kElementBytes;
inline constexpr std::size_t kC64CellBytes = kOwnedCellWidth * kOwnedCellRows * kC64Channels * kElementBytes;
inline constexpr std::size_t kC64HaloCellBytes = kHaloCellWidth * kHaloCellRows * kC64Channels * kElementBytes;
inline constexpr std::size_t kC8QuadrantCellBytes = kOwnedCellWidth * kOwnedCellRows * kC8Channels * kElementBytes;
inline constexpr std::size_t kC8HaloChunkBytes = kHaloCellWidth * kHaloCellRows * kC8Channels * kElementBytes;

// Reference package dimensions describe the FSR4 model metadata only. The
// active front ingress is a row-major C16 frame with DMA tensor halo views.
inline constexpr std::uint32_t kReferencePackageWidth = 64u;
inline constexpr std::uint32_t kReferencePackageRows = 32u;
inline constexpr std::uint32_t kCapacityAlignmentWidth = kOwnedCellWidth;
inline constexpr std::uint32_t kCapacityAlignmentRows = kOwnedCellRows;
inline constexpr std::uint32_t kDownsampleLadderCellRowAlignment = kOwnedCellRows * 4u;

inline constexpr std::uint32_t kSpatial2ColLaneGroupColumns = 2u;
inline constexpr std::uint32_t kSpatial4ColLaneGroupColumns = 4u;
inline constexpr std::uint32_t kComputeRows = 4u;
inline constexpr std::uint32_t kFrontC16HaloPatchFifoDepth = 1u;
inline constexpr std::uint32_t kFrontIngressGuardPixels = 1u;
inline constexpr std::size_t kFrontC16ParameterSliceBytes = 4048u;
inline constexpr std::size_t kEncoderC32ParameterSliceBytes = 2576u;
inline constexpr std::size_t kResidentFrontEncoderParameterBytes = kFrontC16ParameterSliceBytes + kEncoderC32ParameterSliceBytes;
inline constexpr std::uint32_t kFrontC16EdgeLeft = 1u << 0u;
inline constexpr std::uint32_t kFrontC16EdgeRight = 1u << 1u;
inline constexpr std::uint32_t kFrontC16EdgeTop = 1u << 2u;
inline constexpr std::uint32_t kFrontC16EdgeBottom = 1u << 3u;
inline constexpr std::size_t kMemoryTileBytes = 512u * 1024u;
inline constexpr std::uint32_t kNpu1Columns = 4u;
inline constexpr std::uint32_t kNpu2Columns = 8u;

struct FrameCapacity {
    std::uint32_t modelOutputWidth = 0u;
    std::uint32_t modelOutputRows = 0u;
    std::uint32_t seamWidth = 0u;
    std::uint32_t seamRows = 0u;
    std::uint32_t cellColumns = 0u;
    std::uint32_t cellRows = 0u;
    std::uint32_t cellCount = 0u;
    std::size_t frameSlotBytes = 0u;
    std::size_t frontIngressSlotBytes = 0u;
};

struct LaneStripe {
    std::uint32_t laneIndex = 0u;
    std::uint32_t firstCellColumn = 0u;
    std::uint32_t cellColumns = 0u;
    std::uint32_t cellRows = 0u;
    std::uint32_t firstCellIndex = 0u;
    std::uint32_t cellCount = 0u;
    std::size_t frameSlotFirstByte = 0u;
    std::size_t semanticBytes = 0u;
};

struct ColumnShard {
    std::uint32_t laneIndex = 0u;
    std::uint32_t macroColumnIndex = 0u;
    std::uint32_t physicalColumn = 0u;
    std::uint32_t firstCellColumn = 0u;
    std::uint32_t cellColumns = 0u;
    std::uint32_t laneCellColumns = 0u;
    std::uint32_t cellRows = 0u;
    std::size_t firstByte = 0u;
    std::size_t laneRowStrideBytes = 0u;
    std::size_t rowBytes = 0u;
    std::size_t frontC16HaloPatchBytes = 0u;
    bool ownsFrameLeftEdge = false;
    bool ownsFrameRightEdge = false;
};

struct FrontC16HaloPatchPlan {
    ColumnShard shard{};
    std::uint32_t guardPixels = 0u;
    std::uint32_t patchWidth = 0u;
    std::uint32_t patchRows = 0u;
    std::size_t patchBytes = 0u;
    std::uint32_t fifoDepth = 0u;
    std::size_t activeFifoBytes = 0u;
    std::size_t frontIngressSlotBytes = 0u;
};

[[nodiscard]] constexpr std::uint32_t ceilHalf(const std::uint32_t value)
{
    return (value + 1u) / 2u;
}

[[nodiscard]] constexpr std::uint32_t alignUp(const std::uint32_t value, const std::uint32_t alignment)
{
    const std::uint32_t remainder = alignment == 0u ? 0u : value % alignment;
    return remainder == 0u ? value : value + (alignment - remainder);
}

[[nodiscard]] constexpr FrameCapacity frameCapacityForModelOutput(
    const std::uint32_t modelOutputWidth,
    const std::uint32_t modelOutputRows)
{
    const std::uint32_t seamWidth = alignUp(ceilHalf(modelOutputWidth), kCapacityAlignmentWidth);
    const std::uint32_t seamRows = alignUp(ceilHalf(modelOutputRows), kDownsampleLadderCellRowAlignment);
    const std::uint32_t cellColumns = seamWidth / kOwnedCellWidth;
    const std::uint32_t cellRows = seamRows / kOwnedCellRows;
    const std::uint32_t cellCount = cellColumns * cellRows;
    return FrameCapacity{
        .modelOutputWidth = seamWidth * 2u,
        .modelOutputRows = seamRows * 2u,
        .seamWidth = seamWidth,
        .seamRows = seamRows,
        .cellColumns = cellColumns,
        .cellRows = cellRows,
        .cellCount = cellCount,
        .frameSlotBytes = static_cast<std::size_t>(seamWidth) * seamRows * kC16PixelBytes,
        .frontIngressSlotBytes = static_cast<std::size_t>(seamWidth + 2u * kFrontIngressGuardPixels) *
            (seamRows + 2u * kFrontIngressGuardPixels) * kC16PixelBytes,
    };
}

inline constexpr FrameCapacity kR1080Capacity = frameCapacityForModelOutput(1920u, 1080u);
inline constexpr FrameCapacity kR2160Capacity = frameCapacityForModelOutput(3840u, 2160u);
inline constexpr FrameCapacity kR4320Capacity = frameCapacityForModelOutput(7680u, 4320u);

[[nodiscard]] constexpr std::uint32_t autoLaneGroups(
    const std::uint32_t deviceColumns,
    const std::uint32_t laneGroupColumns)
{
    return deviceColumns / laneGroupColumns;
}

[[nodiscard]] constexpr std::uint32_t splitSpanWidth(
    const std::uint32_t total,
    const std::uint32_t parts,
    const std::uint32_t index)
{
    const std::uint32_t base = parts == 0u ? 0u : total / parts;
    const std::uint32_t extra = parts == 0u ? 0u : total % parts;
    return base + (index < extra ? 1u : 0u);
}

[[nodiscard]] constexpr std::uint32_t splitSpanFirst(
    const std::uint32_t total,
    const std::uint32_t parts,
    const std::uint32_t index)
{
    const std::uint32_t base = parts == 0u ? 0u : total / parts;
    const std::uint32_t extra = parts == 0u ? 0u : total % parts;
    return index * base + (index < extra ? index : extra);
}

[[nodiscard]] constexpr LaneStripe laneStripeFor(
    const FrameCapacity capacity,
    const std::uint32_t laneGroups,
    const std::uint32_t laneIndex)
{
    const std::uint32_t baseCellColumns = capacity.cellColumns / laneGroups;
    const std::uint32_t extraCellColumns = capacity.cellColumns % laneGroups;
    const std::uint32_t cellColumns = baseCellColumns + (laneIndex < extraCellColumns ? 1u : 0u);
    const std::uint32_t firstCellColumn = laneIndex * baseCellColumns + (laneIndex < extraCellColumns ? laneIndex : extraCellColumns);
    const std::uint32_t firstCellIndex = firstCellColumn * capacity.cellRows;
    const std::uint32_t cellCount = cellColumns * capacity.cellRows;
    return LaneStripe{
        .laneIndex = laneIndex,
        .firstCellColumn = firstCellColumn,
        .cellColumns = cellColumns,
        .cellRows = capacity.cellRows,
        .firstCellIndex = firstCellIndex,
        .cellCount = cellCount,
        .frameSlotFirstByte = static_cast<std::size_t>(firstCellColumn) * kOwnedCellWidth * kC16PixelBytes,
        .semanticBytes = static_cast<std::size_t>(cellCount) * kC16CellBytes,
    };
}

[[nodiscard]] constexpr std::size_t frameCellByteOffset(
    const FrameCapacity capacity,
    const std::uint32_t /* laneGroups */,
    const std::uint32_t cellColumn,
    const std::uint32_t cellRow)
{
    const std::uint32_t pixelX = cellColumn * kOwnedCellWidth;
    const std::uint32_t pixelY = cellRow * kOwnedCellRows;
    return (static_cast<std::size_t>(pixelY) * capacity.seamWidth + pixelX) * kC16PixelBytes;
}

[[nodiscard]] constexpr ColumnShard columnShardFor(
    const FrameCapacity capacity,
    const std::uint32_t laneGroups,
    const std::uint32_t laneGroupColumns,
    const std::uint32_t laneIndex,
    const std::uint32_t macroColumnIndex)
{
    const LaneStripe stripe = laneStripeFor(capacity, laneGroups, laneIndex);
    const std::uint32_t localFirstColumn = splitSpanFirst(stripe.cellColumns, laneGroupColumns, macroColumnIndex);
    const std::uint32_t localColumns = splitSpanWidth(stripe.cellColumns, laneGroupColumns, macroColumnIndex);
    const std::uint32_t firstCellColumn = stripe.firstCellColumn + localFirstColumn;
    const std::uint32_t physicalColumn = laneIndex * laneGroupColumns + macroColumnIndex;
    return ColumnShard{
        .laneIndex = laneIndex,
        .macroColumnIndex = macroColumnIndex,
        .physicalColumn = physicalColumn,
        .firstCellColumn = firstCellColumn,
        .cellColumns = localColumns,
        .laneCellColumns = stripe.cellColumns,
        .cellRows = capacity.cellRows,
        .firstByte = frameCellByteOffset(capacity, laneGroups, firstCellColumn, 0u),
        .laneRowStrideBytes = static_cast<std::size_t>(capacity.seamWidth) * kOwnedCellRows * kC16PixelBytes,
        .rowBytes = static_cast<std::size_t>(localColumns) * kC16CellBytes,
        .frontC16HaloPatchBytes = kC16HaloCellBytes,
        .ownsFrameLeftEdge = firstCellColumn == 0u,
        .ownsFrameRightEdge = firstCellColumn + localColumns == capacity.cellColumns,
    };
}

[[nodiscard]] constexpr ColumnShard columnShardForPhysicalColumn(
    const FrameCapacity capacity,
    const std::uint32_t laneGroups,
    const std::uint32_t laneGroupColumns,
    const std::uint32_t physicalColumn)
{
    return columnShardFor(capacity, laneGroups, laneGroupColumns, physicalColumn / laneGroupColumns, physicalColumn % laneGroupColumns);
}

[[nodiscard]] constexpr bool frontC16CellRowIsTopEdge(const std::uint32_t cellRow)
{
    return cellRow == 0u;
}

[[nodiscard]] constexpr bool frontC16CellRowIsBottomEdge(
    const FrameCapacity capacity,
    const std::uint32_t cellRow)
{
    return cellRow + 1u == capacity.cellRows;
}

[[nodiscard]] constexpr std::uint32_t frontC16FrameEdgeFlags(
    const FrameCapacity capacity,
    const ColumnShard shard,
    const std::uint32_t cellRow)
{
    return (shard.ownsFrameLeftEdge ? kFrontC16EdgeLeft : 0u) |
        (shard.ownsFrameRightEdge ? kFrontC16EdgeRight : 0u) |
        (frontC16CellRowIsTopEdge(cellRow) ? kFrontC16EdgeTop : 0u) |
        (frontC16CellRowIsBottomEdge(capacity, cellRow) ? kFrontC16EdgeBottom : 0u);
}

[[nodiscard]] constexpr FrontC16HaloPatchPlan frontC16HaloPatchPlanForShard(
    const FrameCapacity capacity,
    const ColumnShard shard)
{
    return FrontC16HaloPatchPlan{
        .shard = shard,
        .guardPixels = kHaloGuardPixels,
        .patchWidth = kHaloCellWidth,
        .patchRows = kHaloCellRows,
        .patchBytes = kC16HaloCellBytes,
        .fifoDepth = kFrontC16HaloPatchFifoDepth,
        .activeFifoBytes = kC16HaloCellBytes * kFrontC16HaloPatchFifoDepth,
        .frontIngressSlotBytes = capacity.frontIngressSlotBytes,
    };
}

static_assert(kSpatialAbiVersion == 10u);
static_assert(kFrontIngressGuardPixels == 1u);
static_assert(kC16PixelBytes == 16u);
static_assert(kC16CellBytes == 2048u);
static_assert(kC16RowSeamBytes == 128u);
static_assert(kC16HaloCellBytes == 2880u);
static_assert(kC32CellBytes == 4096u);
static_assert(kC32HaloCellBytes == 5760u);
static_assert(kC64CellBytes == 8192u);
static_assert(kC64HaloCellBytes == 11520u);
static_assert(kC8QuadrantCellBytes == 1024u);
static_assert(kC8HaloChunkBytes == 1440u);
static_assert(kMemoryTileBytes == 524288u);
static_assert(kR1080Capacity.frameSlotBytes == 8355840u);
static_assert(kR1080Capacity.frontIngressSlotBytes == 8404032u);
static_assert(kR2160Capacity.frameSlotBytes == 33423360u);
static_assert(kR2160Capacity.frontIngressSlotBytes == 33519680u);
static_assert(kR4320Capacity.frameSlotBytes == 133693440u);
static_assert(kR4320Capacity.frontIngressSlotBytes == 133886016u);
static_assert(autoLaneGroups(kNpu2Columns, kSpatial2ColLaneGroupColumns) == 4u);
static_assert(autoLaneGroups(kNpu2Columns, kSpatial4ColLaneGroupColumns) == 2u);
static_assert(frameCellByteOffset(kR2160Capacity, 4u, 0u, 0u) == 0u);
static_assert(frameCellByteOffset(kR2160Capacity, 4u, 1u, 0u) == 256u);
static_assert(frameCellByteOffset(kR2160Capacity, 4u, 0u, 1u) == 245760u);
static_assert(frameCellByteOffset(kR2160Capacity, 4u, 30u, 0u) == 7680u);
static_assert(columnShardForPhysicalColumn(kR2160Capacity, 4u, kSpatial2ColLaneGroupColumns, 0u).cellColumns == 15u);
static_assert(columnShardForPhysicalColumn(kR2160Capacity, 4u, kSpatial2ColLaneGroupColumns, 0u).firstByte == 0u);
static_assert(columnShardForPhysicalColumn(kR2160Capacity, 4u, kSpatial2ColLaneGroupColumns, 1u).firstByte == 3840u);
static_assert(frontC16HaloPatchPlanForShard(kR2160Capacity, columnShardForPhysicalColumn(kR2160Capacity, 4u, kSpatial2ColLaneGroupColumns, 0u)).patchBytes == 2880u);
static_assert((frontC16FrameEdgeFlags(kR2160Capacity, columnShardForPhysicalColumn(kR2160Capacity, 4u, kSpatial2ColLaneGroupColumns, 0u), 0u) & kFrontC16EdgeTop) != 0u);
static_assert((frontC16FrameEdgeFlags(kR2160Capacity, columnShardForPhysicalColumn(kR2160Capacity, 4u, kSpatial2ColLaneGroupColumns, 0u), kR2160Capacity.cellRows - 1u) & kFrontC16EdgeBottom) != 0u);

} // namespace apusr::kernels::middle_core_spatial
