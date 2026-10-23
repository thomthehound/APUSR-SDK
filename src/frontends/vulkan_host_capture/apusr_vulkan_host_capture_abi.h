// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>

#if defined(_WIN32)
#define APUSR_VK_HOST_CALL __cdecl
#if defined(APUSR_VK_HOST_CAPTURE_BUILD)
// The DLL's .def file is the single export authority for the implementation.
#define APUSR_VK_HOST_API extern "C"
#else
#define APUSR_VK_HOST_API extern "C" __declspec(dllimport)
#endif
#else
#define APUSR_VK_HOST_CALL
#define APUSR_VK_HOST_API extern "C"
#endif

enum : std::uint32_t {
    APUSR_VK_HOST_QUEUE_UNKNOWN = 0,
    APUSR_VK_HOST_QUEUE_GRAPHICS = 1,
    APUSR_VK_HOST_QUEUE_COMPUTE = 2,
    APUSR_VK_HOST_QUEUE_TRANSFER = 3,
};

enum : std::uint32_t {
    APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR = 1u << 0,
    APUSR_VK_HOST_QUEUE_ROLE_INPUT = 1u << 1,
    APUSR_VK_HOST_QUEUE_ROLE_GENERATION = 1u << 2,
    APUSR_VK_HOST_QUEUE_ROLE_PRESENTATION = 1u << 3,
    APUSR_VK_HOST_QUEUE_ROLE_ACQUIRE = 1u << 4,
    // Dedicated copy/DMA lane for virtual-source and generated-output replay
    // into private WSI images. Never classify this as a compute queue.
    APUSR_VK_HOST_QUEUE_ROLE_REPLAY_TRANSFER = 1u << 5,
};

enum : std::uint32_t {
    APUSR_VK_HOST_ANTI_LAG_DRIVER_CONTROL = 0,
    APUSR_VK_HOST_ANTI_LAG_ON = 1,
    APUSR_VK_HOST_ANTI_LAG_OFF = 2,
};

enum : std::uint32_t {
    APUSR_VK_HOST_PRESENT_GENERATED = 1u << 0,
    APUSR_VK_HOST_PRESENT_PROFILE_CAPTURE = 1u << 1,
};

enum : std::uint32_t {
    APUSR_VK_HOST_NATIVE_DOT4 = 1u << 0,
    APUSR_VK_HOST_REQUIRED_CORE_FEATURES = 1u << 1,
    APUSR_VK_HOST_REQUIRED_VULKAN12_FEATURES = 1u << 2,
    APUSR_VK_HOST_SUBGROUP_QUAD = 1u << 3,
    APUSR_VK_HOST_ANTI_LAG = 1u << 4,
    APUSR_VK_HOST_FRAMEGEN_TIMELINE = 1u << 5,
    APUSR_VK_HOST_SHADER_FLOAT16 = 1u << 6,
    // Frame-generation compute pipelines may request a native 64-lane subgroup
    // through Vulkan subgroup-size control. This is deliberately separate from
    // the FSR4 required mask so ordinary upscaling remains available on devices
    // that cannot provide wave64 for the FI/OF path.
    APUSR_VK_HOST_FRAMEGEN_SUBGROUP64 = 1u << 7,
    // FSR4 compute pipelines may request either exact subgroup width. These
    // are capabilities, not a selected policy; the frontend chooses auto/32/64.
    APUSR_VK_HOST_FSR4_SUBGROUP32 = 1u << 8,
    APUSR_VK_HOST_FSR4_SUBGROUP64 = 1u << 9,
    APUSR_VK_HOST_REQUIRED_MASK =
        APUSR_VK_HOST_NATIVE_DOT4 |
        APUSR_VK_HOST_REQUIRED_CORE_FEATURES |
        APUSR_VK_HOST_REQUIRED_VULKAN12_FEATURES |
        APUSR_VK_HOST_SUBGROUP_QUAD,
};

