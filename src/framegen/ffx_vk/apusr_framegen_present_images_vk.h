// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_present_resources_vk.h"

#include <array>
#include <cstdint>

struct ApusrFramegenVulkanPresentImageContext;

inline constexpr std::uint32_t APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE = 0xffffffffu;
// Double-buffer generated output so generation can advance as soon as FI/OF
// completes even while the previous output waits for its midpoint decision or
// physical WSI replay. FI/OF history itself remains one-generation-in-flight.
inline constexpr std::uint32_t APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS = 2u;

struct ApusrFramegenVulkanPresentImageDescription {
    ApusrFramegenVulkanDeviceContext deviceContext{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t nativeFormat = 0;
    std::uint32_t imageUsage = 0;
    // Public application backbuffers. In offscreenProxy mode these are
    // APUSR-owned ordinary images returned from vkGetSwapchainImagesKHR.
    std::uint32_t imageCount = 0;
    void* const* images = nullptr;
    // Private real WSI images owned only by APUSR's presentation worker.
    std::uint32_t presentationImageCount = 0;
    void* const* presentationImages = nullptr;
    // Usage negotiated for the private real WSI images. In proxy mode this is
    // intentionally distinct from imageUsage, which describes virtual sources.
    std::uint32_t presentationImageUsage = 0;
    bool offscreenProxy = false;
    std::array<std::uint32_t, 4u> internalQueueFamilyIndices{};
    std::uint32_t internalQueueFamilyIndexCount = 0u;
};

struct ApusrFramegenVulkanPresentFrameResources {
    ApusrFramegenStreamlineVulkanResource presentColor{};
    ApusrFramegenStreamlineVulkanResource generatedOutput{};
    bool sampledSwapchainDirectly = false;
    bool storageSwapchainDirectly = false;
};

// Clean-sheet proxy resources: public images are APUSR-retained source frames;
// private real WSI images are physical presentation targets. Generated output
// goes directly to WSI when STORAGE is negotiated, with a two-slot provider
// output ring retained only for the transfer-replay compatibility path.
ApusrFramegenResult apusrFramegenCreateVulkanPresentImageContext(
    const ApusrFramegenVulkanPresentImageDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanPresentImageContext** outContext) noexcept;

ApusrFramegenResult apusrFramegenInitializeVulkanPresentImageContext(
    ApusrFramegenVulkanPresentImageContext* context,
    void* commandBuffer) noexcept;

void apusrFramegenCommitVulkanPresentImageInitialization(
    ApusrFramegenVulkanPresentImageContext* context) noexcept;

void apusrFramegenRollbackVulkanPresentImageInitialization(
    ApusrFramegenVulkanPresentImageContext* context) noexcept;

// True when the private real WSI images can be used as storage targets. This
// is the preferred proxy path: FI writes the generated frame directly into an
// acquired presentation image and completely eliminates generated replay.
bool apusrFramegenVulkanDirectWsiStorageSupported(
    const ApusrFramegenVulkanPresentImageContext* context) noexcept;

// Real-time proxy FG path. The retained virtual source is indexed by the host
// backbuffer slot. If directPresentationImageIndex names a private real WSI
// image, FI writes directly into it. Otherwise generationSlotIndex selects the
// independently double-buffered compatibility output ring.
ApusrFramegenResult apusrFramegenPrepareVulkanOffscreenGenerationImages(
    ApusrFramegenVulkanPresentImageContext* context,
    void* inputCommandBuffer,
    void* generationCommandBuffer,
    std::uint32_t sourceImageIndex,
    std::uint32_t generationSlotIndex,
    std::uint32_t directPresentationImageIndex,
    ApusrFramegenVulkanPresentFrameResources* outResources) noexcept;

// Completes a direct-to-WSI generation target's layout transition. No-op for
// the compatibility generated-output ring.
ApusrFramegenResult apusrFramegenFinishVulkanOffscreenGenerationImages(
    ApusrFramegenVulkanPresentImageContext* context,
    void* generationCommandBuffer,
    std::uint32_t directPresentationImageIndex) noexcept;

// Presentation-worker copy from APUSR's generated image into a freshly
// acquired real WSI image.
ApusrFramegenResult apusrFramegenRecordVulkanGeneratedReplay(
    ApusrFramegenVulkanPresentImageContext* context,
    void* commandBuffer,
    std::uint32_t sourceImageIndex,
    std::uint32_t targetImageIndex) noexcept;

// Presentation-worker copy from the retained virtual source frame into a
// freshly acquired real WSI image. The source remains in GENERAL throughout
// its read-only lifetime; the target leaves in PRESENT_SRC_KHR.
ApusrFramegenResult apusrFramegenRecordVulkanSourceReplay(
    ApusrFramegenVulkanPresentImageContext* context,
    void* commandBuffer,
    std::uint32_t sourceImageIndex,
    std::uint32_t targetImageIndex) noexcept;

// Post-acquire recovery: discard an acquired WSI image and place it in
// PRESENT_SRC_KHR so it can be legally retired even if source replay recording
// failed before a copy could be submitted.
ApusrFramegenResult apusrFramegenRecordVulkanAcquiredImageRetirement(
    ApusrFramegenVulkanPresentImageContext* context,
    void* commandBuffer,
    std::uint32_t targetImageIndex) noexcept;

void apusrFramegenDestroyVulkanPresentImageContext(
    ApusrFramegenVulkanPresentImageContext* context,
    const ApusrFramegenAllocator& allocator) noexcept;
