// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_backend_vk.h"

#include <cstdint>

struct ApusrFramegenVulkanPresentSubmitContext;

struct ApusrFramegenVulkanPresentSubmitDescription {
    ApusrFramegenVulkanDeviceContext deviceContext{};
    // Phase A: transient client inputs are consumed and retained state is
    // prepared. This is always an APUSR-owned queue, never the game's queue.
    void* inputQueue = nullptr;
    std::uint32_t inputQueueFamilyIndex = 0;
    // Phase B: optical flow + frame interpolation. Prefer a genuine
    // non-graphics compute queue when one exists; otherwise this aliases the
    // private input/generation graphics queue.
    void* generationQueue = nullptr;
    std::uint32_t generationQueueFamilyIndex = 0;
    // One command pair per virtual source slot. Slot reuse is proven by the
    // generation timeline carried by the virtual-swapchain lifetime contract.
    std::uint32_t slotCount = 0;
};

ApusrFramegenResult apusrFramegenCreateVulkanPresentSubmitContext(
    const ApusrFramegenVulkanPresentSubmitDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanPresentSubmitContext** outContext) noexcept;

// Nonblocking begin. completedGenerationValue is the already-observed value of
// APUSR's generation timeline. If this source slot is unexpectedly still in
// flight, BUSY is returned instead of ever stalling the application thread.
ApusrFramegenResult apusrFramegenBeginVulkanSplitPresentSubmit(
    ApusrFramegenVulkanPresentSubmitContext* context,
    std::uint32_t slotIndex,
    std::uint64_t completedGenerationValue,
    void** outInputCommandBuffer,
    void** outGenerationCommandBuffer) noexcept;

// Clean-sheet off-screen proxy submit:
//   1. consume the application's binary render-finished semaphores once and
//      fan them out through producerReady;
//   2. Phase A waits producerReady and signals inputCompletion at the final use
//      of transient Streamline inputs;
//   3. Phase B follows by queue order on the common same-queue path, or waits
//      inputCompletion only when true cross-family async compute is active.
//
// On the same queue all three VkSubmitInfo objects are issued in one
// vkQueueSubmit host call. Cross-queue operation uses one input call containing
// fan-out + Phase A and one generation call. No per-slot fences or binary
// bridge semaphores exist in this path; the timelines are the lifetime truth.
ApusrFramegenResult apusrFramegenSubmitVulkanSplitOffscreenPresentWork(
    ApusrFramegenVulkanPresentSubmitContext* context,
    std::uint32_t slotIndex,
    void* const* applicationWaitSemaphores,
    std::uint32_t applicationWaitSemaphoreCount,
    void* generatedReuseTimelineSemaphore,
    std::uint64_t generatedReuseTimelineValue,
    // When the previous FI/OF dispatch is still executing on the generation
    // queue, Phase A for the next source waits for that generation value rather
    // than dropping the new frame. This preserves the two-frame FI history
    // without turning a few milliseconds of overlap into a skip+warmup pair.
    std::uint64_t priorGenerationCompletionValue,
    // Binary semaphore from a zero-timeout private WSI acquire when generation
    // writes directly into the presentation image. Null on output-ring fallback.
    void* directTargetAcquireSemaphore,
    std::uint64_t* outProducerReadyValue,
    std::uint64_t* outInputCompletionValue,
    std::uint64_t* outGenerationCompletionValue) noexcept;

void* apusrFramegenVulkanProducerReadySemaphore(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept;
// Value that the next successful split submit will signal on producerReady.
// Used to prearm mandatory source replay without consuming the application's
// binary render-finished semaphore twice.
std::uint64_t apusrFramegenVulkanNextProducerReadyValue(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept;

// Cold recovery for a source replay prearmed against the next producerReady
// value when CPU-side generation preparation subsequently fails. Consumes the
// application's binary render-finished waits and signals exactly that next
// timeline value, allowing mandatory source replay to complete without ever
// submitting the abandoned Phase A/B command buffers.
ApusrFramegenResult apusrFramegenSignalVulkanProducerReadyOnly(
    ApusrFramegenVulkanPresentSubmitContext* context,
    void* const* applicationWaitSemaphores,
    std::uint32_t applicationWaitSemaphoreCount,
    std::uint64_t* outProducerReadyValue) noexcept;
void* apusrFramegenVulkanInputCompletionSemaphore(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept;
void* apusrFramegenVulkanGenerationCompletionSemaphore(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept;

std::uint64_t apusrFramegenVulkanCompletedProducerReadyValue(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept;
std::uint64_t apusrFramegenVulkanCompletedInputValue(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept;
std::uint64_t apusrFramegenVulkanCompletedGenerationValue(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept;

// Host-wait for the application's render-finished fan-out timeline. This is
// the point at which Phase A becomes physically eligible to execute; presentation
// deadlines must not consume the generation budget before this value completes.
bool apusrFramegenWaitVulkanProducerReadyValue(
    const ApusrFramegenVulkanPresentSubmitContext* context,
    std::uint64_t value,
    std::uint64_t timeoutNanoseconds) noexcept;

// Host-wait for FI/OF completion without polling. timeoutNanoseconds is a
// real-time presentation budget, not a GPU scheduling dependency: VK_SUCCESS
// means the requested generation value is complete, while timeout/error both
// return false so the presenter can fail open to the mandatory source frame.
bool apusrFramegenWaitVulkanGenerationValue(
    const ApusrFramegenVulkanPresentSubmitContext* context,
    std::uint64_t value,
    std::uint64_t timeoutNanoseconds) noexcept;

void apusrFramegenAbortVulkanPresentSubmit(
    ApusrFramegenVulkanPresentSubmitContext* context,
    std::uint32_t slotIndex) noexcept;

void apusrFramegenDestroyVulkanPresentSubmitContext(
    ApusrFramegenVulkanPresentSubmitContext* context,
    const ApusrFramegenAllocator& allocator) noexcept;
