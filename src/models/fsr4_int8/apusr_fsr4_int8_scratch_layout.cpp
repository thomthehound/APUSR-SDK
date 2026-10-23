// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_scratch_layout.h>
#include <apusr/models/apusr_fsr4_int8_resources.h>

#include <algorithm>
#include <cstdint>

namespace apusr::models::fsr4_int8 {
namespace {

constexpr std::uint32_t kC16 = 16;
constexpr std::uint32_t kPass1TileWidth = 64;
constexpr std::uint32_t kPass1TileHeight = 2;
constexpr std::uint32_t kPass1Halo = 1;
constexpr std::uint32_t kC16PixelAlignmentBytes = 16;

[[nodiscard]] constexpr std::uint32_t roundUpDiv(const std::uint32_t value, const std::uint32_t divisor) noexcept
{
    return (value + divisor - 1u) / divisor;
}

[[nodiscard]] constexpr std::uint32_t alignUp(const std::uint32_t value, const std::uint32_t alignment) noexcept
{
    return roundUpDiv(value, alignment) * alignment;
}

[[nodiscard]] constexpr std::uint64_t tensorBytes(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t channels) noexcept
{
    return static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) *
        static_cast<std::uint64_t>(channels);
}

[[nodiscard]] DisplaySize capacityDisplaySize(const FrameCapacity frameCapacity) noexcept
{
    const FrameCapacityLimits limits = frameCapacityLimits(frameCapacity);
    return DisplaySize{.width = limits.maxWidth, .height = limits.maxHeight};
}

[[nodiscard]] std::uint64_t generatedMaxScratchEndBytes(const FrameCapacity frameCapacity) noexcept
{
    switch (frameCapacity) {
    case FrameCapacity::R1080: return 20'736'000ull;
    case FrameCapacity::R2160: return 82'944'000ull;
    case FrameCapacity::R4320: return 331'776'000ull;
    }
    return 0;
}

[[nodiscard]] constexpr std::uint64_t endBytes(
    const std::uint64_t offsetBytes,
    const std::uint64_t byteSize) noexcept
{
    return offsetBytes + byteSize;
}

[[nodiscard]] ScratchRegionDesc makeRegion(
    const ScratchRegionRole role,
    const ScratchRegionOwner owner,
    const char* const name,
    const std::uint64_t offsetBytes,
    const std::uint64_t byteSize,
    const bool producerWrites,
    const bool consumerReads,
    const bool mayAliasAfterBarrier) noexcept
{
    return ScratchRegionDesc{
        .role = role,
        .owner = owner,
        .name = name,
        .offsetBytes = offsetBytes,
        .byteSize = byteSize,
        .endBytes = endBytes(offsetBytes, byteSize),
        .alignmentBytes = kC16PixelAlignmentBytes,
        .producerWrites = producerWrites,
        .consumerReads = consumerReads,
        .mayAliasAfterBarrier = mayAliasAfterBarrier,
    };
}

[[nodiscard]] bool regionsAreContiguous(const std::vector<ScratchRegionDesc>& regions) noexcept
{
    std::uint64_t expectedOffset = 0;
    for (const ScratchRegionDesc& region : regions) {
        if (region.offsetBytes != expectedOffset) {
            return false;
        }
        expectedOffset = region.endBytes;
    }
    return true;
}

[[nodiscard]] bool regionsAreNonOverlapping(const std::vector<ScratchRegionDesc>& regions) noexcept
{
    std::uint64_t previousEnd = 0;
    for (const ScratchRegionDesc& region : regions) {
        if (region.offsetBytes < previousEnd) {
            return false;
        }
        if (region.endBytes < region.offsetBytes) {
            return false;
        }
        previousEnd = region.endBytes;
    }
    return true;
}

} // namespace

const char* scratchLayoutVariantName(const ScratchLayoutVariant variant) noexcept
{
    switch (variant) {
    case ScratchLayoutVariant::DenseStage0Current: return "dense-stage0-current";
    case ScratchLayoutVariant::PaddedStage0Candidate: return "padded-stage0-candidate";
    }
    return "unknown";
}

