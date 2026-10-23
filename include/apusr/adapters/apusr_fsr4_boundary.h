// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_tokens.h>
#include <apusr/apusr_upscaler_common.h>

#include <cstdint>
#include <string>

namespace apusr::adapters {

enum class Fsr4ModelPreset : std::uint32_t {
    NativeAA,
    Quality,
    Balanced,
    Performance,
    DynamicResolution,
    UltraPerformance,
};

enum class Fsr4ModelResolution : std::uint32_t {
    R1080,
    R2160,
    R4320,
};

enum class NpuTargetFamily : std::uint32_t {
    Npu1Aie2,
    Npu2Aie2p,
};

struct Fsr4BoundaryTensorDesc {
    std::string producerTensorId;
    std::string consumerTensorId;
    std::string scaleDomain;
    float quantizationScale = 0.0f;
    TokenShape shape;
};

struct Fsr4RealBoundaryContract {
    Fsr4ModelPreset preset = Fsr4ModelPreset::Balanced;
    Fsr4ModelResolution modelResolution = Fsr4ModelResolution::R2160;
    NpuTargetFamily npuTarget = NpuTargetFamily::Npu2Aie2p;
    // activeRenderSize is the low-resolution game input for this dispatch.
    // activeOutputSize is the visible FSR4 upscale rectangle for this dispatch.
    // maxActiveOutputSize is the context capacity used to select the model family.
    // modelOutputSize is alignUp(activeOutputSize, 8), matching FSR4's model domain.
    RenderSize activeRenderSize;
    DisplaySize activeOutputSize;
    DisplaySize maxActiveOutputSize;
    DisplaySize modelOutputSize;
    Fsr4BoundaryTensorDesc ingress;
    Fsr4BoundaryTensorDesc egress;
    std::uint32_t tokenBytes = kTokenBytes;
    std::uint32_t logicalWidth = 0;
    std::uint32_t logicalHeight = 0;
    std::uint32_t paddedWidth = 0;
    std::uint64_t validRowBytes = 0;
    std::uint64_t rowPitchBytes = 0;
    std::uint64_t slabBytes = 0;
};

[[nodiscard]] const char* fsr4ModelPresetName(Fsr4ModelPreset preset);
[[nodiscard]] const char* fsr4ModelPresetBundleName(Fsr4ModelPreset preset);
[[nodiscard]] const char* fsr4ModelResolutionName(Fsr4ModelResolution resolution);
[[nodiscard]] const char* npuTargetFamilyName(NpuTargetFamily target);
[[nodiscard]] std::string fsr4ModelPermutationName(Fsr4ModelPreset preset, Fsr4ModelResolution resolution);
[[nodiscard]] const char* fsr4ParameterProfileNameForNpuTarget(NpuTargetFamily target);

[[nodiscard]] float fsr4UpscaleRatioForQualityMode(std::uint32_t qualityMode);
[[nodiscard]] std::uint32_t fsr4RenderExtentFromDisplayExtent(std::uint32_t displayExtent, float upscaleRatio);
[[nodiscard]] DisplaySize alignFsr4ModelOutputSize(DisplaySize activeOutputSize);
[[nodiscard]] Fsr4ModelPreset selectFsr4ModelPreset(
    RenderSize activeRenderSize,
    DisplaySize activeOutputSize,
    const UpscalerFeatureFlags& features);
[[nodiscard]] Fsr4ModelResolution selectFsr4ModelResolution(DisplaySize maxActiveOutputSize);
[[nodiscard]] Fsr4BoundaryTensorDesc makeFsr4IngressTensorDesc(DisplaySize modelOutputSize, float quantizationScale = 0.0f);
[[nodiscard]] Fsr4BoundaryTensorDesc makeFsr4EgressTensorDesc(DisplaySize modelOutputSize, float quantizationScale = 0.0f);
[[nodiscard]] Fsr4RealBoundaryContract makeFsr4RealBoundaryContract(
    RenderSize activeRenderSize,
    DisplaySize activeOutputSize,
    DisplaySize maxActiveOutputSize,
    const UpscalerFeatureFlags& features,
    NpuTargetFamily npuTarget,
    float ingressQuantizationScale = 0.0f,
    float egressQuantizationScale = 0.0f);
[[nodiscard]] Status validateFsr4RealBoundaryContract(const Fsr4RealBoundaryContract& contract);

} // namespace apusr::adapters
