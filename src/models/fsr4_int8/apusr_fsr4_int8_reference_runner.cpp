// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_reference_runner.h>
#include <apusr/generated/apusr_fsr4_reference_initializers.h>
#include <apusr/generated/apusr_fsr4_reference_pass0_weights.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace apusr::models::fsr4_int8 {
namespace {

constexpr std::uint32_t roundUpDiv(const std::uint32_t value, const std::uint32_t divisor)
{
    return divisor == 0 ? 0 : (value + divisor - 1) / divisor;
}

constexpr std::uint32_t alignUp(const std::uint32_t value, const std::uint32_t alignment)
{
    return alignment == 0 ? value : roundUpDiv(value, alignment) * alignment;
}

std::string define(const char* name, const std::uint32_t value)
{
    return std::string{name} + "=" + std::to_string(value);
}

std::uint32_t frameCapacityDefineValue(const FrameCapacity frameCapacity)
{
    switch (frameCapacity) {
    case FrameCapacity::R1080: return 0;
    case FrameCapacity::R2160: return 1;
    case FrameCapacity::R4320: return 2;
    }
    return 1;
}

std::uint32_t boolDefine(const bool value) noexcept
{
    return value ? 1u : 0u;
}

std::uint32_t colorSpaceDefineValue(const ColorSpace colorSpace) noexcept
{
    return colorSpacePermutationValue(colorSpace);
}

const char* optimizedPresetDefine(const Preset preset) noexcept
{
    switch (preset) {
    case Preset::NativeAA: return "APUSR_FSR4_I8_PRESET_NATIVE";
    case Preset::Quality: return "APUSR_FSR4_I8_PRESET_QUALITY";
    case Preset::Balanced: return "APUSR_FSR4_I8_PRESET_BALANCED";
    case Preset::Performance: return "APUSR_FSR4_I8_PRESET_PERFORMANCE";
    case Preset::DynamicResolution: return "APUSR_FSR4_I8_PRESET_DRS";
    case Preset::UltraPerformance: return "APUSR_FSR4_I8_PRESET_ULTRAPERF";
    }
    return "APUSR_FSR4_I8_PRESET_BALANCED";
}

const char* optimizedCapacityDefine(const FrameCapacity frameCapacity) noexcept
{
    switch (frameCapacity) {
    case FrameCapacity::R1080: return "APUSR_FSR4_I8_CAPACITY_1080";
    case FrameCapacity::R2160: return "APUSR_FSR4_I8_CAPACITY_2160";
    case FrameCapacity::R4320: return "APUSR_FSR4_I8_CAPACITY_4320";
    }
    return "APUSR_FSR4_I8_CAPACITY_2160";
}

DispatchGrid middleDispatch(const std::uint32_t stage, const DisplaySize alignedUpscaleSize, const PipelineVariant pipelineVariant)
{
    const std::uint32_t w0 = alignedUpscaleSize.width / 2u;
    const std::uint32_t h0 = alignedUpscaleSize.height / 2u;
    const std::uint32_t w1 = alignedUpscaleSize.width / 4u;
    const std::uint32_t h1 = alignedUpscaleSize.height / 4u;
    const std::uint32_t w2 = alignedUpscaleSize.width / 8u;
    const std::uint32_t h2 = alignedUpscaleSize.height / 8u;

    switch (stage) {
    case 1:
    case 2: {
        const std::uint32_t tileRows = pipelineVariant == PipelineVariant::Dp4aOptimized ? 2u : 1u;
        return DispatchGrid{.groupsX = roundUpDiv(w0, 64), .groupsY = roundUpDiv(h0, tileRows), .groupsZ = 1};
    }
    case 3:
    case 4:
    case 5:
        return DispatchGrid{.groupsX = roundUpDiv(w1, 64), .groupsY = h1, .groupsZ = 1};
    case 6:
    case 7:
    case 8:
        return DispatchGrid{.groupsX = roundUpDiv(w2, 64), .groupsY = h2, .groupsZ = 1};
    case 9:
        return DispatchGrid{.groupsX = roundUpDiv(w2, 8), .groupsY = roundUpDiv(h2, 8), .groupsZ = 1};
    case 10:
    case 11:
        return DispatchGrid{.groupsX = roundUpDiv(w1, 64), .groupsY = h1, .groupsZ = 1};
    case 12:
        return DispatchGrid{.groupsX = roundUpDiv(w0, 64), .groupsY = h0, .groupsZ = 1};
    default:
        return {};
    }
}

ReferenceShaderCompileDesc shaderDescForPass(const PassDesc& pass, const GraphDesc& graph, const PermutationDesc& permutation)
{
    ReferenceShaderCompileDesc shader{};
    shader.name = pass.name;
    shader.kind = pass.kind;
    shader.logicalStage = pass.logicalStage;
    shader.shaderPath = pass.shaderPath;
    shader.entryPoint = pass.entryPoint;
    shader.includeRoots = graph.artifacts.includeRoots;

    const bool optimized = graph.variant == PipelineVariant::Dp4aOptimized;
    const bool optimizedPass1 = optimized && pass.kind == PassKind::ModelMiddle && pass.logicalStage == 1;
    const bool optimizedPass2 = optimized && pass.kind == PassKind::ModelMiddle && pass.logicalStage == 2;
    if (optimizedPass1) {
        shader.shaderPath = "shaders/dx12/optimized/apusr_fsr4_i8_pass1_tiled_exact.hlsl";
        shader.entryPoint = "apusr_fsr4_i8_pass1_tiled_exact";
    } else if (optimizedPass2) {
        shader.shaderPath = "shaders/dx12/optimized/apusr_fsr4_i8_pass2_tiled_exact.hlsl";
        shader.entryPoint = "apusr_fsr4_i8_pass2_tiled_exact";
    }

    shader.defines.push_back(define("FFX_GPU", 1));
    shader.defines.push_back(define("FFX_HLSL", 1));
    shader.defines.push_back(define("FFX_HLSL_SM", 66));
    if (pass.kind == PassKind::SpdAutoExposure) {
        return shader;
    }
    if (pass.kind == PassKind::Rcas) {
        shader.defines.push_back(define("FFX_MLSR_AUTOEXPOSURE_ENABLED", boolDefine(permutation.autoExposure)));
        shader.defines.push_back(define("FFX_MLSR_COLORSPACE", colorSpaceDefineValue(permutation.colorSpace)));
        return shader;
    }

    shader.defines.push_back(define("FFX_HALF", 1));
    shader.defines.push_back(define("FFX_MLSR_RESOLUTION", frameCapacityDefineValue(permutation.frameCapacity)));
    shader.defines.push_back(define("WMMA_ENABLED", boolDefine(permutation.wmma)));
    shader.defines.push_back(define("DOT4_ENABLED", boolDefine(!permutation.wmma)));
    shader.defines.push_back(define("FFX_MLSR_DEPTH_INVERTED", boolDefine(permutation.depthInverted)));
    shader.defines.push_back(define("FFX_MLSR_LOW_RES_MV", boolDefine(permutation.lowResMotionVectors)));
    shader.defines.push_back(define("FFX_MLSR_JITTERED_MOTION_VECTORS", boolDefine(permutation.jitteredMotionVectors)));
    shader.defines.push_back(define("FFX_MLSR_AUTOEXPOSURE_ENABLED", boolDefine(permutation.autoExposure)));
    shader.defines.push_back(define("FFX_MLSR_COLORSPACE", colorSpaceDefineValue(permutation.colorSpace)));
    shader.defines.push_back(define("FFX_DEBUG_VISUALIZE", boolDefine(permutation.debugVisualize)));

    if (optimized && pass.kind == PassKind::ModelPre) {
        shader.defines.push_back(define("APUSR_FSR4_I8_PADDED_STAGE0_CARRIER", 1));
    }
    if (optimizedPass1 || optimizedPass2) {
        shader.defines.push_back(define(optimizedPresetDefine(permutation.preset), 1));
        shader.defines.push_back(define(optimizedCapacityDefine(permutation.frameCapacity), 1));
    } else if (pass.kind == PassKind::ModelMiddle) {
        shader.defines.push_back(define((std::string{"MLSR_PASS_"} + std::to_string(pass.logicalStage)).c_str(), 1));
    }
    if (optimized && pass.kind == PassKind::ModelMiddle && pass.logicalStage == 12) {
        shader.defines.push_back(define("APUSR_FSR4_I8_CNB_LDS_C16_HALO", 1));
        shader.defines.push_back(define("APUSR_FSR4_I8_CNB_STREAMED_HIDDEN_CONTRACT", 1));
    }
    if (optimized && pass.kind == PassKind::ModelPost) {
        shader.defines.push_back(define("APUSR_FSR4_I8_POST_LDS_CNB", 1));
        shader.defines.push_back(define("APUSR_FSR4_I8_POST_STREAMED_HIDDEN_CONTRACT", 1));
        shader.defines.push_back(define("APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT", 1));
        shader.defines.push_back(define("APUSR_FSR4_I8_POST_LDS_MODEL_FILTER_INPUT_SOA", 1));
    }
    return shader;
}

ReferenceResourceBindingDesc srvTextureBinding(
    const ResourceId resource,
    const char* shaderName,
    const std::uint32_t shaderRegister,
    const bool optional = false)
{
    return ReferenceResourceBindingDesc{
        .bindingClass = ReferenceBindingClass::SrvTexture,
        .access = ReferenceResourceAccess::Read,
        .resource = resource,
        .ffxIdentifier = ffxResourceIdentifier(resource),
        .shaderRegister = shaderRegister,
        .shaderName = shaderName,
        .optional = optional,
    };
}

ReferenceResourceBindingDesc uavTextureBinding(
    const ResourceId resource,
    const char* shaderName,
    const std::uint32_t shaderRegister,
    const ReferenceResourceAccess access = ReferenceResourceAccess::Write,
    const bool optional = false)
{
    return ReferenceResourceBindingDesc{
        .bindingClass = ReferenceBindingClass::UavTexture,
        .access = access,
        .resource = resource,
        .ffxIdentifier = ffxResourceIdentifier(resource),
        .shaderRegister = shaderRegister,
        .shaderName = shaderName,
        .optional = optional,
    };
}

ReferenceResourceBindingDesc srvBufferBinding(
    const ResourceId resource,
    const char* shaderName,
    const std::uint32_t shaderRegister)
{
    return ReferenceResourceBindingDesc{
        .bindingClass = ReferenceBindingClass::SrvBuffer,
        .access = ReferenceResourceAccess::Read,
        .resource = resource,
        .ffxIdentifier = ffxResourceIdentifier(resource),
        .shaderRegister = shaderRegister,
        .shaderName = shaderName,
    };
}

ReferenceResourceBindingDesc uavBufferBinding(
    const ResourceId resource,
    const char* shaderName,
    const std::uint32_t shaderRegister,
    const ReferenceResourceAccess access)
{
    return ReferenceResourceBindingDesc{
        .bindingClass = ReferenceBindingClass::UavBuffer,
        .access = access,
        .resource = resource,
        .ffxIdentifier = ffxResourceIdentifier(resource),
        .shaderRegister = shaderRegister,
        .shaderName = shaderName,
    };
}

ReferenceResourceBindingDesc constantBinding(
    const ReferenceConstantBufferRole role,
    const std::uint32_t ffxIdentifier,
    const char* shaderName,
    const std::uint32_t shaderRegister)
{
    return ReferenceResourceBindingDesc{
        .bindingClass = ReferenceBindingClass::ConstantBuffer,
        .access = ReferenceResourceAccess::Read,
        .constantBuffer = role,
        .ffxIdentifier = ffxIdentifier,
        .shaderRegister = shaderRegister,
        .shaderName = shaderName,
    };
}

ReferenceResourceBindingDesc samplerBinding(const char* shaderName, const std::uint32_t shaderRegister)
{
    return ReferenceResourceBindingDesc{
        .bindingClass = ReferenceBindingClass::Sampler,
        .access = ReferenceResourceAccess::Read,
        .shaderRegister = shaderRegister,
        .shaderName = shaderName,
    };
}

void addExposureBinding(std::vector<ReferenceResourceBindingDesc>& bindings, const PermutationDesc& permutation)
{
    if (permutation.autoExposure) {
        bindings.push_back(srvTextureBinding(ResourceId::AutoExposure, "r_auto_exposure_texture", 17));
    } else {
        bindings.push_back(srvTextureBinding(ResourceId::InputExposure, "r_input_exposure", 6));
    }
}

std::vector<ReferenceResourceBindingDesc> bindingsForPass(
    const PassDesc& pass,
    const GraphDesc& graph,
    const PermutationDesc& permutation)
{
    std::vector<ReferenceResourceBindingDesc> bindings;
    switch (pass.kind) {
    case PassKind::SpdAutoExposure:
        bindings.reserve(5);
        bindings.push_back(srvTextureBinding(ResourceId::InputColor, "r_input_color", 0));
        bindings.push_back(uavTextureBinding(ResourceId::SpdAtomicCount, "rw_spd_global_atomic", 0, ReferenceResourceAccess::ReadWrite));
        bindings.push_back(uavTextureBinding(ResourceId::SpdMip5, "rw_autoexp_mip_5", 1, ReferenceResourceAccess::ReadWrite));
        bindings.push_back(uavTextureBinding(ResourceId::AutoExposure, "rw_auto_exposure_texture", 2, ReferenceResourceAccess::ReadWrite));
        bindings.push_back(constantBinding(ReferenceConstantBufferRole::SpdAutoExposure, 2, "AutoExposureSPDConstants", 0));
        break;

    case PassKind::ModelPre:
        bindings.reserve(12);
        bindings.push_back(samplerBinding("g_history_sampler", 0));
        bindings.push_back(srvTextureBinding(ResourceId::History, "r_history_color", 0));
        bindings.push_back(srvTextureBinding(ResourceId::InputMotionVectors, "r_velocity", 1));
        bindings.push_back(srvTextureBinding(ResourceId::InputDepth, "r_depth", 2));
        bindings.push_back(srvTextureBinding(ResourceId::InputColor, "r_input_color", 3));
        bindings.push_back(srvTextureBinding(ResourceId::Recurrent, "r_recurrent_0", 4));
        addExposureBinding(bindings, permutation);
        bindings.push_back(uavTextureBinding(ResourceId::HistoryReprojected, "rw_reprojected_color", 3));
        bindings.push_back(uavBufferBinding(ResourceId::ScratchBuffer, "ScratchBuffer", 11, ReferenceResourceAccess::Write));
        bindings.push_back(constantBinding(ReferenceConstantBufferRole::Fsr4Upscaler, 0, "MLSR_Optimized_Constants", 0));
        bindings.push_back(constantBinding(ReferenceConstantBufferRole::PassWeights, 4, "cbPass_Weights", 1));
        break;

    case PassKind::ModelMiddle:
        bindings.reserve(3);
        // Every generated middle shader consumes and produces the shared tensor
        // in-place. Only stages 6-9 also bind the immutable initializer data.
        bindings.push_back(uavBufferBinding(ResourceId::ScratchBuffer, "ScratchBuffer", 1, ReferenceResourceAccess::ReadWrite));
        if (pass.logicalStage >= 6 && pass.logicalStage <= 9) {
            bindings.push_back(srvBufferBinding(ResourceId::InitializerBuffer, "InitializerBuffer", 1));
        }
        // The optimized pass1/pass2 replacements need active display dimensions;
        // the strict generated shaders do not declare this CBV.
        if (graph.variant == PipelineVariant::Dp4aOptimized && pass.logicalStage >= 1 && pass.logicalStage <= 2) {
            bindings.push_back(constantBinding(ReferenceConstantBufferRole::Fsr4Upscaler, 0, "MLSR_Optimized_Constants", 0));
        }
        break;

    case PassKind::ModelPost:
        bindings.reserve(permutation.debugVisualize ? 10 : 9);
        bindings.push_back(srvTextureBinding(ResourceId::InputColor, "r_input_color", 3));
        addExposureBinding(bindings, permutation);
        bindings.push_back(srvTextureBinding(ResourceId::HistoryReprojected, "r_reprojected_color", 9));
        bindings.push_back(uavTextureBinding(ResourceId::History, "rw_history_color", 1));
        bindings.push_back(uavTextureBinding(ResourceId::MlsrOutput, "rw_mlsr_output_color", 2));
        bindings.push_back(uavTextureBinding(ResourceId::Recurrent, "rw_recurrent_0", 6));
        bindings.push_back(uavBufferBinding(ResourceId::ScratchBuffer, "ScratchBuffer", 11, ReferenceResourceAccess::Read));
        if (permutation.debugVisualize) {
            bindings.push_back(uavTextureBinding(ResourceId::DebugInformation, "rw_debug_visualization", 12));
        }
        bindings.push_back(constantBinding(ReferenceConstantBufferRole::Fsr4Upscaler, 0, "MLSR_Optimized_Constants", 0));
        break;

    case PassKind::Rcas:
        bindings.reserve(4);
        addExposureBinding(bindings, permutation);
        bindings.push_back(srvTextureBinding(ResourceId::RcasTemp, "r_rcas_input", 18));
        bindings.push_back(uavTextureBinding(ResourceId::RcasOutput, "rw_rcas_output", 11));
        bindings.push_back(constantBinding(ReferenceConstantBufferRole::Rcas, 3, "cbRCAS", 0));
        break;

    case PassKind::DebugView:
        bindings.reserve(7);
        bindings.push_back(samplerBinding("g_history_sampler", 0));
        bindings.push_back(srvTextureBinding(ResourceId::InputMotionVectors, "r_velocity", 1));
        bindings.push_back(srvTextureBinding(ResourceId::DebugInformation, "r_debug_visualization", 19));
        bindings.push_back(srvTextureBinding(ResourceId::ResultColor, "r_result_color", 20));
        bindings.push_back(uavTextureBinding(ResourceId::MlsrOutput, "rw_mlsr_output_color", 2));
        bindings.push_back(uavTextureBinding(ResourceId::RcasOutput, "rw_rcas_output", 11));
        bindings.push_back(constantBinding(ReferenceConstantBufferRole::Fsr4Upscaler, 0, "MLSR_Optimized_Constants", 0));
        break;
    }
    return bindings;
}

ReferenceDispatchDesc dispatchForPass(const PassDesc& pass, const GraphDesc& graph, const PermutationDesc& permutation, const DispatchGrid grid)
{
    ReferenceDispatchDesc dispatch{};
    dispatch.name = pass.name;
    dispatch.kind = pass.kind;
    dispatch.logicalStage = pass.logicalStage;
    dispatch.grid = grid;
    dispatch.bindings = bindingsForPass(pass, graph, permutation);
    dispatch.shader = shaderDescForPass(pass, graph, permutation);
    dispatch.optional = pass.optional;
    dispatch.enabled = true;
    return dispatch;
}

// FidelityFX SPD setup is small enough to keep here so the scheduler can be
// validated without linking the SDK backend.  It mirrors ffxSpdSetup's public
// behavior for a dispatch rectangle starting at zero.
AutoExposureSpdConstants makeSpdConstants(const RenderSize renderSize, const float preExposure)
{
    const std::uint32_t groupsX = roundUpDiv(renderSize.width, 64u);
    const std::uint32_t groupsY = roundUpDiv(renderSize.height, 64u);
    const std::uint32_t maxDim = std::max(renderSize.width, renderSize.height);
    std::uint32_t mips = 0;
    for (std::uint32_t v = maxDim; v > 1; v >>= 1) {
        ++mips;
    }
    mips = std::min<std::uint32_t>(mips, 12u);

    AutoExposureSpdConstants constants{};
    constants.mips = mips;
    constants.numWorkGroups = groupsX * groupsY;
    constants.workGroupOffset[0] = 0;
    constants.workGroupOffset[1] = 0;
    constants.invInputSize[0] = renderSize.width != 0 ? 1.0f / static_cast<float>(renderSize.width) : 0.0f;
    constants.invInputSize[1] = renderSize.height != 0 ? 1.0f / static_cast<float>(renderSize.height) : 0.0f;
    constants.preExposure = preExposure;
    return constants;
}

std::uint32_t floatBits(const float value)
{
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint16_t floatToHalfBits(const float value)
{
    const std::uint32_t bits = floatBits(value);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23u) & 0xffu) - 127 + 15;
    std::uint32_t mantissa = bits & 0x007fffffu;

    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x00800000u;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
        std::uint32_t halfMantissa = mantissa >> shift;
        const std::uint32_t roundBit = (mantissa >> (shift - 1u)) & 1u;
        halfMantissa += roundBit;
        return static_cast<std::uint16_t>(sign | halfMantissa);
    }

    if (exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    }

    std::uint32_t half = sign | (static_cast<std::uint32_t>(exponent) << 10u) | (mantissa >> 13u);
    if ((mantissa >> 12u) & 1u) {
        ++half;
    }
    return static_cast<std::uint16_t>(half);
}

