// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/models/apusr_fsr4_int8_graph.h>
#include <apusr/models/apusr_fsr4_int8_resources.h>
#include <apusr/apusr_common.h>
#include <apusr/apusr_runtime.h>
#include <apusr/apusr_upscaler_common.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace apusr::models::fsr4_int8 {

// These structures intentionally mirror the FFX FSR4 DX12 provider's GPU
// constant-buffer layouts.  Keep them API-neutral here: DX12/Vulkan backends
// decide how the bytes are uploaded and bound.
struct alignas(256) OptimizedConstants {
    float invSize[2]{};
    float scale[2]{};
    float invScale[2]{};
    float jitter[2]{};
    float motionVectorScale[2]{};
    float textureSize[2]{};
    float maxRenderSize[2]{};
    float motionVectorJitterCancellation[2]{};

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t reset = 0;
    std::uint32_t widthLowRes = 0;

    std::uint32_t heightLowRes = 0;
    float preExposure = 1.0f;
    float previousPreExposure = 1.0f;
    std::uint32_t rcasEnabled = 0;

    float rcasSharpness = 0.0f;
    std::uint32_t useDefaultExposure = 0;
};

struct alignas(256) AutoExposureSpdConstants {
    std::uint32_t mips = 0;
    std::uint32_t numWorkGroups = 0;
    std::uint32_t workGroupOffset[2]{};
    float invInputSize[2]{};
    float preExposure = 1.0f;
    float padding = 0.0f;
};

struct alignas(256) RcasConstants {
    std::uint32_t config[4]{};
    float preExposure = 1.0f;
    std::uint32_t useDefaultExposure = 0;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
};

struct alignas(256) Pass0WeightsConstants {
    std::array<std::uint32_t, 256> downscaleConvWeightDwords{};
};

static_assert(sizeof(OptimizedConstants) == 256, "FSR4 optimized constants mirror FFX alignas(256) layout");
static_assert(sizeof(AutoExposureSpdConstants) == 256, "FSR4 auto-exposure constants mirror FFX alignas(256) layout");
static_assert(sizeof(RcasConstants) == 256, "FSR4 RCAS constants mirror FFX alignas(256) layout");
static_assert(sizeof(Pass0WeightsConstants) == 1024, "FSR4 pass-0 weights are 256 dwords");
static_assert(offsetof(OptimizedConstants, invSize) == 0);
static_assert(offsetof(OptimizedConstants, motionVectorJitterCancellation) == 56);
static_assert(offsetof(OptimizedConstants, width) == 64);
static_assert(offsetof(OptimizedConstants, heightLowRes) == 80);
static_assert(offsetof(OptimizedConstants, rcasSharpness) == 96);
static_assert(offsetof(AutoExposureSpdConstants, invInputSize) == 16);
static_assert(offsetof(AutoExposureSpdConstants, preExposure) == 24);
static_assert(offsetof(RcasConstants, config) == 0);
static_assert(offsetof(RcasConstants, preExposure) == 16);
static_assert(offsetof(RcasConstants, useDefaultExposure) == 20);

struct ReferenceRunnerCreateDesc {
    PipelineVariant pipelineVariant = PipelineVariant::StrictReference;
    RenderSize maxRenderSize;
    DisplaySize maxUpscaleSize;
    UpscalerFeatureFlags features;
    Preset initialPreset = Preset::Balanced;
    bool allowDebugVisualization = false;
    bool supportsWmma = false;
};

struct ReferenceRunnerFrameDesc {
    FrameDesc frame;
    DisplaySize maxUpscaleSize;
    Preset residentModelPreset = Preset::Balanced;
    float previousPreExposure = 1.0f;
    float previousJitter[2]{};
    bool forceReset = false;
};

enum class ReferenceConstantBufferRole : std::uint32_t {
    None,
    Fsr4Upscaler,
    SpdAutoExposure,
    Rcas,
    PassWeights,
};

enum class ReferenceBindingClass : std::uint32_t {
    SrvTexture,
    UavTexture,
    SrvBuffer,
    UavBuffer,
    ConstantBuffer,
    Sampler,
};

enum class ReferenceResourceAccess : std::uint32_t {
    Read,
    Write,
    ReadWrite,
};

