// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <cstddef>
#include <cstdint>

#if defined(__AIEARCH__) || defined(__AIE_ARCH__)
#include <aie_api/aie.hpp>
#else
#error "APUSR vector primitives are AIE-only. Host tests must use host reference code."
#endif

namespace {

constexpr int kCellWidth = 16;
constexpr int kCellRows = 8;
constexpr int kC16 = 16;
constexpr int kCellBytes = kCellWidth * kCellRows * kC16;
constexpr int kC32 = 32;
constexpr int kMmulM = 4;
constexpr int kMmulK = 8;
constexpr int kMmulN = 8;
constexpr int kElements = 32;
constexpr int kActElements = 32;
constexpr int kWeightElements = 64;
constexpr int kPixelGroups = 32;
constexpr int kCellPixelGroupsPerRow = 4;
constexpr int kInputGroupsC16 = 2;
constexpr int kInputGroupsC32 = 4;
constexpr int kInputGroupsC64 = 8;
constexpr int kOutputChunksC16 = 2;
constexpr int kOutputChunksC32 = 4;
constexpr int kOutputChunksC64 = 8;
constexpr int kHaloPatchWidth = 18;
constexpr int kHaloPatchRows = 10;
constexpr int kStage12FusedSourceRows = 12;
constexpr int kStage12FusedSourceColumns = 20;
constexpr int kStage12FusedSourceBytes = kStage12FusedSourceRows * kStage12FusedSourceColumns * kC16;
constexpr int kStage12Stage1PatchRows = 10;
constexpr int kStage12Stage1PatchColumns = 18;
constexpr int kStage12Stage1PatchGroupsPerRow = 6;
constexpr int kStage12Stage1PatchBytes = kStage12Stage1PatchRows * kStage12Stage1PatchGroupsPerRow * kOutputChunksC16 * kElements;
constexpr int kStage34C16WindowRows = 20;
constexpr int kStage34C16WindowColumns = 40;
constexpr int kStage34C16WindowBytes = kStage34C16WindowRows * kStage34C16WindowColumns * kC16;
constexpr int kStage34C32HaloPatchRows = 10;
constexpr int kStage34C32HaloPatchGroupsPerRow = 5;
constexpr int kStage34C32HaloPatchBytes = kStage34C32HaloPatchRows * kStage34C32HaloPatchGroupsPerRow * kOutputChunksC32 * kElements;

constexpr std::size_t kI32Bytes = sizeof(std::int32_t);
constexpr std::size_t kFrontHeaderBytes = 4 * kI32Bytes;
constexpr std::size_t kFrontSpatialWeightsOffset = kFrontHeaderBytes;
constexpr std::size_t kFrontSpatialWeightBytes = 2304;
constexpr std::size_t kFrontSpatialBiasOffset = kFrontSpatialWeightsOffset + kFrontSpatialWeightBytes;
constexpr std::size_t kFrontSpatialScaleOffset = kFrontSpatialBiasOffset + 64;
constexpr std::size_t kFrontExpandWeightsOffset = kFrontSpatialScaleOffset + 64;
constexpr std::size_t kFrontExpandWeightBytes = 512;
constexpr std::size_t kFrontExpandBiasOffset = kFrontExpandWeightsOffset + kFrontExpandWeightBytes;
constexpr std::size_t kFrontExpandScaleOffset = kFrontExpandBiasOffset + 128;
constexpr std::size_t kFrontContractWeightsOffset = kFrontExpandScaleOffset + 128;
constexpr std::size_t kFrontContractWeightBytes = 512;
constexpr std::size_t kFrontContractBiasOffset = kFrontContractWeightsOffset + kFrontContractWeightBytes;
constexpr std::size_t kFrontContractScaleOffset = kFrontContractBiasOffset + 64;
constexpr std::size_t kFrontResidualScaleOffset = kFrontContractScaleOffset + 64;
constexpr std::size_t kFrontResidualShiftOffset = kFrontResidualScaleOffset + 64;
constexpr std::size_t kFrontStage1CompactRecordBytes = kFrontResidualShiftOffset + 64 + 64;
static_assert(kFrontStage1CompactRecordBytes == 4048);

// Stage-1 hot records are produced by model-epoch loading from the compact
// source arena. They trade resident tile memory for fewer per-cell unpack loops.
constexpr std::size_t kStage1HotHeaderOffset = 0;
constexpr std::size_t kStage1HotSpatialWeightsOffset = 16;
constexpr std::size_t kStage1HotSpatialBiasBlocksOffset = kStage1HotSpatialWeightsOffset + kFrontSpatialWeightBytes;
constexpr std::size_t kStage1HotSpatialScaleBlocksOffset = kStage1HotSpatialBiasBlocksOffset + 256;
constexpr std::size_t kStage1HotExpandWeightsOffset = kStage1HotSpatialScaleBlocksOffset + 256;
constexpr std::size_t kStage1HotExpandBiasBlocksOffset = kStage1HotExpandWeightsOffset + kFrontExpandWeightBytes;
constexpr std::size_t kStage1HotExpandScaleBlocksOffset = kStage1HotExpandBiasBlocksOffset + 512;
constexpr std::size_t kStage1HotContractWeightsOffset = kStage1HotExpandScaleBlocksOffset + 512;
constexpr std::size_t kStage1HotContractBiasBlocksOffset = kStage1HotContractWeightsOffset + kFrontContractWeightBytes;
constexpr std::size_t kStage1HotContractScaleBlocksOffset = kStage1HotContractBiasBlocksOffset + 256;
constexpr std::size_t kStage1HotResidualScaleBlocksOffset = kStage1HotContractScaleBlocksOffset + 256;
constexpr std::size_t kStage1HotReservedOffset = kStage1HotResidualScaleBlocksOffset + 256;
constexpr std::size_t kStage1HotRecordBytes = kStage1HotReservedOffset + 128;
static_assert(kStage1HotRecordBytes == 5776);


constexpr std::size_t kStage34HotHeaderOffset = 0;
constexpr std::size_t kStage34HotDownWeightsOffset = 16;
constexpr std::size_t kStage34HotDownWeightBytes = 2048;
constexpr std::size_t kStage34HotDownBiasBlocksOffset = kStage34HotDownWeightsOffset + kStage34HotDownWeightBytes;
constexpr std::size_t kStage34HotDownScaleBlocksOffset = kStage34HotDownBiasBlocksOffset + 512;
constexpr std::size_t kStage34HotFnbSpatialWeightsOffset = kStage34HotDownScaleBlocksOffset + 512;
constexpr std::size_t kStage34HotFnbSpatialWeightBytes = 2304;
constexpr std::size_t kStage34HotFnbSpatialBiasBlocksOffset = kStage34HotFnbSpatialWeightsOffset + kStage34HotFnbSpatialWeightBytes;
constexpr std::size_t kStage34HotFnbSpatialScaleBlocksOffset = kStage34HotFnbSpatialBiasBlocksOffset + 256;
constexpr std::size_t kStage34HotFnbExpandWeightsOffset = kStage34HotFnbSpatialScaleBlocksOffset + 256;
constexpr std::size_t kStage34HotFnbExpandWeightBytes = 2048;
constexpr std::size_t kStage34HotFnbExpandBiasBlocksOffset = kStage34HotFnbExpandWeightsOffset + kStage34HotFnbExpandWeightBytes;
constexpr std::size_t kStage34HotFnbExpandScaleBlocksOffset = kStage34HotFnbExpandBiasBlocksOffset + 1024;
constexpr std::size_t kStage34HotFnbContractWeightsOffset = kStage34HotFnbExpandScaleBlocksOffset + 1024;
constexpr std::size_t kStage34HotFnbContractWeightBytes = 2048;
constexpr std::size_t kStage34HotFnbContractBiasBlocksOffset = kStage34HotFnbContractWeightsOffset + kStage34HotFnbContractWeightBytes;
constexpr std::size_t kStage34HotFnbContractScaleBlocksOffset = kStage34HotFnbContractBiasBlocksOffset + 512;
constexpr std::size_t kStage34HotFnbResidualScaleBlocksOffset = kStage34HotFnbContractScaleBlocksOffset + 512;
constexpr std::size_t kStage34HotReservedOffset = kStage34HotFnbResidualScaleBlocksOffset + 512;
constexpr std::size_t kStage34HotRecordBytes = kStage34HotReservedOffset + 128;
static_assert(kStage34HotRecordBytes == 13712);

constexpr std::size_t kPrimitiveHeaderBytes = 4 * kI32Bytes;
constexpr std::size_t kC32ToC16WeightsOffset = kPrimitiveHeaderBytes;
constexpr std::size_t kC32ToC16WeightBytes = 512;
constexpr std::size_t kC32ToC16BiasOffset = kC32ToC16WeightsOffset + kC32ToC16WeightBytes;
constexpr std::size_t kC32ToC16ScaleOffset = kC32ToC16BiasOffset + 64;

constexpr std::size_t kC32ToC64WeightsOffset = kPrimitiveHeaderBytes;
constexpr std::size_t kC32ToC64WeightBytes = 2048;
constexpr std::size_t kC32ToC64BiasOffset = kC32ToC64WeightsOffset + kC32ToC64WeightBytes;
constexpr std::size_t kC32ToC64ScaleOffset = kC32ToC64BiasOffset + 256;

constexpr std::size_t kC64ToC32WeightsOffset = kPrimitiveHeaderBytes;
constexpr std::size_t kC64ToC32WeightBytes = 2048;
constexpr std::size_t kC64ToC32BiasOffset = kC64ToC32WeightsOffset + kC64ToC32WeightBytes;
constexpr std::size_t kC64ToC32ScaleOffset = kC64ToC32BiasOffset + 128;

using MmulS8S8 = aie::mmul<kMmulM, kMmulK, kMmulN, int8, int8>;

template <int kCount>
inline void fill_i32_m4x8(const std::int32_t* compact, const int base, std::int32_t* block)
{
    for (int pixel = 0; pixel < kMmulM; ++pixel) {
        for (int lane = 0; lane < kMmulN; ++lane) {
            block[pixel * kMmulN + lane] = compact[base + lane];
        }
    }
}

inline const std::int8_t* row_cell_for_tap(
    const std::int8_t* above_cell,
    const std::int8_t* center_cell,
    const std::int8_t* below_cell,
    const int cell_row,
    const int dy,
    int& source_row)
{
    if (dy == 0) {
        source_row = cell_row;
        return center_cell;
    }
    if (dy < 0) {
        if (cell_row == 0) {
            source_row = kCellRows - 1;
            return above_cell;
        }
        source_row = cell_row - 1;
        return center_cell;
    }
    if (cell_row == kCellRows - 1) {
        source_row = 0;
        return below_cell;
    }
    source_row = cell_row + 1;
    return center_cell;
}

inline const std::int8_t* seam_for_tap(
    const std::int8_t* above_seam,
    const std::int8_t* center_seam,
    const std::int8_t* below_seam,
    const int cell_row,
    const int dy,
    int& source_row)
{
    if (dy == 0) {
        source_row = cell_row;
        return center_seam;
    }
    if (dy < 0) {
        if (cell_row == 0) {
            source_row = kCellRows - 1;
            return above_seam;
        }
        source_row = cell_row - 1;
        return center_seam;
    }
    if (cell_row == kCellRows - 1) {
        source_row = 0;
        return below_seam;
    }
    source_row = cell_row + 1;
    return center_seam;
}

inline const std::int8_t* select_full3x3_cell_for_tap(
    const std::int8_t* above_left_cell,
    const std::int8_t* above_center_cell,
    const std::int8_t* above_right_cell,
    const std::int8_t* center_left_cell,
    const std::int8_t* center_cell,
    const std::int8_t* center_right_cell,
    const std::int8_t* below_left_cell,
    const std::int8_t* below_center_cell,
    const std::int8_t* below_right_cell,
    const int cell_row,
    const int dy,
    const int dx,
    int& source_row,
    int& source_column)
{
    const int horizontal_side = dx < 0 && source_column == 0 ? -1 : (dx > 0 && source_column == kCellWidth - 1 ? 1 : 0);
    if (horizontal_side == 0) {
        source_column += dx;
    } else if (horizontal_side < 0) {
        source_column = kCellWidth - 1;
    } else {
        source_column = 0;
    }

    if (dy < 0 && cell_row == 0) {
        source_row = kCellRows - 1;
        if (horizontal_side < 0) {
            return above_left_cell;
        }
        if (horizontal_side > 0) {
            return above_right_cell;
        }
        return above_center_cell;
    }
    if (dy > 0 && cell_row == kCellRows - 1) {
        source_row = 0;
        if (horizontal_side < 0) {
            return below_left_cell;
        }
        if (horizontal_side > 0) {
            return below_right_cell;
        }
        return below_center_cell;
    }

    source_row = cell_row + dy;
    if (horizontal_side < 0) {
        return center_left_cell;
    }
    if (horizontal_side > 0) {
        return center_right_cell;
    }
    return center_cell;
}

inline void gather_c16_m4x8_full3x3_tap(
    const std::int8_t* above_left_cell,
    const std::int8_t* above_center_cell,
    const std::int8_t* above_right_cell,
    const std::int8_t* center_left_cell,
    const std::int8_t* center_cell,
    const std::int8_t* center_right_cell,
    const std::int8_t* below_left_cell,
    const std::int8_t* below_center_cell,
    const std::int8_t* below_right_cell,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx,
    std::int8_t* block)
{
    const int channel_base = channel_group * kMmulK;
    for (int pixel = 0; pixel < kMmulM; ++pixel) {
        const int pixel_index = pixel_base + pixel;
        const int cell_row = pixel_index >> 4;
        int source_column = pixel_index & (kCellWidth - 1);
        int source_row = cell_row;
        const std::int8_t* source_cell = select_full3x3_cell_for_tap(
            above_left_cell,
            above_center_cell,
            above_right_cell,
            center_left_cell,
            center_cell,
            center_right_cell,
            below_left_cell,
            below_center_cell,
            below_right_cell,
            cell_row,
            dy,
            dx,
            source_row,
            source_column);
        const std::int8_t* src = source_cell + ((source_row << 4) + source_column) * kC16 + channel_base;
        for (int lane = 0; lane < kMmulK; ++lane) {
            block[pixel * kMmulK + lane] = src[lane];
        }
    }
}

template <bool kUseLeftSeam, bool kUseRightSeam>
inline void gather_c16_m4x8_spatial_tap(
    const std::int8_t* above_cell,
    const std::int8_t* center_cell,
    const std::int8_t* below_cell,
    const std::int8_t* left_above_seam,
    const std::int8_t* left_center_seam,
    const std::int8_t* left_below_seam,
    const std::int8_t* right_above_seam,
    const std::int8_t* right_center_seam,
    const std::int8_t* right_below_seam,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx,
    std::int8_t* block)
{
    const int channel_base = channel_group * kMmulK;
    for (int pixel = 0; pixel < kMmulM; ++pixel) {
        const int pixel_index = pixel_base + pixel;
        const int cell_row = pixel_index >> 4;
        const int cell_column = pixel_index & (kCellWidth - 1);
        int source_row = cell_row;
        const std::int8_t* source_cell = row_cell_for_tap(above_cell, center_cell, below_cell, cell_row, dy, source_row);
        const std::int8_t* src = source_cell + ((source_row << 4) + cell_column) * kC16 + channel_base;

        if (dx < 0) {
            if (cell_column != 0) {
                src -= kC16;
            } else if constexpr (kUseLeftSeam) {
                int seam_row = source_row;
                const std::int8_t* seam = seam_for_tap(
                    left_above_seam, left_center_seam, left_below_seam, cell_row, dy, seam_row);
                src = seam + seam_row * kC16 + channel_base;
            }
        } else if (dx > 0) {
            if (cell_column != kCellWidth - 1) {
                src += kC16;
            } else if constexpr (kUseRightSeam) {
                int seam_row = source_row;
                const std::int8_t* seam = seam_for_tap(
                    right_above_seam, right_center_seam, right_below_seam, cell_row, dy, seam_row);
                src = seam + seam_row * kC16 + channel_base;
            }
        }

        for (int lane = 0; lane < kMmulK; ++lane) {
            block[pixel * kMmulK + lane] = src[lane];
        }
    }
}

inline std::size_t m4x8_cell_offset(const int pixel_base, const int channel_chunk, const int channel_chunks)
{
    const int row = pixel_base >> 4;
    const int group_x = (pixel_base & (kCellWidth - 1)) >> 2;
    const int block = row * kCellPixelGroupsPerRow + group_x;
    return (static_cast<std::size_t>(block) * channel_chunks + channel_chunk) * kElements;
}

inline aie::vector<int8, kActElements> load_c16_m4x8(const std::int8_t* input, const int pixel_base, const int channel_group)
{
    return aie::load_v<kActElements>(input + m4x8_cell_offset(pixel_base, channel_group, kInputGroupsC16));
}

inline aie::vector<int32, kElements> load_c16_m4x8_i32(const std::int8_t* input, const int pixel_base, const int channel_group)
{
    const aie::vector<int8, kElements> residual_i8 = load_c16_m4x8(input, pixel_base, channel_group);
    const aie::vector<int16, kElements> residual_i16 = aie::unpack(residual_i8);
    return aie::unpack(residual_i16);
}

inline aie::vector<int8, kActElements> load_c32_m4x8(const std::int8_t* input, const int pixel_base, const int channel_group)
{
    return aie::load_v<kActElements>(input + m4x8_cell_offset(pixel_base, channel_group, kInputGroupsC32));
}

inline void store_c32_m4x8(std::int8_t* output, const int pixel_base, const int output_chunk, const aie::vector<int8, kElements>& block)
{
    aie::store_v(output + m4x8_cell_offset(pixel_base, output_chunk, kOutputChunksC32), block);
}

inline aie::vector<int8, kActElements> load_c64_m4x8(const std::int8_t* input, const int pixel_base, const int channel_group)
{
    return aie::load_v<kActElements>(input + m4x8_cell_offset(pixel_base, channel_group, kInputGroupsC64));
}

inline void store_c64_m4x8(std::int8_t* output, const int pixel_base, const int output_chunk, const aie::vector<int8, kElements>& block)
{
    aie::store_v(output + m4x8_cell_offset(pixel_base, output_chunk, kOutputChunksC64), block);
}

inline void store_c16_m4x8(std::int8_t* output, const int pixel_base, const int output_chunk, const aie::vector<int8, kElements>& block)
{
    aie::store_v(output + m4x8_cell_offset(pixel_base, output_chunk, kOutputChunksC16), block);
}

inline void copy_c16_cell_vector(std::int8_t* output, const std::int8_t* input)
{
    constexpr int kC16CellVectorBlocks = 64;
    static_assert(kC16CellVectorBlocks * kElements == kCellBytes);
    for (int block_index = 0; block_index < kC16CellVectorBlocks; ++block_index) {
        const int offset = block_index * kElements;
        const aie::vector<int8, kElements> block = aie::load_v<kElements>(input + offset);
        aie::store_v(output + offset, block);
    }
}


inline void copy_c16_cell_row_vector(std::int8_t* output, const std::int8_t* input, const int row)
{
    constexpr int kC16CellRowBytes = kCellWidth * kC16;
    constexpr int kC16CellRowVectorBlocks = 8;
    static_assert(kC16CellRowVectorBlocks * kElements == kC16CellRowBytes);
    const std::int8_t* row_input = input + static_cast<std::size_t>(row) * kC16CellRowBytes;
    for (int block_index = 0; block_index < kC16CellRowVectorBlocks; ++block_index) {
        const int offset = block_index * kElements;
        const aie::vector<int8, kElements> block = aie::load_v<kElements>(row_input + offset);
        aie::store_v(output + offset, block);
    }
}

inline void copy_c16_window2x2_to_raster_macroblock_vector(std::int8_t* output, const std::int8_t* window2x2)
{
    constexpr int kC16CellRowBytes = kCellWidth * kC16;
    constexpr int kMacroblockRowBytes = 2 * kC16CellRowBytes;
    const std::int8_t* top_left = window2x2;
    const std::int8_t* top_right = window2x2 + kCellBytes;
    const std::int8_t* bottom_left = window2x2 + 2 * kCellBytes;
    const std::int8_t* bottom_right = window2x2 + 3 * kCellBytes;
    for (int row = 0; row < kCellRows; ++row) {
        std::int8_t* top_output = output + static_cast<std::size_t>(row) * kMacroblockRowBytes;
        copy_c16_cell_row_vector(top_output, top_left, row);
        copy_c16_cell_row_vector(top_output + kC16CellRowBytes, top_right, row);

        std::int8_t* bottom_output = output + static_cast<std::size_t>(row + kCellRows) * kMacroblockRowBytes;
        copy_c16_cell_row_vector(bottom_output, bottom_left, row);
        copy_c16_cell_row_vector(bottom_output + kC16CellRowBytes, bottom_right, row);
    }
}

inline const std::int32_t* i32_at(const std::int8_t* base, const std::size_t offset)
{
    return reinterpret_cast<const std::int32_t*>(base + offset);
}

template <bool kUseLeftSeam, bool kUseRightSeam>
inline void spatial3x3_c16_m4x8(
    const std::int8_t* above_cell,
    const std::int8_t* center_cell,
    const std::int8_t* below_cell,
    const std::int8_t* left_above_seam,
    const std::int8_t* left_center_seam,
    const std::int8_t* left_below_seam,
    const std::int8_t* right_above_seam,
    const std::int8_t* right_center_seam,
    const std::int8_t* right_below_seam,
    const std::int8_t* params,
    const int pixel_base,
    const int output_chunk,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];
    const std::int8_t* weights = params + kFrontSpatialWeightsOffset;
    const std::int32_t* bias = i32_at(params, kFrontSpatialBiasOffset);
    const std::int32_t* scale = i32_at(params, kFrontSpatialScaleOffset);

