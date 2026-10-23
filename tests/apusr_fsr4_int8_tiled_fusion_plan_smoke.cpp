// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_graph.h>
#include <apusr/models/apusr_fsr4_int8_tiled_fusion.h>

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

using namespace apusr;
using namespace apusr::models::fsr4_int8;

const Dp4aFusionBoundaryDesc* boundaryByName(const Dp4aTiledFusionIslandDesc& island, const char* name)
{
    for (const Dp4aFusionBoundaryDesc& boundary : island.boundaries) {
        if (boundary.name != nullptr && std::strcmp(boundary.name, name) == 0) {
            return &boundary;
        }
    }
    return nullptr;
}

} // namespace

int main()
{
    const DisplaySize display{.width = 3840, .height = 2160};

    const GraphDesc strictGraph = makeReferenceGraph(Preset::Balanced, FrameCapacity::R2160, display);
    const Dp4aTiledFusionPlanDesc strictPlan = makeDp4aTiledFusionPlan(strictGraph);
    assert(strictPlan.pipelineVariant == PipelineVariant::StrictReference);
    assert(strictPlan.strictReferenceGraphUnchanged);
    assert(!strictPlan.runtimeReplacementImplemented);
    assert(strictPlan.islands.empty());
    assert(!pipelineVariantAllowsDp4aTiledFusion(PipelineVariant::StrictReference));

    const GraphDesc tunedGraph = makeDp4aTunedCandidateGraph(Preset::Balanced, FrameCapacity::R2160, display);
    const Dp4aTiledFusionPlanDesc tunedPlan = makeDp4aTiledFusionPlan(tunedGraph);
    assert(tunedPlan.pipelineVariant == PipelineVariant::Dp4aTunedCandidate);
    assert(tunedPlan.strictReferenceGraphUnchanged);
    assert(!tunedPlan.runtimeReplacementImplemented);
    assert(pipelineVariantAllowsDp4aTiledFusion(PipelineVariant::Dp4aTunedCandidate));
    assert(pipelineVariantAllowsDp4aTiledFusion(PipelineVariant::Dp4aOptimized));
    assert(tunedPlan.modelC16Size.width == 1920);
    assert(tunedPlan.modelC16Size.height == 1080);
    assert(tunedPlan.islands.size() == 1);

    const Dp4aTiledFusionIslandDesc& island = tunedPlan.islands.front();
    assert(std::strcmp(island.name, "front-c16-stage1-2-3-paired-macroblock") == 0);
    assert(island.kind == Dp4aTiledFusionKind::Stage1Stage2Stage3C16PairedMacroblock);
    assert(std::strcmp(dp4aTiledFusionKindName(island.kind), "stage1-stage2-stage3-c16-paired-macroblock") == 0);
    assert(island.firstLogicalStage == 1);
    assert(island.lastLogicalStage == 3);
    assert(island.outputLogicalStage == 3);
    assert(island.exactLocalQuantizationRequired);
    assert(!island.runtimeReplacementImplemented);
    assert(island.shaderArtifactAvailable);
    assert(island.requiresSkipSidebandStore);
    assert(island.requiresStage0SemanticChange);
    assert(std::strcmp(island.candidateShaderPath, "shaders/dx12/optimized/apusr_fsr4_i8_pass1_tiled_exact.hlsl") == 0);
    assert(std::strcmp(island.candidateEntryPoint, "apusr_fsr4_i8_pass1_tiled_exact") == 0);
    assert(std::strcmp(island.candidateDispatchName, "apusr-fsr4-i8-pass1-tiled-exact") == 0);

    assert(island.sourceMacroTile.width == 36);
    assert(island.sourceMacroTile.height == 20);
    assert(island.sourceMacroTile.channels == 16);
    assert(island.sourceMacroTile.bytes() == 11520);

    assert(island.localStage1Patch.width == 34);
    assert(island.localStage1Patch.height == 18);
    assert(island.localStage1Patch.channels == 16);
    assert(island.localStage1Patch.bytes() == 9792);

    assert(island.localStage2Patch.width == 32);
    assert(island.localStage2Patch.height == 16);
    assert(island.localStage2Patch.channels == 16);
    assert(island.localStage2Patch.bytes() == 8192);

    assert(island.outputTile.width == 16);
    assert(island.outputTile.height == 8);
    assert(island.outputTile.channels == 32);
    assert(island.outputTile.bytes() == 4096);

    const std::uint64_t c16FrameBytes = 1920ull * 1080ull * 16ull;
    const auto* stage1To2 = boundaryByName(island, "stage1-to-stage2-c16");
    assert(stage1To2 != nullptr);
    assert(stage1To2->policy == Dp4aFusionBoundaryPolicy::DeleteTransientGlobalTensor);
    assert(stage1To2->conservativeFrameBytes == 2ull * c16FrameBytes);
    assert(std::strcmp(dp4aFusionBoundaryPolicyName(stage1To2->policy), "delete-transient-global-tensor") == 0);

    const auto* skipA = boundaryByName(island, "stage2-skipa-c16");
    assert(skipA != nullptr);
    assert(skipA->policy == Dp4aFusionBoundaryPolicy::TeeSkipSideband);
    assert(skipA->conservativeFrameBytes == c16FrameBytes);

    const auto* stage2To3 = boundaryByName(island, "stage2-to-stage3-mainpath-c16");
    assert(stage2To3 != nullptr);
    assert(stage2To3->policy == Dp4aFusionBoundaryPolicy::DeleteTransientGlobalTensor);
    assert(stage2To3->conservativeFrameBytes == c16FrameBytes);

    const auto* stage3Out = boundaryByName(island, "stage3-output-c32");
    assert(stage3Out != nullptr);
    assert(stage3Out->policy == Dp4aFusionBoundaryPolicy::PreserveGlobalTensor);
    assert(stage3Out->conservativeFrameBytes == 960ull * 540ull * 32ull);

    // We only count deleted global traffic in avoided bytes. SkipA is a required
    // sideband store, not an avoided write, because the decoder still needs it.
    assert(island.conservativeAvoidedFrameBytes == 3ull * c16FrameBytes);

    std::cout << "APUSR FSR4 INT8 tiled DP4A fusion plan smoke: front C16 macroblock contract validated\n";
    return 0;
}