const char* scratchRegionRoleName(const ScratchRegionRole role) noexcept
{
    switch (role) {
    case ScratchRegionRole::Stage0C16Carrier: return "stage0-c16-carrier";
    case ScratchRegionRole::GeneratedStage1AndTail: return "generated-stage1-and-tail";
    case ScratchRegionRole::DeclaredReserve: return "declared-reserve";
    }
    return "unknown";
}

const char* scratchRegionOwnerName(const ScratchRegionOwner owner) noexcept
{
    switch (owner) {
    case ScratchRegionOwner::GpuOnly: return "gpu-only";
    case ScratchRegionOwner::SharedUmaCandidate: return "shared-uma-candidate";
    }
    return "unknown";
}

DisplaySize alignReferenceUpscaleSizeForScratch(const DisplaySize displaySize) noexcept
{
    return DisplaySize{.width = alignUp(displaySize.width, 8u), .height = alignUp(displaySize.height, 8u)};
}

FrontC16PassTilingDesc makeFrontC16PassTilingDesc(const DisplaySize displaySize) noexcept
{
    const DisplaySize aligned = alignReferenceUpscaleSizeForScratch(displaySize);
    const FrameCapacity capacity = frameCapacityForDisplay(aligned);
    const DisplaySize capacityDisplay = capacityDisplaySize(capacity);
    const std::uint32_t activeC16Width = aligned.width / 2u;
    const std::uint32_t activeC16Height = aligned.height / 2u;
    const std::uint32_t groupsX = roundUpDiv(activeC16Width, kPass1TileWidth);
    const std::uint32_t lastValid = groupsX == 0u ? 0u : activeC16Width - ((groupsX - 1u) * kPass1TileWidth);
    const std::uint32_t lastPadding = lastValid == 0u || lastValid == kPass1TileWidth ? 0u : kPass1TileWidth - lastValid;

    return FrontC16PassTilingDesc{
        .activeDisplaySize = displaySize,
        .alignedDisplaySize = aligned,
        .frameCapacity = capacity,
        .activeC16Size = DisplaySize{.width = activeC16Width, .height = activeC16Height},
        .capacityC16Size = DisplaySize{.width = capacityDisplay.width / 2u, .height = capacityDisplay.height / 2u},
        .tileWidth = kPass1TileWidth,
        .tileHeight = kPass1TileHeight,
        .loadWidth = kPass1TileWidth + 2u * kPass1Halo,
        .loadHeight = kPass1TileHeight + 2u * kPass1Halo,
        .channels = kC16,
        .groupsX = groupsX,
        .groupsY = roundUpDiv(activeC16Height, kPass1TileHeight),
        .lastGroupValidPixels = lastValid,
        .lastGroupPaddingPixels = lastPadding,
        .activeWidthIsIntegralTile = lastPadding == 0u,
        .groupsXIsEven = (groupsX % 2u) == 0u,
    };
}

