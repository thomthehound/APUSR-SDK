// SPDX-License-Identifier: MIT
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_pacing_dx12.h"

#include "apusr_framegen_swapchain_dx12.h"
#include "apusr_framegen_debug_pacing_dx12.h"
#include "apusr_framegen_dx12_helpers.h"
#include "antilag2/ffx_antilag2_dx12.h"
#include "apusr_framegen_backend_dx12.h"
#include "apusr_framegen_dx12_resource_compat.h"

#include <apusr/apusr_framegen.h>

#include <timeapi.h>

namespace apusr::framegen::dx12 {
namespace {

struct PacingProfile final {
    // `stream` gates heavier per-thread timing. `flowStream` is propagated from
    // the sampled source Present so the causal chain remains intact even when
    // the worker's independent sampling gate is not open for this occurrence.
    apusr::profile::StreamHandle stream{};
    apusr::profile::StreamHandle flowStream{};
    std::uint64_t frameId = 0u;
    std::uint64_t occurrenceId = 0u;
    std::uint64_t flowId = 0u;
    std::uint64_t sampleSequence = 0u;

    void flow(const char* detail, apusr::profile::FlowPhase phase = apusr::profile::FlowPhase::Step,
              std::uint64_t value0 = 0u, std::uint64_t value1 = 0u) const noexcept
    {
        if (!flowStream || flowId == 0u)
            return;
        apusr::profile::recordFlowEvent({
            .stream = flowStream,
            .frameId = frameId,
            .occurrenceId = occurrenceId,
            .flowId = flowId,
            .name = "presentation.lifecycle",
            .detail = detail,
            .phase = phase,
            .value0 = value0,
            .value1 = value1,
        });
    }

    void queue(
        const char* name,
        ID3D12CommandQueue* nativeQueue,
        const apusr::profile::QueueOperation operation,
        ID3D12Fence* fence = nullptr,
        const std::uint64_t value = 0u,
        ID3D12CommandQueue* relatedQueue = nullptr,
        const std::uint32_t objectCount = 0u) const noexcept
    {
        if (!stream || nativeQueue == nullptr)
            return;
        apusr::profile::recordQueueEvent(apusr::profile::QueueEvent{
            .stream = stream,
            .frameId = frameId,
            .sampleSequence = sampleSequence,
            .name = name,
            .nativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(nativeQueue)),
            .syncObject = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fence)),
            .syncValue = value,
            .relatedQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(relatedQueue)),
            .submitSequence = sampleSequence,
            .operation = operation,
            .objectCount = objectCount,
        });
    }
};

[[nodiscard]] std::uint64_t pacingProfileHash(const PresentSchedule& schedule) noexcept
{
    std::uint64_t profileHash = 0x4150555350414345ull; // "APUSPACE"
    profileHash = apusr::profile::hashCombine(profileHash, schedule.vsync ? 1u : 0u);
    profileHash = apusr::profile::hashCombine(profileHash, schedule.numFramesToPresent);
    profileHash = apusr::profile::hashCombine(profileHash, schedule.drawDebugPacingLines ? 1u : 0u);
    for (std::size_t index = 0; index < kPresentFrameTypeCount; ++index) {
        const ScheduledFrame& frame = schedule.frame(static_cast<PresentFrameType>(index));
        profileHash = apusr::profile::hashCombine(profileHash, frame.enabled ? 1u : 0u);
    }
    return profileHash;
}

[[nodiscard]] FfxApiResource makeFrameResource(const ScheduledFrame& frame, PresentFrameType frameType)
{
    auto* resource = static_cast<ID3D12Resource*>(frame.resource.value);
    if (!resource)
        return {};

    const FfxApiResourceState state = frameType == PresentFrameType::Generated
        ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
        : FFX_API_RESOURCE_STATE_PRESENT;
    return apusrFramegenResourceDX12(resource, apusrFramegenResourceDescriptionDX12(resource), nullptr, state);
}

