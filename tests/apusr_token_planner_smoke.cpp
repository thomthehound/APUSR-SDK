// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr/kernels/apusr_token_planner.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using namespace apusr::kernels::planner;

    const AieMlHardwareModel hardware{};
    require(hardware.tileDataMemoryBytes == 65536u, "AIE-ML tile memory model changed");
    require(hardware.conservativeActivationBudgetBytes == 32768u, "planner budget should stay conservative");

    require(pixelsPerStreamToken(streamTokenForChannels(16u)) == 16u, "C16 token width should be 16 pixels for 256-byte tokens");
    require(pixelsPerStreamToken(streamTokenForChannels(32u)) == 8u, "C32 token width should be 8 pixels for 256-byte tokens");
    require(pixelsPerStreamToken(streamTokenForChannels(64u)) == 4u, "C64 token width should be 4 pixels for 256-byte tokens");

    const auto c16_16x8 = evaluateMicroTile(MicroTileCandidate{16u, 8u, 16u, 1u, OperatorShape{3u, 3u, 1u, 1u}}, streamTokenForChannels(16u));
    require(c16_16x8.inputWindowBytes == 2880u, "C16 16x8 input footprint should include 3x3 halo");
    require(c16_16x8.outputBytes == 2048u, "C16 16x8 output footprint changed");
    require(c16_16x8.liveActivationBytes == 4928u, "C16 16x8 live footprint changed");
    require(c16_16x8.fitsConservativeBudget, "C16 16x8 should fit the conservative tile budget");
    require(c16_16x8.outputWidthIsTokenAligned, "C16 16x8 should align to C16 stream tokens");

    const auto c32_16x8 = evaluateMicroTile(MicroTileCandidate{16u, 8u, 32u, 1u, OperatorShape{3u, 3u, 1u, 1u}}, streamTokenForChannels(32u));
    require(c32_16x8.liveActivationBytes == 9856u, "C32 16x8 live footprint changed");
    require(c32_16x8.fitsConservativeBudget, "C32 16x8 should fit the conservative tile budget");
    require(c32_16x8.outputWidthIsTokenAligned, "C32 16x8 should align to C32 stream tokens");

    const auto c64_16x8 = evaluateMicroTile(MicroTileCandidate{16u, 8u, 64u, 1u, OperatorShape{3u, 3u, 1u, 1u}}, streamTokenForChannels(64u));
    require(c64_16x8.liveActivationBytes == 19712u, "C64 16x8 live footprint changed");
    require(c64_16x8.fitsConservativeBudget, "C64 16x8 still fits the conservative activation budget");
    require(c64_16x8.outputWidthIsTokenAligned, "C64 16x8 should align to C64 stream tokens");

    const auto c64_32x8 = evaluateMicroTile(MicroTileCandidate{32u, 8u, 64u, 1u, OperatorShape{3u, 3u, 1u, 1u}}, streamTokenForChannels(64u));
    require(c64_32x8.liveActivationBytes == 38144u, "C64 32x8 live footprint changed");
    require(!c64_32x8.fitsConservativeBudget, "C64 32x8 should not be a safe default candidate");
    require(c64_32x8.fitsTileMemory, "C64 32x8 should fit raw tile memory but exceed the safe budget");

    require(isGoodFirstSpatialCandidate(MicroTileCandidate{16u, 8u, 16u, 1u, OperatorShape{3u, 3u, 1u, 1u}}),
            "first spatial candidate should be C16 16x8");
    require(!isGoodFirstSpatialCandidate(MicroTileCandidate{16u, 8u, 32u, 1u, OperatorShape{3u, 3u, 1u, 1u}}),
            "first spatial candidate should not silently generalize to C32");

    std::cout << "APUSR token planner smoke: C16 first-kernel candidate and C32/C64 pressure validated\n";
    return 0;
}
