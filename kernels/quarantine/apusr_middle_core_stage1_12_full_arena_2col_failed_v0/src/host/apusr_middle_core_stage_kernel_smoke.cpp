// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

extern "C" void apusr_middle_core_convnext16_tile_i8(
    const std::int8_t* input_tile_18x10_c16,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_16x8_c16);

extern "C" void apusr_middle_core_down2x2_fasternet32_tile_i8(
    const std::int8_t* input_tile_40x20_c16,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_16x8_c32);

extern "C" void apusr_middle_core_fasternet32_tile_i8(
    const std::int8_t* input_tile_18x10_c32,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_16x8_c32);

extern "C" void apusr_middle_core_fnb32_ct2d16_add_tile_i8(
    const std::int8_t* input_tile_18x10_c32,
    const std::int8_t* skip_tile_32x16_c16,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_32x16_c16);

extern "C" void apusr_middle_core_fnb32_c8group_ct2d16_add_tile_i8(
    const std::int8_t* input_chunk0_18x10_c8,
    const std::int8_t* input_chunk1_18x10_c8,
    const std::int8_t* input_chunk2_18x10_c8,
    const std::int8_t* input_chunk3_18x10_c8,
    const std::int8_t* skip_tile_32x16_c16,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_32x16_c16);

extern "C" void apusr_middle_core_down2x2_fasternet64_tile_i8(
    const std::int8_t* input_tile_40x20_c32,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_16x8_c64);

extern "C" void apusr_middle_core_fasternet64_tile_i8(
    const std::int8_t* input_tile_18x10_c64,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_16x8_c64);

extern "C" void apusr_middle_core_fnb64_ct2d32_add_tile_i8(
    const std::int8_t* input_tile_18x10_c64,
    const std::int8_t* skip_tile_32x16_c32,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_32x16_c32);

extern "C" void apusr_middle_core_ct2d32_ochunk_add_tile_i8(
    const std::int8_t* input_tile_16x8_c64,
    const std::int8_t* skip_tile_32x16_c8,
    const std::int8_t* param_blob,
    std::int8_t* output_tile_32x16_c8);

extern "C" void apusr_middle_core_ct2d32_ochunk_halo4_add_tile_i8(
    const std::int8_t* input_tile_16x8_c64,
    const std::int8_t* skip_tile_32x16_c8,
    const std::int8_t* param_blob,
    std::int8_t* output_halo_tl_18x10_c8,
    std::int8_t* output_halo_tr_18x10_c8,
    std::int8_t* output_halo_bl_18x10_c8,
    std::int8_t* output_halo_br_18x10_c8);


extern "C" void apusr_middle_core_stage5_c8group_skipb_downsource_tile_i8(
    const std::int8_t* stage5_output_quadrants_qmajor_c8group,
    std::int8_t* skipb_grouped_c8_32x16_c32,
    std::int8_t* downsource_grouped_c8_40x20_c32);

extern "C" void apusr_middle_core_stage6_7_c64_halo4_tile_i8(
    const std::int8_t* stage6_7_output_quadrants_qmajor_nhwc_c64,
    std::int8_t* stage8_halos_qmajor_nhwc_c64);

extern "C" void apusr_middle_core_stage10_c8group_stage11_halo4_c8group_tile_i8(
    const std::int8_t* stage10_output_quadrants_qmajor_c8group,
    std::int8_t* stage11_halos_qmajor_c8group);

extern "C" void apusr_middle_core_stage10_c8group_stage11_halo4_tile_i8(
    const std::int8_t* stage10_output_quadrants_qmajor_c8group,
    std::int8_t* stage11_halos_qmajor_nhwc_c32);

extern "C" void apusr_middle_core_stage11_c16_stage12_halo4_tile_i8(
    const std::int8_t* stage11_output_32x16_nhwc_c16,
    std::int8_t* stage12_halos_qmajor_nhwc_c16);

extern "C" void apusr_middle_core_fasternet32_c8group_tile_i8(
    const std::int8_t* input_chunk0_18x10_c8,
    const std::int8_t* input_chunk1_18x10_c8,
    const std::int8_t* input_chunk2_18x10_c8,
    const std::int8_t* input_chunk3_18x10_c8,
    const std::int8_t* param_blob,
    std::int8_t* output_chunk0_16x8_c8,
    std::int8_t* output_chunk1_16x8_c8,
    std::int8_t* output_chunk2_16x8_c8,
    std::int8_t* output_chunk3_16x8_c8);

