// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_presentation_vk.h"

#include <apusr/apusr_framegen.h>
#include <apusr/apusr_profiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr std::int32_t kVkSuccess = 0;
constexpr std::int32_t kVkSuboptimalKhr = 1000001003;
constexpr std::int32_t kVkErrorInitializationFailed = -3;

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

constexpr Nanoseconds kMinSourceFrameInterval = std::chrono::milliseconds(2);
constexpr Nanoseconds kMaxSourceFrameInterval = std::chrono::milliseconds(100);
constexpr std::int64_t kSteadyClockFrequency = 1'000'000'000;
constexpr double kPacingSafetyMarginSeconds = 0.0001;
constexpr double kPacingVarianceFactor = 0.1;

[[nodiscard]] bool validSourceFrameInterval(const Nanoseconds interval) noexcept
{
    return interval >= kMinSourceFrameInterval && interval < kMaxSourceFrameInterval;
}

struct PresentTask {
    void* queue = nullptr;
    void* swapchain = nullptr;
    std::uint64_t frameId = 0;
    std::uint32_t sourceImageIndex = 0;
    std::array<void*, APUSR_FRAMEGEN_VULKAN_MAX_PRESENT_WAITS> sourceWaitSemaphores{};
    std::uint32_t sourceWaitSemaphoreCount = 0;
    bool hasGeneratedFrame = false;
    bool paceGeneratedFrame = true;
    bool paceSourceFrame = false;
    std::uint32_t generatedImageIndex = 0;
    void* generatedWaitSemaphore = nullptr;
    bool replayGeneratedFrame = false;
    bool directGeneratedFrame = false;
    std::uint32_t generationReplaySlotIndex = 0;
    void* generationStartSemaphore = nullptr;
    std::uint64_t generationStartValue = 0u;
    void* generationCompletionSemaphore = nullptr;
    std::uint64_t generationCompletionValue = 0u;
    std::uint64_t generatedReplayCompletionValue = 0u;
    bool replaySourceFrame = false;
    std::uint32_t sourceReplaySlotIndex = 0;
    std::uint64_t sourceReplayCompletionValue = 0u;
    void* sourceReplayUpstreamSemaphore = nullptr;
    std::uint64_t sourceReplayUpstreamValue = 0u;
    // Generation may consume up to one smoothed source-display interval. This
    // is an admission/relevance budget only; it is deliberately not the ideal
    // generated-frame midpoint. A generated frame that finishes after the
    // midpoint can still form a valid G->S pair if it completes within this
    // source-period budget.
    Clock::time_point generationDeadline{};
    Clock::time_point sourceTarget{};
    Nanoseconds sourceFrameInterval{};
    Nanoseconds generatedVisibleDuration{};
    Nanoseconds generationBudget{};
    bool generationReady = true;
    bool generationDeadlineMiss = false;
    bool profileCapture = false;
    std::uint64_t profileSampleSequence = 0u;
    std::uint64_t profileScheduledCpuTick = 0u;
    std::uint64_t profileReadinessCompletedCpuTick = 0u;
    std::uint64_t profileSubmitSequence = 0u;
};

bool acceptedPresentResult(const std::int32_t result) noexcept
{
    return result == kVkSuccess || result == kVkSuboptimalKhr;
}

struct PrecisionWaiter {
#if defined(_WIN32)
    HANDLE timer = nullptr;

    PrecisionWaiter() noexcept
    {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer)
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0u, TIMER_MODIFY_STATE | SYNCHRONIZE);
    }

    ~PrecisionWaiter() noexcept
    {
        if (timer)
            CloseHandle(timer);
    }
#endif

    void waitUntil(const Clock::time_point target) noexcept
    {
        // Leave only a very small scheduler-jitter tail to active waiting. A
        // high-resolution Windows waitable timer keeps the presenter precise
        // without burning a CPU core for hundreds of microseconds every frame.
        const auto spinWindow = std::chrono::microseconds(50);
        const auto coarseTarget = target - spinWindow;
        const auto now = Clock::now();
        if (now < coarseTarget) {
#if defined(_WIN32)
            bool waited = false;
            if (timer) {
                const auto remaining100ns = std::chrono::duration_cast<
                    std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>>(coarseTarget - now).count();
                LARGE_INTEGER due{};
                due.QuadPart = -std::max<std::int64_t>(remaining100ns, 1);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    (void)WaitForSingleObject(timer, INFINITE);
                    waited = true;
                }
            }
            if (!waited)
                std::this_thread::sleep_until(coarseTarget);
#else
            std::this_thread::sleep_until(coarseTarget);
#endif
        }
        while (Clock::now() < target)
            std::this_thread::yield();
    }
};

} // namespace

struct ApusrFramegenVulkanPresentation {
    ApusrFramegenVulkanPresentationDescription description{};
    PresentTask* tasks = nullptr;
    PresentTask* readyTasks = nullptr;
    std::size_t taskCapacity = 0u;
    std::mutex mutex;
    std::condition_variable readinessWake;
    std::condition_variable presenterWake;
    std::condition_variable applicationLeadWake;
    std::condition_variable drained;
    std::thread readinessWorker;
    std::thread presenterWorker;
    std::size_t head = 0;
    std::size_t count = 0;
    std::size_t readyHead = 0;
    std::size_t readyCount = 0;
    std::size_t outstanding = 0;
    bool readinessActive = false;
    bool presenterActive = false;
    bool readinessExited = false;
    bool shutdown = false;
    Clock::time_point previousSchedule{};
    Nanoseconds sourceFrameInterval{std::chrono::microseconds(16667)};
    Nanoseconds pacedPresentInterval{std::chrono::microseconds(8333)};
    apusr::framegen::PacingEstimator pacingEstimator{};
    std::uint32_t pacingEstimatorSamples = 0u;
    // Worker-owned estimate of the most recent physical source-frame display
    // point. It is deliberately pair-local: every new generated/source pair
    // starts from the source frame that actually preceded it. There is no
    // persistent midpoint lattice to drag future frames after a late pair.
    Clock::time_point previousPhysicalSourcePresent{};
    std::atomic<std::int32_t> deferredSourceError{kVkSuccess};
    apusr::profile::StreamHandle profileStream{};
    apusr::profile::SampleGate profileSampleGate{};
    std::uint64_t profileSubmitSequence = 0u;
};

namespace {

[[nodiscard]] Nanoseconds updatePacedPresentInterval(
    ApusrFramegenVulkanPresentation& presentation,
    const Nanoseconds observedSourceInterval) noexcept
{
    if (observedSourceInterval >= kMaxSourceFrameInterval) {
        // AMD's swapchain drops its short pacing history across large hitches.
        // Keep the last useful half-frame interval instead of turning a hitch
        // into several giant generated/source display intervals while the
        // estimator warms again.
        presentation.pacingEstimator.reset();
        presentation.pacingEstimatorSamples = 0u;
        return presentation.pacedPresentInterval;
    }
    if (!validSourceFrameInterval(observedSourceInterval))
        return presentation.pacedPresentInterval;

    presentation.pacingEstimator.update(static_cast<double>(observedSourceInterval.count()));
    ++presentation.pacingEstimatorSamples;

    if (presentation.pacingEstimatorSamples < 10u) {
        // Do not let one startup/hitch sample become a giant visible G/S gap.
        // The full 10-sample estimator is intentionally conservative; during
        // warm-up, follow the latest half-period but bound each adjustment to
        // 0.5x..2x the previous pacing interval. This converges immediately
        // from 60-Hz initialization to ordinary 30-FPS source cadence while a
        // 90-ms outlier can no longer create a ~45-ms half-frame stall.
        const Nanoseconds candidate = observedSourceInterval / 2;
        const Nanoseconds minimum = std::max(
            Nanoseconds(1), presentation.pacedPresentInterval / 2);
        const Nanoseconds maximum = presentation.pacedPresentInterval * 2;
        presentation.pacedPresentInterval = std::clamp(candidate, minimum, maximum);
        return presentation.pacedPresentInterval;
    }

    const std::uint64_t targetNs = presentation.pacingEstimator.targetDelta(
        kSteadyClockFrequency,
        kPacingSafetyMarginSeconds,
        kPacingVarianceFactor);
    if (targetNs != 0u)
        presentation.pacedPresentInterval = Nanoseconds(static_cast<std::int64_t>(targetNs));
    return presentation.pacedPresentInterval;
}

struct PresentationProfile final {
    apusr::profile::StreamHandle stream{};
    std::uint64_t frameId = 0u;
    std::uint64_t sampleSequence = 0u;
    std::uint64_t submitSequence = 0u;
    bool capture = false;