void rcasCon(std::uint32_t outConfig[4], const float sharpness)
{
    // Mirrors FidelityFX FSR1 FsrRcasCon exactly: con[0] is the FP32 bit pattern
    // of exp2(-sharpness), con[1] packs the same value as two fp16 lanes.
    const float sharp = std::exp2(-sharpness);
    const std::uint16_t half = floatToHalfBits(sharp);
    outConfig[0] = floatBits(sharp);
    outConfig[1] = static_cast<std::uint32_t>(half) | (static_cast<std::uint32_t>(half) << 16u);
    outConfig[2] = 0;
    outConfig[3] = 0;
}

} // namespace

DisplaySize alignReferenceUpscaleSize(const DisplaySize size)
{
    return DisplaySize{.width = alignUp(size.width, 8u), .height = alignUp(size.height, 8u)};
}

Preset referencePresetForFrame(const ReferenceRunnerFrameDesc& frameDesc)
{
    if (frameDesc.residentModelPreset == Preset::DynamicResolution ||
        frameDesc.frame.options.features.dynamicResolution ||
        frameDesc.frame.options.qualityMode == UpscalerQualityMode::DynamicResolution) {
        return Preset::DynamicResolution;
    }
    if (frameDesc.frame.options.qualityMode != UpscalerQualityMode::Auto) {
        return presetForQualityMode(frameDesc.frame.options.qualityMode);
    }
    return presetForDimensions(frameDesc.frame.renderSize, frameDesc.frame.displaySize);
}

