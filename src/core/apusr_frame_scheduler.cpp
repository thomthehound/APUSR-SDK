// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_runtime.h>
#include <apusr/apusr_diagnostics.h>
#include <apusr/kernels/apusr_debug_tile_constants.h>
#include <apusr/kernels/apusr_frame_conveyor_abi.h>

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace apusr {

namespace {

[[nodiscard]] std::uint64_t ingressBandByteOffset(
    const TokenStreamSlot& slot,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    const std::uint64_t frameRowOffset = slot.framePayloadSlot ?
        static_cast<std::uint64_t>(band.inputFirstRow) * tokenRowPitchBytes(segment.inputBoundary.shape) : 0;
    return slot.ingressByteOffset + frameRowOffset;
}

[[nodiscard]] std::uint64_t egressBandByteOffset(
    const TokenStreamSlot& slot,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    const std::uint64_t frameRowOffset = slot.framePayloadSlot ?
        static_cast<std::uint64_t>(band.egressFirstRow) * tokenRowPitchBytes(segment.outputBoundary.shape) : 0;
    return slot.egressByteOffset + frameRowOffset;
}


[[nodiscard]] TokenShape zeroGuardedIngressShape(TokenShape shape)
{
    shape.layout = TokenLayout::ZeroGuardedRasterNhwcC16;
    return shape;
}

[[nodiscard]] std::uint64_t zeroGuardedIngressFrameBytes(const SegmentManifest& segment)
{
    return tokenSlabBytes(zeroGuardedIngressShape(segment.inputBoundary.shape));
}

[[nodiscard]] std::uint64_t egressFrameBytes(const SegmentManifest& segment)
{
    return tokenSlabBytes(segment.outputBoundary.shape);
}

[[nodiscard]] std::uint64_t frameTransportBytes(const std::uint64_t semanticBytes)
{
    return alignUp(semanticBytes, kernels::frame_conveyor::kChunkBytes);
}


[[nodiscard]] std::uint64_t egressFrameTransportBytes(const SegmentManifest& segment)
{
    return frameTransportBytes(egressFrameBytes(segment));
}

Status recordExport(
    GraphicsBackend& graphics,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band,
    const bool restoreInputStatesAfterExport)
{
    Status status = graphics.resetCommandContext(slot.exportCommands);
    if (!status.succeeded()) {
        return status;
    }

    TokenBoundary destinationBoundary = segment.inputBoundary;
    destinationBoundary.shape = band.inputShape;
    destinationBoundary.byteSize = band.inputByteSize;

    ExportPassDesc exportDesc{
        .frameIndex = frame.frameIndex,
        .color = frame.surfaces.color,
        .depth = frame.surfaces.depth,
        .motionVectors = frame.surfaces.motionVectors,
        .exposure = frame.surfaces.exposure,
        .reactiveMask = frame.surfaces.reactiveMask,
        .transparencyComposition = frame.surfaces.transparencyComposition,
        .displaySize = frame.displaySize,
        .frameConstants = makeUpscalerFrameConstants(
            frame.renderSize,
            frame.displaySize,
            frame.jitter,
            frame.previousJitter,
            frame.options.features.displayResolutionMotionVectors ? MotionVectorResolution::DisplayResolution : MotionVectorResolution::RenderResolution),
        .frameOptions = frame.options,
        .destination = slot.ingressTokenSlab,
        .destinationByteOffset = ingressBandByteOffset(slot, segment, band),
        .destinationBoundary = destinationBoundary,
        .band = band,
        .restoreInputStatesAfterExport = restoreInputStatesAfterExport,
    };

    status = graphics.recordExportPass(slot.exportCommands, exportDesc);
    if (!status.succeeded()) {
        return status;
    }

    return graphics.closeCommandContext(slot.exportCommands);
}

Status recordImport(
    GraphicsBackend& graphics,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    Status status = graphics.resetCommandContext(slot.importCommands);
    if (!status.succeeded()) {
        return status;
    }

    const bool roundTripVerify = false;

    ImportPassDesc importDesc{
        .frameIndex = frame.frameIndex,
        .source = slot.egressTokenSlab,
        .sourceByteOffset = egressBandByteOffset(slot, segment, band),
        .sourceBoundary = segment.outputBoundary,
        .band = band,
        .color = frame.surfaces.color,
        .exposure = frame.surfaces.exposure,
        .output = frame.surfaces.output,
        .frameConstants = makeUpscalerFrameConstants(
            frame.renderSize,
            frame.displaySize,
            frame.jitter,
            frame.previousJitter,
            frame.options.features.displayResolutionMotionVectors ? MotionVectorResolution::DisplayResolution : MotionVectorResolution::RenderResolution),
        .frameOptions = frame.options,
        .roundTripCopyVerify = RoundTripCopyVerifyDesc{
            .enabled = roundTripVerify,
            .expected = slot.ingressTokenSlab,
            .expectedByteOffset = 0,
            .actualByteOffset = 0,
            .byteSize = roundTripVerify ? (static_cast<std::uint64_t>(segment.streamPlan.bandCount) * segment.streamPlan.maxInputBandBytes) : 0,
            .completionSignal = TimelineFenceSignal{.fence = slot.slotRetiredFence, .value = slot.slotRetiredValue},
        },
    };

    status = graphics.recordImportPass(slot.importCommands, importDesc);
    if (!status.succeeded()) {
        return status;
    }

    return graphics.closeCommandContext(slot.importCommands);
}

constexpr std::uint32_t kKernelTileProbeTimeoutMs = 2000;

[[nodiscard]] NpuPayloadContract gpuPayloadContract(const TokenBandDesc& band)
{
    return NpuPayloadContract{
        .ingressProducer = NpuPayloadProducer::GpuQueue,
        .egressConsumer = NpuPayloadConsumer::GpuQueue,
        .ingressByteSize = band.inputByteSize,
        .egressByteSize = band.outputByteSize,
    };
}


[[nodiscard]] TokenBandDesc makeFrameCommandBand(const SegmentManifest& segment)
{
    TokenBandDesc band{};
    band.bandIndex = 0;
    band.bandCount = 1;
    band.logicalFirstRow = 0;
    band.logicalRowCount = segment.outputBoundary.shape.height;
    band.inputFirstRow = 0;
    const TokenShape guardedIngressShape = zeroGuardedIngressShape(segment.inputBoundary.shape);
    band.inputRowCount = tokenPaddedRowCount(guardedIngressShape);
    band.outputFirstRow = 0;
    band.outputRowCount = segment.outputBoundary.shape.height;
    band.egressFirstRow = 0;
    band.egressRowCount = segment.outputBoundary.shape.height;
    band.firstBandInFrame = true;
    band.lastBandInFrame = true;
    band.inputShape = guardedIngressShape;
    band.outputShape = segment.outputBoundary.shape;
    band.inputByteSize = zeroGuardedIngressFrameBytes(segment);
    band.outputByteSize = egressFrameBytes(segment);
    return band;
}

[[nodiscard]] NpuPayloadContract cpuDiagnosticPayloadContract(
    const std::uint64_t ingressByteSize,
    const std::uint64_t egressByteSize)
{
    return NpuPayloadContract{
        .ingressProducer = NpuPayloadProducer::CpuDiagnostic,
        .egressConsumer = NpuPayloadConsumer::CpuDiagnostic,
        .ingressByteSize = ingressByteSize,
        .egressByteSize = egressByteSize,
    };
}


[[nodiscard]] std::uint32_t doorbellFlagsForBand(const TokenBandDesc& band)
{
    std::uint32_t flags = NpuDoorbellDescriptorFlagNone;
    if (band.firstBandInFrame) {
        flags |= NpuDoorbellDescriptorFlagFirstBand;
    }
    if (band.lastBandInFrame) {
        flags |= NpuDoorbellDescriptorFlagLastBand;
    }
    return flags;
}

[[nodiscard]] std::uint32_t doorbellIngressOwner(const NpuPayloadContract& contract)
{
    return contract.ingressProducer == NpuPayloadProducer::CpuDiagnostic ?
        static_cast<std::uint32_t>(NpuDoorbellPayloadOwner::CpuDiagnostic) :
        static_cast<std::uint32_t>(NpuDoorbellPayloadOwner::GpuQueue);
}

[[nodiscard]] std::uint32_t doorbellEgressConsumer(const NpuPayloadContract& contract)
{
    switch (contract.egressConsumer) {
    case NpuPayloadConsumer::CpuDiagnostic:
        return static_cast<std::uint32_t>(NpuDoorbellCompletionConsumer::CpuDiagnostic);
    case NpuPayloadConsumer::None:
        return static_cast<std::uint32_t>(NpuDoorbellCompletionConsumer::None);
    case NpuPayloadConsumer::GpuQueue:
        return static_cast<std::uint32_t>(NpuDoorbellCompletionConsumer::GpuQueue);
    }
    return static_cast<std::uint32_t>(NpuDoorbellCompletionConsumer::GpuQueue);
}

[[nodiscard]] NpuDoorbellTokenBandDesc makeDoorbellTokenBandDesc(
    const NpuDispatchDesc& dispatch,
    const std::uint64_t sequence,
    const std::uint64_t frameIndex,
    const std::uint32_t segmentIndex)
{
    return NpuDoorbellTokenBandDesc{
        .kind = static_cast<std::uint32_t>(NpuDoorbellDescriptorKind::TokenBand),
        .abiVersion = kNpuPersistentDoorbellAbiVersion,
        .byteSize = kNpuPersistentDoorbellDescriptorStride,
        .flags = doorbellFlagsForBand(dispatch.band),
        .sequence = sequence,
        .frameIndex = frameIndex,
        .segmentIndex = segmentIndex,
        .tokenSlotIndex = dispatch.tokenSlotIndex,
        .bandIndex = dispatch.band.bandIndex,
        .bandCount = dispatch.band.bandCount,
        .inputFirstRow = dispatch.band.inputFirstRow,
        .inputRowCount = dispatch.band.inputRowCount,
        .outputFirstRow = dispatch.band.outputFirstRow,
        .outputRowCount = dispatch.band.outputRowCount,
        .egressFirstRow = dispatch.band.egressFirstRow,
        .egressRowCount = dispatch.band.egressRowCount,
        .renderWidth = dispatch.renderSize.width,
        .renderHeight = dispatch.renderSize.height,
        .inputByteOffset = dispatch.input.byteOffset,
        .inputByteSize = dispatch.payloadContract.ingressByteSize,
        .outputByteOffset = dispatch.output.byteOffset,
        .outputByteSize = dispatch.payloadContract.egressByteSize,
        .ingressOwner = doorbellIngressOwner(dispatch.payloadContract),
        .egressConsumer = doorbellEgressConsumer(dispatch.payloadContract),
        .tokenLogicalWidth = dispatch.band.inputShape.width,
        .tokenPaddedWidth = tokenPaddedRowWidth(dispatch.band.inputShape),
        .tokenRowPitchBytes = static_cast<std::uint32_t>(tokenRowPitchBytes(dispatch.band.inputShape)),
        .tokenValidRowBytes = static_cast<std::uint32_t>(tokenValidRowBytes(dispatch.band.inputShape)),
    };
}

[[nodiscard]] Status validateDoorbellControlRingStorage(
    const NpuDoorbellRingHeader& header,
    const NpuBufferView& controlRing)
{
    const std::uint64_t requiredBytes =
        static_cast<std::uint64_t>(header.descriptorOffsetBytes) +
        (static_cast<std::uint64_t>(header.descriptorCapacity) * header.descriptorStrideBytes);
    if (controlRing.sharedBuffer.nativeBuffer.value == nullptr) {
        return Status::invalidArgument("persistent doorbell control ring requires a mapped shared buffer");
    }
    if (controlRing.byteSize < requiredBytes || controlRing.sharedBuffer.byteSize < requiredBytes) {
        return Status::invalidArgument("persistent doorbell control ring buffer is smaller than the ABI layout");
    }
    return Status::ok();
}

void writeDoorbellControlRingHeader(
    const NpuDoorbellRingHeader& header,
    NpuBufferView& controlRing,
    NpuDoorbellRingStats& stats)
{
    auto* base = static_cast<std::uint8_t*>(controlRing.sharedBuffer.nativeBuffer.value);
    std::memcpy(base, &header, sizeof(header));
    stats.controlRingWriteCount += 1;
    stats.controlRingWriteBytes += sizeof(header);
}

void writeDoorbellControlRingDescriptors(
    const std::vector<NpuDoorbellTokenBandDesc>& descriptors,
    const std::uint32_t startIndex,
    const std::uint32_t count,
    NpuBufferView& controlRing,
    NpuDoorbellRingStats& stats)
{
    auto* base = static_cast<std::uint8_t*>(controlRing.sharedBuffer.nativeBuffer.value);
    const std::uint64_t byteOffset =
        kNpuPersistentDoorbellDescriptorOffset +
        (static_cast<std::uint64_t>(startIndex) * kNpuPersistentDoorbellDescriptorStride);
    const std::uint64_t byteSize = static_cast<std::uint64_t>(count) * kNpuPersistentDoorbellDescriptorStride;
    std::memcpy(base + byteOffset, descriptors.data() + startIndex, static_cast<std::size_t>(byteSize));
    stats.controlRingWriteCount += 1;
    stats.controlRingWriteBytes += byteSize;
}

[[nodiscard]] constexpr std::uint32_t tileInputIndex(
    const std::uint32_t y,
    const std::uint32_t x,
    const std::uint32_t channel)
{
    return ((y * kernels::debug_tile::kWorstCaseInputWindowWidth + x) * kernels::debug_tile::kModelChannels) + channel;
}

[[nodiscard]] constexpr std::uint32_t tileOutputIndex(
    const std::uint32_t y,
    const std::uint32_t x,
    const std::uint32_t channel)
{
    return ((y * kernels::debug_tile::kDefaultMicroTileOutputWidth + x) * kernels::debug_tile::kModelChannels) + channel;
}

[[nodiscard]] std::uint8_t* mappedIngressBytes(TokenStreamSlot& slot)
{
    return static_cast<std::uint8_t*>(slot.ingressTokenSlab.nativeBuffer.value) + slot.ingressByteOffset;
}

[[nodiscard]] const std::uint8_t* mappedIngressBytes(const TokenStreamSlot& slot)
{
    return static_cast<const std::uint8_t*>(slot.ingressTokenSlab.nativeBuffer.value) + slot.ingressByteOffset;
}

[[nodiscard]] std::uint8_t* mappedEgressBytes(TokenStreamSlot& slot)
{
    return static_cast<std::uint8_t*>(slot.egressTokenSlab.nativeBuffer.value) + slot.egressByteOffset;
}

[[nodiscard]] const std::uint8_t* mappedEgressBytes(const TokenStreamSlot& slot)
{
    return static_cast<const std::uint8_t*>(slot.egressTokenSlab.nativeBuffer.value) + slot.egressByteOffset;
}

[[nodiscard]] std::uint8_t kernelTileProbeInputValue(
    const std::uint32_t y,
    const std::uint32_t x,
    const std::uint32_t channel)
{
    // Keep probe inputs small so the first tile-local arithmetic kernel can
    // accumulate nine i8 values without requiring widening or saturation yet.
    return static_cast<std::uint8_t>((y * 3u + x * 5u + channel) & 0x7u);
}

[[nodiscard]] std::uint8_t expectedKernelTileProbeOutput(
    const std::uint8_t* input,
    const std::uint32_t outputY,
    const std::uint32_t outputX,
    const std::uint32_t channel)
{
    std::uint32_t sum = 0u;
    for (std::uint32_t kernelY = 0; kernelY < 3u; ++kernelY) {
        for (std::uint32_t kernelX = 0; kernelX < 3u; ++kernelX) {
            const std::uint32_t inputIndex = tileInputIndex(outputY + kernelY, outputX + kernelX, channel);
            sum += input[inputIndex];
        }
    }

    return static_cast<std::uint8_t>(sum);
}

void fillKernelTileProbeInput(TokenStreamSlot& slot)
{
    auto* input = mappedIngressBytes(slot);
    auto* output = mappedEgressBytes(slot);

    std::memset(input, 0, static_cast<std::size_t>(kernels::debug_tile::kWorstCaseInputWindowBytes));
    for (std::uint32_t y = 0; y < kernels::debug_tile::kWorstCaseInputWindowRows; ++y) {
        for (std::uint32_t x = 0; x < kernels::debug_tile::kWorstCaseInputWindowWidth; ++x) {
            for (std::uint32_t channel = 0; channel < kernels::debug_tile::kModelChannels; ++channel) {
                input[tileInputIndex(y, x, channel)] = kernelTileProbeInputValue(y, x, channel);
            }
        }
    }

    std::memset(output, 0xa5, static_cast<std::size_t>(kernels::debug_tile::kWorstCaseOutputTileBytes));
}

[[nodiscard]] Status verifyKernelTileProbeOutput(const TokenStreamSlot& slot)
{
    const auto* input = mappedIngressBytes(slot);
    const auto* output = mappedEgressBytes(slot);

    for (std::uint32_t y = 0; y < kernels::debug_tile::kDefaultMicroTileOutputRows; ++y) {
        for (std::uint32_t x = 0; x < kernels::debug_tile::kDefaultMicroTileOutputWidth; ++x) {
            for (std::uint32_t channel = 0; channel < kernels::debug_tile::kModelChannels; ++channel) {
                const std::uint32_t offset = tileOutputIndex(y, x, channel);
                const std::uint8_t expected = expectedKernelTileProbeOutput(input, y, x, channel);
                const std::uint8_t actual = output[offset];
                if (actual != expected) {
                    return Status::runtimeFailure(
                        "KernelTileProbe mismatch at y=" + std::to_string(y) +
                        " x=" + std::to_string(x) +
                        " c=" + std::to_string(channel) +
                        " byte=" + std::to_string(offset) +
                        ": expected " + std::to_string(expected) +
                        ", got " + std::to_string(actual));
                }
            }
        }
    }

    return Status::ok();
}

void publishKernelTileOverlay(TokenStreamSlot& slot, DiagnosticTileOverlayDesc& overlay)
{
    TokenShape tileShape{
        .width = kernels::debug_tile::kDefaultMicroTileOutputWidth,
        .height = kernels::debug_tile::kDefaultMicroTileOutputRows,
        .channels = kernels::debug_tile::kModelChannels,
        .elementType = TokenElementType::Int8,
        .layout = TokenLayout::RasterNhwcC16,
    };

    overlay = DiagnosticTileOverlayDesc{
        .available = true,
        .source = slot.egressTokenSlab,
        .sourceBoundary = TokenBoundary{
            .name = "apusr.kernel_tile_overlay.egress",
            .shape = tileShape,
            .byteOffset = 0,
            .byteSize = kernels::debug_tile::kWorstCaseOutputTileBytes,
        },
        .band = TokenBandDesc{
            .bandIndex = 0xffffffffu,
            .bandCount = 1,
            .logicalFirstRow = 0,
            .logicalRowCount = kernels::debug_tile::kDefaultMicroTileOutputRows,
            .inputFirstRow = 0,
            .inputRowCount = kernels::debug_tile::kWorstCaseInputWindowRows,
            .outputFirstRow = 0,
            .outputRowCount = kernels::debug_tile::kDefaultMicroTileOutputRows,
            .firstBandInFrame = true,
            .lastBandInFrame = true,
            .inputShape = TokenShape{
                .width = kernels::debug_tile::kWorstCaseInputWindowWidth,
                .height = kernels::debug_tile::kWorstCaseInputWindowRows,
                .channels = kernels::debug_tile::kModelChannels,
                .elementType = TokenElementType::Int8,
                .layout = TokenLayout::RasterNhwcC16,
            },
            .outputShape = tileShape,
            .inputByteSize = kernels::debug_tile::kWorstCaseInputWindowBytes,
            .outputByteSize = kernels::debug_tile::kWorstCaseOutputTileBytes,
        },
    };
}


[[nodiscard]] std::uint8_t kernelFrameOverlayInputValue(
    const std::uint32_t globalY,
    const std::uint32_t globalX,
    const std::uint32_t channel)
{
    // Keep the full-frame stress pattern inside the same non-overflow range as
    // the tile probe. The visible overlay should be structured enough to expose
    // wrong x/y/channel placement without depending on real frame contents yet.
    return static_cast<std::uint8_t>((globalY * 3u + globalX * 5u + channel) & 0x7u);
}

void fillKernelFrameOverlayTileInput(
    TokenStreamSlot& scratchSlot,
    const TokenShape& frameShape,
    const std::uint32_t tileOriginX,
    const std::uint32_t tileOriginY)
{
    auto* input = mappedIngressBytes(scratchSlot);
    auto* output = mappedEgressBytes(scratchSlot);

    std::memset(input, 0, static_cast<std::size_t>(kernels::debug_tile::kWorstCaseInputWindowBytes));
    for (std::uint32_t y = 0; y < kernels::debug_tile::kWorstCaseInputWindowRows; ++y) {
        for (std::uint32_t x = 0; x < kernels::debug_tile::kWorstCaseInputWindowWidth; ++x) {
            const std::uint32_t sourceX = std::min(
                tileOriginX + x,
                frameShape.width > 0 ? frameShape.width - 1u : 0u);
            const std::uint32_t sourceY = std::min(
                tileOriginY + y,
                frameShape.height > 0 ? frameShape.height - 1u : 0u);
            for (std::uint32_t channel = 0; channel < kernels::debug_tile::kModelChannels; ++channel) {
                input[tileInputIndex(y, x, channel)] = kernelFrameOverlayInputValue(sourceY, sourceX, channel);
            }
        }
    }

    std::memset(output, 0xa5, static_cast<std::size_t>(kernels::debug_tile::kWorstCaseOutputTileBytes));
}

void scatterKernelFrameOverlayTileOutput(
    const TokenStreamSlot& scratchSlot,
    DiagnosticTileOverlayDesc& overlay,
    const std::uint32_t outputOriginX,
    const std::uint32_t outputOriginY)
{
    const auto* tileOutput = mappedEgressBytes(scratchSlot);
    auto* frameOutput = static_cast<std::uint8_t*>(overlay.source.nativeBuffer.value);
    const std::uint32_t frameWidth = overlay.sourceBoundary.shape.width;
    const std::uint32_t frameHeight = overlay.sourceBoundary.shape.height;

    for (std::uint32_t y = 0; y < kernels::debug_tile::kDefaultMicroTileOutputRows; ++y) {
        const std::uint32_t dstY = outputOriginY + y;
        if (dstY >= frameHeight) {
            break;
        }
        for (std::uint32_t x = 0; x < kernels::debug_tile::kDefaultMicroTileOutputWidth; ++x) {
            const std::uint32_t dstX = outputOriginX + x;
            if (dstX >= frameWidth) {
                break;
            }
            for (std::uint32_t channel = 0; channel < kernels::debug_tile::kModelChannels; ++channel) {
                const std::uint32_t sourceOffset = tileOutputIndex(y, x, channel);
                const std::uint64_t dstOffset =
                    ((static_cast<std::uint64_t>(dstY) * frameWidth + dstX) * kernels::debug_tile::kModelChannels) + channel;
                frameOutput[dstOffset] = tileOutput[sourceOffset];
            }
        }
    }
}

[[nodiscard]] Status ensureKernelFrameOverlayBuffer(
    RuntimeConfig& config,
    const SegmentManifest& segment,
    DiagnosticTileOverlayDesc& overlay)
{
    if (overlay.available && overlay.source.nativeBuffer.value != nullptr) {
        return Status::ok();
    }

    SharedBuffer source{};
    SharedBufferDesc desc{
        .byteSize = segment.outputBoundary.byteSize,
        .alignment = kHostUserptrSharedBufferAlignmentBytes,
        .debugName = "APUSR full-frame native diagnostic overlay",
    };
    Status status = config.graphicsBackend->createTokenBuffer(desc, source);
    if (!status.succeeded()) {
        return status;
    }

    overlay = DiagnosticTileOverlayDesc{
        .available = false,
        .source = source,
        .sourceBoundary = TokenBoundary{
            .name = "apusr.kernel_frame_overlay.egress",
            .shape = segment.outputBoundary.shape,
            .byteOffset = 0,
            .byteSize = segment.outputBoundary.byteSize,
        },
        .band = TokenBandDesc{
            .bandIndex = 0xfffffffeu,
            .bandCount = 1,
            .logicalFirstRow = 0,
            .logicalRowCount = segment.outputBoundary.shape.height,
            .inputFirstRow = 0,
            .inputRowCount = segment.outputBoundary.shape.height,
            .outputFirstRow = 0,
            .outputRowCount = segment.outputBoundary.shape.height,
            .firstBandInFrame = true,
            .lastBandInFrame = true,
            .inputShape = segment.inputBoundary.shape,
            .outputShape = segment.outputBoundary.shape,
            .inputByteSize = segment.inputBoundary.byteSize,
            .outputByteSize = segment.outputBoundary.byteSize,
        },
    };

    std::memset(overlay.source.nativeBuffer.value, 0, static_cast<std::size_t>(overlay.source.byteSize));
    return Status::ok();
}

[[nodiscard]] Status runKernelFrameOverlayTile(
    RuntimeConfig& config,
    TokenStreamSlot& scratchSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    GraphicsBackend& graphics = *config.graphicsBackend;
    NpuBackend& npu = *config.npuBackend;

    scratchSlot.gpuReadyValue += 1;
    scratchSlot.npuDoneValue += 1;

    NpuDispatchDesc npuDesc{
        .segment = segment,
        .band = band,
        .tokenSlotIndex = scratchSlot.index,
        .input = scratchSlot.npuIngressTokenSlab,
        .output = scratchSlot.npuEgressTokenSlab,
        .residentParameterArena = config.residentParameterArena,
        .skipSpillArena = config.skipSpillArena,
        .modelEpoch = config.modelEpoch,
        .waitBeforeRun = TimelineFenceWait{.fence = scratchSlot.gpuReadyFence, .value = scratchSlot.gpuReadyValue},
        .signalAfterRun = TimelineFenceSignal{.fence = scratchSlot.npuDoneFence, .value = scratchSlot.npuDoneValue},
        .renderSize = frame.renderSize,
        .payloadContract = cpuDiagnosticPayloadContract(
            kernels::debug_tile::kWorstCaseInputWindowBytes,
            kernels::debug_tile::kWorstCaseOutputTileBytes),
    };

    Status status = npu.enqueue(npuDesc);
    if (!status.succeeded()) {
        return status;
    }

    GpuSubmitDesc readySignal{
        .commandContext = CommandContext{},
        .waits = {},
        .signals = {TimelineFenceSignal{.fence = scratchSlot.gpuReadyFence, .value = scratchSlot.gpuReadyValue}},
        .debugName = "APUSR KernelFrameOverlay D3D12-ready fence",
    };
    status = graphics.submitGpuWork(readySignal);
    if (!status.succeeded()) {
        return status;
    }

    status = npu.waitForLastDispatch(npuDesc, kKernelTileProbeTimeoutMs);
    if (!status.succeeded()) {
        return status;
    }

    return verifyKernelTileProbeOutput(scratchSlot);
}

} // namespace

Status recordTokenBandExport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    return recordExport(*config.graphicsBackend, slot, frame, segment, band, false);
}