    void queue(
        const char* name,
        const apusr::profile::QueueOperation operation,
        void* nativeQueue,
        void* syncObject = nullptr,
        const std::uint64_t syncValue = 0u,
        const std::uint32_t objectCount = 0u,
        const std::uint32_t flags = 0u) const noexcept
    {
        if (!capture || !stream)
            return;
        apusr::profile::recordQueueEvent({
            .stream = stream,
            .frameId = frameId,
            .sampleSequence = sampleSequence,
            .name = name,
            .nativeQueue = reinterpret_cast<std::uintptr_t>(nativeQueue),
            .syncObject = reinterpret_cast<std::uintptr_t>(syncObject),
            .syncValue = syncValue,
            .submitSequence = submitSequence,
            .operation = operation,
            .objectCount = objectCount,
            .flags = flags,
        });
    }

    void presentWaits(
        const char* presentName,
        const char* waitName,
        void* nativeQueue,
        void* const* waits,
        const std::uint32_t waitCount,
        const bool generated) const noexcept
    {
        if (!capture)
            return;
        queue(
            presentName,
            apusr::profile::QueueOperation::Present,
            nativeQueue,
            nullptr,
            0u,
            waitCount,
            generated ? 1u : 0u);
        for (std::uint32_t i = 0u; i < waitCount; ++i) {
            queue(
                waitName,
                apusr::profile::QueueOperation::Wait,
                nativeQueue,
                waits ? waits[i] : nullptr,
                0u,
                1u,
                generated ? 1u : 0u);
        }
    }
};

[[nodiscard]] PresentationProfile presentationProfile(
    const ApusrFramegenVulkanPresentation& presentation,
    const PresentTask& task) noexcept
{
    return {
        .stream = presentation.profileStream,
        .frameId = task.frameId,
        .sampleSequence = task.profileSampleSequence,
        .submitSequence = task.profileSubmitSequence,
        .capture = task.profileCapture,
    };
}

void recordDeferredPresentError(
    const PresentationProfile& profile,
    const char* stage,
    const std::int32_t result,
    const std::uint64_t context = 0u) noexcept
{
    if (!profile.stream || result >= 0)
        return;
    apusr::profile::recordError(
        profile.stream,
        profile.frameId,
        stage,
        apusr::StatusCode::RuntimeFailure,
        "Vulkan frame-generation presentation failed.",
        static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
        context);
}

[[nodiscard]] bool presentationTimelineReady(
    const ApusrFramegenVulkanPresentation& presentation,
    void* semaphore,
    const std::uint64_t value) noexcept
{
    return presentation.description.timelineReady && semaphore && value != 0u &&
        presentation.description.timelineReady(
            presentation.description.userContext, semaphore, value);
}

[[nodiscard]] bool generationTimelineReady(
    const ApusrFramegenVulkanPresentation& presentation,
    const PresentTask& task,
    const PresentationProfile& profile) noexcept
{
    apusr::profile::CpuScope scope(
        profile.capture ? profile.stream : apusr::profile::StreamHandle{},
        profile.frameId,
        profile.sampleSequence,
        "presentation.generation_ready_check");
    return presentationTimelineReady(
        presentation, task.generationCompletionSemaphore, task.generationCompletionValue);
}

void generationReadinessWorker(ApusrFramegenVulkanPresentation* presentation) noexcept
{
    bool profileThreadRecorded = false;
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"APUSR Vulkan FI Readiness");
#endif

