// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

namespace {

constexpr int apusr_kCellWidth = 16;
constexpr int apusr_kCellRows = 8;
constexpr int apusr_kM4x8M = 4;
constexpr int apusr_kM4x8N = 8;
constexpr int apusr_kM4x8Elements = apusr_kM4x8M * apusr_kM4x8N;
constexpr int apusr_kM4x8GroupsPerRow = 4;

inline int apusr_middle_core_m4x8_offset(const int pixel_base, const int channel_chunk, const int channel_chunks)
{
    const int row = pixel_base >> 4;
    const int group_x = (pixel_base & (apusr_kCellWidth - 1)) >> 2;
    const int block = row * apusr_kM4x8GroupsPerRow + group_x;
    return (block * channel_chunks + channel_chunk) * apusr_kM4x8Elements;
}

inline int apusr_middle_core_m4x8_offset_for_pixel(const int pixel, const int channel, const int channel_chunks)
{
    const int pixel_base = pixel & ~(apusr_kM4x8M - 1);
    const int pixel_lane = pixel & (apusr_kM4x8M - 1);
    const int channel_chunk = channel >> 3;
    const int channel_lane = channel & (apusr_kM4x8N - 1);
    return apusr_middle_core_m4x8_offset(pixel_base, channel_chunk, channel_chunks) + pixel_lane * apusr_kM4x8N + channel_lane;
}

} // namespace

extern "C" void apusr_middle_core_copy_c16_cell_i8(
    const signed char* input_cell_16x8_c16,
    signed char* output_cell_16x8_c16,
    const int byte_count)
{
    for (int index = 0; index < byte_count; ++index) {
        output_cell_16x8_c16[index] = input_cell_16x8_c16[index];
    }
}

extern "C" void apusr_middle_core_c16_cell_to_halo_i8(
    const signed char* input_cell_16x8_c16,
    signed char* output_halo_18x10_c16)
{
    for (int index = 0; index < 18 * 10 * 16; ++index) {
        output_halo_18x10_c16[index] = 0;
    }

    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 16; ++column) {
            for (int channel = 0; channel < 16; ++channel) {
                const int input_index = ((row * 16) + column) * 16 + channel;
                const int output_index = (((row + 1) * 18) + (column + 1)) * 16 + channel;
                output_halo_18x10_c16[output_index] = input_cell_16x8_c16[input_index];
            }
        }
    }
}

extern "C" void apusr_middle_core_c16_halo_to_cell_i8(
    const signed char* input_halo_18x10_c16,
    signed char* output_cell_16x8_c16)
{
    for (int row = 0; row < 8; ++row) {
        for (int column = 0; column < 16; ++column) {
            for (int channel = 0; channel < 16; ++channel) {
                const int input_index = (((row + 1) * 18) + (column + 1)) * 16 + channel;
                const int output_index = ((row * 16) + column) * 16 + channel;
                output_cell_16x8_c16[output_index] = input_halo_18x10_c16[input_index];
            }
        }
    }
}

extern "C" void apusr_middle_core_c16_cell_extract_left_seam_i8(
    const signed char* input_cell_16x8_c16,
    signed char* output_left_seam_8x1_c16)
{
    for (int row = 0; row < 8; ++row) {
        for (int channel = 0; channel < 16; ++channel) {
            output_left_seam_8x1_c16[(row * 16) + channel] = input_cell_16x8_c16[(row * 16 * 16) + channel];
        }
    }
}

extern "C" void apusr_middle_core_c16_cell_extract_right_seam_i8(
    const signed char* input_cell_16x8_c16,
    signed char* output_right_seam_8x1_c16)
{
    for (int row = 0; row < 8; ++row) {
        const int input_base = ((row * 16) + 15) * 16;
        for (int channel = 0; channel < 16; ++channel) {
            output_right_seam_8x1_c16[(row * 16) + channel] = input_cell_16x8_c16[input_base + channel];
        }
    }
}

