// APUSR-maintained FrameGen backend types derived from the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

#include "apusr_framegen_resource_types.h"

#include <cstddef>
#include <cstdint>
#include <cwchar>

inline constexpr uint32_t APUSR_FRAMEGEN_MAX_SRVS = 16;
inline constexpr uint32_t APUSR_FRAMEGEN_MAX_UAVS = 16;
inline constexpr uint32_t APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS = 3;
inline constexpr uint32_t APUSR_FRAMEGEN_RESOURCE_NAME_SIZE = 64;
inline constexpr uint32_t APUSR_FRAMEGEN_MAX_QUEUED_FRAMES = 4;
inline constexpr uint32_t APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT = 512;
inline constexpr uint32_t APUSR_FRAMEGEN_MAX_PASSES_PER_CONTEXT = 50;
inline constexpr uint32_t APUSR_FRAMEGEN_CONSTANT_BUFFER_ENTRY_SIZE = 4096;
inline constexpr uint32_t APUSR_FRAMEGEN_MAX_BARRIERS = 128;
inline constexpr uint32_t APUSR_FRAMEGEN_MAX_GPU_JOBS = 256;
inline constexpr uint32_t APUSR_FRAMEGEN_MAX_SAMPLERS = 16;
inline constexpr uint32_t APUSR_FRAMEGEN_DESCRIPTOR_RING_SIZE =
    APUSR_FRAMEGEN_MAX_QUEUED_FRAMES * APUSR_FRAMEGEN_MAX_PASSES_PER_CONTEXT * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT;
inline constexpr uint32_t APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE =
    APUSR_FRAMEGEN_MAX_QUEUED_FRAMES * APUSR_FRAMEGEN_MAX_PASSES_PER_CONTEXT * APUSR_FRAMEGEN_CONSTANT_BUFFER_ENTRY_SIZE;

struct ApusrFramegenResource {
    int32_t internalIndex;
};

enum ApusrFramegenResourceInitDataType : uint32_t {
    APUSR_FRAMEGEN_RESOURCE_INIT_INVALID = 0,
    APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED,
    APUSR_FRAMEGEN_RESOURCE_INIT_BUFFER,
    APUSR_FRAMEGEN_RESOURCE_INIT_VALUE,
};

struct ApusrFramegenResourceInitData {
    ApusrFramegenResourceInitDataType type;
    size_t size;
    union {
        void* buffer;
        unsigned char value;
    };

    static ApusrFramegenResourceInitData fromValue(size_t dataSize, uint8_t initValue) noexcept
    {
        ApusrFramegenResourceInitData initData;
        initData.type = APUSR_FRAMEGEN_RESOURCE_INIT_VALUE;
        initData.size = dataSize;
        initData.value = initValue;
        return initData;
    }

    static ApusrFramegenResourceInitData fromBuffer(size_t dataSize, void* initBuffer) noexcept
    {
        ApusrFramegenResourceInitData initData;
        initData.type = APUSR_FRAMEGEN_RESOURCE_INIT_BUFFER;
        initData.size = dataSize;
        initData.buffer = initBuffer;
        return initData;
    }
};

struct ApusrFramegenCreateResourceDescription {
    ApusrFramegenHeapType heapType;
    ApusrFramegenResourceDescription resourceDescription;
    ApusrFramegenResourceState initialState;
    const wchar_t* name;
    uint32_t id;
    ApusrFramegenResourceInitData initData;
};

struct ApusrFramegenSamplerDescription {
    ApusrFramegenFilter filter;
    ApusrFramegenAddressMode addressModeU;
    ApusrFramegenAddressMode addressModeV;
    ApusrFramegenAddressMode addressModeW;
};

struct ApusrFramegenPipelineDescription {
    const ApusrFramegenSamplerDescription* samplers;
    size_t samplerCount;
    wchar_t name[APUSR_FRAMEGEN_RESOURCE_NAME_SIZE];
    bool indirectWorkload;
};

