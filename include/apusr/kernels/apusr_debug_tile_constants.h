// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>

namespace apusr::kernels::debug_tile {

inline constexpr std::uint32_t kFilterWidth = 3u;
inline constexpr std::uint32_t kFilterHeight = 3u;
inline constexpr std::uint32_t kInputHaloRows = 1u;
inline constexpr std::uint32_t kInputHaloColumns = 1u;
inline constexpr std::uint32_t kDefaultTokenBandRows = 16u;
inline constexpr std::uint32_t kDefaultTokenRingDepth = 6u;
inline constexpr std::uint32_t kStreamTokenBytes = 256u;
inline constexpr std::uint32_t kModelChannels = 16u;
inline constexpr std::uint32_t kElementBytes = 1u;
inline constexpr std::uint32_t kPixelsPerStreamToken = kStreamTokenBytes / (kModelChannels * kElementBytes);
inline constexpr std::uint32_t kDefaultMicroTileOutputWidth = kPixelsPerStreamToken;
inline constexpr std::uint32_t kDefaultMicroTileOutputRows = 8u;
inline constexpr std::uint32_t kScaleFractionBits = 16u;

inline constexpr std::uint32_t kWorstCaseInputWindowWidth = kDefaultMicroTileOutputWidth + 2u * kInputHaloColumns;
inline constexpr std::uint32_t kWorstCaseInputWindowRows = kDefaultMicroTileOutputRows + 2u * kInputHaloRows;
inline constexpr std::uint32_t kWorstCaseInputWindowBytes =
    kWorstCaseInputWindowWidth * kWorstCaseInputWindowRows * kModelChannels * kElementBytes;
inline constexpr std::uint32_t kWorstCaseOutputTileBytes =
    kDefaultMicroTileOutputWidth * kDefaultMicroTileOutputRows * kModelChannels * kElementBytes;
inline constexpr std::uint32_t kWorstCaseLiveActivationBytes =
    kWorstCaseInputWindowBytes + kWorstCaseOutputTileBytes;

static_assert(kPixelsPerStreamToken == 16, "C16/INT8/256-byte stream token should cover 16 pixels");
static_assert(kWorstCaseInputWindowBytes == 2880, "default input microtile footprint changed unexpectedly");
static_assert(kWorstCaseOutputTileBytes == 2048, "default output microtile footprint changed unexpectedly");
static_assert(kWorstCaseLiveActivationBytes == 4928, "default live activation footprint changed unexpectedly");

} // namespace apusr::kernels::debug_tile