extern "C" void apusr_middle_core_c16_lr_seam_pair_extract_left_i8(
    const signed char* input_lr_seam_pair_2x8x1_c16,
    signed char* output_left_seam_8x1_c16)
{
    for (int index = 0; index < 8 * 16; ++index) {
        output_left_seam_8x1_c16[index] = input_lr_seam_pair_2x8x1_c16[index];
    }
}

extern "C" void apusr_middle_core_c16_lr_seam_pair_extract_right_i8(
    const signed char* input_lr_seam_pair_2x8x1_c16,
    signed char* output_right_seam_8x1_c16)
{
    for (int index = 0; index < 8 * 16; ++index) {
        output_right_seam_8x1_c16[index] = input_lr_seam_pair_2x8x1_c16[(8 * 16) + index];
    }
}

extern "C" void apusr_middle_core_front_c16_to_c32_i8(
    const signed char* input_cell_16x8_c16,
    signed char* output_cell_16x8_c32)
{
    for (int pixel = 0; pixel < apusr_kCellRows * apusr_kCellWidth; ++pixel) {
        for (int channel = 0; channel < 32; ++channel) {
            const int input_index = pixel * 16 + (channel & 15);
            const int output_index = apusr_middle_core_m4x8_offset_for_pixel(pixel, channel, 4);
            output_cell_16x8_c32[output_index] = input_cell_16x8_c16[input_index];
        }
    }
}

extern "C" void apusr_middle_core_encoder_c32_to_c64_i8(
    const signed char* input_cell_16x8_c32,
    signed char* output_cell_16x8_c64)
{
    for (int pixel_base = 0; pixel_base < apusr_kCellRows * apusr_kCellWidth; pixel_base += apusr_kM4x8M) {
        for (int chunk = 0; chunk < 8; ++chunk) {
            const int input_base = apusr_middle_core_m4x8_offset(pixel_base, chunk & 3, 4);
            const int output_base = apusr_middle_core_m4x8_offset(pixel_base, chunk, 8);
            for (int lane = 0; lane < apusr_kM4x8Elements; ++lane) {
                output_cell_16x8_c64[output_base + lane] = input_cell_16x8_c32[input_base + lane];
            }
        }
    }
}

extern "C" void apusr_middle_core_bottleneck_c64_to_c32_i8(
    const signed char* input_cell_16x8_c64,
    signed char* output_cell_16x8_c32)
{
    for (int pixel_base = 0; pixel_base < apusr_kCellRows * apusr_kCellWidth; pixel_base += apusr_kM4x8M) {
        for (int chunk = 0; chunk < 4; ++chunk) {
            const int input_base = apusr_middle_core_m4x8_offset(pixel_base, chunk, 8);
            const int output_base = apusr_middle_core_m4x8_offset(pixel_base, chunk, 4);
            for (int lane = 0; lane < apusr_kM4x8Elements; ++lane) {
                output_cell_16x8_c32[output_base + lane] = input_cell_16x8_c64[input_base + lane];
            }
        }
    }
}

extern "C" void apusr_middle_core_decoder_c32_to_c16_i8(
    const signed char* input_cell_16x8_c32,
    signed char* output_cell_16x8_c16)
{
    for (int pixel = 0; pixel < apusr_kCellRows * apusr_kCellWidth; ++pixel) {
        for (int channel = 0; channel < 16; ++channel) {
            const int input_index = apusr_middle_core_m4x8_offset_for_pixel(pixel, channel, 4);
            const int output_index = pixel * 16 + channel;
            output_cell_16x8_c16[output_index] = input_cell_16x8_c32[input_index];
        }
    }
}