Status recordFrameExport(
    RuntimeConfig& config,
    TokenStreamSlot& framePayloadSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment)
{
    return recordExport(*config.graphicsBackend, framePayloadSlot, frame, segment, makeFrameCommandBand(segment), false);
}

Status submitTokenBandExport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const TimelineFenceWait* reuseWait)
{
    std::vector<TimelineFenceWait> exportWaits;
    if (reuseWait != nullptr) {
        exportWaits.push_back(*reuseWait);
    }

    GpuSubmitDesc exportSubmit{
        .commandContext = slot.exportCommands,
        .waits = std::move(exportWaits),
        .signals = {TimelineFenceSignal{.fence = slot.gpuReadyFence, .value = slot.gpuReadyValue}},
        .debugName = "APUSR GPU token-band export",
    };

    return config.graphicsBackend->submitGpuWork(exportSubmit);
}

Status submitFrameExport(
    RuntimeConfig& config,
    TokenStreamSlot& framePayloadSlot,
    TokenStreamSlot& frameSyncSlot,
    const TimelineFenceWait* reuseWait)
{
    std::vector<TimelineFenceWait> exportWaits;
    if (reuseWait != nullptr) {
        exportWaits.push_back(*reuseWait);
    }

    GpuSubmitDesc exportSubmit{
        .commandContext = framePayloadSlot.exportCommands,
        .waits = std::move(exportWaits),
        .signals = {TimelineFenceSignal{.fence = frameSyncSlot.gpuReadyFence, .value = frameSyncSlot.gpuReadyValue}},
        .debugName = "APUSR GPU frame-conveyor export",
    };

    return config.graphicsBackend->submitGpuWork(exportSubmit);
}