struct ApusrVulkanHostQueueInfo {
    std::uint32_t structSize = sizeof(ApusrVulkanHostQueueInfo);
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    std::uint32_t familyIndex = 0;
    std::uint32_t queueIndex = 0;
    std::uint32_t familyFlags = 0;
    std::uint32_t queueCreateFlags = 0;
    std::uint32_t kind = APUSR_VK_HOST_QUEUE_UNKNOWN;
    std::uint32_t ownershipFlags = 0;
};

inline constexpr std::uint32_t APUSR_VK_HOST_INVALID_IMAGE_INDEX = 0xffffffffu;

enum : std::uint32_t {
    APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_REQUESTED = 1u << 0,
    APUSR_VK_HOST_SWAPCHAIN_TRANSFER_SRC = 1u << 1,
    APUSR_VK_HOST_SWAPCHAIN_TRANSFER_DST = 1u << 2,
    APUSR_VK_HOST_SWAPCHAIN_SAMPLED = 1u << 3,
    APUSR_VK_HOST_SWAPCHAIN_STORAGE = 1u << 4,
    APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_EXTRA_IMAGES = 1u << 5,
    // The application's vkGetSwapchainImagesKHR/vkAcquireNextImageKHR path is
    // backed by APUSR-owned off-screen images. Real WSI images are private to
    // the frame-generation presenter.
    APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_VIRTUALIZED = 1u << 6,
    // Device creation enabled swapchain-maintenance1, so an opportunistically
    // acquired generated WSI image can be released instead of presented when
    // generation misses its real-time deadline.
    APUSR_VK_HOST_SWAPCHAIN_MAINTENANCE1 = 1u << 7,
    // The private real swapchain accepts VK_EXT_present_timing relative-time
    // scheduling. APUSR can queue the source immediately after a generated
    // frame and let WSI hold it until the interpolation midpoint instead of
    // sleeping a CPU presentation worker.
    APUSR_VK_HOST_SWAPCHAIN_PRESENT_TIMING_RELATIVE = 1u << 8,
};

struct ApusrVulkanHostSwapchainInfo {
    std::uint32_t structSize = sizeof(ApusrVulkanHostSwapchainInfo);
    void* device = nullptr;
    void* swapchain = nullptr;
    void* surface = nullptr;
    std::uint32_t imageFormat = 0;
    std::uint32_t imageColorSpace = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t imageArrayLayers = 0;
    std::uint32_t imageUsage = 0;
    // Usage of the private real WSI images. imageUsage names the public images
    // and therefore becomes the virtual-backbuffer usage in proxy mode.
    std::uint32_t realImageUsage = 0;
    std::uint32_t originalImageUsage = 0;
    std::uint32_t supportedImageUsage = 0;
    std::uint32_t framegenFlags = 0;
    std::uint32_t imageSharingMode = 0;
    std::uint32_t presentMode = 0;
    std::uint32_t minImageCount = 0;
    std::uint32_t imageCount = 0;
    std::uint32_t realImageCount = 0;
    std::uint32_t latestAcquiredImageIndex = APUSR_VK_HOST_INVALID_IMAGE_INDEX;
    std::uint32_t latestPresentedImageIndex = APUSR_VK_HOST_INVALID_IMAGE_INDEX;
    std::uint64_t acquireSerial = 0;
    std::uint64_t presentSerial = 0;
};

struct ApusrVulkanHostDeviceInfo {
    std::uint32_t structSize = sizeof(ApusrVulkanHostDeviceInfo);
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    std::uint32_t negotiationFlags = 0;
    std::uint32_t queueCount = 0;
};

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrInstallVulkanHostCapture();
APUSR_VK_HOST_API std::uint32_t APUSR_VK_HOST_CALL apusrVulkanHostDeviceNegotiationFlags(void* device);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetDeviceInfo(void* device, ApusrVulkanHostDeviceInfo* outInfo);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetQueueInfo(
    void* device,
    std::uint32_t queueOrdinal,
    ApusrVulkanHostQueueInfo* outInfo);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostResolveQueue(
    void* device,
    std::uint32_t preferredKind,
    ApusrVulkanHostQueueInfo* outInfo);
APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostAssociateInstance(
    void* device,
    void* instance,
    void* physicalDevice);
APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostRequestAntiLag(bool requested);
APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostRequestFrameGeneration(bool requested);
APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostSetFrameGenerationActive(bool active);
APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostRequestLegacyOpticalFlowQueueRole(bool requested);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostAntiLagSupported(void* physicalDevice);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostAntiLagAvailable(void* device);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostAntiLagInput(
    void* device,
    std::uint64_t frameIndex,
    std::uint32_t mode,
    std::uint32_t maxFps);
// Completes an Anti-Lag Off state transition as a real INPUT/PRESENT pair.
// The INPUT update is emitted immediately and the matching PRESENT update is
// attached to the next physical vkQueuePresentKHR.
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostAntiLagArmPairedDisable(void* device);
APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostAntiLagSetPresentFrame(
    void* device,
    std::uint64_t frameIndex);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetSwapchainInfo(
    void* swapchain,
    ApusrVulkanHostSwapchainInfo* outInfo);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetSwapchainImage(
    void* swapchain,
    std::uint32_t imageIndex,
    void** outImage);
// Returns the underlying WSI image. Only APUSR's presentation worker should
// use this for a virtualized swapchain; applications see the off-screen image
// returned by apusrVulkanHostGetSwapchainImage/vkGetSwapchainImagesKHR.
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetRealSwapchainImage(
    void* swapchain,
    std::uint32_t imageIndex,
    void** outImage);
// Completes one virtual acquire/present ownership cycle. The virtual image is
// not returned to the application until every non-zero release dependency has
// completed on the GPU. Acquire signaling itself never waits on those values.
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostReleaseVirtualSwapchainImage(
    void* swapchain,
    std::uint32_t imageIndex,
    void* firstReleaseSemaphore,
    std::uint64_t firstReleaseValue,
    void* secondReleaseSemaphore,
    std::uint64_t secondReleaseValue);
// Teardown-only drain. Waits every outstanding virtual-slot GPU dependency
// and clears the host's references before the owning timelines are destroyed.
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostWaitVirtualSwapchainReleaseDependencies(
    void* swapchain);
APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostAcquireNextImageForFrameGeneration(
    void* device,
    void* swapchain,
    std::uint64_t timeout,
    void* semaphore,
    std::uint32_t* outImageIndex);
APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostReleaseRealSwapchainImage(
    void* device,
    void* swapchain,
    std::uint32_t imageIndex);
APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentDirect(
    void* queue,
    const void* presentInfo);
APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentSource(
    void* queue,
    const void* presentInfo);
// Attempts a WSI-timed source present. If relative present timing is not
// negotiated for every swapchain in presentInfo, returns without presenting
// and sets *outScheduled=false so the caller can use its precise host fallback.
APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentSourceRelative(
    void* queue,
    const void* presentInfo,
    std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled);
APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentGeneratedRelative(
    void* queue,
    const void* presentInfo,
    std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled);
APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentFrameGeneration(
    void* queue,
    const void* presentInfo,
    std::uint64_t frameId,
    std::uint32_t flags);
APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentSourceRelativeFrameGeneration(
    void* queue,
    const void* presentInfo,
    std::uint64_t frameId,
    std::uint32_t flags,
    std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled);
APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentGeneratedRelativeFrameGeneration(
    void* queue,
    const void* presentInfo,
    std::uint64_t frameId,
    std::uint32_t flags,
    std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled);
APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostSetQueuePresentInterceptor(void* queuePresentProc);
APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostSetSwapchainDestroyInterceptor(void* destroySwapchainProc);