    for (;;) {
        PresentTask task{};
        {
            std::unique_lock lock(presentation->mutex);
            presentation->readinessWake.wait(lock, [&] {
                return presentation->shutdown || presentation->count != 0u;
            });
            if (presentation->shutdown && presentation->count == 0u)
                break;
            task = presentation->tasks[presentation->head];
            presentation->head = (presentation->head + 1u) % presentation->taskCapacity;
            --presentation->count;
            presentation->readinessActive = true;
        }

        const PresentationProfile profile = presentationProfile(*presentation, task);
        if (!profileThreadRecorded && presentation->profileStream) {
            apusr::profile::recordThreadInfo({
                .stream = presentation->profileStream,
                .name = "APUSR Vulkan FI Readiness",
                .role = "framegen.readiness",
#if defined(_WIN32)
                .priority = THREAD_PRIORITY_HIGHEST,
#endif
            });
            profileThreadRecorded = true;
        }
        const std::uint64_t readinessStartTick = profile.capture ? apusr::profile::cpuTick() : 0u;
        if (profile.capture && task.profileScheduledCpuTick != 0u &&
            readinessStartTick >= task.profileScheduledCpuTick) {
            apusr::profile::recordCpuInterval({
                .stream = profile.stream,
                .frameId = profile.frameId,
                .sampleSequence = profile.sampleSequence,
                .startCpuTick = task.profileScheduledCpuTick,
                .endCpuTick = readinessStartTick,
                .name = "presentation.readiness_queue_delay",
            });
        }
        apusr::profile::CpuScope readinessScope(
            profile.capture ? profile.stream : apusr::profile::StreamHandle{},
            profile.frameId,
            profile.sampleSequence,
            "presentation.readiness_task");

        const bool timelineBoundGeneration = task.hasGeneratedFrame &&
            (task.replayGeneratedFrame || task.directGeneratedFrame) &&
            task.generationCompletionSemaphore && task.generationCompletionValue != 0u;
        task.generationReady = task.hasGeneratedFrame;
        task.generationDeadlineMiss = false;
        if (timelineBoundGeneration) {
            task.generationReady = generationTimelineReady(*presentation, task, profile);
            if (!task.generationReady && task.paceGeneratedFrame) {
                // The generation budget measures the work APUSR can influence.
                // CPU Present can arrive while the game's render-finished binary
                // is still unsignaled; Phase A is physically unable to execute
                // until producerReady completes. Do not burn the entire relevance
                // budget waiting for work that has not become runnable yet.
                const bool startBoundGeneration = task.generationStartSemaphore &&
                    task.generationStartValue != 0u;
                if (startBoundGeneration && !presentationTimelineReady(
                        *presentation, task.generationStartSemaphore, task.generationStartValue)) {
                    if (!presentation->description.waitTimeline) {
                        task.generationDeadlineMiss = true;
                    } else {
                        apusr::profile::WaitScope waitScope(
                            profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                            profile.frameId,
                            profile.sampleSequence,
                            "presentation.wait_generation_start_timeline",
                            apusr::profile::WaitKind::TimelineSemaphore,
                            reinterpret_cast<std::uintptr_t>(task.generationStartSemaphore),
                            task.generationStartValue,
                            0u,
                            "producer_ready");
                        // Producer readiness has no FI deadline of its own: the source frame
                        // cannot be physically consumed before this dependency resolves either.
                        // Wait in bounded host chunks so teardown can still interrupt a title
                        // that abandons an unsignaled render-finished semaphore.
                        bool startReady = false;
                        constexpr std::uint64_t kProducerReadyPollNs = 1'000'000u;
                        while (!startReady) {
                            startReady = presentation->description.waitTimeline(
                                presentation->description.userContext,
                                task.generationStartSemaphore,
                                task.generationStartValue,
                                kProducerReadyPollNs);
                            if (startReady)
                                break;
                            std::lock_guard lock(presentation->mutex);
                            if (presentation->shutdown)
                                break;
                        }
                        waitScope.setResult(
                            startReady ? apusr::profile::WaitResult::Satisfied
                                       : apusr::profile::WaitResult::Interrupted,
                            startReady ? task.generationStartValue : 0u);
                        if (!startReady)
                            task.generationDeadlineMiss = true;
                    }
                }

                if (!task.generationDeadlineMiss) {
                    if (startBoundGeneration)
                        task.generationDeadline = Clock::now() + task.generationBudget;

                    // This stage deliberately knows nothing about the physical
                    // presentation phase. Its only job is to get FI readiness off
                    // the presenter thread while the current G->S pair is being
                    // paced. Display-space admission is re-checked by the presenter.
                    const auto safetyMargin = task.directGeneratedFrame
                        ? std::chrono::microseconds(100)
                        : std::chrono::microseconds(500);
                    const Clock::time_point deadline =
                        task.generationDeadline > Clock::time_point{} + safetyMargin
                        ? task.generationDeadline - safetyMargin
                        : task.generationDeadline;
                    const Clock::time_point readinessCheck = Clock::now();
                    if (readinessCheck >= deadline) {
                        task.generationDeadlineMiss = true;
                    } else if (presentation->description.waitTimeline) {
                        const auto timeout = std::chrono::duration_cast<Nanoseconds>(deadline - readinessCheck);
                        apusr::profile::WaitScope waitScope(
                            profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                            profile.frameId,
                            profile.sampleSequence,
                            "presentation.wait_generation_timeline",
                            apusr::profile::WaitKind::TimelineSemaphore,
                            reinterpret_cast<std::uintptr_t>(task.generationCompletionSemaphore),
                            task.generationCompletionValue,
                            static_cast<std::uint64_t>(std::max<std::int64_t>(timeout.count(), 0)),
                            "generation_completion");
                        task.generationReady = presentation->description.waitTimeline(
                            presentation->description.userContext,
                            task.generationCompletionSemaphore,
                            task.generationCompletionValue,
                            static_cast<std::uint64_t>(std::max<std::int64_t>(timeout.count(), 0)));
                        const bool generationSatisfied = task.generationReady && Clock::now() < deadline;
                        waitScope.setResult(
                            generationSatisfied ? apusr::profile::WaitResult::Satisfied
                                                : apusr::profile::WaitResult::Timeout,
                            generationSatisfied ? task.generationCompletionValue : 0u);
                        if (!generationSatisfied) {
                            task.generationReady = false;
                            task.generationDeadlineMiss = true;
                        }
                    } else {
                        PrecisionWaiter waiter{};
                        {
                            const auto timerBegin = Clock::now();
                            const auto timerTimeout = deadline > timerBegin
                                ? std::chrono::duration_cast<Nanoseconds>(deadline - timerBegin)
                                : Nanoseconds::zero();
                            apusr::profile::WaitScope waitScope(
                                profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                                profile.frameId,
                                profile.sampleSequence,
                                "presentation.wait_generation_deadline",
                                apusr::profile::WaitKind::Timer,
                                0u,
                                0u,
                                static_cast<std::uint64_t>(std::max<std::int64_t>(timerTimeout.count(), 0)),
                                "generation_deadline");
                            waiter.waitUntil(deadline);
                            waitScope.setResult(apusr::profile::WaitResult::Satisfied);
                        }
                        task.generationReady = generationTimelineReady(*presentation, task, profile);
                        if (!task.generationReady || Clock::now() >= deadline) {
                            task.generationReady = false;
                            task.generationDeadlineMiss = true;
                        }
                    }
                }
            }
        }

        if (profile.capture && timelineBoundGeneration && task.generationDeadlineMiss) {
            apusr::profile::recordCounter({
                .stream = profile.stream,
                .frameId = profile.frameId,
                .name = "presentation.generation_deadline_miss",
                .value = 1u,
                .value2 = task.generationCompletionValue,
                .flags = task.directGeneratedFrame ? 1u : (task.replayGeneratedFrame ? 2u : 0u),
            });
        }
        task.profileReadinessCompletedCpuTick = profile.capture ? apusr::profile::cpuTick() : 0u;

        {
            std::lock_guard lock(presentation->mutex);
            const std::size_t tail =
                (presentation->readyHead + presentation->readyCount) % presentation->taskCapacity;
            presentation->readyTasks[tail] = task;
            ++presentation->readyCount;
            presentation->readinessActive = false;
            presentation->presenterWake.notify_one();
        }
    }

    {
        std::lock_guard lock(presentation->mutex);
        presentation->readinessActive = false;
        presentation->readinessExited = true;
        presentation->presenterWake.notify_all();
    }
}