namespace {

inline signed char apusr_middle_core_saturate_i8(const int value)
{
    if (value > 127) {
        return 127;
    }
    if (value < -128) {
        return -128;
    }
    return static_cast<signed char>(value);
}

template <int kCellColumns>
void apusr_middle_core_front_c16_rows_to_c32_row_impl(
    const signed char* row_above_c16,
    const signed char* row_center_c16,
    const signed char* row_below_c16,
    signed char* output_row_c32,
    const int edge_flags)
{
    constexpr int kCellWidth = 16;
    constexpr int kCellRows = 8;
    constexpr int kC16 = 16;
    constexpr int kC32 = 32;
    constexpr int kPixelsPerRow = kCellColumns * kCellWidth;
    constexpr int kFrontEdgeTop = 1 << 2;
    constexpr int kFrontEdgeBottom = 1 << 3;

    const bool top_frame_edge = (edge_flags & kFrontEdgeTop) != 0;
    const bool bottom_frame_edge = (edge_flags & kFrontEdgeBottom) != 0;

    for (int y = 0; y < kCellRows; ++y) {
        for (int x = 0; x < kPixelsPerRow; ++x) {
            const int center_base = ((y * kPixelsPerRow) + x) * kC16;
            const int out_base = ((y * kPixelsPerRow) + x) * kC32;
            const int above_base = (y == 0) ? (((kCellRows - 1) * kPixelsPerRow) + x) * kC16 : (((y - 1) * kPixelsPerRow) + x) * kC16;
            const int below_base = (y == kCellRows - 1) ? x * kC16 : (((y + 1) * kPixelsPerRow) + x) * kC16;

            for (int channel = 0; channel < kC16; ++channel) {
                const int center = row_center_c16[center_base + channel];
                const int above = (top_frame_edge && y == 0) ? 0 : ((y == 0) ? row_above_c16[above_base + channel] : row_center_c16[above_base + channel]);
                const int below = (bottom_frame_edge && y == kCellRows - 1) ? 0 : ((y == kCellRows - 1) ? row_below_c16[below_base + channel] : row_center_c16[below_base + channel]);
                const int vertical_smooth = (above + center + center + below) >> 2;
                const int vertical_delta = below - above;
                output_row_c32[out_base + channel] = apusr_middle_core_saturate_i8(vertical_smooth);
                output_row_c32[out_base + kC16 + channel] = apusr_middle_core_saturate_i8(vertical_delta);
            }
        }
    }
}

template <int kCellColumns>
void apusr_middle_core_c16_row_copy_extract_lr_seams_impl(
    const signed char* input_row_c16,
    signed char* output_row_c16,
    signed char* output_left_seam_c16,
    signed char* output_right_seam_c16)
{
    constexpr int kCellWidth = 16;
    constexpr int kCellRows = 8;
    constexpr int kC16 = 16;
    constexpr int kPixelsPerRow = kCellColumns * kCellWidth;

    for (int y = 0; y < kCellRows; ++y) {
        for (int x = 0; x < kPixelsPerRow; ++x) {
            const int row_base = ((y * kPixelsPerRow) + x) * kC16;
            for (int channel = 0; channel < kC16; ++channel) {
                const signed char value = input_row_c16[row_base + channel];
                output_row_c16[row_base + channel] = value;
                if (x == 0) {
                    output_left_seam_c16[(y * kC16) + channel] = value;
                }
                if (x == kPixelsPerRow - 1) {
                    output_right_seam_c16[(y * kC16) + channel] = value;
                }
            }
        }
    }
}

template <int kCellColumns>
inline int apusr_middle_core_load_c16_spatial_tap(
    const signed char* row_above_c16,
    const signed char* row_center_c16,
    const signed char* row_below_c16,
    const signed char* left_above_c16,
    const signed char* left_center_c16,
    const signed char* left_below_c16,
    const signed char* right_above_c16,
    const signed char* right_center_c16,
    const signed char* right_below_c16,
    const int y,
    const int x,
    const int vertical_delta,
    const int channel,
    const bool left_frame_edge,
    const bool right_frame_edge,
    const bool top_frame_edge,
    const bool bottom_frame_edge)
{
    constexpr int kCellWidth = 16;
    constexpr int kCellRows = 8;
    constexpr int kC16 = 16;
    constexpr int kPixelsPerRow = kCellColumns * kCellWidth;

    const signed char* row = row_center_c16;
    const signed char* left = left_center_c16;
    const signed char* right = right_center_c16;
    int source_y = y;

    if (vertical_delta < 0) {
        if (y == 0) {
            if (top_frame_edge) {
                return 0;
            }
            row = row_above_c16;
            left = left_above_c16;
            right = right_above_c16;
            source_y = kCellRows - 1;
        } else {
            source_y = y - 1;
        }
    } else if (vertical_delta > 0) {
        if (y == kCellRows - 1) {
            if (bottom_frame_edge) {
                return 0;
            }
            row = row_below_c16;
            left = left_below_c16;
            right = right_below_c16;
            source_y = 0;
        } else {
            source_y = y + 1;
        }
    }

    if (x < 0) {
        return left_frame_edge ? 0 : left[(source_y * kC16) + channel];
    }
    if (x >= kPixelsPerRow) {
        return right_frame_edge ? 0 : right[(source_y * kC16) + channel];
    }
    return row[((source_y * kPixelsPerRow) + x) * kC16 + channel];
}

template <int kCellColumns>
void apusr_middle_core_front_c16_rows_lr_to_c32_row_impl(
    const signed char* row_above_c16,
    const signed char* row_center_c16,
    const signed char* row_below_c16,
    const signed char* left_above_c16,
    const signed char* left_center_c16,
    const signed char* left_below_c16,
    const signed char* right_above_c16,
    const signed char* right_center_c16,
    const signed char* right_below_c16,
    signed char* output_row_c32,
    const int edge_flags)
{
    constexpr int kCellWidth = 16;
    constexpr int kCellRows = 8;
    constexpr int kC16 = 16;
    constexpr int kC32 = 32;
    constexpr int kPixelsPerRow = kCellColumns * kCellWidth;
    constexpr int kFrontEdgeLeft = 1 << 0;
    constexpr int kFrontEdgeRight = 1 << 1;
    constexpr int kFrontEdgeTop = 1 << 2;
    constexpr int kFrontEdgeBottom = 1 << 3;

    const bool left_frame_edge = (edge_flags & kFrontEdgeLeft) != 0;
    const bool right_frame_edge = (edge_flags & kFrontEdgeRight) != 0;
    const bool top_frame_edge = (edge_flags & kFrontEdgeTop) != 0;
    const bool bottom_frame_edge = (edge_flags & kFrontEdgeBottom) != 0;

    for (int y = 0; y < kCellRows; ++y) {
        for (int x = 0; x < kPixelsPerRow; ++x) {
            const int out_base = ((y * kPixelsPerRow) + x) * kC32;
            for (int channel = 0; channel < kC16; ++channel) {
                const int center = apusr_middle_core_load_c16_spatial_tap<kCellColumns>(
                    row_above_c16, row_center_c16, row_below_c16,
                    left_above_c16, left_center_c16, left_below_c16,
                    right_above_c16, right_center_c16, right_below_c16,
                    y, x, 0, channel,
                    left_frame_edge, right_frame_edge, top_frame_edge, bottom_frame_edge);
                const int above = apusr_middle_core_load_c16_spatial_tap<kCellColumns>(
                    row_above_c16, row_center_c16, row_below_c16,
                    left_above_c16, left_center_c16, left_below_c16,
                    right_above_c16, right_center_c16, right_below_c16,
                    y, x, -1, channel,
                    left_frame_edge, right_frame_edge, top_frame_edge, bottom_frame_edge);
                const int below = apusr_middle_core_load_c16_spatial_tap<kCellColumns>(
                    row_above_c16, row_center_c16, row_below_c16,
                    left_above_c16, left_center_c16, left_below_c16,
                    right_above_c16, right_center_c16, right_below_c16,
                    y, x, 1, channel,
                    left_frame_edge, right_frame_edge, top_frame_edge, bottom_frame_edge);
                const int left = apusr_middle_core_load_c16_spatial_tap<kCellColumns>(
                    row_above_c16, row_center_c16, row_below_c16,
                    left_above_c16, left_center_c16, left_below_c16,
                    right_above_c16, right_center_c16, right_below_c16,
                    y, x - 1, 0, channel,
                    left_frame_edge, right_frame_edge, top_frame_edge, bottom_frame_edge);
                const int right = apusr_middle_core_load_c16_spatial_tap<kCellColumns>(
                    row_above_c16, row_center_c16, row_below_c16,
                    left_above_c16, left_center_c16, left_below_c16,
                    right_above_c16, right_center_c16, right_below_c16,
                    y, x + 1, 0, channel,
                    left_frame_edge, right_frame_edge, top_frame_edge, bottom_frame_edge);

                const int spatial_smooth = (above + below + left + right + (center * 4)) >> 3;
                const int spatial_delta = ((right - left) + (below - above)) >> 1;
                output_row_c32[out_base + channel] = apusr_middle_core_saturate_i8(spatial_smooth);
                output_row_c32[out_base + kC16 + channel] = apusr_middle_core_saturate_i8(spatial_delta);
            }
        }
    }
}

template <int kCellColumns>
void apusr_middle_core_front_c16_window_to_c32_cell_impl(
    const signed char* cell_above_c16,
    const signed char* cell_center_c16,
    const signed char* cell_below_c16,
    signed char* output_cell_c32,
    const int edge_flags)
{
    (void)kCellColumns;
    constexpr int kCellWidth = 16;
    constexpr int kCellRows = 8;
    constexpr int kC16 = 16;
    constexpr int kC32 = 32;
    constexpr int kFrontEdgeTop = 1 << 2;
    constexpr int kFrontEdgeBottom = 1 << 3;

    const bool top_frame_edge = (edge_flags & kFrontEdgeTop) != 0;
    const bool bottom_frame_edge = (edge_flags & kFrontEdgeBottom) != 0;

    for (int y = 0; y < kCellRows; ++y) {
        for (int x = 0; x < kCellWidth; ++x) {
            const int center_base = ((y * kCellWidth) + x) * kC16;
            const int output_base = ((y * kCellWidth) + x) * kC32;
            const int above_base = (y == 0) ? (((kCellRows - 1) * kCellWidth) + x) * kC16 : (((y - 1) * kCellWidth) + x) * kC16;
            const int below_base = (y == kCellRows - 1) ? x * kC16 : (((y + 1) * kCellWidth) + x) * kC16;

            for (int channel = 0; channel < kC16; ++channel) {
                const int center = cell_center_c16[center_base + channel];
                const int above = (top_frame_edge && y == 0) ? 0 : ((y == 0) ? cell_above_c16[above_base + channel] : cell_center_c16[above_base + channel]);
                const int below = (bottom_frame_edge && y == kCellRows - 1) ? 0 : ((y == kCellRows - 1) ? cell_below_c16[below_base + channel] : cell_center_c16[below_base + channel]);
                output_cell_c32[output_base + channel] = static_cast<signed char>(center);
                output_cell_c32[output_base + kC16 + channel] = apusr_middle_core_saturate_i8(below - above);
            }
        }
    }
}

template <int kCellColumns>
void apusr_middle_core_encoder_c32_to_c16_snapshot_cell_impl(
    const signed char* input_cell_c32,
    signed char* output_cell_c16)
{
    (void)kCellColumns;
    constexpr int kCellWidth = 16;
    constexpr int kCellRows = 8;
    constexpr int kC16 = 16;
    constexpr int kC32 = 32;

    for (int y = 0; y < kCellRows; ++y) {
        for (int x = 0; x < kCellWidth; ++x) {
            const int input_base = ((y * kCellWidth) + x) * kC32;
            const int output_base = ((y * kCellWidth) + x) * kC16;
            for (int channel = 0; channel < kC16; ++channel) {
                output_cell_c16[output_base + channel] = input_cell_c32[input_base + channel];
            }
        }
    }
}

template <int kCellColumns>
void apusr_middle_core_c32_row_to_c16_row_impl(const signed char* input_row_c32, signed char* output_row_c16)
{
    constexpr int kCellWidth = 16;
    constexpr int kCellRows = 8;
    constexpr int kC16 = 16;
    constexpr int kC32 = 32;
    constexpr int kPixelsPerRow = kCellColumns * kCellWidth;

    for (int y = 0; y < kCellRows; ++y) {
        for (int x = 0; x < kPixelsPerRow; ++x) {
            const int in_base = ((y * kPixelsPerRow) + x) * kC32;
            const int out_base = ((y * kPixelsPerRow) + x) * kC16;
            for (int channel = 0; channel < kC16; ++channel) {
                output_row_c16[out_base + channel] = input_row_c32[in_base + channel];
            }
        }
    }
}

} // namespace

