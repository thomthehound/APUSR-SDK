// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_graph.h>

#include <cassert>
#include <cstring>

namespace {

void assertGrid(const apusr::DispatchGrid grid, const std::uint32_t x, const std::uint32_t y, const std::uint32_t z = 1)
{
    assert(grid.groupsX == x);
    assert(grid.groupsY == y);
    assert(grid.groupsZ == z);
}

const apusr::models::fsr4_int8::PassDesc& passByName(
    const apusr::models::fsr4_int8::GraphDesc& graph,
    const char* name)
{
    for (const auto& pass : graph.passes) {
        if (std::strcmp(pass.name, name) == 0) {
            return pass;
        }
    }
    assert(false && "missing FSR4 INT8 graph pass");
    return graph.passes.front();
}

} // namespace

int main()
{
    using namespace apusr::models::fsr4_int8;

    static_assert(static_cast<std::uint32_t>(FrameCapacity::R2160) == 2160);
    static_assert(static_cast<std::uint32_t>(Preset::DynamicResolution) == 4);
    static_assert(static_cast<std::uint32_t>(Preset::UltraPerformance) == 5);
    assert(std::strcmp(presetName(Preset::Balanced), "balanced") == 0);
    assert(std::strcmp(frameCapacityName(FrameCapacity::R2160), "2160") == 0);
    assert(std::strcmp(colorSpaceName(ColorSpace::Srgb), "srgb") == 0);
    assert(std::strcmp(pipelineVariantName(PipelineVariant::StrictReference), "strict-reference") == 0);
    assert(std::strcmp(pipelineVariantName(PipelineVariant::Dp4aTunedCandidate), "cpu-dp4a-tuned") == 0);
    assert(std::strcmp(pipelineVariantName(PipelineVariant::Dp4aOptimized), "dp4a-optimized") == 0);
    assert(pipelineVariantIsStrictReference(PipelineVariant::StrictReference));
    assert(!pipelineVariantIsStrictReference(PipelineVariant::Dp4aTunedCandidate));
    assert(!pipelineVariantIsStrictReference(PipelineVariant::Dp4aOptimized));
    assert(!pipelineVariantAllowsModelChanges(PipelineVariant::StrictReference));
    assert(pipelineVariantAllowsModelChanges(PipelineVariant::Dp4aTunedCandidate));
    assert(pipelineVariantAllowsModelChanges(PipelineVariant::Dp4aOptimized));
    assert(presetForQualityMode(apusr::UpscalerQualityMode::Performance) == Preset::Performance);
    assert(presetForScale(1.70f) == Preset::Balanced);
    assert(frameCapacityForDisplay(apusr::DisplaySize{.width = 1920, .height = 1080}) == FrameCapacity::R1080);
    assert(frameCapacityForDisplay(apusr::DisplaySize{.width = 1921, .height = 1080}) == FrameCapacity::R2160);
    assert(frameCapacityForDisplay(apusr::DisplaySize{.width = 3440, .height = 1440}) == FrameCapacity::R2160);
    assert(frameCapacityForDisplay(apusr::DisplaySize{.width = 3840, .height = 2161}) == FrameCapacity::R4320);
    assert(displayFitsFrameCapacity(apusr::DisplaySize{.width = 3840, .height = 2160}, FrameCapacity::R2160));
    assert(!displayFitsFrameCapacity(apusr::DisplaySize{.width = 3841, .height = 2160}, FrameCapacity::R2160));
    assert(isReferenceDisplaySizeSupported(apusr::DisplaySize{.width = 7680, .height = 4320}));
    assert(!isReferenceDisplaySizeSupported(apusr::DisplaySize{.width = 7681, .height = 4320}));
    assert(presetPermutationValue(Preset::DynamicResolution) == 4);
    assert(presetPermutationValue(Preset::UltraPerformance) == 5);
    assert(colorSpacePermutationValue(ColorSpace::Pq) == 3);
    assert(maxResolutionPermutationValue(FrameCapacity::R2160) == 1);
    assert(permutationKey(PermutationDesc{.preset = Preset::Balanced, .frameCapacity = FrameCapacity::R2160}) == 10);
    assert(permutationKey(PermutationDesc{.preset = Preset::Balanced, .frameCapacity = FrameCapacity::R2160, .depthInverted = true, .autoExposure = true, .jitteredMotionVectors = true, .colorSpace = ColorSpace::Srgb}) == 3914);
    const ProductPermutationMatrixDesc productMatrix = defaultDx12Dp4aProductPermutationMatrix();
    assert(productPermutationCount(productMatrix) == 1152);
    assert(enumerateProductPermutations(productMatrix).size() == productPermutationCount(productMatrix));
    assert(productPermutationCount(defaultDx12Dp4aProductPermutationMatrix(true)) == 2304);

    const apusr::DisplaySize display{.width = 3440, .height = 1440};
    const GraphDesc graph = makeReferenceGraph(Preset::Balanced, FrameCapacity::R2160, display);
    assert(graph.variant == PipelineVariant::StrictReference);
    assert(graph.passes.size() == 17);
    assert(graph.artifacts.preShader == "models/fsr4_int8/reference/fsr4_4_0_2_int8/balanced/pre.hlsl");
    assert(graph.artifacts.passesShader == "models/fsr4_int8/reference/fsr4_4_0_2_int8/balanced/passes_2160.hlsl");
    assert(graph.artifacts.postShader == "models/fsr4_int8/reference/fsr4_4_0_2_int8/balanced/post.hlsl");
    assert(graph.artifacts.initializers == "models/fsr4_int8/reference/fsr4_4_0_2_int8/balanced/initializers.bin");
    assert(graph.artifacts.includeRoot == "models/fsr4_int8/reference/fsr4_4_0_2_int8/include");
    assert(graph.artifacts.fsr4IncludeRoot == "models/fsr4_int8/reference/fsr4_4_0_2_int8/include/fsr4");
    assert(graph.artifacts.includeRoots.size() == 4);

    assert(passByName(graph, "fsr4_pre").entryPoint == "main");
    assert(passByName(graph, "fsr4_pass_12").entryPoint == "fsr4_model_v07_i8_pass12");
    assert(passByName(graph, "fsr4_post").entryPoint == "main");
    assert(passByName(graph, "rcas").entryPoint == "main");
    assertGrid(passByName(graph, "fsr4_pre").dp4aDispatch, 215, 90);
    assertGrid(passByName(graph, "fsr4_pass_1").dp4aDispatch, 27, 720);
    assertGrid(passByName(graph, "fsr4_pass_3").dp4aDispatch, 14, 360);
    assertGrid(passByName(graph, "fsr4_pass_6").dp4aDispatch, 7, 180);
    assertGrid(passByName(graph, "fsr4_pass_9").dp4aDispatch, 54, 23);
    assertGrid(passByName(graph, "fsr4_pass_12").dp4aDispatch, 27, 720);
    assertGrid(passByName(graph, "fsr4_post").dp4aDispatch, 215, 90);
    assertGrid(passByName(graph, "debug_view").dp4aDispatch, 430, 180);

    assert(passByName(graph, "fsr4_pass_1").npuIslandCandidate);
    assert(passByName(graph, "fsr4_pass_3").npuIslandCandidate);
    assert(!passByName(graph, "fsr4_pass_4").npuIslandCandidate);
    assert(passByName(graph, "fsr4_pass_12").wmmaCandidate);
    assert(passByName(graph, "rcas").optional);
    const GraphDesc tunedGraph = makeDp4aTunedCandidateGraph(Preset::Balanced, FrameCapacity::R2160, display);
    assert(tunedGraph.variant == PipelineVariant::Dp4aTunedCandidate);
    assert(tunedGraph.passes.size() == graph.passes.size());
    assert(passByName(tunedGraph, "fsr4_pre").shaderPath == passByName(graph, "fsr4_pre").shaderPath);
    assert(passByName(tunedGraph, "fsr4_pass_12").entryPoint == passByName(graph, "fsr4_pass_12").entryPoint);

    const GraphDesc optimizedGraph = makeDp4aOptimizedGraph(Preset::Balanced, FrameCapacity::R2160, display);
    assert(optimizedGraph.variant == PipelineVariant::Dp4aOptimized);
    assert(optimizedGraph.passes.size() == graph.passes.size());
    assert(passByName(optimizedGraph, "fsr4_pre").shaderPath == passByName(graph, "fsr4_pre").shaderPath);
    assertGrid(passByName(optimizedGraph, "fsr4_pass_1").dp4aDispatch, 27, 360);
    assertGrid(passByName(optimizedGraph, "fsr4_pass_2").dp4aDispatch, 27, 360);
    assertGrid(passByName(optimizedGraph, "fsr4_pass_12").dp4aDispatch, 27, 720);

    return 0;
}