PermutationDesc makeReferencePermutationDesc(
    const ReferenceRunnerCreateDesc& createDesc,
    const Preset preset,
    const FrameCapacity frameCapacity,
    const UpscalerFrameOptions& frameOptions)
{
    ColorSpace colorSpace = ColorSpace::Linear;
    if (frameOptions.features.colorSpacePq) {
        colorSpace = ColorSpace::Pq;
    } else if (frameOptions.features.colorSpaceSrgb) {
        colorSpace = ColorSpace::Srgb;
    } else if (frameOptions.features.nonLinearColorSpace) {
        colorSpace = ColorSpace::Nonlinear;
    }

    return PermutationDesc{
        .preset = preset,
        .frameCapacity = frameCapacity,
        .wmma = createDesc.supportsWmma,
        .depthInverted = createDesc.features.depthInverted,
        .lowResMotionVectors = !createDesc.features.displayResolutionMotionVectors,
        .autoExposure = createDesc.features.autoExposure,
        .jitteredMotionVectors = createDesc.features.motionVectorJitterCancellation,
        .colorSpace = colorSpace,
        .debugVisualize = createDesc.allowDebugVisualization && frameOptions.features.debugVisualization,
    };
}

OptimizedConstants makeOptimizedConstants(
    const ReferenceRunnerCreateDesc& createDesc,
    const ReferenceRunnerFrameDesc& frameDesc,
    const DisplaySize alignedUpscaleSize,
    const bool forceReset)
{
    const RenderSize render = frameDesc.frame.renderSize;
    const UpscalerFrameOptions& options = frameDesc.frame.options;
    const RenderSize mvSize = createDesc.features.displayResolutionMotionVectors ?
        RenderSize{.width = frameDesc.frame.displaySize.width, .height = frameDesc.frame.displaySize.height} : render;

    OptimizedConstants constants{};
    constants.invSize[0] = alignedUpscaleSize.width != 0 ? 1.0f / static_cast<float>(alignedUpscaleSize.width) : 0.0f;
    constants.invSize[1] = alignedUpscaleSize.height != 0 ? 1.0f / static_cast<float>(alignedUpscaleSize.height) : 0.0f;
    constants.scale[0] = render.width != 0 ? static_cast<float>(alignedUpscaleSize.width) / static_cast<float>(render.width) : 1.0f;
    constants.scale[1] = render.height != 0 ? static_cast<float>(alignedUpscaleSize.height) / static_cast<float>(render.height) : 1.0f;
    constants.invScale[0] = constants.scale[0] != 0.0f ? 1.0f / constants.scale[0] : 1.0f;
    constants.invScale[1] = constants.scale[1] != 0.0f ? 1.0f / constants.scale[1] : 1.0f;
    constants.jitter[0] = frameDesc.frame.jitter.x;
    constants.jitter[1] = frameDesc.frame.jitter.y;
    constants.motionVectorScale[0] = mvSize.width != 0 ? options.motionVectorScale.x / static_cast<float>(mvSize.width) : 0.0f;
    constants.motionVectorScale[1] = mvSize.height != 0 ? options.motionVectorScale.y / static_cast<float>(mvSize.height) : 0.0f;
    if (createDesc.features.motionVectorJitterCancellation) {
        constants.motionVectorJitterCancellation[0] = mvSize.width != 0 ? (frameDesc.previousJitter[0] - constants.jitter[0]) / static_cast<float>(mvSize.width) : 0.0f;
        constants.motionVectorJitterCancellation[1] = mvSize.height != 0 ? (frameDesc.previousJitter[1] - constants.jitter[1]) / static_cast<float>(mvSize.height) : 0.0f;
    }
    const DisplaySize alignedMaxUpscaleSize = alignReferenceUpscaleSize(createDesc.maxUpscaleSize);
    constants.textureSize[0] = static_cast<float>(alignedMaxUpscaleSize.width);
    constants.textureSize[1] = static_cast<float>(alignedMaxUpscaleSize.height);
    constants.maxRenderSize[0] = static_cast<float>(createDesc.maxRenderSize.width);
    constants.maxRenderSize[1] = static_cast<float>(createDesc.maxRenderSize.height);
    constants.width = alignedUpscaleSize.width;
    constants.height = alignedUpscaleSize.height;
    constants.reset = (options.resetHistory || frameDesc.forceReset || forceReset) ? 1u : 0u;
    constants.widthLowRes = render.width;
    constants.heightLowRes = render.height;
    constants.previousPreExposure = frameDesc.previousPreExposure;
    constants.preExposure = options.preExposure;
    constants.rcasEnabled = options.sharpeningRequested ? 1u : 0u;
    constants.rcasSharpness = options.sharpness;
    constants.useDefaultExposure = options.hasExposure ? 0u : 1u;
    return constants;
}

