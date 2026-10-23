// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/adapters/apusr_fsr4_boundary.h>

#include <string>

namespace apusr::adapters {

const char* fsr4ModelPresetName(const Fsr4ModelPreset preset)
{
    switch (preset) {
    case Fsr4ModelPreset::NativeAA:
        return "nativeaa";
    case Fsr4ModelPreset::Quality:
        return "quality";
    case Fsr4ModelPreset::Balanced:
        return "balanced";
    case Fsr4ModelPreset::Performance:
        return "performance";
    case Fsr4ModelPreset::DynamicResolution:
        return "drs";
    case Fsr4ModelPreset::UltraPerformance:
        return "ultraperf";
    }
    return "unknown";
}

const char* fsr4ModelPresetBundleName(const Fsr4ModelPreset preset)
{
    switch (preset) {
    case Fsr4ModelPreset::NativeAA:
        return "native";
    case Fsr4ModelPreset::Quality:
        return "quality";
    case Fsr4ModelPreset::Balanced:
        return "balanced";
    case Fsr4ModelPreset::Performance:
        return "performance";
    case Fsr4ModelPreset::DynamicResolution:
        return "drs";
    case Fsr4ModelPreset::UltraPerformance:
        return "ultraperf";
    }
    return "unknown";
}

const char* fsr4ModelResolutionName(const Fsr4ModelResolution resolution)
{
    switch (resolution) {
    case Fsr4ModelResolution::R1080:
        return "1080";
    case Fsr4ModelResolution::R2160:
        return "2160";
    case Fsr4ModelResolution::R4320:
        return "4320";
    }
    return "unknown";
}

const char* npuTargetFamilyName(const NpuTargetFamily target)
{
    switch (target) {
    case NpuTargetFamily::Npu1Aie2:
        return "npu1";
    case NpuTargetFamily::Npu2Aie2p:
        return "npu2";
    }
    return "unknown";
}

std::string fsr4ModelPermutationName(const Fsr4ModelPreset preset, const Fsr4ModelResolution resolution)
{
    return std::string(fsr4ModelPresetBundleName(preset)) + "__" + fsr4ModelResolutionName(resolution);
}

const char* fsr4ParameterProfileNameForNpuTarget(const NpuTargetFamily target)
{
    switch (target) {
    case NpuTargetFamily::Npu1Aie2:
    case NpuTargetFamily::Npu2Aie2p:
        return "am020_4x4_base";
    }
    return "unknown";
}

float fsr4UpscaleRatioForQualityMode(const std::uint32_t qualityMode)
{
    switch (qualityMode) {
    case 0:
        return 1.0f;
    case 1:
        return 1.5f;
    case 2:
        return 1.7f;
    case 3:
        return 2.0f;
    case 4:
        return 3.0f;
    default:
        return 0.0f;
    }
}

std::uint32_t fsr4RenderExtentFromDisplayExtent(const std::uint32_t displayExtent, const float upscaleRatio)
{
    if (displayExtent == 0 || upscaleRatio <= 0.0f) {
        return 0;
    }
    return static_cast<std::uint32_t>(static_cast<float>(displayExtent) / upscaleRatio);
}

DisplaySize alignFsr4ModelOutputSize(const DisplaySize activeOutputSize)
{
    return DisplaySize{
        .width = static_cast<std::uint32_t>(alignUp(activeOutputSize.width, 8)),
        .height = static_cast<std::uint32_t>(alignUp(activeOutputSize.height, 8)),
    };
}

Fsr4ModelPreset selectFsr4ModelPreset(
    const RenderSize activeRenderSize,
    const DisplaySize activeOutputSize,
    const UpscalerFeatureFlags& features)
{
    if (features.dynamicResolution) {
        return Fsr4ModelPreset::DynamicResolution;
    }
    if (activeRenderSize.width == 0 || activeOutputSize.width == 0) {
        return Fsr4ModelPreset::NativeAA;
    }

    if (upscaleExtentMeetsRatio(activeRenderSize.width, activeOutputSize.width, 299u, 100u)) {
        return Fsr4ModelPreset::UltraPerformance;
    }
    if (upscaleExtentMeetsRatio(activeRenderSize.width, activeOutputSize.width, 199u, 100u)) {
        return Fsr4ModelPreset::Performance;
    }
    if (upscaleExtentMeetsRatio(activeRenderSize.width, activeOutputSize.width, 169u, 100u)) {
        return Fsr4ModelPreset::Balanced;
    }
    if (upscaleExtentWithinOnePixelOfRatio(activeRenderSize.width, activeOutputSize.width, 3u, 2u)) {
        return Fsr4ModelPreset::Quality;
    }
    return Fsr4ModelPreset::NativeAA;
}

Fsr4ModelResolution selectFsr4ModelResolution(const DisplaySize maxActiveOutputSize)
{
    Fsr4ModelResolution resolution = Fsr4ModelResolution::R1080;
    if (maxActiveOutputSize.width > 1920 || maxActiveOutputSize.height > 1080) {
        resolution = Fsr4ModelResolution::R2160;
    }
    if (maxActiveOutputSize.width > 3840 || maxActiveOutputSize.height > 2160) {
        resolution = Fsr4ModelResolution::R4320;
    }
    return resolution;
}

Fsr4BoundaryTensorDesc makeFsr4IngressTensorDesc(const DisplaySize modelOutputSize, const float quantizationScale)
{
    return Fsr4BoundaryTensorDesc{
        .producerTensorId = "passes:slice_0",
        .consumerTensorId = "passes:quantized_NHWC__encoder2_ResidualBlock_0_body_input_quantization_act_quant_export_handler_QuantizeLinear_output_0",
        .scaleDomain = quantizationScale > 0.0f ? "qscale:stage0_output" : "qscale:from_ffxnpu_logical_metadata",
        .quantizationScale = quantizationScale,
        .shape = makeFsr4MiddleBoundaryShapeFromModelOutput(modelOutputSize),
    };
}

Fsr4BoundaryTensorDesc makeFsr4EgressTensorDesc(const DisplaySize modelOutputSize, const float quantizationScale)
{
    TokenShape shape = makeFsr4MiddleBoundaryShapeFromModelOutput(modelOutputSize);
    shape.layout = TokenLayout::CellM4x8C16;
    return Fsr4BoundaryTensorDesc{
        .producerTensorId = "passes:slice_24",
        .consumerTensorId = "passes:fused_quantized_NHWC__decoder2_ResidualBlock_2_body_input_quantization_act_quant_export_handler_QuantizeLinear_output_0",
        .scaleDomain = quantizationScale > 0.0f ? "qscale:stage12_output" : "qscale:from_ffxnpu_logical_metadata",
        .quantizationScale = quantizationScale,
        .shape = shape,
    };
}

Fsr4RealBoundaryContract makeFsr4RealBoundaryContract(
    const RenderSize activeRenderSize,
    const DisplaySize activeOutputSize,
    const DisplaySize maxActiveOutputSize,
    const UpscalerFeatureFlags& features,
    const NpuTargetFamily npuTarget,
    const float ingressQuantizationScale,
    const float egressQuantizationScale)
{
    const DisplaySize modelOutputSize = alignFsr4ModelOutputSize(activeOutputSize);
    const TokenShape boundaryShape = makeFsr4MiddleBoundaryShapeFromModelOutput(modelOutputSize);
    return Fsr4RealBoundaryContract{
        .preset = selectFsr4ModelPreset(activeRenderSize, activeOutputSize, features),
        .modelResolution = selectFsr4ModelResolution(maxActiveOutputSize),
        .npuTarget = npuTarget,
        .activeRenderSize = activeRenderSize,
        .activeOutputSize = activeOutputSize,
        .maxActiveOutputSize = maxActiveOutputSize,
        .modelOutputSize = modelOutputSize,
        .ingress = makeFsr4IngressTensorDesc(modelOutputSize, ingressQuantizationScale),
        .egress = makeFsr4EgressTensorDesc(modelOutputSize, egressQuantizationScale),
        .tokenBytes = kTokenBytes,
        .logicalWidth = boundaryShape.width,
        .logicalHeight = boundaryShape.height,
        .paddedWidth = tokenPaddedRowWidth(boundaryShape),
        .validRowBytes = tokenValidRowBytes(boundaryShape),
        .rowPitchBytes = tokenRowPitchBytes(boundaryShape),
        .slabBytes = tokenSlabBytes(boundaryShape),
    };
}

Status validateFsr4RealBoundaryContract(const Fsr4RealBoundaryContract& contract)
{
    if (contract.activeRenderSize.width == 0 || contract.activeRenderSize.height == 0 ||
        contract.activeOutputSize.width == 0 || contract.activeOutputSize.height == 0 ||
        contract.modelOutputSize.width == 0 || contract.modelOutputSize.height == 0) {
        return Status::invalidArgument("FSR4 real boundary requires non-zero active render, active output, and model output sizes");
    }
    const DisplaySize expectedModelOutputSize = alignFsr4ModelOutputSize(contract.activeOutputSize);
    if (contract.modelOutputSize.width != expectedModelOutputSize.width ||
        contract.modelOutputSize.height != expectedModelOutputSize.height) {
        return Status::invalidArgument("FSR4 real boundary model output size must be the 8-pixel-aligned active output size");
    }
    if (contract.tokenBytes != kTokenBytes) {
        return Status::invalidArgument("FSR4 real boundary token byte count changed");
    }
    if (contract.ingress.shape.width != contract.egress.shape.width ||
        contract.ingress.shape.height != contract.egress.shape.height ||
        contract.ingress.shape.channels != kFsr4MiddleCoreChannels ||
        contract.egress.shape.channels != kFsr4MiddleCoreChannels) {
        return Status::invalidArgument("FSR4 real boundary must stay symmetric C16 at the middle-core seam");
    }
    if (contract.ingress.shape.elementType != TokenElementType::Int8 ||
        contract.egress.shape.elementType != TokenElementType::Int8 ||
        contract.ingress.shape.layout != TokenLayout::RasterNhwcC16 ||
        contract.egress.shape.layout != TokenLayout::CellM4x8C16) {
        return Status::invalidArgument("FSR4 real boundary must use row-major C16 ingress and m4x8 C16 egress tokens");
    }
    const TokenShape expectedShape = makeFsr4MiddleBoundaryShapeFromModelOutput(contract.modelOutputSize);
    if (contract.ingress.shape.width != expectedShape.width || contract.ingress.shape.height != expectedShape.height) {
        return Status::invalidArgument("FSR4 real boundary tensor shape must derive from the model output extent");
    }
    if (contract.logicalWidth != contract.ingress.shape.width || contract.logicalHeight != contract.ingress.shape.height) {
        return Status::invalidArgument("FSR4 real boundary dimensions must match the ingress tensor shape");
    }
    if (contract.paddedWidth != tokenPaddedRowWidth(contract.ingress.shape) ||
        contract.validRowBytes != tokenValidRowBytes(contract.ingress.shape) ||
        contract.rowPitchBytes != tokenRowPitchBytes(contract.ingress.shape) ||
        contract.slabBytes != tokenSlabBytes(contract.ingress.shape)) {
        return Status::invalidArgument("FSR4 real boundary row layout is inconsistent with the token shape");
    }
    if (contract.ingress.producerTensorId.empty() || contract.ingress.consumerTensorId.empty() ||
        contract.egress.producerTensorId.empty() || contract.egress.consumerTensorId.empty()) {
        return Status::invalidArgument("FSR4 real boundary requires producer and consumer tensor identities");
    }
    return Status::ok();
}

} // namespace apusr::adapters
