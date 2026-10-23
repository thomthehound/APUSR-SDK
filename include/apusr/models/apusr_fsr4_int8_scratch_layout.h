// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/models/apusr_fsr4_int8_graph.h>

#include <cstdint>
#include <vector>

namespace apusr::models::fsr4_int8 {

struct FrontC16PassTilingDesc {
    DisplaySize activeDisplaySize;
    DisplaySize alignedDisplaySize;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    DisplaySize activeC16Size;
    DisplaySize capacityC16Size;
    std::uint32_t tileWidth = 64;
    std::uint32_t tileHeight = 1;
    std::uint32_t loadWidth = 66;
    std::uint32_t loadHeight = 3;
    std::uint32_t channels = 16;
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
    std::uint32_t lastGroupValidPixels = 0;
    std::uint32_t lastGroupPaddingPixels = 0;
    bool activeWidthIsIntegralTile = false;
    bool groupsXIsEven = false;
};

struct FrontC16ScratchCarrierDesc {
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    DisplaySize capacityDisplaySize;
    DisplaySize denseC16Size;
    DisplaySize paddedStage0C16Size;
    std::uint32_t c16Channels = 16;
    std::uint64_t denseStage0Bytes = 0;
    std::uint64_t paddedStage0Bytes = 0;
    std::uint64_t paddedStage0ExtraBytes = 0;
    std::uint64_t currentPass1OutputOffsetBytes = 0;
    std::uint64_t paddedPass1OutputOffsetBytes = 0;
    std::uint64_t generatedMaxScratchEndBytes = 0;
    std::uint64_t declaredScratchBytes = 0;
    std::uint64_t declaredReserveBytes = 0;
    std::uint64_t paddedLayoutMaxScratchEndBytes = 0;
    std::uint64_t paddedLayoutReserveBytes = 0;
    bool paddedStage0FitsDeclaredScratch = false;
};

struct MiddlePassTilingDesc {
    std::uint32_t logicalStage = 0;
    DisplaySize activeDisplaySize;
    DisplaySize alignedDisplaySize;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    DisplaySize tensorSize;
    std::uint32_t downscaleShift = 0;
    std::uint32_t tileWidth = 0;
    std::uint32_t tileHeight = 0;
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
    std::uint32_t lastGroupValidPixelsX = 0;
    std::uint32_t lastGroupPaddingPixelsX = 0;
    std::uint32_t lastGroupValidPixelsY = 0;
    std::uint32_t lastGroupPaddingPixelsY = 0;
    bool activeWidthIsIntegralTile = false;
    bool activeHeightIsIntegralTile = false;
    bool groupsXIsEven = false;
    bool validStage = false;
};

enum class ScratchLayoutVariant : std::uint32_t {
    DenseStage0Current,
    PaddedStage0Candidate,
};

enum class ScratchRegionRole : std::uint32_t {
    Stage0C16Carrier,
    GeneratedStage1AndTail,
    DeclaredReserve,
};

enum class ScratchRegionOwner : std::uint32_t {
    GpuOnly,
    SharedUmaCandidate,
};

struct ScratchRegionDesc {
    ScratchRegionRole role = ScratchRegionRole::Stage0C16Carrier;
    ScratchRegionOwner owner = ScratchRegionOwner::GpuOnly;
    const char* name = nullptr;
    std::uint64_t offsetBytes = 0;
    std::uint64_t byteSize = 0;
    std::uint64_t endBytes = 0;
    std::uint32_t alignmentBytes = 16;
    bool producerWrites = false;
    bool consumerReads = false;
    bool mayAliasAfterBarrier = false;
};

struct FrontC16ScratchLayoutContractDesc {
    ScratchLayoutVariant variant = ScratchLayoutVariant::DenseStage0Current;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    DisplaySize capacityDisplaySize;
    DisplaySize stage0StorageSize;
    std::uint32_t stage0Channels = 16;
    std::uint32_t stage0LogicalOriginX = 0;
    std::uint32_t stage0LogicalOriginY = 0;
    std::uint64_t stage0RowStrideBytes = 0;
    std::uint64_t stage1OutputOffsetBytes = 0;
    std::uint64_t generatedMaxScratchEndBytes = 0;
    std::uint64_t maxUsedScratchBytes = 0;
    std::uint64_t declaredScratchBytes = 0;
    std::uint64_t reserveBytes = 0;
    std::uint64_t extraBytesVsDenseCurrent = 0;
    bool fitsDeclaredScratch = false;
    bool regionsAreContiguous = false;
    bool regionsAreNonOverlapping = false;
    bool stage0OriginHasOnePixelHalo = false;
    std::vector<ScratchRegionDesc> regions;
};

[[nodiscard]] const char* scratchLayoutVariantName(ScratchLayoutVariant variant) noexcept;
[[nodiscard]] const char* scratchRegionRoleName(ScratchRegionRole role) noexcept;
[[nodiscard]] const char* scratchRegionOwnerName(ScratchRegionOwner owner) noexcept;

[[nodiscard]] DisplaySize alignReferenceUpscaleSizeForScratch(DisplaySize displaySize) noexcept;
[[nodiscard]] FrontC16PassTilingDesc makeFrontC16PassTilingDesc(DisplaySize displaySize) noexcept;
[[nodiscard]] MiddlePassTilingDesc makeMiddlePassTilingDesc(std::uint32_t logicalStage, DisplaySize displaySize) noexcept;
[[nodiscard]] FrontC16ScratchCarrierDesc makeFrontC16ScratchCarrierDesc(FrameCapacity frameCapacity) noexcept;
[[nodiscard]] bool frontC16PaddedStage0CarrierFitsCurrentScratch(FrameCapacity frameCapacity) noexcept;
[[nodiscard]] FrontC16ScratchLayoutContractDesc makeFrontC16ScratchLayoutContract(
    FrameCapacity frameCapacity,
    ScratchLayoutVariant variant);

} // namespace apusr::models::fsr4_int8
