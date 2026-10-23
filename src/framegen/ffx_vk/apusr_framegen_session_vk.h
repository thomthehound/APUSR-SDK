// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_backend_vk.h"
#include "../algorithm/apusr_framegen_algorithm_types.h"

#include <cstdint>

struct ApusrFramegenVulkanSession;

struct ApusrFramegenVulkanSessionDescription {
    ApusrFramegenVulkanDeviceContext deviceContext{};
    std::uint32_t maxRenderWidth = 0;
    std::uint32_t maxRenderHeight = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    ApusrFramegenSurfaceFormat backBufferFormat = APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    ApusrFramegenSurfaceFormat hudlessFormat = APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    ApusrFramegenSurfaceFormat distortionFormat = APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    std::uint32_t distortionWidth = 0u;
    std::uint32_t distortionHeight = 0u;
    bool depthInverted = false;
    bool depthInfinite = false;
    bool motionVectorsAtDisplayResolution = false;
    bool motionVectorsJittered = false;
    bool hdr = false;
};

struct ApusrFramegenVulkanFrameDescription {
    void* commandBuffer = nullptr;
    std::uint64_t frameId = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    ApusrFramegenRect2D interpolationRect{};
    ApusrFramegenFloatCoords2D jitterOffset{};
    ApusrFramegenFloatCoords2D motionVectorScale{};
    float frameTimeDelta = 0.0f;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraFovAngleVertical = 0.0f;
    float viewSpaceToMetersFactor = 1.0f;
    float cameraPosition[3]{};
    float cameraUp[3]{};
    float cameraRight[3]{};
    float cameraForward[3]{};
    bool reset = false;
    ApusrFramegenBackbufferTransferFunction backbufferTransferFunction = APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    float minMaxLuminance[2]{};
    std::uint32_t prepareFlags = 0;
    std::uint32_t dispatchFlags = 0;

    ApusrFramegenExternalResource depth{};
    ApusrFramegenExternalResource motionVectors{};
    ApusrFramegenExternalResource presentColor{};
    ApusrFramegenExternalResource hudlessColor{};
    ApusrFramegenExternalResource distortionField{};
    ApusrFramegenExternalResource output{};
};

ApusrFramegenResult apusrFramegenCreateVulkanSession(
    const ApusrFramegenVulkanSessionDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanSession** outSession) noexcept;

void apusrFramegenDestroyVulkanSession(
    ApusrFramegenVulkanSession* session,
    const ApusrFramegenAllocator& allocator) noexcept;

// Records the phase that consumes Streamline-owned depth/motion-vector inputs.
// A successful prepare must be followed by DispatchPrepared for the same frame
// before another frame is prepared. Splitting this boundary lets Vulkan
// eBlockNoClientQueues signal input release without waiting for optical flow
// and interpolation to finish.
ApusrFramegenResult apusrFramegenPrepareVulkanSession(
    ApusrFramegenVulkanSession* session,
    const ApusrFramegenVulkanFrameDescription& frame) noexcept;

ApusrFramegenResult apusrFramegenDispatchPreparedVulkanSession(
    ApusrFramegenVulkanSession* session,
    const ApusrFramegenVulkanFrameDescription& frame) noexcept;

// Monolithic convenience path used by callers that do not need an early input
// release boundary.
ApusrFramegenResult apusrFramegenDispatchVulkanSession(
    ApusrFramegenVulkanSession* session,
    const ApusrFramegenVulkanFrameDescription& frame) noexcept;