void presentationWorker(ApusrFramegenVulkanPresentation* presentation) noexcept
{
    bool profileThreadRecorded = false;
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"APUSR Vulkan Presenter");
#endif
    PrecisionWaiter precisionWaiter{};

    for (;;) {
        PresentTask task{};
        {
            std::unique_lock lock(presentation->mutex);
            presentation->presenterWake.wait(lock, [&] {
                return presentation->readyCount != 0u ||
                    (presentation->shutdown && presentation->readinessExited);
            });
            if (presentation->readyCount == 0u &&
                presentation->shutdown && presentation->readinessExited)
                break;
            task = presentation->readyTasks[presentation->readyHead];
            presentation->readyHead = (presentation->readyHead + 1u) % presentation->taskCapacity;
            --presentation->readyCount;
            presentation->presenterActive = true;
        }

        const PresentationProfile profile = presentationProfile(*presentation, task);
        if (!profileThreadRecorded && presentation->profileStream) {
            apusr::profile::recordThreadInfo({
                .stream = presentation->profileStream,
                .name = "APUSR Vulkan Presenter",
                .role = "framegen.presenter",
#if defined(_WIN32)
                .priority = THREAD_PRIORITY_HIGHEST,
#endif
            });
            profileThreadRecorded = true;
        }
        const std::uint64_t workerStartTick = profile.capture ? apusr::profile::cpuTick() : 0u;
        if (profile.capture && task.profileScheduledCpuTick != 0u &&
            workerStartTick >= task.profileScheduledCpuTick) {
            apusr::profile::recordCpuInterval({
                .stream = profile.stream,
                .frameId = profile.frameId,
                .sampleSequence = profile.sampleSequence,
                .startCpuTick = task.profileScheduledCpuTick,
                .endCpuTick = workerStartTick,
                .name = "presentation.worker_queue_delay",
            });
        }
        if (profile.capture && task.profileReadinessCompletedCpuTick != 0u &&
            workerStartTick >= task.profileReadinessCompletedCpuTick) {
            apusr::profile::recordCpuInterval({
                .stream = profile.stream,
                .frameId = profile.frameId,
                .sampleSequence = profile.sampleSequence,
                .startCpuTick = task.profileReadinessCompletedCpuTick,
                .endCpuTick = workerStartTick,
                .name = "presentation.presenter_queue_delay",
            });
        }
        apusr::profile::CpuScope workerScope(
            profile.capture ? profile.stream : apusr::profile::StreamHandle{},
            profile.frameId,
            profile.sampleSequence,
            "presentation.worker_task");

        // Prepare real source replay independently of generation. Generated
        // schedules wait only on the producer-ready fan-out timeline, so this
        // mandatory WSI copy can overlap Phase A and never waits on heavy FI/OF.
        std::uint32_t sourceImageIndex = task.sourceImageIndex;
        std::array<void*, APUSR_FRAMEGEN_VULKAN_MAX_PRESENT_WAITS> sourceWaits = task.sourceWaitSemaphores;
        std::uint32_t sourceWaitCount = task.sourceWaitSemaphoreCount;
        bool sourceReady = true;
        if (task.replaySourceFrame) {
            apusr::profile::CpuScope sourceReplayScope(
                profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                profile.frameId,
                profile.sampleSequence,
                "presentation.prepare_source_replay");
            sourceReady = false;
            if (presentation->description.prepareSourceReplay) {
                void* replayWait = nullptr;
                bool sourceCopied = false;
                const ApusrFramegenResult replayResult = presentation->description.prepareSourceReplay(
                        presentation->description.userContext,
                        task.sourceReplaySlotIndex,
                        task.sourceReplayCompletionValue,
                        task.sourceWaitSemaphoreCount ? task.sourceWaitSemaphores.data() : nullptr,
                        task.sourceWaitSemaphoreCount,
                        task.sourceReplayUpstreamSemaphore,
                        task.sourceReplayUpstreamValue,
                        &sourceImageIndex,
                        &replayWait,
                        &sourceCopied);
                if (replayResult == APUSR_FRAMEGEN_OK && replayWait) {
                    sourceWaits = {};
                    sourceWaits[0] = replayWait;
                    sourceWaitCount = 1u;
                    sourceReady = true;
                    if (!sourceCopied) {
                        // The replay context still owns an acquired WSI image;
                        // present its retirement semaphore before surfacing the
                        // failure so teardown cannot strand WSI ownership.
                        presentation->deferredSourceError.store(
                            kVkErrorInitializationFailed, std::memory_order_release);
                        if (profile.stream) {
                            apusr::profile::recordError(
                                profile.stream,
                                profile.frameId,
                                "presentation.prepare_source_replay.copy",
                                apusr::StatusCode::RuntimeFailure,
                                "Vulkan source replay retained WSI ownership but did not complete its copy.",
                                static_cast<std::uint64_t>(APUSR_FRAMEGEN_ERROR_BACKEND_API),
                                task.sourceReplaySlotIndex);
                        }
                    }
                } else {
                    presentation->deferredSourceError.store(
                        kVkErrorInitializationFailed, std::memory_order_release);
                    if (profile.stream) {
                        apusr::profile::recordError(
                            profile.stream,
                            profile.frameId,
                            "presentation.prepare_source_replay",
                            apusr::StatusCode::RuntimeFailure,
                            "Vulkan source replay preparation failed.",
                            static_cast<std::uint64_t>(replayResult),
                            task.sourceReplaySlotIndex);
                    }
                }
            }
        }

        // A real-time frame generator is fail-open by definition: source
        // presentation is mandatory, generated presentation is opportunistic.
        // Never enqueue an unsatisfied generation-timeline wait on the physical
        // WSI queue. That mistake lets one slow FI/OF dispatch stall both the
        // generated frame and the real source frame behind it.
        std::int32_t generatedResult = kVkSuccess;
        bool generatedAttempted = false;
        bool directPresentBridgePrepared = false;
        Clock::time_point generatedVisibleAt{};

        const auto presentGeneratedImage = [&] (
            const std::uint32_t imageIndex,
            void* waitSemaphore,
            const char* presentName,
            const char* waitName,
            const char* cpuScopeName) noexcept -> std::int32_t {
            const bool paced = task.paceGeneratedFrame &&
                task.generatedVisibleDuration > Nanoseconds::zero();
            // The ideal generated slot is pair-local: one half interval after
            // the preceding physical source. It is a pacing target, not an
            // eligibility deadline. If FI becomes ready after this point but
            // still within the full source-period generation budget, present G
            // immediately and start the real frame's half interval from G.
            const Clock::time_point target = paced &&
                presentation->previousPhysicalSourcePresent != Clock::time_point{}
                ? presentation->previousPhysicalSourcePresent + task.generatedVisibleDuration
                : Clock::time_point{};
            const Clock::time_point targetCheck = Clock::now();
            if (profile.capture && target != Clock::time_point{}) {
                const auto late = targetCheck > target
                    ? std::chrono::duration_cast<Nanoseconds>(targetCheck - target).count()
                    : 0;
                apusr::profile::recordCounter({
                    .stream = profile.stream,
                    .frameId = profile.frameId,
                    .name = "presentation.generated_target_late_ns",
                    .value = static_cast<std::uint64_t>(std::max<std::int64_t>(late, 0)),
                    .value2 = static_cast<std::uint64_t>(
                        std::max<std::int64_t>(task.generatedVisibleDuration.count(), 0)),
                });
            }

            // Relative WSI timing describes the minimum visibility of the
            // previous physical image. Keep using it even when CPU-side G is
            // late: if the previous source has already satisfied the half-frame
            // duration, WSI may present G immediately. This preserves native
            // display timing without turning the midpoint into a drop deadline.
            if (target != Clock::time_point{} &&
                presentation->description.presentGeneratedRelative) {
                bool scheduled = false;
                profile.presentWaits(
                    "presentation.present_generated_relative",
                    "presentation.present_generated_relative.wait",
                    task.queue,
                    &waitSemaphore,
                    1u,
                    true);
                apusr::profile::CpuScope relativeScope(
                    profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                    profile.frameId,
                    profile.sampleSequence,
                    "presentation.present_generated_relative");
                const std::int32_t relativeResult =
                    presentation->description.presentGeneratedRelative(
                        presentation->description.userContext,
                        task.queue,
                        task.swapchain,
                        imageIndex,
                        &waitSemaphore,
                        1u,
                        profile.frameId,
                        profile.capture,
                        static_cast<std::uint64_t>(task.generatedVisibleDuration.count()),
                        &scheduled);
                if (scheduled) {
                    generatedVisibleAt = std::max(target, Clock::now());
                    if (profile.capture) {
                        apusr::profile::recordCounter({
                            .stream = profile.stream,
                            .frameId = profile.frameId,
                            .name = "presentation.generated_wsi_timed",
                            .value = 1u,
                            .value2 = static_cast<std::uint64_t>(task.generatedVisibleDuration.count()),
                        });
                    }
                    return relativeResult;
                }
                if (relativeResult < 0)
                    return relativeResult;
            }

            if (target != Clock::time_point{} && Clock::now() < target) {
                const auto waitBegin = Clock::now();
                const auto waitTimeout = target > waitBegin
                    ? std::chrono::duration_cast<Nanoseconds>(target - waitBegin)
                    : Nanoseconds::zero();
                apusr::profile::WaitScope waitScope(
                    profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                    profile.frameId,
                    profile.sampleSequence,
                    "presentation.wait_generated_target",
                    apusr::profile::WaitKind::Timer,
                    0u,
                    0u,
                    static_cast<std::uint64_t>(std::max<std::int64_t>(waitTimeout.count(), 0)),
                    "generated_target");
                precisionWaiter.waitUntil(target);
                waitScope.setResult(apusr::profile::WaitResult::Satisfied);
            }

            profile.presentWaits(
                presentName,
                waitName,
                task.queue,
                &waitSemaphore,
                1u,
                true);
            apusr::profile::CpuScope presentScope(
                profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                profile.frameId,
                profile.sampleSequence,
                cpuScopeName);
            generatedVisibleAt = Clock::now();
            return presentation->description.presentImage(
                presentation->description.userContext,
                task.queue,
                task.swapchain,
                imageIndex,
                &waitSemaphore,
                1u,
                profile.frameId,
                profile.capture,
                true);
        };

        if (task.hasGeneratedFrame) {
            // FI readiness is resolved by a dedicated high-priority worker while
            // the presenter is pacing the previous pair. This mirrors AMD's
            // interpolation-thread/presenter-thread split and removes the
            // half-frame head-of-line delay visible in profiling.
            bool generationReady = task.generationReady;

            if (generationReady) {
                generatedAttempted = true;
                if (task.replayGeneratedFrame) {
                    generatedResult = kVkErrorInitializationFailed;
                    if (presentation->description.prepareGeneratedReplay) {
                        std::uint32_t generatedImageIndex = 0u;
                        void* generatedWait = nullptr;
                        bool generatedCopied = false;
                        ApusrFramegenResult replayResult = APUSR_FRAMEGEN_ERROR_BACKEND_API;
                        {
                            // Measure replay preparation itself. Pacing/present
                            // belongs to its own scopes and must not inflate this
                            // number into a fake multi-millisecond copy cost.
                            apusr::profile::CpuScope generatedReplayScope(
                                profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                                profile.frameId,
                                profile.sampleSequence,
                                "presentation.prepare_generated_replay");
                            replayResult = presentation->description.prepareGeneratedReplay(
                                presentation->description.userContext,
                                task.generationReplaySlotIndex,
                                task.generatedReplayCompletionValue,
                                nullptr,
                                0u,
                                task.generationCompletionSemaphore,
                                task.generationCompletionValue,
                                &generatedImageIndex,
                                &generatedWait,
                                &generatedCopied);
                        }
                        if (replayResult == APUSR_FRAMEGEN_OK && generatedWait) {
                            generatedResult = presentGeneratedImage(
                                generatedImageIndex,
                                generatedWait,
                                "presentation.present_generated",
                                "presentation.present_generated.wait",
                                "presentation.present_generated");
                            if (!generatedCopied) {
                                presentation->deferredSourceError.store(
                                    kVkErrorInitializationFailed, std::memory_order_release);
                                if (profile.stream) {
                                    apusr::profile::recordError(
                                        profile.stream,
                                        profile.frameId,
                                        "presentation.prepare_generated_replay.copy",
                                        apusr::StatusCode::RuntimeFailure,
                                        "Vulkan generated replay retained WSI ownership but did not complete its copy.",
                                        static_cast<std::uint64_t>(APUSR_FRAMEGEN_ERROR_BACKEND_API),
                                        task.generationReplaySlotIndex);
                                }
                            }
                        } else if (replayResult == APUSR_FRAMEGEN_OK) {
                            // No private WSI image was immediately available.
                            // Treat this as a deliberate generated-frame drop,
                            // not an error and never as a reason to delay source.
                            generatedAttempted = false;
                            generatedResult = kVkSuccess;
                        } else if (profile.stream) {
                            apusr::profile::recordError(
                                profile.stream,
                                profile.frameId,
                                "presentation.prepare_generated_replay",
                                apusr::StatusCode::RuntimeFailure,
                                "Vulkan generated replay preparation failed.",
                                static_cast<std::uint64_t>(replayResult),
                                task.generationReplaySlotIndex);
                        }
                    }
                } else if (task.directGeneratedFrame) {
                    generatedResult = kVkErrorInitializationFailed;
                    if (presentation->description.prepareDirectPresent) {
                        void* generatedWait = nullptr;
                        ApusrFramegenResult bridgeResult = APUSR_FRAMEGEN_ERROR_BACKEND_API;
                        {
                            apusr::profile::CpuScope directBridgeScope(
                                profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                                profile.frameId,
                                profile.sampleSequence,
                                "presentation.prepare_direct_present");
                            bridgeResult = presentation->description.prepareDirectPresent(
                                presentation->description.userContext,
                                task.generatedImageIndex,
                                task.generationCompletionSemaphore,
                                task.generationCompletionValue,
                                &generatedWait);
                        }
                        if (bridgeResult == APUSR_FRAMEGEN_OK && generatedWait) {
                            directPresentBridgePrepared = true;
                            generatedResult = presentGeneratedImage(
                                task.generatedImageIndex,
                                generatedWait,
                                "presentation.present_generated_direct",
                                "presentation.present_generated_direct.wait",
                                "presentation.present_generated_direct");
                        } else if (bridgeResult != APUSR_FRAMEGEN_OK && profile.stream) {
                            apusr::profile::recordError(
                                profile.stream,
                                profile.frameId,
                                "presentation.prepare_direct_present",
                                apusr::StatusCode::RuntimeFailure,
                                "Vulkan direct generated present bridge preparation failed.",
                                static_cast<std::uint64_t>(bridgeResult),
                                task.generatedImageIndex);
                        }
                    }
                } else {
                    void* generatedWait = task.generatedWaitSemaphore;
                    generatedResult = presentGeneratedImage(
                        task.generatedImageIndex,
                        generatedWait,
                        "presentation.present_generated",
                        "presentation.present_generated.wait",
                        "presentation.present_generated");
                }
            }
        }

        const bool generatedPresented =
            generatedAttempted && acceptedPresentResult(generatedResult);
        if (profile.stream && task.hasGeneratedFrame) {
            const char* reason = "generated_presented";
            apusr::profile::DecisionOutcome outcome = apusr::profile::DecisionOutcome::Presented;
            std::uint32_t decisionFlags = 0u;
            if (task.generationDeadlineMiss) {
                reason = "generation_deadline";
                outcome = apusr::profile::DecisionOutcome::Dropped;
                decisionFlags = apusr::profile::DecisionTriggerBurst;
            } else if (!generatedAttempted) {
                reason = task.generationReady ? "generated_not_attempted" : "generation_not_ready";
                outcome = apusr::profile::DecisionOutcome::Dropped;
                decisionFlags = apusr::profile::DecisionTriggerBurst;
            } else if (!generatedPresented) {
                reason = "generated_present_failed";
                outcome = apusr::profile::DecisionOutcome::Failed;
                decisionFlags = apusr::profile::DecisionTriggerBurst;
            }
            apusr::profile::recordDecision({
                .stream = profile.stream,
                .frameId = profile.frameId,
                .correlationId = task.generationCompletionValue,
                .name = "framegen.generated_present",
                .reason = reason,
                .outcome = outcome,
                .value0 = task.generationStartValue,
                .value1 = task.generationCompletionValue,
                .value2 = static_cast<std::uint64_t>(static_cast<std::int64_t>(generatedResult)),
                .value3 = task.directGeneratedFrame ? 1u : (task.replayGeneratedFrame ? 2u : 0u),
                .flags = decisionFlags,
            });
        }
        const bool pacedGeneratedPresented = generatedPresented && task.paceGeneratedFrame &&
            task.generatedVisibleDuration > Nanoseconds::zero() &&
            generatedVisibleAt != Clock::time_point{};

        // AMD-style pair-local pacing: once G is accepted, S is one identical
        // display interval after G. A G that misses its ideal midpoint therefore
        // causes at most the pipeline's one-time phase acquisition; there is no
        // global lattice and no catch-up. If G is genuinely unavailable, source
        // wins and keeps the source-only cadence from the preceding real frame.
        if (task.paceSourceFrame && task.generatedVisibleDuration > Nanoseconds::zero()) {
            if (pacedGeneratedPresented) {
                task.sourceTarget = generatedVisibleAt + task.generatedVisibleDuration;
            } else if (presentation->previousPhysicalSourcePresent != Clock::time_point{}) {
                task.sourceTarget =
                    presentation->previousPhysicalSourcePresent + task.generatedVisibleDuration * 2;
            }
        }

        if (profile.capture && task.hasGeneratedFrame) {
            apusr::profile::recordCounter({
                .stream = profile.stream,
                .frameId = profile.frameId,
                .name = "presentation.generated_attempted",
                .value = generatedAttempted ? 1u : 0u,
                .value2 = generatedPresented ? 1u : 0u,
                .flags = task.directGeneratedFrame ? 1u : (task.replayGeneratedFrame ? 2u : 0u),
            });
        }

        if (generatedAttempted && generatedResult < 0) {
            presentation->deferredSourceError.store(generatedResult, std::memory_order_release);
            recordDeferredPresentError(profile, "presentation.present_generated", generatedResult);
        }

        // Generation ownership is settled as soon as the generated present has
        // either been queued or deliberately dropped. Do not retain provider
        // output for the following half-frame source pacing interval; the replay
        // completion timeline still protects any copy that actually references
        // the output slot.
        if (task.replayGeneratedFrame && presentation->description.generationDecision) {
            presentation->description.generationDecision(
                presentation->description.userContext,
                task.frameId,
                task.generationReplaySlotIndex,
                task.generationCompletionValue,
                generatedPresented);
        }
        if (task.directGeneratedFrame && presentation->description.directGenerationDecision) {
            presentation->description.directGenerationDecision(
                presentation->description.userContext,
                task.frameId,
                task.generatedImageIndex,
                task.generationCompletionSemaphore,
                task.generationCompletionValue,
                generatedPresented,
                directPresentBridgePrepared);
        }

        // Give the image immediately preceding S its intended visibility. For
        // a generated pair this is exactly the same delta used for G. For a
        // source-only/drop path it is the remaining source-to-source interval.
        bool sourcePresentedByWsiTiming = false;
        bool sourcePresentAccepted = false;
        const bool pacedSource = task.paceSourceFrame &&
            task.sourceTarget != Clock::time_point{};
        const Clock::time_point precedingVisibleAt = pacedGeneratedPresented
            ? generatedVisibleAt
            : presentation->previousPhysicalSourcePresent;
        if (pacedSource && sourceReady && precedingVisibleAt != Clock::time_point{} &&
            presentation->description.presentSourceRelative) {
            const Nanoseconds remainingPreviousVisible = pacedGeneratedPresented
                ? task.generatedVisibleDuration
                : (task.sourceTarget > precedingVisibleAt
                    ? std::chrono::duration_cast<Nanoseconds>(task.sourceTarget - precedingVisibleAt)
                    : Nanoseconds::zero());
            if (remainingPreviousVisible > Nanoseconds::zero()) {
                bool scheduled = false;
                profile.presentWaits(
                    "presentation.present_source_relative",
                    "presentation.present_source_relative.wait",
                    task.queue,
                    sourceWaits.data(),
                    sourceWaitCount,
                    false);
                apusr::profile::CpuScope presentRelativeScope(
                    profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                    profile.frameId,
                    profile.sampleSequence,
                    "presentation.present_source_relative");
                const std::int32_t timedSourceResult = presentation->description.presentSourceRelative(
                    presentation->description.userContext,
                    task.queue,
                    task.swapchain,
                    sourceImageIndex,
                    sourceWaits.data(),
                    sourceWaitCount,
                    profile.frameId,
                    profile.capture,
                    static_cast<std::uint64_t>(remainingPreviousVisible.count()),
                    &scheduled);
                if (scheduled) {
                    sourcePresentedByWsiTiming = true;
                    sourcePresentAccepted = acceptedPresentResult(timedSourceResult);
                    if (sourcePresentAccepted)
                        presentation->previousPhysicalSourcePresent = task.sourceTarget;
                    if (profile.capture) {
                        apusr::profile::recordCounter({
                            .stream = profile.stream,
                            .frameId = profile.frameId,
                            .name = "presentation.source_wsi_timed",
                            .value = 1u,
                            .value2 = static_cast<std::uint64_t>(remainingPreviousVisible.count()),
                            .flags = pacedGeneratedPresented ? 1u : 0u,
                        });
                    }
                    if (timedSourceResult < 0) {
                        presentation->deferredSourceError.store(timedSourceResult, std::memory_order_release);
                        recordDeferredPresentError(profile, "presentation.present_source_relative", timedSourceResult);
                    }
                } else if (timedSourceResult < 0) {
                    presentation->deferredSourceError.store(timedSourceResult, std::memory_order_release);
                    recordDeferredPresentError(profile, "presentation.present_source_relative", timedSourceResult);
                }
            }
        }

        if (!sourcePresentedByWsiTiming && pacedSource && sourceReady &&
            Clock::now() < task.sourceTarget) {
            const auto waitBegin = Clock::now();
            const auto waitTimeout = task.sourceTarget > waitBegin
                ? std::chrono::duration_cast<Nanoseconds>(task.sourceTarget - waitBegin)
                : Nanoseconds::zero();
            apusr::profile::WaitScope waitScope(
                profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                profile.frameId,
                profile.sampleSequence,
                "presentation.wait_source_target",
                apusr::profile::WaitKind::Timer,
                0u,
                0u,
                static_cast<std::uint64_t>(std::max<std::int64_t>(waitTimeout.count(), 0)),
                "source_target");
            precisionWaiter.waitUntil(task.sourceTarget);
            waitScope.setResult(apusr::profile::WaitResult::Satisfied);
        }

        if (profile.capture && pacedSource) {
            const auto now = Clock::now();
            const auto late = now > task.sourceTarget
                ? std::chrono::duration_cast<Nanoseconds>(now - task.sourceTarget).count()
                : 0;
            apusr::profile::recordCounter({
                .stream = profile.stream,
                .frameId = profile.frameId,
                .name = "presentation.source_target_late_ns",
                .value = static_cast<std::uint64_t>(std::max<std::int64_t>(late, 0)),
                .value2 = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(task.generatedVisibleDuration.count(), 0)),
            });
        }

        if (sourceReady && !sourcePresentedByWsiTiming) {
            profile.presentWaits(
                "presentation.present_source",
                "presentation.present_source.wait",
                task.queue,
                sourceWaits.data(),
                sourceWaitCount,
                false);
            apusr::profile::CpuScope presentSourceScope(
                profile.capture ? profile.stream : apusr::profile::StreamHandle{},
                profile.frameId,
                profile.sampleSequence,
                "presentation.present_source");
            const Clock::time_point sourceIssuedAt = Clock::now();
            const std::int32_t sourceResult = presentation->description.presentImage(
                presentation->description.userContext,
                task.queue,
                task.swapchain,
                sourceImageIndex,
                sourceWaits.data(),
                sourceWaitCount,
                profile.frameId,
                profile.capture,
                false);
            sourcePresentAccepted = acceptedPresentResult(sourceResult);
            if (sourcePresentAccepted)
                presentation->previousPhysicalSourcePresent = sourceIssuedAt;
            if (sourceResult < 0) {
                presentation->deferredSourceError.store(sourceResult, std::memory_order_release);
                recordDeferredPresentError(profile, "presentation.present_source", sourceResult);
            }
        }

        {
            std::lock_guard lock(presentation->mutex);
            presentation->presenterActive = false;
            if (presentation->outstanding != 0u)
                --presentation->outstanding;
            // A configured application-present lead bound is released one
            // task at a time as physical presentation retires. Wake on every
            // retirement rather than only on a complete drain.
            presentation->applicationLeadWake.notify_all();
            if (presentation->outstanding == 0u)
                presentation->drained.notify_all();
        }
    }
}

} // namespace