Status scheduleTokenBandExport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band,
    const TimelineFenceWait* reuseWait)
{
    Status status = recordTokenBandExport(config, slot, frame, segment, band);
    if (!status.succeeded()) {
        return status;
    }

    return submitTokenBandExport(config, slot, reuseWait);
}


Status scheduleTokenBandBorrowedExport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    if (frame.borrowedCommandList.value == nullptr) {
        return Status::invalidArgument("borrowed export requires the frontend FFX command list");
    }

    CommandContext borrowedContext{
        .commandList = frame.borrowedCommandList,
        .commandAllocator = NativeHandle{},
        .commandQueue = NativeHandle{},
        .nativeContext = NativeHandle{},
        .phase = CommandPhase::ExportTokens,
    };

    TokenBoundary destinationBoundary = segment.inputBoundary;
    destinationBoundary.shape = band.inputShape;
    destinationBoundary.byteSize = band.inputByteSize;

    ExportPassDesc exportDesc{
        .frameIndex = frame.frameIndex,
        .color = frame.surfaces.color,
        .depth = frame.surfaces.depth,
        .motionVectors = frame.surfaces.motionVectors,
        .exposure = frame.surfaces.exposure,
        .reactiveMask = frame.surfaces.reactiveMask,
        .transparencyComposition = frame.surfaces.transparencyComposition,
        .displaySize = frame.displaySize,
        .frameConstants = makeUpscalerFrameConstants(
            frame.renderSize,
            frame.displaySize,
            frame.jitter,
            frame.previousJitter,
            frame.options.features.displayResolutionMotionVectors ? MotionVectorResolution::DisplayResolution : MotionVectorResolution::RenderResolution),
        .frameOptions = frame.options,
        .destination = slot.ingressTokenSlab,
        .destinationByteOffset = ingressBandByteOffset(slot, segment, band),
        .destinationBoundary = destinationBoundary,
        .band = band,
        .restoreInputStatesAfterExport = true,
    };

    return config.graphicsBackend->recordExportPass(borrowedContext, exportDesc);
}

