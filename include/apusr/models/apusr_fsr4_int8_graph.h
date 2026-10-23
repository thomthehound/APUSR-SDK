// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_upscaler_common.h>

#include <cstdint>
#include <string>
#include <vector>

namespace apusr::models::fsr4_int8 {

enum class Preset : std::uint32_t {
    // Keep these values in lock-step with ffxint8 fsr4_shaders::Preset.
    NativeAA = 0,
    Quality = 1,
    Balanced = 2,
    Performance = 3,
    DynamicResolution = 4,
    UltraPerformance = 5,
};

enum class FrameCapacity : std::uint32_t {
    R1080 = 1080,
    R2160 = 2160,
    R4320 = 4320,
};

enum class PassKind : std::uint32_t {
    SpdAutoExposure,
    ModelPre,
    ModelMiddle,
    ModelPost,
    Rcas,
    DebugView,
};

enum class PipelineVariant : std::uint32_t {
    StrictReference,
    Dp4aTunedCandidate,
    Dp4aOptimized,
};

enum class ColorSpace : std::uint32_t {
    Linear = 0,
    Nonlinear = 1,
    Srgb = 2,
    Pq = 3,
};

struct PermutationDesc {
    Preset preset = Preset::Balanced;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    bool wmma = false;
    bool depthInverted = false;
    bool lowResMotionVectors = false;
    bool autoExposure = false;
    bool jitteredMotionVectors = false;
    ColorSpace colorSpace = ColorSpace::Linear;
    bool debugVisualize = false;
};

struct FrameCapacityLimits {
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    std::uint32_t maxWidth = 3840;
    std::uint32_t maxHeight = 2160;
};

struct ProductPermutationMatrixDesc {
    std::vector<Preset> presets;
    std::vector<FrameCapacity> frameCapacities;
    std::vector<ColorSpace> colorSpaces;
    bool includeDp4a = true;
    bool includeWmma = false;
    bool includeDepthInverted = true;
    bool includeLowResMotionVectors = true;
    bool includeAutoExposure = true;
    bool includeJitteredMotionVectors = true;
    bool includeDebugVisualization = false;
};

struct ReferenceArtifactPaths {
    std::string presetRoot;
    std::string includeRoot;
    std::string fsr4IncludeRoot;
    std::string preShader;
    std::string passesShader;
    std::string postShader;
    std::string initializers;
    std::string autoExposureShader;
    std::string rcasShader;
    std::string debugViewShader;
    std::vector<std::string> includeRoots;
};

struct PassDesc {
    std::uint32_t logicalStage = 0;
    PassKind kind = PassKind::ModelMiddle;
    const char* name = nullptr;
    std::string shaderPath;
    std::string entryPoint;
    DispatchGrid dp4aDispatch;
    bool modelPass = false;
    bool generatedMiddleShader = false;
    bool consumesInitializers = false;
    bool optional = false;
    bool gpuReferenceRequired = true;
    bool wmmaCandidate = false;
    bool npuIslandCandidate = false;
};

struct GraphDesc {
    PipelineVariant variant = PipelineVariant::StrictReference;
    Preset preset = Preset::Balanced;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    DisplaySize displaySize;
    ReferenceArtifactPaths artifacts;
    std::vector<PassDesc> passes;
};

[[nodiscard]] const char* presetName(Preset preset);
[[nodiscard]] const char* presetArtifactName(Preset preset);
[[nodiscard]] const char* frameCapacityName(FrameCapacity frameCapacity);
[[nodiscard]] const char* passKindName(PassKind kind);
[[nodiscard]] const char* pipelineVariantName(PipelineVariant variant);
[[nodiscard]] bool pipelineVariantIsStrictReference(PipelineVariant variant);
[[nodiscard]] bool pipelineVariantAllowsModelChanges(PipelineVariant variant);
[[nodiscard]] const char* colorSpaceName(ColorSpace colorSpace);

[[nodiscard]] Preset presetForQualityMode(UpscalerQualityMode qualityMode);
[[nodiscard]] Preset presetForScale(float displayToRenderScale, Preset drsPreset = Preset::DynamicResolution);
[[nodiscard]] Preset presetForDimensions(RenderSize renderSize, DisplaySize displaySize);
[[nodiscard]] FrameCapacityLimits frameCapacityLimits(FrameCapacity frameCapacity);
[[nodiscard]] bool displayFitsFrameCapacity(DisplaySize displaySize, FrameCapacity frameCapacity);
[[nodiscard]] bool isReferenceDisplaySizeSupported(DisplaySize displaySize);
[[nodiscard]] FrameCapacity frameCapacityForDisplay(DisplaySize displaySize);
[[nodiscard]] const char* passesShaderName(FrameCapacity frameCapacity);
[[nodiscard]] std::uint32_t maxResolutionPermutationValue(FrameCapacity frameCapacity);
[[nodiscard]] std::uint32_t presetPermutationValue(Preset preset);
[[nodiscard]] std::uint32_t colorSpacePermutationValue(ColorSpace colorSpace);
[[nodiscard]] std::uint32_t permutationKey(const PermutationDesc& desc);
[[nodiscard]] ProductPermutationMatrixDesc defaultDx12Dp4aProductPermutationMatrix(bool includeDeveloperDebugVisualization = false);
[[nodiscard]] std::vector<PermutationDesc> enumerateProductPermutations(const ProductPermutationMatrixDesc& matrix);
[[nodiscard]] std::uint64_t productPermutationCount(const ProductPermutationMatrixDesc& matrix);

[[nodiscard]] DispatchGrid dp4aDispatchForLogicalStage(std::uint32_t logicalStage, DisplaySize displaySize);
[[nodiscard]] ReferenceArtifactPaths referenceArtifactPaths(Preset preset, FrameCapacity frameCapacity);
[[nodiscard]] GraphDesc makeFsr4Int8Graph(PipelineVariant variant, Preset preset, FrameCapacity frameCapacity, DisplaySize displaySize);
[[nodiscard]] GraphDesc makeReferenceGraph(Preset preset, FrameCapacity frameCapacity, DisplaySize displaySize);
[[nodiscard]] GraphDesc makeDp4aTunedCandidateGraph(Preset preset, FrameCapacity frameCapacity, DisplaySize displaySize);
[[nodiscard]] GraphDesc makeDp4aOptimizedGraph(Preset preset, FrameCapacity frameCapacity, DisplaySize displaySize);


} // namespace apusr::models::fsr4_int8
