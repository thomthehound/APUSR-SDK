// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_tokens.h>

#include <algorithm>
#include <limits>

namespace {

[[nodiscard]] std::uint32_t ceilHalf(const std::uint32_t value)
{
    return (value + 1u) / 2u;
}

[[nodiscard]] bool isPackageLayout(const apusr::TokenLayout layout)
{
    return layout == apusr::TokenLayout::PackageMajorC16 ||
        layout == apusr::TokenLayout::LaneMajorPackageC16;
}

[[nodiscard]] bool isCellLayout(const apusr::TokenLayout layout)
{
    return layout == apusr::TokenLayout::LaneMajorCellC16 ||
        layout == apusr::TokenLayout::CellM4x8C16;
}

[[nodiscard]] bool isZeroGuardedRasterLayout(const apusr::TokenLayout layout)
{
    return layout == apusr::TokenLayout::ZeroGuardedRasterNhwcC16;
}

} // namespace

namespace apusr {

std::uint32_t elementSizeBytes(const TokenElementType elementType)
{
    switch (elementType) {
    case TokenElementType::Int8:
    case TokenElementType::UInt8:
        return 1;
    case TokenElementType::Float16:
    case TokenElementType::BFloat16:
        return 2;
    case TokenElementType::Float32:
        return 4;
    }
    return 0;
}

std::uint64_t alignUp(const std::uint64_t value, const std::uint64_t alignment)
{
    if (alignment == 0) {
        return value;
    }
    const std::uint64_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

std::uint64_t rawTensorBytes(const TokenShape& shape)
{
    const std::uint64_t elementBytes = elementSizeBytes(shape.elementType);
    return static_cast<std::uint64_t>(shape.width) * shape.height * shape.channels * elementBytes;
}

std::uint32_t tokenRowPixelAlignment(const TokenShape& shape)
{
    if (isPackageLayout(shape.layout)) {
        return kFsr4MiddleCorePackageWidth;
    }
    if (isCellLayout(shape.layout)) {
        return kFsr4MiddleCoreCellWidth;
    }

    const std::uint64_t bytesPerPixel = static_cast<std::uint64_t>(shape.channels) * elementSizeBytes(shape.elementType);
    if (bytesPerPixel == 0 || bytesPerPixel > kTokenBytes) {
        return 1;
    }
    const std::uint64_t pixels = kTokenBytes / bytesPerPixel;
    return pixels == 0 ? 1u : static_cast<std::uint32_t>(pixels);
}

std::uint32_t tokenPaddedRowWidth(const TokenShape& shape)
{
    const std::uint32_t paddedWidth = static_cast<std::uint32_t>(alignUp(shape.width, tokenRowPixelAlignment(shape)));
    return isZeroGuardedRasterLayout(shape.layout) ? paddedWidth + 2u : paddedWidth;
}

std::uint32_t tokenPaddedRowCount(const TokenShape& shape)
{
    if (isPackageLayout(shape.layout)) {
        return static_cast<std::uint32_t>(alignUp(shape.height, kFsr4MiddleCorePackageRows));
    }
    if (isCellLayout(shape.layout)) {
        return static_cast<std::uint32_t>(alignUp(shape.height, kFsr4MiddleCoreCellRows));
    }
    if (isZeroGuardedRasterLayout(shape.layout)) {
        return shape.height + 2u;
    }
    return shape.height;
}

std::uint32_t tokenPackageColumnCount(const TokenShape& shape)
{
    if (!isPackageLayout(shape.layout)) {
        return 0;
    }
    return tokenPaddedRowWidth(shape) / kFsr4MiddleCorePackageWidth;
}

std::uint32_t tokenPackageRowCount(const TokenShape& shape)
{
    if (!isPackageLayout(shape.layout)) {
        return 0;
    }
    return tokenPaddedRowCount(shape) / kFsr4MiddleCorePackageRows;
}

std::uint32_t tokenCellColumnCount(const TokenShape& shape)
{
    if (!isCellLayout(shape.layout)) {
        return 0;
    }
    return tokenPaddedRowWidth(shape) / kFsr4MiddleCoreCellWidth;
}

std::uint32_t tokenCellRowCount(const TokenShape& shape)
{
    if (!isCellLayout(shape.layout)) {
        return 0;
    }
    return tokenPaddedRowCount(shape) / kFsr4MiddleCoreCellRows;
}

std::uint32_t tokenTransportTileColumnCount(const TokenShape& shape)
{
    if (isPackageLayout(shape.layout)) {
        return tokenPackageColumnCount(shape);
    }
    if (isCellLayout(shape.layout)) {
        return tokenCellColumnCount(shape);
    }
    return 0;
}

std::uint32_t tokenTransportTileRowCount(const TokenShape& shape)
{
    if (isPackageLayout(shape.layout)) {
        return tokenPackageRowCount(shape);
    }
    if (isCellLayout(shape.layout)) {
        return tokenCellRowCount(shape);
    }
    return 0;
}

std::uint32_t tokenTransportLaneGroupCount(const TokenShape& shape)
{
    if (shape.layout != TokenLayout::LaneMajorPackageC16 &&
        shape.layout != TokenLayout::LaneMajorCellC16) {
        return 1;
    }
    return std::max(shape.laneGroups, 1u);
}

std::uint32_t tokenPackageLaneGroupCount(const TokenShape& shape)
{
    return tokenTransportLaneGroupCount(shape);
}

std::uint64_t tokenValidRowBytes(const TokenShape& shape)
{
    const std::uint64_t elementBytes = elementSizeBytes(shape.elementType);
    return static_cast<std::uint64_t>(shape.width) * shape.channels * elementBytes;
}

std::uint64_t tokenRowPitchBytes(const TokenShape& shape)
{
    const std::uint64_t elementBytes = elementSizeBytes(shape.elementType);
    return static_cast<std::uint64_t>(tokenPaddedRowWidth(shape)) * shape.channels * elementBytes;
}

std::uint64_t tokenSlabBytes(const TokenShape& shape)
{
    return tokenRowPitchBytes(shape) * tokenPaddedRowCount(shape);
}

std::uint64_t tokenRowBytes(const TokenShape& shape)
{
    return tokenRowPitchBytes(shape);
}

std::uint32_t tokenBandCount(const TokenShape& shape, const std::uint32_t bandRows)
{
    if (shape.height == 0 || bandRows == 0) {
        return 0;
    }
    return (shape.height + bandRows - 1) / bandRows;
}

std::uint32_t tokenLookaheadBands(const std::uint32_t lookaheadRows, const std::uint32_t bandRows)
{
    if (lookaheadRows == 0 || bandRows == 0) {
        return 0;
    }
    return (lookaheadRows + bandRows - 1) / bandRows;
}

TokenShape makeTokenBandShape(const TokenShape& fullShape, const std::uint32_t rowCount)
{
    TokenShape bandShape = fullShape;
    bandShape.height = rowCount;
    return bandShape;
}

TokenStreamPlan makeTokenStreamPlan(
    const TokenBoundary& inputBoundary,
    const TokenBoundary& outputBoundary,
    const std::uint32_t bandRows,
    const std::uint32_t inputHaloRows,
    const std::uint32_t outputHaloRows,
    const std::uint32_t minImportLookaheadRows,
    const std::uint32_t maxSpatialKernelHeight,
    const std::uint32_t maxSpatialKernelWidth,
    const TokenStreamMode mode)
{
    TokenStreamPlan plan{};
    plan.mode = mode;
    plan.bandRows = bandRows;
    plan.inputHaloRows = inputHaloRows;
    plan.outputHaloRows = outputHaloRows;
    plan.bandCount = tokenBandCount(outputBoundary.shape, bandRows);
    plan.minImportLookaheadRows = minImportLookaheadRows;
    plan.minImportLookaheadBands = tokenLookaheadBands(minImportLookaheadRows, bandRows);
    plan.maxSpatialKernelHeight = maxSpatialKernelHeight;
    plan.maxSpatialKernelWidth = maxSpatialKernelWidth;
    plan.npuOwnsInternalStreamState = mode == TokenStreamMode::ContinuousFrameStream;

    for (std::uint32_t bandIndex = 0; bandIndex < plan.bandCount; ++bandIndex) {
        const TokenBandDesc band = makeTokenBandDesc(inputBoundary, outputBoundary, plan, bandIndex);
        plan.maxInputBandBytes = std::max(plan.maxInputBandBytes, band.inputByteSize);
        plan.maxOutputBandBytes = std::max(plan.maxOutputBandBytes, band.outputByteSize);
    }

    return plan;
}

TokenBandDesc makeTokenBandDesc(
    const TokenBoundary& inputBoundary,
    const TokenBoundary& outputBoundary,
    const TokenStreamPlan& streamPlan,
    const std::uint32_t bandIndex)
{
    const std::uint32_t logicalFirstRow = bandIndex * streamPlan.bandRows;
    const std::uint32_t remainingOutputRows =
        logicalFirstRow < outputBoundary.shape.height ? outputBoundary.shape.height - logicalFirstRow : 0;
    const std::uint32_t logicalRowCount = std::min(streamPlan.bandRows, remainingOutputRows);

    const std::uint32_t inputFirstRow =
        logicalFirstRow > streamPlan.inputHaloRows ? logicalFirstRow - streamPlan.inputHaloRows : 0;
    const std::uint32_t requestedInputLastRow = logicalFirstRow + logicalRowCount + streamPlan.inputHaloRows;
    const std::uint32_t inputLastRow = std::min(requestedInputLastRow, inputBoundary.shape.height);
    const std::uint32_t inputRowCount = inputLastRow > inputFirstRow ? inputLastRow - inputFirstRow : 0;

    const std::uint32_t egressFirstRow =
        logicalFirstRow > streamPlan.outputHaloRows ? logicalFirstRow - streamPlan.outputHaloRows : 0;
    const std::uint32_t requestedEgressLastRow = logicalFirstRow + logicalRowCount + streamPlan.outputHaloRows;
    const std::uint32_t egressLastRow = std::min(requestedEgressLastRow, outputBoundary.shape.height);
    const std::uint32_t egressRowCount = egressLastRow > egressFirstRow ? egressLastRow - egressFirstRow : 0;

    TokenBandDesc band{};
    band.bandIndex = bandIndex;
    band.bandCount = streamPlan.bandCount;
    band.logicalFirstRow = logicalFirstRow;
    band.logicalRowCount = logicalRowCount;
    band.inputFirstRow = inputFirstRow;
    band.inputRowCount = inputRowCount;
    band.outputFirstRow = logicalFirstRow;
    band.outputRowCount = logicalRowCount;
    band.egressFirstRow = egressFirstRow;
    band.egressRowCount = egressRowCount;
    band.firstBandInFrame = bandIndex == 0;
    band.lastBandInFrame = bandIndex + 1 == streamPlan.bandCount;
    band.inputShape = makeTokenBandShape(inputBoundary.shape, inputRowCount);
    band.outputShape = makeTokenBandShape(outputBoundary.shape, egressRowCount);
    band.inputByteSize = tokenSlabBytes(band.inputShape);
    band.outputByteSize = tokenSlabBytes(band.outputShape);
    return band;
}

TokenShape makeFsr4MiddleBoundaryShapeFromModelOutput(const DisplaySize modelOutputSize)
{
    return TokenShape{
        .width = ceilHalf(modelOutputSize.width),
        .height = ceilHalf(modelOutputSize.height),
        .channels = kFsr4MiddleCoreChannels,
        .elementType = TokenElementType::Int8,
        .layout = TokenLayout::RasterNhwcC16,
    };
}

} // namespace apusr
