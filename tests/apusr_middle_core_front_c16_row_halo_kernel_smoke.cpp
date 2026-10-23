// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "../kernels/apusr_middle_core_stage1_12_spatial2col/src/aie/apusr_middle_core_c16_cell_copy.cc"

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

constexpr int kCells = 7;
constexpr int kCellWidth = 16;
constexpr int kCellRows = 8;
constexpr int kC16 = 16;
constexpr int kC32 = 32;
constexpr int kPixelsPerRow = kCells * kCellWidth;
constexpr int kC16RowBytes = kPixelsPerRow * kCellRows * kC16;
constexpr int kC32RowBytes = kPixelsPerRow * kCellRows * kC32;
constexpr int kSeamBytes = kCellRows * kC16;

int c16Index(int y, int x, int c)
{
    return ((y * kPixelsPerRow) + x) * kC16 + c;
}

int c32Index(int y, int x, int c)
{
    return ((y * kPixelsPerRow) + x) * kC32 + c;
}

int seamIndex(int y, int c)
{
    return y * kC16 + c;
}

} // namespace

int main()
{
    std::vector<signed char> input(kC16RowBytes, 8);
    std::vector<signed char> copied(kC16RowBytes, 0);
    std::vector<signed char> own_left(kSeamBytes, 0);
    std::vector<signed char> own_right(kSeamBytes, 0);

    input[c16Index(0, 0, 3)] = 11;
    input[c16Index(0, kPixelsPerRow - 1, 3)] = 22;
    apusr_middle_core_c16_row_copy_extract_lr_seams_7c_i8(input.data(), copied.data(), own_left.data(), own_right.data());
    require(copied[c16Index(0, 0, 3)] == 11, "seam extractor should preserve the row payload");
    require(own_left[seamIndex(0, 3)] == 11, "left seam should come from the first owned pixel column");
    require(own_right[seamIndex(0, 3)] == 22, "right seam should come from the last owned pixel column");

    std::vector<signed char> one_cell(kCellWidth * kCellRows * kC16, 0);
    std::vector<signed char> cell_left(kSeamBytes, 0);
    std::vector<signed char> cell_right(kSeamBytes, 0);
    one_cell[((2 * kCellWidth) + 0) * kC16 + 5] = 33;
    one_cell[((2 * kCellWidth) + (kCellWidth - 1)) * kC16 + 5] = 44;
    apusr_middle_core_c16_cell_extract_left_seam_i8(one_cell.data(), cell_left.data());
    apusr_middle_core_c16_cell_extract_right_seam_i8(one_cell.data(), cell_right.data());
    require(cell_left[seamIndex(2, 5)] == 33, "cell left seam should come from the first pixel column");
    require(cell_right[seamIndex(2, 5)] == 44, "cell right seam should come from the last pixel column");

    std::vector<signed char> seam_pair(kSeamBytes * 2, 0);
    std::vector<signed char> pair_left(kSeamBytes, 0);
    std::vector<signed char> pair_right(kSeamBytes, 0);
    seam_pair[seamIndex(1, 6)] = 55;
    seam_pair[kSeamBytes + seamIndex(3, 7)] = 66;
    apusr_middle_core_c16_lr_seam_pair_extract_left_i8(seam_pair.data(), pair_left.data());
    apusr_middle_core_c16_lr_seam_pair_extract_right_i8(seam_pair.data(), pair_right.data());
    require(pair_left[seamIndex(1, 6)] == 55, "joined seam pair should expose the left half");
    require(pair_right[seamIndex(3, 7)] == 66, "joined seam pair should expose the right half");

    std::vector<signed char> above(kC16RowBytes, 4);
    std::vector<signed char> center(kC16RowBytes, 8);
    std::vector<signed char> below(kC16RowBytes, 12);
    std::vector<signed char> left_above(kSeamBytes, 4);
    std::vector<signed char> left_center(kSeamBytes, 16);
    std::vector<signed char> left_below(kSeamBytes, 12);
    std::vector<signed char> right_above(kSeamBytes, 4);
    std::vector<signed char> right_center(kSeamBytes, 24);
    std::vector<signed char> right_below(kSeamBytes, 12);
    std::vector<signed char> c32(kC32RowBytes, 0);

    apusr_middle_core_front_c16_rows_lr_to_c32_row_7c_i8(
        above.data(), center.data(), below.data(),
        left_above.data(), left_center.data(), left_below.data(),
        right_above.data(), right_center.data(), right_below.data(),
        c32.data(), 0);

    require(c32[c32Index(4, 0, 0)] == 9, "left interior seam should feed the 3x3 front smooth path");
    require(c32[c32Index(4, kPixelsPerRow - 1, 0)] == 10, "right interior seam should feed the 3x3 front smooth path");
    require(c32[c32Index(4, kPixelsPerRow - 1, 16)] == 8, "right interior seam should feed the C32 spatial-delta half");

    apusr_middle_core_front_c16_rows_lr_to_c32_row_7c_i8(
        above.data(), center.data(), below.data(),
        left_above.data(), left_center.data(), left_below.data(),
        right_above.data(), right_center.data(), right_below.data(),
        c32.data(), 1 << 0);
    require(c32[c32Index(4, 0, 0)] == 7, "left frame edge should zero-pad instead of consuming the left seam");

    apusr_middle_core_front_c16_rows_lr_to_c32_row_7c_i8(
        above.data(), center.data(), below.data(),
        left_above.data(), left_center.data(), left_below.data(),
        right_above.data(), right_center.data(), right_below.data(),
        c32.data(), 1 << 1);
    require(c32[c32Index(4, kPixelsPerRow - 1, 0)] == 7, "right frame edge should zero-pad instead of consuming the right seam");

    std::fill(left_center.begin(), left_center.end(), static_cast<signed char>(-128));
    center[c16Index(4, 1, 0)] = 127;
    apusr_middle_core_front_c16_rows_lr_to_c32_row_7c_i8(
        above.data(), center.data(), below.data(),
        left_above.data(), left_center.data(), left_below.data(),
        right_above.data(), right_center.data(), right_below.data(),
        c32.data(), 0);
    require(c32[c32Index(4, 0, 16)] == 127, "horizontal seam delta should saturate to int8");

    std::cout << "APUSR middle-core front C16 row halo kernel smoke: horizontal seam path validated\n";
    return 0;
}
