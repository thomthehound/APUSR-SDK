// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>

#include <cstdint>
#include <string>

namespace apusr {

inline constexpr std::uint32_t kTokenBytes = 256;
inline constexpr std::uint32_t kFsr4MiddleCoreChannels = 16;
inline constexpr std::uint32_t kDefaultTokenBandRows = 16;
inline constexpr std::uint32_t kFsr4MiddleCoreCellWidth = 16;
inline constexpr std::uint32_t kFsr4MiddleCoreCellRows = 8;
inline constexpr std::uint32_t kFsr4MiddleCoreCellBytes =
    kFsr4MiddleCoreCellWidth * kFsr4MiddleCoreCellRows * kFsr4MiddleCoreChannels;
inline constexpr std::uint32_t kFsr4MiddleCorePackageWidth = 64;
inline constexpr std::uint32_t kFsr4MiddleCorePackageRows = 32;
inline constexpr std::uint32_t kFsr4MiddleCorePackageBytes =
    kFsr4MiddleCorePackageWidth * kFsr4MiddleCorePackageRows * kFsr4MiddleCoreChannels;

enum class TokenElementType : std::uint32_t {
    Int8,
    UInt8,
    Float16,
    BFloat16,
    Float32,
};

enum class TokenLayout : std::uint32_t {
    RasterNhwcC16 = 0,
    PackageMajorC16 = 1,
    LaneMajorPackageC16 = 2,
    LaneMajorCellC16 = 3,
    ZeroGuardedRasterNhwcC16 = 4,
    CellM4x8C16 = 5,
};

enum class TokenStreamMode : std::uint32_t {
    ContinuousFrameStream,
    IndependentBandDebug,
};

struct TokenShape {
    // width and height are logical tensor pixels. Token-row helpers derive the
    // physical padded row pitch required by the GPU/NPU stream ABI.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    TokenElementType elementType = TokenElementType::Int8;
    TokenLayout layout = TokenLayout::RasterNhwcC16;
    std::uint32_t laneGroups = 1;
};

struct TokenBoundary {
    std::string name;
    TokenShape shape;
    std::uint64_t byteOffset = 0;
    std::uint64_t byteSize = 0;
};

struct TokenStreamPlan {
    TokenStreamMode mode = TokenStreamMode::ContinuousFrameStream;
    std::uint32_t bandRows = kDefaultTokenBandRows;
    std::uint32_t inputHaloRows = 0;
    std::uint32_t outputHaloRows = 0;
    std::uint32_t bandCount = 0;
    std::uint32_t minImportLookaheadRows = 0;
    std::uint32_t minImportLookaheadBands = 0;
    std::uint32_t maxSpatialKernelHeight = 1;
    std::uint32_t maxSpatialKernelWidth = 1;
    std::uint64_t maxInputBandBytes = 0;
    std::uint64_t maxOutputBandBytes = 0;
    bool npuOwnsInternalStreamState = true;
};

struct TokenBandDesc {
    std::uint32_t bandIndex = 0;
    std::uint32_t bandCount = 0;
    std::uint32_t logicalFirstRow = 0;
    std::uint32_t logicalRowCount = 0;
    std::uint32_t inputFirstRow = 0;
    std::uint32_t inputRowCount = 0;
    std::uint32_t outputFirstRow = 0;
    std::uint32_t outputRowCount = 0;
    std::uint32_t egressFirstRow = 0;
    std::uint32_t egressRowCount = 0;
    bool firstBandInFrame = false;
    bool lastBandInFrame = false;
    TokenShape inputShape;
    TokenShape outputShape;
    std::uint64_t inputByteSize = 0;
    std::uint64_t outputByteSize = 0;
};

[[nodiscard]] std::uint32_t elementSizeBytes(TokenElementType elementType);
[[nodiscard]] std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment);
[[nodiscard]] std::uint64_t rawTensorBytes(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenRowPixelAlignment(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenPaddedRowWidth(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenPaddedRowCount(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenPackageColumnCount(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenPackageRowCount(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenCellColumnCount(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenCellRowCount(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenTransportTileColumnCount(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenTransportTileRowCount(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenTransportLaneGroupCount(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenPackageLaneGroupCount(const TokenShape& shape);
[[nodiscard]] std::uint64_t tokenValidRowBytes(const TokenShape& shape);
[[nodiscard]] std::uint64_t tokenRowPitchBytes(const TokenShape& shape);
[[nodiscard]] std::uint64_t tokenSlabBytes(const TokenShape& shape);
[[nodiscard]] std::uint64_t tokenRowBytes(const TokenShape& shape);
[[nodiscard]] std::uint32_t tokenBandCount(const TokenShape& shape, std::uint32_t bandRows);
[[nodiscard]] std::uint32_t tokenLookaheadBands(std::uint32_t lookaheadRows, std::uint32_t bandRows);
[[nodiscard]] TokenShape makeTokenBandShape(const TokenShape& fullShape, std::uint32_t rowCount);
[[nodiscard]] TokenStreamPlan makeTokenStreamPlan(
    const TokenBoundary& inputBoundary,
    const TokenBoundary& outputBoundary,
    std::uint32_t bandRows,
    std::uint32_t inputHaloRows,
    std::uint32_t outputHaloRows = 0,
    std::uint32_t minImportLookaheadRows = 0,
    std::uint32_t maxSpatialKernelHeight = 1,
    std::uint32_t maxSpatialKernelWidth = 1,
    TokenStreamMode mode = TokenStreamMode::ContinuousFrameStream);
[[nodiscard]] TokenBandDesc makeTokenBandDesc(
    const TokenBoundary& inputBoundary,
    const TokenBoundary& outputBoundary,
    const TokenStreamPlan& streamPlan,
    std::uint32_t bandIndex);
// modelOutputSize is already in FSR4 model coordinates: active output aligned to 8 pixels.
[[nodiscard]] TokenShape makeFsr4MiddleBoundaryShapeFromModelOutput(DisplaySize modelOutputSize);

} // namespace apusr
