// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_backend_interface.h"

namespace apusr::framegen {

ApusrFramegenResult resourceAllocationSize(
    ApusrFramegenDevice device,
    const ApusrFramegenCreateResourceDescription& description,
    uint64_t& sizeInBytes) noexcept;

inline ApusrFramegenVersion backendVersion(ApusrFramegenBackendInterface& backend) noexcept
{
    return backend.getVersion(&backend);
}

// Context creation is transactional: if capability discovery fails after the
// backend allocates a context, tear that context down before returning.
inline ApusrFramegenResult createEffectContext(
    ApusrFramegenBackendInterface& backend,
    uint32_t& effectContextId,
    ApusrFramegenDeviceCapabilities& capabilities) noexcept
{
    ApusrFramegenResult result = backend.createContext(&backend, &effectContextId);
    if (result != APUSR_FRAMEGEN_OK)
        return result;

    result = backend.getDeviceCapabilities(&backend, &capabilities);
    if (result != APUSR_FRAMEGEN_OK) {
        backend.destroyContext(&backend, effectContextId);
        effectContextId = 0;
    }
    return result;
}

// Per-effect backend seam. Methods inline through the current callback table,
// so the helper adds no second dispatch layer.
class BackendContext {
public:
    explicit BackendContext(ApusrFramegenBackendInterface& backend, uint32_t effectContextId) noexcept
        : backend_(&backend), effectContextId_(effectContextId)
    {
    }

    ApusrFramegenResult createResource(
        const ApusrFramegenCreateResourceDescription& description,
        ApusrFramegenResource& outResource) const noexcept
    {
        return backend_->createResource(backend_, &description, effectContextId_, &outResource);
    }

    void releaseResource(ApusrFramegenResource resource) const noexcept
    {
        (void)backend_->destroyResource(backend_, resource, effectContextId_);
    }

    void releaseCopyResource(ApusrFramegenResource resource) const noexcept
    {
        releaseResource({resource.internalIndex + 1});
    }

    ApusrFramegenResult createPipeline(
        ApusrFramegenShaderBlob& shaderBlob,
        const ApusrFramegenPipelineDescription& description,
        ApusrFramegenPipeline& outPipeline) const noexcept
    {
        return backend_->createPipeline(backend_, &shaderBlob, &description, effectContextId_, &outPipeline);
    }

    void releasePipeline(ApusrFramegenPipeline& pipeline) const noexcept
    {
        (void)backend_->destroyPipeline(backend_, &pipeline, effectContextId_);
    }

    ApusrFramegenResult gpuMemoryUsage(ApusrFramegenMemoryUsage& memoryUsage) const noexcept
    {
        return backend_->getGpuMemoryUsage(backend_, effectContextId_, &memoryUsage);
    }

    void releaseContext() const noexcept
    {
        (void)backend_->destroyContext(backend_, effectContextId_);
    }

    ApusrFramegenResult registerResource(
        const ApusrFramegenExternalResource& resource,
        ApusrFramegenResource& outResource) const noexcept
    {
        return backend_->registerResource(backend_, &resource, effectContextId_, &outResource);
    }

    ApusrFramegenResourceDescription resourceDescription(ApusrFramegenResource resource) const noexcept
    {
        return backend_->getResourceDescription(backend_, resource);
    }

    ApusrFramegenResult stageConstants(
        void* data,
        uint32_t size,
        ApusrFramegenConstantBuffer& constantBuffer) const noexcept
    {
        return backend_->stageConstants(backend_, data, size, &constantBuffer);
    }

    ApusrFramegenResult schedule(const ApusrFramegenGpuJob& job) const noexcept
    {
        return backend_->scheduleJob(backend_, &job);
    }

    ApusrFramegenResult execute(ApusrFramegenCommandList commandList) const noexcept
    {
        return backend_->executeJobs(backend_, commandList, effectContextId_);
    }

    ApusrFramegenResult unregisterResources(ApusrFramegenCommandList commandList) const noexcept
    {
        return backend_->unregisterResources(backend_, commandList, effectContextId_);
    }

private:
    ApusrFramegenBackendInterface* backend_;
    uint32_t effectContextId_;
};
} // namespace apusr::framegen
