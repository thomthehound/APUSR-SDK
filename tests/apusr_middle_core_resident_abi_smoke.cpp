// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/backends/apusr_npu_xrt.h>
#include <apusr/apusr_npu.h>
#include <apusr/kernels/apusr_middle_core_resident_abi.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << "\n";
        std::exit(1);
    }
}

} // namespace

int main()
{
    using namespace apusr::kernels::middle_core;

    static_assert(kKernelAbiVersion == 1U);
    static_assert(kKernelOpcode == 3U);
    static_assert(kResidentParameterBufferArgIndex == 3);
    static_assert(kInputTokenBufferArgIndex == 4);
    static_assert(kOutputTokenBufferArgIndex == 5);
    static_assert(kSkipSpillBufferArgIndex == 6);
    static_assert(kControlRingBufferArgIndex < 0);

    static_assert(kStage1To12Stage0InputPackageBytes == 32768U);
    static_assert(kStage1To12Stage1InputPackageBytes == 46080U);
    static_assert(kStage1To12Stage0InputCellBytes == 2048U);
    static_assert(kStage1To12ReferencePackageWidth == 64U);
    static_assert(kStage1To12ReferencePackageRows == 32U);
    static_assert(kStage1To12TransportCellWidth == 16U);
    static_assert(kStage1To12TransportCellRows == 8U);

    static_assert(kStage1To12R1080FrameCapacity.modelOutputWidth == 1920U);
    static_assert(kStage1To12R1080FrameCapacity.modelOutputRows == 1088U);
    static_assert(kStage1To12R1080FrameCapacity.seamWidth == 960U);
    static_assert(kStage1To12R1080FrameCapacity.seamRows == 544U);
    static_assert(kStage1To12R1080FrameCapacity.frameSlotBytes == 8355840U);
    static_assert(kStage1To12R2160FrameCapacity.modelOutputWidth == 3840U);
    static_assert(kStage1To12R2160FrameCapacity.modelOutputRows == 2176U);
    static_assert(kStage1To12R2160FrameCapacity.seamRows == 1088U);
    static_assert(kStage1To12R2160FrameCapacity.cellColumns == 120U);
    static_assert(kStage1To12R2160FrameCapacity.cellRows == 136U);
    static_assert(kStage1To12R2160FrameCapacity.frameSlotBytes == 33423360U);
    static_assert(kStage1To12R4320FrameCapacity.modelOutputWidth == 7680U);
    static_assert(kStage1To12R4320FrameCapacity.modelOutputRows == 4352U);
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

    static_assert(kStage1To12ResidentParameterArenaBytes == 122880U);
    static_assert(kStage1To12Stage1RecordHeaderOffset == 0U);
    static_assert(kStage1To12Stage1SpatialWeightsOffset == 16U);
    static_assert(kStage1To12Stage1ExpandWeightsOffset == 2448U);
    static_assert(kStage1To12Stage1ContractWeightsOffset == 3216U);
    static_assert(kStage1To12Stage1ResidualMultiplierOffset == 3856U);
    static_assert(kStage1To12Stage1ResidualShiftOffset == 3920U);
    static_assert(kStage1To12Stage1ReservedOffset + 64U == kStage1To12Stage1ParamBytes);
    static_assert(kStage1To12ResidentParameterArenaPayloadBytes == 122160U);
    static_assert(kStage1To12ResidentStage8Fnb64Offset == 63232U);
    static_assert(kStage1To12ResidentStage9Ct2d32Offset == 91424U);
    static_assert(kStage1To12ResidentStage10Fnb32C8GroupOffset == 100320U);
    static_assert(kStage1To12ResidentStage11Fnb32Ct2d16Offset == 108032U);
    static_assert(kStage1To12ResidentStage12ConvNext16Offset == 118112U);
    static_assert(kStage1To12ResidentStage9Ct2d32Offset + kStage1To12Stage9ParameterChunks * kStage1To12Stage9ParamChunkBytes == kStage1To12ResidentStage10Fnb32C8GroupOffset);
    static_assert(kStage1To12ModelEpochParameterDmaBytes == 122048U);
    static_assert(kStage1To12HotPathParameterDmaBytesPerRun == 0U);

    apusr::backends::AmdXrtKernelAbi abi{};
    abi.residentParameterBufferArgIndex = kResidentParameterBufferArgIndex;
    abi.inputTokenBufferArgIndex = kInputTokenBufferArgIndex;
    abi.outputTokenBufferArgIndex = kOutputTokenBufferArgIndex;
    abi.skipSpillBufferArgIndex = kSkipSpillBufferArgIndex;
    abi.controlRingBufferArgIndex = kControlRingBufferArgIndex;

    require(abi.opcodeArgIndex == kOpcodeArgIndex, "opcode arg index should match the mlir-aie DPU ABI");
    require(abi.instructionBufferArgIndex == kInstructionBufferArgIndex, "instruction BO arg index should match the mlir-aie DPU ABI");
    require(abi.instructionCountArgIndex == kInstructionCountArgIndex, "instruction count arg index should match the mlir-aie DPU ABI");
    require(abi.residentParameterBufferArgIndex == 3, "resident parameter arena must be arg3");
    require(abi.inputTokenBufferArgIndex == 4, "complete frame-slot ingress must be arg4");
    require(abi.outputTokenBufferArgIndex == 5, "complete frame-slot egress must be arg5");
    require(abi.skipSpillBufferArgIndex == 6, "long-lived skip-spill arena must be arg6 when exposed");
    require(abi.controlRingBufferArgIndex < 0, "product profile does not expose a control-ring arg yet");

    apusr::NpuDispatchDesc stage1To12Dispatch{};
    stage1To12Dispatch.residentParameterArena.byteSize = kStage1To12ResidentParameterArenaBytes;
    stage1To12Dispatch.skipSpillArena.byteSize = kStage1To12SkipSpillArenaBytes;
    stage1To12Dispatch.input.byteSize = kStage1To12TokenBytes;
    stage1To12Dispatch.output.byteSize = kStage1To12EgressBytes;
    require(stage1To12Dispatch.residentParameterArena.byteSize == 122880U, "resident parameter arena size changed");
    require(stage1To12Dispatch.skipSpillArena.byteSize == 50135040U, "SkipA/SkipB spill arena size changed");
    require(stage1To12Dispatch.input.byteSize == 33423360U, "Stage1->12 ingress frame-slot size changed");
    require(stage1To12Dispatch.output.byteSize == 33423360U, "Stage1->12 egress frame-slot size changed");
    require(kStage1To12ModelEpochParameterDmaBytes == 122048U, "Stage1->12 epoch parameter load byte count changed");
    require(kStage1To12HotPathParameterDmaBytesPerRun == 0U, "hot frame-slot runs must not schedule bulk parameter DMA");

    std::cout << "APUSR middle-core resident ABI smoke: product frame-slot ABI and epoch-loaded parameters validated\n";
    return 0;
}