HRESULT compositeSwapchainFrame(
    FrameinterpolationPresentInfo& presenter, PacingData& pacing, PresentFrameType frameType, const PacingProfile& profile)
{
    const PresentSchedule& schedule = pacing.schedule;
    const ScheduledFrame& frame = schedule.frame(frameType);

    // Only generated frames depend on interpolation completion. The real frame
    // is already ordered by the game/present queue handoff.
    if (frameType == PresentFrameType::Generated) {
        profile.queue("pacing.interpolation_ready", presenter.presentQueue, apusr::profile::QueueOperation::Wait,
                      presenter.interpolationFence, frame.interpolationCompletedFenceValue, presenter.interpolationQueue);
        presenter.presentQueue->Wait(presenter.interpolationFence, frame.interpolationCompletedFenceValue);
    }

    const FfxApiResource sourceFrame = makeFrameResource(frame, frameType);
    if (!sourceFrame.resource)
        return E_INVALIDARG;

    if (schedule.drawDebugPacingLines) {
        auto commands = presenter.commandPool.get(presenter.presentQueue, L"APUSR composite debug pacing");
        const uint32_t backBufferIndex = presenter.swapChain->GetCurrentBackBufferIndex();
        ID3D12Resource* backBuffer = nullptr;
        const HRESULT getBufferResult = presenter.swapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
        if (FAILED(getBufferResult)) {
            commands->drop();
            return getBufferResult;
        }

        FfxPresentCallbackDescription desc{};
        desc.commandList = apusrFramegenCommandListDX12(commands->reset());
        desc.device = presenter.device;
        desc.isInterpolatedFrame = frameType != PresentFrameType::Real;
        desc.outputSwapChainBuffer = apusrFramegenResourceDX12(
            backBuffer, apusrFramegenResourceDescriptionDX12(backBuffer), nullptr, FFX_API_RESOURCE_STATE_PRESENT);
        desc.currentBackBuffer = sourceFrame;
        desc.currentUI = pacing.uiSurface;
        desc.usePremulAlpha = schedule.usePremulAlphaComposite;
        desc.frameID = schedule.frameId;

        ffxFrameInterpolationDebugPacing(&desc);
        profile.queue("pacing.debug_composite", presenter.presentQueue, apusr::profile::QueueOperation::Submit,
                      nullptr, 0u, nullptr, 1u);
        commands->execute(true);
        SafeRelease(backBuffer);
    }

    FFX_ASSERT(pacing.presentCallback != nullptr);
    if (!pacing.presentCallback)
        return E_POINTER;

    auto commands = presenter.commandPool.get(presenter.presentQueue, L"APUSR composite frame");
    const uint32_t backBufferIndex = presenter.swapChain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    const HRESULT getBufferResult = presenter.swapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    if (FAILED(getBufferResult)) {
        commands->drop();
        return getBufferResult;
    }

    FfxPresentCallbackDescription desc{};
    desc.commandList = apusrFramegenCommandListDX12(commands->reset());
    desc.device = presenter.device;
    desc.isInterpolatedFrame = frameType != PresentFrameType::Real;
    desc.outputSwapChainBuffer = apusrFramegenResourceDX12(
        backBuffer, apusrFramegenResourceDescriptionDX12(backBuffer), nullptr, FFX_API_RESOURCE_STATE_PRESENT);
    desc.currentBackBuffer = sourceFrame;
    desc.currentUI = pacing.uiSurface;
    desc.usePremulAlpha = schedule.usePremulAlphaComposite;
    desc.frameID = schedule.frameId;

    if (pacing.presentCallback(&desc, pacing.presentCallbackContext) != FFX_OK) {
        commands->drop(true);
        SafeRelease(backBuffer);
        return E_FAIL;
    }
    profile.queue("pacing.composite", presenter.presentQueue, apusr::profile::QueueOperation::Submit,
                  nullptr, 0u, nullptr, 1u);
    commands->execute(true);
    SafeRelease(backBuffer);

    profile.queue("pacing.composition_complete", presenter.presentQueue, apusr::profile::QueueOperation::Signal,
                  presenter.compositionFenceGPU, frame.presentIndex);
    presenter.presentQueue->Signal(presenter.compositionFenceGPU, frame.presentIndex);
    presenter.compositionFenceCPU->Signal(frame.presentIndex);
    return S_OK;
}