    alignas(32) std::int8_t act[kActElements];
    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                gather_c16_m4x8_spatial_tap<kUseLeftSeam, kUseRightSeam>(
                    above_cell,
                    center_cell,
                    below_cell,
                    left_above_seam,
                    left_center_seam,
                    left_below_seam,
                    right_above_seam,
                    right_center_seam,
                    right_below_seam,
                    pixel_base,
                    input_group,
                    dy,
                    dx,
                    act);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const int channel_base = output_chunk * kMmulN;
    fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
    fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

inline void spatial3x3_c16_full3x3_m4x8(
    const std::int8_t* above_left_cell,
    const std::int8_t* above_center_cell,
    const std::int8_t* above_right_cell,
    const std::int8_t* center_left_cell,
    const std::int8_t* center_cell,
    const std::int8_t* center_right_cell,
    const std::int8_t* below_left_cell,
    const std::int8_t* below_center_cell,
    const std::int8_t* below_right_cell,
    const std::int8_t* params,
    const int pixel_base,
    const int output_chunk,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];
    const std::int8_t* weights = params + kFrontSpatialWeightsOffset;
    const std::int32_t* bias = i32_at(params, kFrontSpatialBiasOffset);
    const std::int32_t* scale = i32_at(params, kFrontSpatialScaleOffset);

    alignas(32) std::int8_t act[kActElements];
    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                gather_c16_m4x8_full3x3_tap(
                    above_left_cell,
                    above_center_cell,
                    above_right_cell,
                    center_left_cell,
                    center_cell,
                    center_right_cell,
                    below_left_cell,
                    below_center_cell,
                    below_right_cell,
                    pixel_base,
                    input_group,
                    dy,
                    dx,
                    act);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const int channel_base = output_chunk * kMmulN;
    fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
    fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

void c16_spatial3x3_full3x3_expand_srs_to_c32_cell_impl(
    const std::int8_t* above_left_cell_c16,
    const std::int8_t* above_center_cell_c16,
    const std::int8_t* above_right_cell_c16,
    const std::int8_t* center_left_cell_c16,
    const std::int8_t* center_cell_c16,
    const std::int8_t* center_right_cell_c16,
    const std::int8_t* below_left_cell_c16,
    const std::int8_t* below_center_cell_c16,
    const std::int8_t* below_right_cell_c16,
    const std::int8_t* params,
    std::int8_t* output_c32)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const std::int8_t* weights = params + kFrontExpandWeightsOffset;
    const std::int32_t* bias = i32_at(params, kFrontExpandBiasOffset);
    const std::int32_t* scale = i32_at(params, kFrontExpandScaleOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        spatial3x3_c16_full3x3_m4x8(
            above_left_cell_c16,
            above_center_cell_c16,
            above_right_cell_c16,
            center_left_cell_c16,
            center_cell_c16,
            center_right_cell_c16,
            below_left_cell_c16,
            below_center_cell_c16,
            below_right_cell_c16,
            params,
            pixel_base,
            0,
            spatial_block_0);
        spatial3x3_c16_full3x3_m4x8(
            above_left_cell_c16,
            above_center_cell_c16,
            above_right_cell_c16,
            center_left_cell_c16,
            center_cell_c16,
            center_right_cell_c16,
            below_left_cell_c16,
            below_center_cell_c16,
            below_right_cell_c16,
            params,
            pixel_base,
            1,
            spatial_block_1);

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const int channel_base = chunk * kMmulN;
            fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
            fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            store_c32_m4x8(output_c32, pixel_base, chunk, result_v);
        }
    }
}

template <bool kUseLeftSeam = false, bool kUseRightSeam = false>
void c16_spatial3x3_expand_srs_to_c32_cell_impl(
    const std::int8_t* above_cell_c16,
    const std::int8_t* center_cell_c16,
    const std::int8_t* below_cell_c16,
    const std::int8_t* params,
    std::int8_t* output_c32,
    const std::int8_t* left_above_seam = nullptr,
    const std::int8_t* left_center_seam = nullptr,
    const std::int8_t* left_below_seam = nullptr,
    const std::int8_t* right_above_seam = nullptr,
    const std::int8_t* right_center_seam = nullptr,
    const std::int8_t* right_below_seam = nullptr)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const std::int8_t* weights = params + kFrontExpandWeightsOffset;
    const std::int32_t* bias = i32_at(params, kFrontExpandBiasOffset);
    const std::int32_t* scale = i32_at(params, kFrontExpandScaleOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        spatial3x3_c16_m4x8<kUseLeftSeam, kUseRightSeam>(
            above_cell_c16,
            center_cell_c16,
            below_cell_c16,
            left_above_seam,
            left_center_seam,
            left_below_seam,
            right_above_seam,
            right_center_seam,
            right_below_seam,
            params,
            pixel_base,
            0,
            spatial_block_0);
        spatial3x3_c16_m4x8<kUseLeftSeam, kUseRightSeam>(
            above_cell_c16,
            center_cell_c16,
            below_cell_c16,
            left_above_seam,
            left_center_seam,
            left_below_seam,
            right_above_seam,
            right_center_seam,
            right_below_seam,
            params,
            pixel_base,
            1,
            spatial_block_1);

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const int channel_base = chunk * kMmulN;
            fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
            fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            store_c32_m4x8(output_c32, pixel_base, chunk, result_v);
        }
    }
}


inline aie::vector<int8, kActElements> select_c16_m4x8_c8(const aie::vector<int8, 64>& row_window, const int channel_group)
{
    if (channel_group == 0) {
        return aie::filter_even(row_window, 8);
    }
    return aie::filter_odd(row_window, 8);
}

inline aie::vector<int8, 64> load_c16_halo_patch_4px_window(
    const std::int8_t* halo_patch_c16,
    const int patch_row,
    const int patch_column)
{
    int aligned_column = patch_column & ~1;
    int shift_bytes = (patch_column - aligned_column) * kC16;
    if ((patch_column & 1) != 0 && aligned_column + 7 >= kHaloPatchWidth) {
        aligned_column = kHaloPatchWidth - 8;
        shift_bytes = (patch_column - aligned_column) * kC16;
    }

    const std::int8_t* row_base = halo_patch_c16 + (patch_row * kHaloPatchWidth + aligned_column) * kC16;
    const aie::vector<int8, 64> lo = aie::load_v<64>(row_base);
    if (shift_bytes == 0) {
        return lo;
    }

    const aie::vector<int8, 64> hi = aie::load_v<64>(row_base + 64);
    const auto wide = aie::concat(lo, hi);
    const auto shifted = aie::shuffle_down(wide, shift_bytes);
    return shifted.extract<64>(0);
}

inline aie::vector<int8, kActElements> load_c16_m4x8_halo_patch_tap(
    const std::int8_t* halo_patch_c16,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx)
{
    const int cell_row = pixel_base >> 4;
    const int cell_column = pixel_base & (kCellWidth - 1);
    const int patch_row = cell_row + 1 + dy;
    const int patch_column = cell_column + 1 + dx;
    return select_c16_m4x8_c8(load_c16_halo_patch_4px_window(halo_patch_c16, patch_row, patch_column), channel_group);
}

inline aie::vector<int32, kElements> load_c16_m4x8_halo_patch_tap_i32(
    const std::int8_t* halo_patch_c16,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx)
{
    const aie::vector<int8, kElements> residual_i8 = load_c16_m4x8_halo_patch_tap(
        halo_patch_c16,
        pixel_base,
        channel_group,
        dy,
        dx);
    const aie::vector<int16, kElements> residual_i16 = aie::unpack(residual_i8);
    return aie::unpack(residual_i16);
}

inline const std::int8_t* downsample_source_cell(
    const std::int8_t* top_left_cell_c16,
    const std::int8_t* top_right_cell_c16,
    const std::int8_t* bottom_left_cell_c16,
    const std::int8_t* bottom_right_cell_c16,
    const int input_y,
    const int input_x_base,
    int& local_row,
    int& local_x_base)
{
    const bool bottom = input_y >= kCellRows;
    const bool right = input_x_base >= kCellWidth;
    local_row = bottom ? input_y - kCellRows : input_y;
    local_x_base = right ? input_x_base - kCellWidth : input_x_base;
    if (bottom) {
        return right ? bottom_right_cell_c16 : bottom_left_cell_c16;
    }
    return right ? top_right_cell_c16 : top_left_cell_c16;
}

inline aie::vector<int8, 64> load_c16_8px_c8_window(
    const std::int8_t* cell_c16,
    const int local_row,
    const int local_x_base,
    const int channel_group)
{
    const std::int8_t* row_base = cell_c16 + ((local_row * kCellWidth + local_x_base) * kC16);
    const aie::vector<int8, 64> lo = aie::load_v<64>(row_base);
    const aie::vector<int8, 64> hi = aie::load_v<64>(row_base + 64);
    const aie::vector<int8, kElements> lo_c8 = select_c16_m4x8_c8(lo, channel_group);
    const aie::vector<int8, kElements> hi_c8 = select_c16_m4x8_c8(hi, channel_group);
    return aie::concat(lo_c8, hi_c8);
}

inline aie::vector<int8, kActElements> load_c16_downsample_m4x8_tap(
    const std::int8_t* top_left_cell_c16,
    const std::int8_t* top_right_cell_c16,
    const std::int8_t* bottom_left_cell_c16,
    const std::int8_t* bottom_right_cell_c16,
    const int pixel_base,
    const int channel_group,
    const int tap_y,
    const int tap_x)
{
    const int output_row = pixel_base >> 4;
    const int output_column = pixel_base & (kCellWidth - 1);
    const int input_y = output_row * 2 + tap_y;
    const int input_x_base = output_column * 2;
    int local_row = 0;
    int local_x_base = 0;
    const std::int8_t* source_cell = downsample_source_cell(
        top_left_cell_c16,
        top_right_cell_c16,
        bottom_left_cell_c16,
        bottom_right_cell_c16,
        input_y,
        input_x_base,
        local_row,
        local_x_base);
    const aie::vector<int8, 64> eight_pixels_c8 = load_c16_8px_c8_window(source_cell, local_row, local_x_base, channel_group);
    return tap_x == 0 ? aie::filter_even(eight_pixels_c8, 8) : aie::filter_odd(eight_pixels_c8, 8);
}

