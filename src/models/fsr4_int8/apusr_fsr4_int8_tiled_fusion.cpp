// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_tiled_fusion.h>

#include <algorithm>
#include <cstdint>

namespace apusr::models::fsr4_int8 {
namespace {

constexpr std::uint32_t kC16 = 16;
constexpr std::uint32_t kC32 = 32;
constexpr std::uint32_t kStageCellWidth = 16;
constexpr std::uint32_t kStageCellHeight = 8;

[[nodiscard]] constexpr std::uint32_t halfDown(const std::uint32_t value) noexcept
{
    return value >> 1u;
}

[[nodiscard]] constexpr std::uint64_t tensorBytes(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t channels) noexcept
{
    return static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) *
        static_cast<std::uint64_t>(channels);
}

[[nodiscard]] bool hasStageRange(const GraphDesc& graph, const std::uint32_t first, const std::uint32_t last)
{
    for (std::uint32_t stage = first; stage <= last; ++stage) {
        const auto found = std::find_if(graph.passes.begin(), graph.passes.end(), [stage](const PassDesc& pass) {
            return pass.modelPass && pass.logicalStage == stage;
        });
        if (found == graph.passes.end()) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] Dp4aTiledFusionIslandDesc makeFrontC16PairedMacroblockIsland(const DisplaySize displaySize)
{
    const std::uint32_t c16Width = halfDown(displaySize.width);
    const std::uint32_t c16Height = halfDown(displaySize.height);
    const std::uint64_t c16FrameBytes = tensorBytes(c16Width, c16Height, kC16);

    Dp4aTiledFusionIslandDesc island{};
    island.name = "front-c16-stage1-2-3-paired-macroblock";
    island.kind = Dp4aTiledFusionKind::Stage1Stage2Stage3C16PairedMacroblock;
    island.firstLogicalStage = 1;
    island.lastLogicalStage = 3;
    island.outputLogicalStage = 3;
    island.exactLocalQuantizationRequired = true;
    island.runtimeReplacementImplemented = false;
    island.shaderArtifactAvailable = true;
    island.requiresSkipSidebandStore = true;
    island.requiresStage0SemanticChange = true;
    island.candidateShaderPath = "shaders/dx12/optimized/apusr_fsr4_i8_pass1_tiled_exact.hlsl";
    island.candidateEntryPoint = "apusr_fsr4_i8_pass1_tiled_exact";
    island.candidateDispatchName = "apusr-fsr4-i8-pass1-tiled-exact";

    // Two 16x8 C16 stage-2 cells in X and Y form a 32x16 C16 patch.  Stage 2
    // needs a 3x3 Stage-1 halo, so the local Stage-1 boundary is 34x18.  Stage
    // 1 itself also needs a 3x3 halo, so the original C16 source macro-tile is
    // 36x20.  Stage 3 is k2/s2 C16->C32 and emits a 16x8 C32 tile.
    island.sourceMacroTile = Dp4aTileShapeDesc{.width = 36, .height = 20, .channels = kC16};
    island.localStage1Patch = Dp4aTileShapeDesc{.width = 34, .height = 18, .channels = kC16};
    island.localStage2Patch = Dp4aTileShapeDesc{.width = 2u * kStageCellWidth, .height = 2u * kStageCellHeight, .channels = kC16};
    island.outputTile = Dp4aTileShapeDesc{.width = kStageCellWidth, .height = kStageCellHeight, .channels = kC32};

    const std::uint64_t stage1GlobalWriteAndStage2GlobalRead = 2u * c16FrameBytes;
    const std::uint64_t stage2MainPathGlobalRead = c16FrameBytes;

    island.boundaries.push_back(Dp4aFusionBoundaryDesc{
        .name = "stage1-to-stage2-c16",
        .policy = Dp4aFusionBoundaryPolicy::DeleteTransientGlobalTensor,
        .tile = island.localStage1Patch,
        .conservativeFrameBytes = stage1GlobalWriteAndStage2GlobalRead,
    });
    island.boundaries.push_back(Dp4aFusionBoundaryDesc{
        .name = "stage2-skipa-c16",
        .policy = Dp4aFusionBoundaryPolicy::TeeSkipSideband,
        .tile = island.localStage2Patch,
        .conservativeFrameBytes = c16FrameBytes,
    });
    island.boundaries.push_back(Dp4aFusionBoundaryDesc{
        .name = "stage2-to-stage3-mainpath-c16",
        .policy = Dp4aFusionBoundaryPolicy::DeleteTransientGlobalTensor,
        .tile = island.localStage2Patch,
        .conservativeFrameBytes = stage2MainPathGlobalRead,
    });
    island.boundaries.push_back(Dp4aFusionBoundaryDesc{
        .name = "stage3-output-c32",
        .policy = Dp4aFusionBoundaryPolicy::PreserveGlobalTensor,
        .tile = island.outputTile,
        .conservativeFrameBytes = tensorBytes(c16Width >> 1u, c16Height >> 1u, kC32),
    });

    island.conservativeAvoidedFrameBytes = stage1GlobalWriteAndStage2GlobalRead + stage2MainPathGlobalRead;
    return island;
}

} // namespace

const char* dp4aFusionBoundaryPolicyName(const Dp4aFusionBoundaryPolicy policy)
{
    switch (policy) {
    case Dp4aFusionBoundaryPolicy::PreserveGlobalTensor:
        return "preserve-global-tensor";
    case Dp4aFusionBoundaryPolicy::PreserveLocalQuantizedTensor:
        return "preserve-local-quantized-tensor";
    case Dp4aFusionBoundaryPolicy::DeleteTransientGlobalTensor:
        return "delete-transient-global-tensor";
    case Dp4aFusionBoundaryPolicy::TeeSkipSideband:
        return "tee-skip-sideband";
    }
    return "unknown";
}

const char* dp4aTiledFusionKindName(const Dp4aTiledFusionKind kind)
{
    switch (kind) {
    case Dp4aTiledFusionKind::SingleStageExactTile:
        return "single-stage-exact-tile";
    case Dp4aTiledFusionKind::Stage1Stage2C16FusedOverlap:
        return "stage1-stage2-c16-fused-overlap";
    case Dp4aTiledFusionKind::Stage1Stage2Stage3C16PairedMacroblock:
        return "stage1-stage2-stage3-c16-paired-macroblock";
    }
    return "unknown";
}

bool pipelineVariantAllowsDp4aTiledFusion(const PipelineVariant variant)
{
    return variant == PipelineVariant::Dp4aTunedCandidate || variant == PipelineVariant::Dp4aOptimized;
}

Dp4aTiledFusionPlanDesc makeDp4aTiledFusionPlan(const GraphDesc& graph)
{
    Dp4aTiledFusionPlanDesc plan{};
    plan.pipelineVariant = graph.variant;
    plan.displaySize = graph.displaySize;
    plan.modelC16Size = DisplaySize{.width = halfDown(graph.displaySize.width), .height = halfDown(graph.displaySize.height)};
    plan.strictReferenceGraphUnchanged = true;
    plan.runtimeReplacementImplemented = false;

    if (!pipelineVariantAllowsDp4aTiledFusion(graph.variant)) {
        return plan;
    }
    if (graph.displaySize.width == 0 || graph.displaySize.height == 0) {
        return plan;
    }
    if (!hasStageRange(graph, 1, 3)) {
        return plan;
    }

    // Next locality target: mirror the NPU Stage1+2->Stage3 paired-macroblock
    // dataflow on the GPU while staying DOT4/DP4A.  This is only a replacement
    // plan until a candidate shader is actually wired; it must not silently
    // fall back to the imported reference dispatches under a new runtime name.
    plan.islands.push_back(makeFrontC16PairedMacroblockIsland(graph.displaySize));
    return plan;
}

} // namespace apusr::models::fsr4_int8