AutoExposureSpdConstants makeAutoExposureSpdConstants(const RenderSize renderSize, const float preExposure)
{
    return makeSpdConstants(renderSize, preExposure);
}

RcasConstants makeRcasConstants(
    const float sharpness,
    const float preExposure,
    const bool useDefaultExposure,
    const DisplaySize activeOutputSize)
{
    RcasConstants constants{};
    const float clampedSharpness = std::clamp(sharpness, 0.0f, 1.0f);
    const float remappedSharpness = (-2.0f * clampedSharpness) + 2.0f;
    rcasCon(constants.config, remappedSharpness);
    constants.preExposure = preExposure;
    constants.useDefaultExposure = useDefaultExposure ? 1u : 0u;
    constants.outputWidth = activeOutputSize.width;
    constants.outputHeight = activeOutputSize.height;
    return constants;
}

void refreshReferenceFramePlanPerFrameData(
    const ReferenceRunnerCreateDesc& createDesc,
    const ReferenceRunnerFrameDesc& frameDesc,
    ReferenceFramePlan& plan)
{
    ReferenceRunnerFrameDesc effectiveFrame = frameDesc;
    if (effectiveFrame.frame.displaySize.width == 0 && effectiveFrame.frame.displaySize.height == 0) {
        effectiveFrame.frame.displaySize = createDesc.maxUpscaleSize;
    }
    plan.constants = makeOptimizedConstants(createDesc, effectiveFrame, plan.alignedUpscaleSize, frameDesc.forceReset);
    plan.spdConstants = makeAutoExposureSpdConstants(
        effectiveFrame.frame.renderSize,
        effectiveFrame.frame.options.preExposure);
    plan.rcasConstants = makeRcasConstants(
        effectiveFrame.frame.options.sharpness,
        effectiveFrame.frame.options.preExposure,
        !effectiveFrame.frame.options.hasExposure,
        effectiveFrame.frame.displaySize);
}

