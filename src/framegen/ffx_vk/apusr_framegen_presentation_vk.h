// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_allocator.h"
#include "apusr_framegen_backend_interface.h"

#include <cstdint>

struct ApusrFramegenVulkanPresentation;

inline constexpr std::uint32_t APUSR_FRAMEGEN_VULKAN_MAX_PRESENT_WAITS = 64u;

using ApusrFramegenVulkanPresentImageCallback = std::int32_t (*)(
    void* userContext,
    void* queue,
    void* swapchain,
    std::uint32_t imageIndex,
    void* const* waitSemaphores,
    std::uint32_t waitSemaphoreCount,
    std::uint64_t frameId,
    bool profileCapture,
    bool generatedFrame) noexcept;

// Preferred source->generated pacing path. The callback queues the generated
// frame now but asks WSI to keep the preceding source image visible for at
// least minimumPreviousImageVisibleNs. If unsupported it must leave
// *outScheduled false without presenting so the worker can fall back to its
// precise host-timer path. This mirrors the source-relative callback below and
// lets both halves of a generated/source pair use the same display interval.
using ApusrFramegenVulkanPresentGeneratedRelativeCallback = std::int32_t (*)(
    void* userContext,
    void* queue,
    void* swapchain,
    std::uint32_t imageIndex,
    void* const* waitSemaphores,
    std::uint32_t waitSemaphoreCount,
    std::uint64_t frameId,
    bool profileCapture,
    std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled) noexcept;

// Preferred pacing path for a generated->source pair. The callback queues the
// mandatory source immediately but asks WSI to keep the preceding generated
// image visible for at least minimumPreviousImageVisibleNs. If unsupported it
// must leave *outScheduled false without presenting so the worker can use its
// precise host-timer fallback.
using ApusrFramegenVulkanPresentSourceRelativeCallback = std::int32_t (*)(
    void* userContext,
    void* queue,
    void* swapchain,
    std::uint32_t imageIndex,
    void* const* waitSemaphores,
    std::uint32_t waitSemaphoreCount,
    std::uint64_t frameId,
    bool profileCapture,
    std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled) noexcept;

using ApusrFramegenVulkanPrepareReplayCallback = ApusrFramegenResult (*)(
    void* userContext,
    std::uint32_t sourceSlotIndex,
    std::uint64_t completionValue,
    void* const* producerWaitSemaphores,
    std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore,
    std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex,
    void** outWaitSemaphore,
    bool* outCopied) noexcept;

// Nonblocking timeline query used by the real-time presenter. A slow FI/OF
// dispatch must never be placed as an unsatisfied wait on the physical WSI
// queue, because that would hold the real source frame behind generation.
using ApusrFramegenVulkanTimelineReadyCallback = bool (*)(
    void* userContext,
    void* timelineSemaphore,
    std::uint64_t value) noexcept;

// Efficient bounded host wait for the same generation timeline. This wakes as
// soon as FI/OF completes instead of sleeping blindly until the presentation
// deadline. False means timeout/error and therefore "present source now".
using ApusrFramegenVulkanTimelineWaitCallback = bool (*)(
    void* userContext,
    void* timelineSemaphore,
    std::uint64_t value,
    std::uint64_t timeoutNanoseconds) noexcept;

using ApusrFramegenVulkanGenerationDecisionCallback = void (*)(
    void* userContext,
    std::uint64_t frameId,
    std::uint32_t generationReplaySlotIndex,
    std::uint64_t generationCompletionValue,
    bool generatedFramePresented) noexcept;

using ApusrFramegenVulkanPrepareDirectPresentCallback = ApusrFramegenResult (*)(
    void* userContext,
    std::uint32_t imageIndex,
    void* generationTimelineSemaphore,
    std::uint64_t generationTimelineValue,
    void** outPresentWaitSemaphore) noexcept;

using ApusrFramegenVulkanDirectGenerationDecisionCallback = void (*)(
    void* userContext,
    std::uint64_t frameId,
    std::uint32_t imageIndex,
    void* generationTimelineSemaphore,
    std::uint64_t generationTimelineValue,
    bool generatedFramePresented,
    bool presentBridgePrepared) noexcept;

struct ApusrFramegenVulkanPresentationDescription {
    void* userContext = nullptr;
    ApusrFramegenVulkanPresentImageCallback presentImage = nullptr;
    ApusrFramegenVulkanPresentGeneratedRelativeCallback presentGeneratedRelative = nullptr;
    ApusrFramegenVulkanPresentSourceRelativeCallback presentSourceRelative = nullptr;
    // Full proxy hooks. Each callback privately acquires a real WSI image,
    // records the requested APUSR-owned image into it, and returns a binary
    // semaphore suitable for vkQueuePresentKHR. Generated replay additionally
    // waits the FI/OF completion timeline supplied by the schedule.
    ApusrFramegenVulkanPrepareReplayCallback prepareGeneratedReplay = nullptr;
    ApusrFramegenVulkanPrepareReplayCallback prepareSourceReplay = nullptr;
    ApusrFramegenVulkanTimelineReadyCallback timelineReady = nullptr;
    ApusrFramegenVulkanTimelineWaitCallback waitTimeline = nullptr;
    ApusrFramegenVulkanGenerationDecisionCallback generationDecision = nullptr;
    ApusrFramegenVulkanPrepareDirectPresentCallback prepareDirectPresent = nullptr;
    ApusrFramegenVulkanDirectGenerationDecisionCallback directGenerationDecision = nullptr;
    // Preallocated presenter admission bound. Proxy frontends should size this
    // to the maximum number of frames that can simultaneously retain either a
    // virtual source slot or a privately acquired real-WSI image. No memory is
    // allocated while scheduling a frame and queue pressure is never resolved
    // by an emergency full drain on the application thread.
    std::uint32_t maxPendingPresentCount = 16u;
    // Optional steady-state application lead bound. When non-zero, scheduling
    // waits on presenter retirement before accepting a task that would exceed
    // this many outstanding physical-source presentation tasks. This is
    // ordinary bounded WSI-style back-pressure, not midpoint pacing: the
    // application thread never sleeps to a generated-frame target and the
    // readiness/presentation workers remain asynchronous. Zero preserves the
    // unbounded-to-capacity behavior used by generic callers/tests.
    std::uint32_t maxApplicationPresentQueueDepth = 0u;
    // Used until two application presents establish an observed cadence.
    float initialSourceFrameTimeMs = 16.666667f;
};

