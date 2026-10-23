// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "could not open " << path << '\n';
        std::exit(1);
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

int main()
{
#ifdef APUSR_REPO_ROOT
    const std::filesystem::path repoRoot = APUSR_REPO_ROOT;
#else
    const std::filesystem::path repoRoot = std::filesystem::current_path() / "..";
#endif
    const std::string primitive2col = readText(
        repoRoot / "kernels/apusr_middle_core_stage1_12_spatial2col/src/aie/apusr_middle_core_vector_primitives.cc");
    const std::string primitive4col = readText(
        repoRoot / "kernels/apusr_middle_core_stage1_12_spatial4col/src/aie/apusr_middle_core_vector_primitives.cc");
    const std::string graph = readText(repoRoot / "tools/analysis/middle_core_spatial_graph.py");

    require(primitive2col == primitive4col, "active 2col and 4col vector primitive sources must stay aligned");

    for (const std::string* primitive : {&primitive2col, &primitive4col}) {
        require(contains(*primitive, "aie::mmul<"), "vector primitive must use AIE mmul");
        require(contains(*primitive, "aie::mul<acc64>"), "requant multiply must stay on the AIE vector path");
        require(contains(*primitive, "aie::to_vector<int8>"), "epilogue must narrow through AIE SRS-style conversion");
        require(contains(*primitive, "set_saturation"), "primitive must enable saturating narrowing");
        require(contains(*primitive, "set_rounding"), "primitive must set explicit SRS rounding");
        require(contains(*primitive, "load_c16_m4x8_halo_patch_tap"), "front primitive must load halo taps in m4x8 vector shape");
        require(contains(*primitive, "aie::concat"), "odd-column halo taps must be assembled with vector concat, not scalar byte packing");
        require(contains(*primitive, "aie::shuffle_down"), "odd-column halo taps must use vector shuffle alignment");
        require(contains(*primitive, "m4x8_cell_offset"), "internal C32/C64 cell storage must be m4x8 block-major");
        require(contains(*primitive, "store_c32_m4x8"), "front primitive must store C32 m4x8 blocks directly");
        require(contains(*primitive, "kStage1HotContractWeightsOffset"), "Stage-1 primitive must consume contract weights from the tile-local hot record");
        require(contains(*primitive, "kStage1HotResidualScaleBlocksOffset"), "Stage-1 primitive must consume residual scale blocks from the tile-local hot record");
        require(contains(*primitive, "load_c16_m4x8_halo_patch_tap_i32"), "resident residual legs must widen C16 residual taps through the vector load/unpack path");
        require(contains(*primitive, "aie::unpack(residual_i8)"), "resident residual legs must avoid scalar i8-to-i32 gather loops");
        require(contains(*primitive, "spatial3x3_c16_halo_patch_m4x8_with_epilogue_blocks"), "Stage-1 proof must hoist compact epilogue expansion out of the per-cell spatial loop");
        require(contains(*primitive, "kStage1HotExpandBiasBlocksOffset"), "Stage-1 proof must load expand epilogue blocks from the tile-local hot record");
        require(contains(*primitive, "kStage1HotContractBiasBlocksOffset"), "Stage-1 proof must load contract epilogue blocks from the tile-local hot record");
        require(contains(*primitive, "kStage1HotResidualScaleBlocksOffset"), "Stage-1 proof must load residual scale blocks from the tile-local hot record");
        require(!contains(*primitive, "expand_bias_blocks[kOutputChunksC32]"), "Stage-1 hot path must not allocate expanded epilogue arrays inside the primitive");
        require(!contains(*primitive, "const aie::vector<int8, kElements> residual_v"), "resident residual legs must not multiply int8 residual vectors by int32 scale vectors");
        require(!contains(*primitive, "residual_block"), "resident residual legs must not allocate scalar-expanded residual scratch blocks");
        require(contains(*primitive, "kFrontStage1CompactRecordBytes"), "Stage-1 source arena record size must remain explicit");
        require(contains(*primitive, "kStage1HotRecordBytes"), "Stage-1 primitive must lock the expanded tile-local hot record size");
        require(contains(*primitive, "c16_halo_patch_stage1_convnext16_srs_to_c16_cell_impl"), "Stage-1 primitive must implement C16 halo patch to C16 cell directly");
        require(contains(*primitive, "apusr_middle_core_stage1_convnext16_halo_patch_to_c16_cell_i8"), "Stage-1 primitive must expose the resident Stage-1 entry point");
        require(contains(*primitive, "load_c32_m4x8"), "C32 consumers must load m4x8 blocks directly");
        require(contains(*primitive, "store_c16_m4x8"), "final C16 egress must store m4x8 blocks directly");
        require(!contains(*primitive, "scatter_c16_m4x8"), "final C16 egress must not scatter row-major bytes");
        require(contains(*primitive, "spatial3x3_c16_halo_patch_m4x8"), "front primitive must run C16 spatial 3x3 over the halo patch");
        require(contains(*primitive, "c16_halo_patch_spatial3x3_expand_srs_to_c32_cell_impl"), "front primitive must fuse spatial 3x3 and C16-to-C32 expansion");
        require(contains(*primitive, "apusr_middle_core_front_c16_halo_patch_expand_srs_to_c32_cell_i8"), "front primitive must expose the halo-patch vector/SRS entry point");
        require(contains(*primitive, "materialize_stage1_2_macro_stage1_patch34x18"), "paired Stage1+2 macroblock must materialize one shared Stage1 patch");
        require(contains(*primitive, "stage1_2_macro_stage2_from_stage1_patch34x18_cell"), "paired Stage1+2 macroblock must reuse the shared Stage1 patch for all four Stage2 cells");
        require(!contains(*primitive, "materialize_stage1_2_source20x12_from_macroblock36x20"), "paired macroblock must not slice four 20x12 windows and recompute Stage1");

        require(!contains(*primitive, "long long"), "primitive must not use scalar long long");
        require(!contains(*primitive, "int64_t"), "primitive must not use scalar int64_t");
        require(!contains(*primitive, "uint64_t"), "primitive must not use scalar uint64_t");
        require(!contains(*primitive, "__int64"), "primitive must not use scalar __int64");
        require(!contains(*primitive, " / "), "primitive must not use C/C++ division");
        require(!contains(*primitive, " % "), "primitive must not use C/C++ modulo");
    }

    require(contains(graph, "hot-path parameter streams: 0"), "graph summary must keep hot parameter streams at zero");
    require(contains(graph, "return build_stage1_2_to_stage3_paired_macroblock_resident_program"), "default spatial-core must emit the Stage1+2 -> Stage3 paired macroblock SkipA graph");
    require(contains(graph, "active milestone graph: fused Stage1+2 -> paired Stage2 2x2 C16 macroblock -> Stage3 downsample"), "spatial-core summary must describe the active Stage1-4 milestone graph");
    require(contains(graph, "Stage2 output policy: produce every Stage2 C16 cell exactly once in paired 2x2 macroblocks, teeing the same cells to arg6 SkipA"), "spatial-core summary must describe the Stage2 SkipA tee");
    require(contains(graph, "ring policy: no full Stage2 row ObjectFifos and no fused Stage3/4 20x40 C16 mega-window in the primary path"), "spatial-core summary must be honest about the Stage2 ring carrier");
    require(contains(graph, "apusr_middle_core_stage1_2_paired_macroblock36x20_to_c16_window2x2_i8_shared_skipa"), "spatial-core must call the fused Stage1+2 resident primitive");
    require(contains(graph, "apusr_middle_core_stage3_downsample_window_to_c32_cell_i8"), "spatial-core must call the split Stage3 downsample primitive");
    require(contains(graph, "skipa_fifos"), "spatial-core must tee fused Stage1+2 output to SkipA");
    require(contains(graph, "skipa_c16_2x2_raster_l1l3"), "spatial-core must have a single raster SkipA ObjectFifo consumer");
    require(contains(graph, "skipa_c16_2x2_raster_l1l3\", depth=1"), "spatial-core must keep SkipA raster sideband to one macroblock of occupancy");
    require(contains(graph, "skip_spill_arena_ty"), "spatial-core must expose the combined SkipA/SkipB arena as a sequence argument");
    require(contains(graph, "_stage2_skipa_spill_macroblock2x2_raster_tap"), "spatial-core must use the bounded raster SkipA 2x2 spill tap");
    require(contains(graph, "c16_window2x2_ty"), "spatial-core carrier must expose the paired Stage2 C16 macroblock object");
    require(contains(graph, "_stage1_2_macroblock36x20_for_stage3_row_block_tap"), "Stage3 input must be built from paired Stage2 macroblocks");
    require(!contains(graph, "return build_stage1_2_to_stage3_4_worker_ring_resident_program"), "default spatial-core must not route through the rejected Stage3/4 row-ring graph");
    require(contains(graph, "_stage3_c32_interior_output_row_block_tap"), "Stage3 C32 drain must use bounded row-block taps");
    require(contains(graph, "_stage1_2_macroblock36x20_for_stage3_row_block_tap"), "fused Stage1+2 grouped ingress must use bounded source-window row-block taps");
    require(contains(graph, "apusr_middle_core_vector_primitives.o"), "graph paths must call the vector primitive object");
    require(contains(graph, "build_stage1_resident_program"), "graph must expose the standalone Stage-1 resident proof");
    require(contains(graph, "stage1_params = Buffer(stage1_param_ty"), "Stage-1 proof must keep Stage-1 parameters tile-local and resident");
    require(contains(graph, "STAGE1_HOT_RECORD_BYTES"), "Stage-1 proof must use the expanded tile-local hot record size");
    require(contains(graph, "initial_value=_stage1_identity_params()"), "Stage-1 proof must seed a complete tile-local hot record for the compile proof");
    require(contains(graph, "apusr_middle_core_stage1_convnext16_halo_patch_to_c16_cell_i8"), "Stage-1 proof must call the resident Stage-1 primitive");
    require(!contains(graph, "neighbor_memory_tile_joined_pair_object_fifo"), "spatial-core must not keep the old joined-pair seam staging path active");
    require(!contains(graph, "right_to_"), "spatial-core must not wire right-to-left seam ObjectFifos");
    require(!contains(graph, "left_to_"), "spatial-core must not wire left-to-right seam ObjectFifos");
    require(!contains(graph, "neighbor_lr_seam_pair"), "spatial-core must not join explicit seam directions");
    require(!contains(graph, "core_front_row_ring_l2l1"), "spatial-core must not allocate a full-shard front row ring");
    require(!contains(graph, "FRONT_C16_RING_ROWS * shard.cell_columns"), "spatial-core must not allocate full-shard cell row-ring ObjectFifo depths");
    require(!contains(graph, "_transfer_shard_row_tap(plan.frame_slot_bytes, shard)"), "spatial-core must not use full shard-row taps for front haloing");
    require(!contains(graph, "zero_row"), "spatial-core must not synthesize top/bottom rows in compute-tile L1");
    require(!contains(graph, "zero_seam"), "spatial-core must not synthesize seam rows in compute-tile L1");
    require(!contains(graph, "\"apusr_middle_core_front_c16_to_c32_i8\""), "spatial-core must not use the placeholder scalar front kernel");
    require(!contains(graph, "parameter package stream"), "graph must not reintroduce parameter package streams");
    require(contains(graph, "_stage2_skipa_spill_macroblock2x2_raster_tap"), "Stage2 graph must expose a bounded raster SkipA 2x2 spill tap");
    require(contains(graph, "skip_spill_arena_ty"), "Stage2 graph must expose the combined SkipA/SkipB arena as a sequence argument");
    require(contains(graph, "skipa_fifos"), "fused Stage1+2 graph must tee Stage2 output to SkipA");
    require(contains(graph, "stage2_skipa_spill_fifos"), "split Stage1/2 graph must tee Stage2 output to SkipA");
    require(contains(graph, "skipa_c16_2x2_raster_l1l3"), "fused Stage1+2 carrier must have one raster SkipA ObjectFifo consumer");
    require(contains(graph, "s12ring_skipa_spill_c16_l2l3"), "split Stage1/2 carrier must have a SkipA ObjectFifo consumer");

    std::cout << "APUSR AIE vector primitive contract smoke: halo-patch vector/SRS hot path validated\n";
    return 0;
}