#define APUSR_DEFINE_FRONT_C16_ROW_KERNELS(CELLS) \
extern "C" void apusr_middle_core_front_c16_rows_to_c32_row_##CELLS##c_i8( \
    const signed char* row_above_c16, \
    const signed char* row_center_c16, \
    const signed char* row_below_c16, \
    signed char* output_row_c32, \
    const int edge_flags) \
{ \
    apusr_middle_core_front_c16_rows_to_c32_row_impl<CELLS>(row_above_c16, row_center_c16, row_below_c16, output_row_c32, edge_flags); \
} \
extern "C" void apusr_middle_core_c16_row_copy_extract_lr_seams_##CELLS##c_i8( \
    const signed char* input_row_c16, \
    signed char* output_row_c16, \
    signed char* output_left_seam_c16, \
    signed char* output_right_seam_c16) \
{ \
    apusr_middle_core_c16_row_copy_extract_lr_seams_impl<CELLS>(input_row_c16, output_row_c16, output_left_seam_c16, output_right_seam_c16); \
} \
extern "C" void apusr_middle_core_front_c16_rows_lr_to_c32_row_##CELLS##c_i8( \
    const signed char* row_above_c16, \
    const signed char* row_center_c16, \
    const signed char* row_below_c16, \
    const signed char* left_above_c16, \
    const signed char* left_center_c16, \
    const signed char* left_below_c16, \
    const signed char* right_above_c16, \
    const signed char* right_center_c16, \
    const signed char* right_below_c16, \
    signed char* output_row_c32, \
    const int edge_flags) \
{ \
    apusr_middle_core_front_c16_rows_lr_to_c32_row_impl<CELLS>(row_above_c16, row_center_c16, row_below_c16, left_above_c16, left_center_c16, left_below_c16, right_above_c16, right_center_c16, right_below_c16, output_row_c32, edge_flags); \
} \
extern "C" void apusr_middle_core_front_c16_window_to_c32_cell_##CELLS##c_i8( \
    const signed char* cell_above_c16, \
    const signed char* cell_center_c16, \
    const signed char* cell_below_c16, \
    signed char* output_cell_c32, \
    const int edge_flags) \
{ \
    apusr_middle_core_front_c16_window_to_c32_cell_impl<CELLS>(cell_above_c16, cell_center_c16, cell_below_c16, output_cell_c32, edge_flags); \
} \
extern "C" void apusr_middle_core_encoder_c32_to_c16_snapshot_cell_##CELLS##c_i8( \
    const signed char* input_cell_c32, \
    signed char* output_cell_c16) \
{ \
    apusr_middle_core_encoder_c32_to_c16_snapshot_cell_impl<CELLS>(input_cell_c32, output_cell_c16); \
} \
extern "C" void apusr_middle_core_c32_row_to_c16_row_##CELLS##c_i8(const signed char* input_row_c32, signed char* output_row_c16) \
{ \
    apusr_middle_core_c32_row_to_c16_row_impl<CELLS>(input_row_c32, output_row_c16); \
}

APUSR_DEFINE_FRONT_C16_ROW_KERNELS(7)
APUSR_DEFINE_FRONT_C16_ROW_KERNELS(8)
APUSR_DEFINE_FRONT_C16_ROW_KERNELS(15)
APUSR_DEFINE_FRONT_C16_ROW_KERNELS(30)

#undef APUSR_DEFINE_FRONT_C16_ROW_KERNELS