struct ApusrFramegenVulkanPresentationSchedule {
    void* queue = nullptr;
    void* swapchain = nullptr;
    std::uint64_t frameId = 0;
    std::uint32_t sourceImageIndex = 0;
    void* const* sourceWaitSemaphores = nullptr;
    std::uint32_t sourceWaitSemaphoreCount = 0;

    bool hasGeneratedFrame = false;
    bool paceGeneratedFrame = true;
    // Keep the mandatory source on the active frame-generation display
    // cadence even when this particular source has no usable generated frame.
    // Frontends should set this only while FG is enabled; FG-off source replay
    // remains immediate until native swapchain restoration is implemented.
    bool paceSourceFrame = false;
    std::uint32_t generatedImageIndex = 0;
    void* generatedWaitSemaphore = nullptr;

    // Full off-screen proxy: generated output is APUSR-owned, so the worker
    // acquires a private real WSI image and replays this slot after the
    // generation timeline reaches generationCompletionValue.
    bool replayGeneratedFrame = false;
    // Preferred proxy path: generatedImageIndex is an already-acquired real WSI
    // storage target written directly by FI. The worker creates only the tiny
    // timeline->binary bridge after generation is proven ready.
    bool directGeneratedFrame = false;
    std::uint32_t generationReplaySlotIndex = 0;
    // The application's render-finished fan-out timeline. Phase A cannot begin
    // before this value completes, so a real-time generation relevance budget
    // is anchored here rather than at CPU-side Present interception.
    void* generationStartSemaphore = nullptr;
    std::uint64_t generationStartValue = 0;
    // FI/OF completion dependency.
    void* generationCompletionSemaphore = nullptr;
    std::uint64_t generationCompletionValue = 0;
    // Optional pre-reserved generated-replay completion value. Production
    // fail-open scheduling leaves this zero and reserves only if generation is
    // actually ready before the presentation deadline.
    std::uint64_t generatedReplayCompletionValue = 0;

    // Proxy path: sourceImageIndex names a retained virtual source, not a WSI
    // image. The worker asks prepareSourceReplay for a freshly acquired real
    // WSI target and its ready semaphore. sourceWaitSemaphores are consumed by
    // the replay copy on source-only frames, avoiding a no-op bridge submit.
    bool replaySourceFrame = false;
    std::uint32_t sourceReplaySlotIndex = 0;
    std::uint64_t sourceReplayCompletionValue = 0;
    // Producer-ready dependency for generated proxy schedules. It is the
    // timeline fan-out of the application's render-finished binary semaphores,
    // allowing source replay and Phase A to consume producer completion
    // independently and concurrently. Source-only schedules instead pass the
    // application binary semaphores directly in sourceWaitSemaphores.
    void* sourceReplayUpstreamSemaphore = nullptr;
    std::uint64_t sourceReplayUpstreamValue = 0;

    // Observed upstream source-production delta. Frontends should prefer a
    // marker/timestamp captured before proxy presentation (for Streamline,
    // PCL PresentStart) so the pacer cannot measure its own WSI delays.
    float sourceFrameTimeMs = 0.0f;
};

ApusrFramegenResult apusrFramegenCreateVulkanPresentation(
    const ApusrFramegenVulkanPresentationDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanPresentation** outPresentation) noexcept;

// Copies every semaphore handle into fixed internal storage before returning.
// No allocation occurs on this per-frame path.
ApusrFramegenResult apusrFramegenScheduleVulkanPresentation(
    ApusrFramegenVulkanPresentation* presentation,
    const ApusrFramegenVulkanPresentationSchedule& schedule) noexcept;

// Waits until every already-scheduled present has completed. Intended only for
// mode/swapchain teardown, never the steady-state frame path.
void apusrFramegenDrainVulkanPresentation(
    ApusrFramegenVulkanPresentation* presentation) noexcept;

// Returns and clears a deferred negative source-present result. This lets an
// asynchronous presenter report OUT_OF_DATE/device-loss style failures to the
// application on its next present call without making normal presentation
// synchronous again.
std::int32_t apusrFramegenTakeVulkanPresentationError(
    ApusrFramegenVulkanPresentation* presentation) noexcept;

void apusrFramegenDestroyVulkanPresentation(
    ApusrFramegenVulkanPresentation* presentation,
    const ApusrFramegenAllocator& allocator) noexcept;
