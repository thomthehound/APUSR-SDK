// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <cstdint>

namespace apusr::aie_middle_core {
namespace {

constexpr int kQuadrants = 4;
constexpr int kC8Chunks = 4;
constexpr int kC8 = 8;
constexpr int kC16 = 16;
constexpr int kC32 = 32;
constexpr int kC64 = 64;
constexpr int kTileWidth = 16;
constexpr int kTileHeight = 8;
constexpr int kFullWidth = 32;
constexpr int kFullHeight = 16;
constexpr int kC32DownsourceWidth = 40;
constexpr int kC32DownsourceHeight = 20;
constexpr int kHaloWidth = 18;
constexpr int kHaloHeight = 10;

constexpr int kQuadrantChunkBytes = kTileWidth * kTileHeight * kC8;
constexpr int kGroupedQuadrantPackageBytes = kQuadrants * kC8Chunks * kQuadrantChunkBytes;
constexpr int kGroupedC32FullBytes = kC8Chunks * kFullWidth * kFullHeight * kC8;
constexpr int kGroupedC32DownsourceBytes = kC8Chunks * kC32DownsourceWidth * kC32DownsourceHeight * kC8;
constexpr int kC64QuadrantBytes = kTileWidth * kTileHeight * kC64;
constexpr int kC64QuadrantPackageBytes = kQuadrants * kC64QuadrantBytes;
constexpr int kC64HaloBytes = kHaloWidth * kHaloHeight * kC64;
constexpr int kC64HaloPackageBytes = kQuadrants * kC64HaloBytes;
constexpr int kC32HaloBytes = kHaloWidth * kHaloHeight * kC32;
constexpr int kC32HaloPackageBytes = kQuadrants * kC32HaloBytes;
constexpr int kGroupedC8HaloPackageBytes = kQuadrants * kC8Chunks * kHaloWidth * kHaloHeight * kC8;
constexpr int kC16FullBytes = kFullWidth * kFullHeight * kC16;
constexpr int kC16HaloBytes = kHaloWidth * kHaloHeight * kC16;
constexpr int kC16HaloPackageBytes = kQuadrants * kC16HaloBytes;

constexpr int kHaloOriginY[kQuadrants] = {0, 0, kTileHeight - 2, kTileHeight - 2};
constexpr int kHaloOriginX[kQuadrants] = {0, kTileWidth - 2, 0, kTileWidth - 2};

inline void zero_i8(std::int8_t* dst, const int count)
{
    for (int index = 0; index < count; ++index) {
        dst[index] = 0;
    }
}

[[nodiscard]] inline int quadrant_from_xy(const int x, const int y)
{
    return (y >= kTileHeight ? 2 : 0) + (x >= kTileWidth ? 1 : 0);
}

[[nodiscard]] inline int grouped_quadrant_offset(const int quadrant, const int chunk, const int y, const int x, const int lane)
{
    return (((quadrant * kC8Chunks + chunk) * kTileHeight + y) * kTileWidth + x) * kC8 + lane;
}

[[nodiscard]] inline int grouped_full_offset(const int chunk, const int y, const int x, const int lane)
{
    return ((chunk * kFullHeight + y) * kFullWidth + x) * kC8 + lane;
}

[[nodiscard]] inline int grouped_downsource_offset(const int chunk, const int y, const int x, const int lane)
{
    return ((chunk * kC32DownsourceHeight + y) * kC32DownsourceWidth + x) * kC8 + lane;
}

[[nodiscard]] inline int c64_quadrant_offset(const int quadrant, const int y, const int x, const int channel)
{
    return ((quadrant * kTileHeight + y) * kTileWidth + x) * kC64 + channel;
}

[[nodiscard]] inline int c64_halo_offset(const int quadrant, const int y, const int x, const int channel)
{
    return ((quadrant * kHaloHeight + y) * kHaloWidth + x) * kC64 + channel;
}

[[nodiscard]] inline int c32_halo_offset(const int quadrant, const int y, const int x, const int channel)
{
    return ((quadrant * kHaloHeight + y) * kHaloWidth + x) * kC32 + channel;
}

[[nodiscard]] inline int grouped_c8_halo_offset(const int quadrant, const int chunk, const int y, const int x, const int lane)
{
    return (((quadrant * kC8Chunks + chunk) * kHaloHeight + y) * kHaloWidth + x) * kC8 + lane;
}

[[nodiscard]] inline int c16_full_offset(const int y, const int x, const int channel)
{
    return ((y * kFullWidth + x) * kC16) + channel;
}

[[nodiscard]] inline int c16_halo_offset(const int quadrant, const int y, const int x, const int channel)
{
    return ((quadrant * kHaloHeight + y) * kHaloWidth + x) * kC16 + channel;
}

inline void scatter_c64_pixel_to_halo(
    const std::int8_t* source_pixel_c64,
    std::int8_t* output_halos_qmajor_nhwc,
    const int quadrant,
    const int full_y,
    const int full_x)
{
    const int halo_y = full_y - kHaloOriginY[quadrant];
    const int halo_x = full_x - kHaloOriginX[quadrant];
    if (halo_y < 0 || halo_y >= kHaloHeight || halo_x < 0 || halo_x >= kHaloWidth) {
        return;
    }
    for (int channel = 0; channel < kC64; ++channel) {
        output_halos_qmajor_nhwc[c64_halo_offset(quadrant, halo_y, halo_x, channel)] = source_pixel_c64[channel];
    }
}

inline void scatter_c32_pixel_to_halo(
    const std::int8_t* source_pixel_c32,
    std::int8_t* output_halos_qmajor_nhwc,
    const int quadrant,
    const int full_y,
    const int full_x)
{
    const int halo_y = full_y - kHaloOriginY[quadrant];
    const int halo_x = full_x - kHaloOriginX[quadrant];
    if (halo_y < 0 || halo_y >= kHaloHeight || halo_x < 0 || halo_x >= kHaloWidth) {
        return;
    }
    for (int channel = 0; channel < kC32; ++channel) {
        output_halos_qmajor_nhwc[c32_halo_offset(quadrant, halo_y, halo_x, channel)] = source_pixel_c32[channel];
    }
}

inline void scatter_c16_pixel_to_halo(
    const std::int8_t* source_pixel_c16,
    std::int8_t* output_halos_qmajor_nhwc,
    const int quadrant,
    const int full_y,
    const int full_x)
{
    const int halo_y = full_y - kHaloOriginY[quadrant];
    const int halo_x = full_x - kHaloOriginX[quadrant];
    if (halo_y < 0 || halo_y >= kHaloHeight || halo_x < 0 || halo_x >= kHaloWidth) {
        return;
    }
    for (int channel = 0; channel < kC16; ++channel) {
        output_halos_qmajor_nhwc[c16_halo_offset(quadrant, halo_y, halo_x, channel)] = source_pixel_c16[channel];
    }
}

} // namespace

extern "C" void apusr_middle_core_c16_quadrants_stage_halo16_tile_i8(
    const std::int8_t* stage_output_subtiles_qmajor_nhwc,
    std::int8_t* next_stage_halos_qmajor_nhwc)
{
    constexpr int kSubtilesPerQuadrant = 4;
    constexpr int kSubtileTileBytes = kTileWidth * kTileHeight * kC16;
    constexpr int kSubtileHaloBytes = kHaloWidth * kHaloHeight * kC16;
    zero_i8(next_stage_halos_qmajor_nhwc, kQuadrants * kSubtilesPerQuadrant * kSubtileHaloBytes);

    for (int quadrant = 0; quadrant < kQuadrants; ++quadrant) {
        for (int subtile = 0; subtile < kSubtilesPerQuadrant; ++subtile) {
            const std::int8_t* input_tile = stage_output_subtiles_qmajor_nhwc + (quadrant * kSubtilesPerQuadrant + subtile) * kSubtileTileBytes;
            std::int8_t* output_halo = next_stage_halos_qmajor_nhwc + (quadrant * kSubtilesPerQuadrant + subtile) * kSubtileHaloBytes;
            for (int y = 0; y < kTileHeight; ++y) {
                for (int x = 0; x < kTileWidth; ++x) {
                    const std::int8_t* source = input_tile + ((y * kTileWidth + x) * kC16);
                    for (int channel = 0; channel < kC16; ++channel) {
                        output_halo[((y + 1) * kHaloWidth + (x + 1)) * kC16 + channel] = source[channel];
                    }
                }
            }
        }
    }
}

extern "C" void apusr_middle_core_stage2_c16_skipa_downsource4_tile_i8(
    const std::int8_t* stage2_output_subtiles_qmajor_nhwc,
    std::int8_t* skipa_tiles_qmajor_nhwc,
    std::int8_t* stage3_downsource_tiles_qmajor_nhwc)
{
    constexpr int kSubtilesPerQuadrant = 4;
    constexpr int kSubtileTileBytes = kTileWidth * kTileHeight * kC16;
    constexpr int kSkipaTileBytes = kFullWidth * kFullHeight * kC16;
    constexpr int kStage3DownsourceTileBytes = kC32DownsourceWidth * kC32DownsourceHeight * kC16;
    zero_i8(skipa_tiles_qmajor_nhwc, kQuadrants * kSkipaTileBytes);
    zero_i8(stage3_downsource_tiles_qmajor_nhwc, kQuadrants * kStage3DownsourceTileBytes);

    for (int quadrant = 0; quadrant < kQuadrants; ++quadrant) {
        for (int subtile = 0; subtile < kSubtilesPerQuadrant; ++subtile) {
            const int subtile_origin_y = (subtile >= 2) ? kTileHeight : 0;
            const int subtile_origin_x = (subtile & 1) ? kTileWidth : 0;
            const std::int8_t* input_tile = stage2_output_subtiles_qmajor_nhwc + (quadrant * kSubtilesPerQuadrant + subtile) * kSubtileTileBytes;
            std::int8_t* skipa_tile = skipa_tiles_qmajor_nhwc + quadrant * kSkipaTileBytes;
            std::int8_t* downsource_tile = stage3_downsource_tiles_qmajor_nhwc + quadrant * kStage3DownsourceTileBytes;
            for (int y = 0; y < kTileHeight; ++y) {
                for (int x = 0; x < kTileWidth; ++x) {
                    const int full_y = subtile_origin_y + y;
                    const int full_x = subtile_origin_x + x;
                    const std::int8_t* source = input_tile + ((y * kTileWidth + x) * kC16);
                    for (int channel = 0; channel < kC16; ++channel) {
                        const std::int8_t value = source[channel];
                        skipa_tile[((full_y * kFullWidth + full_x) * kC16) + channel] = value;
                        downsource_tile[(((full_y + 2) * kC32DownsourceWidth + (full_x + 4)) * kC16) + channel] = value;
                    }
                }
            }
        }
    }
}

extern "C" void apusr_middle_core_stage3_4_c32_stage5_halo4_c8group_tile_i8(
    const std::int8_t* stage3_4_output_tiles_qmajor_nhwc,
    std::int8_t* stage5_grouped_c8_halos_qmajor)
{
    constexpr int kC32TileBytes = kTileWidth * kTileHeight * kC32;
    zero_i8(stage5_grouped_c8_halos_qmajor, kGroupedC8HaloPackageBytes);

    for (int quadrant = 0; quadrant < kQuadrants; ++quadrant) {
        const std::int8_t* input_tile = stage3_4_output_tiles_qmajor_nhwc + quadrant * kC32TileBytes;
        for (int y = 0; y < kTileHeight; ++y) {
            for (int x = 0; x < kTileWidth; ++x) {
                for (int channel = 0; channel < kC32; ++channel) {
                    const int chunk = channel / kC8;
                    const int lane = channel % kC8;
                    stage5_grouped_c8_halos_qmajor[grouped_c8_halo_offset(quadrant, chunk, y + 1, x + 1, lane)] = input_tile[((y * kTileWidth + x) * kC32) + channel];
                }
            }
        }
    }
}

extern "C" void apusr_middle_core_stage5_c8group_skipb_downsource_tile_i8(
    const std::int8_t* stage5_output_quadrants_qmajor_c8group,
    std::int8_t* skipb_grouped_c8_32x16_c32,
    std::int8_t* downsource_grouped_c8_40x20_c32)
{
    zero_i8(skipb_grouped_c8_32x16_c32, kGroupedC32FullBytes);
    zero_i8(downsource_grouped_c8_40x20_c32, kGroupedC32DownsourceBytes);

    for (int full_y = 0; full_y < kFullHeight; ++full_y) {
        const int quadrant_y = full_y >= kTileHeight ? full_y - kTileHeight : full_y;
        const int downsource_y = full_y + 2;
        for (int full_x = 0; full_x < kFullWidth; ++full_x) {
            const int quadrant_x = full_x >= kTileWidth ? full_x - kTileWidth : full_x;
            const int quadrant = quadrant_from_xy(full_x, full_y);
            const int downsource_x = full_x + 4;
            for (int chunk = 0; chunk < kC8Chunks; ++chunk) {
                for (int lane = 0; lane < kC8; ++lane) {
                    const std::int8_t value = stage5_output_quadrants_qmajor_c8group[grouped_quadrant_offset(
                        quadrant,
                        chunk,
                        quadrant_y,
                        quadrant_x,
                        lane)];
                    skipb_grouped_c8_32x16_c32[grouped_full_offset(chunk, full_y, full_x, lane)] = value;
                    downsource_grouped_c8_40x20_c32[grouped_downsource_offset(chunk, downsource_y, downsource_x, lane)] = value;
                }
            }
        }
    }
}

extern "C" void apusr_middle_core_stage6_7_c64_halo4_tile_i8(
    const std::int8_t* stage6_7_output_quadrants_qmajor_nhwc_c64,
    std::int8_t* stage8_halos_qmajor_nhwc_c64)
{
    zero_i8(stage8_halos_qmajor_nhwc_c64, kC64HaloPackageBytes);

    for (int full_y = 0; full_y < kFullHeight; ++full_y) {
        const int quadrant_y = full_y >= kTileHeight ? full_y - kTileHeight : full_y;
        for (int full_x = 0; full_x < kFullWidth; ++full_x) {
            const int quadrant_x = full_x >= kTileWidth ? full_x - kTileWidth : full_x;
            const int source_quadrant = quadrant_from_xy(full_x, full_y);
            const std::int8_t* source_pixel =
                stage6_7_output_quadrants_qmajor_nhwc_c64 + c64_quadrant_offset(source_quadrant, quadrant_y, quadrant_x, 0);
            for (int halo_quadrant = 0; halo_quadrant < kQuadrants; ++halo_quadrant) {
                scatter_c64_pixel_to_halo(source_pixel, stage8_halos_qmajor_nhwc_c64, halo_quadrant, full_y, full_x);
            }
        }
    }
}


extern "C" void apusr_middle_core_stage6_7_c64_stage8_halo_tile_i8(
    const std::int8_t* stage6_7_output_16x8_nhwc_c64,
    std::int8_t* stage8_halo_18x10_nhwc_c64)
{
    zero_i8(stage8_halo_18x10_nhwc_c64, kC64HaloBytes);

    for (int y = 0; y < kTileHeight; ++y) {
        for (int x = 0; x < kTileWidth; ++x) {
            const std::int8_t* source_pixel = stage6_7_output_16x8_nhwc_c64 + ((y * kTileWidth + x) * kC64);
            std::int8_t* halo_pixel = stage8_halo_18x10_nhwc_c64 + (((y + 1) * kHaloWidth + (x + 1)) * kC64);
            for (int channel = 0; channel < kC64; ++channel) {
                halo_pixel[channel] = source_pixel[channel];
            }
        }
    }
}


extern "C" void apusr_middle_core_stage10_c8group_stage11_halo4_c8group_tile_i8(
    const std::int8_t* stage10_output_quadrants_qmajor_c8group,
    std::int8_t* stage11_halos_qmajor_c8group)
{
    zero_i8(stage11_halos_qmajor_c8group, kGroupedC8HaloPackageBytes);

    for (int full_y = 0; full_y < kFullHeight; ++full_y) {
        const int quadrant_y = full_y >= kTileHeight ? full_y - kTileHeight : full_y;
        for (int full_x = 0; full_x < kFullWidth; ++full_x) {
            const int quadrant_x = full_x >= kTileWidth ? full_x - kTileWidth : full_x;
            const int source_quadrant = quadrant_from_xy(full_x, full_y);
            for (int halo_quadrant = 0; halo_quadrant < kQuadrants; ++halo_quadrant) {
                const int halo_y = full_y - kHaloOriginY[halo_quadrant];
                const int halo_x = full_x - kHaloOriginX[halo_quadrant];
                if (halo_y < 0 || halo_y >= kHaloHeight || halo_x < 0 || halo_x >= kHaloWidth) {
                    continue;
                }
                for (int chunk = 0; chunk < kC8Chunks; ++chunk) {
                    for (int lane = 0; lane < kC8; ++lane) {
                        stage11_halos_qmajor_c8group[grouped_c8_halo_offset(halo_quadrant, chunk, halo_y, halo_x, lane)] =
                            stage10_output_quadrants_qmajor_c8group[grouped_quadrant_offset(
                                source_quadrant,
                                chunk,
                                quadrant_y,
                                quadrant_x,
                                lane)];
                    }
                }
            }
        }
    }
}

extern "C" void apusr_middle_core_stage10_c8group_stage11_halo4_tile_i8(
    const std::int8_t* stage10_output_quadrants_qmajor_c8group,
    std::int8_t* stage11_halos_qmajor_nhwc_c32)
{
    zero_i8(stage11_halos_qmajor_nhwc_c32, kC32HaloPackageBytes);

    alignas(32) std::int8_t pixel_c32[kC32]{};
    for (int full_y = 0; full_y < kFullHeight; ++full_y) {
        const int quadrant_y = full_y >= kTileHeight ? full_y - kTileHeight : full_y;
        for (int full_x = 0; full_x < kFullWidth; ++full_x) {
            const int quadrant_x = full_x >= kTileWidth ? full_x - kTileWidth : full_x;
            const int source_quadrant = quadrant_from_xy(full_x, full_y);
            for (int chunk = 0; chunk < kC8Chunks; ++chunk) {
                for (int lane = 0; lane < kC8; ++lane) {
                    pixel_c32[chunk * kC8 + lane] = stage10_output_quadrants_qmajor_c8group[grouped_quadrant_offset(
                        source_quadrant,
                        chunk,
                        quadrant_y,
                        quadrant_x,
                        lane)];
                }
            }
            for (int halo_quadrant = 0; halo_quadrant < kQuadrants; ++halo_quadrant) {
                scatter_c32_pixel_to_halo(pixel_c32, stage11_halos_qmajor_nhwc_c32, halo_quadrant, full_y, full_x);
            }
        }
    }
}

extern "C" void apusr_middle_core_stage11_c16_stage12_halo4_tile_i8(
    const std::int8_t* stage11_output_32x16_nhwc_c16,
    std::int8_t* stage12_halos_qmajor_nhwc_c16)
{
    zero_i8(stage12_halos_qmajor_nhwc_c16, kC16HaloPackageBytes);

    for (int full_y = 0; full_y < kFullHeight; ++full_y) {
        for (int full_x = 0; full_x < kFullWidth; ++full_x) {
            const std::int8_t* source_pixel = stage11_output_32x16_nhwc_c16 + c16_full_offset(full_y, full_x, 0);
            for (int halo_quadrant = 0; halo_quadrant < kQuadrants; ++halo_quadrant) {
                scatter_c16_pixel_to_halo(source_pixel, stage12_halos_qmajor_nhwc_c16, halo_quadrant, full_y, full_x);
            }
        }
    }
}

} // namespace apusr::aie_middle_core