std::vector<ReferenceShaderCompileDesc> makeReferenceShaderCompilePlan(const GraphDesc& graph, const PermutationDesc& permutation)
{
    std::vector<ReferenceShaderCompileDesc> shaders;
    shaders.reserve(graph.passes.size());
    for (const PassDesc& pass : graph.passes) {
        shaders.push_back(shaderDescForPass(pass, graph, permutation));
    }
    return shaders;
}


std::vector<ReferenceResourceUsageDesc> makeReferenceResourceUsagePlan(const ReferenceFramePlan& framePlan)
{
    std::vector<ReferenceResourceUsageDesc> usage;
    for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
        if (!dispatch.enabled) {
            continue;
        }
        for (const ReferenceResourceBindingDesc& binding : dispatch.bindings) {
            if (binding.resource == ResourceId::Null) {
                continue;
            }
            usage.push_back(ReferenceResourceUsageDesc{
                .dispatchName = dispatch.name,
                .passKind = dispatch.kind,
                .logicalStage = dispatch.logicalStage,
                .resource = binding.resource,
                .bindingClass = binding.bindingClass,
                .access = binding.access,
                .shaderRegister = binding.shaderRegister,
                .registerSpace = binding.registerSpace,
                .shaderName = binding.shaderName,
                .optional = binding.optional,
            });
        }
    }
    return usage;
}