void presentFrame(
    FrameinterpolationPresentInfo& presenter, const PacingData& pacing, PresentFrameType frameType, const PacingProfile& profile)
{
    const PresentSchedule& schedule = pacing.schedule;
    const ScheduledFrame& frame = schedule.frame(frameType);
    const UINT syncInterval = schedule.vsync ? 1u : 0u;
    const bool exclusiveFullscreen = isExclusiveFullscreen(presenter.swapChain);
    const bool allowTearing = schedule.tearingSupported && !exclusiveFullscreen && syncInterval == 0;
    const UINT flags = allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0;

    struct AntiLag2Data {
        AMD::AntiLag2DX12::Context* context;
        bool enabled;
    } antiLag{};

    // {5083ae5b-8070-4fca-8ee5-3582dd367d13}
    static const GUID kAntiLag2DataGuid = {
        0x5083ae5b, 0x8070, 0x4fca, {0x8e, 0xe5, 0x35, 0x82, 0xdd, 0x36, 0x7d, 0x13}};

    UINT antiLagSize = sizeof(antiLag);
    if (SUCCEEDED(presenter.swapChain->GetPrivateData(kAntiLag2DataGuid, &antiLagSize, &antiLag)) && antiLag.enabled)
        AMD::AntiLag2DX12::SetFrameGenFrameType(antiLag.context, frameType != PresentFrameType::Real);

    {
        apusr::profile::CpuScope presentScope(
            profile.stream, profile.frameId, profile.sampleSequence,
            frameType == PresentFrameType::Generated ? "pacing.present_generated" : "pacing.present_real");
        profile.queue(frameType == PresentFrameType::Generated ? "pacing.present_generated" : "pacing.present_real",
                      presenter.presentQueue, apusr::profile::QueueOperation::Present);
        const std::uint64_t dxgiPresentBeginTick = profile.stream ? apusr::profile::cpuTick() : 0u;
        const HRESULT presentResult = presenter.swapChain->Present(syncInterval, flags);
        const std::uint64_t dxgiPresentEndTick = profile.stream ? apusr::profile::cpuTick() : 0u;
        recordDxgiPresentTiming(
            presenter, profile.stream, profile.frameId, profile.sampleSequence,
            dxgiPresentBeginTick, dxgiPresentEndTick, frameType == PresentFrameType::Generated);
        const bool lastPhysicalPresent = frameType == PresentFrameType::Real ||
            !schedule.frame(PresentFrameType::Real).enabled;
        const bool presentFailed = FAILED(presentResult);
        const char* flowDetail = frameType == PresentFrameType::Generated
            ? (presentFailed ? "generated_present_failed" : "generated_present")
            : (presentFailed ? "real_present_failed" : "real_present");
        profile.flow(flowDetail,
                     lastPhysicalPresent
                         ? (presentFailed ? apusr::profile::FlowPhase::Cancel : apusr::profile::FlowPhase::End)
                         : apusr::profile::FlowPhase::Step,
                     frame.presentIndex,
                     static_cast<std::uint64_t>(static_cast<std::int64_t>(presentResult)));
        if (FAILED(presentResult) && presenter.profileStream) {
            apusr::profile::recordError(
                presenter.profileStream,
                profile.frameId,
                frameType == PresentFrameType::Generated ? "DXGI.Present.generated" : "DXGI.Present.real",
                apusr::StatusCode::RuntimeFailure,
                "D3D12 FrameGen presenter swapchain Present failed.",
                static_cast<std::uint64_t>(static_cast<std::int64_t>(presentResult)),
                static_cast<std::uint64_t>(frameType));
        }
    }
    profile.queue("pacing.present_complete", presenter.presentQueue, apusr::profile::QueueOperation::Signal,
                  presenter.presentFence, frame.presentIndex);
    presenter.presentQueue->Signal(presenter.presentFence, frame.presentIndex);
}