ApusrFramegenResult apusrFramegenCreateVulkanPresentation(
    const ApusrFramegenVulkanPresentationDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanPresentation** outPresentation) noexcept
{
    if (!outPresentation)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outPresentation = nullptr;
    if (!description.presentImage || !(description.initialSourceFrameTimeMs > 0.0f) ||
        description.maxPendingPresentCount == 0u ||
        description.maxApplicationPresentQueueDepth > description.maxPendingPresentCount)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    auto* presentation = allocator.construct<ApusrFramegenVulkanPresentation>();
    if (!presentation)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    presentation->description = description;
    presentation->taskCapacity = description.maxPendingPresentCount;
    presentation->tasks = static_cast<PresentTask*>(
        allocator.alloc(sizeof(PresentTask) * presentation->taskCapacity));
    presentation->readyTasks = static_cast<PresentTask*>(
        allocator.alloc(sizeof(PresentTask) * presentation->taskCapacity));
    if (!presentation->tasks || !presentation->readyTasks) {
        if (presentation->readyTasks)
            allocator.dealloc(presentation->readyTasks);
        if (presentation->tasks)
            allocator.dealloc(presentation->tasks);
        allocator.destroy(presentation);
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    for (std::size_t i = 0u; i < presentation->taskCapacity; ++i) {
        ::new (static_cast<void*>(presentation->tasks + i)) PresentTask{};
        ::new (static_cast<void*>(presentation->readyTasks + i)) PresentTask{};
    }
    const auto initialNs = static_cast<std::int64_t>(description.initialSourceFrameTimeMs * 1'000'000.0f);
    presentation->sourceFrameInterval = Nanoseconds(std::max<std::int64_t>(initialNs, 1));
    presentation->pacedPresentInterval = presentation->sourceFrameInterval / 2;
    try {
        presentation->readinessWorker = std::thread(generationReadinessWorker, presentation);
        presentation->presenterWorker = std::thread(presentationWorker, presentation);
    } catch (...) {
        {
            std::lock_guard lock(presentation->mutex);
            presentation->shutdown = true;
            presentation->readinessWake.notify_all();
            presentation->presenterWake.notify_all();
        }
        if (presentation->readinessWorker.joinable())
            presentation->readinessWorker.join();
        if (presentation->presenterWorker.joinable())
            presentation->presenterWorker.join();
        for (std::size_t i = 0u; i < presentation->taskCapacity; ++i) {
            presentation->tasks[i].~PresentTask();
            presentation->readyTasks[i].~PresentTask();
        }
        allocator.dealloc(presentation->tasks);
        allocator.dealloc(presentation->readyTasks);
        presentation->tasks = nullptr;
        presentation->readyTasks = nullptr;
        allocator.destroy(presentation);
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    *outPresentation = presentation;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenScheduleVulkanPresentation(
    ApusrFramegenVulkanPresentation* presentation,
    const ApusrFramegenVulkanPresentationSchedule& schedule) noexcept
{
    if (!presentation)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (!schedule.queue || !schedule.swapchain ||
        schedule.sourceWaitSemaphoreCount > APUSR_FRAMEGEN_VULKAN_MAX_PRESENT_WAITS ||
        (schedule.sourceWaitSemaphoreCount != 0u && !schedule.sourceWaitSemaphores) ||
        (schedule.hasGeneratedFrame && !schedule.replayGeneratedFrame && !schedule.directGeneratedFrame &&
            !schedule.generatedWaitSemaphore) ||
        (schedule.replayGeneratedFrame && schedule.directGeneratedFrame) ||
        (schedule.directGeneratedFrame && (!schedule.hasGeneratedFrame ||
            !presentation->description.prepareDirectPresent ||
            !presentation->description.directGenerationDecision || !presentation->description.timelineReady ||
            !schedule.generationCompletionSemaphore || schedule.generationCompletionValue == 0u)) ||
        (schedule.replayGeneratedFrame && (!schedule.hasGeneratedFrame ||
            !presentation->description.prepareGeneratedReplay || !presentation->description.timelineReady ||
            !schedule.generationCompletionSemaphore || schedule.generationCompletionValue == 0u)) ||
        ((schedule.generationStartSemaphore == nullptr) != (schedule.generationStartValue == 0u)) ||
        (schedule.replaySourceFrame && (!presentation->description.prepareSourceReplay ||
            schedule.sourceReplayCompletionValue == 0u ||
            ((schedule.sourceReplayUpstreamSemaphore == nullptr) != (schedule.sourceReplayUpstreamValue == 0u)))) ||
        (!schedule.replayGeneratedFrame && !schedule.replaySourceFrame &&
            schedule.sourceImageIndex == schedule.generatedImageIndex && schedule.hasGeneratedFrame))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    std::unique_lock lock(presentation->mutex);
    if (presentation->shutdown)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    if (!presentation->profileStream && apusr::profile::enabled()) {
        presentation->profileStream = apusr::profile::registerStream({
            .backend = apusr::profile::BackendApi::Vulkan,
            .component = apusr::profile::Component::FrameGeneration,
            .name = "framegen.presentation",
            .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                apusr::profile::StreamCapabilityCounters |
                apusr::profile::StreamCapabilityErrors |
                apusr::profile::StreamCapabilityQueueEvents |
                apusr::profile::StreamCapabilityDecisions |
                apusr::profile::StreamCapabilityThreadInfo |
                apusr::profile::StreamCapabilityWaitIntervals,
            .nativeQueue = reinterpret_cast<std::uintptr_t>(schedule.queue),
        });
        presentation->profileSampleGate.reset();
    }

    apusr::profile::SampleDecision profileDecision{};
    if (presentation->profileStream) {
        std::uint64_t profileConfiguration = apusr::profile::hashCombine(0u, schedule.hasGeneratedFrame ? 1u : 0u);
        profileConfiguration = apusr::profile::hashCombine(profileConfiguration, schedule.paceGeneratedFrame ? 1u : 0u);
        profileConfiguration = apusr::profile::hashCombine(profileConfiguration, schedule.paceSourceFrame ? 1u : 0u);
        profileConfiguration = apusr::profile::hashCombine(profileConfiguration, schedule.replayGeneratedFrame ? 1u : 0u);
        profileConfiguration = apusr::profile::hashCombine(profileConfiguration, schedule.directGeneratedFrame ? 1u : 0u);
        profileConfiguration = apusr::profile::hashCombine(profileConfiguration, schedule.replaySourceFrame ? 1u : 0u);
        profileConfiguration = apusr::profile::hashCombine(profileConfiguration, schedule.sourceWaitSemaphoreCount);
        profileDecision = presentation->profileSampleGate.decide(profileConfiguration, schedule.frameId);
    }

    const std::size_t applicationLeadLimit =
        static_cast<std::size_t>(presentation->description.maxApplicationPresentQueueDepth);
    if (applicationLeadLimit != 0u && presentation->outstanding >= applicationLeadLimit) {
        apusr::profile::WaitScope leadWait(
            profileDecision.capture ? presentation->profileStream : apusr::profile::StreamHandle{},
            schedule.frameId,
            profileDecision.sampleSequence,
            "presentation.wait_application_lead",
            apusr::profile::WaitKind::CpuPrimitive,
            0u,
            static_cast<std::uint64_t>(applicationLeadLimit),
            0u,
            "physical_source_backpressure");
        presentation->applicationLeadWake.wait(lock, [&] {
            return presentation->shutdown || presentation->outstanding < applicationLeadLimit;
        });
        if (presentation->shutdown) {
            leadWait.setResult(apusr::profile::WaitResult::Interrupted);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
        leadWait.setResult(
            apusr::profile::WaitResult::Satisfied,
            static_cast<std::uint64_t>(presentation->outstanding));
    }
    if (presentation->outstanding == presentation->taskCapacity)
        return APUSR_FRAMEGEN_ERROR_BUSY;

    // Scheduling time starts after any intentional WSI-style back-pressure.
    // Otherwise the lead wait would incorrectly consume the next task's
    // generation relevance budget and source-cadence observation.
    const Clock::time_point now = Clock::now();

    const bool updatePacing = schedule.paceSourceFrame ||
        (schedule.hasGeneratedFrame && schedule.paceGeneratedFrame);
    const auto suppliedInterval = schedule.sourceFrameTimeMs > 0.0f
        ? Nanoseconds(static_cast<std::int64_t>(schedule.sourceFrameTimeMs * 1'000'000.0f))
        : Nanoseconds::zero();
    Nanoseconds pacingObservation{};
    if (updatePacing && suppliedInterval >= kMinSourceFrameInterval &&
        suppliedInterval <= kMaxSourceFrameInterval) {
        // Keep the full source interval as the generation-relevance budget.
        // Physical display spacing is smoothed separately below so a single
        // source-frame jitter spike cannot directly become a visible G/S gap.
        presentation->sourceFrameInterval = suppliedInterval;
        pacingObservation = suppliedInterval;
    } else if (updatePacing && presentation->previousSchedule != Clock::time_point{}) {
        const Nanoseconds observed = std::chrono::duration_cast<Nanoseconds>(now - presentation->previousSchedule);
        if (validSourceFrameInterval(observed)) {
            presentation->sourceFrameInterval = Nanoseconds(
                (presentation->sourceFrameInterval.count() * 7 + observed.count() * 3) / 10);
            pacingObservation = observed;
        } else if (observed >= kMaxSourceFrameInterval) {
            pacingObservation = observed;
        }
    }
    if (updatePacing)
        presentation->previousSchedule = now;
    if (pacingObservation > Nanoseconds::zero())
        (void)updatePacedPresentInterval(*presentation, pacingObservation);

    const std::size_t tail = (presentation->head + presentation->count) % presentation->taskCapacity;
    PresentTask& task = presentation->tasks[tail];
    task = {};
    task.queue = schedule.queue;
    task.swapchain = schedule.swapchain;
    task.frameId = schedule.frameId;
    task.sourceImageIndex = schedule.sourceImageIndex;
    task.sourceWaitSemaphoreCount = schedule.sourceWaitSemaphoreCount;
    for (std::uint32_t i = 0; i < schedule.sourceWaitSemaphoreCount; ++i)
        task.sourceWaitSemaphores[i] = schedule.sourceWaitSemaphores[i];
    task.hasGeneratedFrame = schedule.hasGeneratedFrame;
    task.paceGeneratedFrame = schedule.paceGeneratedFrame;
    task.paceSourceFrame = schedule.paceSourceFrame ||
        (schedule.hasGeneratedFrame && schedule.paceGeneratedFrame);
    task.generatedImageIndex = schedule.generatedImageIndex;
    task.generatedWaitSemaphore = schedule.generatedWaitSemaphore;
    task.replayGeneratedFrame = schedule.replayGeneratedFrame;
    task.directGeneratedFrame = schedule.directGeneratedFrame;
    task.generationReplaySlotIndex = schedule.generationReplaySlotIndex;
    task.generationStartSemaphore = schedule.generationStartSemaphore;
    task.generationStartValue = schedule.generationStartValue;
    task.generationCompletionSemaphore = schedule.generationCompletionSemaphore;
    task.generationCompletionValue = schedule.generationCompletionValue;
    task.generatedReplayCompletionValue = schedule.generatedReplayCompletionValue;
    task.replaySourceFrame = schedule.replaySourceFrame;
    task.sourceReplaySlotIndex = schedule.sourceReplaySlotIndex;
    task.sourceReplayCompletionValue = schedule.sourceReplayCompletionValue;
    task.sourceReplayUpstreamSemaphore = schedule.sourceReplayUpstreamSemaphore;
    task.sourceReplayUpstreamValue = schedule.sourceReplayUpstreamValue;
    task.generatedVisibleDuration = task.paceSourceFrame
        ? presentation->pacedPresentInterval
        : Nanoseconds::zero();
    task.sourceFrameInterval = presentation->sourceFrameInterval;
    task.generationBudget = task.generatedVisibleDuration > Nanoseconds::zero()
        ? task.generatedVisibleDuration * 2
        : presentation->sourceFrameInterval;
    // Legacy/non-split callers without an explicit start timeline retain the
    // schedule-time budget. Split Vulkan generation re-anchors this deadline in
    // the readiness worker when producerReady proves Phase A can actually run.
    task.generationDeadline = schedule.hasGeneratedFrame && schedule.paceGeneratedFrame
        ? now + task.generationBudget
        : now;
    // The presenter derives pair-local targets from its worker-owned physical
    // source anchor. Keeping that state off the application thread avoids a
    // cross-thread presentation-clock race.
    task.sourceTarget = {};
    task.profileCapture = profileDecision.capture;
    task.profileSampleSequence = profileDecision.sampleSequence;
    task.profileScheduledCpuTick = profileDecision.capture ? apusr::profile::cpuTick() : 0u;
    task.profileSubmitSequence = presentation->profileStream ? ++presentation->profileSubmitSequence : 0u;
    if (profileDecision.capture) {
        apusr::profile::recordCounter({
            .stream = presentation->profileStream,
            .frameId = schedule.frameId,
            .name = "presentation.queue_depth",
            .value = presentation->outstanding + 1u,
            .value2 = presentation->taskCapacity,
        });
        apusr::profile::recordCounter({
            .stream = presentation->profileStream,
            .frameId = schedule.frameId,
            .name = "presentation.source_interval_ns",
            .value = static_cast<std::uint64_t>(
                std::max<std::int64_t>(presentation->sourceFrameInterval.count(), 0)),
            .value2 = schedule.hasGeneratedFrame ? 1u : 0u,
            .flags = schedule.directGeneratedFrame ? 1u : (schedule.replayGeneratedFrame ? 2u : 0u),
        });
        apusr::profile::recordCounter({
            .stream = presentation->profileStream,
            .frameId = schedule.frameId,
            .name = "presentation.paced_present_interval_ns",
            .value = static_cast<std::uint64_t>(
                std::max<std::int64_t>(presentation->pacedPresentInterval.count(), 0)),
            .value2 = presentation->pacingEstimatorSamples,
            .flags = schedule.directGeneratedFrame ? 1u : (schedule.replayGeneratedFrame ? 2u : 0u),
        });
        if (schedule.hasGeneratedFrame && schedule.paceGeneratedFrame) {
            apusr::profile::recordCounter({
                .stream = presentation->profileStream,
                .frameId = schedule.frameId,
                .name = "presentation.generation_budget_ns",
                .value = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(task.generationBudget.count(), 0)),
                .value2 = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(task.generatedVisibleDuration.count(), 0)),
                .flags = schedule.directGeneratedFrame ? 1u : (schedule.replayGeneratedFrame ? 2u : 0u),
            });
        }
    }
    ++presentation->count;
    ++presentation->outstanding;
    presentation->readinessWake.notify_one();
    return APUSR_FRAMEGEN_OK;
}

void apusrFramegenDrainVulkanPresentation(ApusrFramegenVulkanPresentation* presentation) noexcept
{
    if (!presentation)
        return;
    std::unique_lock lock(presentation->mutex);
    presentation->drained.wait(lock, [&] {
        return presentation->outstanding == 0u;
    });
}

std::int32_t apusrFramegenTakeVulkanPresentationError(
    ApusrFramegenVulkanPresentation* presentation) noexcept
{
    return presentation
        ? presentation->deferredSourceError.exchange(kVkSuccess, std::memory_order_acq_rel)
        : kVkSuccess;
}

void apusrFramegenDestroyVulkanPresentation(
    ApusrFramegenVulkanPresentation* presentation,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!presentation)
        return;
    {
        std::lock_guard lock(presentation->mutex);
        presentation->shutdown = true;
        presentation->readinessWake.notify_all();
        presentation->presenterWake.notify_all();
        presentation->applicationLeadWake.notify_all();
    }
    if (presentation->readinessWorker.joinable())
        presentation->readinessWorker.join();
    if (presentation->presenterWorker.joinable())
        presentation->presenterWorker.join();
    for (std::size_t i = 0u; i < presentation->taskCapacity; ++i) {
        presentation->tasks[i].~PresentTask();
        presentation->readyTasks[i].~PresentTask();
    }
    allocator.dealloc(presentation->tasks);
    allocator.dealloc(presentation->readyTasks);
    presentation->tasks = nullptr;
    presentation->readyTasks = nullptr;
    allocator.destroy(presentation);
}