ReferenceFramePlan makeReferenceFramePlan(const ReferenceRunnerCreateDesc& createDesc, const ReferenceRunnerFrameDesc& frameDesc)
{
    ReferenceFramePlan plan{};
    ReferenceRunnerFrameDesc effectiveFrame = frameDesc;
    if (effectiveFrame.frame.displaySize.width == 0 && effectiveFrame.frame.displaySize.height == 0) {
        effectiveFrame.frame.displaySize = createDesc.maxUpscaleSize;
    }
    plan.pipelineVariant = createDesc.pipelineVariant;
    plan.alignedUpscaleSize = alignReferenceUpscaleSize(effectiveFrame.frame.displaySize);
    plan.frameCapacity = frameCapacityForDisplay(createDesc.maxUpscaleSize.width != 0 ? createDesc.maxUpscaleSize : plan.alignedUpscaleSize);
    plan.preset = referencePresetForFrame(effectiveFrame);
    plan.permutation = makeReferencePermutationDesc(createDesc, plan.preset, plan.frameCapacity, effectiveFrame.frame.options);
    plan.permutationKey = permutationKey(plan.permutation);
    refreshReferenceFramePlanPerFrameData(createDesc, effectiveFrame, plan);

    GraphDesc graph = makeFsr4Int8Graph(plan.pipelineVariant, plan.preset, plan.frameCapacity, plan.alignedUpscaleSize);
    plan.dispatches.reserve(graph.passes.size());
    for (const PassDesc& pass : graph.passes) {
        if (pass.kind == PassKind::SpdAutoExposure && !createDesc.features.autoExposure) {
            continue;
        }
        if (pass.kind == PassKind::Rcas && !frameDesc.frame.options.sharpeningRequested) {
            continue;
        }
        if (pass.kind == PassKind::DebugView && !(createDesc.allowDebugVisualization && effectiveFrame.frame.options.features.debugVisualization)) {
            continue;
        }

        DispatchGrid grid = pass.dp4aDispatch;
        if (pass.kind == PassKind::ModelMiddle) {
            grid = middleDispatch(pass.logicalStage, plan.alignedUpscaleSize, plan.pipelineVariant);
        } else if (pass.kind == PassKind::ModelPre) {
            grid = DispatchGrid{.groupsX = roundUpDiv(plan.alignedUpscaleSize.width, 16u), .groupsY = roundUpDiv(plan.alignedUpscaleSize.height, 16u), .groupsZ = 1};
        } else if (pass.kind == PassKind::ModelPost || pass.kind == PassKind::Rcas) {
            grid = DispatchGrid{.groupsX = roundUpDiv(plan.alignedUpscaleSize.width, 16u), .groupsY = roundUpDiv(plan.alignedUpscaleSize.height, 16u), .groupsZ = 1};
        } else if (pass.kind == PassKind::SpdAutoExposure) {
            grid = DispatchGrid{.groupsX = roundUpDiv(effectiveFrame.frame.renderSize.width, 64u), .groupsY = roundUpDiv(effectiveFrame.frame.renderSize.height, 64u), .groupsZ = 1};
        }
        plan.dispatches.push_back(dispatchForPass(pass, graph, plan.permutation, grid));
    }
    return plan;
}