namespace {

constexpr int kInputTileBytes = 18 * 10 * 16;
constexpr int kOutputTileBytes = 16 * 8 * 16;
constexpr int kSpatialWeightsOffset = 16;
constexpr int kSpatialBiasOffset = kSpatialWeightsOffset + (2 * 9 * 2 * 64);
constexpr int kSpatialMultiplierOffset = kSpatialBiasOffset + (16 * 4);
constexpr int kExpandWeightsOffset = kSpatialMultiplierOffset + (16 * 4);
constexpr int kExpandBiasOffset = kExpandWeightsOffset + (4 * 2 * 64);
constexpr int kExpandMultiplierOffset = kExpandBiasOffset + (32 * 4);
constexpr int kContractWeightsOffset = kExpandMultiplierOffset + (32 * 4);
constexpr int kContractBiasOffset = kContractWeightsOffset + (2 * 4 * 64);
constexpr int kContractMultiplierOffset = kContractBiasOffset + (16 * 4);
constexpr int kResidualMultiplierOffset = kContractMultiplierOffset + (16 * 4);
constexpr int kResidualShiftOffset = kResidualMultiplierOffset + (16 * 4);
constexpr int kResidualZeroPointOffset = kResidualShiftOffset + (16 * 4);
constexpr int kParamBytes = kResidualZeroPointOffset + (16 * 4);

constexpr int kDownFnbInputTileBytes = 40 * 20 * 16;
constexpr int kDownFnbOutputTileBytes = 16 * 8 * 32;
constexpr int kDownFnbDownWeightsOffset = 16;
constexpr int kDownFnbDownBiasOffset = kDownFnbDownWeightsOffset + (4 * 4 * 2 * 64);
constexpr int kDownFnbDownMultiplierOffset = kDownFnbDownBiasOffset + (32 * 4);
constexpr int kDownFnbSpatialWeightsOffset = kDownFnbDownMultiplierOffset + (32 * 4);
constexpr int kDownFnbSpatialBiasOffset = kDownFnbSpatialWeightsOffset + (9 * 2 * 2 * 64);
constexpr int kDownFnbSpatialMultiplierOffset = kDownFnbSpatialBiasOffset + (16 * 4);
constexpr int kDownFnbExpandWeightsOffset = kDownFnbSpatialMultiplierOffset + (16 * 4);
constexpr int kDownFnbExpandBiasOffset = kDownFnbExpandWeightsOffset + (8 * 4 * 64);
constexpr int kDownFnbExpandMultiplierOffset = kDownFnbExpandBiasOffset + (64 * 4);
constexpr int kDownFnbContractWeightsOffset = kDownFnbExpandMultiplierOffset + (64 * 4);
constexpr int kDownFnbContractBiasOffset = kDownFnbContractWeightsOffset + (4 * 8 * 64);
constexpr int kDownFnbContractMultiplierOffset = kDownFnbContractBiasOffset + (32 * 4);
constexpr int kDownFnbResidualMultiplierOffset = kDownFnbContractMultiplierOffset + (32 * 4);
constexpr int kDownFnbResidualShiftOffset = kDownFnbResidualMultiplierOffset + (32 * 4);
constexpr int kDownFnbResidualZeroPointOffset = kDownFnbResidualShiftOffset + (32 * 4);
constexpr int kDownFnbParamBytes = kDownFnbResidualZeroPointOffset + (32 * 4);

constexpr int kFnb32InputTileBytes = 18 * 10 * 32;
constexpr int kFnb32OutputTileBytes = 16 * 8 * 32;
constexpr int kFnb32C8InputChunkBytes = 18 * 10 * 8;
constexpr int kFnb32C8OutputChunkBytes = 16 * 8 * 8;
constexpr int kFnb32SpatialWeightsOffset = 16;
constexpr int kFnb32SpatialBiasOffset = kFnb32SpatialWeightsOffset + (9 * 2 * 2 * 64);
constexpr int kFnb32SpatialMultiplierOffset = kFnb32SpatialBiasOffset + (16 * 4);
constexpr int kFnb32ExpandWeightsOffset = kFnb32SpatialMultiplierOffset + (16 * 4);
constexpr int kFnb32ExpandBiasOffset = kFnb32ExpandWeightsOffset + (8 * 4 * 64);
constexpr int kFnb32ExpandMultiplierOffset = kFnb32ExpandBiasOffset + (64 * 4);
constexpr int kFnb32ContractWeightsOffset = kFnb32ExpandMultiplierOffset + (64 * 4);
constexpr int kFnb32ContractBiasOffset = kFnb32ContractWeightsOffset + (4 * 8 * 64);
constexpr int kFnb32ContractMultiplierOffset = kFnb32ContractBiasOffset + (32 * 4);
constexpr int kFnb32ResidualMultiplierOffset = kFnb32ContractMultiplierOffset + (32 * 4);
constexpr int kFnb32ResidualShiftOffset = kFnb32ResidualMultiplierOffset + (32 * 4);
constexpr int kFnb32ResidualZeroPointOffset = kFnb32ResidualShiftOffset + (32 * 4);
constexpr int kFnb32ParamBytes = kFnb32ResidualZeroPointOffset + (32 * 4);

constexpr int kFnb32Ct2d16PrimaryTileBytes = 18 * 10 * 32;
constexpr int kFnb32Ct2d16SkipTileBytes = 32 * 16 * 16;
constexpr int kFnb32Ct2d16OutputTileBytes = 32 * 16 * 16;
constexpr int kFnb32Ct2d16SpatialWeightsOffset = 16;
constexpr int kFnb32Ct2d16SpatialBiasOffset = kFnb32Ct2d16SpatialWeightsOffset + (9 * 2 * 2 * 64);
constexpr int kFnb32Ct2d16SpatialMultiplierOffset = kFnb32Ct2d16SpatialBiasOffset + (16 * 4);
constexpr int kFnb32Ct2d16ExpandWeightsOffset = kFnb32Ct2d16SpatialMultiplierOffset + (16 * 4);
constexpr int kFnb32Ct2d16ExpandBiasOffset = kFnb32Ct2d16ExpandWeightsOffset + (8 * 4 * 64);
constexpr int kFnb32Ct2d16ExpandMultiplierOffset = kFnb32Ct2d16ExpandBiasOffset + (64 * 4);
constexpr int kFnb32Ct2d16ContractWeightsOffset = kFnb32Ct2d16ExpandMultiplierOffset + (64 * 4);
constexpr int kFnb32Ct2d16ContractBiasOffset = kFnb32Ct2d16ContractWeightsOffset + (4 * 8 * 64);
constexpr int kFnb32Ct2d16ContractMultiplierOffset = kFnb32Ct2d16ContractBiasOffset + (32 * 4);
constexpr int kFnb32Ct2d16ResidualMultiplierOffset = kFnb32Ct2d16ContractMultiplierOffset + (32 * 4);
constexpr int kFnb32Ct2d16ResidualShiftOffset = kFnb32Ct2d16ResidualMultiplierOffset + (32 * 4);
constexpr int kFnb32Ct2d16ResidualZeroPointOffset = kFnb32Ct2d16ResidualShiftOffset + (32 * 4);
constexpr int kFnb32Ct2d16Ct2dWeightsOffset = kFnb32Ct2d16ResidualZeroPointOffset + (32 * 4);
constexpr int kFnb32Ct2d16Ct2dBiasOffset = kFnb32Ct2d16Ct2dWeightsOffset + (4 * 2 * 4 * 64);
constexpr int kFnb32Ct2d16Ct2dMultiplierOffset = kFnb32Ct2d16Ct2dBiasOffset + (16 * 4);
constexpr int kFnb32Ct2d16SkipMultiplierOffset = kFnb32Ct2d16Ct2dMultiplierOffset + (16 * 4);
constexpr int kFnb32Ct2d16SkipShiftOffset = kFnb32Ct2d16SkipMultiplierOffset + (16 * 4);
constexpr int kFnb32Ct2d16SkipZeroPointOffset = kFnb32Ct2d16SkipShiftOffset + (16 * 4);
constexpr int kFnb32Ct2d16ParamBytes = kFnb32Ct2d16SkipZeroPointOffset + (16 * 4);

constexpr int kDownFnb64InputTileBytes = 40 * 20 * 32;
constexpr int kDownFnb64OutputTileBytes = 16 * 8 * 64;
constexpr int kDownFnb64DownWeightsOffset = 16;
constexpr int kDownFnb64DownBiasOffset = kDownFnb64DownWeightsOffset + (4 * 8 * 4 * 64);
constexpr int kDownFnb64DownMultiplierOffset = kDownFnb64DownBiasOffset + (64 * 4);
constexpr int kDownFnb64SpatialWeightsOffset = kDownFnb64DownMultiplierOffset + (64 * 4);
constexpr int kDownFnb64SpatialBiasOffset = kDownFnb64SpatialWeightsOffset + (9 * 4 * 4 * 64);
constexpr int kDownFnb64SpatialMultiplierOffset = kDownFnb64SpatialBiasOffset + (32 * 4);
constexpr int kDownFnb64ExpandWeightsOffset = kDownFnb64SpatialMultiplierOffset + (32 * 4);
constexpr int kDownFnb64ExpandBiasOffset = kDownFnb64ExpandWeightsOffset + (16 * 8 * 64);
constexpr int kDownFnb64ExpandMultiplierOffset = kDownFnb64ExpandBiasOffset + (128 * 4);
constexpr int kDownFnb64ContractWeightsOffset = kDownFnb64ExpandMultiplierOffset + (128 * 4);
constexpr int kDownFnb64ContractBiasOffset = kDownFnb64ContractWeightsOffset + (8 * 16 * 64);
constexpr int kDownFnb64ContractMultiplierOffset = kDownFnb64ContractBiasOffset + (64 * 4);
constexpr int kDownFnb64ResidualMultiplierOffset = kDownFnb64ContractMultiplierOffset + (64 * 4);
constexpr int kDownFnb64ResidualShiftOffset = kDownFnb64ResidualMultiplierOffset + (64 * 4);
constexpr int kDownFnb64ResidualZeroPointOffset = kDownFnb64ResidualShiftOffset + (64 * 4);
constexpr int kDownFnb64ParamBytes = kDownFnb64ResidualZeroPointOffset + (64 * 4);

constexpr int kFnb64InputTileBytes = 18 * 10 * 64;
constexpr int kFnb64OutputTileBytes = 16 * 8 * 64;
constexpr int kFnb64SpatialWeightsOffset = 16;
constexpr int kFnb64SpatialBiasOffset = kFnb64SpatialWeightsOffset + (9 * 4 * 4 * 64);
constexpr int kFnb64SpatialMultiplierOffset = kFnb64SpatialBiasOffset + (32 * 4);
constexpr int kFnb64ExpandWeightsOffset = kFnb64SpatialMultiplierOffset + (32 * 4);
constexpr int kFnb64ExpandBiasOffset = kFnb64ExpandWeightsOffset + (16 * 8 * 64);
constexpr int kFnb64ExpandMultiplierOffset = kFnb64ExpandBiasOffset + (128 * 4);
constexpr int kFnb64ContractWeightsOffset = kFnb64ExpandMultiplierOffset + (128 * 4);
constexpr int kFnb64ContractBiasOffset = kFnb64ContractWeightsOffset + (8 * 16 * 64);
constexpr int kFnb64ContractMultiplierOffset = kFnb64ContractBiasOffset + (64 * 4);
constexpr int kFnb64ResidualMultiplierOffset = kFnb64ContractMultiplierOffset + (64 * 4);
constexpr int kFnb64ResidualShiftOffset = kFnb64ResidualMultiplierOffset + (64 * 4);
constexpr int kFnb64ResidualZeroPointOffset = kFnb64ResidualShiftOffset + (64 * 4);
constexpr int kFnb64ParamBytes = kFnb64ResidualZeroPointOffset + (64 * 4);

constexpr int kFnb64Ct2d32PrimaryTileBytes = 18 * 10 * 64;
constexpr int kFnb64Ct2d32SkipTileBytes = 32 * 16 * 32;
constexpr int kFnb64Ct2d32OutputTileBytes = 32 * 16 * 32;
constexpr int kFnb64Ct2d32SpatialWeightsOffset = 16;
constexpr int kFnb64Ct2d32SpatialBiasOffset = kFnb64Ct2d32SpatialWeightsOffset + (9 * 4 * 4 * 64);
constexpr int kFnb64Ct2d32SpatialMultiplierOffset = kFnb64Ct2d32SpatialBiasOffset + (32 * 4);
constexpr int kFnb64Ct2d32ExpandWeightsOffset = kFnb64Ct2d32SpatialMultiplierOffset + (32 * 4);
constexpr int kFnb64Ct2d32ExpandBiasOffset = kFnb64Ct2d32ExpandWeightsOffset + (16 * 8 * 64);
constexpr int kFnb64Ct2d32ExpandMultiplierOffset = kFnb64Ct2d32ExpandBiasOffset + (128 * 4);
constexpr int kFnb64Ct2d32ContractWeightsOffset = kFnb64Ct2d32ExpandMultiplierOffset + (128 * 4);
constexpr int kFnb64Ct2d32ContractBiasOffset = kFnb64Ct2d32ContractWeightsOffset + (8 * 16 * 64);
constexpr int kFnb64Ct2d32ContractMultiplierOffset = kFnb64Ct2d32ContractBiasOffset + (64 * 4);
constexpr int kFnb64Ct2d32ResidualMultiplierOffset = kFnb64Ct2d32ContractMultiplierOffset + (64 * 4);
constexpr int kFnb64Ct2d32ResidualShiftOffset = kFnb64Ct2d32ResidualMultiplierOffset + (64 * 4);
constexpr int kFnb64Ct2d32ResidualZeroPointOffset = kFnb64Ct2d32ResidualShiftOffset + (64 * 4);
constexpr int kFnb64Ct2d32Ct2dWeightsOffset = kFnb64Ct2d32ResidualZeroPointOffset + (64 * 4);
constexpr int kFnb64Ct2d32Ct2dBiasOffset = kFnb64Ct2d32Ct2dWeightsOffset + (4 * 4 * 8 * 64);
constexpr int kFnb64Ct2d32Ct2dMultiplierOffset = kFnb64Ct2d32Ct2dBiasOffset + (32 * 4);
constexpr int kFnb64Ct2d32SkipMultiplierOffset = kFnb64Ct2d32Ct2dMultiplierOffset + (32 * 4);
constexpr int kFnb64Ct2d32SkipShiftOffset = kFnb64Ct2d32SkipMultiplierOffset + (32 * 4);
constexpr int kFnb64Ct2d32SkipZeroPointOffset = kFnb64Ct2d32SkipShiftOffset + (32 * 4);
constexpr int kFnb64Ct2d32ParamBytes = kFnb64Ct2d32SkipZeroPointOffset + (32 * 4);


constexpr int kBridgeQuadrants = 4;
constexpr int kBridgeC8Chunks = 4;
constexpr int kBridgeC8 = 8;
constexpr int kBridgeC16 = 16;
constexpr int kBridgeC32 = 32;
constexpr int kBridgeC64 = 64;
constexpr int kBridgeTileWidth = 16;
constexpr int kBridgeTileHeight = 8;
constexpr int kBridgeFullWidth = 32;
constexpr int kBridgeFullHeight = 16;
constexpr int kBridgeDownsourceWidth = 40;
constexpr int kBridgeDownsourceHeight = 20;
constexpr int kBridgeHaloWidth = 18;
constexpr int kBridgeHaloHeight = 10;
constexpr int kBridgeQuadrantChunkBytes = kBridgeTileWidth * kBridgeTileHeight * kBridgeC8;
constexpr int kBridgeGroupedQuadrantsBytes = kBridgeQuadrants * kBridgeC8Chunks * kBridgeQuadrantChunkBytes;
constexpr int kBridgeGroupedC32FullBytes = kBridgeC8Chunks * kBridgeFullWidth * kBridgeFullHeight * kBridgeC8;
constexpr int kBridgeGroupedC32DownsourceBytes = kBridgeC8Chunks * kBridgeDownsourceWidth * kBridgeDownsourceHeight * kBridgeC8;
constexpr int kBridgeC64QuadrantsBytes = kBridgeQuadrants * kBridgeTileWidth * kBridgeTileHeight * kBridgeC64;
constexpr int kBridgeC64HaloPackageBytes = kBridgeQuadrants * kBridgeHaloWidth * kBridgeHaloHeight * kBridgeC64;
constexpr int kBridgeC32HaloPackageBytes = kBridgeQuadrants * kBridgeHaloWidth * kBridgeHaloHeight * kBridgeC32;
constexpr int kBridgeGroupedC8HaloPackageBytes = kBridgeQuadrants * kBridgeC8Chunks * kBridgeHaloWidth * kBridgeHaloHeight * kBridgeC8;
constexpr int kBridgeC16FullBytes = kBridgeFullWidth * kBridgeFullHeight * kBridgeC16;
constexpr int kBridgeC16HaloPackageBytes = kBridgeQuadrants * kBridgeHaloWidth * kBridgeHaloHeight * kBridgeC16;

[[nodiscard]] int bridge_grouped_quadrant_offset(const int quadrant, const int chunk, const int y, const int x, const int lane)
{
    return (((quadrant * kBridgeC8Chunks + chunk) * kBridgeTileHeight + y) * kBridgeTileWidth + x) * kBridgeC8 + lane;
}

[[nodiscard]] int bridge_grouped_full_offset(const int chunk, const int y, const int x, const int lane)
{
    return ((chunk * kBridgeFullHeight + y) * kBridgeFullWidth + x) * kBridgeC8 + lane;
}

[[nodiscard]] int bridge_grouped_downsource_offset(const int chunk, const int y, const int x, const int lane)
{
    return ((chunk * kBridgeDownsourceHeight + y) * kBridgeDownsourceWidth + x) * kBridgeC8 + lane;
}

[[nodiscard]] int bridge_c64_quadrant_offset(const int quadrant, const int y, const int x, const int channel)
{
    return ((quadrant * kBridgeTileHeight + y) * kBridgeTileWidth + x) * kBridgeC64 + channel;
}

[[nodiscard]] int bridge_c64_halo_offset(const int quadrant, const int y, const int x, const int channel)
{
    return ((quadrant * kBridgeHaloHeight + y) * kBridgeHaloWidth + x) * kBridgeC64 + channel;
}

[[nodiscard]] int bridge_c32_halo_offset(const int quadrant, const int y, const int x, const int channel)
{
    return ((quadrant * kBridgeHaloHeight + y) * kBridgeHaloWidth + x) * kBridgeC32 + channel;
}

[[nodiscard]] int bridge_grouped_c8_halo_offset(const int quadrant, const int chunk, const int y, const int x, const int lane)
{
    return (((quadrant * kBridgeC8Chunks + chunk) * kBridgeHaloHeight + y) * kBridgeHaloWidth + x) * kBridgeC8 + lane;
}

[[nodiscard]] int bridge_c16_full_offset(const int y, const int x, const int channel)
{
    return ((y * kBridgeFullWidth + x) * kBridgeC16) + channel;
}

[[nodiscard]] int bridge_c16_halo_offset(const int quadrant, const int y, const int x, const int channel)
{
    return ((quadrant * kBridgeHaloHeight + y) * kBridgeHaloWidth + x) * kBridgeC16 + channel;
}

constexpr int kCt2d32OchunkInputTileBytes = 16 * 8 * 64;
constexpr int kCt2d32OchunkSkipTileBytes = 32 * 16 * 8;
constexpr int kCt2d32OchunkOutputTileBytes = 32 * 16 * 8;
constexpr int kCt2d32OchunkHaloTileBytes = 18 * 10 * 8;
constexpr int kCt2d32OchunkWeightsOffset = 16;
constexpr int kCt2d32OchunkBiasOffset = kCt2d32OchunkWeightsOffset + (4 * 8 * 64);
constexpr int kCt2d32OchunkMultiplierOffset = kCt2d32OchunkBiasOffset + (8 * 4);
constexpr int kCt2d32OchunkSkipMultiplierOffset = kCt2d32OchunkMultiplierOffset + (8 * 4);
constexpr int kCt2d32OchunkSkipShiftOffset = kCt2d32OchunkSkipMultiplierOffset + (8 * 4);
constexpr int kCt2d32OchunkSkipZeroPointOffset = kCt2d32OchunkSkipShiftOffset + (8 * 4);
constexpr int kCt2d32OchunkParamBytes = kCt2d32OchunkSkipZeroPointOffset + (8 * 4);

std::int32_t* i32_at(std::vector<std::int8_t>& bytes, const int offset)
{
    return reinterpret_cast<std::int32_t*>(bytes.data() + offset);
}

std::int8_t* i8_at(std::vector<std::int8_t>& bytes, const int offset)
{
    return bytes.data() + offset;
}

bool expect_i8(const char* label, const std::int8_t got, const std::int8_t expected)
{
    if (got == expected) {
        return true;
    }
    std::cerr << label << " mismatch: got " << static_cast<int>(got) << " expected " << static_cast<int>(expected) << "\n";
    return false;
}

} // namespace

