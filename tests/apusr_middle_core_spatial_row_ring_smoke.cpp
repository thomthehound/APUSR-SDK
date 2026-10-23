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

    constexpr std::uint32_t spatial2Npu2Lanes = autoLaneGroups(kNpu2Columns, kSpatial2ColLaneGroupColumns);
    constexpr std::uint32_t spatial4Npu2Lanes = autoLaneGroups(kNpu2Columns, kSpatial4ColLaneGroupColumns);

    constexpr ColumnShard r2160LeftShard = columnShardForPhysicalColumn(kR2160Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 0u);
    constexpr ColumnShard r2160InteriorShard = columnShardForPhysicalColumn(kR2160Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 3u);
    constexpr ColumnShard r2160RightShard = columnShardForPhysicalColumn(kR2160Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 7u);
    constexpr ColumnShard r4320WorstShard = columnShardForPhysicalColumn(kR4320Capacity, spatial2Npu2Lanes, kSpatial2ColLaneGroupColumns, 0u);
    constexpr ColumnShard r2160FourColShard = columnShardForPhysicalColumn(kR2160Capacity, spatial4Npu2Lanes, kSpatial4ColLaneGroupColumns, 3u);

    constexpr FrontC16HaloPatchPlan leftPatch = frontC16HaloPatchPlanForShard(kR2160Capacity, r2160LeftShard);
    constexpr FrontC16HaloPatchPlan interiorPatch = frontC16HaloPatchPlanForShard(kR2160Capacity, r2160InteriorShard);
    constexpr FrontC16HaloPatchPlan rightPatch = frontC16HaloPatchPlanForShard(kR2160Capacity, r2160RightShard);
    constexpr FrontC16HaloPatchPlan worstPatch = frontC16HaloPatchPlanForShard(kR4320Capacity, r4320WorstShard);
    constexpr FrontC16HaloPatchPlan fourColPatch = frontC16HaloPatchPlanForShard(kR2160Capacity, r2160FourColShard);

    static_assert(leftPatch.guardPixels == 1u);
    static_assert(leftPatch.patchWidth == 18u);
    static_assert(leftPatch.patchRows == 10u);
    static_assert(leftPatch.patchBytes == 2880u);
    static_assert(leftPatch.fifoDepth == 1u);
    static_assert(leftPatch.activeFifoBytes == 2880u);
    static_assert(leftPatch.frontIngressSlotBytes == kR2160Capacity.frontIngressSlotBytes);
    static_assert(interiorPatch.patchBytes == 2880u);
    static_assert(rightPatch.patchBytes == 2880u);
    static_assert(worstPatch.patchBytes == 2880u);
    static_assert(fourColPatch.patchBytes == 2880u);

    constexpr std::uint32_t topLeftFlags = frontC16FrameEdgeFlags(kR2160Capacity, r2160LeftShard, 0u);
    constexpr std::uint32_t middleLeftFlags = frontC16FrameEdgeFlags(kR2160Capacity, r2160LeftShard, 17u);
    constexpr std::uint32_t bottomRightFlags = frontC16FrameEdgeFlags(kR2160Capacity, r2160RightShard, kR2160Capacity.cellRows - 1u);
    constexpr std::uint32_t topInteriorFlags = frontC16FrameEdgeFlags(kR2160Capacity, r2160InteriorShard, 0u);

    static_assert((topLeftFlags & kFrontC16EdgeLeft) != 0u);
    static_assert((topLeftFlags & kFrontC16EdgeTop) != 0u);
    static_assert((topLeftFlags & kFrontC16EdgeRight) == 0u);
    static_assert((middleLeftFlags & kFrontC16EdgeLeft) != 0u);
    static_assert((middleLeftFlags & kFrontC16EdgeTop) == 0u);
    static_assert((middleLeftFlags & kFrontC16EdgeBottom) == 0u);
    static_assert((bottomRightFlags & kFrontC16EdgeRight) != 0u);
    static_assert((bottomRightFlags & kFrontC16EdgeBottom) != 0u);
    static_assert((topInteriorFlags & kFrontC16EdgeLeft) == 0u);
    static_assert((topInteriorFlags & kFrontC16EdgeRight) == 0u);
    static_assert((topInteriorFlags & kFrontC16EdgeTop) != 0u);

    require(leftPatch.activeFifoBytes < 4u * 1024u, "front halo patch should stay a small hot FIFO object");
    require(worstPatch.activeFifoBytes == kC16HaloCellBytes, "worst placement should not inflate the compute-tile front object");
    require(kR2160Capacity.frontIngressSlotBytes > kR2160Capacity.frameSlotBytes, "zero-guarded ingress view must include one-pixel frame padding");

    std::cout << "APUSR middle-core spatial halo-patch smoke: DMA tensor patch carrier and edge flags validated\n";
    return 0;
}
