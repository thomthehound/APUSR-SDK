// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_streamline_vk.h"

#include <array>
#include <cstdint>

struct ApusrFramegenVulkanGeneratedOutputPool;

struct ApusrFramegenVulkanGeneratedOutputPoolDescription {
    ApusrFramegenVulkanDeviceContext deviceContext{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t nativeFormat = 0;
    std::uint32_t imageCount = 0;
    // Zero selects the normal generated-output contract (sampled + storage +
    // transfer). Staging-only callers can request a narrower usage set.
    std::uint32_t imageUsage = 0;
    // Internal images use concurrent sharing only when APUSR actually spans
    // queue families (for example graphics capture/replay + async compute).
    std::array<std::uint32_t, 4u> queueFamilyIndices{};
    std::uint32_t queueFamilyIndexCount = 0u;
};

// Own storage-capable generated-frame intermediates separately from the game's
// swapchain. This is required because Vulkan swapchain images are not promised
// to support STORAGE usage. Creation allocates device-local images/views; the
// caller records one initialization barrier before first use.
ApusrFramegenResult apusrFramegenCreateVulkanGeneratedOutputPool(
    const ApusrFramegenVulkanGeneratedOutputPoolDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanGeneratedOutputPool** outPool) noexcept;

ApusrFramegenResult apusrFramegenInitializeVulkanGeneratedOutputPool(
    ApusrFramegenVulkanGeneratedOutputPool* pool,
    void* commandBuffer) noexcept;

// Initialization barriers become authoritative only after the command buffer
// containing them has been submitted successfully.  An aborted command must
// roll the recorded state back so the barriers are emitted again next time.
void apusrFramegenCommitVulkanGeneratedOutputPoolInitialization(
    ApusrFramegenVulkanGeneratedOutputPool* pool) noexcept;

void apusrFramegenRollbackVulkanGeneratedOutputPoolInitialization(
    ApusrFramegenVulkanGeneratedOutputPool* pool) noexcept;

ApusrFramegenResult apusrFramegenGetVulkanGeneratedOutput(
    const ApusrFramegenVulkanGeneratedOutputPool* pool,
    std::uint32_t imageIndex,
    ApusrFramegenStreamlineVulkanResource* outResource) noexcept;

void apusrFramegenDestroyVulkanGeneratedOutputPool(
    ApusrFramegenVulkanGeneratedOutputPool* pool,
    const ApusrFramegenAllocator& allocator) noexcept;
