// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/models/apusr_fsr4_int8_graph.h>
#include <apusr/models/apusr_fsr4_int8_tiled_fusion.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef APUSR_REPO_ROOT
#define APUSR_REPO_ROOT "."
#endif

namespace {

using namespace apusr;
using namespace apusr::models::fsr4_int8;

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    assert(stream && "expected shader artifact to be present");
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

bool contains(const std::string& haystack, const char* needle)
{
    return haystack.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    const GraphDesc graph = makeDp4aTunedCandidateGraph(Preset::Balanced, FrameCapacity::R2160, DisplaySize{3840, 2160});
    const Dp4aTiledFusionPlanDesc plan = makeDp4aTiledFusionPlan(graph);
    assert(plan.islands.size() == 1);

    const Dp4aTiledFusionIslandDesc& island = plan.islands.front();
    assert(island.shaderArtifactAvailable);
    assert(!island.runtimeReplacementImplemented);
    assert(island.requiresStage0SemanticChange);
    assert(island.candidateShaderPath != nullptr);
    assert(island.candidateEntryPoint != nullptr);

    const std::filesystem::path shaderPath = std::filesystem::path(APUSR_REPO_ROOT) / island.candidateShaderPath;
    const std::string shader = readTextFile(shaderPath);

    assert(contains(shader, "APUSR_FSR4_I8_PASS1_TILED_EXACT_AVAILABLE 1"));
    assert(contains(shader, "APUSR_FSR4_I8_TILED_FUSION_STAGE0_SEMANTIC_CHANGE_REQUIRED 1"));
    assert(contains(shader, "APUSR_FSR4_I8_TILED_FUSION_EXACT_STAGE_BOUNDARY 1"));
    assert(contains(shader, "APUSR_FSR4_I8_TILED_FUSION_USES_GROUPSHARED_C16_HALO 1"));
    assert(contains(shader, island.candidateEntryPoint));
    assert(contains(shader, "groupshared uint apusr_pass1_c16_halo_tile"));
    assert(contains(shader, "GroupMemoryBarrierWithGroupSync"));
    assert(contains(shader, "APUSR_FSR4_PASS1_LOAD_W = APUSR_FSR4_PASS1_TILE_W + 2"));
    assert(contains(shader, "APUSR_FSR4_PASS1_INPUT_OFFSET_BYTES = 0"));
    assert(contains(shader, "APUSR_FSR4_PASS1_PADDED_STAGE0_BYTES"));
    assert(contains(shader, "ScratchBuffer.Load4(apusrPass1GlobalC16Offset"));
    assert(!contains(shader, "interiorLoadTile"));
    assert(contains(shader, "const uint activeWidth = width >> 1u"));
    assert(contains(shader, "ox >= activeWidth || oy >= activeHeight"));
    assert(contains(shader, "Padded Stage0 contract"));
    assert(contains(shader, "APUSR_FSR4_PASS1_LOAD_PIXELS"));
    assert(contains(shader, "APUSR_FSR4_PASS1_OUTPUT_OFFSET_BYTES = APUSR_FSR4_PASS1_PADDED_STAGE0_BYTES"));
    assert(contains(shader, "ScratchBuffer.Store4(apusrPass1OutputC16Offset"));

    std::filesystem::path metadataPath = shaderPath;
    metadataPath.replace_extension(".json");
    const std::string metadata = readTextFile(metadataPath);
    assert(contains(metadata, "\"shader_artifact_available\": true"));
    assert(contains(metadata, "\"implemented_runtime_replacement\": true"));
    assert(contains(metadata, "\"requires_stage0_semantic_change\": true"));
    assert(contains(metadata, "\"input_halo_tile\": ["));
    assert(contains(metadata, "\"groupshared_input_bytes\": 4224"));
    assert(contains(metadata, "\"output_tile\": ["));
    assert(contains(metadata, "\"preload_global_load_shape\": \"Load4 per C16 pixel\""));
    assert(contains(metadata, "\"interior_tile_fast_path\": false"));
    assert(contains(metadata, "\"semantic_change_required\": true"));
    assert(contains(metadata, "\"padded_c16_carrier\""));
    assert(contains(metadata, "\"status\": \"enabled_for_dp4a_optimized_front_c16\""));
    assert(contains(metadata, "\"stage1_output_offset\": \"paddedStage0Bytes\""));
    assert(contains(metadata, "\"scratch_budget_source\": \"include/apusr/models/apusr_fsr4_int8_scratch_layout.h\""));
    assert(contains(metadata, "\"supports_partial_final_strip\": true"));
    assert(contains(metadata, "\"does_not_assume_even_group_count\": true"));


    const std::filesystem::path pass2ShaderPath = std::filesystem::path(APUSR_REPO_ROOT) /
        "shaders/dx12/optimized/apusr_fsr4_i8_pass2_tiled_exact.hlsl";
    const std::string pass2Shader = readTextFile(pass2ShaderPath);
    assert(contains(pass2Shader, "APUSR_FSR4_I8_PASS2_TILED_EXACT_AVAILABLE 1"));
    assert(contains(pass2Shader, "APUSR_FSR4_I8_TILED_FUSION_STAGE0_SEMANTIC_CHANGE_REQUIRED 1"));
    assert(contains(pass2Shader, "APUSR_FSR4_I8_TILED_FUSION_EXACT_STAGE_BOUNDARY 1"));
    assert(contains(pass2Shader, "groupshared uint apusr_pass2_c16_halo_tile"));
    assert(contains(pass2Shader, "ScratchBuffer.Load4(apusrPass2GlobalC16Offset"));
    assert(contains(pass2Shader, "interiorLoadTile"));
    assert(contains(pass2Shader, "const uint activeWidth = width >> 1u"));
    assert(contains(pass2Shader, "ox >= activeWidth || oy >= activeHeight"));
    assert(contains(pass2Shader, "partial final strips from odd, portrait, ultrawide, and surround"));
    assert(contains(pass2Shader, "APUSR_FSR4_PASS2_INPUT_OFFSET_BYTES = APUSR_FSR4_PASS2_PADDED_STAGE0_BYTES"));
    assert(contains(pass2Shader, "APUSR_FSR4_PASS2_OUTPUT_OFFSET_BYTES = 0"));
    assert(contains(pass2Shader, "ScratchBuffer.Store4(apusrPass2OutputC16Offset"));

    std::filesystem::path pass2MetadataPath = pass2ShaderPath;
    pass2MetadataPath.replace_extension(".json");
    const std::string pass2Metadata = readTextFile(pass2MetadataPath);
    assert(contains(pass2Metadata, "\"pipeline_variant\": \"dp4a-optimized\""));
    assert(contains(pass2Metadata, "\"requires_stage0_semantic_change\": true"));
    assert(contains(pass2Metadata, "\"supports_partial_final_strip\": true"));
    assert(contains(pass2Metadata, "\"does_not_assume_even_group_count\": true"));

    return 0;
}
