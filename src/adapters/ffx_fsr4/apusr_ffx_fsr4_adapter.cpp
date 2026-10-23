// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/adapters/apusr_ffx_fsr4_adapter.h>
#include <apusr/adapters/apusr_fsr4_boundary.h>
#include <apusr/kernels/apusr_middle_core_resident_abi.h>

#include <algorithm>
#include <utility>


#ifndef APUSR_MIDDLE_CORE_WORKER_GROUPS
#define APUSR_MIDDLE_CORE_WORKER_GROUPS 2
#endif

namespace apusr::adapters {

namespace {

[[nodiscard]] std::uint32_t ceilDiv(const std::uint32_t numerator, const std::uint32_t denominator)
{
    return denominator == 0 ? 0 : (numerator + denominator - 1) / denominator;
}

[[nodiscard]] std::uint32_t kernelFutureRows(const std::uint32_t kernelHeight)
{
    return kernelHeight > 1 ? kernelHeight / 2 : 0;
}


[[nodiscard]] std::uint64_t packagedModelEpochParameterDmaBytes()
{
#if APUSR_PACKAGED_KERNEL_PROFILE == 17
    return kernels::middle_core::kStage1To12ModelEpochParameterDmaBytes;
#else
    return 0;
#endif
}

[[nodiscard]] std::uint64_t packagedHotPathParameterDmaBytesPerRun()
{
#if APUSR_PACKAGED_KERNEL_PROFILE == 17
    return kernels::middle_core::kStage1To12HotPathParameterDmaBytesPerRun;
#else
    return 0;
#endif
}

[[nodiscard]] kernels::middle_core::Stage1To12FrameCapacity packagedMiddleCoreCapacity(
    const DisplaySize maxActiveOutputSize)
{
    switch (selectFsr4ModelResolution(maxActiveOutputSize)) {
    case Fsr4ModelResolution::R1080:
        return kernels::middle_core::kStage1To12R1080FrameCapacity;
    case Fsr4ModelResolution::R2160:
        return kernels::middle_core::kStage1To12R2160FrameCapacity;
    case Fsr4ModelResolution::R4320:
        return kernels::middle_core::kStage1To12R4320FrameCapacity;
    }
    return kernels::middle_core::kStage1To12R2160FrameCapacity;
}

[[nodiscard]] DisplaySize packagedMiddleCoreModelOutputCapacity(const DisplaySize maxActiveOutputSize)
{
#if APUSR_PACKAGED_KERNEL_PROFILE == 17
    const kernels::middle_core::Stage1To12FrameCapacity capacity =
        packagedMiddleCoreCapacity(maxActiveOutputSize);
    return DisplaySize{
        .width = static_cast<std::uint32_t>(capacity.modelOutputWidth),
        .height = static_cast<std::uint32_t>(capacity.modelOutputRows),
    };
#else
    return alignFsr4ModelOutputSize(maxActiveOutputSize);
#endif
}

} // namespace

std::vector<SegmentStageDesc> makeFsr4MiddleCoreStages()
{
    const auto makeStage = [](
                               const std::uint32_t logicalStage,
                               std::string name,
                               std::string op,
                               const StageCadence cadence,
                               const std::uint32_t inputChannels,
                               const std::uint32_t outputChannels,
                               const std::uint32_t inputHeightDivisor,
                               const std::uint32_t outputHeightDivisor,
                               const std::uint32_t spatialKernelWidth,
                               const std::uint32_t spatialKernelHeight,
                               std::string producesSkip = {},
                               std::string consumesSkip = {}) {
        return SegmentStageDesc{
            .logicalStage = logicalStage,
            .name = std::move(name),
            .op = std::move(op),
            .cadence = cadence,
            .inputChannels = inputChannels,
            .outputChannels = outputChannels,
            .inputHeightDivisor = inputHeightDivisor,
            .outputHeightDivisor = outputHeightDivisor,
            .spatialKernelWidth = spatialKernelWidth,
            .spatialKernelHeight = spatialKernelHeight,
            .outputTransport = EdgeTransportPolicy::DirectStream,
            .producesSkip = std::move(producesSkip),
            .consumesSkip = std::move(consumesSkip),
        };
    };

    return {
        makeStage(1, "stage1.convnext_c16", "ConvNextBlock", StageCadence::OneToOne, 16, 16, 2, 2, 3, 3),
        makeStage(2, "stage2.convnext_c16_skip_a", "ConvNextBlock", StageCadence::OneToOne, 16, 16, 2, 2, 3, 3, "skip_a_c16"),
        makeStage(3, "stage3.downsample_c16_to_c32", "FusedConv2D_k2s2b_QuantizedOutput", StageCadence::Downsample2x, 16, 32, 2, 4, 2, 2),
        makeStage(4, "stage4.fasternet_c32", "FasterNetBlock", StageCadence::OneToOne, 32, 32, 4, 4, 3, 3),
        makeStage(5, "stage5.fasternet_c32_skip_b", "FasterNetBlock", StageCadence::OneToOne, 32, 32, 4, 4, 3, 3, "skip_b_c32"),
        makeStage(6, "stage6.downsample_c32_to_c64", "FusedConv2D_k2s2b_QuantizedOutput", StageCadence::Downsample2x, 32, 64, 4, 8, 2, 2),
        makeStage(7, "stage7.fasternet_c64", "FasterNetBlock", StageCadence::OneToOne, 64, 64, 8, 8, 3, 3),
        makeStage(8, "stage8.fasternet_c64", "FasterNetBlock", StageCadence::OneToOne, 64, 64, 8, 8, 3, 3),
        makeStage(9, "stage9.upsample_add_skip_b", "FNB_CT2D_ADD", StageCadence::Upsample2x, 64, 32, 8, 4, 2, 2, {}, "skip_b_c32"),
        makeStage(10, "stage10.fasternet_c32_decoder", "FasterNetBlock", StageCadence::OneToOne, 32, 32, 4, 4, 3, 3),
        makeStage(11, "stage11.upsample_add_skip_a", "FNB_CT2D_ADD", StageCadence::Upsample2x, 32, 16, 4, 2, 2, 2, {}, "skip_a_c16"),
        makeStage(12, "stage12.convnext_c16_decoder", "ConvNextBlock", StageCadence::OneToOne, 16, 16, 2, 2, 3, 3),
    };
}

std::vector<SkipDelayDesc> makeFsr4MiddleCoreSkipDelays()
{
    return {
        SkipDelayDesc{
            .name = "skip_a_c16",
            .producerLogicalStage = 2,
            .consumerLogicalStage = 11,
            .channels = 16,
            .widthDivisor = 2,
            .heightDivisor = 2,
            .transport = EdgeTransportPolicy::BufferedDmaDelay,
            .recommendedLiveRows = 32,
        },
        SkipDelayDesc{
            .name = "skip_b_c32",
            .producerLogicalStage = 5,
            .consumerLogicalStage = 9,
            .channels = 32,
            .widthDivisor = 4,
            .heightDivisor = 4,
            .transport = EdgeTransportPolicy::BufferedDmaDelay,
            .recommendedLiveRows = 24,
        },
    };
}

std::uint32_t estimateFsr4MiddleCoreLookaheadRows()
{
    std::uint32_t futureRows = 0;
    std::vector<SegmentStageDesc> stages = makeFsr4MiddleCoreStages();
    for (auto stage = stages.rbegin(); stage != stages.rend(); ++stage) {
        switch (stage->cadence) {
        case StageCadence::OneToOne:
            futureRows += kernelFutureRows(stage->spatialKernelHeight);
            break;
        case StageCadence::Downsample2x:
            futureRows = futureRows * 2 + (stage->spatialKernelHeight > 0 ? stage->spatialKernelHeight - 1 : 0);
            break;
        case StageCadence::Upsample2x:
            futureRows = ceilDiv(futureRows, 2);
            break;
        }
    }
    return futureRows;
}

SegmentManifest makeFsr4MiddleCoreManifestForActiveOutput(
    const DisplaySize activeOutputSize,
    const std::uint32_t tokenBandRows)
{
    const DisplaySize modelOutputSize = packagedMiddleCoreModelOutputCapacity(activeOutputSize);
    TokenShape inputShape = makeFsr4MiddleBoundaryShapeFromModelOutput(modelOutputSize);
    TokenShape outputShape = inputShape;
    outputShape.layout = TokenLayout::CellM4x8C16;

    TokenBoundary input{
        .name = "fsr4.logical_stage_0.output.c16_int8",
        .shape = inputShape,
        .byteOffset = 0,
        .byteSize = tokenSlabBytes(inputShape),
    };

    TokenBoundary output{
        .name = "fsr4.logical_stage_13.input.c16_int8.m4x8",
        .shape = outputShape,
        .byteOffset = 0,
        .byteSize = tokenSlabBytes(outputShape),
    };

    std::vector<SegmentStageDesc> stages = makeFsr4MiddleCoreStages();
    std::uint32_t lookaheadRows = estimateFsr4MiddleCoreLookaheadRows();
    std::uint32_t outputHaloRows = 1;
    std::uint32_t maxKernelHeight = 1;
    std::uint32_t maxKernelWidth = 1;
    for (const SegmentStageDesc& stage : stages) {
        maxKernelHeight = std::max(maxKernelHeight, stage.spatialKernelHeight);
        maxKernelWidth = std::max(maxKernelWidth, stage.spatialKernelWidth);
    }

    const TokenStreamPlan streamPlan = makeTokenStreamPlan(
        input,
        output,
        tokenBandRows,
        0,
        outputHaloRows,
        lookaheadRows,
        maxKernelHeight,
        maxKernelWidth,
        TokenStreamMode::ContinuousFrameStream);

    return SegmentManifest{
        .name = "fsr4.int8.middle_core",
        .modelFamily = ModelFamily::Fsr4Int8,
        .firstLogicalStage = 1,
        .lastLogicalStage = 12,
        .inputBoundary = std::move(input),
        .outputBoundary = std::move(output),
        .streamPlan = streamPlan,
        .stages = std::move(stages),
        .skipDelays = makeFsr4MiddleCoreSkipDelays(),
    };
}

RuntimeManifest makeFsr4RuntimeManifestForActiveOutput(
    const DisplaySize activeOutputSize,
    const std::uint32_t tokenBandRows)
{
    RuntimeManifest manifest{};
    manifest.name = "apusr.fsr4.int8";
    manifest.segments.push_back(makeFsr4MiddleCoreManifestForActiveOutput(activeOutputSize, tokenBandRows));
    return manifest;
}

RuntimeConfig makeFsr4RuntimeConfig(
    const Fsr4AdapterConfig& adapterConfig,
    GraphicsBackendPtr graphicsBackend,
    NpuBackendPtr npuBackend)
{
    RuntimeManifest manifest = makeFsr4RuntimeManifestForActiveOutput(adapterConfig.maxActiveOutputSize, adapterConfig.tokenBandRows);
    std::uint32_t tokenRingDepth = adapterConfig.tokenRingDepth;
    if (!manifest.segments.empty()) {
        // Product native execution uses frame slots. Each slot owns a complete
        // C16 seam; token bands stay inside the NPU-owned schedule.
        tokenRingDepth = std::max(tokenRingDepth, 2u);
    }

    RuntimeConfig config{
        .vendorPolicy = adapterConfig.vendorPolicy,
        .requirements = RuntimeRequirements{},
        .tokenRingDepth = tokenRingDepth,
        .manifest = std::move(manifest),
        .residentParameterArenaData = nullptr,
        .residentParameterArenaBytes = 0,
        .modelEpochParameterDmaBytes = packagedModelEpochParameterDmaBytes(),
        .hotPathParameterDmaBytesPerRun = packagedHotPathParameterDmaBytesPerRun(),
        .residentParameterArena = NpuBufferView{},
        .skipSpillArenaBytes = 0,
        .skipSpillArena = NpuBufferView{},
        .graphicsBackend = std::move(graphicsBackend),
        .npuBackend = std::move(npuBackend),
    };

    const NativeBringupStage nativeStage = compiledNativeBringupStage();
    config.npuProducesCompleteTransportFrame = packagedNativeKernelProducesCompleteTransportFrame(nativeStage);
    config.npuProducesModelEgressFrame = packagedNativeKernelProducesModelEgressFrame(nativeStage);
    config.diagnosticImportTransportEchoToOutput = false;

    return config;
}

} // namespace apusr::adapters