struct ApusrFramegenResourceBinding {
    uint32_t slotIndex;
    uint32_t arrayIndex;
    uint32_t resourceIdentifier;
    const char* name;
};

struct ApusrFramegenPipeline {
    void* rootSignature;
    void* cmdSignature;
    void* pipeline;

    // Stable shader metadata used by native backends for sampled profiling and
    // diagnostics. Populated at pipeline creation; normal dispatches perform no
    // lookup or string work when profiling is disabled.
    std::uint8_t shaderEffect = 0;
    std::uint8_t shaderPassId = 0;
    std::uint16_t shaderReserved = 0;
    std::uint32_t shaderPermutationOptions = 0;
    std::uint32_t shaderByteSize = 0;
    std::uint32_t requiredSubgroupSize = 0;

    uint32_t uavTextureCount;
    uint32_t srvTextureCount;
    uint32_t srvBufferCount;
    uint32_t uavBufferCount;
    uint32_t constCount;
    uint32_t uavDescriptorTableSize;
    uint32_t srvDescriptorTableSize;

    ApusrFramegenResourceBinding uavTextureBindings[APUSR_FRAMEGEN_MAX_UAVS];
    ApusrFramegenResourceBinding srvTextureBindings[APUSR_FRAMEGEN_MAX_SRVS];
    ApusrFramegenResourceBinding srvBufferBindings[APUSR_FRAMEGEN_MAX_SRVS];
    ApusrFramegenResourceBinding uavBufferBindings[APUSR_FRAMEGEN_MAX_UAVS];
    ApusrFramegenResourceBinding constantBufferBindings[APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS];

    wchar_t name[APUSR_FRAMEGEN_RESOURCE_NAME_SIZE];
};

struct ApusrFramegenConstantAllocation {
    uint64_t gpuAddress;
};

struct ApusrFramegenConstantBuffer {
    uint32_t num32BitEntries;
    uint32_t* data;
};

struct ApusrFramegenTextureSrv {
    ApusrFramegenResource resource;
};

struct ApusrFramegenBufferSrv {
    uint32_t offset;
    uint32_t size;
    uint32_t stride;
    ApusrFramegenResource resource;
};

struct ApusrFramegenTextureUav {
    uint32_t mip;
    ApusrFramegenResource resource;
};

struct ApusrFramegenBufferUav {
    uint32_t offset;
    uint32_t size;
    uint32_t stride;
    ApusrFramegenResource resource;
};

enum ApusrFramegenJobType : uint32_t {
    APUSR_FRAMEGEN_JOB_CLEAR_FLOAT = 0,
    APUSR_FRAMEGEN_JOB_COPY = 1,
    APUSR_FRAMEGEN_JOB_COMPUTE = 2,
    APUSR_FRAMEGEN_JOB_BARRIER = 3,
    APUSR_FRAMEGEN_JOB_DISCARD = 4,
};

enum ApusrFramegenJobFlagBits : uint32_t {
    APUSR_FRAMEGEN_JOB_FLAG_NONE = 0,
    APUSR_FRAMEGEN_JOB_FLAG_SKIP_BARRIERS = 1u << 0,
};

struct ApusrFramegenClearFloatJob {
    float color[4];
    ApusrFramegenResource target;
};

struct ApusrFramegenCopyJob {
    ApusrFramegenResource src;
    uint32_t srcOffset;
    ApusrFramegenResource dst;
    uint32_t dstOffset;
    uint32_t size;
};