int main()
{
    std::array<std::int8_t, kInputTileBytes> input{};
    std::array<std::int8_t, kOutputTileBytes> output{};
    std::vector<std::int8_t> params(kParamBytes, 0);

    input[((1 * 18 + 1) * 16) + 0] = 2;

    auto* header = i32_at(params, 0);
    header[0] = 0;
    header[1] = 0;
    header[2] = 0;

    auto* spatial_weights = i8_at(params, kSpatialWeightsOffset);
    spatial_weights[((0 * 9 + 4) * 2 + 0) * 64 + 0] = 1;

    auto* spatial_multiplier = i32_at(params, kSpatialMultiplierOffset);
    for (int channel = 0; channel < 16; ++channel) {
        spatial_multiplier[channel] = 1;
    }

    auto* expand_weights = i8_at(params, kExpandWeightsOffset);
    expand_weights[(0 * 2 + 0) * 64 + 0] = 100;

    auto* expand_multiplier = i32_at(params, kExpandMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        expand_multiplier[channel] = 1;
    }

    auto* contract_weights = i8_at(params, kContractWeightsOffset);
    contract_weights[(0 * 4 + 0) * 64 + 0] = -1;

    auto* contract_multiplier = i32_at(params, kContractMultiplierOffset);
    for (int channel = 0; channel < 16; ++channel) {
        contract_multiplier[channel] = 1;
    }

    apusr_middle_core_convnext16_tile_i8(input.data(), params.data(), output.data());
    if (!expect_i8("ConvNext tile keeps full-range u8 carrier through contraction", output[0], -128)) {
        return 1;
    }

    std::array<std::int8_t, kDownFnbInputTileBytes> down_fnb_input{};
    std::array<std::int8_t, kDownFnbOutputTileBytes> down_fnb_output{};
    std::vector<std::int8_t> down_fnb_params(kDownFnbParamBytes, 0);

    down_fnb_input[((2 * 40 + 2) * 16) + 0] = 2;

    auto* down_fnb_header = i32_at(down_fnb_params, 0);
    down_fnb_header[0] = 0;
    down_fnb_header[1] = 0;
    down_fnb_header[2] = 0;
    down_fnb_header[3] = 0;

    auto* down_fnb_down_weights = i8_at(down_fnb_params, kDownFnbDownWeightsOffset);
    down_fnb_down_weights[((0 * 4 + 0) * 2 + 0) * 64 + 0] = 1;

    auto* down_fnb_down_multiplier = i32_at(down_fnb_params, kDownFnbDownMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        down_fnb_down_multiplier[channel] = 1;
    }

    auto* down_fnb_spatial_weights = i8_at(down_fnb_params, kDownFnbSpatialWeightsOffset);
    down_fnb_spatial_weights[((0 * 9 + 4) * 2 + 0) * 64 + 0] = 1;

    auto* down_fnb_spatial_multiplier = i32_at(down_fnb_params, kDownFnbSpatialMultiplierOffset);
    for (int channel = 0; channel < 16; ++channel) {
        down_fnb_spatial_multiplier[channel] = 1;
    }

    auto* down_fnb_expand_weights = i8_at(down_fnb_params, kDownFnbExpandWeightsOffset);
    down_fnb_expand_weights[(0 * 4 + 0) * 64 + 0] = 100;

    auto* down_fnb_expand_multiplier = i32_at(down_fnb_params, kDownFnbExpandMultiplierOffset);
    for (int channel = 0; channel < 64; ++channel) {
        down_fnb_expand_multiplier[channel] = 1;
    }

    auto* down_fnb_contract_weights = i8_at(down_fnb_params, kDownFnbContractWeightsOffset);
    down_fnb_contract_weights[(0 * 8 + 0) * 64 + 0] = -1;

    auto* down_fnb_contract_multiplier = i32_at(down_fnb_params, kDownFnbContractMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        down_fnb_contract_multiplier[channel] = 1;
    }

    apusr_middle_core_down2x2_fasternet32_tile_i8(down_fnb_input.data(), down_fnb_params.data(), down_fnb_output.data());
    if (!expect_i8("Down2x2+FasterNet32 tile keeps fused local downsample halo and u8 carrier", down_fnb_output[0], -128)) {
        return 1;
    }



    std::array<std::int8_t, kFnb32InputTileBytes> fnb32_input{};
    std::array<std::int8_t, kFnb32OutputTileBytes> fnb32_output{};
    std::vector<std::int8_t> fnb32_params(kFnb32ParamBytes, 0);

    fnb32_input[((1 * 18 + 1) * 32) + 16] = 2;

    auto* fnb32_header = i32_at(fnb32_params, 0);
    fnb32_header[0] = 0;
    fnb32_header[1] = 0;
    fnb32_header[2] = 0;

    auto* fnb32_spatial_multiplier = i32_at(fnb32_params, kFnb32SpatialMultiplierOffset);
    for (int channel = 0; channel < 16; ++channel) {
        fnb32_spatial_multiplier[channel] = 1;
    }

    auto* fnb32_expand_weights = i8_at(fnb32_params, kFnb32ExpandWeightsOffset);
    fnb32_expand_weights[(0 * 4 + 2) * 64 + 0] = 100;

    auto* fnb32_expand_multiplier = i32_at(fnb32_params, kFnb32ExpandMultiplierOffset);
    for (int channel = 0; channel < 64; ++channel) {
        fnb32_expand_multiplier[channel] = 1;
    }

    auto* fnb32_contract_weights = i8_at(fnb32_params, kFnb32ContractWeightsOffset);
    fnb32_contract_weights[(0 * 8 + 0) * 64 + 0] = -1;

    auto* fnb32_contract_multiplier = i32_at(fnb32_params, kFnb32ContractMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        fnb32_contract_multiplier[channel] = 1;
    }

    apusr_middle_core_fasternet32_tile_i8(fnb32_input.data(), fnb32_params.data(), fnb32_output.data());
    if (!expect_i8("FasterNet32 tile keeps non-spatial channels local through u8 carrier contraction", fnb32_output[0], -128)) {
        return 1;
    }

    std::array<std::int8_t, kFnb32C8InputChunkBytes> fnb32_c8_input0{};
    std::array<std::int8_t, kFnb32C8InputChunkBytes> fnb32_c8_input1{};
    std::array<std::int8_t, kFnb32C8InputChunkBytes> fnb32_c8_input2{};
    std::array<std::int8_t, kFnb32C8InputChunkBytes> fnb32_c8_input3{};
    std::array<std::int8_t, kFnb32C8OutputChunkBytes> fnb32_c8_output0{};
    std::array<std::int8_t, kFnb32C8OutputChunkBytes> fnb32_c8_output1{};
    std::array<std::int8_t, kFnb32C8OutputChunkBytes> fnb32_c8_output2{};
    std::array<std::int8_t, kFnb32C8OutputChunkBytes> fnb32_c8_output3{};

    fnb32_c8_input2[((1 * 18 + 1) * 8) + 0] = 2;
    apusr_middle_core_fasternet32_c8group_tile_i8(
        fnb32_c8_input0.data(),
        fnb32_c8_input1.data(),
        fnb32_c8_input2.data(),
        fnb32_c8_input3.data(),
        fnb32_params.data(),
        fnb32_c8_output0.data(),
        fnb32_c8_output1.data(),
        fnb32_c8_output2.data(),
        fnb32_c8_output3.data());
    if (!expect_i8("FasterNet32 C8-grouped handoff consumes stage9 chunks without NHWC repack", fnb32_c8_output0[0], -128)) {
        return 1;
    }


    std::array<std::int8_t, kFnb32Ct2d16PrimaryTileBytes> fnb32_ct2d16_primary{};
    std::array<std::int8_t, kFnb32Ct2d16SkipTileBytes> fnb32_ct2d16_skip{};
    std::array<std::int8_t, kFnb32Ct2d16OutputTileBytes> fnb32_ct2d16_output{};
    std::vector<std::int8_t> fnb32_ct2d16_params(kFnb32Ct2d16ParamBytes, 0);

    fnb32_ct2d16_primary[((1 * 18 + 1) * 32) + 16] = 2;
    fnb32_ct2d16_skip[((0 * 32 + 1) * 16) + 0] = 5;

    auto* fnb32_ct2d16_header = i32_at(fnb32_ct2d16_params, 0);
    fnb32_ct2d16_header[0] = 0;
    fnb32_ct2d16_header[1] = 0;
    fnb32_ct2d16_header[2] = 0;
    fnb32_ct2d16_header[3] = 0;

    auto* fnb32_ct2d16_spatial_multiplier = i32_at(fnb32_ct2d16_params, kFnb32Ct2d16SpatialMultiplierOffset);
    for (int channel = 0; channel < 16; ++channel) {
        fnb32_ct2d16_spatial_multiplier[channel] = 1;
    }

    auto* fnb32_ct2d16_expand_weights = i8_at(fnb32_ct2d16_params, kFnb32Ct2d16ExpandWeightsOffset);
    fnb32_ct2d16_expand_weights[(0 * 4 + 2) * 64 + 0] = 100;

    auto* fnb32_ct2d16_expand_multiplier = i32_at(fnb32_ct2d16_params, kFnb32Ct2d16ExpandMultiplierOffset);
    for (int channel = 0; channel < 64; ++channel) {
        fnb32_ct2d16_expand_multiplier[channel] = 1;
    }

    auto* fnb32_ct2d16_contract_weights = i8_at(fnb32_ct2d16_params, kFnb32Ct2d16ContractWeightsOffset);
    fnb32_ct2d16_contract_weights[(0 * 8 + 0) * 64 + 0] = -1;

    auto* fnb32_ct2d16_contract_multiplier = i32_at(fnb32_ct2d16_params, kFnb32Ct2d16ContractMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        fnb32_ct2d16_contract_multiplier[channel] = 1;
    }

    auto* fnb32_ct2d16_ct2d_weights = i8_at(fnb32_ct2d16_params, kFnb32Ct2d16Ct2dWeightsOffset);
    fnb32_ct2d16_ct2d_weights[((1 * 2 + 0) * 4 + 0) * 64 + 0] = 1;

    auto* fnb32_ct2d16_ct2d_multiplier = i32_at(fnb32_ct2d16_params, kFnb32Ct2d16Ct2dMultiplierOffset);
    for (int channel = 0; channel < 16; ++channel) {
        fnb32_ct2d16_ct2d_multiplier[channel] = 1;
    }

    auto* fnb32_ct2d16_skip_multiplier = i32_at(fnb32_ct2d16_params, kFnb32Ct2d16SkipMultiplierOffset);
    for (int channel = 0; channel < 16; ++channel) {
        fnb32_ct2d16_skip_multiplier[channel] = 1;
    }

    apusr_middle_core_fnb32_ct2d16_add_tile_i8(
        fnb32_ct2d16_primary.data(),
        fnb32_ct2d16_skip.data(),
        fnb32_ct2d16_params.data(),
        fnb32_ct2d16_output.data());
    if (!expect_i8("FasterNet32+CT2D16 skip-add tile fuses low-scale body and high-scale skip join", fnb32_ct2d16_output[1 * 16], -123)) {
        return 1;
    }

    std::array<std::int8_t, kFnb32C8InputChunkBytes> fnb32_ct2d16_c8_input0{};
    std::array<std::int8_t, kFnb32C8InputChunkBytes> fnb32_ct2d16_c8_input1{};
    std::array<std::int8_t, kFnb32C8InputChunkBytes> fnb32_ct2d16_c8_input2{};
    std::array<std::int8_t, kFnb32C8InputChunkBytes> fnb32_ct2d16_c8_input3{};
    std::array<std::int8_t, kFnb32Ct2d16OutputTileBytes> fnb32_ct2d16_c8_output{};
    fnb32_ct2d16_c8_input2[((1 * 18 + 1) * 8) + 0] = 2;
    apusr_middle_core_fnb32_c8group_ct2d16_add_tile_i8(
        fnb32_ct2d16_c8_input0.data(),
        fnb32_ct2d16_c8_input1.data(),
        fnb32_ct2d16_c8_input2.data(),
        fnb32_ct2d16_c8_input3.data(),
        fnb32_ct2d16_skip.data(),
        fnb32_ct2d16_params.data(),
        fnb32_ct2d16_c8_output.data());
    if (!expect_i8(
            "FasterNet32 C8-grouped stage11 skip join consumes stage10 queues without NHWC repack",
            fnb32_ct2d16_c8_output[1 * 16],
            -123)) {
        return 1;
    }


    std::array<std::int8_t, kDownFnb64InputTileBytes> down_fnb64_input{};
    std::array<std::int8_t, kDownFnb64OutputTileBytes> down_fnb64_output{};
    std::vector<std::int8_t> down_fnb64_params(kDownFnb64ParamBytes, 0);

    down_fnb64_input[((2 * 40 + 2) * 32) + 0] = 2;

    auto* down_fnb64_header = i32_at(down_fnb64_params, 0);
    down_fnb64_header[0] = 0;
    down_fnb64_header[1] = 0;
    down_fnb64_header[2] = 0;
    down_fnb64_header[3] = 0;

    auto* down_fnb64_down_weights = i8_at(down_fnb64_params, kDownFnb64DownWeightsOffset);
    down_fnb64_down_weights[((4 * 4 + 0) * 4 + 0) * 64 + 0] = 1;

    auto* down_fnb64_down_multiplier = i32_at(down_fnb64_params, kDownFnb64DownMultiplierOffset);
    for (int channel = 0; channel < 64; ++channel) {
        down_fnb64_down_multiplier[channel] = 1;
    }

    auto* down_fnb64_spatial_multiplier = i32_at(down_fnb64_params, kDownFnb64SpatialMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        down_fnb64_spatial_multiplier[channel] = 1;
    }

    auto* down_fnb64_expand_weights = i8_at(down_fnb64_params, kDownFnb64ExpandWeightsOffset);
    down_fnb64_expand_weights[(0 * 8 + 4) * 64 + 0] = 100;

    auto* down_fnb64_expand_multiplier = i32_at(down_fnb64_params, kDownFnb64ExpandMultiplierOffset);
    for (int channel = 0; channel < 128; ++channel) {
        down_fnb64_expand_multiplier[channel] = 1;
    }

    auto* down_fnb64_contract_weights = i8_at(down_fnb64_params, kDownFnb64ContractWeightsOffset);
    down_fnb64_contract_weights[(0 * 16 + 0) * 64 + 0] = -1;

    auto* down_fnb64_contract_multiplier = i32_at(down_fnb64_params, kDownFnb64ContractMultiplierOffset);
    for (int channel = 0; channel < 64; ++channel) {
        down_fnb64_contract_multiplier[channel] = 1;
    }

    apusr_middle_core_down2x2_fasternet64_tile_i8(down_fnb64_input.data(), down_fnb64_params.data(), down_fnb64_output.data());
    if (!expect_i8("Down2x2+FasterNet64 tile keeps row-ring downsample and u8 carrier local", down_fnb64_output[0], -128)) {
        return 1;
    }

    std::array<std::int8_t, kFnb64InputTileBytes> fnb64_input{};
    std::array<std::int8_t, kFnb64OutputTileBytes> fnb64_output{};
    std::vector<std::int8_t> fnb64_params(kFnb64ParamBytes, 0);

    fnb64_input[((1 * 18 + 1) * 64) + 32] = 2;

    auto* fnb64_header = i32_at(fnb64_params, 0);
    fnb64_header[0] = 0;
    fnb64_header[1] = 0;
    fnb64_header[2] = 0;

    auto* fnb64_spatial_multiplier = i32_at(fnb64_params, kFnb64SpatialMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        fnb64_spatial_multiplier[channel] = 1;
    }

    auto* fnb64_expand_weights = i8_at(fnb64_params, kFnb64ExpandWeightsOffset);
    fnb64_expand_weights[(0 * 8 + 4) * 64 + 0] = 100;

    auto* fnb64_expand_multiplier = i32_at(fnb64_params, kFnb64ExpandMultiplierOffset);
    for (int channel = 0; channel < 128; ++channel) {
        fnb64_expand_multiplier[channel] = 1;
    }

    auto* fnb64_contract_weights = i8_at(fnb64_params, kFnb64ContractWeightsOffset);
    fnb64_contract_weights[(0 * 16 + 0) * 64 + 0] = -1;

    auto* fnb64_contract_multiplier = i32_at(fnb64_params, kFnb64ContractMultiplierOffset);
    for (int channel = 0; channel < 64; ++channel) {
        fnb64_contract_multiplier[channel] = 1;
    }

    apusr_middle_core_fasternet64_tile_i8(fnb64_input.data(), fnb64_params.data(), fnb64_output.data());
    if (!expect_i8("FasterNet64 tile keeps C64 bypass channels local through u8 carrier contraction", fnb64_output[0], -128)) {
        return 1;
    }



    std::array<std::int8_t, kFnb64Ct2d32PrimaryTileBytes> fnb64_ct2d32_primary{};
    std::array<std::int8_t, kFnb64Ct2d32SkipTileBytes> fnb64_ct2d32_skip{};
    std::array<std::int8_t, kFnb64Ct2d32OutputTileBytes> fnb64_ct2d32_output{};
    std::vector<std::int8_t> fnb64_ct2d32_params(kFnb64Ct2d32ParamBytes, 0);

    fnb64_ct2d32_primary[((1 * 18 + 1) * 64) + 32] = 2;
    fnb64_ct2d32_skip[((0 * 32 + 1) * 32) + 0] = 5;

    auto* fnb64_ct2d32_header = i32_at(fnb64_ct2d32_params, 0);
    fnb64_ct2d32_header[0] = 0;
    fnb64_ct2d32_header[1] = 0;
    fnb64_ct2d32_header[2] = 0;
    fnb64_ct2d32_header[3] = 0;

    auto* fnb64_ct2d32_spatial_multiplier = i32_at(fnb64_ct2d32_params, kFnb64Ct2d32SpatialMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        fnb64_ct2d32_spatial_multiplier[channel] = 1;
    }

    auto* fnb64_ct2d32_expand_weights = i8_at(fnb64_ct2d32_params, kFnb64Ct2d32ExpandWeightsOffset);
    fnb64_ct2d32_expand_weights[(0 * 8 + 4) * 64 + 0] = 100;

    auto* fnb64_ct2d32_expand_multiplier = i32_at(fnb64_ct2d32_params, kFnb64Ct2d32ExpandMultiplierOffset);
    for (int channel = 0; channel < 128; ++channel) {
        fnb64_ct2d32_expand_multiplier[channel] = 1;
    }

    auto* fnb64_ct2d32_contract_weights = i8_at(fnb64_ct2d32_params, kFnb64Ct2d32ContractWeightsOffset);
    fnb64_ct2d32_contract_weights[(0 * 16 + 0) * 64 + 0] = -1;

    auto* fnb64_ct2d32_contract_multiplier = i32_at(fnb64_ct2d32_params, kFnb64Ct2d32ContractMultiplierOffset);
    for (int channel = 0; channel < 64; ++channel) {
        fnb64_ct2d32_contract_multiplier[channel] = 1;
    }

    auto* fnb64_ct2d32_ct2d_weights = i8_at(fnb64_ct2d32_params, kFnb64Ct2d32Ct2dWeightsOffset);
    fnb64_ct2d32_ct2d_weights[((1 * 4 + 0) * 8 + 0) * 64 + 0] = 1;

    auto* fnb64_ct2d32_ct2d_multiplier = i32_at(fnb64_ct2d32_params, kFnb64Ct2d32Ct2dMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        fnb64_ct2d32_ct2d_multiplier[channel] = 1;
    }

    auto* fnb64_ct2d32_skip_multiplier = i32_at(fnb64_ct2d32_params, kFnb64Ct2d32SkipMultiplierOffset);
    for (int channel = 0; channel < 32; ++channel) {
        fnb64_ct2d32_skip_multiplier[channel] = 1;
    }

    apusr_middle_core_fnb64_ct2d32_add_tile_i8(
        fnb64_ct2d32_primary.data(),
        fnb64_ct2d32_skip.data(),
        fnb64_ct2d32_params.data(),
        fnb64_ct2d32_output.data());
    if (!expect_i8("FasterNet64+CT2D32 skip-add tile fuses C64 body and C32 skip join", fnb64_ct2d32_output[1 * 32], -123)) {
        return 1;
    }

    std::array<std::int8_t, kCt2d32OchunkInputTileBytes> ct2d32_ochunk_input{};
    std::array<std::int8_t, kCt2d32OchunkSkipTileBytes> ct2d32_ochunk_skip{};
    std::array<std::int8_t, kCt2d32OchunkOutputTileBytes> ct2d32_ochunk_output{};
    std::vector<std::int8_t> ct2d32_ochunk_params(kCt2d32OchunkParamBytes, 0);

    ct2d32_ochunk_input[0] = 2;
    ct2d32_ochunk_skip[0] = 5;

    auto* ct2d32_ochunk_header = i32_at(ct2d32_ochunk_params, 0);
    ct2d32_ochunk_header[0] = 0;

    auto* ct2d32_ochunk_weights = i8_at(ct2d32_ochunk_params, kCt2d32OchunkWeightsOffset);
    ct2d32_ochunk_weights[0] = 1;

    auto* ct2d32_ochunk_multiplier = i32_at(ct2d32_ochunk_params, kCt2d32OchunkMultiplierOffset);
    auto* ct2d32_ochunk_skip_multiplier = i32_at(ct2d32_ochunk_params, kCt2d32OchunkSkipMultiplierOffset);
    for (int channel = 0; channel < 8; ++channel) {
        ct2d32_ochunk_multiplier[channel] = 1;
        ct2d32_ochunk_skip_multiplier[channel] = 1;
    }

    apusr_middle_core_ct2d32_ochunk_add_tile_i8(
        ct2d32_ochunk_input.data(),
        ct2d32_ochunk_skip.data(),
        ct2d32_ochunk_params.data(),
        ct2d32_ochunk_output.data());
    if (!expect_i8("CT2D32 C8 output chunk keeps the stage-9 join fissioned without reduction", ct2d32_ochunk_output[0], 7)) {
        return 1;
    }


    std::array<std::int8_t, kCt2d32OchunkHaloTileBytes> ct2d32_halo_tl{};
    std::array<std::int8_t, kCt2d32OchunkHaloTileBytes> ct2d32_halo_tr{};
    std::array<std::int8_t, kCt2d32OchunkHaloTileBytes> ct2d32_halo_bl{};
    std::array<std::int8_t, kCt2d32OchunkHaloTileBytes> ct2d32_halo_br{};
    apusr_middle_core_ct2d32_ochunk_halo4_add_tile_i8(
        ct2d32_ochunk_input.data(),
        ct2d32_ochunk_skip.data(),
        ct2d32_ochunk_params.data(),
        ct2d32_halo_tl.data(),
        ct2d32_halo_tr.data(),
        ct2d32_halo_bl.data(),
        ct2d32_halo_br.data());
    if (!expect_i8("CT2D32 halo-queue emitter maps top-left stage10 halo", ct2d32_halo_tl[0], 7)) {
        return 1;
    }
    if (!expect_i8("CT2D32 halo-queue emitter maps top-right overlap", ct2d32_halo_tr[0], 0)) {
        return 1;
    }


    std::array<std::int8_t, kBridgeGroupedQuadrantsBytes> stage5_grouped{};
    std::array<std::int8_t, kBridgeGroupedC32FullBytes> skipb_grouped{};
    std::array<std::int8_t, kBridgeGroupedC32DownsourceBytes> downsource_grouped{};
    stage5_grouped[bridge_grouped_quadrant_offset(3, 2, 7, 15, 6)] = 77;
    apusr_middle_core_stage5_c8group_skipb_downsource_tile_i8(stage5_grouped.data(), skipb_grouped.data(), downsource_grouped.data());
    if (!expect_i8("Stage5 bridge stores skipB in C8 group-major full-tile layout", skipb_grouped[bridge_grouped_full_offset(2, 15, 31, 6)], 77)) {
        return 1;
    }
    if (!expect_i8("Stage5 bridge stores downsample source with 2-row/4-column center placement", downsource_grouped[bridge_grouped_downsource_offset(2, 17, 35, 6)], 77)) {
        return 1;
    }
    if (!expect_i8("Stage5 bridge leaves downsource halo padding zero for neighbor fill", downsource_grouped[bridge_grouped_downsource_offset(2, 0, 0, 6)], 0)) {
        return 1;
    }

    std::array<std::int8_t, kBridgeC64QuadrantsBytes> stage67_quadrants{};
    std::array<std::int8_t, kBridgeC64HaloPackageBytes> stage8_halos{};
    stage67_quadrants[bridge_c64_quadrant_offset(3, 7, 15, 41)] = -33;
    apusr_middle_core_stage6_7_c64_halo4_tile_i8(stage67_quadrants.data(), stage8_halos.data());
    if (!expect_i8("Stage6/7 bridge maps C64 bottom-right full pixel into bottom-right stage8 halo", stage8_halos[bridge_c64_halo_offset(3, 9, 17, 41)], -33)) {
        return 1;
    }
    if (!expect_i8("Stage6/7 bridge keeps unrelated C64 halo entries zero", stage8_halos[bridge_c64_halo_offset(0, 0, 0, 41)], 0)) {
        return 1;
    }

    std::array<std::int8_t, kBridgeGroupedQuadrantsBytes> stage10_grouped{};
    std::array<std::int8_t, kBridgeGroupedC8HaloPackageBytes> stage11_grouped_halos{};
    std::array<std::int8_t, kBridgeC32HaloPackageBytes> stage11_halos{};
    stage10_grouped[bridge_grouped_quadrant_offset(3, 3, 7, 15, 4)] = 88;
    apusr_middle_core_stage10_c8group_stage11_halo4_c8group_tile_i8(stage10_grouped.data(), stage11_grouped_halos.data());
    if (!expect_i8(
            "Stage10 bridge preserves C8 grouping into bottom-right stage11 halo",
            stage11_grouped_halos[bridge_grouped_c8_halo_offset(3, 3, 9, 17, 4)],
            88)) {
        return 1;
    }

    apusr_middle_core_stage10_c8group_stage11_halo4_tile_i8(stage10_grouped.data(), stage11_halos.data());
    if (!expect_i8("Legacy Stage10 bridge gathers grouped C8 chunks into C32 stage11 halo", stage11_halos[bridge_c32_halo_offset(3, 9, 17, 28)], 88)) {
        return 1;
    }

    std::array<std::int8_t, kBridgeC16FullBytes> stage11_output{};
    std::array<std::int8_t, kBridgeC16HaloPackageBytes> stage12_halos{};
    stage11_output[bridge_c16_full_offset(15, 31, 9)] = -44;
    apusr_middle_core_stage11_c16_stage12_halo4_tile_i8(stage11_output.data(), stage12_halos.data());
    if (!expect_i8("Stage11 bridge maps C16 bottom-right full pixel into bottom-right stage12 halo", stage12_halos[bridge_c16_halo_offset(3, 9, 17, 9)], -44)) {
        return 1;
    }

    std::cout << "APUSR middle-core stage kernel smoke passed\n";
    return 0;
}
