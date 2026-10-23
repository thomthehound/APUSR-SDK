// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr/kernels/apusr_middle_core_spatial_abi.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using namespace apusr::kernels::middle_core_spatial;

    static_assert(kSpatialAbiVersion == 10u);
    static_assert(kOwnedCellWidth == 16u);
    static_assert(kOwnedCellRows == 8u);
    static_assert(kHaloCellWidth == 18u);
    static_assert(kHaloCellRows == 10u);
    static_assert(kC16PixelBytes == 16u);
    static_assert(kC16CellBytes == 2048u);
    static_assert(kC16HaloCellBytes == 2880u);

    require(kR2160Capacity.modelOutputWidth == 3840u, "R2160 width should remain a capacity class");
    require(kR2160Capacity.modelOutputRows == 2176u, "R2160 padded rows should cover full downsample ladder cells");
    require(kR2160Capacity.seamWidth == 1920u, "R2160 seam width changed");
    require(kR2160Capacity.seamRows == 1088u, "R2160 seam rows changed");
    require(kR2160Capacity.cellColumns == 120u, "R2160 C16 cell columns changed");
    require(kR2160Capacity.cellRows == 136u, "R2160 C16 cell rows changed");
    require(kR2160Capacity.cellCount == 16320u, "R2160 cell count changed");
    require(kR2160Capacity.frameSlotBytes == 33423360u, "R2160 frame slot bytes changed");
    require(kR2160Capacity.frontIngressSlotBytes == 33519680u, "R2160 zero-guarded ingress bytes changed");

    constexpr std::uint32_t spatial2Npu2Lanes = autoLaneGroups(kNpu2Columns, kSpatial2ColLaneGroupColumns);
    constexpr std::uint32_t spatial4Npu2Lanes = autoLaneGroups(kNpu2Columns, kSpatial4ColLaneGroupColumns);
    static_assert(spatial2Npu2Lanes == 4u);
    static_assert(spatial4Npu2Lanes == 2u);

    constexpr LaneStripe twoColLane0 = laneStripeFor(kR2160Capacity, spatial2Npu2Lanes, 0u);
    constexpr LaneStripe twoColLane1 = laneStripeFor(kR2160Capacity, spatial2Npu2Lanes, 1u);
    constexpr LaneStripe twoColLane3 = laneStripeFor(kR2160Capacity, spatial2Npu2Lanes, 3u);
    constexpr ColumnShard twoColShard0 = columnShardForPhysicalColumn(kR2160Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 0u);
    constexpr ColumnShard twoColShard1 = columnShardForPhysicalColumn(kR2160Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 1u);
    constexpr ColumnShard twoColShard7 = columnShardForPhysicalColumn(kR2160Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 7u);

    static_assert(twoColLane0.firstCellColumn == 0u);
    static_assert(twoColLane0.cellColumns == 30u);
    static_assert(twoColLane0.cellCount == 4080u);
    static_assert(twoColLane1.firstCellColumn == 30u);
    static_assert(twoColLane3.firstCellColumn == 90u);
    static_assert(twoColLane3.cellColumns == 30u);
    static_assert(twoColShard0.firstCellColumn == 0u);
    static_assert(twoColShard0.cellColumns == 15u);
    static_assert(twoColShard0.frontC16HaloPatchBytes == kC16HaloCellBytes);
    static_assert(twoColShard0.ownsFrameLeftEdge);
    static_assert(!twoColShard0.ownsFrameRightEdge);
    static_assert(twoColShard1.firstCellColumn == 15u);
    static_assert(twoColShard1.firstByte == 15u * kOwnedCellWidth * kC16PixelBytes);
    static_assert(twoColShard7.firstCellColumn == 105u);
    static_assert(twoColShard7.ownsFrameRightEdge);

    constexpr LaneStripe fourColLane0 = laneStripeFor(kR2160Capacity, spatial4Npu2Lanes, 0u);
    constexpr LaneStripe fourColLane1 = laneStripeFor(kR2160Capacity, spatial4Npu2Lanes, 1u);
    constexpr ColumnShard fourColShard0 = columnShardForPhysicalColumn(kR2160Capacity, spatial4Npu2Lanes, kSpatial4ColLaneGroupColumns, 0u);
    constexpr ColumnShard fourColShard3 = columnShardForPhysicalColumn(kR2160Capacity, spatial4Npu2Lanes, kSpatial4ColLaneGroupColumns, 3u);
    constexpr ColumnShard r1080SmallShard = columnShardForPhysicalColumn(kR1080Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 0u);
    constexpr ColumnShard r4320LargeShard = columnShardForPhysicalColumn(kR4320Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 0u);
    static_assert(fourColLane0.cellColumns == 60u);
    static_assert(fourColLane0.cellCount == 8160u);
    static_assert(fourColLane1.firstCellColumn == 60u);
    static_assert(fourColLane1.cellColumns == 60u);
    static_assert(fourColShard0.cellColumns == 15u);
    static_assert(fourColShard3.firstCellColumn == 45u);
    static_assert(fourColShard3.cellColumns == 15u);
    static_assert(r1080SmallShard.cellColumns == 8u);
    static_assert(r4320LargeShard.cellColumns == 30u);

    require(frameCellByteOffset(kR2160Capacity, spatial2Npu2Lanes, 0u, 0u) == 0u, "first cell offset should be zero");
    require(frameCellByteOffset(kR2160Capacity, spatial2Npu2Lanes, 1u, 0u) == 256u, "row-major pixel cell-column offset changed");
    require(frameCellByteOffset(kR2160Capacity, spatial2Npu2Lanes, 0u, 1u) == 245760u, "row-major next-cell-row offset changed");
    require(frameCellByteOffset(kR2160Capacity, spatial2Npu2Lanes, 30u, 0u) == twoColLane1.frameSlotFirstByte, "lane 1 first byte changed");
    require(frameCellByteOffset(kR2160Capacity, spatial2Npu2Lanes, 119u, 135u) + kC16CellBytes <= kR2160Capacity.frameSlotBytes, "last cell exceeds frame slot");

    std::cout << "APUSR middle-core spatial layout smoke: row-major C16 frame and placement stripes validated\n";
    return 0;
}