MiddlePassTilingDesc makeMiddlePassTilingDesc(const std::uint32_t logicalStage, const DisplaySize displaySize) noexcept
{
    const DisplaySize aligned = alignReferenceUpscaleSizeForScratch(displaySize);
    const FrameCapacity capacity = frameCapacityForDisplay(aligned);

    std::uint32_t downscaleShift = 0u;
    std::uint32_t tileWidth = 0u;
    std::uint32_t tileHeight = 0u;
    switch (logicalStage) {
    case 1:
    case 2:
        downscaleShift = 1u;
        tileWidth = 64u;
        tileHeight = 2u;
        break;
    case 12:
        downscaleShift = 1u;
        tileWidth = 64u;
        tileHeight = 1u;
        break;
    case 3:
    case 4:
    case 5:
    case 10:
    case 11:
        downscaleShift = 2u;
        tileWidth = 64u;
        tileHeight = 1u;
        break;
    case 6:
    case 7:
    case 8:
        downscaleShift = 3u;
        tileWidth = 64u;
        tileHeight = 1u;
        break;
    case 9:
        downscaleShift = 3u;
        tileWidth = 8u;
        tileHeight = 8u;
        break;
    default:
        return MiddlePassTilingDesc{
            .logicalStage = logicalStage,
            .activeDisplaySize = displaySize,
            .alignedDisplaySize = aligned,
            .frameCapacity = capacity,
            .tensorSize = {},
        };
    }

    const DisplaySize tensor{.width = aligned.width >> downscaleShift, .height = aligned.height >> downscaleShift};
    const std::uint32_t groupsX = roundUpDiv(tensor.width, tileWidth);
    const std::uint32_t groupsY = roundUpDiv(tensor.height, tileHeight);
    const std::uint32_t lastValidX = groupsX == 0u ? 0u : tensor.width - ((groupsX - 1u) * tileWidth);
    const std::uint32_t lastValidY = groupsY == 0u ? 0u : tensor.height - ((groupsY - 1u) * tileHeight);
    const std::uint32_t padX = lastValidX == 0u || lastValidX == tileWidth ? 0u : tileWidth - lastValidX;
    const std::uint32_t padY = lastValidY == 0u || lastValidY == tileHeight ? 0u : tileHeight - lastValidY;

    return MiddlePassTilingDesc{
        .logicalStage = logicalStage,
        .activeDisplaySize = displaySize,
        .alignedDisplaySize = aligned,
        .frameCapacity = capacity,
        .tensorSize = tensor,
        .downscaleShift = downscaleShift,
        .tileWidth = tileWidth,
        .tileHeight = tileHeight,
        .groupsX = groupsX,
        .groupsY = groupsY,
        .lastGroupValidPixelsX = lastValidX,
        .lastGroupPaddingPixelsX = padX,
        .lastGroupValidPixelsY = lastValidY,
        .lastGroupPaddingPixelsY = padY,
        .activeWidthIsIntegralTile = padX == 0u,
        .activeHeightIsIntegralTile = padY == 0u,
        .groupsXIsEven = (groupsX % 2u) == 0u,
        .validStage = true,
    };
}

FrontC16ScratchCarrierDesc makeFrontC16ScratchCarrierDesc(const FrameCapacity frameCapacity) noexcept
{
    const DisplaySize capacityDisplay = capacityDisplaySize(frameCapacity);
    const DisplaySize denseC16{.width = capacityDisplay.width / 2u, .height = capacityDisplay.height / 2u};
    const DisplaySize paddedC16{.width = denseC16.width + 2u, .height = denseC16.height + 2u};
    const std::uint64_t denseBytes = tensorBytes(denseC16.width, denseC16.height, kC16);
    const std::uint64_t paddedBytes = tensorBytes(paddedC16.width, paddedC16.height, kC16);
    const std::uint64_t generatedMax = generatedMaxScratchEndBytes(frameCapacity);
    const std::uint64_t declared = scratchByteSize(frameCapacity);
    const std::uint64_t paddedMax = generatedMax + (paddedBytes - denseBytes);

    return FrontC16ScratchCarrierDesc{
        .frameCapacity = frameCapacity,
        .capacityDisplaySize = capacityDisplay,
        .denseC16Size = denseC16,
        .paddedStage0C16Size = paddedC16,
        .c16Channels = kC16,
        .denseStage0Bytes = denseBytes,
        .paddedStage0Bytes = paddedBytes,
        .paddedStage0ExtraBytes = paddedBytes - denseBytes,
        .currentPass1OutputOffsetBytes = denseBytes,
        .paddedPass1OutputOffsetBytes = paddedBytes,
        .generatedMaxScratchEndBytes = generatedMax,
        .declaredScratchBytes = declared,
        .declaredReserveBytes = declared >= generatedMax ? declared - generatedMax : 0u,
        .paddedLayoutMaxScratchEndBytes = paddedMax,
        .paddedLayoutReserveBytes = declared >= paddedMax ? declared - paddedMax : 0u,
        .paddedStage0FitsDeclaredScratch = paddedMax <= declared,
    };
}

