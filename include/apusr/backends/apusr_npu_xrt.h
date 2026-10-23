// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_npu.h>

#include <cstdint>
#include <string>
#include <vector>

namespace apusr::backends {

struct AmdXrtKernelAbi {
    // mlir-aie generated XRT kernels use a control ABI before runtime_sequence
    // memref BO arguments: opcode, instruction BO, and instruction count.
    // TUNAH proved this form for the queue-fence handoff tests.
    int opcodeArgIndex = 0;
    int instructionBufferArgIndex = 1;
    int instructionCountArgIndex = 2;
    int inputTokenBufferArgIndex = 3;
    int outputTokenBufferArgIndex = 4;

    // Long-lived skip tensors use a persistent spill arena. Product graphs that
    // expose this argument should bind one combined SkipA/SkipB BO; immediate
    // frame ingress and egress BOs must not be aliased as the skip store.
    int skipSpillBufferArgIndex = -1;

    // Resident parameter arenas are bound separately from hot token payloads.
    // Profiles that expose this argument should load model parameters once per
    // context/model epoch and keep frame tokens parameter-free.
    int residentParameterBufferArgIndex = -1;

    // Disabled for kernels that do not expose a persistent control-ring BO. A
    // persistent-control profile should bind the GPU/host-written descriptor ring
    // directly.
    int controlRingBufferArgIndex = -1;

    // These remain disabled until the selected MLIR runtime_sequence exposes the
    // matching scalar args.
    int bandIndexArgIndex = -1;
    int bandCountArgIndex = -1;
    int inputFirstRowArgIndex = -1;
    int inputRowCountArgIndex = -1;
    int outputFirstRowArgIndex = -1;
    int outputRowCountArgIndex = -1;
    int renderWidthArgIndex = -1;
    int renderHeightArgIndex = -1;
    int firstBandFlagArgIndex = -1;
    int lastBandFlagArgIndex = -1;
};

struct AmdXrtBackendConfig {
    std::uint32_t deviceIndex = 0;
    std::string xclbinPath;
    std::vector<char> xclbinData;
    std::string kernelName;
    std::vector<char> instructionData;
    std::vector<char> parameterLoadInstructionData;
    std::uint32_t opcode = 3;
    AmdXrtKernelAbi kernelAbi;
    bool requireFenceInterop = true;
    bool requireUserptrImport = true;
    bool preferExclusiveHwContext = false;
};

[[nodiscard]] NpuBackendPtr createAmdXrtNpuBackend(AmdXrtBackendConfig config);

} // namespace apusr::backends