inline void spatial3x3_c16_halo_patch_m4x8(
    const std::int8_t* halo_patch_c16,
    const std::int8_t* params,
    const int pixel_base,
    const int output_chunk,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];
    const std::int8_t* weights = params + kFrontSpatialWeightsOffset;
    const std::int32_t* bias = i32_at(params, kFrontSpatialBiasOffset);
    const std::int32_t* scale = i32_at(params, kFrontSpatialScaleOffset);

    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_c16_m4x8_halo_patch_tap(
                    halo_patch_c16,
                    pixel_base,
                    input_group,
                    dy,
                    dx);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const int channel_base = output_chunk * kMmulN;
    fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
    fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

inline void spatial3x3_c16_halo_patch_m4x8_with_epilogue_blocks(
    const std::int8_t* halo_patch_c16,
    const std::int8_t* params,
    const std::int8_t* weights,
    const int pixel_base,
    const int output_chunk,
    const std::int32_t* bias_block,
    const std::int32_t* scale_block,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_c16_m4x8_halo_patch_tap(
                    halo_patch_c16,
                    pixel_base,
                    input_group,
                    dy,
                    dx);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

void c16_halo_patch_spatial3x3_expand_srs_to_c32_cell_impl(
    const std::int8_t* halo_patch_c16,
    const std::int8_t* params,
    std::int8_t* output_c32)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const std::int8_t* weights = params + kFrontExpandWeightsOffset;
    const std::int32_t* bias = i32_at(params, kFrontExpandBiasOffset);
    const std::int32_t* scale = i32_at(params, kFrontExpandScaleOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        spatial3x3_c16_halo_patch_m4x8(halo_patch_c16, params, pixel_base, 0, spatial_block_0);
        spatial3x3_c16_halo_patch_m4x8(halo_patch_c16, params, pixel_base, 1, spatial_block_1);

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const int channel_base = chunk * kMmulN;
            fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
            fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            store_c32_m4x8(output_c32, pixel_base, chunk, result_v);
        }
    }
}


void c16_halo_patch_stage1_convnext16_srs_to_c16_cell_impl(
    const std::int8_t* halo_patch_c16,
    const std::int8_t* params,
    std::int8_t* output_c16)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const int contract_shift = header[2];
    const std::int8_t* expand_weights = params + kStage1HotExpandWeightsOffset;
    const std::int32_t* expand_bias_blocks = i32_at(params, kStage1HotExpandBiasBlocksOffset);
    const std::int32_t* expand_scale_blocks = i32_at(params, kStage1HotExpandScaleBlocksOffset);
    const std::int8_t* contract_weights = params + kStage1HotContractWeightsOffset;
    const std::int32_t* contract_bias_blocks = i32_at(params, kStage1HotContractBiasBlocksOffset);
    const std::int32_t* contract_scale_blocks = i32_at(params, kStage1HotContractScaleBlocksOffset);
    const std::int32_t* residual_scale_blocks = i32_at(params, kStage1HotResidualScaleBlocksOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int8_t hidden_blocks[kOutputChunksC32][kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        spatial3x3_c16_halo_patch_m4x8_with_epilogue_blocks(
            halo_patch_c16,
            params,
            params + kStage1HotSpatialWeightsOffset,
            pixel_base,
            0,
            i32_at(params, kStage1HotSpatialBiasBlocksOffset),
            i32_at(params, kStage1HotSpatialScaleBlocksOffset),
            spatial_block_0);
        spatial3x3_c16_halo_patch_m4x8_with_epilogue_blocks(
            halo_patch_c16,
            params,
            params + kStage1HotSpatialWeightsOffset,
            pixel_base,
            1,
            i32_at(params, kStage1HotSpatialBiasBlocksOffset) + kElements,
            i32_at(params, kStage1HotSpatialScaleBlocksOffset) + kElements,
            spatial_block_1);

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(expand_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(expand_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            aie::store_v(hidden_blocks[chunk], result_v);
        }

        for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(hidden_blocks[input_group]);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(contract_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(contract_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto contract_scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int32, kElements> residual_v = load_c16_m4x8_halo_patch_tap_i32(
                halo_patch_c16,
                pixel_base,
                chunk,
                0,
                0);
            const aie::vector<int32, kElements> residual_scale_v = aie::load_v<kElements>(residual_scale_blocks + chunk * kElements);
            const auto residual_scaled_acc = aie::mul<acc64>(residual_v, residual_scale_v);
            const auto summed_acc = aie::add(contract_scaled_acc, residual_scaled_acc);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(summed_acc, contract_shift);
            store_c16_m4x8(output_c16, pixel_base, chunk, result_v);
        }
    }
}

inline int stage1_2_stage1_patch_group_index(const int column_base)
{
    if (column_base == 13) {
        return 4;
    }
    if (column_base >= 14) {
        return 5;
    }
    return column_base >> 2;
}

inline int stage1_2_stage1_patch_group_base(const int group_index)
{
    if (group_index == 4) {
        return 13;
    }
    if (group_index == 5) {
        return 14;
    }
    return group_index * kMmulM;
}

inline std::size_t stage1_2_stage1_patch_block_offset(const int patch_row, const int column_base, const int channel_chunk)
{
    const int group_index = stage1_2_stage1_patch_group_index(column_base);
    return ((static_cast<std::size_t>(patch_row) * kStage12Stage1PatchGroupsPerRow + group_index) * kOutputChunksC16 + channel_chunk) * kElements;
}

inline void store_stage1_2_stage1_patch_m4x8(
    std::int8_t* patch_c16,
    const int patch_row,
    const int column_base,
    const int channel_chunk,
    const aie::vector<int8, kElements>& block)
{
    aie::store_v(patch_c16 + stage1_2_stage1_patch_block_offset(patch_row, column_base, channel_chunk), block);
}

inline aie::vector<int8, kActElements> load_stage1_2_stage1_patch_m4x8(
    const std::int8_t* patch_c16,
    const int patch_row,
    const int column_base,
    const int channel_group)
{
    return aie::load_v<kActElements>(patch_c16 + stage1_2_stage1_patch_block_offset(patch_row, column_base, channel_group));
}

inline aie::vector<int8, 64> load_stage1_2_source_window_4px_c16(
    const std::int8_t* source_window_c16,
    const int source_row,
    const int source_column)
{
    int aligned_column = source_column & ~1;
    int shift_bytes = (source_column - aligned_column) * kC16;
    if ((source_column & 1) != 0 && aligned_column + 7 >= kStage12FusedSourceColumns) {
        aligned_column = kStage12FusedSourceColumns - 8;
        shift_bytes = (source_column - aligned_column) * kC16;
    }

    const std::int8_t* row_base = source_window_c16 + (source_row * kStage12FusedSourceColumns + aligned_column) * kC16;
    const aie::vector<int8, 64> lo = aie::load_v<64>(row_base);
    if (shift_bytes == 0) {
        return lo;
    }
    const aie::vector<int8, 64> hi = aie::load_v<64>(row_base + 64);
    const auto wide = aie::concat(lo, hi);
    const auto shifted = aie::shuffle_down(wide, shift_bytes);
    return shifted.extract<64>(0);
}

inline aie::vector<int8, kActElements> load_stage1_2_source_m4x8_tap(
    const std::int8_t* source_window_c16,
    const int output_row,
    const int output_column_base,
    const int channel_group,
    const int dy,
    const int dx)
{
    const int source_row = output_row + 1 + dy;
    const int source_column = output_column_base + 1 + dx;
    return select_c16_m4x8_c8(load_stage1_2_source_window_4px_c16(source_window_c16, source_row, source_column), channel_group);
}

inline aie::vector<int32, kElements> load_stage1_2_source_m4x8_center_i32(
    const std::int8_t* source_window_c16,
    const int output_row,
    const int output_column_base,
    const int channel_group)
{
    const aie::vector<int8, kElements> residual_i8 = load_stage1_2_source_m4x8_tap(
        source_window_c16,
        output_row,
        output_column_base,
        channel_group,
        0,
        0);
    const aie::vector<int16, kElements> residual_i16 = aie::unpack(residual_i8);
    return aie::unpack(residual_i16);
}

inline void stage1_2_stage1_spatial3x3_m4x8_with_epilogue_blocks(
    const std::int8_t* source_window_c16,
    const std::int8_t* params,
    const std::int8_t* weights,
    const int output_row,
    const int output_column_base,
    const int output_chunk,
    const std::int32_t* bias_block,
    const std::int32_t* scale_block,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_stage1_2_source_m4x8_tap(
                    source_window_c16,
                    output_row,
                    output_column_base,
                    input_group,
                    dy,
                    dx);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

inline void materialize_stage1_2_stage1_patch18x10(
    const std::int8_t* source_window_c16,
    const std::int8_t* params,
    std::int8_t* stage1_patch_c16)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const int contract_shift = header[2];
    const std::int8_t* expand_weights = params + kStage1HotExpandWeightsOffset;
    const std::int32_t* expand_bias_blocks = i32_at(params, kStage1HotExpandBiasBlocksOffset);
    const std::int32_t* expand_scale_blocks = i32_at(params, kStage1HotExpandScaleBlocksOffset);
    const std::int8_t* contract_weights = params + kStage1HotContractWeightsOffset;
    const std::int32_t* contract_bias_blocks = i32_at(params, kStage1HotContractBiasBlocksOffset);
    const std::int32_t* contract_scale_blocks = i32_at(params, kStage1HotContractScaleBlocksOffset);
    const std::int32_t* residual_scale_blocks = i32_at(params, kStage1HotResidualScaleBlocksOffset);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int8_t hidden_blocks[kOutputChunksC32][kElements];

    for (int output_row = 0; output_row < kStage12Stage1PatchRows; ++output_row) {
        for (int output_column_base_index = 0; output_column_base_index < kStage12Stage1PatchGroupsPerRow; ++output_column_base_index) {
            const int output_column_base = stage1_2_stage1_patch_group_base(output_column_base_index);
            stage1_2_stage1_spatial3x3_m4x8_with_epilogue_blocks(
                source_window_c16,
                params,
                params + kStage1HotSpatialWeightsOffset,
                output_row,
                output_column_base,
                0,
                i32_at(params, kStage1HotSpatialBiasBlocksOffset),
                i32_at(params, kStage1HotSpatialScaleBlocksOffset),
                spatial_block_0);
            stage1_2_stage1_spatial3x3_m4x8_with_epilogue_blocks(
                source_window_c16,
                params,
                params + kStage1HotSpatialWeightsOffset,
                output_row,
                output_column_base,
                1,
                i32_at(params, kStage1HotSpatialBiasBlocksOffset) + kElements,
                i32_at(params, kStage1HotSpatialScaleBlocksOffset) + kElements,
                spatial_block_1);

            for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
                MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
                for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                    const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                    const std::size_t weight_offset =
                        ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                    const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                    const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                    accumulator.mac(act_v, weight_v);
                }

                const auto dot_acc = accumulator.to_accum();
                const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(expand_bias_blocks + chunk * kElements);
                const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(expand_scale_blocks + chunk * kElements);
                const auto biased_acc = aie::add(dot_acc, bias_v);
                const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
                const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
                const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
                aie::store_v(hidden_blocks[chunk], result_v);
            }

            for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
                MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
                for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                    const std::size_t weight_offset =
                        ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                    const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(hidden_blocks[input_group]);
                    const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                    accumulator.mac(act_v, weight_v);
                }

                const auto dot_acc = accumulator.to_accum();
                const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(contract_bias_blocks + chunk * kElements);
                const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(contract_scale_blocks + chunk * kElements);
                const auto biased_acc = aie::add(dot_acc, bias_v);
                const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
                const auto contract_scaled_acc = aie::mul<acc64>(biased_v, scale_v);
                const aie::vector<int32, kElements> residual_v = load_stage1_2_source_m4x8_center_i32(
                    source_window_c16,
                    output_row,
                    output_column_base,
                    chunk);
                const aie::vector<int32, kElements> residual_scale_v = aie::load_v<kElements>(residual_scale_blocks + chunk * kElements);
                const auto residual_scaled_acc = aie::mul<acc64>(residual_v, residual_scale_v);
                const auto summed_acc = aie::add(contract_scaled_acc, residual_scaled_acc);
                const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(summed_acc, contract_shift);
                store_stage1_2_stage1_patch_m4x8(stage1_patch_c16, output_row, output_column_base, chunk, result_v);
            }
        }
    }
}

inline aie::vector<int8, kActElements> load_stage1_2_stage1_patch_tap(
    const std::int8_t* stage1_patch_c16,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx)
{
    const int cell_row = pixel_base >> 4;
    const int cell_column = pixel_base & (kCellWidth - 1);
    const int patch_row = cell_row + 1 + dy;
    const int patch_column = cell_column + 1 + dx;
    const int first_base = patch_column == 13 ? 13 : (patch_column >= 14 ? 14 : (patch_column & ~(kMmulM - 1)));
    const aie::vector<int8, kActElements> first = load_stage1_2_stage1_patch_m4x8(
        stage1_patch_c16,
        patch_row,
        first_base,
        channel_group);
    if (patch_column == first_base) {
        return first;
    }
    const int second_base = first_base >= 12 ? 13 : first_base + kMmulM;
    const aie::vector<int8, kActElements> second = load_stage1_2_stage1_patch_m4x8(
        stage1_patch_c16,
        patch_row,
        second_base,
        channel_group);
    const int shift_bytes = (patch_column - first_base) * kMmulK;
    const auto wide = aie::concat(first, second);
    const auto shifted = aie::shuffle_down(wide, shift_bytes);
    return shifted.extract<kActElements>(0);
}

inline aie::vector<int32, kElements> load_stage1_2_stage1_patch_tap_i32(
    const std::int8_t* stage1_patch_c16,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx)
{
    const aie::vector<int8, kElements> residual_i8 = load_stage1_2_stage1_patch_tap(
        stage1_patch_c16,
        pixel_base,
        channel_group,
        dy,
        dx);
    const aie::vector<int16, kElements> residual_i16 = aie::unpack(residual_i8);
    return aie::unpack(residual_i16);
}

inline void stage1_2_stage2_spatial3x3_m4x8_with_epilogue_blocks(
    const std::int8_t* stage1_patch_c16,
    const std::int8_t* params,
    const int pixel_base,
    const int output_chunk,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];
    const std::int8_t* weights = params + kStage1HotSpatialWeightsOffset;
    const std::int32_t* bias_blocks = i32_at(params, kStage1HotSpatialBiasBlocksOffset);
    const std::int32_t* scale_blocks = i32_at(params, kStage1HotSpatialScaleBlocksOffset);

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_stage1_2_stage1_patch_tap(
                    stage1_patch_c16,
                    pixel_base,
                    input_group,
                    dy,
                    dx);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_blocks + output_chunk * kElements);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_blocks + output_chunk * kElements);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

