// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstddef>
#include <cstdint>

namespace apusr::kernels::middle_core_layout {

inline constexpr std::uint32_t kLayoutAbiVersion = 1;
inline constexpr std::uint32_t kMacroLaneWidthColumns = 2;
inline constexpr std::uint32_t kComputeRows = 4;
inline constexpr std::uint32_t kMinimumComputeColumns = 4;
inline constexpr std::uint32_t kC8GroupChannels = 8;
inline constexpr std::uint32_t kC16SeamChannels = 16;
inline constexpr std::size_t kC16PixelBytes = 16;
inline constexpr std::size_t kAieMlMemoryTileBytes = 512u * 1024u;
inline constexpr std::size_t kMacroLaneMemoryStripBytes = kMacroLaneWidthColumns * kAieMlMemoryTileBytes;

// C32/C64 internal tensors use group -> y -> x -> c8 unless a later measured
// bottleneck justifies a named exception.
inline constexpr std::uint32_t kInternalGroupMajorChunkChannels = kC8GroupChannels;

// Fused downsample/FasterNet stages keep only the rows needed by the following
// 3x3 spatial window.
inline constexpr std::uint32_t kDownsampleRowRingRows = 3;

} // namespace apusr::kernels::middle_core_layout
