// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_execution_plan.h>

#include <array>
#include <cstring>

namespace apusr {
namespace {

constexpr std::array<UpscalerPassDesc, 14> kFsr4Int8ReferenceProviderPasses{{
    UpscalerPassDesc{
        .name = "pre",
        .providerDispatchIndex = 0,
        .logicalStageFirst = 0,
        .logicalStageLast = 0,
        .kind = UpscalerPassKind::PreBookend,
        .defaultPlacement = UpscalerPassPlacement::GraphicsQueue,
        .readsTemporalHistory = true,
        .writesTemporalHistory = false,
        .acceptsNpuIsland = false,
    },
    UpscalerPassDesc{.name = "pass1", .providerDispatchIndex = 1, .logicalStageFirst = 1, .logicalStageLast = 1, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass2", .providerDispatchIndex = 2, .logicalStageFirst = 2, .logicalStageLast = 2, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass3", .providerDispatchIndex = 3, .logicalStageFirst = 3, .logicalStageLast = 3, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass4", .providerDispatchIndex = 4, .logicalStageFirst = 4, .logicalStageLast = 4, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass5", .providerDispatchIndex = 5, .logicalStageFirst = 5, .logicalStageLast = 5, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass6", .providerDispatchIndex = 6, .logicalStageFirst = 6, .logicalStageLast = 6, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass7", .providerDispatchIndex = 7, .logicalStageFirst = 7, .logicalStageLast = 7, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass8", .providerDispatchIndex = 8, .logicalStageFirst = 8, .logicalStageLast = 8, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass9", .providerDispatchIndex = 9, .logicalStageFirst = 9, .logicalStageLast = 9, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass10", .providerDispatchIndex = 10, .logicalStageFirst = 10, .logicalStageLast = 10, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass11", .providerDispatchIndex = 11, .logicalStageFirst = 11, .logicalStageLast = 11, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{.name = "pass12", .providerDispatchIndex = 12, .logicalStageFirst = 12, .logicalStageLast = 12, .kind = UpscalerPassKind::ModelMiddle, .defaultPlacement = UpscalerPassPlacement::GraphicsQueue, .acceptsNpuIsland = true},
    UpscalerPassDesc{
        .name = "post",
        .providerDispatchIndex = 13,
        .logicalStageFirst = 13,
        .logicalStageLast = 13,
        .kind = UpscalerPassKind::PostBookend,
        .defaultPlacement = UpscalerPassPlacement::GraphicsQueue,
        .readsTemporalHistory = true,
        .writesTemporalHistory = true,
        .acceptsNpuIsland = false,
    },
}};

[[nodiscard]] bool frontendPathSupportsGraphicsApi(const UpscalerFrontendPath frontendPath, const GraphicsApi graphicsApi)
{
    switch (frontendPath) {
    case UpscalerFrontendPath::NativeApusr:
        return graphicsApi == GraphicsApi::Dx12 || graphicsApi == GraphicsApi::Vulkan;
    case UpscalerFrontendPath::FidelityFxFsr4Dx12DropIn:
        return graphicsApi == GraphicsApi::Dx12;
    case UpscalerFrontendPath::NgxDlssDx12DropIn:
        return graphicsApi == GraphicsApi::Dx12;
    }
    return false;
}

} // namespace

UpscalerExecutionPlanDesc makeFsr4Int8ReferencePlan(
    const UpscalerFrontendPath frontendPath,
    const UpscalerLatencyPolicy latencyPolicy)
{
    return UpscalerExecutionPlanDesc{
        .frontendPath = frontendPath,
        .graphicsApi = GraphicsApi::Dx12,
        .modelFamily = ModelFamily::Fsr4Int8,
        .executionPath = UpscalerExecutionPath::GpuFsr4Int8Reference,
        .latencyPolicy = latencyPolicy,
        .usesFrameSlotTransport = latencyPolicy == UpscalerLatencyPolicy::FrameDelayed,
        .allowNpuIslands = false,
    };
}

UpscalerExecutionPlanDesc makeFsr4Int8Dp4aTunedPlan(
    const UpscalerFrontendPath frontendPath,
    const UpscalerLatencyPolicy latencyPolicy)
{
    return UpscalerExecutionPlanDesc{
        .frontendPath = frontendPath,
        .graphicsApi = GraphicsApi::Dx12,
        .modelFamily = ModelFamily::Fsr4Int8,
        .executionPath = UpscalerExecutionPath::GpuFsr4Int8Dp4aTuned,
        .latencyPolicy = latencyPolicy,
        .usesFrameSlotTransport = latencyPolicy == UpscalerLatencyPolicy::FrameDelayed,
        .allowNpuIslands = false,
    };
}

UpscalerExecutionPlanDesc makeFsr4WmmaPlan(
    const UpscalerFrontendPath frontendPath,
    const UpscalerLatencyPolicy latencyPolicy)
{
    return UpscalerExecutionPlanDesc{
        .frontendPath = frontendPath,
        .graphicsApi = GraphicsApi::Dx12,
        .modelFamily = ModelFamily::Fsr4Int8,
        .executionPath = UpscalerExecutionPath::GpuWmma,
        .latencyPolicy = latencyPolicy,
        .usesFrameSlotTransport = latencyPolicy == UpscalerLatencyPolicy::FrameDelayed,
        .allowNpuIslands = false,
    };
}

UpscalerExecutionPlanDesc makeFsr4NpuFrameSlotPlan()
{
    return UpscalerExecutionPlanDesc{
        .frontendPath = UpscalerFrontendPath::FidelityFxFsr4Dx12DropIn,
        .graphicsApi = GraphicsApi::Dx12,
        .modelFamily = ModelFamily::Fsr4Int8,
        .executionPath = UpscalerExecutionPath::NpuCompleteFrameSlot,
        .latencyPolicy = UpscalerLatencyPolicy::FrameDelayed,
        .usesFrameSlotTransport = true,
        .allowNpuIslands = false,
    };
}

UpscalerExecutionPlanDesc makeFsr4NgxDlssHybridIslandPlan()
{
    return UpscalerExecutionPlanDesc{
        .frontendPath = UpscalerFrontendPath::NgxDlssDx12DropIn,
        .graphicsApi = GraphicsApi::Dx12,
        .modelFamily = ModelFamily::Fsr4Int8,
        .executionPath = UpscalerExecutionPath::HybridNpuIslands,
        .latencyPolicy = UpscalerLatencyPolicy::InlineQueue,
        .usesFrameSlotTransport = false,
        .allowNpuIslands = true,
    };
}

std::span<const UpscalerPassDesc> fsr4Int8ReferenceProviderPasses()
{
    return kFsr4Int8ReferenceProviderPasses;
}

std::uint32_t fsr4Int8ReferenceProviderPassCount()
{
    return static_cast<std::uint32_t>(kFsr4Int8ReferenceProviderPasses.size());
}

const UpscalerPassDesc* findFsr4Int8ReferenceProviderPass(const char* name)
{
    if (name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    for (const UpscalerPassDesc& pass : kFsr4Int8ReferenceProviderPasses) {
        if (std::strcmp(pass.name, name) == 0) {
            return &pass;
        }
    }
    return nullptr;
}

bool upscalerExecutionPathUsesNpu(const UpscalerExecutionPath path)
{
    return path == UpscalerExecutionPath::NpuCompleteFrameSlot || path == UpscalerExecutionPath::HybridNpuIslands;
}

bool upscalerExecutionPathIsGpuOnly(const UpscalerExecutionPath path)
{
    return path == UpscalerExecutionPath::GpuFsr4Int8Reference ||
           path == UpscalerExecutionPath::GpuFsr4Int8Dp4aTuned ||
           path == UpscalerExecutionPath::GpuWmma;
}

bool upscalerLatencyPolicyIsInline(const UpscalerLatencyPolicy policy)
{
    return policy == UpscalerLatencyPolicy::InlineQueue;
}

Status validateUpscalerExecutionPlanDesc(const UpscalerExecutionPlanDesc& desc)
{
    if (desc.modelFamily != ModelFamily::Fsr4Int8) {
        return Status::invalidArgument("upscaler execution plan currently supports only the FSR4 INT8 model family");
    }
    if (!frontendPathSupportsGraphicsApi(desc.frontendPath, desc.graphicsApi)) {
        return Status::invalidArgument("upscaler frontend path does not support the selected graphics API");
    }
    if (desc.frontendPath == UpscalerFrontendPath::NgxDlssDx12DropIn &&
        desc.executionPath == UpscalerExecutionPath::HybridNpuIslands &&
        desc.latencyPolicy != UpscalerLatencyPolicy::InlineQueue) {
        return Status::invalidArgument("NGX/DLSS hybrid island plans must use inline queue execution");
    }
    if (upscalerExecutionPathIsGpuOnly(desc.executionPath) && desc.allowNpuIslands) {
        return Status::invalidArgument("GPU-only execution plans must not enable NPU islands");
    }
    if (desc.executionPath == UpscalerExecutionPath::HybridNpuIslands && !desc.allowNpuIslands) {
        return Status::invalidArgument("hybrid execution plans must enable NPU islands");
    }
    if (desc.executionPath == UpscalerExecutionPath::NpuCompleteFrameSlot && !desc.usesFrameSlotTransport) {
        return Status::invalidArgument("complete-frame NPU execution requires frame-slot transport");
    }
    return Status::ok();
}

const char* upscalerFrontendPathName(const UpscalerFrontendPath path)
{
    switch (path) {
    case UpscalerFrontendPath::NativeApusr:
        return "NativeApusr";
    case UpscalerFrontendPath::FidelityFxFsr4Dx12DropIn:
        return "FidelityFxFsr4Dx12DropIn";
    case UpscalerFrontendPath::NgxDlssDx12DropIn:
        return "NgxDlssDx12DropIn";
    }
    return "Unknown";
}

const char* upscalerExecutionPathName(const UpscalerExecutionPath path)
{
    switch (path) {
    case UpscalerExecutionPath::GpuFsr4Int8Reference:
        return "GpuFsr4Int8Reference";
    case UpscalerExecutionPath::GpuFsr4Int8Dp4aTuned:
        return "GpuFsr4Int8Dp4aTuned";
    case UpscalerExecutionPath::GpuWmma:
        return "GpuWmma";
    case UpscalerExecutionPath::NpuCompleteFrameSlot:
        return "NpuCompleteFrameSlot";
    case UpscalerExecutionPath::HybridNpuIslands:
        return "HybridNpuIslands";
    }
    return "Unknown";
}

const char* upscalerLatencyPolicyName(const UpscalerLatencyPolicy policy)
{
    switch (policy) {
    case UpscalerLatencyPolicy::FrameDelayed:
        return "FrameDelayed";
    case UpscalerLatencyPolicy::InlineQueue:
        return "InlineQueue";
    }
    return "Unknown";
}

const char* upscalerPassKindName(const UpscalerPassKind kind)
{
    switch (kind) {
    case UpscalerPassKind::PreBookend:
        return "PreBookend";
    case UpscalerPassKind::ModelMiddle:
        return "ModelMiddle";
    case UpscalerPassKind::PostBookend:
        return "PostBookend";
    case UpscalerPassKind::Rcas:
        return "Rcas";
    case UpscalerPassKind::AutoExposure:
        return "AutoExposure";
    case UpscalerPassKind::Diagnostic:
        return "Diagnostic";
    }
    return "Unknown";
}

const char* upscalerPassPlacementName(const UpscalerPassPlacement placement)
{
    switch (placement) {
    case UpscalerPassPlacement::GraphicsQueue:
        return "GraphicsQueue";
    case UpscalerPassPlacement::NpuIsland:
        return "NpuIsland";
    case UpscalerPassPlacement::HybridGpuNpu:
        return "HybridGpuNpu";
    }
    return "Unknown";
}

} // namespace apusr