inline void stage1_2_stage2_from_stage1_patch18x10(
    const std::int8_t* stage1_patch_c16,
    const std::int8_t* params,
    std::int8_t* output_c16)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const int contract_shift = header[2];
    const std::int8_t* expand_weights = params + kStage1HotExpandWeightsOffset;
    const std::int32_t* expand_bias_blocks = i32_at(params, kStage1HotExpandBiasBlocksOffset);
    const std::int32_t* expand_scale_blocks = i32_at(params, kStage1HotExpandScaleBlocksOffset);
    const std::int8_t* contract_weights = params + kStage1HotContractWeightsOffset;
    const std::int32_t* contract_bias_blocks = i32_at(params, kStage1HotContractBiasBlocksOffset);
    const std::int32_t* contract_scale_blocks = i32_at(params, kStage1HotContractScaleBlocksOffset);
    const std::int32_t* residual_scale_blocks = i32_at(params, kStage1HotResidualScaleBlocksOffset);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int8_t hidden_blocks[kOutputChunksC32][kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        stage1_2_stage2_spatial3x3_m4x8_with_epilogue_blocks(stage1_patch_c16, params, pixel_base, 0, spatial_block_0);
        stage1_2_stage2_spatial3x3_m4x8_with_epilogue_blocks(stage1_patch_c16, params, pixel_base, 1, spatial_block_1);

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(expand_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(expand_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            aie::store_v(hidden_blocks[chunk], result_v);
        }

        for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(hidden_blocks[input_group]);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(contract_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(contract_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto contract_scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int32, kElements> residual_v = load_stage1_2_stage1_patch_tap_i32(
                stage1_patch_c16,
                pixel_base,
                chunk,
                0,
                0);
            const aie::vector<int32, kElements> residual_scale_v = aie::load_v<kElements>(residual_scale_blocks + chunk * kElements);
            const auto residual_scaled_acc = aie::mul<acc64>(residual_v, residual_scale_v);
            const auto summed_acc = aie::add(contract_scaled_acc, residual_scaled_acc);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(summed_acc, contract_shift);
            store_c16_m4x8(output_c16, pixel_base, chunk, result_v);
        }
    }
}

void c16_window20x12_stage1_stage2_fused_srs_to_c16_cell_impl(
    const std::int8_t* source_window_c16,
    const std::int8_t* stage1_params,
    const std::int8_t* stage2_params,
    std::int8_t* output_c16)
{
    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t stage1_patch_c16[kStage12Stage1PatchBytes];
    materialize_stage1_2_stage1_patch18x10(source_window_c16, stage1_params, stage1_patch_c16);
    stage1_2_stage2_from_stage1_patch18x10(stage1_patch_c16, stage2_params, output_c16);
}


inline const std::int8_t* stage2_window3x3_cell(
    const std::int8_t* window3x3_c16,
    const int vertical_side,
    const int horizontal_side)
{
    return window3x3_c16 + ((vertical_side + 1) * 3 + (horizontal_side + 1)) * kCellBytes;
}

inline const std::int8_t* stage2_window3x3_cell_for_tap(
    const std::int8_t* window3x3_c16,
    const int cell_row,
    const int dy,
    const int horizontal_side,
    int& source_row)
{
    int vertical_side = 0;
    if (dy < 0 && cell_row == 0) {
        vertical_side = -1;
        source_row = kCellRows - 1;
    } else if (dy > 0 && cell_row == kCellRows - 1) {
        vertical_side = 1;
        source_row = 0;
    } else {
        source_row = cell_row + dy;
    }
    return stage2_window3x3_cell(window3x3_c16, vertical_side, horizontal_side);
}

inline aie::vector<int8, kActElements> load_stage2_c16_window3x3_tap(
    const std::int8_t* window3x3_c16,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx)
{
    const int cell_row = pixel_base >> 4;
    const int cell_column = pixel_base & (kCellWidth - 1);
    const int start_column = cell_column + dx;
    int source_row = 0;

    if ((start_column & (kMmulM - 1)) == 0 && start_column >= 0 && start_column <= kCellWidth - kMmulM) {
        const std::int8_t* cell = stage2_window3x3_cell_for_tap(window3x3_c16, cell_row, dy, 0, source_row);
        return load_c16_m4x8(cell, (source_row << 4) + start_column, channel_group);
    }

    int first_horizontal_side = 0;
    int first_group_column = start_column & ~(kMmulM - 1);
    int first_virtual_column = first_group_column;
    if (start_column < 0) {
        first_horizontal_side = -1;
        first_group_column = kCellWidth - kMmulM;
        first_virtual_column = -kMmulM;
    }

    int second_horizontal_side = first_horizontal_side;
    int second_group_column = first_group_column + kMmulM;
    if (first_horizontal_side < 0) {
        second_horizontal_side = 0;
        second_group_column = 0;
    } else if (second_group_column >= kCellWidth) {
        second_horizontal_side = 1;
        second_group_column = 0;
    }

    int first_source_row = 0;
    int second_source_row = 0;
    const std::int8_t* first_cell = stage2_window3x3_cell_for_tap(
        window3x3_c16,
        cell_row,
        dy,
        first_horizontal_side,
        first_source_row);
    const std::int8_t* second_cell = stage2_window3x3_cell_for_tap(
        window3x3_c16,
        cell_row,
        dy,
        second_horizontal_side,
        second_source_row);
    const aie::vector<int8, kActElements> first = load_c16_m4x8(
        first_cell,
        (first_source_row << 4) + first_group_column,
        channel_group);
    const aie::vector<int8, kActElements> second = load_c16_m4x8(
        second_cell,
        (second_source_row << 4) + second_group_column,
        channel_group);
    const int shift_bytes = (start_column - first_virtual_column) * kMmulK;
    const auto wide = aie::concat(first, second);
    const auto shifted = aie::shuffle_down(wide, shift_bytes);
    return shifted.extract<kActElements>(0);
}

inline void stage2_spatial3x3_c16_window3x3_m4x8_with_epilogue_blocks(
    const std::int8_t* window3x3_c16,
    const std::int8_t* params,
    const int pixel_base,
    const int output_chunk,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];
    const std::int8_t* weights = params + kStage1HotSpatialWeightsOffset;
    const std::int32_t* bias_blocks = i32_at(params, kStage1HotSpatialBiasBlocksOffset);
    const std::int32_t* scale_blocks = i32_at(params, kStage1HotSpatialScaleBlocksOffset);

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_stage2_c16_window3x3_tap(
                    window3x3_c16,
                    pixel_base,
                    input_group,
                    dy,
                    dx);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_blocks + output_chunk * kElements);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_blocks + output_chunk * kElements);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}


void c16_window3x3_convnext16_srs_to_c16_cell_impl(
    const std::int8_t* window3x3_c16,
    const std::int8_t* params,
    std::int8_t* output_c16)
{
    const std::int8_t* center_cell_c16 = window3x3_c16 + 4 * kCellBytes;

    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const int contract_shift = header[2];
    const std::int8_t* expand_weights = params + kStage1HotExpandWeightsOffset;
    const std::int32_t* expand_bias_blocks = i32_at(params, kStage1HotExpandBiasBlocksOffset);
    const std::int32_t* expand_scale_blocks = i32_at(params, kStage1HotExpandScaleBlocksOffset);
    const std::int8_t* contract_weights = params + kStage1HotContractWeightsOffset;
    const std::int32_t* contract_bias_blocks = i32_at(params, kStage1HotContractBiasBlocksOffset);
    const std::int32_t* contract_scale_blocks = i32_at(params, kStage1HotContractScaleBlocksOffset);
    const std::int32_t* residual_scale_blocks = i32_at(params, kStage1HotResidualScaleBlocksOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int8_t hidden_blocks[kOutputChunksC32][kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        stage2_spatial3x3_c16_window3x3_m4x8_with_epilogue_blocks(
            window3x3_c16,
            params,
            pixel_base,
            0,
            spatial_block_0);
        stage2_spatial3x3_c16_window3x3_m4x8_with_epilogue_blocks(
            window3x3_c16,
            params,
            pixel_base,
            1,
            spatial_block_1);

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(expand_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(expand_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            aie::store_v(hidden_blocks[chunk], result_v);
        }

        for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(hidden_blocks[input_group]);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(contract_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(contract_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto contract_scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int32, kElements> residual_v = load_c16_m4x8_i32(center_cell_c16, pixel_base, chunk);
            const aie::vector<int32, kElements> residual_scale_v = aie::load_v<kElements>(residual_scale_blocks + chunk * kElements);
            const auto residual_scaled_acc = aie::mul<acc64>(residual_v, residual_scale_v);
            const auto summed_acc = aie::add(contract_scaled_acc, residual_scaled_acc);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(summed_acc, contract_shift);
            store_c16_m4x8(output_c16, pixel_base, chunk, result_v);
        }
    }
}




inline const std::int8_t* stage2_3row_window_cell(
    const std::int8_t* row_window3_c16,
    const int horizontal_side)
{
    return row_window3_c16 + (horizontal_side + 1) * kCellBytes;
}

inline const std::int8_t* stage2_3row_window_cell_for_tap(
    const std::int8_t* top_row_window3_c16,
    const std::int8_t* center_row_window3_c16,
    const std::int8_t* bottom_row_window3_c16,
    const int cell_row,
    const int dy,
    const int horizontal_side,
    int& source_row)
{
    const std::int8_t* row_window = center_row_window3_c16;
    if (dy < 0 && cell_row == 0) {
        row_window = top_row_window3_c16;
        source_row = kCellRows - 1;
    } else if (dy > 0 && cell_row == kCellRows - 1) {
        row_window = bottom_row_window3_c16;
        source_row = 0;
    } else {
        source_row = cell_row + dy;
    }
    return stage2_3row_window_cell(row_window, horizontal_side);
}

inline aie::vector<int8, kActElements> load_stage2_c16_3row_window_tap(
    const std::int8_t* top_row_window3_c16,
    const std::int8_t* center_row_window3_c16,
    const std::int8_t* bottom_row_window3_c16,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx)
{
    const int cell_row = pixel_base >> 4;
    const int cell_column = pixel_base & (kCellWidth - 1);
    const int start_column = cell_column + dx;
    int source_row = 0;

    if ((start_column & (kMmulM - 1)) == 0 && start_column >= 0 && start_column <= kCellWidth - kMmulM) {
        const std::int8_t* cell = stage2_3row_window_cell_for_tap(
            top_row_window3_c16,
            center_row_window3_c16,
            bottom_row_window3_c16,
            cell_row,
            dy,
            0,
            source_row);
        return load_c16_m4x8(cell, (source_row << 4) + start_column, channel_group);
    }

    int first_horizontal_side = 0;
    int first_group_column = start_column & ~(kMmulM - 1);
    int first_virtual_column = first_group_column;
    if (start_column < 0) {
        first_horizontal_side = -1;
        first_group_column = kCellWidth - kMmulM;
        first_virtual_column = -kMmulM;
    }

    int second_horizontal_side = first_horizontal_side;
    int second_group_column = first_group_column + kMmulM;
    if (first_horizontal_side < 0) {
        second_horizontal_side = 0;
        second_group_column = 0;
    } else if (second_group_column >= kCellWidth) {
        second_horizontal_side = 1;
        second_group_column = 0;
    }

    int first_source_row = 0;
    int second_source_row = 0;
    const std::int8_t* first_cell = stage2_3row_window_cell_for_tap(
        top_row_window3_c16,
        center_row_window3_c16,
        bottom_row_window3_c16,
        cell_row,
        dy,
        first_horizontal_side,
        first_source_row);
    const std::int8_t* second_cell = stage2_3row_window_cell_for_tap(
        top_row_window3_c16,
        center_row_window3_c16,
        bottom_row_window3_c16,
        cell_row,
        dy,
        second_horizontal_side,
        second_source_row);
    const aie::vector<int8, kActElements> first = load_c16_m4x8(
        first_cell,
        (first_source_row << 4) + first_group_column,
        channel_group);
    const aie::vector<int8, kActElements> second = load_c16_m4x8(
        second_cell,
        (second_source_row << 4) + second_group_column,
        channel_group);
    const int shift_bytes = (start_column - first_virtual_column) * kMmulK;
    const auto wide = aie::concat(first, second);
    const auto shifted = aie::shuffle_down(wide, shift_bytes);
    return shifted.extract<kActElements>(0);
}

inline void stage2_spatial3x3_c16_3row_window_m4x8_with_epilogue_blocks(
    const std::int8_t* top_row_window3_c16,
    const std::int8_t* center_row_window3_c16,
    const std::int8_t* bottom_row_window3_c16,
    const std::int8_t* params,
    const int pixel_base,
    const int output_chunk,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];
    const std::int8_t* weights = params + kStage1HotSpatialWeightsOffset;
    const std::int32_t* bias_blocks = i32_at(params, kStage1HotSpatialBiasBlocksOffset);
    const std::int32_t* scale_blocks = i32_at(params, kStage1HotSpatialScaleBlocksOffset);

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_stage2_c16_3row_window_tap(
                    top_row_window3_c16,
                    center_row_window3_c16,
                    bottom_row_window3_c16,
                    pixel_base,
                    input_group,
                    dy,
                    dx);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_blocks + output_chunk * kElements);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_blocks + output_chunk * kElements);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

void c16_3row_window_convnext16_srs_to_c16_cell_impl(
    const std::int8_t* top_row_window3_c16,
    const std::int8_t* center_row_window3_c16,
    const std::int8_t* bottom_row_window3_c16,
    const std::int8_t* params,
    std::int8_t* output_c16)
{
    const std::int8_t* center_cell_c16 = center_row_window3_c16 + kCellBytes;

    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const int contract_shift = header[2];
    const std::int8_t* expand_weights = params + kStage1HotExpandWeightsOffset;
    const std::int32_t* expand_bias_blocks = i32_at(params, kStage1HotExpandBiasBlocksOffset);
    const std::int32_t* expand_scale_blocks = i32_at(params, kStage1HotExpandScaleBlocksOffset);
    const std::int8_t* contract_weights = params + kStage1HotContractWeightsOffset;
    const std::int32_t* contract_bias_blocks = i32_at(params, kStage1HotContractBiasBlocksOffset);
    const std::int32_t* contract_scale_blocks = i32_at(params, kStage1HotContractScaleBlocksOffset);
    const std::int32_t* residual_scale_blocks = i32_at(params, kStage1HotResidualScaleBlocksOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int8_t hidden_blocks[kOutputChunksC32][kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        stage2_spatial3x3_c16_3row_window_m4x8_with_epilogue_blocks(
            top_row_window3_c16,
            center_row_window3_c16,
            bottom_row_window3_c16,
            params,
            pixel_base,
            0,
            spatial_block_0);
        stage2_spatial3x3_c16_3row_window_m4x8_with_epilogue_blocks(
            top_row_window3_c16,
            center_row_window3_c16,
            bottom_row_window3_c16,
            params,
            pixel_base,
            1,
            spatial_block_1);

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(expand_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(expand_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            aie::store_v(hidden_blocks[chunk], result_v);
        }

        for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(hidden_blocks[input_group]);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(contract_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(contract_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto contract_scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int32, kElements> residual_v = load_c16_m4x8_i32(center_cell_c16, pixel_base, chunk);
            const aie::vector<int32, kElements> residual_scale_v = aie::load_v<kElements>(residual_scale_blocks + chunk * kElements);
            const auto residual_scaled_acc = aie::mul<acc64>(residual_v, residual_scale_v);
            const auto summed_acc = aie::add(contract_scaled_acc, residual_scaled_acc);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(summed_acc, contract_shift);
            store_c16_m4x8(output_c16, pixel_base, chunk, result_v);
        }
    }
}


void c16_downsample2x2_srs_to_c32_cell_impl(
    const std::int8_t* top_left_cell_c16,
    const std::int8_t* top_right_cell_c16,
    const std::int8_t* bottom_left_cell_c16,
    const std::int8_t* bottom_right_cell_c16,
    const std::int8_t* params,
    std::int8_t* output_c32)
{
    const std::int32_t* header = i32_at(params, 0);
    const int down_shift = header[0];
    const std::int8_t* weights = params + kStage34HotDownWeightsOffset;
    const std::int32_t* bias_blocks = i32_at(params, kStage34HotDownBiasBlocksOffset);
    const std::int32_t* scale_blocks = i32_at(params, kStage34HotDownScaleBlocksOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            int tap = 0;
            for (int tap_y = 0; tap_y < 2; ++tap_y) {
                for (int tap_x = 0; tap_x < 2; ++tap_x) {
                    for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                        const aie::vector<int8, kActElements> act_v = load_c16_downsample_m4x8_tap(
                            top_left_cell_c16,
                            top_right_cell_c16,
                            bottom_left_cell_c16,
                            bottom_right_cell_c16,
                            pixel_base,
                            input_group,
                            tap_y,
                            tap_x);
                        const std::size_t weight_offset =
                            (((static_cast<std::size_t>(tap) * kOutputChunksC32 + chunk) * kInputGroupsC16) + input_group) *
                            kWeightElements;
                        const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                        accumulator.mac(act_v, weight_v);
                    }
                    ++tap;
                }
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, down_shift);
            store_c32_m4x8(output_c32, pixel_base, chunk, result_v);
        }
    }
}



inline aie::vector<int8, 64> load_stage3_4_c16_window_8px_c8(
    const std::int8_t* window_c16,
    const int input_y,
    const int input_x_base,
    const int channel_group)
{
    const std::int8_t* row_base = window_c16 + ((input_y * kStage34C16WindowColumns + input_x_base) * kC16);
    const aie::vector<int8, 64> lo = aie::load_v<64>(row_base);
    const aie::vector<int8, 64> hi = aie::load_v<64>(row_base + 64);
    const aie::vector<int8, kElements> lo_c8 = select_c16_m4x8_c8(lo, channel_group);
    const aie::vector<int8, kElements> hi_c8 = select_c16_m4x8_c8(hi, channel_group);
    return aie::concat(lo_c8, hi_c8);
}

inline aie::vector<int8, kActElements> load_stage3_4_c16_window_downsample_tap(
    const std::int8_t* window_c16,
    const int c32_y,
    const int c32_x_base,
    const int channel_group,
    const int tap_y,
    const int tap_x)
{
    const int input_y = 2 + c32_y * 2 + tap_y;
    const int input_x_base = 2 + c32_x_base * 2;
    const aie::vector<int8, 64> source_pixels = load_stage3_4_c16_window_8px_c8(
        window_c16,
        input_y,
        input_x_base,
        channel_group);
    return tap_x == 0 ? aie::filter_even(source_pixels, 8) : aie::filter_odd(source_pixels, 8);
}

inline std::size_t stage3_4_c32_halo_patch_block_offset(
    const int patch_row,
    const int patch_group,
    const int channel_group)
{
    return ((static_cast<std::size_t>(patch_row) * kStage34C32HaloPatchGroupsPerRow + patch_group) * kOutputChunksC32 + channel_group) * kElements;
}

inline aie::vector<int8, kElements> stage3_4_downsample_window_m4x8(
    const std::int8_t* window_c16,
    const std::int8_t* params,
    const int c32_y,
    const int c32_x_base,
    const int output_chunk)
{
    const std::int32_t* header = i32_at(params, 0);
    const int down_shift = header[0];
    const std::int8_t* weights = params + kStage34HotDownWeightsOffset;
    const std::int32_t* bias_blocks = i32_at(params, kStage34HotDownBiasBlocksOffset);
    const std::int32_t* scale_blocks = i32_at(params, kStage34HotDownScaleBlocksOffset);

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int tap_y = 0; tap_y < 2; ++tap_y) {
        for (int tap_x = 0; tap_x < 2; ++tap_x) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_stage3_4_c16_window_downsample_tap(
                    window_c16,
                    c32_y,
                    c32_x_base,
                    input_group,
                    tap_y,
                    tap_x);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC32 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_blocks + output_chunk * kElements);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_blocks + output_chunk * kElements);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    return aie::to_vector<int8>(scaled_acc, down_shift);
}