bool frontC16PaddedStage0CarrierFitsCurrentScratch(const FrameCapacity frameCapacity) noexcept
{
    return makeFrontC16ScratchCarrierDesc(frameCapacity).paddedStage0FitsDeclaredScratch;
}

FrontC16ScratchLayoutContractDesc makeFrontC16ScratchLayoutContract(
    const FrameCapacity frameCapacity,
    const ScratchLayoutVariant variant)
{
    const FrontC16ScratchCarrierDesc carrier = makeFrontC16ScratchCarrierDesc(frameCapacity);
    const bool padded = variant == ScratchLayoutVariant::PaddedStage0Candidate;
    const DisplaySize stage0Storage = padded ? carrier.paddedStage0C16Size : carrier.denseC16Size;
    const std::uint32_t stage0Origin = padded ? 1u : 0u;
    const std::uint64_t stage0Bytes = padded ? carrier.paddedStage0Bytes : carrier.denseStage0Bytes;
    const std::uint64_t stage1Offset = padded ? carrier.paddedPass1OutputOffsetBytes : carrier.currentPass1OutputOffsetBytes;
    const std::uint64_t maxUsed = padded ? carrier.paddedLayoutMaxScratchEndBytes : carrier.generatedMaxScratchEndBytes;
    const std::uint64_t generatedTailBytes = maxUsed >= stage1Offset ? maxUsed - stage1Offset : 0u;
    const std::uint64_t reserveBytes = carrier.declaredScratchBytes >= maxUsed ? carrier.declaredScratchBytes - maxUsed : 0u;

    std::vector<ScratchRegionDesc> regions;
    regions.reserve(3);
    regions.push_back(makeRegion(
        ScratchRegionRole::Stage0C16Carrier,
        padded ? ScratchRegionOwner::SharedUmaCandidate : ScratchRegionOwner::GpuOnly,
        padded ? "stage0_c16_padded_carrier" : "stage0_c16_dense_current",
        0u,
        stage0Bytes,
        true,
        true,
        false));
    regions.push_back(makeRegion(
        ScratchRegionRole::GeneratedStage1AndTail,
        ScratchRegionOwner::GpuOnly,
        padded ? "generated_stage1_to_post_shifted" : "generated_stage1_to_post_current",
        stage1Offset,
        generatedTailBytes,
        true,
        true,
        true));
    regions.push_back(makeRegion(
        ScratchRegionRole::DeclaredReserve,
        padded ? ScratchRegionOwner::SharedUmaCandidate : ScratchRegionOwner::GpuOnly,
        padded ? "declared_reserve_after_padded_shift" : "declared_reserve_current",
        maxUsed,
        reserveBytes,
        false,
        false,
        false));

    return FrontC16ScratchLayoutContractDesc{
        .variant = variant,
        .frameCapacity = frameCapacity,
        .capacityDisplaySize = carrier.capacityDisplaySize,
        .stage0StorageSize = stage0Storage,
        .stage0Channels = kC16,
        .stage0LogicalOriginX = stage0Origin,
        .stage0LogicalOriginY = stage0Origin,
        .stage0RowStrideBytes = static_cast<std::uint64_t>(stage0Storage.width) * kC16,
        .stage1OutputOffsetBytes = stage1Offset,
        .generatedMaxScratchEndBytes = carrier.generatedMaxScratchEndBytes,
        .maxUsedScratchBytes = maxUsed,
        .declaredScratchBytes = carrier.declaredScratchBytes,
        .reserveBytes = reserveBytes,
        .extraBytesVsDenseCurrent = padded ? carrier.paddedStage0ExtraBytes : 0u,
        .fitsDeclaredScratch = maxUsed <= carrier.declaredScratchBytes,
        .regionsAreContiguous = regionsAreContiguous(regions),
        .regionsAreNonOverlapping = regionsAreNonOverlapping(regions),
        .stage0OriginHasOnePixelHalo = padded,
        .regions = regions,
    };
}

} // namespace apusr::models::fsr4_int8