Status scheduleTokenBandNpu(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    NpuBackend& npu = *config.npuBackend;

    NpuDispatchDesc npuDesc{
        .segment = segment,
        .band = band,
        .tokenSlotIndex = slot.index,
        .input = slot.npuIngressTokenSlab,
        .output = slot.npuEgressTokenSlab,
        .residentParameterArena = config.residentParameterArena,
        .skipSpillArena = config.skipSpillArena,
        .modelEpoch = config.modelEpoch,
        .waitBeforeRun = TimelineFenceWait{.fence = slot.gpuReadyFence, .value = slot.gpuReadyValue},
        .signalAfterRun = TimelineFenceSignal{.fence = slot.npuDoneFence, .value = slot.npuDoneValue},
        .renderSize = frame.renderSize,
        .payloadContract = gpuPayloadContract(band),
    };

    return npu.enqueue(npuDesc);
}

Status enqueueFrameNpuWorklist(
    RuntimeConfig& config,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const std::vector<TokenBandDesc>& bands,
    const std::vector<TokenStreamSlot*>& slots,
    std::vector<NpuDispatchDesc>& dispatchScratch,
    NpuDoorbellRingHeader& doorbellRingHeader,
    std::vector<NpuDoorbellTokenBandDesc>& doorbellRingDescriptors,
    NpuBufferView& doorbellControlRing,
    const NpuBufferView& ingressPayloadArena,
    const NpuBufferView& egressPayloadArena,
    const NpuBufferView& residentParameterArena,
    const NpuBufferView& skipSpillArena,
    NpuDoorbellRingStats& doorbellRingStats)
{
    if (bands.size() != slots.size()) {
        return Status::invalidArgument("NPU frame worklist requires one token slot per band");
    }
    if (bands.empty()) {
        return Status::invalidArgument("NPU frame worklist requires at least one token band");
    }
    if (doorbellRingHeader.magic != kNpuPersistentDoorbellAbiMagic ||
        doorbellRingHeader.version != kNpuPersistentDoorbellAbiVersion ||
        doorbellRingHeader.headerByteSize != kNpuPersistentDoorbellHeaderBytes ||
        doorbellRingHeader.descriptorOffsetBytes != kNpuPersistentDoorbellDescriptorOffset ||
        doorbellRingHeader.descriptorStrideBytes != kNpuPersistentDoorbellDescriptorStride) {
        return Status::invalidArgument("persistent doorbell ring header does not match the ABI");
    }
    if (doorbellRingHeader.descriptorCapacity == 0 ||
        doorbellRingHeader.descriptorCapacity != doorbellRingDescriptors.size()) {
        return Status::invalidArgument("persistent doorbell ring storage does not match the header capacity");
    }
    if (bands.size() > doorbellRingHeader.descriptorCapacity) {
        return Status::invalidArgument("persistent doorbell ring cannot hold one complete frame worklist");
    }
    Status status = validateDoorbellControlRingStorage(doorbellRingHeader, doorbellControlRing);
    if (!status.succeeded()) {
        return status;
    }

    dispatchScratch.clear();
    dispatchScratch.reserve(bands.size());
    for (std::size_t bandIndex = 0; bandIndex < bands.size(); ++bandIndex) {
        const TokenStreamSlot* slot = slots[bandIndex];
        if (slot == nullptr) {
            return Status::invalidArgument("NPU frame worklist contains a null token slot");
        }

        const TokenBandDesc& band = bands[bandIndex];
        dispatchScratch.push_back(NpuDispatchDesc{
            .segment = segment,
            .band = band,
            .tokenSlotIndex = slot->index,
            .input = slot->npuIngressTokenSlab,
            .output = slot->npuEgressTokenSlab,
            .residentParameterArena = residentParameterArena,
            .skipSpillArena = skipSpillArena,
            .modelEpoch = config.modelEpoch,
            .waitBeforeRun = TimelineFenceWait{.fence = slot->gpuReadyFence, .value = slot->gpuReadyValue},
            .signalAfterRun = TimelineFenceSignal{.fence = slot->npuDoneFence, .value = slot->npuDoneValue},
            .renderSize = frame.renderSize,
            .payloadContract = gpuPayloadContract(band),
        });
    }

    const std::uint64_t capacity = doorbellRingHeader.descriptorCapacity;
    const std::uint64_t occupancy = doorbellRingHeader.publishedSequence - doorbellRingHeader.retiredSequence;
    const std::uint64_t dispatchCount = dispatchScratch.size();
    if (dispatchCount > capacity - occupancy) {
        return Status::runtimeFailure("persistent doorbell ring is full");
    }

    const std::uint64_t startSequence = doorbellRingHeader.publishedSequence;
    const std::uint32_t startIndex = static_cast<std::uint32_t>(startSequence % capacity);
    if (static_cast<std::uint64_t>(startIndex) + dispatchCount > capacity) {
        return Status::runtimeFailure("persistent doorbell ring reservation would split a frame worklist");
    }

    for (std::size_t dispatchIndex = 0; dispatchIndex < dispatchScratch.size(); ++dispatchIndex) {
        doorbellRingDescriptors[startIndex + dispatchIndex] = makeDoorbellTokenBandDesc(
            dispatchScratch[dispatchIndex],
            startSequence + static_cast<std::uint64_t>(dispatchIndex),
            frame.frameIndex,
            0);
    }

    writeDoorbellControlRingDescriptors(
        doorbellRingDescriptors,
        startIndex,
        static_cast<std::uint32_t>(dispatchCount),
        doorbellControlRing,
        doorbellRingStats);

    doorbellRingHeader.publishedSequence += dispatchCount;
    doorbellRingHeader.publishedFrameEpoch = frame.frameIndex;
    doorbellRingHeader.reconfigureEpoch = config.modelEpoch;
    doorbellRingStats.reserveCount += 1;
    doorbellRingStats.stagedDescriptorCount += dispatchCount;
    doorbellRingStats.occupancyHighWatermark = std::max(
        doorbellRingStats.occupancyHighWatermark,
        doorbellRingHeader.publishedSequence - doorbellRingHeader.retiredSequence);
    doorbellRingStats.publishedSequence = doorbellRingHeader.publishedSequence;
    doorbellRingStats.retiredSequence = doorbellRingHeader.retiredSequence;
    doorbellRingStats.completedSequence = doorbellRingHeader.completedSequence;
    writeDoorbellControlRingHeader(doorbellRingHeader, doorbellControlRing, doorbellRingStats);

    NpuFrameDispatchDesc frameDispatch{
        .frameIndex = frame.frameIndex,
        .segment = &segment,
        .renderSize = frame.renderSize,
        .dispatches = dispatchScratch.data(),
        .doorbellRingHeader = &doorbellRingHeader,
        .doorbellDescriptors = doorbellRingDescriptors.data() + startIndex,
        .doorbellControlRing = doorbellControlRing,
        .residentParameterArena = residentParameterArena,
        .skipSpillArena = skipSpillArena,
        .modelEpoch = config.modelEpoch,
        .ingressPayloadArena = ingressPayloadArena,
        .egressPayloadArena = egressPayloadArena,
        .doorbellDescriptorByteOffset =
            kNpuPersistentDoorbellDescriptorOffset +
            (static_cast<std::uint64_t>(startIndex) * kNpuPersistentDoorbellDescriptorStride),
        .doorbellStartSequence = startSequence,
        .doorbellStartIndex = startIndex,
        .doorbellRingCapacity = doorbellRingHeader.descriptorCapacity,
        .dispatchCount = static_cast<std::uint32_t>(dispatchScratch.size()),
    };

    status = config.npuBackend->enqueueFrame(frameDispatch);
    if (!status.succeeded()) {
        return status;
    }

    // The current AMD/XRT bridge retires control-ring ownership after the
    // backend has accepted the staged work and lowered it to XRT commands. It
    // does not publish output completion here; visible completion remains the
    // explicit NPU-done fence path until a persistent completion epoch exists.
    doorbellRingHeader.retiredSequence = doorbellRingHeader.publishedSequence;
    doorbellRingStats.commitCount += 1;
    doorbellRingStats.publishedSequence = doorbellRingHeader.publishedSequence;
    doorbellRingStats.retiredSequence = doorbellRingHeader.retiredSequence;
    doorbellRingStats.completedSequence = doorbellRingHeader.completedSequence;
    writeDoorbellControlRingHeader(doorbellRingHeader, doorbellControlRing, doorbellRingStats);
    return Status::ok();
}


