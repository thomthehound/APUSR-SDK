// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_backend_vk.h"
#include "apusr_framegen_present_images_vk.h"

#include <cstdint>

struct ApusrFramegenVulkanSourceReplayContext;

enum ApusrFramegenVulkanReplayContent : std::uint32_t {
    APUSR_FRAMEGEN_VULKAN_REPLAY_SOURCE = 0u,
    APUSR_FRAMEGEN_VULKAN_REPLAY_GENERATED = 1u,
};

using ApusrFramegenVulkanAcquireImageCallback = std::int32_t (*)(
    void* userContext,
    void* device,
    void* swapchain,
    std::uint64_t timeout,
    void* semaphore,
    std::uint32_t* outImageIndex) noexcept;

using ApusrFramegenVulkanReleaseImageCallback = bool (*)(
    void* userContext,
    void* device,
    void* swapchain,
    std::uint32_t imageIndex) noexcept;

struct ApusrFramegenVulkanSourceReplayMetrics {
    std::uint64_t waitPreviousUs = 0u;
    std::uint64_t acquireUs = 0u;
    std::uint64_t recordUs = 0u;
    std::uint64_t submitUs = 0u;
};

struct ApusrFramegenVulkanSourceReplayDescription {
    ApusrFramegenVulkanDeviceContext deviceContext{};
    void* queue = nullptr;
    std::uint32_t queueFamilyIndex = 0;
    // Optional shared std::mutex owned by the frontend. Source and generated
    // replay contexts may target the same VkQueue; this gate provides Vulkan's
    // required external synchronization across those independent contexts.
    void* queueSubmitMutex = nullptr;
    void* device = nullptr;
    void* swapchain = nullptr;
    // Number of private real WSI targets.
    std::uint32_t presentImageCount = 0;
    // Number of APUSR source/generation slots indexed by the virtual host
    // backbuffer index.
    std::uint32_t sourceSlotCount = 0;
    ApusrFramegenVulkanPresentImageContext* presentImages = nullptr;
    ApusrFramegenVulkanReplayContent content = APUSR_FRAMEGEN_VULKAN_REPLAY_SOURCE;
    void* userContext = nullptr;
    ApusrFramegenVulkanAcquireImageCallback acquireImage = nullptr;
    // Optional swapchain-maintenance1 release. When available, cold failures
    // after acquire return the private image to WSI without presenting an
    // undefined retirement frame. Source prearm is enabled only with this
    // recovery primitive so an application-thread miss can never strand WSI.
    ApusrFramegenVulkanReleaseImageCallback releaseImage = nullptr;
};

ApusrFramegenResult apusrFramegenCreateVulkanSourceReplayContext(
    const ApusrFramegenVulkanSourceReplayDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanSourceReplayContext** outContext) noexcept;

// Completion timeline shared by both replay kinds. Source replay completion is
// one of the retained virtual slot's reuse dependencies; generated replay
// completion gates reuse of the corresponding generated-output ring slot.
void* apusrFramegenVulkanReplayConsumedSemaphore(
    const ApusrFramegenVulkanSourceReplayContext* context) noexcept;
std::uint64_t apusrFramegenVulkanCompletedReplayValue(
    const ApusrFramegenVulkanSourceReplayContext* context) noexcept;
std::uint64_t apusrFramegenReserveVulkanReplayCompletion(
    ApusrFramegenVulkanSourceReplayContext* context) noexcept;

// Runs on the dedicated presentation worker. It acquires a real WSI target,
// records the replay copy, and submits immediately. On success the returned
// binary semaphore is suitable as vkQueuePresentKHR's wait for outImageIndex.
// outSourceCopied is false only when recovery retires an acquired WSI image
// after a post-acquire backend failure.

// Generic real-WSI replay used by the full off-screen proxy. Source replay may
// block for a mandatory physical target. Generated replay uses a zero-timeout
// acquire and is dropped cleanly when WSI has no immediately reusable image,
// so optional generation can never head-of-line-block the source frame.
ApusrFramegenResult apusrFramegenPrepareVulkanReplay(
    ApusrFramegenVulkanSourceReplayContext* context,
    std::uint32_t sourceSlotIndex,
    std::uint64_t completionValue,
    void* const* producerWaitSemaphores,
    std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore,
    std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex,
    void** outReadySemaphore,
    bool* outCopied,
    ApusrFramegenVulkanSourceReplayMetrics* outMetrics = nullptr) noexcept;

// Application-thread fast prearm for the mandatory source target. Neither a
// busy replay command buffer nor an unavailable real WSI image may block. On a
// clean miss this returns OK with outPrepared=false and leaves completionValue
// unsignaled so the presentation worker can use the same reservation later.
// Prearm requires releaseImage so every post-acquire error is recoverable
// without displaying an undefined real-WSI image.
ApusrFramegenResult apusrFramegenTryPrepareVulkanSourceReplay(
    ApusrFramegenVulkanSourceReplayContext* context,
    std::uint32_t sourceSlotIndex,
    std::uint64_t completionValue,
    void* const* producerWaitSemaphores,
    std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore,
    std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex,
    void** outReadySemaphore,
    bool* outPrepared,
    ApusrFramegenVulkanSourceReplayMetrics* outMetrics = nullptr) noexcept;

void apusrFramegenDestroyVulkanSourceReplayContext(
    ApusrFramegenVulkanSourceReplayContext* context,
    const ApusrFramegenAllocator& allocator) noexcept;