DWORD WINAPI presenterThread(LPVOID context)
{
    auto* presenter = static_cast<FrameinterpolationPresentInfo*>(context);
    if (!presenter)
        return ERROR_INVALID_PARAMETER;

    if (presenter->profileStream) {
        apusr::profile::recordThreadInfo({
            .stream = presenter->profileStream,
            .name = "APUSR D3D12 Presenter",
            .role = "framegen.presenter",
            .priority = THREAD_PRIORITY_HIGHEST,
        });
    }

    UINT64 framesPresented = 0;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const int64_t qpcFrequency = frequency.QuadPart;

    TIMECAPS timerCaps{};
    timerCaps.wPeriodMin = UNKNOWN_TIMER_RESOlUTION;
    presenter->previousPresentQpc = 0;

    while (!presenter->shutdown) {
        WaitForSingleObject(presenter->pacerEvent, INFINITE);
        if (presenter->shutdown)
            break;

        EnterCriticalSection(&presenter->criticalSectionScheduledFrame);
        PacingData entry = presenter->scheduledPresents;
        presenter->scheduledPresents.invalidate();
        LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);

        PresentSchedule& schedule = entry.schedule;
        if (schedule.numFramesToPresent == 0)
            continue;

        const std::uint64_t profileHash = pacingProfileHash(schedule);
        const apusr::profile::SampleDecision profileDecision = presenter->profileStream
            ? presenter->profileQueueSampleGate.decide(profileHash, schedule.frameId)
            : apusr::profile::SampleDecision{};
        const PacingProfile profile{
            .stream = profileDecision.capture ? presenter->profileStream : apusr::profile::StreamHandle{},
            .flowStream = entry.profileFlowStream,
            .frameId = schedule.frameId,
            .occurrenceId = entry.profileOccurrenceId,
            .flowId = entry.profileFlowId,
            .sampleSequence = profileDecision.sampleSequence,
        };

        // Dropped entries still advance the present fence; otherwise a later wait can deadlock.
        profile.queue("pacing.base_present", presenter->presentQueue, apusr::profile::QueueOperation::Signal,
                      presenter->presentFence, schedule.numFramesSentForPresentationBase);
        presenter->presentQueue->Signal(presenter->presentFence, schedule.numFramesSentForPresentationBase);
        profile.queue("pacing.interpolation_batch_ready", presenter->presentQueue, apusr::profile::QueueOperation::Wait,
                      presenter->interpolationFence, schedule.interpolationCompletedFenceValue, presenter->interpolationQueue);
        presenter->presentQueue->Wait(presenter->interpolationFence, schedule.interpolationCompletedFenceValue);

        for (std::size_t index = 0; index < kPresentFrameTypeCount; ++index) {
            const auto frameType = static_cast<PresentFrameType>(index);
            const ScheduledFrame& frame = schedule.frame(frameType);
            if (!frame.enabled)
                continue;

            const HRESULT compositeResult = compositeSwapchainFrame(*presenter, entry, frameType, profile);
            if (FAILED(compositeResult)) {
                const bool lastPhysicalPresent = frameType == PresentFrameType::Real ||
                    !schedule.frame(PresentFrameType::Real).enabled;
                profile.flow(frameType == PresentFrameType::Generated
                                 ? "generated_composite_failed" : "real_composite_failed",
                             lastPhysicalPresent ? apusr::profile::FlowPhase::Cancel
                                                 : apusr::profile::FlowPhase::Step,
                             frame.presentIndex,
                             static_cast<std::uint64_t>(static_cast<std::int64_t>(compositeResult)));
                if (presenter->profileStream) {
                    apusr::profile::recordError(
                        presenter->profileStream,
                        schedule.frameId,
                        frameType == PresentFrameType::Generated
                            ? "pacing.composite_generated" : "pacing.composite_real",
                        apusr::StatusCode::RuntimeFailure,
                        "D3D12 FrameGen composition failed; frame presentation was dropped.",
                        static_cast<std::uint64_t>(static_cast<std::int64_t>(compositeResult)),
                        static_cast<std::uint64_t>(frameType));
                }
                // A failed composition means no GPU work references this frame. Advance every
                // timeline the normal path would have advanced so shutdown, UI replacement,
                // and later presents cannot deadlock on a frame that was deliberately dropped.
                profile.queue("pacing.composition_complete_failed", presenter->presentQueue,
                              apusr::profile::QueueOperation::Signal, presenter->compositionFenceGPU, frame.presentIndex);
                presenter->presentQueue->Signal(presenter->compositionFenceGPU, frame.presentIndex);
                presenter->compositionFenceCPU->Signal(frame.presentIndex);
                if (frame.presentIndex == schedule.replacementBufferFenceSignal) {
                    profile.queue("pacing.replacement_available_failed", presenter->presentQueue,
                                  apusr::profile::QueueOperation::Signal, presenter->replacementBufferFence, frame.presentIndex);
                    presenter->presentQueue->Signal(presenter->replacementBufferFence, frame.presentIndex);
                }
                profile.queue("pacing.present_complete_failed", presenter->presentQueue,
                              apusr::profile::QueueOperation::Signal, presenter->presentFence, frame.presentIndex);
                presenter->presentQueue->Signal(presenter->presentFence, frame.presentIndex);
                continue;
            }

            if (frame.presentIndex == schedule.replacementBufferFenceSignal) {
                profile.queue("pacing.replacement_available", presenter->presentQueue,
                              apusr::profile::QueueOperation::Signal, presenter->replacementBufferFence,
                              schedule.replacementBufferFenceSignal);
                presenter->presentQueue->Signal(presenter->replacementBufferFence, schedule.replacementBufferFenceSignal);
            }

            const MMRESULT timerResult = timeGetDevCaps(&timerCaps, sizeof(timerCaps));
            if (timerResult != MMSYSERR_NOERROR || !presenter->allowHybridSpin)
                timerCaps.wPeriodMin = UNKNOWN_TIMER_RESOlUTION;
            else
                timerCaps.wPeriodMin = FFX_MAXIMUM(1, timerCaps.wPeriodMin);

            {
                apusr::profile::WaitScope waitScope(
                    profile.stream,
                    profile.frameId,
                    profile.sampleSequence,
                    "pacing.wait_composition_gpu",
                    apusr::profile::WaitKind::GpuFence,
                    reinterpret_cast<std::uintptr_t>(presenter->compositionFenceGPU),
                    frame.presentIndex,
                    0u,
                    "composition_complete");
                const bool waitSatisfied = waitForFenceValue(presenter->compositionFenceGPU, frame.presentIndex);
                waitScope.setResult(waitSatisfied ? apusr::profile::WaitResult::Satisfied
                                                  : apusr::profile::WaitResult::Failed,
                                    waitSatisfied ? frame.presentIndex : 0u);
            }
            const uint64_t targetQpc = presenter->previousPresentQpc + frame.presentTickDelta;
            {
                LARGE_INTEGER beforeTargetWait{};
                QueryPerformanceCounter(&beforeTargetWait);
                const std::uint64_t timeoutNs = targetQpc > beforeTargetWait.QuadPart && qpcFrequency > 0
                    ? static_cast<std::uint64_t>((targetQpc - beforeTargetWait.QuadPart) * 1'000'000'000ll / qpcFrequency)
                    : 0u;
                apusr::profile::WaitScope waitScope(
                    profile.stream,
                    profile.frameId,
                    profile.sampleSequence,
                    "pacing.wait_target_qpc",
                    apusr::profile::WaitKind::Timer,
                    0u,
                    static_cast<std::uint64_t>(targetQpc),
                    timeoutNs,
                    "presentation_target");
                waitForPerformanceCount(targetQpc, qpcFrequency, timerCaps.wPeriodMin, presenter->hybridSpinTime);
                waitScope.setResult(apusr::profile::WaitResult::Satisfied);
            }

            LARGE_INTEGER currentPresentQpc{};
            QueryPerformanceCounter(&currentPresentQpc);
            presenter->previousPresentQpc = currentPresentQpc.QuadPart;

            presentFrame(*presenter, entry, frameType, profile);
        }

        framesPresented = schedule.numFramesSentForPresentationBase + schedule.numFramesToPresent;
    }

    waitForFenceValue(presenter->presentFence, framesPresented);
    return 0;
}
} // namespace