inline void materialize_stage3_4_c32_halo_patch(
    const std::int8_t* window_c16,
    const std::int8_t* params,
    std::int8_t* halo_patch_c32)
{
    for (int patch_row = 0; patch_row < kStage34C32HaloPatchRows; ++patch_row) {
        const int c32_y = patch_row - 1;
        for (int patch_group = 0; patch_group < kStage34C32HaloPatchGroupsPerRow; ++patch_group) {
            const int c32_x_base = patch_group * kMmulM - 1;
            for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
                const aie::vector<int8, kElements> block = stage3_4_downsample_window_m4x8(
                    window_c16,
                    params,
                    c32_y,
                    c32_x_base,
                    chunk);
                aie::store_v(halo_patch_c32 + stage3_4_c32_halo_patch_block_offset(patch_row, patch_group, chunk), block);
            }
        }
    }
}

inline aie::vector<int8, kActElements> load_stage3_4_c32_halo_patch_m4x8(
    const std::int8_t* halo_patch_c32,
    const int pixel_base,
    const int dy,
    const int dx,
    const int channel_group)
{
    const int cell_row = pixel_base >> 4;
    const int cell_column = pixel_base & (kCellWidth - 1);
    const int patch_row = cell_row + 1 + dy;
    const int patch_column = cell_column + 1 + dx;
    const int patch_group = patch_column >> 2;
    const int group_offset = patch_column & (kMmulM - 1);
    const std::size_t offset = stage3_4_c32_halo_patch_block_offset(patch_row, patch_group, channel_group);
    const aie::vector<int8, kElements> lo = aie::load_v<kElements>(halo_patch_c32 + offset);
    if (group_offset == 0) {
        return lo;
    }
    const aie::vector<int8, kElements> hi = aie::load_v<kElements>(
        halo_patch_c32 + offset + kOutputChunksC32 * kElements);
    const auto wide = aie::concat(lo, hi);
    const auto shifted = aie::shuffle_down(wide, group_offset * kMmulK);
    return shifted.extract<kElements>(0);
}

inline void stage3_4_fnb32_partial_spatial_m4x8(
    const std::int8_t* halo_patch_c32,
    const std::int8_t* params,
    const int pixel_base,
    const int output_chunk,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[1];
    const std::int8_t* weights = params + kStage34HotFnbSpatialWeightsOffset;
    const std::int32_t* bias_blocks = i32_at(params, kStage34HotFnbSpatialBiasBlocksOffset);
    const std::int32_t* scale_blocks = i32_at(params, kStage34HotFnbSpatialScaleBlocksOffset);

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_stage3_4_c32_halo_patch_m4x8(
                    halo_patch_c32,
                    pixel_base,
                    dy,
                    dx,
                    input_group);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_blocks + output_chunk * kElements);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_blocks + output_chunk * kElements);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

void c16_window20x40_downsample_fnb32_srs_to_c32_cell_impl(
    const std::int8_t* window_c16,
    const std::int8_t* params,
    std::int8_t* output_c32)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[2];
    const int contract_shift = header[3];
    const std::int8_t* expand_weights = params + kStage34HotFnbExpandWeightsOffset;
    const std::int32_t* expand_bias_blocks = i32_at(params, kStage34HotFnbExpandBiasBlocksOffset);
    const std::int32_t* expand_scale_blocks = i32_at(params, kStage34HotFnbExpandScaleBlocksOffset);
    const std::int8_t* contract_weights = params + kStage34HotFnbContractWeightsOffset;
    const std::int32_t* contract_bias_blocks = i32_at(params, kStage34HotFnbContractBiasBlocksOffset);
    const std::int32_t* contract_scale_blocks = i32_at(params, kStage34HotFnbContractScaleBlocksOffset);
    const std::int32_t* residual_scale_blocks = i32_at(params, kStage34HotFnbResidualScaleBlocksOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t halo_patch_c32[kStage34C32HaloPatchBytes];
    alignas(32) std::int8_t spatial_blocks[kOutputChunksC16][kElements];
    alignas(32) std::int8_t hidden_blocks[kOutputChunksC64][kElements];

    materialize_stage3_4_c32_halo_patch(window_c16, params, halo_patch_c32);

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
            stage3_4_fnb32_partial_spatial_m4x8(
                halo_patch_c32,
                params,
                pixel_base,
                chunk,
                spatial_blocks[chunk]);
        }

        for (int chunk = 0; chunk < kOutputChunksC64; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = input_group < kOutputChunksC16
                    ? aie::load_v<kActElements>(spatial_blocks[input_group])
                    : load_stage3_4_c32_halo_patch_m4x8(halo_patch_c32, pixel_base, 0, 0, input_group);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(expand_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(expand_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            aie::store_v(hidden_blocks[chunk], result_v);
        }

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC64; ++input_group) {
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC64) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(hidden_blocks[input_group]);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(contract_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(contract_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto contract_scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kActElements> residual_i8 = load_stage3_4_c32_halo_patch_m4x8(
                halo_patch_c32,
                pixel_base,
                0,
                0,
                chunk);
            const aie::vector<int16, kElements> residual_i16 = aie::unpack(residual_i8);
            const aie::vector<int32, kElements> residual_v = aie::unpack(residual_i16);
            const aie::vector<int32, kElements> residual_scale_v = aie::load_v<kElements>(residual_scale_blocks + chunk * kElements);
            const auto residual_scaled_acc = aie::mul<acc64>(residual_v, residual_scale_v);
            const auto summed_acc = aie::add(contract_scaled_acc, residual_scaled_acc);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(summed_acc, contract_shift);
            store_c32_m4x8(output_c32, pixel_base, chunk, result_v);
        }
    }
}

void c32_expand_srs_to_c64_cell_impl(
    const std::int8_t* input_c32,
    const std::int8_t* params,
    std::int8_t* output_c64)
{
    const std::int32_t* header = i32_at(params, 0);
    const int shift = header[0];
    const std::int8_t* weights = params + kC32ToC64WeightsOffset;
    const std::int32_t* bias = i32_at(params, kC32ToC64BiasOffset);
    const std::int32_t* scale = i32_at(params, kC32ToC64ScaleOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        for (int chunk = 0; chunk < kOutputChunksC64; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_c32_m4x8(input_c32, pixel_base, input_group);
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const int channel_base = chunk * kMmulN;
            fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
            fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, shift);
            store_c64_m4x8(output_c64, pixel_base, chunk, result_v);
        }
    }
}

void c64_contract_srs_to_c32_cell_impl(
    const std::int8_t* input_c64,
    const std::int8_t* params,
    std::int8_t* output_c32)
{
    const std::int32_t* header = i32_at(params, 0);
    const int shift = header[0];
    const std::int8_t* weights = params + kC64ToC32WeightsOffset;
    const std::int32_t* bias = i32_at(params, kC64ToC32BiasOffset);
    const std::int32_t* scale = i32_at(params, kC64ToC32ScaleOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC64; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_c64_m4x8(input_c64, pixel_base, input_group);
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC64) + input_group) * kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const int channel_base = chunk * kMmulN;
            fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
            fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, shift);
            store_c32_m4x8(output_c32, pixel_base, chunk, result_v);
        }
    }
}

void c32_contract_srs_to_c16_cell_impl(
    const std::int8_t* input_c32,
    const std::int8_t* params,
    std::int8_t* output_c16)
{
    const std::int32_t* header = i32_at(params, 0);
    const int shift = header[0];
    const std::int8_t* weights = params + kC32ToC16WeightsOffset;
    const std::int32_t* bias = i32_at(params, kC32ToC16BiasOffset);
    const std::int32_t* scale = i32_at(params, kC32ToC16ScaleOffset);

    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int32_t bias_block[kElements];
    alignas(32) std::int32_t scale_block[kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_c32_m4x8(input_c32, pixel_base, input_group);
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const int channel_base = chunk * kMmulN;
            fill_i32_m4x8<kElements>(bias, channel_base, bias_block);
            fill_i32_m4x8<kElements>(scale, channel_base, scale_block);
            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, shift);
            store_c16_m4x8(output_c16, pixel_base, chunk, result_v);
        }
    }
}


constexpr int kFrontWindowCells = 7;
inline const std::int8_t* window_cell(const std::int8_t* window, const int cell_index)
{
    return window + cell_index * kCellBytes;
}

inline void copy_cell_column_to_seam(const std::int8_t* cell, const int column, std::int8_t* seam)
{
    for (int row = 0; row < kCellRows; ++row) {
        const std::int8_t* src = cell + ((row * kCellWidth + column) * kC16);
        std::int8_t* dst = seam + row * kC16;
        for (int channel = 0; channel < kC16; ++channel) {
            dst[channel] = src[channel];
        }
    }
}

inline void copy_seam(const std::int8_t* src, std::int8_t* dst)
{
    for (int index = 0; index < kCellRows * kC16; ++index) {
        dst[index] = src[index];
    }
}

inline void materialize_window_left_seams(
    const std::int8_t* above_window,
    const std::int8_t* center_window,
    const std::int8_t* below_window,
    const std::int8_t* external_above,
    const std::int8_t* external_center,
    const std::int8_t* external_below,
    const int center_index,
    std::int8_t* left_above,
    std::int8_t* left_center,
    std::int8_t* left_below)
{
    if (center_index > 0) {
        copy_cell_column_to_seam(window_cell(above_window, center_index - 1), kCellWidth - 1, left_above);
        copy_cell_column_to_seam(window_cell(center_window, center_index - 1), kCellWidth - 1, left_center);
        copy_cell_column_to_seam(window_cell(below_window, center_index - 1), kCellWidth - 1, left_below);
        return;
    }
    copy_seam(external_above, left_above);
    copy_seam(external_center, left_center);
    copy_seam(external_below, left_below);
}

inline void materialize_window_right_seams(
    const std::int8_t* above_window,
    const std::int8_t* center_window,
    const std::int8_t* below_window,
    const std::int8_t* external_above,
    const std::int8_t* external_center,
    const std::int8_t* external_below,
    const int center_index,
    std::int8_t* right_above,
    std::int8_t* right_center,
    std::int8_t* right_below)
{
    if (center_index + 1 < kFrontWindowCells) {
        copy_cell_column_to_seam(window_cell(above_window, center_index + 1), 0, right_above);
        copy_cell_column_to_seam(window_cell(center_window, center_index + 1), 0, right_center);
        copy_cell_column_to_seam(window_cell(below_window, center_index + 1), 0, right_below);
        return;
    }
    copy_seam(external_above, right_above);
    copy_seam(external_center, right_center);
    copy_seam(external_below, right_below);
}


template <int kRowCells>
inline const std::int8_t* row_cell(const std::int8_t* row, const int cell_index)
{
    return row + cell_index * kCellBytes;
}

template <int kRowCells>
inline void materialize_row_left_seams(
    const std::int8_t* above_row,
    const std::int8_t* center_row,
    const std::int8_t* below_row,
    const std::int8_t* external_above,
    const std::int8_t* external_center,
    const std::int8_t* external_below,
    const int center_index,
    std::int8_t* left_above,
    std::int8_t* left_center,
    std::int8_t* left_below)
{
    if (center_index > 0) {
        copy_cell_column_to_seam(row_cell<kRowCells>(above_row, center_index - 1), kCellWidth - 1, left_above);
        copy_cell_column_to_seam(row_cell<kRowCells>(center_row, center_index - 1), kCellWidth - 1, left_center);
        copy_cell_column_to_seam(row_cell<kRowCells>(below_row, center_index - 1), kCellWidth - 1, left_below);
        return;
    }
    copy_seam(external_above, left_above);
    copy_seam(external_center, left_center);
    copy_seam(external_below, left_below);
}

template <int kRowCells>
inline void materialize_row_right_seams(
    const std::int8_t* above_row,
    const std::int8_t* center_row,
    const std::int8_t* below_row,
    const std::int8_t* external_above,
    const std::int8_t* external_center,
    const std::int8_t* external_below,
    const int center_index,
    std::int8_t* right_above,
    std::int8_t* right_center,
    std::int8_t* right_below)
{
    if (center_index + 1 < kRowCells) {
        copy_cell_column_to_seam(row_cell<kRowCells>(above_row, center_index + 1), 0, right_above);
        copy_cell_column_to_seam(row_cell<kRowCells>(center_row, center_index + 1), 0, right_center);
        copy_cell_column_to_seam(row_cell<kRowCells>(below_row, center_index + 1), 0, right_below);
        return;
    }
    copy_seam(external_above, right_above);
    copy_seam(external_center, right_center);
    copy_seam(external_below, right_below);
}