Status enqueueFrameCommandConveyor(
    RuntimeConfig& config,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    TokenStreamSlot& framePayloadSlot,
    TokenStreamSlot& frameSyncSlot,
    std::vector<NpuDispatchDesc>& dispatchScratch,
    NpuDoorbellRingHeader& doorbellRingHeader,
    std::vector<NpuDoorbellTokenBandDesc>& doorbellRingDescriptors,
    NpuBufferView& doorbellControlRing,
    const NpuBufferView& ingressPayloadArena,
    const NpuBufferView& egressPayloadArena,
    const NpuBufferView& residentParameterArena,
    const NpuBufferView& skipSpillArena,
    NpuDoorbellRingStats& doorbellRingStats)
{
    (void)ingressPayloadArena;
    (void)egressPayloadArena;

    if (!framePayloadSlot.framePayloadSlot) {
        return Status::invalidArgument("frame command conveyor requires a frame payload slot");
    }
    if (doorbellRingHeader.magic != kNpuPersistentDoorbellAbiMagic ||
        doorbellRingHeader.version != kNpuPersistentDoorbellAbiVersion ||
        doorbellRingHeader.headerByteSize != kNpuPersistentDoorbellHeaderBytes ||
        doorbellRingHeader.descriptorOffsetBytes != kNpuPersistentDoorbellDescriptorOffset ||
        doorbellRingHeader.descriptorStrideBytes != kNpuPersistentDoorbellDescriptorStride) {
        return Status::invalidArgument("persistent doorbell ring header does not match the ABI");
    }
    if (doorbellRingHeader.descriptorCapacity == 0 ||
        doorbellRingHeader.descriptorCapacity != doorbellRingDescriptors.size()) {
        return Status::invalidArgument("persistent doorbell ring storage does not match the header capacity");
    }
    Status status = validateDoorbellControlRingStorage(doorbellRingHeader, doorbellControlRing);
    if (!status.succeeded()) {
        return status;
    }

    const std::uint64_t ingressBytes = zeroGuardedIngressFrameBytes(segment);
    const std::uint64_t egressBytes = egressFrameBytes(segment);
    const std::uint64_t ingressTransportBytes = frameTransportBytes(ingressBytes);
    const std::uint64_t egressTransportBytes = egressFrameTransportBytes(segment);
    if (ingressBytes == 0 || egressBytes == 0 || ingressTransportBytes == 0 || egressTransportBytes == 0 ||
        ingressTransportBytes > framePayloadSlot.npuIngressTokenSlab.byteSize ||
        egressTransportBytes > framePayloadSlot.npuEgressTokenSlab.byteSize) {
        return Status::invalidArgument("frame command conveyor payload exceeds the imported frame transport slot");
    }
    NpuBufferView ingressView = framePayloadSlot.npuIngressTokenSlab;
    ingressView.byteOffset = 0;
    ingressView.byteSize = ingressTransportBytes;
    NpuBufferView egressView = framePayloadSlot.npuEgressTokenSlab;
    egressView.byteOffset = 0;
    egressView.byteSize = egressTransportBytes;

    dispatchScratch.clear();
    dispatchScratch.push_back(NpuDispatchDesc{
        .segment = segment,
        .band = makeFrameCommandBand(segment),
        .tokenSlotIndex = framePayloadSlot.frameSlotIndex,
        .input = ingressView,
        .output = egressView,
        .residentParameterArena = residentParameterArena,
        .skipSpillArena = skipSpillArena,
        .modelEpoch = config.modelEpoch,
        .waitBeforeRun = TimelineFenceWait{.fence = frameSyncSlot.gpuReadyFence, .value = frameSyncSlot.gpuReadyValue},
        .signalAfterRun = TimelineFenceSignal{.fence = frameSyncSlot.npuDoneFence, .value = frameSyncSlot.npuDoneValue},
        .renderSize = frame.renderSize,
        .payloadContract = NpuPayloadContract{
            .ingressProducer = NpuPayloadProducer::GpuQueue,
            .egressConsumer = NpuPayloadConsumer::GpuQueue,
            .ingressByteSize = ingressTransportBytes,
            .egressByteSize = egressTransportBytes,
        },
    });

    const std::uint64_t capacity = doorbellRingHeader.descriptorCapacity;
    const std::uint64_t occupancy = doorbellRingHeader.publishedSequence - doorbellRingHeader.retiredSequence;
    if (occupancy >= capacity) {
        return Status::runtimeFailure("persistent doorbell ring is full");
    }

    const std::uint64_t startSequence = doorbellRingHeader.publishedSequence;
    const std::uint32_t startIndex = static_cast<std::uint32_t>(startSequence % capacity);
    doorbellRingDescriptors[startIndex] = makeDoorbellTokenBandDesc(
        dispatchScratch.front(),
        startSequence,
        frame.frameIndex,
        0);

    writeDoorbellControlRingDescriptors(
        doorbellRingDescriptors,
        startIndex,
        1,
        doorbellControlRing,
        doorbellRingStats);

    doorbellRingHeader.publishedSequence += 1;
    doorbellRingHeader.publishedFrameEpoch = frame.frameIndex;
    doorbellRingHeader.reconfigureEpoch = config.modelEpoch;
    doorbellRingStats.reserveCount += 1;
    doorbellRingStats.stagedDescriptorCount += 1;
    doorbellRingStats.occupancyHighWatermark = std::max(
        doorbellRingStats.occupancyHighWatermark,
        doorbellRingHeader.publishedSequence - doorbellRingHeader.retiredSequence);
    doorbellRingStats.publishedSequence = doorbellRingHeader.publishedSequence;
    doorbellRingStats.retiredSequence = doorbellRingHeader.retiredSequence;
    doorbellRingStats.completedSequence = doorbellRingHeader.completedSequence;
    writeDoorbellControlRingHeader(doorbellRingHeader, doorbellControlRing, doorbellRingStats);

    NpuFrameDispatchDesc frameDispatch{
        .frameIndex = frame.frameIndex,
        .segment = &segment,
        .renderSize = frame.renderSize,
        .dispatches = dispatchScratch.data(),
        .doorbellRingHeader = &doorbellRingHeader,
        .doorbellDescriptors = doorbellRingDescriptors.data() + startIndex,
        .doorbellControlRing = doorbellControlRing,
        .residentParameterArena = residentParameterArena,
        .skipSpillArena = skipSpillArena,
        .modelEpoch = config.modelEpoch,
        .ingressPayloadArena = framePayloadSlot.npuIngressTokenSlab,
        .egressPayloadArena = framePayloadSlot.npuEgressTokenSlab,
        .doorbellDescriptorByteOffset =
            kNpuPersistentDoorbellDescriptorOffset +
            (static_cast<std::uint64_t>(startIndex) * kNpuPersistentDoorbellDescriptorStride),
        .doorbellStartSequence = startSequence,
        .doorbellStartIndex = startIndex,
        .doorbellRingCapacity = doorbellRingHeader.descriptorCapacity,
        .dispatchCount = 1,
    };

    status = config.npuBackend->enqueueFrame(frameDispatch);
    if (!status.succeeded()) {
        return status;
    }

    doorbellRingHeader.retiredSequence = doorbellRingHeader.publishedSequence;
    doorbellRingStats.commitCount += 1;
    doorbellRingStats.publishedSequence = doorbellRingHeader.publishedSequence;
    doorbellRingStats.retiredSequence = doorbellRingHeader.retiredSequence;
    doorbellRingStats.completedSequence = doorbellRingHeader.completedSequence;
    writeDoorbellControlRingHeader(doorbellRingHeader, doorbellControlRing, doorbellRingStats);
    return Status::ok();
}

