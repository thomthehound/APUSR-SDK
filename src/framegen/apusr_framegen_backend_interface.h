// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_backend_types.h"
#include "apusr_framegen_core_types.h"

#include <cstddef>
#include <cstdint>

struct ApusrFramegenBackendInterface;

using ApusrFramegenPass = uint32_t;

using ApusrFramegenGetVersionFunc = ApusrFramegenVersion (*)(ApusrFramegenBackendInterface* backend);
using ApusrFramegenGetGpuMemoryUsageFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, uint32_t effectContextId, ApusrFramegenMemoryUsage* outUsage);
using ApusrFramegenCreateContextFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, uint32_t* effectContextId);
using ApusrFramegenGetDeviceCapabilitiesFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, ApusrFramegenDeviceCapabilities* outCapabilities);
using ApusrFramegenDestroyContextFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, uint32_t effectContextId);
using ApusrFramegenCreateResourceFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, const ApusrFramegenCreateResourceDescription* description, uint32_t effectContextId, ApusrFramegenResource* outResource);
using ApusrFramegenDestroyResourceFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, ApusrFramegenResource resource, uint32_t effectContextId);
using ApusrFramegenRegisterResourceFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, const ApusrFramegenExternalResource* resource, uint32_t effectContextId, ApusrFramegenResource* outResource);
using ApusrFramegenGetResourceFunc = ApusrFramegenExternalResource (*)(ApusrFramegenBackendInterface* backend, ApusrFramegenResource resource);
using ApusrFramegenUnregisterResourcesFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, ApusrFramegenCommandList commandList, uint32_t effectContextId);
using ApusrFramegenGetResourceDescriptionFunc = ApusrFramegenResourceDescription (*)(ApusrFramegenBackendInterface* backend, ApusrFramegenResource resource);
using ApusrFramegenStageConstantsFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, void* data, uint32_t size, ApusrFramegenConstantBuffer* constantBuffer);
using ApusrFramegenCreatePipelineFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, ApusrFramegenShaderBlob* shaderBlob, const ApusrFramegenPipelineDescription* description, uint32_t effectContextId, ApusrFramegenPipeline* outPipeline);
using ApusrFramegenDestroyPipelineFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, ApusrFramegenPipeline* pipeline, uint32_t effectContextId);
using ApusrFramegenScheduleJobFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, const ApusrFramegenGpuJob* job);
using ApusrFramegenExecuteJobsFunc = ApusrFramegenResult (*)(ApusrFramegenBackendInterface* backend, ApusrFramegenCommandList commandList, uint32_t effectContextId);

struct ApusrFramegenBackendInterface {
    ApusrFramegenGetVersionFunc getVersion = nullptr;
    ApusrFramegenGetGpuMemoryUsageFunc getGpuMemoryUsage = nullptr;
    ApusrFramegenCreateContextFunc createContext = nullptr;
    ApusrFramegenGetDeviceCapabilitiesFunc getDeviceCapabilities = nullptr;
    ApusrFramegenDestroyContextFunc destroyContext = nullptr;
    ApusrFramegenCreateResourceFunc createResource = nullptr;
    ApusrFramegenDestroyResourceFunc destroyResource = nullptr;
    ApusrFramegenRegisterResourceFunc registerResource = nullptr;
    ApusrFramegenGetResourceFunc getResource = nullptr;
    ApusrFramegenUnregisterResourcesFunc unregisterResources = nullptr;
    ApusrFramegenGetResourceDescriptionFunc getResourceDescription = nullptr;
    ApusrFramegenStageConstantsFunc stageConstants = nullptr;
    ApusrFramegenCreatePipelineFunc createPipeline = nullptr;
    ApusrFramegenDestroyPipelineFunc destroyPipeline = nullptr;
    ApusrFramegenScheduleJobFunc scheduleJob = nullptr;
    ApusrFramegenExecuteJobsFunc executeJobs = nullptr;
    void* scratchBuffer = nullptr;
    size_t scratchBufferSize = 0;
    ApusrFramegenDevice device{};
};

constexpr ApusrFramegenVersion apusrFramegenMakeVersion(uint32_t major, uint32_t minor, uint32_t patch) noexcept
{
    return (major << 22u) | (minor << 12u) | patch;
}

constexpr ApusrFramegenVersion APUSR_FRAMEGEN_BACKEND_VERSION = apusrFramegenMakeVersion(2, 0, 0);