template <int kRowCells>
void c16_row_spatial3x3_expand_srs_to_c32_cell_impl(
    const std::int8_t* above_row_c16,
    const std::int8_t* center_row_c16,
    const std::int8_t* below_row_c16,
    const std::int8_t* params,
    const std::int8_t* left_above_seam_c16,
    const std::int8_t* left_center_seam_c16,
    const std::int8_t* left_below_seam_c16,
    const std::int8_t* right_above_seam_c16,
    const std::int8_t* right_center_seam_c16,
    const std::int8_t* right_below_seam_c16,
    std::int8_t* output_c32,
    const int center_index)
{
    alignas(32) std::int8_t left_above[kCellRows * kC16];
    alignas(32) std::int8_t left_center[kCellRows * kC16];
    alignas(32) std::int8_t left_below[kCellRows * kC16];
    alignas(32) std::int8_t right_above[kCellRows * kC16];
    alignas(32) std::int8_t right_center[kCellRows * kC16];
    alignas(32) std::int8_t right_below[kCellRows * kC16];

    materialize_row_left_seams<kRowCells>(
        above_row_c16,
        center_row_c16,
        below_row_c16,
        left_above_seam_c16,
        left_center_seam_c16,
        left_below_seam_c16,
        center_index,
        left_above,
        left_center,
        left_below);
    materialize_row_right_seams<kRowCells>(
        above_row_c16,
        center_row_c16,
        below_row_c16,
        right_above_seam_c16,
        right_center_seam_c16,
        right_below_seam_c16,
        center_index,
        right_above,
        right_center,
        right_below);

    c16_spatial3x3_expand_srs_to_c32_cell_impl<true, true>(
        row_cell<kRowCells>(above_row_c16, center_index),
        row_cell<kRowCells>(center_row_c16, center_index),
        row_cell<kRowCells>(below_row_c16, center_index),
        params,
        output_c32,
        left_above,
        left_center,
        left_below,
        right_above,
        right_center,
        right_below);
}

void c16_window7_spatial3x3_expand_srs_to_c32_cell_impl(
    const std::int8_t* above_window_c16,
    const std::int8_t* center_window_c16,
    const std::int8_t* below_window_c16,
    const std::int8_t* params,
    const std::int8_t* left_above_seam_c16,
    const std::int8_t* left_center_seam_c16,
    const std::int8_t* left_below_seam_c16,
    const std::int8_t* right_above_seam_c16,
    const std::int8_t* right_center_seam_c16,
    const std::int8_t* right_below_seam_c16,
    std::int8_t* output_c32,
    const int center_index)
{
    alignas(32) std::int8_t left_above[kCellRows * kC16];
    alignas(32) std::int8_t left_center[kCellRows * kC16];
    alignas(32) std::int8_t left_below[kCellRows * kC16];
    alignas(32) std::int8_t right_above[kCellRows * kC16];
    alignas(32) std::int8_t right_center[kCellRows * kC16];
    alignas(32) std::int8_t right_below[kCellRows * kC16];

    materialize_window_left_seams(
        above_window_c16,
        center_window_c16,
        below_window_c16,
        left_above_seam_c16,
        left_center_seam_c16,
        left_below_seam_c16,
        center_index,
        left_above,
        left_center,
        left_below);
    materialize_window_right_seams(
        above_window_c16,
        center_window_c16,
        below_window_c16,
        right_above_seam_c16,
        right_center_seam_c16,
        right_below_seam_c16,
        center_index,
        right_above,
        right_center,
        right_below);

    c16_spatial3x3_expand_srs_to_c32_cell_impl<true, true>(
        window_cell(above_window_c16, center_index),
        window_cell(center_window_c16, center_index),
        window_cell(below_window_c16, center_index),
        params,
        output_c32,
        left_above,
        left_center,
        left_below,
        right_above,
        right_center,
        right_below);
}


} // namespace



extern "C" void apusr_middle_core_stage3_downsample_c16_to_c32_cell_i8(
    const signed char* top_left_cell_c16,
    const signed char* top_right_cell_c16,
    const signed char* bottom_left_cell_c16,
    const signed char* bottom_right_cell_c16,
    const signed char* stage3_4_params,
    signed char* output_cell_c32)
{
    c16_downsample2x2_srs_to_c32_cell_impl(
        reinterpret_cast<const std::int8_t*>(top_left_cell_c16),
        reinterpret_cast<const std::int8_t*>(top_right_cell_c16),
        reinterpret_cast<const std::int8_t*>(bottom_left_cell_c16),
        reinterpret_cast<const std::int8_t*>(bottom_right_cell_c16),
        reinterpret_cast<const std::int8_t*>(stage3_4_params),
        reinterpret_cast<std::int8_t*>(output_cell_c32));
}

extern "C" void apusr_middle_core_stage3_downsample_window_to_c32_cell_i8(
    const signed char* input_window_c16_2x2,
    const signed char* stage3_4_params,
    signed char* output_cell_c32)
{
    const std::int8_t* base = reinterpret_cast<const std::int8_t*>(input_window_c16_2x2);
    c16_downsample2x2_srs_to_c32_cell_impl(
        base,
        base + kCellBytes,
        base + 2 * kCellBytes,
        base + 3 * kCellBytes,
        reinterpret_cast<const std::int8_t*>(stage3_4_params),
        reinterpret_cast<std::int8_t*>(output_cell_c32));
}


extern "C" void apusr_middle_core_stage3_4_fused_window20x40_to_c32_cell_i8(
    const signed char* input_window_c16_20x40,
    const signed char* stage3_4_params,
    signed char* output_cell_c32)
{
    c16_window20x40_downsample_fnb32_srs_to_c32_cell_impl(
        reinterpret_cast<const std::int8_t*>(input_window_c16_20x40),
        reinterpret_cast<const std::int8_t*>(stage3_4_params),
        reinterpret_cast<std::int8_t*>(output_cell_c32));
}




inline const std::int8_t* stage2_row_ring_cell(const std::int8_t* row_base, const int cell_column)
{
    return row_base + static_cast<std::size_t>(cell_column) * kCellBytes;
}

inline void copy_c16_pixel_from_blocked_cell(
    const std::int8_t* cell,
    const int pixel_row,
    const int pixel_column,
    std::int8_t* output_pixel)
{
    const int group_x = pixel_column >> 2;
    const int lane_x = pixel_column & 3;
    const std::size_t block_base =
        (static_cast<std::size_t>(pixel_row) * kCellPixelGroupsPerRow + group_x) * kOutputChunksC16 * kElements;

    const aie::vector<int8, kElements> lo_c8 = aie::load_v<kElements>(cell + block_base);
    const aie::vector<int8, kElements> hi_c8 = aie::load_v<kElements>(cell + block_base + kElements);
    const auto [zipped_lo, zipped_hi] = aie::interleave_zip(lo_c8, hi_c8, kMmulK);
    if (lane_x < 2) {
        const aie::vector<int8, 16> pixel = zipped_lo.template extract<16>(lane_x);
        aie::store_v(output_pixel, pixel);
    } else {
        const aie::vector<int8, 16> pixel = zipped_hi.template extract<16>(lane_x - 2);
        aie::store_v(output_pixel, pixel);
    }
}

inline void materialize_stage3_4_c16_window_from_stage2_rows(
    const std::int8_t* row0,
    const std::int8_t* row1,
    const std::int8_t* row2,
    const std::int8_t* row3,
    const int output_cell_column,
    std::int8_t* window_c16)
{
    const std::int8_t* rows[4] = {row0, row1, row2, row3};
    constexpr int kStage34WindowStartPixelInCell = kCellWidth - 2;
    const int first_pixel_x = output_cell_column * 2 * kCellWidth + kStage34WindowStartPixelInCell;

    for (int y = 0; y < kStage34C16WindowRows; ++y) {
        const int source_y = kStage34WindowStartPixelInCell + y;
        const int source_cell_row = source_y >> 3;
        const int local_y = source_y & (kCellRows - 1);
        const std::int8_t* source_row = rows[source_cell_row];
        for (int x = 0; x < kStage34C16WindowColumns; ++x) {
            const int source_x = first_pixel_x + x;
            const int source_cell_column = source_x >> 4;
            const int local_x = source_x & (kCellWidth - 1);
            const std::int8_t* source_cell = stage2_row_ring_cell(source_row, source_cell_column);
            copy_c16_pixel_from_blocked_cell(
                source_cell,
                local_y,
                local_x,
                window_c16 + (static_cast<std::size_t>(y) * kStage34C16WindowColumns + x) * kC16);
        }
    }
}



constexpr int kStage1_2MacroblockSourceColumns = 36;
constexpr int kStage1_2MacroblockSourceRows = 20;

constexpr int kStage1_2MacroStage1PatchRows = 18;
constexpr int kStage1_2MacroStage1PatchGroupsPerRow = 10;
constexpr int kStage1_2MacroStage1PatchBytes =
    kStage1_2MacroStage1PatchRows * kStage1_2MacroStage1PatchGroupsPerRow * kOutputChunksC16 * kElements;

inline int stage1_2_macro_patch_group_base(const int group_index)
{
    if (group_index == 8) {
        return 29;
    }
    if (group_index == 9) {
        return 30;
    }
    return group_index * kMmulM;
}

inline int stage1_2_macro_patch_group_index_for_base(const int column_base)
{
    if (column_base == 29) {
        return 8;
    }
    if (column_base == 30) {
        return 9;
    }
    return column_base >> 2;
}

inline int stage1_2_macro_patch_base_for_tap(const int patch_column)
{
    if (patch_column == 29 || patch_column == 30) {
        return patch_column;
    }
    return patch_column & ~(kMmulM - 1);
}

inline std::size_t stage1_2_macro_patch_block_offset(
    const int patch_row,
    const int column_base,
    const int channel_chunk)
{
    const int group_index = stage1_2_macro_patch_group_index_for_base(column_base);
    return ((static_cast<std::size_t>(patch_row) * kStage1_2MacroStage1PatchGroupsPerRow + group_index) *
                kOutputChunksC16 +
            channel_chunk) *
        kElements;
}

inline void store_stage1_2_macro_patch_m4x8(
    std::int8_t* patch_c16,
    const int patch_row,
    const int column_base,
    const int channel_chunk,
    const aie::vector<int8, kElements>& block)
{
    aie::store_v(patch_c16 + stage1_2_macro_patch_block_offset(patch_row, column_base, channel_chunk), block);
}

inline aie::vector<int8, kActElements> load_stage1_2_macro_patch_m4x8(
    const std::int8_t* patch_c16,
    const int patch_row,
    const int column_base,
    const int channel_group)
{
    return aie::load_v<kActElements>(patch_c16 + stage1_2_macro_patch_block_offset(patch_row, column_base, channel_group));
}

inline aie::vector<int8, 64> load_stage1_2_macro_source_4px_c16(
    const std::int8_t* source_macroblock_c16,
    const int source_row,
    const int source_column)
{
    int aligned_column = source_column & ~1;
    int shift_bytes = (source_column - aligned_column) * kC16;
    if ((source_column & 1) != 0 && aligned_column + 7 >= kStage1_2MacroblockSourceColumns) {
        aligned_column = kStage1_2MacroblockSourceColumns - 8;
        shift_bytes = (source_column - aligned_column) * kC16;
    }

    const std::int8_t* row_base = source_macroblock_c16 +
        (static_cast<std::size_t>(source_row) * kStage1_2MacroblockSourceColumns + aligned_column) * kC16;
    const aie::vector<int8, 64> lo = aie::load_v<64>(row_base);
    if (shift_bytes == 0) {
        return lo;
    }
    const aie::vector<int8, 64> hi = aie::load_v<64>(row_base + 64);
    const auto wide = aie::concat(lo, hi);
    const auto shifted = aie::shuffle_down(wide, shift_bytes);
    return shifted.extract<64>(0);
}

inline aie::vector<int8, kActElements> load_stage1_2_macro_source_m4x8_tap(
    const std::int8_t* source_macroblock_c16,
    const int output_row,
    const int output_column_base,
    const int channel_group,
    const int dy,
    const int dx)
{
    const int source_row = output_row + 1 + dy;
    const int source_column = output_column_base + 1 + dx;
    return select_c16_m4x8_c8(
        load_stage1_2_macro_source_4px_c16(source_macroblock_c16, source_row, source_column),
        channel_group);
}

inline aie::vector<int32, kElements> load_stage1_2_macro_source_m4x8_center_i32(
    const std::int8_t* source_macroblock_c16,
    const int output_row,
    const int output_column_base,
    const int channel_group)
{
    const aie::vector<int8, kElements> residual_i8 = load_stage1_2_macro_source_m4x8_tap(
        source_macroblock_c16,
        output_row,
        output_column_base,
        channel_group,
        0,
        0);
    const aie::vector<int16, kElements> residual_i16 = aie::unpack(residual_i8);
    return aie::unpack(residual_i16);
}

inline void stage1_2_macro_stage1_spatial3x3_m4x8_with_epilogue_blocks(
    const std::int8_t* source_macroblock_c16,
    const std::int8_t* params,
    const std::int8_t* weights,
    const int output_row,
    const int output_column_base,
    const int output_chunk,
    const std::int32_t* bias_block,
    const std::int32_t* scale_block,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_stage1_2_macro_source_m4x8_tap(
                    source_macroblock_c16,
                    output_row,
                    output_column_base,
                    input_group,
                    dy,
                    dx);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_block);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_block);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

inline void materialize_stage1_2_macro_stage1_patch34x18(
    const std::int8_t* source_macroblock_c16,
    const std::int8_t* params,
    std::int8_t* stage1_patch_c16)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const int contract_shift = header[2];
    const std::int8_t* expand_weights = params + kStage1HotExpandWeightsOffset;
    const std::int32_t* expand_bias_blocks = i32_at(params, kStage1HotExpandBiasBlocksOffset);
    const std::int32_t* expand_scale_blocks = i32_at(params, kStage1HotExpandScaleBlocksOffset);
    const std::int8_t* contract_weights = params + kStage1HotContractWeightsOffset;
    const std::int32_t* contract_bias_blocks = i32_at(params, kStage1HotContractBiasBlocksOffset);
    const std::int32_t* contract_scale_blocks = i32_at(params, kStage1HotContractScaleBlocksOffset);
    const std::int32_t* residual_scale_blocks = i32_at(params, kStage1HotResidualScaleBlocksOffset);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int8_t hidden_blocks[kOutputChunksC32][kElements];

    for (int output_row = 0; output_row < kStage1_2MacroStage1PatchRows; ++output_row) {
        for (int output_column_base_index = 0; output_column_base_index < kStage1_2MacroStage1PatchGroupsPerRow; ++output_column_base_index) {
            const int output_column_base = stage1_2_macro_patch_group_base(output_column_base_index);
            stage1_2_macro_stage1_spatial3x3_m4x8_with_epilogue_blocks(
                source_macroblock_c16,
                params,
                params + kStage1HotSpatialWeightsOffset,
                output_row,
                output_column_base,
                0,
                i32_at(params, kStage1HotSpatialBiasBlocksOffset),
                i32_at(params, kStage1HotSpatialScaleBlocksOffset),
                spatial_block_0);
            stage1_2_macro_stage1_spatial3x3_m4x8_with_epilogue_blocks(
                source_macroblock_c16,
                params,
                params + kStage1HotSpatialWeightsOffset,
                output_row,
                output_column_base,
                1,
                i32_at(params, kStage1HotSpatialBiasBlocksOffset) + kElements,
                i32_at(params, kStage1HotSpatialScaleBlocksOffset) + kElements,
                spatial_block_1);

            for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
                MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
                for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                    const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                    const std::size_t weight_offset =
                        ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                    const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                    const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                    accumulator.mac(act_v, weight_v);
                }

                const auto dot_acc = accumulator.to_accum();
                const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(expand_bias_blocks + chunk * kElements);
                const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(expand_scale_blocks + chunk * kElements);
                const auto biased_acc = aie::add(dot_acc, bias_v);
                const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
                const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
                const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
                aie::store_v(hidden_blocks[chunk], result_v);
            }

            for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
                MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
                for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                    const std::size_t weight_offset =
                        ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                    const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(hidden_blocks[input_group]);
                    const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                    accumulator.mac(act_v, weight_v);
                }

                const auto dot_acc = accumulator.to_accum();
                const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(contract_bias_blocks + chunk * kElements);
                const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(contract_scale_blocks + chunk * kElements);
                const auto biased_acc = aie::add(dot_acc, bias_v);
                const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
                const auto contract_scaled_acc = aie::mul<acc64>(biased_v, scale_v);
                const aie::vector<int32, kElements> residual_v = load_stage1_2_macro_source_m4x8_center_i32(
                    source_macroblock_c16,
                    output_row,
                    output_column_base,
                    chunk);
                const aie::vector<int32, kElements> residual_scale_v = aie::load_v<kElements>(residual_scale_blocks + chunk * kElements);
                const auto residual_scaled_acc = aie::mul<acc64>(residual_v, residual_scale_v);
                const auto summed_acc = aie::add(contract_scaled_acc, residual_scaled_acc);
                const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(summed_acc, contract_shift);
                store_stage1_2_macro_patch_m4x8(stage1_patch_c16, output_row, output_column_base, chunk, result_v);
            }
        }
    }
}

