// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_allocator.h"
#include "apusr_framegen_backend_interface.h"

#include <array>
#include <cstddef>
#include <cstdint>

struct ApusrFramegenVulkanDeviceContext {
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    void* getInstanceProcAddr = nullptr;
    void* getDeviceProcAddr = nullptr;

    // Vulkan requires dynamic uniform offsets to honor this device limit. APUSR
    // uses explicit descriptor offsets, but the upload ring obeys the same
    // alignment so each staged block is independently bindable.
    std::uint32_t minUniformBufferOffsetAlignment = 256;

    // FFX selects its wave64 FI/OF permutations from this range. When host
    // capture negotiated Vulkan subgroup-size control, APUSR binds those
    // permutations to an actual 64-lane compute subgroup at pipeline creation.
    std::uint32_t subgroupSize = 32;
    std::uint32_t subgroupSizeMin = 32;
    std::uint32_t subgroupSizeMax = 32;
    bool subgroupSizeControlSupported = false;
    bool fp16Supported = false;

    // Internal FI/OF resources cross these queue families. Keep ownership
    // concurrent when Phase A runs on graphics and Phase B runs on async compute.
    std::array<std::uint32_t, 2> internalQueueFamilyIndices{};
    std::uint32_t internalQueueFamilyIndexCount = 0u;
};

// Optional metadata for externally-owned Vulkan images. Streamline already
// supplies this information for tagged resources, so preserve it rather than
// manufacturing a whole-image color view from VkImage alone. The structure is
// consumed synchronously by registerResource; callers may keep it on the stack.
struct ApusrFramegenVulkanExternalResourceMetadata {
    void* imageView = nullptr;
    std::uint32_t nativeFormat = 0;
    std::uint32_t nativeLayout = 0xffffffffu;
    std::uint32_t aspectMask = 0;
    std::uint32_t baseMipLevel = 0;
    std::uint32_t levelCount = 0;
    std::uint32_t baseArrayLayer = 0;
    std::uint32_t layerCount = 0;
};


enum ApusrFramegenVulkanGpuProfilePhase : std::uint32_t {
    APUSR_FRAMEGEN_VULKAN_GPU_PROFILE_COMPUTE = 0u,
    APUSR_FRAMEGEN_VULKAN_GPU_PROFILE_PREPARE = 1u,
};

struct ApusrFramegenVulkanGpuProfileFrameInfo {
    std::uint64_t frameId = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    std::int32_t interpolationX = 0;
    std::int32_t interpolationY = 0;
    std::int32_t interpolationWidth = 0;
    std::int32_t interpolationHeight = 0;
    float frameTimeDeltaMs = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionVectorScaleX = 0.0f;
    float motionVectorScaleY = 0.0f;
    std::uint32_t prepareFlags = 0;
    std::uint32_t dispatchFlags = 0;
    bool hasHudlessColor = false;
    bool motionVectorsAtDisplayResolution = false;
    bool motionVectorsJittered = false;
    bool hdr = false;
    bool reset = false;
    ApusrFramegenVulkanGpuProfilePhase phase = APUSR_FRAMEGEN_VULKAN_GPU_PROFILE_COMPUTE;
};

// Optional, sampled pass-level GPU profiling for Vulkan FrameGen prepare and
// heavy FI/OF compute. The two phases use separate profiler streams/gates so
// measuring Phase A cannot consume the Phase-B sample burst.
// The backend decides whether the current frame belongs to a three-frame burst;
// when profiling is disabled these calls are no-ops.
void apusrFramegenBeginVulkanGpuProfile(
    ApusrFramegenBackendInterface* backendInterface,
    void* commandBuffer,
    const ApusrFramegenVulkanGpuProfileFrameInfo& frame) noexcept;

void apusrFramegenEndVulkanGpuProfile(
    ApusrFramegenBackendInterface* backendInterface,
    void* commandBuffer) noexcept;

void apusrFramegenCancelVulkanGpuProfile(
    ApusrFramegenBackendInterface* backendInterface,
    void* commandBuffer) noexcept;

void apusrFramegenFlushVulkanGpuProfile(
    ApusrFramegenBackendInterface* backendInterface) noexcept;

ApusrFramegenResult apusrFramegenCreateVulkanBackend(
    const ApusrFramegenVulkanDeviceContext& deviceContext,
    ApusrFramegenBackendInterface* backendInterface,
    std::size_t contexts,
    const ApusrFramegenAllocator& allocator) noexcept;

ApusrFramegenCommandList apusrFramegenCommandListVK(void* commandBuffer) noexcept;

ApusrFramegenExternalResource apusrFramegenExternalResourceVK(
    void* resource,
    ApusrFramegenResourceDescription description,
    ApusrFramegenResourceState state = APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ) noexcept;

ApusrFramegenExternalResource apusrFramegenExternalResourceVK(
    void* resource,
    ApusrFramegenResourceDescription description,
    ApusrFramegenResourceState state,
    const ApusrFramegenVulkanExternalResourceMetadata* metadata) noexcept;

ApusrFramegenSurfaceFormat apusrFramegenSurfaceFormatFromVulkan(std::uint32_t nativeFormat) noexcept;