struct ApusrFramegenComputeJob {
    const ApusrFramegenPipeline* pipeline;
    uint32_t dimensions[3];
    ApusrFramegenResource cmdArgument;
    uint32_t cmdArgumentOffset;
    ApusrFramegenTextureSrv srvTextures[APUSR_FRAMEGEN_MAX_SRVS];
    ApusrFramegenBufferSrv srvBuffers[APUSR_FRAMEGEN_MAX_SRVS];
    ApusrFramegenTextureUav uavTextures[APUSR_FRAMEGEN_MAX_UAVS];
    ApusrFramegenBufferUav uavBuffers[APUSR_FRAMEGEN_MAX_UAVS];
    ApusrFramegenConstantBuffer constantBuffers[APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS];
    uint32_t flags;
    // Optical-flow pyramid level for profiler attribution. UINT32_MAX means
    // the dispatch is not level-specific (including all FI passes).
    std::uint32_t profileLevel;
};

struct ApusrFramegenBarrierJob {
    ApusrFramegenResource resource;
    ApusrFramegenResourceState newState;
};

struct ApusrFramegenDiscardJob {
    ApusrFramegenResource target;
};

struct ApusrFramegenGpuJob {
    ApusrFramegenJobType jobType;
#if defined(FFX_DX12_ENABLE_GPU_MARKERS) && FFX_DX12_ENABLE_GPU_MARKERS
    wchar_t jobLabel[APUSR_FRAMEGEN_RESOURCE_NAME_SIZE];
#endif
    union {
        ApusrFramegenClearFloatJob clearJob;
        ApusrFramegenCopyJob copyJob;
        ApusrFramegenComputeJob computeJob;
        ApusrFramegenBarrierJob barrierJob;
        ApusrFramegenDiscardJob discardJob;
    };

};

enum ApusrFramegenShaderEffect : std::uint8_t {
    APUSR_FRAMEGEN_SHADER_EFFECT_UNKNOWN = 0,
    APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION = 1,
    APUSR_FRAMEGEN_SHADER_EFFECT_OPTICAL_FLOW = 2,
};

struct ApusrFramegenShaderBlob {
    const uint8_t* data;
    uint32_t size;

    uint32_t cbvCount;
    uint32_t srvTextureCount;
    uint32_t uavTextureCount;
    uint32_t srvBufferCount;
    uint32_t uavBufferCount;

    const char** boundConstantBufferNames;
    const uint32_t* boundConstantBuffers;
    const uint32_t* boundConstantBufferCounts;
    const uint32_t* boundConstantBufferSpaces;

    const char** boundSRVTextureNames;
    const uint32_t* boundSRVTextures;
    const uint32_t* boundSRVTextureCounts;
    const uint32_t* boundSRVTextureSpaces;

    const char** boundUAVTextureNames;
    const uint32_t* boundUAVTextures;
    const uint32_t* boundUAVTextureCounts;
    const uint32_t* boundUAVTextureSpaces;

    const char** boundSRVBufferNames;
    const uint32_t* boundSRVBuffers;
    const uint32_t* boundSRVBufferCounts;
    const uint32_t* boundSRVBufferSpaces;

    const char** boundUAVBufferNames;
    const uint32_t* boundUAVBuffers;
    const uint32_t* boundUAVBufferCounts;
    const uint32_t* boundUAVBufferSpaces;

    // Backend-stable shader identity. The DX12 blob remains the reflection source;
    // API-specific backends use this key to select their native shader payload.
    ApusrFramegenShaderEffect effect;
    std::uint8_t passId;
    std::uint16_t reserved;
    std::uint32_t permutationOptions;
};

#if defined(FFX_DX12_ENABLE_GPU_MARKERS) && FFX_DX12_ENABLE_GPU_MARKERS
#define APUSR_FRAMEGEN_SET_JOB_LABEL(job, label)                                              \
    do {                                                                                       \
        std::wcsncpy((job).jobLabel, (label), APUSR_FRAMEGEN_RESOURCE_NAME_SIZE - 1);          \
        (job).jobLabel[APUSR_FRAMEGEN_RESOURCE_NAME_SIZE - 1] = L'\0';                        \
    } while (false)
#else
#define APUSR_FRAMEGEN_SET_JOB_LABEL(job, label) ((void)0)
#endif