Status submitFrameReadyAfterExports(
    RuntimeConfig& config,
    const std::vector<TokenStreamSlot*>& slots,
    TokenStreamSlot& frameSyncSlot)
{
    std::vector<TimelineFenceWait> waits;
    waits.reserve(slots.size());
    for (const TokenStreamSlot* slot : slots) {
        if (slot == nullptr) {
            return Status::invalidArgument("frame-conveyor debug copy frame-ready submit contains a null token slot");
        }
        waits.push_back(TimelineFenceWait{.fence = slot->gpuReadyFence, .value = slot->gpuReadyValue});
    }

    GpuSubmitDesc frameReadySubmit{
        .commandContext = CommandContext{},
        .waits = std::move(waits),
        .signals = {TimelineFenceSignal{.fence = frameSyncSlot.gpuReadyFence, .value = frameSyncSlot.gpuReadyValue}},
        .debugName = "APUSR frame-conveyor debug copy complete-seam GPU-ready fence",
    };

    return config.graphicsBackend->submitGpuWork(frameReadySubmit);
}

Status scheduleTokenBandImportAfterFrameNpuDone(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    TokenStreamSlot& frameSyncSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    GraphicsBackend& graphics = *config.graphicsBackend;

    Status status = recordImport(graphics, slot, frame, segment, band);
    if (!status.succeeded()) {
        return status;
    }

    GpuSubmitDesc importSubmit{
        .commandContext = slot.importCommands,
        .waits = {TimelineFenceWait{.fence = frameSyncSlot.npuDoneFence, .value = frameSyncSlot.npuDoneValue}},
        .signals = {TimelineFenceSignal{.fence = slot.slotRetiredFence, .value = slot.slotRetiredValue}},
        .debugName = "APUSR frame-conveyor debug copy GPU token-band import",
    };

    return graphics.submitGpuWork(importSubmit);
}

