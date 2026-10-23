// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>

namespace apusr::kernels::planner {

// Hardware facts used by the first planner pass. These are intentionally small
// in scope: they capture the parts that directly affect token/microtile shape.
struct AieMlHardwareModel {
    std::uint32_t tileDataMemoryBytes = 64u * 1024u;
    std::uint32_t conservativeActivationBudgetBytes = 32u * 1024u;
    std::uint32_t programmerVisibleBankBytes = 16u * 1024u;
    std::uint32_t programmerVisibleBankCount = 4u;
    std::uint32_t int8VectorLanes = 64u;
    std::uint32_t streamBeatBytes = 4u;
};

struct StreamTokenShape {
    std::uint32_t tokenBytes = 256u;
    std::uint32_t channels = 16u;
    std::uint32_t elementBytes = 1u;
};

struct OperatorShape {
    std::uint32_t kernelWidth = 3u;
    std::uint32_t kernelHeight = 3u;
    std::uint32_t strideX = 1u;
    std::uint32_t strideY = 1u;
};

struct MicroTileCandidate {
    std::uint32_t outputWidth = 16u;
    std::uint32_t outputRows = 8u;
    std::uint32_t channels = 16u;
    std::uint32_t elementBytes = 1u;
    OperatorShape op{};
};

struct MicroTileMetrics {
    std::uint32_t inputWindowWidth = 0u;
    std::uint32_t inputWindowRows = 0u;
    std::uint32_t inputWindowBytes = 0u;
    std::uint32_t outputBytes = 0u;
    std::uint32_t liveActivationBytes = 0u;
    std::uint32_t streamTokenWidthPixels = 0u;
    std::uint32_t inputStreamTokens = 0u;
    std::uint32_t outputStreamTokens = 0u;
    bool fitsConservativeBudget = false;
    bool fitsTileMemory = false;
    bool outputWidthIsTokenAligned = false;
};

struct ModelStageShape {
    std::uint32_t pass = 0u;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t channels = 0u;
    std::uint32_t elementBytes = 1u;
    bool hasSpatial3x3 = false;
};

constexpr std::uint32_t ceilDiv(std::uint32_t value, std::uint32_t divisor)
{
    return (value + divisor - 1u) / divisor;
}

constexpr std::uint32_t halo(std::uint32_t kernelExtent)
{
    return kernelExtent / 2u;
}

constexpr std::uint32_t pixelsPerStreamToken(StreamTokenShape shape)
{
    return shape.tokenBytes / (shape.channels * shape.elementBytes);
}

constexpr MicroTileMetrics evaluateMicroTile(
    MicroTileCandidate candidate,
    StreamTokenShape streamToken,
    AieMlHardwareModel hardware = {})
{
    MicroTileMetrics metrics{};
    metrics.inputWindowWidth = candidate.outputWidth + 2u * halo(candidate.op.kernelWidth);
    metrics.inputWindowRows = candidate.outputRows + 2u * halo(candidate.op.kernelHeight);
    metrics.inputWindowBytes = metrics.inputWindowWidth * metrics.inputWindowRows * candidate.channels * candidate.elementBytes;
    metrics.outputBytes = candidate.outputWidth * candidate.outputRows * candidate.channels * candidate.elementBytes;
    metrics.liveActivationBytes = metrics.inputWindowBytes + metrics.outputBytes;
    metrics.streamTokenWidthPixels = pixelsPerStreamToken(streamToken);
    metrics.inputStreamTokens = ceilDiv(metrics.inputWindowWidth * metrics.inputWindowRows, metrics.streamTokenWidthPixels);
    metrics.outputStreamTokens = ceilDiv(candidate.outputWidth * candidate.outputRows, metrics.streamTokenWidthPixels);
    metrics.fitsConservativeBudget = metrics.liveActivationBytes <= hardware.conservativeActivationBudgetBytes;
    metrics.fitsTileMemory = metrics.liveActivationBytes <= hardware.tileDataMemoryBytes;
    metrics.outputWidthIsTokenAligned = (metrics.streamTokenWidthPixels != 0u) &&
        ((candidate.outputWidth % metrics.streamTokenWidthPixels) == 0u);
    return metrics;
}

constexpr StreamTokenShape streamTokenForChannels(std::uint32_t channels, std::uint32_t tokenBytes = 256u)
{
    return StreamTokenShape{tokenBytes, channels, 1u};
}

constexpr MicroTileCandidate candidateForStage(
    ModelStageShape stage,
    std::uint32_t outputWidth,
    std::uint32_t outputRows)
{
    return MicroTileCandidate{outputWidth, outputRows, stage.channels, stage.elementBytes, OperatorShape{3u, 3u, 1u, 1u}};
}

constexpr bool isGoodFirstSpatialCandidate(MicroTileCandidate candidate)
{
    const auto metrics = evaluateMicroTile(candidate, streamTokenForChannels(candidate.channels));
    return candidate.channels == 16u &&
        candidate.elementBytes == 1u &&
        candidate.op.kernelWidth == 3u &&
        candidate.op.kernelHeight == 3u &&
        metrics.fitsConservativeBudget &&
        metrics.outputWidthIsTokenAligned;
}

} // namespace apusr::kernels::planner
