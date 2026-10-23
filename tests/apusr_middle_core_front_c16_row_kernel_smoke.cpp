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

int c16Index(int y, int x, int c)
{
    return ((y * kPixelsPerRow) + x) * kC16 + c;
}

int c32Index(int y, int x, int c)
{
    return ((y * kPixelsPerRow) + x) * kC32 + c;
}

} // namespace

int main()
{
    std::vector<signed char> above(kC16RowBytes, 4);
    std::vector<signed char> center(kC16RowBytes, 8);
    std::vector<signed char> below(kC16RowBytes, 12);
    std::vector<signed char> c32(kC32RowBytes, 0);
    std::vector<signed char> collapsed(kC16RowBytes, 0);

    apusr_middle_core_front_c16_rows_to_c32_row_7c_i8(above.data(), center.data(), below.data(), c32.data(), 0);
    require(c32[c32Index(0, 0, 0)] == 7, "row 0 should use the previous cell row when not at the top frame edge");
    require(c32[c32Index(4, 3, 5)] == 8, "middle pixels should use center-row vertical neighbors");
    require(c32[c32Index(7, kPixelsPerRow - 1, 15)] == 9, "row 7 should use the next cell row when not at the bottom frame edge");
    require(c32[c32Index(7, kPixelsPerRow - 1, 31)] == 4, "upper C32 half should carry the vertical delta");

    apusr_middle_core_c32_row_to_c16_row_7c_i8(c32.data(), collapsed.data());
    require(collapsed[c16Index(4, 3, 5)] == c32[c32Index(4, 3, 5)], "C32-to-C16 row collapse should preserve the lower C16 channels");

    apusr_middle_core_front_c16_rows_to_c32_row_7c_i8(above.data(), center.data(), below.data(), c32.data(), 1 << 2);
    require(c32[c32Index(0, 0, 0)] == 6, "top frame edge should zero-pad the missing row above");

    apusr_middle_core_front_c16_rows_to_c32_row_7c_i8(above.data(), center.data(), below.data(), c32.data(), 1 << 3);
    require(c32[c32Index(7, 0, 0)] == 6, "bottom frame edge should zero-pad the missing row below");

    std::fill(above.begin(), above.end(), static_cast<signed char>(-128));
    std::fill(center.begin(), center.end(), static_cast<signed char>(0));
    std::fill(below.begin(), below.end(), static_cast<signed char>(127));
    apusr_middle_core_front_c16_rows_to_c32_row_7c_i8(above.data(), center.data(), below.data(), c32.data(), 0);
    require(c32[c32Index(7, 0, 16)] == 127, "vertical delta should saturate to int8");

    std::cout << "APUSR middle-core front C16 row kernel smoke: vertical row-ring front worker validated\n";
    return 0;
}
