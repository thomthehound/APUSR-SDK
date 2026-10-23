// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstddef>
#include <cstdint>

namespace apusr::kernels::middle_core {

inline constexpr std::uint32_t kKernelAbiVersion = 1;
inline constexpr std::uint32_t kKernelOpcode = 3;

// MLIR-AIE DPU kernels reserve args 0..2 for opcode, instruction BO, and
// instruction count. Arg3 is the resident model-parameter arena. Args 4 and 5
// are the complete frame-slot ingress and egress payloads for the product
// Stage1->12 spatial core. Arg6 is reserved for the long-lived skip-spill
// arena when the selected graph exposes the SkipA/SkipB DDR side channel.
inline constexpr int kOpcodeArgIndex = 0;
inline constexpr int kInstructionBufferArgIndex = 1;
inline constexpr int kInstructionCountArgIndex = 2;
inline constexpr int kResidentParameterBufferArgIndex = 3;
inline constexpr int kInputTokenBufferArgIndex = 4;
inline constexpr int kOutputTokenBufferArgIndex = 5;
inline constexpr int kSkipSpillBufferArgIndex = 6;
inline constexpr int kControlRingBufferArgIndex = -1;

inline constexpr std::uint32_t kStage1To12LaneGroupColumns = 2;
inline constexpr std::uint32_t kStage1To12Stage9ParameterChunks = 4;

inline constexpr std::size_t kStage1To12Stage0InputCellBytes = 2048U;
inline constexpr std::size_t kStage1To12Stage0InputPackageBytes = 32768U;
inline constexpr std::size_t kStage1To12Stage1InputPackageBytes = 46080U;
inline constexpr std::size_t kStage1To12Stage1ParamBytes = 4048U;
inline constexpr std::size_t kStage1To12Stage2ParamBytes = 4048U;
inline constexpr std::size_t kStage1To12Stage3To4ParamBytes = 10000U;
inline constexpr std::size_t kStage1To12Stage2HaloPackageBytes = 46080U;
inline constexpr std::size_t kStage1To12Stage3To4DownsourcePackageBytes = 51200U;
inline constexpr std::size_t kStage1To12Stage3To4OutputPackageBytes = 16384U;
inline constexpr std::size_t kStage1To12Stage5ParamBytes = 8192U;
inline constexpr std::size_t kStage1To12Stage6To7ParamBytes = 36880U;
inline constexpr std::size_t kStage1To12Stage8ParamBytes = 28176U;
inline constexpr std::size_t kStage1To12Stage9ParamChunkBytes = 2224U;
inline constexpr std::size_t kStage1To12Stage10ParamBytes = 7696U;
inline constexpr std::size_t kStage1To12Stage11ParamBytes = 10064U;
inline constexpr std::size_t kStage1To12Stage12ParamBytes = 4048U;
inline constexpr std::size_t kStage1To12ReferencePackageWidth = 64U;
inline constexpr std::size_t kStage1To12ReferencePackageRows = 32U;
inline constexpr std::size_t kStage1To12TransportCellWidth = 16U;
inline constexpr std::size_t kStage1To12TransportCellRows = 8U;
inline constexpr std::size_t kStage1To12DownsampleLadderCellRowAlignment = kStage1To12TransportCellRows * 4U;

struct Stage1To12FrameCapacity {
    std::size_t modelOutputWidth = 0U;
    std::size_t modelOutputRows = 0U;
    std::size_t seamWidth = 0U;
    std::size_t seamRows = 0U;
    std::size_t cellColumns = 0U;
    std::size_t cellRows = 0U;
    std::size_t cellCount = 0U;
    std::size_t frameSlotBytes = 0U;
};

[[nodiscard]] constexpr std::size_t stage1To12CeilHalf(const std::size_t value)
{
    return (value + 1U) / 2U;
}

[[nodiscard]] constexpr std::size_t stage1To12AlignUp(const std::size_t value, const std::size_t alignment)
{
    const std::size_t remainder = alignment == 0U ? 0U : value % alignment;
    return remainder == 0U ? value : value + (alignment - remainder);
}

[[nodiscard]] constexpr Stage1To12FrameCapacity stage1To12FrameCapacityForModelOutput(
    const std::size_t modelOutputWidth,
    const std::size_t modelOutputRows)
{
    const std::size_t seamWidth = stage1To12AlignUp(stage1To12CeilHalf(modelOutputWidth), kStage1To12TransportCellWidth);
    const std::size_t seamRows = stage1To12AlignUp(stage1To12CeilHalf(modelOutputRows), kStage1To12DownsampleLadderCellRowAlignment);
    const std::size_t cellColumns = seamWidth / kStage1To12TransportCellWidth;
    const std::size_t cellRows = seamRows / kStage1To12TransportCellRows;
    const std::size_t cellCount = cellColumns * cellRows;
    return Stage1To12FrameCapacity{
        .modelOutputWidth = seamWidth * 2U,
        .modelOutputRows = seamRows * 2U,
        .seamWidth = seamWidth,
        .seamRows = seamRows,
        .cellColumns = cellColumns,
        .cellRows = cellRows,
        .cellCount = cellCount,
        .frameSlotBytes = cellCount * kStage1To12Stage0InputCellBytes,
    };
}

// FSR4 model families are capacities, not user/test resolutions. The hot
// frame-slot path selects one capacity class from the FSR4 context maximum and
// then clips/imports the active viewport through the GPU bookends.
inline constexpr Stage1To12FrameCapacity kStage1To12R1080FrameCapacity =
    stage1To12FrameCapacityForModelOutput(1920U, 1080U);
inline constexpr Stage1To12FrameCapacity kStage1To12R2160FrameCapacity =
    stage1To12FrameCapacityForModelOutput(3840U, 2160U);
inline constexpr Stage1To12FrameCapacity kStage1To12R4320FrameCapacity =
    stage1To12FrameCapacityForModelOutput(7680U, 4320U);

// The currently checked-in xclbin source emits the R2160 capacity. Product
// packaging must treat this as one capacity profile, not as the user's active
// resolution or quality-mode setting.
inline constexpr Stage1To12FrameCapacity kStage1To12CompiledFrameCapacity = kStage1To12R2160FrameCapacity;
inline constexpr std::size_t kStage1To12FrameSeamWidth = kStage1To12CompiledFrameCapacity.seamWidth;
inline constexpr std::size_t kStage1To12FrameSeamRows = kStage1To12CompiledFrameCapacity.seamRows;
inline constexpr std::size_t kStage1To12FrameCellColumns = kStage1To12CompiledFrameCapacity.cellColumns;
inline constexpr std::size_t kStage1To12FrameCellRows = kStage1To12CompiledFrameCapacity.cellRows;
inline constexpr std::size_t kStage1To12FrameCellCount = kStage1To12CompiledFrameCapacity.cellCount;
inline constexpr std::size_t kStage1To12FrameSlotBytes = kStage1To12CompiledFrameCapacity.frameSlotBytes;
inline constexpr std::size_t kStage1To12TokenBytes = kStage1To12FrameSlotBytes;
inline constexpr std::size_t kStage1To12EgressBytes = kStage1To12FrameSlotBytes;

// Long-lived skip spill arena. This is a separate persistent BO from the frame
// ingress/egress slots because SkipA and SkipB survive many logical stages and
// are not part of the visible GPU-facing frame payload. APUSR keeps only one
// frame inside the NPU core, so the arena reserves exactly one combined
// SkipA/SkipB payload. Future scratch storage must be a separate explicit BO or
// slice, not a hidden skip-frame multiplier.
inline constexpr std::size_t kStage1To12SkipSpillAlignmentBytes = 4096U;
inline constexpr std::size_t kStage1To12SkipABytesPerFrame =
    kStage1To12CompiledFrameCapacity.seamWidth * kStage1To12CompiledFrameCapacity.seamRows * 16U;
inline constexpr std::size_t kStage1To12SkipBBytesPerFrame =
    (kStage1To12CompiledFrameCapacity.seamWidth / 2U) * (kStage1To12CompiledFrameCapacity.seamRows / 2U) * 32U;
inline constexpr std::size_t kStage1To12SkipAOffsetBytes = 0U;
inline constexpr std::size_t kStage1To12SkipBOffsetBytes =
    stage1To12AlignUp(kStage1To12SkipABytesPerFrame, kStage1To12SkipSpillAlignmentBytes);
inline constexpr std::size_t kStage1To12SkipSpillPayloadBytes =
    stage1To12AlignUp(kStage1To12SkipBOffsetBytes + kStage1To12SkipBBytesPerFrame, kStage1To12SkipSpillAlignmentBytes);
inline constexpr std::size_t kStage1To12SkipSpillArenaBytes =
    kStage1To12SkipSpillPayloadBytes;


// Product resident parameter arena. Offsets mirror the FSR4 model-parameter
// order inside the arena; they are not a placement plan for the spatial core.
inline constexpr std::size_t kStage1To12ResidentStage1ConvNext16Offset = 0U;
inline constexpr std::size_t kStage1To12ResidentStage2ConvNext16Offset = 4064U;
inline constexpr std::size_t kStage1To12ResidentStage3To4DownFnb32Offset = 8128U;
inline constexpr std::size_t kStage1To12ResidentStage5Fnb32Offset = 18144U;
inline constexpr std::size_t kStage1To12ResidentStage6To7DownFnb64Offset = 26336U;
inline constexpr std::size_t kStage1To12ResidentStage8Fnb64Offset = 63232U;
inline constexpr std::size_t kStage1To12ResidentStage9Ct2d32Offset = 91424U;
inline constexpr std::size_t kStage1To12ResidentStage10Fnb32C8GroupOffset = 100320U;
inline constexpr std::size_t kStage1To12ResidentStage11Fnb32Ct2d16Offset = 108032U;
inline constexpr std::size_t kStage1To12ResidentStage12ConvNext16Offset = 118112U;
inline constexpr std::size_t kStage1To12ResidentParameterArenaPayloadBytes = 122160U;
inline constexpr std::size_t kStage1To12ResidentParameterArenaBytes = 122880U;
inline constexpr std::size_t kStage1To12Stage1RecordHeaderOffset = 0U;
inline constexpr std::size_t kStage1To12Stage1SpatialWeightsOffset = 16U;
inline constexpr std::size_t kStage1To12Stage1SpatialBiasOffset = 2320U;
inline constexpr std::size_t kStage1To12Stage1SpatialMultiplierOffset = 2384U;
inline constexpr std::size_t kStage1To12Stage1ExpandWeightsOffset = 2448U;
inline constexpr std::size_t kStage1To12Stage1ExpandBiasOffset = 2960U;
inline constexpr std::size_t kStage1To12Stage1ExpandMultiplierOffset = 3088U;
inline constexpr std::size_t kStage1To12Stage1ContractWeightsOffset = 3216U;
inline constexpr std::size_t kStage1To12Stage1ContractBiasOffset = 3728U;
inline constexpr std::size_t kStage1To12Stage1ContractMultiplierOffset = 3792U;
inline constexpr std::size_t kStage1To12Stage1ResidualMultiplierOffset = 3856U;
inline constexpr std::size_t kStage1To12Stage1ResidualShiftOffset = 3920U;
inline constexpr std::size_t kStage1To12Stage1ReservedOffset = 3984U;


// Parameter movement is a model-epoch operation. Hot frame-slot sequences bind
// the arena for ABI stability but schedule zero bulk parameter DMA.
inline constexpr std::size_t kStage1To12ModelEpochParameterDmaBytes =
    kStage1To12Stage1ParamBytes +
    kStage1To12Stage2ParamBytes +
    kStage1To12Stage3To4ParamBytes +
    kStage1To12Stage5ParamBytes +
    kStage1To12Stage6To7ParamBytes +
    kStage1To12Stage8ParamBytes +
    kStage1To12Stage9ParameterChunks * kStage1To12Stage9ParamChunkBytes +
    kStage1To12Stage10ParamBytes +
    kStage1To12Stage11ParamBytes +
    kStage1To12Stage12ParamBytes;
inline constexpr std::size_t kStage1To12HotPathParameterDmaBytesPerRun = 0U;

static_assert(kStage1To12Stage0InputPackageBytes == 32768U);
static_assert(kStage1To12Stage1InputPackageBytes == 46080U);
static_assert(kStage1To12Stage2HaloPackageBytes == 46080U);
static_assert(kStage1To12Stage3To4DownsourcePackageBytes == 51200U);
static_assert(kStage1To12Stage3To4OutputPackageBytes == 16384U);
static_assert(kStage1To12Stage0InputCellBytes == 2048U);
static_assert(kStage1To12ReferencePackageWidth == 64U);
static_assert(kStage1To12ReferencePackageRows == 32U);
static_assert(kStage1To12TransportCellWidth == 16U);
static_assert(kStage1To12TransportCellRows == 8U);
static_assert(kStage1To12R1080FrameCapacity.modelOutputWidth == 1920U);
static_assert(kStage1To12R1080FrameCapacity.modelOutputRows == 1088U);
static_assert(kStage1To12R1080FrameCapacity.seamWidth == 960U);
static_assert(kStage1To12R1080FrameCapacity.seamRows == 544U);
static_assert(kStage1To12R1080FrameCapacity.cellColumns == 60U);
static_assert(kStage1To12R1080FrameCapacity.cellRows == 68U);
static_assert(kStage1To12R1080FrameCapacity.cellCount == 4080U);
static_assert(kStage1To12R1080FrameCapacity.frameSlotBytes == 8355840U);
static_assert(kStage1To12R2160FrameCapacity.modelOutputWidth == 3840U);
static_assert(kStage1To12R2160FrameCapacity.modelOutputRows == 2176U);
static_assert(kStage1To12R2160FrameCapacity.seamRows == 1088U);
static_assert(kStage1To12R2160FrameCapacity.cellColumns == 120U);
static_assert(kStage1To12R2160FrameCapacity.cellRows == 136U);
static_assert(kStage1To12R2160FrameCapacity.frameSlotBytes == 33423360U);
static_assert(kStage1To12R4320FrameCapacity.modelOutputWidth == 7680U);
static_assert(kStage1To12R4320FrameCapacity.modelOutputRows == 4352U);
static_assert(kStage1To12R4320FrameCapacity.seamWidth == 3840U);
static_assert(kStage1To12R4320FrameCapacity.seamRows == 2176U);
static_assert(kStage1To12R4320FrameCapacity.cellColumns == 240U);
static_assert(kStage1To12R4320FrameCapacity.cellRows == 272U);
static_assert(kStage1To12R4320FrameCapacity.cellCount == 65280U);
static_assert(kStage1To12R4320FrameCapacity.frameSlotBytes == 133693440U);
static_assert(kStage1To12FrameSeamWidth == 1920U);
static_assert(kStage1To12FrameSeamRows == 1088U);
static_assert(kStage1To12FrameCellColumns == 120U);
static_assert(kStage1To12FrameCellRows == 136U);
static_assert(kStage1To12FrameCellCount == 16320U);
static_assert(kStage1To12FrameSlotBytes == 33423360U);
static_assert(kStage1To12TokenBytes == 33423360U);
static_assert(kStage1To12EgressBytes == 33423360U);
static_assert(kStage1To12SkipABytesPerFrame == 33423360U);
static_assert(kStage1To12SkipBBytesPerFrame == 16711680U);
static_assert(kStage1To12SkipBOffsetBytes == 33423360U);
static_assert(kStage1To12SkipSpillPayloadBytes == 50135040U);
static_assert(kStage1To12SkipSpillArenaBytes == 50135040U);
static_assert(kStage1To12ResidentParameterArenaPayloadBytes == 122160U);
static_assert(kStage1To12ResidentParameterArenaBytes == 122880U);
static_assert(kStage1To12Stage1RecordHeaderOffset == 0U);
static_assert(kStage1To12Stage1ContractWeightsOffset == 3216U);
static_assert(kStage1To12Stage1ResidualMultiplierOffset == 3856U);
static_assert(kStage1To12Stage1ReservedOffset + 64U == kStage1To12Stage1ParamBytes);
static_assert(kStage1To12ResidentStage9Ct2d32Offset + kStage1To12Stage9ParameterChunks * kStage1To12Stage9ParamChunkBytes == kStage1To12ResidentStage10Fnb32C8GroupOffset);
static_assert(kStage1To12ModelEpochParameterDmaBytes == 122048U);
static_assert(kStage1To12HotPathParameterDmaBytesPerRun == 0U);
static_assert(kStage1To12ResidentParameterArenaPayloadBytes < kStage1To12ResidentParameterArenaBytes);
static_assert((kStage1To12ResidentParameterArenaBytes % 4096U) == 0U);

} // namespace apusr::kernels::middle_core