Status scheduleFrameImportAfterFrameNpuDone(
    RuntimeConfig& config,
    TokenStreamSlot& framePayloadSlot,
    TokenStreamSlot& frameSyncSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment)
{
    GraphicsBackend& graphics = *config.graphicsBackend;

    Status status = recordImport(graphics, framePayloadSlot, frame, segment, makeFrameCommandBand(segment));
    if (!status.succeeded()) {
        return status;
    }

    GpuSubmitDesc importSubmit{
        .commandContext = framePayloadSlot.importCommands,
        .waits = {TimelineFenceWait{.fence = frameSyncSlot.npuDoneFence, .value = frameSyncSlot.npuDoneValue}},
        .signals = {TimelineFenceSignal{.fence = framePayloadSlot.slotRetiredFence, .value = framePayloadSlot.slotRetiredValue}},
        .debugName = "APUSR GPU frame-conveyor import",
    };

    return graphics.submitGpuWork(importSubmit);
}

Status scheduleTokenBandFenceHandoffProbe(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    GraphicsBackend& graphics = *config.graphicsBackend;

    APUSR_TRACE_DIAGNOSTIC("BEGIN ExportAndXrt fence probe");

    // Match the TUNAH proof ordering: arm the XRT run on the ready fence before
    // the D3D12 queue signals that fence. This keeps the diagnostic meaningful
    // without polling or host-signaling an XRT fence.
    Status status = scheduleTokenBandNpu(config, slot, frame, segment, band);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR ExportAndXrt fence probe XRT enqueue failed: " + status.message);
        return status;
    }
    APUSR_TRACE_DIAGNOSTIC("ExportAndXrt fence probe armed XRT wait/run/signal");

    GpuSubmitDesc readySignal{
        .commandContext = CommandContext{},
        .waits = {},
        .signals = {TimelineFenceSignal{.fence = slot.gpuReadyFence, .value = slot.gpuReadyValue}},
        .debugName = "APUSR ExportAndXrt D3D12-ready fence probe",
    };

    status = graphics.submitGpuWork(readySignal);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR ExportAndXrt fence probe D3D12 ready signal failed: " + status.message);
        return status;
    }
    APUSR_TRACE_DIAGNOSTIC("ExportAndXrt fence probe signaled D3D12 ready fence");

    GpuSubmitDesc doneWait{
        .commandContext = CommandContext{},
        .waits = {TimelineFenceWait{.fence = slot.npuDoneFence, .value = slot.npuDoneValue}},
        .signals = {},
        .debugName = "APUSR ExportAndXrt D3D12-done fence probe",
    };

    status = graphics.submitGpuWork(doneWait);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR ExportAndXrt fence probe D3D12 done wait failed: " + status.message);
        return status;
    }

    APUSR_TRACE_DIAGNOSTIC("END ExportAndXrt fence probe");
    return Status::ok();
}

