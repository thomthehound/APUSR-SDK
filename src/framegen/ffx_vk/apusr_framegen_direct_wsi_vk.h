// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_backend_vk.h"

#include <cstdint>

struct ApusrFramegenVulkanDirectWsiContext;

using ApusrFramegenVulkanDirectAcquireImageCallback = std::int32_t (*)(
    void* userContext,
    void* device,
    void* swapchain,
    std::uint64_t timeout,
    void* semaphore,
    std::uint32_t* outImageIndex) noexcept;

using ApusrFramegenVulkanDirectReleaseImageCallback = bool (*)(
    void* userContext,
    void* device,
    void* swapchain,
    std::uint32_t imageIndex) noexcept;

struct ApusrFramegenVulkanDirectWsiDescription {
    ApusrFramegenVulkanDeviceContext deviceContext{};
    // Presenter-thread queue used only for the tiny timeline->binary bridge
    // required by vkQueuePresentKHR.
    void* bridgeQueue = nullptr;
    // Application-thread queue used only to consume a signaled WSI acquire
    // semaphore when generation is aborted before submission. Keeping this
    // separate avoids cross-thread VkQueue access to bridgeQueue.
    void* abortQueue = nullptr;
    void* device = nullptr;
    void* swapchain = nullptr;
    std::uint32_t presentImageCount = 0u;
    void* userContext = nullptr;
    ApusrFramegenVulkanDirectAcquireImageCallback acquireImage = nullptr;
    ApusrFramegenVulkanDirectReleaseImageCallback releaseImage = nullptr;
};

ApusrFramegenResult apusrFramegenCreateVulkanDirectWsiContext(
    const ApusrFramegenVulkanDirectWsiDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanDirectWsiContext** outContext) noexcept;

// Opportunistic zero-timeout acquire. APUSR never stalls the application or
// mandatory source replay to obtain a generated target. On a clean WSI miss,
// returns OK with outAcquired=false.
ApusrFramegenResult apusrFramegenTryAcquireVulkanDirectWsiTarget(
    ApusrFramegenVulkanDirectWsiContext* context,
    std::uint32_t* outImageIndex,
    void** outAcquireSemaphore,
    bool* outAcquired) noexcept;

// After generation is known ready, establish the formal cross-queue memory
// dependency and produce the binary semaphore required by vkQueuePresentKHR.
// The timeline wait is normally already satisfied, so this bridge cannot hold
// the physical WSI queue behind a slow FI/OF dispatch.
ApusrFramegenResult apusrFramegenPrepareVulkanDirectWsiPresent(
    ApusrFramegenVulkanDirectWsiContext* context,
    std::uint32_t imageIndex,
    void* generationTimelineSemaphore,
    std::uint64_t generationTimelineValue,
    void** outPresentWaitSemaphore) noexcept;

// Call after an accepted generated vkQueuePresentKHR. The WSI owns target
// retirement from this point and another opportunistic acquire may proceed.
void apusrFramegenMarkVulkanDirectWsiPresented(
    ApusrFramegenVulkanDirectWsiContext* context,
    std::uint32_t imageIndex) noexcept;

// A generated frame that misses its deadline must not be presented late. Keep
// the acquired image private until generation completes, then release it with
// swapchain-maintenance1. This function is nonblocking.
void apusrFramegenDeferVulkanDirectWsiRelease(
    ApusrFramegenVulkanDirectWsiContext* context,
    std::uint32_t imageIndex,
    void* generationTimelineSemaphore,
    std::uint64_t generationTimelineValue) noexcept;

// Poll and release a deferred target once FI/OF has finished touching it.
// Safe on the application-present thread; never waits.
void apusrFramegenReclaimCompletedVulkanDirectWsiTarget(
    ApusrFramegenVulkanDirectWsiContext* context) noexcept;

// Cold failure path for an acquired target that was never submitted to FI.
// Consumes the signaled acquire semaphore on the application-owned abort queue,
// then returns the image to WSI. Never used on the normal frame path.
void apusrFramegenAbortVulkanDirectWsiAcquire(
    ApusrFramegenVulkanDirectWsiContext* context,
    std::uint32_t imageIndex) noexcept;

void apusrFramegenDestroyVulkanDirectWsiContext(
    ApusrFramegenVulkanDirectWsiContext* context,
    const ApusrFramegenAllocator& allocator) noexcept;
