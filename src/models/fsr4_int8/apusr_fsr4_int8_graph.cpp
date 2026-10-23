// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_graph.h>

#include <array>
#include <string>
#include <utility>

namespace apusr::models::fsr4_int8 {
namespace {

constexpr std::uint32_t kQualityPresetBitShift = 3;
constexpr std::uint32_t kMaxResBitShift = 2;

constexpr std::uint32_t roundUpDiv(const std::uint32_t value, const std::uint32_t divisor)
{
    return divisor == 0 ? 0 : (value + divisor - 1) / divisor;
}

constexpr std::uint32_t alignDownPow2(const std::uint32_t value, const std::uint32_t shift)
{
    return value >> shift;
}

std::string joinPath(const std::string& a, const char* b)
{
    if (a.empty()) {
        return b != nullptr ? std::string{b} : std::string{};
    }
    return a + "/" + (b != nullptr ? b : "");
}

std::string stageEntryPoint(const std::uint32_t logicalStage)
{
    return std::string{"fsr4_model_v07_i8_pass"} + std::to_string(logicalStage);
}

PassDesc modelPass(
    const std::uint32_t logicalStage,
    const PassKind kind,
    const char* name,
    const std::string& shaderPath,
    const DispatchGrid dispatch)
{
    PassDesc pass{};
    pass.logicalStage = logicalStage;
    pass.kind = kind;
    pass.name = name;
    pass.shaderPath = shaderPath;
    pass.entryPoint = stageEntryPoint(logicalStage);
    pass.dp4aDispatch = dispatch;
    pass.modelPass = true;
    pass.generatedMiddleShader = logicalStage >= 1 && logicalStage <= 12;
    pass.consumesInitializers = logicalStage >= 1 && logicalStage <= 12;
    pass.gpuReferenceRequired = true;

    // Keep the optimization seams explicit. The early C16 stages are the first
    // NPU/dataflow-island candidates from the current AIE work. WMMA is only
    // marked on the dense middle/post operators; pre remains a bookend and RCAS
    // remains a non-model cleanup pass.
    pass.npuIslandCandidate = logicalStage >= 1 && logicalStage <= 3;
    pass.wmmaCandidate = kind == PassKind::ModelMiddle || kind == PassKind::ModelPost;
    return pass;
}

} // namespace

const char* presetName(const Preset preset)
{
    switch (preset) {
    case Preset::NativeAA:
        return "native";
    case Preset::Quality:
        return "quality";
    case Preset::Balanced:
        return "balanced";
    case Preset::Performance:
        return "performance";
    case Preset::UltraPerformance:
        return "ultraperf";
    case Preset::DynamicResolution:
        return "drs";
    }
    return "unknown";
}

const char* presetArtifactName(const Preset preset)
{
    return presetName(preset);
}

const char* frameCapacityName(const FrameCapacity frameCapacity)
{
    switch (frameCapacity) {
    case FrameCapacity::R1080:
        return "1080";
    case FrameCapacity::R2160:
        return "2160";
    case FrameCapacity::R4320:
        return "4320";
    }
    return "unknown";
}

const char* passKindName(const PassKind kind)
{
    switch (kind) {
    case PassKind::SpdAutoExposure:
        return "spd-auto-exposure";
    case PassKind::ModelPre:
        return "model-pre";
    case PassKind::ModelMiddle:
        return "model-middle";
    case PassKind::ModelPost:
        return "model-post";
    case PassKind::Rcas:
        return "rcas";
    case PassKind::DebugView:
        return "debug-view";
    }
    return "unknown";
}

const char* pipelineVariantName(const PipelineVariant variant)
{
    switch (variant) {
    case PipelineVariant::StrictReference:
        return "strict-reference";
    case PipelineVariant::Dp4aTunedCandidate:
        return "cpu-dp4a-tuned";
    case PipelineVariant::Dp4aOptimized:
        return "dp4a-optimized";
    }
    return "unknown";
}

bool pipelineVariantIsStrictReference(const PipelineVariant variant)
{
    return variant == PipelineVariant::StrictReference;
}

bool pipelineVariantAllowsModelChanges(const PipelineVariant variant)
{
    return variant == PipelineVariant::Dp4aTunedCandidate || variant == PipelineVariant::Dp4aOptimized;
}

const char* colorSpaceName(const ColorSpace colorSpace)
{
    switch (colorSpace) {
    case ColorSpace::Linear:
        return "linear";
    case ColorSpace::Nonlinear:
        return "nonlinear";
    case ColorSpace::Srgb:
        return "srgb";
    case ColorSpace::Pq:
        return "pq";
    }
    return "unknown";
}

Preset presetForQualityMode(const UpscalerQualityMode qualityMode)
{
    switch (qualityMode) {
    case UpscalerQualityMode::NativeAA:
        return Preset::NativeAA;
    case UpscalerQualityMode::Quality:
        return Preset::Quality;
    case UpscalerQualityMode::Balanced:
        return Preset::Balanced;
    case UpscalerQualityMode::Performance:
        return Preset::Performance;
    case UpscalerQualityMode::UltraPerformance:
        return Preset::UltraPerformance;
    case UpscalerQualityMode::DynamicResolution:
        return Preset::DynamicResolution;
    case UpscalerQualityMode::Auto:
        return Preset::Balanced;
    }
    return Preset::Balanced;
}

Preset presetForScale(const float displayToRenderScale, const Preset drsPreset)
{
    Preset preset = Preset::NativeAA;
    if (displayToRenderScale >= 1.50f) {
        preset = Preset::Quality;
    }
    if (displayToRenderScale >= 1.69f) {
        preset = Preset::Balanced;
    }
    if (displayToRenderScale >= 1.99f) {
        preset = Preset::Performance;
    }
    if (displayToRenderScale >= 2.99f) {
        preset = Preset::UltraPerformance;
    }
    return drsPreset == Preset::DynamicResolution ? preset : drsPreset;
}

Preset presetForDimensions(const RenderSize renderSize, const DisplaySize displaySize)
{
    if (renderSize.width == 0 || displaySize.width == 0) {
        return Preset::NativeAA;
    }
    if (upscaleExtentMeetsRatio(renderSize.width, displaySize.width, 299u, 100u)) {
        return Preset::UltraPerformance;
    }
    if (upscaleExtentMeetsRatio(renderSize.width, displaySize.width, 199u, 100u)) {
        return Preset::Performance;
    }
    if (upscaleExtentMeetsRatio(renderSize.width, displaySize.width, 169u, 100u)) {
        return Preset::Balanced;
    }
    if (upscaleExtentWithinOnePixelOfRatio(renderSize.width, displaySize.width, 3u, 2u)) {
        return Preset::Quality;
    }
    return Preset::NativeAA;
}

FrameCapacityLimits frameCapacityLimits(const FrameCapacity frameCapacity)
{
    switch (frameCapacity) {
    case FrameCapacity::R1080:
        return FrameCapacityLimits{.frameCapacity = FrameCapacity::R1080, .maxWidth = 1920, .maxHeight = 1080};
    case FrameCapacity::R2160:
        return FrameCapacityLimits{.frameCapacity = FrameCapacity::R2160, .maxWidth = 3840, .maxHeight = 2160};
    case FrameCapacity::R4320:
        return FrameCapacityLimits{.frameCapacity = FrameCapacity::R4320, .maxWidth = 7680, .maxHeight = 4320};
    }
    return FrameCapacityLimits{};
}

bool displayFitsFrameCapacity(const DisplaySize displaySize, const FrameCapacity frameCapacity)
{
    const FrameCapacityLimits limits = frameCapacityLimits(frameCapacity);
    return displaySize.width <= limits.maxWidth && displaySize.height <= limits.maxHeight;
}

bool isReferenceDisplaySizeSupported(const DisplaySize displaySize)
{
    return displayFitsFrameCapacity(displaySize, FrameCapacity::R4320);
}

FrameCapacity frameCapacityForDisplay(const DisplaySize displaySize)
{
    // Match ffxint8's maxResolution selection: the 1080 class is used only
    // when both dimensions fit 1920x1080; the 2160 class is used only when
    // both dimensions fit 3840x2160; otherwise the 4320 class is selected.
    // Oversized displays are rejected by backend/front-end validation instead
    // of silently inventing a fourth capacity.
    if (displayFitsFrameCapacity(displaySize, FrameCapacity::R1080)) {
        return FrameCapacity::R1080;
    }
    if (displayFitsFrameCapacity(displaySize, FrameCapacity::R2160)) {
        return FrameCapacity::R2160;
    }
    return FrameCapacity::R4320;
}

const char* passesShaderName(const FrameCapacity frameCapacity)
{
    switch (frameCapacity) {
    case FrameCapacity::R1080:
        return "passes_1080.hlsl";
    case FrameCapacity::R2160:
        return "passes_2160.hlsl";
    case FrameCapacity::R4320:
        return "passes_4320.hlsl";
    }
    return "passes_2160.hlsl";
}

std::uint32_t maxResolutionPermutationValue(const FrameCapacity frameCapacity)
{
    switch (frameCapacity) {
    case FrameCapacity::R1080:
        return 0;
    case FrameCapacity::R2160:
        return 1;
    case FrameCapacity::R4320:
        return 2;
    }
    return 1;
}

std::uint32_t presetPermutationValue(const Preset preset)
{
    // Values must match ffxint8 fsr4_shaders::Preset exactly.
    switch (preset) {
    case Preset::NativeAA:
        return 0;
    case Preset::Quality:
        return 1;
    case Preset::Balanced:
        return 2;
    case Preset::Performance:
        return 3;
    case Preset::DynamicResolution:
        return 4;
    case Preset::UltraPerformance:
        return 5;
    }
    return 2;
}

std::uint32_t colorSpacePermutationValue(const ColorSpace colorSpace)
{
    // Mirrors ffxint8 GetColorspace(): linear=0, generic nonlinear=1, sRGB=2, PQ=3.
    switch (colorSpace) {
    case ColorSpace::Linear:
        return 0;
    case ColorSpace::Nonlinear:
        return 1;
    case ColorSpace::Srgb:
        return 2;
    case ColorSpace::Pq:
        return 3;
    }
    return 0;
}

std::uint32_t permutationKey(const PermutationDesc& desc)
{
    // Keep bit packing identical to ffxint8 internal/shader_selector.h.
    std::uint32_t key = 0;
    if (desc.debugVisualize) {
        key |= 1u;
    }
    key <<= 1;
    if (desc.colorSpace == ColorSpace::Pq) {
        key |= 1u;
    }
    key <<= 1;
    if (desc.colorSpace == ColorSpace::Srgb) {
        key |= 1u;
    }
    key <<= 1;
    if (desc.colorSpace != ColorSpace::Linear) {
        key |= 1u;
    }
    key <<= 1;
    if (desc.jitteredMotionVectors) {
        key |= 1u;
    }
    key <<= 1;
    if (desc.autoExposure) {
        key |= 1u;
    }
    key <<= 1;
    if (desc.lowResMotionVectors) {
        key |= 1u;
    }
    key <<= 1;
    if (desc.depthInverted) {
        key |= 1u;
    }
    key <<= 1;
    if (desc.wmma) {
        key |= 1u;
    }
    key <<= kMaxResBitShift;
    key |= maxResolutionPermutationValue(desc.frameCapacity);
    key <<= kQualityPresetBitShift;
    key |= presetPermutationValue(desc.preset);
    return key;
}

ProductPermutationMatrixDesc defaultDx12Dp4aProductPermutationMatrix(const bool includeDeveloperDebugVisualization)
{
    return ProductPermutationMatrixDesc{
        .presets = {
            Preset::NativeAA,
            Preset::Quality,
            Preset::Balanced,
            Preset::Performance,
            Preset::DynamicResolution,
            Preset::UltraPerformance,
        },
        .frameCapacities = {FrameCapacity::R1080, FrameCapacity::R2160, FrameCapacity::R4320},
        .colorSpaces = {ColorSpace::Linear, ColorSpace::Nonlinear, ColorSpace::Srgb, ColorSpace::Pq},
        .includeDp4a = true,
        .includeWmma = false,
        .includeDepthInverted = true,
        .includeLowResMotionVectors = true,
        .includeAutoExposure = true,
        .includeJitteredMotionVectors = true,
        .includeDebugVisualization = includeDeveloperDebugVisualization,
    };
}

std::vector<PermutationDesc> enumerateProductPermutations(const ProductPermutationMatrixDesc& matrix)
{
    std::vector<PermutationDesc> permutations;
    permutations.reserve(static_cast<std::size_t>(productPermutationCount(matrix)));

    const bool wmmaValues[] = {false, true};
    const bool boolValues[] = {false, true};
    for (const Preset preset : matrix.presets) {
        for (const FrameCapacity frameCapacity : matrix.frameCapacities) {
            for (const ColorSpace colorSpace : matrix.colorSpaces) {
                for (const bool wmma : wmmaValues) {
                    if ((!wmma && !matrix.includeDp4a) || (wmma && !matrix.includeWmma)) {
                        continue;
                    }
                    for (const bool depthInverted : boolValues) {
                        if (depthInverted && !matrix.includeDepthInverted) {
                            continue;
                        }
                        for (const bool lowResMotionVectors : boolValues) {
                            if (lowResMotionVectors && !matrix.includeLowResMotionVectors) {
                                continue;
                            }
                            for (const bool autoExposure : boolValues) {
                                if (autoExposure && !matrix.includeAutoExposure) {
                                    continue;
                                }
                                for (const bool jitteredMotionVectors : boolValues) {
                                    if (jitteredMotionVectors && !matrix.includeJitteredMotionVectors) {
                                        continue;
                                    }
                                    for (const bool debugVisualize : boolValues) {
                                        if (debugVisualize && !matrix.includeDebugVisualization) {
                                            continue;
                                        }
                                        permutations.push_back(PermutationDesc{
                                            .preset = preset,
                                            .frameCapacity = frameCapacity,
                                            .wmma = wmma,
                                            .depthInverted = depthInverted,
                                            .lowResMotionVectors = lowResMotionVectors,
                                            .autoExposure = autoExposure,
                                            .jitteredMotionVectors = jitteredMotionVectors,
                                            .colorSpace = colorSpace,
                                            .debugVisualize = debugVisualize,
                                        });
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return permutations;
}

std::uint64_t productPermutationCount(const ProductPermutationMatrixDesc& matrix)
{
    const std::uint64_t backendCount = (matrix.includeDp4a ? 1ull : 0ull) + (matrix.includeWmma ? 1ull : 0ull);
    if (backendCount == 0) {
        return 0;
    }
    const std::uint64_t depthCount = matrix.includeDepthInverted ? 2ull : 1ull;
    const std::uint64_t mvCount = matrix.includeLowResMotionVectors ? 2ull : 1ull;
    const std::uint64_t exposureCount = matrix.includeAutoExposure ? 2ull : 1ull;
    const std::uint64_t jitterCount = matrix.includeJitteredMotionVectors ? 2ull : 1ull;
    const std::uint64_t debugCount = matrix.includeDebugVisualization ? 2ull : 1ull;
    return static_cast<std::uint64_t>(matrix.presets.size()) *
        static_cast<std::uint64_t>(matrix.frameCapacities.size()) *
        static_cast<std::uint64_t>(matrix.colorSpaces.size()) *
        backendCount * depthCount * mvCount * exposureCount * jitterCount * debugCount;
}

DispatchGrid dp4aDispatchForLogicalStage(
    const std::uint32_t logicalStage,
    const DisplaySize displaySize,
    const PipelineVariant variant)
{
    const std::uint32_t width = displaySize.width;
    const std::uint32_t height = displaySize.height;
    if (width == 0 || height == 0) {
        return {};
    }

    switch (logicalStage) {
    case 0:
        return DispatchGrid{.groupsX = roundUpDiv(width, 16), .groupsY = roundUpDiv(height, 16), .groupsZ = 1};
    case 1:
    case 2: {
        const std::uint32_t ew = alignDownPow2(width, 1);
        const std::uint32_t eh = alignDownPow2(height, 1);
        const std::uint32_t tileRows = variant == PipelineVariant::Dp4aOptimized ? 2u : 1u;
        return DispatchGrid{.groupsX = roundUpDiv(ew, 64), .groupsY = roundUpDiv(eh, tileRows), .groupsZ = 1};
    }
    case 3:
    case 4:
    case 5:
    case 10:
    case 11: {
        const std::uint32_t ew = alignDownPow2(width, 2);
        const std::uint32_t eh = alignDownPow2(height, 2);
        return DispatchGrid{.groupsX = roundUpDiv(ew, 64), .groupsY = eh, .groupsZ = 1};
    }
    case 6:
    case 7:
    case 8: {
        const std::uint32_t ew = alignDownPow2(width, 3);
        const std::uint32_t eh = alignDownPow2(height, 3);
        return DispatchGrid{.groupsX = roundUpDiv(ew, 64), .groupsY = eh, .groupsZ = 1};
    }
    case 9: {
        const std::uint32_t ew = alignDownPow2(width, 3);
        const std::uint32_t eh = alignDownPow2(height, 3);
        return DispatchGrid{.groupsX = roundUpDiv(ew, 8), .groupsY = roundUpDiv(eh, 8), .groupsZ = 1};
    }
    case 12: {
        const std::uint32_t ew = alignDownPow2(width, 1);
        const std::uint32_t eh = alignDownPow2(height, 1);
        return DispatchGrid{.groupsX = roundUpDiv(ew, 64), .groupsY = eh, .groupsZ = 1};
    }
    case 13:
        return DispatchGrid{.groupsX = roundUpDiv(width, 16), .groupsY = roundUpDiv(height, 16), .groupsZ = 1};
    default:
        return {};
    }
}

ReferenceArtifactPaths referenceArtifactPaths(const Preset preset, const FrameCapacity frameCapacity)
{
    const std::string root = "models/fsr4_int8/reference/fsr4_4_0_2_int8";
    const std::string presetRoot = root + "/" + presetArtifactName(preset);
    const std::string commonRoot = root + "/common";
    const std::string includeRoot = root + "/include";
    const std::string fsr4IncludeRoot = includeRoot + "/fsr4";
    ReferenceArtifactPaths paths{
        .presetRoot = presetRoot,
        .includeRoot = includeRoot,
        .fsr4IncludeRoot = fsr4IncludeRoot,
        .preShader = joinPath(presetRoot, "pre.hlsl"),
        .passesShader = joinPath(presetRoot, passesShaderName(frameCapacity)),
        .postShader = joinPath(presetRoot, "post.hlsl"),
        .initializers = joinPath(presetRoot, "initializers.bin"),
        .autoExposureShader = joinPath(commonRoot, "spd_auto_exposure.hlsl"),
        .rcasShader = joinPath(commonRoot, "rcas.hlsl"),
        .debugViewShader = joinPath(commonRoot, "debug_view.hlsl"),
        .includeRoots = {},
    };
    paths.includeRoots.push_back(presetRoot);
    paths.includeRoots.push_back(commonRoot);
    paths.includeRoots.push_back(includeRoot);
    paths.includeRoots.push_back(fsr4IncludeRoot);
    return paths;
}

GraphDesc makeFsr4Int8Graph(
    const PipelineVariant variant,
    const Preset preset,
    const FrameCapacity frameCapacity,
    const DisplaySize displaySize)
{
    GraphDesc graph{};
    graph.variant = variant;
    graph.preset = preset;
    graph.frameCapacity = frameCapacity;
    graph.displaySize = displaySize;
    graph.artifacts = referenceArtifactPaths(preset, frameCapacity);
    graph.passes.reserve(17);

    graph.passes.push_back(PassDesc{
        .logicalStage = 0,
        .kind = PassKind::SpdAutoExposure,
        .name = "spd_auto_exposure",
        .shaderPath = graph.artifacts.autoExposureShader,
        .entryPoint = "main",
        .dp4aDispatch = {},
        .modelPass = false,
        .optional = true,
        .gpuReferenceRequired = true,
    });
    graph.passes.push_back(modelPass(0, PassKind::ModelPre, "fsr4_pre", graph.artifacts.preShader, dp4aDispatchForLogicalStage(0, displaySize, variant)));
    graph.passes.back().entryPoint = "main";
    static constexpr std::array<const char*, 12> kMiddleNames{
        "fsr4_pass_1", "fsr4_pass_2", "fsr4_pass_3", "fsr4_pass_4",
        "fsr4_pass_5", "fsr4_pass_6", "fsr4_pass_7", "fsr4_pass_8",
        "fsr4_pass_9", "fsr4_pass_10", "fsr4_pass_11", "fsr4_pass_12"};
    for (std::uint32_t stage = 1; stage <= 12; ++stage) {
        graph.passes.push_back(modelPass(
            stage,
            PassKind::ModelMiddle,
            kMiddleNames[stage - 1],
            graph.artifacts.passesShader,
            dp4aDispatchForLogicalStage(stage, displaySize, variant)));
    }
    graph.passes.push_back(modelPass(13, PassKind::ModelPost, "fsr4_post", graph.artifacts.postShader, dp4aDispatchForLogicalStage(13, displaySize, variant)));
    graph.passes.back().entryPoint = "main";
    graph.passes.push_back(PassDesc{
        .logicalStage = 14,
        .kind = PassKind::Rcas,
        .name = "rcas",
        .shaderPath = graph.artifacts.rcasShader,
        .entryPoint = "main",
        .dp4aDispatch = dp4aDispatchForLogicalStage(13, displaySize, variant),
        .optional = true,
        .gpuReferenceRequired = true,
    });
    graph.passes.push_back(PassDesc{
        .logicalStage = 15,
        .kind = PassKind::DebugView,
        .name = "debug_view",
        .shaderPath = graph.artifacts.debugViewShader,
        .entryPoint = "main",
        .dp4aDispatch = DispatchGrid{.groupsX = roundUpDiv(displaySize.width, 8), .groupsY = roundUpDiv(displaySize.height, 8), .groupsZ = 1},
        .optional = true,
        .gpuReferenceRequired = true,
    });
    return graph;
}

GraphDesc makeReferenceGraph(const Preset preset, const FrameCapacity frameCapacity, const DisplaySize displaySize)
{
    return makeFsr4Int8Graph(PipelineVariant::StrictReference, preset, frameCapacity, displaySize);
}

GraphDesc makeDp4aTunedCandidateGraph(const Preset preset, const FrameCapacity frameCapacity, const DisplaySize displaySize)
{
    return makeFsr4Int8Graph(PipelineVariant::Dp4aTunedCandidate, preset, frameCapacity, displaySize);
}

GraphDesc makeDp4aOptimizedGraph(const Preset preset, const FrameCapacity frameCapacity, const DisplaySize displaySize)
{
    return makeFsr4Int8Graph(PipelineVariant::Dp4aOptimized, preset, frameCapacity, displaySize);
}



} // namespace apusr::models::fsr4_int8