Status makeReferencePass0WeightsConstants(const Preset preset, Pass0WeightsConstants& outConstants)
{
    const embedded_pass0_weights::Pass0WeightsBlob* weights = embedded_pass0_weights::find(preset);
    if (weights == nullptr || !weights->available()) {
        return Status::invalidArgument("FSR4 INT8 pass-0 weight table is missing for preset=" + std::string(presetName(preset)));
    }
    std::memcpy(
        outConstants.downscaleConvWeightDwords.data(),
        weights->dwords,
        sizeof(std::uint32_t) * outConstants.downscaleConvWeightDwords.size());
    return Status::ok();
}

Status loadReferenceInitializerBytes(const std::string& repoRoot, const Preset preset, std::vector<std::uint8_t>& outBytes)
{
    outBytes.clear();

    // Prefer loose files when a valid repo/model root is available so developer
    // imports can be tested without regenerating the binary.  The product
    // fallback is the embedded initializer table, which keeps drop-in DLL
    // packaging self-contained and avoids game-directory model clutter.
    const ReferenceArtifactPaths paths = referenceArtifactPaths(preset, FrameCapacity::R2160);
    std::filesystem::path initializerPath;
    if (!repoRoot.empty()) {
        initializerPath = std::filesystem::path(repoRoot) / paths.initializers;
        std::ifstream file(initializerPath, std::ios::binary);
        if (file) {
            outBytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            if (outBytes.empty()) {
                return Status::invalidArgument("FSR4 INT8 initializer file was empty: " + initializerPath.string());
            }
            return Status::ok();
        }
    }

    const embedded_initializers::InitializerBlob* embedded = embedded_initializers::find(preset);
    if (embedded != nullptr && embedded->available()) {
        outBytes.assign(embedded->data, embedded->data + embedded->size);
        return Status::ok();
    }

    std::string message = "could not load FSR4 INT8 initializer data for preset=";
    message += presetName(preset);
    if (!initializerPath.empty()) {
        message += "; file was unavailable: ";
        message += initializerPath.string();
    }
    message += "; embedded initializer table did not contain the preset";
    return Status::invalidArgument(message);
}

