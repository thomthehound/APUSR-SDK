// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/models/apusr_fsr4_int8_graph.h>

#include <cstdint>
#include <vector>

namespace apusr::models::fsr4_int8 {

enum class Dp4aFusionBoundaryPolicy : std::uint32_t {
    PreserveGlobalTensor,
    PreserveLocalQuantizedTensor,
    DeleteTransientGlobalTensor,
    TeeSkipSideband,
};

enum class Dp4aTiledFusionKind : std::uint32_t {
    SingleStageExactTile,
    Stage1Stage2C16FusedOverlap,
    Stage1Stage2Stage3C16PairedMacroblock,
};

struct Dp4aTileShapeDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;

    [[nodiscard]] constexpr std::uint32_t bytes() const noexcept
    {
        return width * height * channels;
    }
};

struct Dp4aFusionBoundaryDesc {
    const char* name = nullptr;
    Dp4aFusionBoundaryPolicy policy = Dp4aFusionBoundaryPolicy::PreserveGlobalTensor;
    Dp4aTileShapeDesc tile;
    // Whole-frame byte estimate for the selected display/capacity class.  This
    // is intentionally conservative and counts semantic tensor bytes, not cache
    // transactions, padding, or halo rereads.
    std::uint64_t conservativeFrameBytes = 0;
};

struct Dp4aTiledFusionIslandDesc {
    const char* name = nullptr;
    Dp4aTiledFusionKind kind = Dp4aTiledFusionKind::SingleStageExactTile;
    std::uint32_t firstLogicalStage = 0;
    std::uint32_t lastLogicalStage = 0;
    std::uint32_t outputLogicalStage = 0;
    bool exactLocalQuantizationRequired = true;
    bool runtimeReplacementImplemented = false;
    bool shaderArtifactAvailable = false;
    bool requiresSkipSidebandStore = false;
    bool requiresStage0SemanticChange = false;
    const char* candidateShaderPath = nullptr;
    const char* candidateEntryPoint = nullptr;
    const char* candidateDispatchName = nullptr;
    Dp4aTileShapeDesc sourceMacroTile;
    Dp4aTileShapeDesc localStage1Patch;
    Dp4aTileShapeDesc localStage2Patch;
    Dp4aTileShapeDesc outputTile;
    std::vector<Dp4aFusionBoundaryDesc> boundaries;
    std::uint64_t conservativeAvoidedFrameBytes = 0;
};

struct Dp4aTiledFusionPlanDesc {
    PipelineVariant pipelineVariant = PipelineVariant::StrictReference;
    DisplaySize displaySize;
    DisplaySize modelC16Size;
    bool strictReferenceGraphUnchanged = true;
    bool runtimeReplacementImplemented = false;
    std::vector<Dp4aTiledFusionIslandDesc> islands;
};

[[nodiscard]] const char* dp4aFusionBoundaryPolicyName(Dp4aFusionBoundaryPolicy policy);
[[nodiscard]] const char* dp4aTiledFusionKindName(Dp4aTiledFusionKind kind);
[[nodiscard]] bool pipelineVariantAllowsDp4aTiledFusion(PipelineVariant variant);
[[nodiscard]] Dp4aTiledFusionPlanDesc makeDp4aTiledFusionPlan(const GraphDesc& graph);

} // namespace apusr::models::fsr4_int8