inline aie::vector<int8, kActElements> load_stage1_2_macro_stage1_patch_tap(
    const std::int8_t* stage1_patch_c16,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx,
    const int cell_x,
    const int cell_y)
{
    const int cell_row = pixel_base >> 4;
    const int cell_column = pixel_base & (kCellWidth - 1);
    const int patch_row = cell_y + cell_row + 1 + dy;
    const int patch_column = cell_x + cell_column + 1 + dx;
    const int first_base = stage1_2_macro_patch_base_for_tap(patch_column);
    const aie::vector<int8, kActElements> first = load_stage1_2_macro_patch_m4x8(
        stage1_patch_c16,
        patch_row,
        first_base,
        channel_group);
    if (patch_column == first_base) {
        return first;
    }
    const int second_base = first_base + kMmulM;
    const aie::vector<int8, kActElements> second = load_stage1_2_macro_patch_m4x8(
        stage1_patch_c16,
        patch_row,
        second_base,
        channel_group);
    const int shift_bytes = (patch_column - first_base) * kMmulK;
    const auto wide = aie::concat(first, second);
    const auto shifted = aie::shuffle_down(wide, shift_bytes);
    return shifted.extract<kActElements>(0);
}

inline aie::vector<int32, kElements> load_stage1_2_macro_stage1_patch_tap_i32(
    const std::int8_t* stage1_patch_c16,
    const int pixel_base,
    const int channel_group,
    const int dy,
    const int dx,
    const int cell_x,
    const int cell_y)
{
    const aie::vector<int8, kElements> residual_i8 = load_stage1_2_macro_stage1_patch_tap(
        stage1_patch_c16,
        pixel_base,
        channel_group,
        dy,
        dx,
        cell_x,
        cell_y);
    const aie::vector<int16, kElements> residual_i16 = aie::unpack(residual_i8);
    return aie::unpack(residual_i16);
}

inline void stage1_2_macro_stage2_spatial3x3_m4x8_with_epilogue_blocks(
    const std::int8_t* stage1_patch_c16,
    const std::int8_t* params,
    const int pixel_base,
    const int output_chunk,
    const int cell_x,
    const int cell_y,
    std::int8_t* output_block)
{
    const std::int32_t* header = i32_at(params, 0);
    const int spatial_shift = header[0];
    const std::int8_t* weights = params + kStage1HotSpatialWeightsOffset;
    const std::int32_t* bias_blocks = i32_at(params, kStage1HotSpatialBiasBlocksOffset);
    const std::int32_t* scale_blocks = i32_at(params, kStage1HotSpatialScaleBlocksOffset);

    MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
    int tap = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const aie::vector<int8, kActElements> act_v = load_stage1_2_macro_stage1_patch_tap(
                    stage1_patch_c16,
                    pixel_base,
                    input_group,
                    dy,
                    dx,
                    cell_x,
                    cell_y);
                const std::size_t weight_offset =
                    (((static_cast<std::size_t>(tap) * kOutputChunksC16 + output_chunk) * kInputGroupsC16) + input_group) *
                    kWeightElements;
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }
            ++tap;
        }
    }

    const auto dot_acc = accumulator.to_accum();
    const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(bias_blocks + output_chunk * kElements);
    const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(scale_blocks + output_chunk * kElements);
    const auto biased_acc = aie::add(dot_acc, bias_v);
    const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
    const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
    const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, spatial_shift);
    aie::store_v(output_block, result_v);
}

inline void stage1_2_macro_stage2_from_stage1_patch34x18_cell(
    const std::int8_t* stage1_patch_c16,
    const std::int8_t* params,
    const int cell_x,
    const int cell_y,
    std::int8_t* output_c16)
{
    const std::int32_t* header = i32_at(params, 0);
    const int expand_shift = header[1];
    const int contract_shift = header[2];
    const std::int8_t* expand_weights = params + kStage1HotExpandWeightsOffset;
    const std::int32_t* expand_bias_blocks = i32_at(params, kStage1HotExpandBiasBlocksOffset);
    const std::int32_t* expand_scale_blocks = i32_at(params, kStage1HotExpandScaleBlocksOffset);
    const std::int8_t* contract_weights = params + kStage1HotContractWeightsOffset;
    const std::int32_t* contract_bias_blocks = i32_at(params, kStage1HotContractBiasBlocksOffset);
    const std::int32_t* contract_scale_blocks = i32_at(params, kStage1HotContractScaleBlocksOffset);
    const std::int32_t* residual_scale_blocks = i32_at(params, kStage1HotResidualScaleBlocksOffset);

    alignas(32) std::int8_t spatial_block_0[kElements];
    alignas(32) std::int8_t spatial_block_1[kElements];
    alignas(32) std::int8_t hidden_blocks[kOutputChunksC32][kElements];

    for (int group = 0; group < kPixelGroups; ++group) {
        const int pixel_base = group * kMmulM;
        stage1_2_macro_stage2_spatial3x3_m4x8_with_epilogue_blocks(
            stage1_patch_c16,
            params,
            pixel_base,
            0,
            cell_x,
            cell_y,
            spatial_block_0);
        stage1_2_macro_stage2_spatial3x3_m4x8_with_epilogue_blocks(
            stage1_patch_c16,
            params,
            pixel_base,
            1,
            cell_x,
            cell_y,
            spatial_block_1);

        for (int chunk = 0; chunk < kOutputChunksC32; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC16; ++input_group) {
                const std::int8_t* act = input_group == 0 ? spatial_block_0 : spatial_block_1;
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC16) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(act);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(expand_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(expand_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(expand_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(scaled_acc, expand_shift);
            aie::store_v(hidden_blocks[chunk], result_v);
        }

        for (int chunk = 0; chunk < kOutputChunksC16; ++chunk) {
            MmulS8S8 accumulator = aie::zeros<acc32, kElements>();
            for (int input_group = 0; input_group < kInputGroupsC32; ++input_group) {
                const std::size_t weight_offset =
                    ((static_cast<std::size_t>(chunk) * kInputGroupsC32) + input_group) * kWeightElements;
                const aie::vector<int8, kActElements> act_v = aie::load_v<kActElements>(hidden_blocks[input_group]);
                const aie::vector<int8, kWeightElements> weight_v = aie::load_v<kWeightElements>(contract_weights + weight_offset);
                accumulator.mac(act_v, weight_v);
            }

            const auto dot_acc = accumulator.to_accum();
            const aie::vector<int32, kElements> bias_v = aie::load_v<kElements>(contract_bias_blocks + chunk * kElements);
            const aie::vector<int32, kElements> scale_v = aie::load_v<kElements>(contract_scale_blocks + chunk * kElements);
            const auto biased_acc = aie::add(dot_acc, bias_v);
            const aie::vector<int32, kElements> biased_v = aie::to_vector<int32>(biased_acc, 0);
            const auto contract_scaled_acc = aie::mul<acc64>(biased_v, scale_v);
            const aie::vector<int32, kElements> residual_v = load_stage1_2_macro_stage1_patch_tap_i32(
                stage1_patch_c16,
                pixel_base,
                chunk,
                0,
                0,
                cell_x,
                cell_y);
            const aie::vector<int32, kElements> residual_scale_v = aie::load_v<kElements>(residual_scale_blocks + chunk * kElements);
            const auto residual_scaled_acc = aie::mul<acc64>(residual_v, residual_scale_v);
            const auto summed_acc = aie::add(contract_scaled_acc, residual_scaled_acc);
            const aie::vector<int8, kElements> result_v = aie::to_vector<int8>(summed_acc, contract_shift);
            store_c16_m4x8(output_c16, pixel_base, chunk, result_v);
        }
    }
}


extern "C" void apusr_middle_core_stage1_2_fused_window20x12_to_c16_cell_i8(
    const signed char* input_window_c16_20x12,
    const signed char* stage1_params,
    const signed char* stage2_params,
    signed char* output_cell_c16)
{
    c16_window20x12_stage1_stage2_fused_srs_to_c16_cell_impl(
        reinterpret_cast<const std::int8_t*>(input_window_c16_20x12),
        reinterpret_cast<const std::int8_t*>(stage1_params),
        reinterpret_cast<const std::int8_t*>(stage2_params),
        reinterpret_cast<std::int8_t*>(output_cell_c16));
}



extern "C" void apusr_middle_core_stage1_2_fused_window20x12_to_c16_cell_i8_tee(
    const signed char* input_window_c16_20x12,
    const signed char* stage1_params,
    const signed char* stage2_params,
    signed char* output_cell_c16,
    signed char* skipa_cell_c16)
{
    auto* output = reinterpret_cast<std::int8_t*>(output_cell_c16);
    c16_window20x12_stage1_stage2_fused_srs_to_c16_cell_impl(
        reinterpret_cast<const std::int8_t*>(input_window_c16_20x12),
        reinterpret_cast<const std::int8_t*>(stage1_params),
        reinterpret_cast<const std::int8_t*>(stage2_params),
        output);
    copy_c16_cell_vector(reinterpret_cast<std::int8_t*>(skipa_cell_c16), output);
}


extern "C" void apusr_middle_core_stage1_2_fused_window20x12_to_c16_window2x2_i8_tee(
    const signed char* input_window_c16_20x12,
    const signed char* stage1_params,
    const signed char* stage2_params,
    signed char* output_window2x2_c16,
    signed char* skipa_window2x2_c16,
    std::int32_t cell_index)
{
    std::int8_t* output_cell = reinterpret_cast<std::int8_t*>(output_window2x2_c16) + static_cast<std::size_t>(cell_index) * kCellBytes;
    c16_window20x12_stage1_stage2_fused_srs_to_c16_cell_impl(
        reinterpret_cast<const std::int8_t*>(input_window_c16_20x12),
        reinterpret_cast<const std::int8_t*>(stage1_params),
        reinterpret_cast<const std::int8_t*>(stage2_params),
        output_cell);
    copy_c16_cell_vector(
        reinterpret_cast<std::int8_t*>(skipa_window2x2_c16) + static_cast<std::size_t>(cell_index) * kCellBytes,
        output_cell);
}


extern "C" void apusr_middle_core_stage1_2_paired_macroblock36x20_to_c16_window2x2_i8_shared_skipa(
    const signed char* input_macroblock_c16_36x20,
    const signed char* stage1_params,
    const signed char* stage2_params,
    signed char* output_window2x2_c16,
    signed char* skipa_raster_macroblock2x2_c16)
{
    ::aie::set_saturation(aie::saturation_mode::saturate);
    ::aie::set_rounding(aie::rounding_mode::symmetric_inf);

    alignas(32) std::int8_t stage1_patch_c16[kStage1_2MacroStage1PatchBytes];
    materialize_stage1_2_macro_stage1_patch34x18(
        reinterpret_cast<const std::int8_t*>(input_macroblock_c16_36x20),
        reinterpret_cast<const std::int8_t*>(stage1_params),
        stage1_patch_c16);

    for (int cell_index = 0; cell_index < 4; ++cell_index) {
        const int cell_x = (cell_index & 1) * kCellWidth;
        const int cell_y = (cell_index >> 1) * kCellRows;
        std::int8_t* output_cell = reinterpret_cast<std::int8_t*>(output_window2x2_c16) +
            static_cast<std::size_t>(cell_index) * kCellBytes;
        stage1_2_macro_stage2_from_stage1_patch34x18_cell(
            stage1_patch_c16,
            reinterpret_cast<const std::int8_t*>(stage2_params),
            cell_x,
            cell_y,
            output_cell);
    }

    // SkipA is stored in tensor-raster order. Stage3 consumes the compact
    // four-cell object in cell order, while the sideband arranges the same
    // data as a 32x16 C16 raster macroblock so the S2MM descriptor can use
    // short contiguous row spans instead of a 4096-byte innermost BD dimension.
    copy_c16_window2x2_to_raster_macroblock_vector(
        reinterpret_cast<std::int8_t*>(skipa_raster_macroblock2x2_c16),
        reinterpret_cast<std::int8_t*>(output_window2x2_c16));
}


extern "C" void apusr_middle_core_stage1_2_fused_window20x12_to_c16_row_i8_tee(
    const signed char* input_window_c16_20x12,
    const signed char* stage1_params,
    const signed char* stage2_params,
    signed char* output_row_c16,
    signed char* skipa_row_c16,
    std::int32_t cell_column)
{
    std::int8_t* output_cell = reinterpret_cast<std::int8_t*>(output_row_c16) + static_cast<std::size_t>(cell_column) * kCellBytes;
    c16_window20x12_stage1_stage2_fused_srs_to_c16_cell_impl(
        reinterpret_cast<const std::int8_t*>(input_window_c16_20x12),
        reinterpret_cast<const std::int8_t*>(stage1_params),
        reinterpret_cast<const std::int8_t*>(stage2_params),
        output_cell);
    copy_c16_cell_vector(
        reinterpret_cast<std::int8_t*>(skipa_row_c16) + static_cast<std::size_t>(cell_column) * kCellBytes,
        output_cell);
}

extern "C" void apusr_middle_core_stage3_4_fused_stage2_rows_to_c32_cell_i8(
    const signed char* stage2_row0_c16,
    const signed char* stage2_row1_c16,
    const signed char* stage2_row2_c16,
    const signed char* stage2_row3_c16,
    const signed char* stage3_4_params,
    signed char* output_cell_c32,
    std::int32_t output_cell_column)
{
    alignas(32) std::int8_t window_c16[kStage34C16WindowBytes];
    materialize_stage3_4_c16_window_from_stage2_rows(
        reinterpret_cast<const std::int8_t*>(stage2_row0_c16),
        reinterpret_cast<const std::int8_t*>(stage2_row1_c16),
        reinterpret_cast<const std::int8_t*>(stage2_row2_c16),
        reinterpret_cast<const std::int8_t*>(stage2_row3_c16),
        output_cell_column,
        window_c16);
    c16_window20x40_downsample_fnb32_srs_to_c32_cell_impl(
        window_c16,
        reinterpret_cast<const std::int8_t*>(stage3_4_params),
        reinterpret_cast<std::int8_t*>(output_cell_c32));
}

extern "C" void apusr_middle_core_stage2_convnext16_3row_window_to_c16_cell_i8(
    const signed char* top_row_window3_c16,
    const signed char* center_row_window3_c16,
    const signed char* bottom_row_window3_c16,
    const signed char* stage2_params,
    signed char* output_cell_c16)
{
    c16_3row_window_convnext16_srs_to_c16_cell_impl(
        reinterpret_cast<const std::int8_t*>(top_row_window3_c16),
        reinterpret_cast<const std::int8_t*>(center_row_window3_c16),
        reinterpret_cast<const std::int8_t*>(bottom_row_window3_c16),
        reinterpret_cast<const std::int8_t*>(stage2_params),
        reinterpret_cast<std::int8_t*>(output_cell_c16));
}

extern "C" void apusr_middle_core_stage2_convnext16_halo_patch_to_c16_cell_i8(
    const signed char* input_halo_patch_c16,
    const signed char* stage2_params,
    signed char* output_cell_c16)
{
    c16_halo_patch_stage1_convnext16_srs_to_c16_cell_impl(
        reinterpret_cast<const std::int8_t*>(input_halo_patch_c16),
        reinterpret_cast<const std::int8_t*>(stage2_params),
        reinterpret_cast<std::int8_t*>(output_cell_c16));
}

extern "C" void apusr_middle_core_stage2_convnext16_halo_patch_to_c16_cell_i8_tee(
    const signed char* input_halo_patch_c16,
    const signed char* stage2_params,
    signed char* output_cell_c16,
    signed char* skipa_cell_c16)
{
    auto* output = reinterpret_cast<std::int8_t*>(output_cell_c16);
    c16_halo_patch_stage1_convnext16_srs_to_c16_cell_impl(
        reinterpret_cast<const std::int8_t*>(input_halo_patch_c16),
        reinterpret_cast<const std::int8_t*>(stage2_params),
        output);
    copy_c16_cell_vector(reinterpret_cast<std::int8_t*>(skipa_cell_c16), output);
}

extern "C" void apusr_middle_core_front_c16_halo_patch_expand_srs_to_c32_cell_i8(
    const signed char* halo_patch_c16,
    const signed char* front_params,
    signed char* output_cell_c32)
{
    c16_halo_patch_spatial3x3_expand_srs_to_c32_cell_impl(
        reinterpret_cast<const std::int8_t*>(halo_patch_c16),
        reinterpret_cast<const std::int8_t*>(front_params),
        reinterpret_cast<std::int8_t*>(output_cell_c32));
}

extern "C" void apusr_middle_core_stage1_convnext16_halo_patch_to_c16_cell_i8(
    const signed char* halo_patch_c16,
    const signed char* stage1_params,
    signed char* output_cell_c16)
{
    c16_halo_patch_stage1_convnext16_srs_to_c16_cell_impl(
        reinterpret_cast<const std::int8_t*>(halo_patch_c16),
        reinterpret_cast<const std::int8_t*>(stage1_params),
        reinterpret_cast<std::int8_t*>(output_cell_c16));
}

extern "C" void apusr_middle_core_front_c16_window7_expand_srs_to_c32_cell_i8(
    const signed char* above_window_c16,
    const signed char* center_window_c16,
    const signed char* below_window_c16,
    const signed char* front_params,
    const signed char* left_above_seam_c16,
    const signed char* left_center_seam_c16,
    const signed char* left_below_seam_c16,
    const signed char* right_above_seam_c16,
    const signed char* right_center_seam_c16,
    const signed char* right_below_seam_c16,
    signed char* output_cell_c32,
    std::int32_t center_index)
{
    c16_window7_spatial3x3_expand_srs_to_c32_cell_impl(
        reinterpret_cast<const std::int8_t*>(above_window_c16),
        reinterpret_cast<const std::int8_t*>(center_window_c16),
        reinterpret_cast<const std::int8_t*>(below_window_c16),
        reinterpret_cast<const std::int8_t*>(front_params),
        reinterpret_cast<const std::int8_t*>(left_above_seam_c16),
        reinterpret_cast<const std::int8_t*>(left_center_seam_c16),
        reinterpret_cast<const std::int8_t*>(left_below_seam_c16),
        reinterpret_cast<const std::int8_t*>(right_above_seam_c16),
        reinterpret_cast<const std::int8_t*>(right_center_seam_c16),
        reinterpret_cast<const std::int8_t*>(right_below_seam_c16),
        reinterpret_cast<std::int8_t*>(output_cell_c32),
        center_index);
}

extern "C" void apusr_middle_core_c16_window7_extract_left_seam_i8(
    const signed char* window_c16,
    signed char* output_seam_c16,
    std::int32_t center_index)
{
    copy_cell_column_to_seam(
        window_cell(reinterpret_cast<const std::int8_t*>(window_c16), center_index),
        0,
        reinterpret_cast<std::int8_t*>(output_seam_c16));
}

extern "C" void apusr_middle_core_c16_window7_extract_right_seam_i8(
    const signed char* window_c16,
    signed char* output_seam_c16,
    std::int32_t center_index)
{
    copy_cell_column_to_seam(
        window_cell(reinterpret_cast<const std::int8_t*>(window_c16), center_index),
        kCellWidth - 1,
        reinterpret_cast<std::int8_t*>(output_seam_c16));
}


#define APUSR_DEFINE_FRONT_ROW_PRIMITIVES(CELLS) \
extern "C" void apusr_middle_core_front_c16_row##CELLS##_expand_srs_to_c32_cell_i8( \
    const signed char* above_row_c16, \
    const signed char* center_row_c16, \
    const signed char* below_row_c16, \
    const signed char* front_params, \
    const signed char* left_above_seam_c16, \
    const signed char* left_center_seam_c16, \
    const signed char* left_below_seam_c16, \
    const signed char* right_above_seam_c16, \
    const signed char* right_center_seam_c16, \
    const signed char* right_below_seam_c16, \
    signed char* output_cell_c32, \
    std::int32_t center_index) \
{ \
    c16_row_spatial3x3_expand_srs_to_c32_cell_impl<CELLS>( \
        reinterpret_cast<const std::int8_t*>(above_row_c16), \
        reinterpret_cast<const std::int8_t*>(center_row_c16), \
        reinterpret_cast<const std::int8_t*>(below_row_c16), \
        reinterpret_cast<const std::int8_t*>(front_params), \
        reinterpret_cast<const std::int8_t*>(left_above_seam_c16), \
        reinterpret_cast<const std::int8_t*>(left_center_seam_c16), \
        reinterpret_cast<const std::int8_t*>(left_below_seam_c16), \
        reinterpret_cast<const std::int8_t*>(right_above_seam_c16), \
        reinterpret_cast<const std::int8_t*>(right_center_seam_c16), \
        reinterpret_cast<const std::int8_t*>(right_below_seam_c16), \
        reinterpret_cast<std::int8_t*>(output_cell_c32), \
        center_index); \
} \
extern "C" void apusr_middle_core_c16_row##CELLS##_extract_left_seam_i8( \
    const signed char* row_c16, \
    signed char* output_seam_c16) \
{ \
    copy_cell_column_to_seam( \
        row_cell<CELLS>(reinterpret_cast<const std::int8_t*>(row_c16), 0), \
        0, \
        reinterpret_cast<std::int8_t*>(output_seam_c16)); \
} \
extern "C" void apusr_middle_core_c16_row##CELLS##_extract_right_seam_i8( \
    const signed char* row_c16, \
    signed char* output_seam_c16) \
{ \
    copy_cell_column_to_seam( \
        row_cell<CELLS>(reinterpret_cast<const std::int8_t*>(row_c16), CELLS - 1), \
        kCellWidth - 1, \
        reinterpret_cast<std::int8_t*>(output_seam_c16)); \
}

