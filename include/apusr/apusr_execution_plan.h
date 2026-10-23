// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>
#include <apusr/apusr_manifest.h>

#include <cstdint>
#include <span>

namespace apusr {

enum class UpscalerFrontendPath : std::uint32_t {
    NativeApusr,
    FidelityFxFsr4Dx12DropIn,
    NgxDlssDx12DropIn,
};

enum class UpscalerExecutionPath : std::uint32_t {
    GpuFsr4Int8Reference,
    GpuFsr4Int8Dp4aTuned,
    GpuWmma,
    NpuCompleteFrameSlot,
    HybridNpuIslands,
};

enum class UpscalerLatencyPolicy : std::uint32_t {
    FrameDelayed,
    InlineQueue,
};

enum class UpscalerPassKind : std::uint32_t {
    PreBookend,
    ModelMiddle,
    PostBookend,
    Rcas,
    AutoExposure,
    Diagnostic,
};

enum class UpscalerPassPlacement : std::uint32_t {
    GraphicsQueue,
    NpuIsland,
    HybridGpuNpu,
};

struct UpscalerPassDesc {
    const char* name = "";
    std::uint32_t providerDispatchIndex = 0;
    std::uint32_t logicalStageFirst = 0;
    std::uint32_t logicalStageLast = 0;
    UpscalerPassKind kind = UpscalerPassKind::ModelMiddle;
    UpscalerPassPlacement defaultPlacement = UpscalerPassPlacement::GraphicsQueue;
    bool readsTemporalHistory = false;
    bool writesTemporalHistory = false;
    bool acceptsNpuIsland = false;
};

struct UpscalerExecutionPlanDesc {
    UpscalerFrontendPath frontendPath = UpscalerFrontendPath::FidelityFxFsr4Dx12DropIn;
    GraphicsApi graphicsApi = GraphicsApi::Dx12;
    ModelFamily modelFamily = ModelFamily::Fsr4Int8;
    UpscalerExecutionPath executionPath = UpscalerExecutionPath::GpuFsr4Int8Reference;
    UpscalerLatencyPolicy latencyPolicy = UpscalerLatencyPolicy::FrameDelayed;
    bool usesFrameSlotTransport = true;
    bool allowNpuIslands = false;
};

[[nodiscard]] UpscalerExecutionPlanDesc makeFsr4Int8ReferencePlan(
    UpscalerFrontendPath frontendPath,
    UpscalerLatencyPolicy latencyPolicy);
[[nodiscard]] UpscalerExecutionPlanDesc makeFsr4Int8Dp4aTunedPlan(
    UpscalerFrontendPath frontendPath,
    UpscalerLatencyPolicy latencyPolicy);
[[nodiscard]] UpscalerExecutionPlanDesc makeFsr4WmmaPlan(
    UpscalerFrontendPath frontendPath,
    UpscalerLatencyPolicy latencyPolicy);
[[nodiscard]] UpscalerExecutionPlanDesc makeFsr4NpuFrameSlotPlan();
[[nodiscard]] UpscalerExecutionPlanDesc makeFsr4NgxDlssHybridIslandPlan();

[[nodiscard]] std::span<const UpscalerPassDesc> fsr4Int8ReferenceProviderPasses();
[[nodiscard]] std::uint32_t fsr4Int8ReferenceProviderPassCount();
[[nodiscard]] const UpscalerPassDesc* findFsr4Int8ReferenceProviderPass(const char* name);

[[nodiscard]] bool upscalerExecutionPathUsesNpu(UpscalerExecutionPath path);
[[nodiscard]] bool upscalerExecutionPathIsGpuOnly(UpscalerExecutionPath path);
[[nodiscard]] bool upscalerLatencyPolicyIsInline(UpscalerLatencyPolicy policy);
[[nodiscard]] Status validateUpscalerExecutionPlanDesc(const UpscalerExecutionPlanDesc& desc);

[[nodiscard]] const char* upscalerFrontendPathName(UpscalerFrontendPath path);
[[nodiscard]] const char* upscalerExecutionPathName(UpscalerExecutionPath path);
[[nodiscard]] const char* upscalerLatencyPolicyName(UpscalerLatencyPolicy policy);
[[nodiscard]] const char* upscalerPassKindName(UpscalerPassKind kind);
[[nodiscard]] const char* upscalerPassPlacementName(UpscalerPassPlacement placement);

} // namespace apusr