const char* referenceConstantBufferRoleName(const ReferenceConstantBufferRole role)
{
    switch (role) {
    case ReferenceConstantBufferRole::None: return "none";
    case ReferenceConstantBufferRole::Fsr4Upscaler: return "fsr4-upscaler";
    case ReferenceConstantBufferRole::SpdAutoExposure: return "spd-auto-exposure";
    case ReferenceConstantBufferRole::Rcas: return "rcas";
    case ReferenceConstantBufferRole::PassWeights: return "pass-weights";
    }
    return "unknown";
}

const char* referenceBindingClassName(const ReferenceBindingClass bindingClass)
{
    switch (bindingClass) {
    case ReferenceBindingClass::SrvTexture: return "srv-texture";
    case ReferenceBindingClass::UavTexture: return "uav-texture";
    case ReferenceBindingClass::SrvBuffer: return "srv-buffer";
    case ReferenceBindingClass::UavBuffer: return "uav-buffer";
    case ReferenceBindingClass::ConstantBuffer: return "constant-buffer";
    case ReferenceBindingClass::Sampler: return "sampler";
    }
    return "unknown";
}

const char* referenceResourceAccessName(const ReferenceResourceAccess access)
{
    switch (access) {
    case ReferenceResourceAccess::Read: return "read";
    case ReferenceResourceAccess::Write: return "write";
    case ReferenceResourceAccess::ReadWrite: return "read-write";
    }
    return "unknown";
}


} // namespace apusr::models::fsr4_int8