struct ReferenceResourceBindingDesc {
    ReferenceBindingClass bindingClass = ReferenceBindingClass::SrvTexture;
    ReferenceResourceAccess access = ReferenceResourceAccess::Read;
    ResourceId resource = ResourceId::Null;
    ReferenceConstantBufferRole constantBuffer = ReferenceConstantBufferRole::None;
    std::uint32_t ffxIdentifier = 0;
    std::uint32_t shaderRegister = 0;
    std::uint32_t registerSpace = 0;
    const char* shaderName = nullptr;
    bool optional = false;
};

struct ReferenceResourceUsageDesc {
    const char* dispatchName = nullptr;
    PassKind passKind = PassKind::ModelMiddle;
    std::uint32_t logicalStage = 0;
    ResourceId resource = ResourceId::Null;
    ReferenceBindingClass bindingClass = ReferenceBindingClass::SrvTexture;
    ReferenceResourceAccess access = ReferenceResourceAccess::Read;
    std::uint32_t shaderRegister = 0;
    std::uint32_t registerSpace = 0;
    const char* shaderName = nullptr;
    bool optional = false;
};

struct ReferenceShaderCompileDesc {
    const char* name = nullptr;
    PassKind kind = PassKind::ModelMiddle;
    std::uint32_t logicalStage = 0;
    std::string shaderPath;
    std::string entryPoint;
    std::vector<std::string> defines;
    std::vector<std::string> includeRoots;
};

struct ReferenceDispatchDesc {
    const char* name = nullptr;
    PassKind kind = PassKind::ModelMiddle;
    std::uint32_t logicalStage = 0;
    DispatchGrid grid;
    // Exact descriptor and access contract for this dispatch. Backends lower
    // this list directly; there is no broader stage-level binding table.
    std::vector<ReferenceResourceBindingDesc> bindings;
    ReferenceShaderCompileDesc shader;
    bool optional = false;
    bool enabled = true;
};

struct ReferenceFramePlan {
    PipelineVariant pipelineVariant = PipelineVariant::StrictReference;
    Preset preset = Preset::Balanced;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    PermutationDesc permutation;
    std::uint32_t permutationKey = 0;
    DisplaySize alignedUpscaleSize;
    OptimizedConstants constants;
    AutoExposureSpdConstants spdConstants;
    RcasConstants rcasConstants;
    std::vector<ReferenceDispatchDesc> dispatches;
};

[[nodiscard]] DisplaySize alignReferenceUpscaleSize(DisplaySize size);
[[nodiscard]] Preset referencePresetForFrame(const ReferenceRunnerFrameDesc& frameDesc);
[[nodiscard]] PermutationDesc makeReferencePermutationDesc(const ReferenceRunnerCreateDesc& createDesc, Preset preset, FrameCapacity frameCapacity, const UpscalerFrameOptions& frameOptions);
[[nodiscard]] OptimizedConstants makeOptimizedConstants(const ReferenceRunnerCreateDesc& createDesc, const ReferenceRunnerFrameDesc& frameDesc, DisplaySize alignedUpscaleSize, bool forceReset);
[[nodiscard]] AutoExposureSpdConstants makeAutoExposureSpdConstants(RenderSize renderSize, float preExposure);
[[nodiscard]] RcasConstants makeRcasConstants(float sharpness, float preExposure, bool useDefaultExposure, DisplaySize activeOutputSize);
void refreshReferenceFramePlanPerFrameData(const ReferenceRunnerCreateDesc& createDesc, const ReferenceRunnerFrameDesc& frameDesc, ReferenceFramePlan& plan);
[[nodiscard]] std::vector<ReferenceShaderCompileDesc> makeReferenceShaderCompilePlan(const GraphDesc& graph, const PermutationDesc& permutation);
[[nodiscard]] std::vector<ReferenceResourceUsageDesc> makeReferenceResourceUsagePlan(const ReferenceFramePlan& framePlan);
[[nodiscard]] ReferenceFramePlan makeReferenceFramePlan(const ReferenceRunnerCreateDesc& createDesc, const ReferenceRunnerFrameDesc& frameDesc);
[[nodiscard]] Status loadReferenceInitializerBytes(const std::string& repoRoot, Preset preset, std::vector<std::uint8_t>& outBytes);
[[nodiscard]] Status makeReferencePass0WeightsConstants(Preset preset, Pass0WeightsConstants& outConstants);

[[nodiscard]] const char* referenceConstantBufferRoleName(ReferenceConstantBufferRole role);
[[nodiscard]] const char* referenceBindingClassName(ReferenceBindingClass bindingClass);
[[nodiscard]] const char* referenceResourceAccessName(ReferenceResourceAccess access);

} // namespace apusr::models::fsr4_int8