void markPresentedBuffer(IDXGISwapChain4* swapchain, bool interpolated)
{
    const uint32_t backBufferIndex = swapchain->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer = nullptr;
    if (FAILED(swapchain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer))))
        return;

    FfxFrameInterpolationSwapChainResourceInfo info{};
    info.version = apusrFramegenMakeVersion(FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_MAJOR,
                                                 FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_MINOR,
                                                 FFX_FRAME_INTERPOLATION_SWAP_CHAIN_VERSION_PATCH);
    info.isInterpolated = interpolated;
    const HRESULT hr = backBuffer->SetPrivateData(IID_IFfxFrameInterpolationSwapChainResourceInfo, sizeof(info), &info);
    FFX_ASSERT(SUCCEEDED(hr));
    SafeRelease(backBuffer);
}

DWORD WINAPI interpolationThread(LPVOID context)
{
    auto* presenter = static_cast<FrameinterpolationPresentInfo*>(context);
    if (!presenter)
        return ERROR_INVALID_PARAMETER;

    if (presenter->profileStream) {
        apusr::profile::recordThreadInfo({
            .stream = presenter->profileStream,
            .name = "APUSR D3D12 Interpolation",
            .role = "framegen.interpolation",
            .priority = THREAD_PRIORITY_HIGHEST,
        });
    }

    HANDLE presenterThreadHandle = CreateThread(nullptr, 0, presenterThread, context, 0, nullptr);
    FFX_ASSERT(presenterThreadHandle != nullptr);
    if (!presenterThreadHandle)
        return GetLastError();

    SetThreadPriority(presenterThreadHandle, THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(presenterThreadHandle, L"APUSR Presenter Thread");

    PacingEstimator frameTime{};
    int64_t previousQpc = 0;
    int64_t previousDelta = 0;
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    const int64_t qpcFrequency = frequency.QuadPart;

    while (!presenter->shutdown) {
        WaitForSingleObject(presenter->presentEvent, INFINITE);
        if (presenter->shutdown)
            break;

        EnterCriticalSection(&presenter->criticalSectionScheduledFrame);
        PacingData entry = presenter->scheduledInterpolations;
        presenter->scheduledInterpolations.invalidate();
        LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);

        const apusr::profile::SampleDecision profileDecision = presenter->profileStream
            ? presenter->profileInterpolationSampleGate.decide(
                pacingProfileHash(entry.schedule), entry.schedule.frameId)
            : apusr::profile::SampleDecision{};
        const PacingProfile profile{
            .stream = profileDecision.capture ? presenter->profileStream : apusr::profile::StreamHandle{},
            .flowStream = entry.profileFlowStream,
            .frameId = entry.schedule.frameId,
            .occurrenceId = entry.profileOccurrenceId,
            .flowId = entry.profileFlowId,
            .sampleSequence = profileDecision.sampleSequence,
        };
        apusr::profile::CpuScope interpolationThreadScope(
            profile.stream, profile.frameId, profile.sampleSequence, "interpolation.thread_total");

        LARGE_INTEGER preWaitQpc{};
        QueryPerformanceCounter(&preWaitQpc);
        const bool allowBlockingFenceWait = shouldBlockForFence(
            presenter->previousPresentQpc,
            previousDelta,
            preWaitQpc.QuadPart,
            qpcFrequency,
            presenter->allowWaitForSingleObjectOnFence);

        apusr::profile::recordCounter({
            .stream = profile.stream,
            .frameId = profile.frameId,
            .name = "interpolation.blocking_fence_wait",
            .value = allowBlockingFenceWait ? 1u : 0u,
        });
        {
            const std::uint64_t interpolationValue =
                entry.schedule.frame(PresentFrameType::Generated).interpolationCompletedFenceValue;
            apusr::profile::WaitScope waitScope(
                profile.stream,
                profile.frameId,
                profile.sampleSequence,
                "interpolation.wait_gpu_completion",
                apusr::profile::WaitKind::GpuFence,
                reinterpret_cast<std::uintptr_t>(presenter->interpolationFence),
                interpolationValue,
                0u,
                allowBlockingFenceWait ? "blocking_fence" : "poll_or_callback");
            const bool waitSatisfied = waitForFenceValue(presenter->interpolationFence,
                              interpolationValue,
                              INFINITE,
                              nullptr,
                              allowBlockingFenceWait);
            waitScope.setResult(waitSatisfied ? apusr::profile::WaitResult::Satisfied
                                              : apusr::profile::WaitResult::Failed,
                                waitSatisfied ? interpolationValue : 0u);
        }
        profile.flow("interpolation.completed", apusr::profile::FlowPhase::Step,
                     entry.schedule.frame(PresentFrameType::Generated).interpolationCompletedFenceValue);

        SetEvent(presenter->interpolationEvent);

        uint64_t presentDelta = 0u;
        double deltaQpc = 0.0;
        {
            apusr::profile::CpuScope pacingEstimateScope(
                profile.stream, profile.frameId, profile.sampleSequence, "interpolation.estimate_pacing");
            LARGE_INTEGER currentQpcValue{};
            QueryPerformanceCounter(&currentQpcValue);
            const int64_t currentQpc = currentQpcValue.QuadPart;
            deltaQpc = static_cast<double>(currentQpc - previousQpc) * (previousQpc > 0);
            previousQpc = currentQpc;

            const double resetThreshold = static_cast<double>(qpcFrequency) * 0.1;
            if (deltaQpc > resetThreshold || presenter->resetTimer)
                frameTime.reset();
            else
                frameTime.update(deltaQpc);

            presentDelta = frameTime.targetDelta(
                qpcFrequency,
                presenter->safetyMarginInSec,
                presenter->varianceFactor);
            entry.schedule.frame(PresentFrameType::Generated).presentTickDelta = presentDelta;
            entry.schedule.frame(PresentFrameType::Real).presentTickDelta = presentDelta;
            previousDelta = static_cast<int64_t>(presentDelta);
        }
        apusr::profile::recordCounter({
            .stream = profile.stream,
            .frameId = profile.frameId,
            .name = "interpolation.present_delta_ticks",
            .value = presentDelta,
            .value2 = static_cast<std::uint64_t>(qpcFrequency),
        });
        if (deltaQpc > 0.0) {
            apusr::profile::recordCounter({
                .stream = profile.stream,
                .frameId = profile.frameId,
                .name = "interpolation.observed_delta_ticks",
                .value = static_cast<std::uint64_t>(deltaQpc),
                .value2 = static_cast<std::uint64_t>(qpcFrequency),
            });
        }

        {
            apusr::profile::CpuScope publishScope(
                profile.stream, profile.frameId, profile.sampleSequence, "interpolation.publish_schedule");
            EnterCriticalSection(&presenter->criticalSectionScheduledFrame);
            presenter->scheduledPresents = entry;
            LeaveCriticalSection(&presenter->criticalSectionScheduledFrame);
            profile.flow("presentation.scheduled", apusr::profile::FlowPhase::Step, presentDelta);
            SetEvent(presenter->pacerEvent);
        }
    }

    SetEvent(presenter->pacerEvent);
    WaitForSingleObject(presenterThreadHandle, INFINITE);
    CloseHandle(presenterThreadHandle);
    return 0;
}

} // namespace apusr::framegen::dx12