Status scheduleKernelTileProbe(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band,
    DiagnosticTileOverlayDesc* overlay)
{
    GraphicsBackend& graphics = *config.graphicsBackend;
    NpuBackend& npu = *config.npuBackend;

    (void)frame;
    APUSR_TRACE_DIAGNOSTIC("BEGIN KernelTileProbe");

    if (slot.ingressTokenSlab.nativeBuffer.value == nullptr || slot.egressTokenSlab.nativeBuffer.value == nullptr) {
        return Status::invalidArgument("KernelTileProbe requires CPU-visible token slabs");
    }
    if (slot.ingressTokenSlab.byteSize < kernels::debug_tile::kWorstCaseInputWindowBytes || slot.egressTokenSlab.byteSize < kernels::debug_tile::kWorstCaseOutputTileBytes) {
        return Status::invalidArgument("KernelTileProbe token slabs are smaller than the spatial-3x3 microtile contract");
    }

    fillKernelTileProbeInput(slot);

    NpuDispatchDesc npuDesc{
        .segment = segment,
        .band = band,
        .tokenSlotIndex = slot.index,
        .input = slot.npuIngressTokenSlab,
        .output = slot.npuEgressTokenSlab,
        .residentParameterArena = config.residentParameterArena,
        .skipSpillArena = config.skipSpillArena,
        .modelEpoch = config.modelEpoch,
        .waitBeforeRun = TimelineFenceWait{.fence = slot.gpuReadyFence, .value = slot.gpuReadyValue},
        .signalAfterRun = TimelineFenceSignal{.fence = slot.npuDoneFence, .value = slot.npuDoneValue},
        .renderSize = frame.renderSize,
        .payloadContract = cpuDiagnosticPayloadContract(
            kernels::debug_tile::kWorstCaseInputWindowBytes,
            kernels::debug_tile::kWorstCaseOutputTileBytes),
    };

    Status status = npu.enqueue(npuDesc);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR KernelTileProbe XRT enqueue failed: " + status.message);
        return status;
    }
    APUSR_TRACE_DIAGNOSTIC("KernelTileProbe armed XRT wait/run/signal");

    GpuSubmitDesc readySignal{
        .commandContext = CommandContext{},
        .waits = {},
        .signals = {TimelineFenceSignal{.fence = slot.gpuReadyFence, .value = slot.gpuReadyValue}},
        .debugName = "APUSR KernelTileProbe D3D12-ready fence",
    };

    status = graphics.submitGpuWork(readySignal);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR KernelTileProbe D3D12 ready signal failed: " + status.message);
        return status;
    }
    APUSR_TRACE_DIAGNOSTIC("KernelTileProbe signaled D3D12 ready fence");

    status = npu.waitForLastDispatch(npuDesc, kKernelTileProbeTimeoutMs);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR KernelTileProbe XRT wait failed: " + status.message);
        return status;
    }
    APUSR_TRACE_DIAGNOSTIC("KernelTileProbe XRT run completed");

    status = verifyKernelTileProbeOutput(slot);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR " + status.message);
        return status;
    }
    APUSR_TRACE_DIAGNOSTIC("KernelTileProbe verified 18x10 C16 -> 16x8 C16 unscaled 3x3 worker");
    if (overlay != nullptr) {
        publishKernelTileOverlay(slot, *overlay);
        APUSR_TRACE_DIAGNOSTIC("KernelTileOverlay published verified native tile for borrowed-command-list display");
    }

    GpuSubmitDesc doneWait{
        .commandContext = CommandContext{},
        .waits = {TimelineFenceWait{.fence = slot.npuDoneFence, .value = slot.npuDoneValue}},
        .signals = {},
        .debugName = "APUSR KernelTileProbe D3D12-done fence",
    };

    status = graphics.submitGpuWork(doneWait);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR KernelTileProbe D3D12 done wait failed: " + status.message);
        return status;
    }

    APUSR_TRACE_DIAGNOSTIC("END KernelTileProbe");
    return Status::ok();
}


Status scheduleKernelFrameOverlay(
    RuntimeConfig& config,
    TokenStreamSlot& scratchSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band,
    DiagnosticTileOverlayDesc& overlay)
{
    APUSR_TRACE_DIAGNOSTIC("BEGIN KernelFrameOverlay");

    if (scratchSlot.ingressTokenSlab.nativeBuffer.value == nullptr || scratchSlot.egressTokenSlab.nativeBuffer.value == nullptr) {
        return Status::invalidArgument("KernelFrameOverlay requires CPU-visible scratch token slabs");
    }
    if (scratchSlot.ingressTokenSlab.byteSize < kernels::debug_tile::kWorstCaseInputWindowBytes ||
        scratchSlot.egressTokenSlab.byteSize < kernels::debug_tile::kWorstCaseOutputTileBytes) {
        return Status::invalidArgument("KernelFrameOverlay scratch slabs are smaller than the spatial-3x3 microtile contract");
    }

    Status status = ensureKernelFrameOverlayBuffer(config, segment, overlay);
    if (!status.succeeded()) {
        APUSR_TRACE_DIAGNOSTIC("ERROR KernelFrameOverlay overlay buffer allocation failed: " + status.message);
        return status;
    }

    const TokenShape& frameShape = segment.outputBoundary.shape;
    const std::uint32_t tileWidth = kernels::debug_tile::kDefaultMicroTileOutputWidth;
    const std::uint32_t tileRows = kernels::debug_tile::kDefaultMicroTileOutputRows;
    const std::uint32_t tilesX = (frameShape.width + tileWidth - 1u) / tileWidth;
    const std::uint32_t tilesY = (frameShape.height + tileRows - 1u) / tileRows;
    APUSR_TRACE_DIAGNOSTIC(
        "KernelFrameOverlay scheduling full token frame tiles=" + std::to_string(tilesX * tilesY) +
        " grid=" + std::to_string(tilesX) + "x" + std::to_string(tilesY) +
        " tokenShape=" + std::to_string(frameShape.width) + "x" + std::to_string(frameShape.height));

    for (std::uint32_t tileY = 0; tileY < tilesY; ++tileY) {
        for (std::uint32_t tileX = 0; tileX < tilesX; ++tileX) {
            const std::uint32_t outputOriginX = tileX * tileWidth;
            const std::uint32_t outputOriginY = tileY * tileRows;

            fillKernelFrameOverlayTileInput(scratchSlot, frameShape, outputOriginX, outputOriginY);
            status = runKernelFrameOverlayTile(config, scratchSlot, frame, segment, band);
            if (!status.succeeded()) {
                APUSR_TRACE_DIAGNOSTIC(
                    "ERROR KernelFrameOverlay tile failed at tileX=" + std::to_string(tileX) +
                    " tileY=" + std::to_string(tileY) + ": " + status.message);
                return status;
            }
            scatterKernelFrameOverlayTileOutput(scratchSlot, overlay, outputOriginX, outputOriginY);
        }
        APUSR_TRACE_DIAGNOSTIC(
            "KernelFrameOverlay completed tile row " + std::to_string(tileY + 1u) +
            "/" + std::to_string(tilesY));
    }

    overlay.available = true;
    APUSR_TRACE_DIAGNOSTIC(
        "KernelFrameOverlay completed full token-frame native overlay tiles=" + std::to_string(tilesX * tilesY));
    APUSR_TRACE_DIAGNOSTIC("END KernelFrameOverlay");
    return Status::ok();
}

Status scheduleTokenBandExportAndNpu(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    TimelineFenceWait reuseWait{};
    const TimelineFenceWait* reuseWaitPtr = nullptr;
    if (slot.slotRetiredValue > 1) {
        reuseWait = TimelineFenceWait{.fence = slot.slotRetiredFence, .value = slot.slotRetiredValue - 1};
        reuseWaitPtr = &reuseWait;
    }

    Status status = scheduleTokenBandExport(config, slot, frame, segment, band, reuseWaitPtr);
    if (!status.succeeded()) {
        return status;
    }

    return scheduleTokenBandNpu(config, slot, frame, segment, band);
}

Status scheduleTokenBandImport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band)
{
    GraphicsBackend& graphics = *config.graphicsBackend;

    Status status = recordImport(graphics, slot, frame, segment, band);
    if (!status.succeeded()) {
        return status;
    }

    GpuSubmitDesc importSubmit{
        .commandContext = slot.importCommands,
        .waits = {TimelineFenceWait{.fence = slot.npuDoneFence, .value = slot.npuDoneValue}},
        .signals = {TimelineFenceSignal{.fence = slot.slotRetiredFence, .value = slot.slotRetiredValue}},
        .debugName = "APUSR GPU token-band import",
    };

    return graphics.submitGpuWork(importSubmit);
}

} // namespace apusr