APUSR_DEFINE_FRONT_ROW_PRIMITIVES(7)
APUSR_DEFINE_FRONT_ROW_PRIMITIVES(8)
APUSR_DEFINE_FRONT_ROW_PRIMITIVES(15)
APUSR_DEFINE_FRONT_ROW_PRIMITIVES(30)
APUSR_DEFINE_FRONT_ROW_PRIMITIVES(60)

#define APUSR_DEFINE_VECTOR_PRIMITIVES(CELLS) \
extern "C" void apusr_middle_core_front_c16_expand_srs_to_c32_cell_##CELLS##c_i8( \
    const signed char* above_cell_c16, \
    const signed char* center_cell_c16, \
    const signed char* below_cell_c16, \
    const signed char* front_params, \
    signed char* output_cell_c32) \
{ \
    c16_spatial3x3_expand_srs_to_c32_cell_impl( \
        reinterpret_cast<const std::int8_t*>(above_cell_c16), \
        reinterpret_cast<const std::int8_t*>(center_cell_c16), \
        reinterpret_cast<const std::int8_t*>(below_cell_c16), \
        reinterpret_cast<const std::int8_t*>(front_params), \
        reinterpret_cast<std::int8_t*>(output_cell_c32)); \
} \
extern "C" void apusr_middle_core_front_c16_expand_srs_to_c32_cell_left_seam_##CELLS##c_i8( \
    const signed char* above_cell_c16, \
    const signed char* center_cell_c16, \
    const signed char* below_cell_c16, \
    const signed char* front_params, \
    const signed char* left_above_seam_c16, \
    const signed char* left_center_seam_c16, \
    const signed char* left_below_seam_c16, \
    signed char* output_cell_c32) \
{ \
    c16_spatial3x3_expand_srs_to_c32_cell_impl<true, false>( \
        reinterpret_cast<const std::int8_t*>(above_cell_c16), \
        reinterpret_cast<const std::int8_t*>(center_cell_c16), \
        reinterpret_cast<const std::int8_t*>(below_cell_c16), \
        reinterpret_cast<const std::int8_t*>(front_params), \
        reinterpret_cast<std::int8_t*>(output_cell_c32), \
        reinterpret_cast<const std::int8_t*>(left_above_seam_c16), \
        reinterpret_cast<const std::int8_t*>(left_center_seam_c16), \
        reinterpret_cast<const std::int8_t*>(left_below_seam_c16), \
        nullptr, \
        nullptr, \
        nullptr); \
} \
extern "C" void apusr_middle_core_front_c16_expand_srs_to_c32_cell_right_seam_##CELLS##c_i8( \
    const signed char* above_cell_c16, \
    const signed char* center_cell_c16, \
    const signed char* below_cell_c16, \
    const signed char* front_params, \
    const signed char* right_above_seam_c16, \
    const signed char* right_center_seam_c16, \
    const signed char* right_below_seam_c16, \
    signed char* output_cell_c32) \
{ \
    c16_spatial3x3_expand_srs_to_c32_cell_impl<false, true>( \
        reinterpret_cast<const std::int8_t*>(above_cell_c16), \
        reinterpret_cast<const std::int8_t*>(center_cell_c16), \
        reinterpret_cast<const std::int8_t*>(below_cell_c16), \
        reinterpret_cast<const std::int8_t*>(front_params), \
        reinterpret_cast<std::int8_t*>(output_cell_c32), \
        nullptr, \
        nullptr, \
        nullptr, \
        reinterpret_cast<const std::int8_t*>(right_above_seam_c16), \
        reinterpret_cast<const std::int8_t*>(right_center_seam_c16), \
        reinterpret_cast<const std::int8_t*>(right_below_seam_c16)); \
} \
extern "C" void apusr_middle_core_front_c16_expand_srs_to_c32_cell_lr_seam_##CELLS##c_i8( \
    const signed char* above_cell_c16, \
    const signed char* center_cell_c16, \
    const signed char* below_cell_c16, \
    const signed char* front_params, \
    const signed char* left_above_seam_c16, \
    const signed char* left_center_seam_c16, \
    const signed char* left_below_seam_c16, \
    const signed char* right_above_seam_c16, \
    const signed char* right_center_seam_c16, \
    const signed char* right_below_seam_c16, \
    signed char* output_cell_c32) \
{ \
    c16_spatial3x3_expand_srs_to_c32_cell_impl<true, true>( \
        reinterpret_cast<const std::int8_t*>(above_cell_c16), \
        reinterpret_cast<const std::int8_t*>(center_cell_c16), \
        reinterpret_cast<const std::int8_t*>(below_cell_c16), \
        reinterpret_cast<const std::int8_t*>(front_params), \
        reinterpret_cast<std::int8_t*>(output_cell_c32), \
        reinterpret_cast<const std::int8_t*>(left_above_seam_c16), \
        reinterpret_cast<const std::int8_t*>(left_center_seam_c16), \
        reinterpret_cast<const std::int8_t*>(left_below_seam_c16), \
        reinterpret_cast<const std::int8_t*>(right_above_seam_c16), \
        reinterpret_cast<const std::int8_t*>(right_center_seam_c16), \
        reinterpret_cast<const std::int8_t*>(right_below_seam_c16)); \
} \
extern "C" void apusr_middle_core_front_c16_expand_srs_to_c32_cell_true3x3_##CELLS##c_i8( \
    const signed char* above_left_cell_c16, \
    const signed char* above_center_cell_c16, \
    const signed char* above_right_cell_c16, \
    const signed char* center_left_cell_c16, \
    const signed char* center_cell_c16, \
    const signed char* center_right_cell_c16, \
    const signed char* below_left_cell_c16, \
    const signed char* below_center_cell_c16, \
    const signed char* below_right_cell_c16, \
    const signed char* front_params, \
    signed char* output_cell_c32) \
{ \
    c16_spatial3x3_full3x3_expand_srs_to_c32_cell_impl( \
        reinterpret_cast<const std::int8_t*>(above_left_cell_c16), \
        reinterpret_cast<const std::int8_t*>(above_center_cell_c16), \
        reinterpret_cast<const std::int8_t*>(above_right_cell_c16), \
        reinterpret_cast<const std::int8_t*>(center_left_cell_c16), \
        reinterpret_cast<const std::int8_t*>(center_cell_c16), \
        reinterpret_cast<const std::int8_t*>(center_right_cell_c16), \
        reinterpret_cast<const std::int8_t*>(below_left_cell_c16), \
        reinterpret_cast<const std::int8_t*>(below_center_cell_c16), \
        reinterpret_cast<const std::int8_t*>(below_right_cell_c16), \
        reinterpret_cast<const std::int8_t*>(front_params), \
        reinterpret_cast<std::int8_t*>(output_cell_c32)); \
} \
extern "C" void apusr_middle_core_c32_expand_srs_to_c64_cell_##CELLS##c_i8( \
    const signed char* input_cell_c32, \
    const signed char* expand_params, \
    signed char* output_cell_c64) \
{ \
    c32_expand_srs_to_c64_cell_impl( \
        reinterpret_cast<const std::int8_t*>(input_cell_c32), \
        reinterpret_cast<const std::int8_t*>(expand_params), \
        reinterpret_cast<std::int8_t*>(output_cell_c64)); \
} \
extern "C" void apusr_middle_core_c64_contract_srs_to_c32_cell_##CELLS##c_i8( \
    const signed char* input_cell_c64, \
    const signed char* contract_params, \
    signed char* output_cell_c32) \
{ \
    c64_contract_srs_to_c32_cell_impl( \
        reinterpret_cast<const std::int8_t*>(input_cell_c64), \
        reinterpret_cast<const std::int8_t*>(contract_params), \
        reinterpret_cast<std::int8_t*>(output_cell_c32)); \
} \
extern "C" void apusr_middle_core_c32_contract_srs_to_c16_cell_##CELLS##c_i8( \
    const signed char* input_cell_c32, \
    const signed char* contract_params, \
    signed char* output_cell_c16) \
{ \
    c32_contract_srs_to_c16_cell_impl( \
        reinterpret_cast<const std::int8_t*>(input_cell_c32), \
        reinterpret_cast<const std::int8_t*>(contract_params), \
        reinterpret_cast<std::int8_t*>(output_cell_c16)); \
}

APUSR_DEFINE_VECTOR_PRIMITIVES(7)
APUSR_DEFINE_VECTOR_PRIMITIVES(8)
APUSR_DEFINE_VECTOR_PRIMITIVES(15)
APUSR_DEFINE_VECTOR_PRIMITIVES(30)
APUSR_DEFINE_VECTOR_PRIMITIVES(60)

#undef APUSR_DEFINE_VECTOR_PRIMITIVES
