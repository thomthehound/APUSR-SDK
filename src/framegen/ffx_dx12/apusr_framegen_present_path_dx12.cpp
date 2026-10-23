// This file includes presentation logic derived from the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "apusr_framegen_present_path_dx12.h"

#include "apusr_framegen_swapchain_dx12.h"
#include "apusr_framegen_dx12_helpers.h"
#include "apusr_framegen_pacing_dx12.h"
#include "apusr_framegen_backend_dx12.h"
#include "apusr_framegen_dx12_resource_compat.h"

#include <utility>

namespace apusr::framegen::dx12 {

namespace {

bool ensureProfileAdapter(FrameinterpolationPresentInfo& presentInfo) noexcept
{
    if (presentInfo.profileAdapter != nullptr)
        return true;
    if (presentInfo.device == nullptr)
        return false;

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return false;
    IDXGIAdapter3* adapter = nullptr;
    const LUID luid = presentInfo.device->GetAdapterLuid();
    const HRESULT hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    factory->Release();
    if (FAILED(hr)) {
        SafeRelease(adapter);
        return false;
    }
    presentInfo.profileAdapter = adapter;
    return true;
}

void recordVideoMemoryBudget(
    FrameinterpolationPresentInfo& presentInfo,
    const apusr::profile::StreamHandle stream,
    const std::uint64_t frameId,
    const std::uint32_t burstFrame) noexcept
{
    if (!stream || burstFrame != 0u)
        return;
    if (!ensureProfileAdapter(presentInfo)) {
        if (!presentInfo.profileMemoryBudgetUnavailableLogged) {
            presentInfo.profileMemoryBudgetUnavailableLogged = true;
            apusr::profile::recordInstant(stream, frameId, "memory.budget_unavailable");
        }
        return;
    }

    struct SegmentTotals {
        std::uint64_t usage = 0u;
        std::uint64_t budget = 0u;
        std::uint64_t reservation = 0u;
        std::uint64_t availableForReservation = 0u;
        std::uint32_t validNodes = 0u;
    };
    const auto querySegment = [&](const DXGI_MEMORY_SEGMENT_GROUP group) noexcept {
        SegmentTotals totals{};
        const UINT nodeCount = presentInfo.device->GetNodeCount();
        for (UINT node = 0u; node < nodeCount; ++node) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (FAILED(presentInfo.profileAdapter->QueryVideoMemoryInfo(node, group, &info)))
                continue;
            totals.usage += info.CurrentUsage;
            totals.budget += info.Budget;
            totals.reservation += info.CurrentReservation;
            totals.availableForReservation += info.AvailableForReservation;
            ++totals.validNodes;
        }
        return totals;
    };
    const auto emit = [&](const char* budgetName, const char* reservationName, const SegmentTotals& totals) noexcept {
        if (totals.validNodes == 0u)
            return;
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = stream,
            .frameId = frameId,
            .name = budgetName,
            .value = totals.usage,
            .value2 = totals.budget,
            .flags = totals.validNodes,
        });
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = stream,
            .frameId = frameId,
            .name = reservationName,
            .value = totals.reservation,
            .value2 = totals.availableForReservation,
            .flags = totals.validNodes,
        });
    };
    emit("memory.budget.local", "memory.reservation.local", querySegment(DXGI_MEMORY_SEGMENT_GROUP_LOCAL));
    emit("memory.budget.nonlocal", "memory.reservation.nonlocal", querySegment(DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL));
}

} // namespace

struct PresentPath::ProfileContext {
    apusr::profile::StreamHandle stream{};
    std::uint64_t frameId = 0u;
    std::uint64_t occurrenceId = 0u;
    std::uint64_t flowId = 0u;
    std::uint64_t sampleSequence = 0u;
    std::uint64_t submitSequence = 0u;

    void flow(const char* detail, apusr::profile::FlowPhase phase = apusr::profile::FlowPhase::Step,
              std::uint64_t value0 = 0u, std::uint64_t value1 = 0u) const noexcept
    {
        if (!stream || flowId == 0u)
            return;
        apusr::profile::recordFlowEvent({
            .stream = stream,
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

    void recordQueue(
        const char* name,
        ID3D12CommandQueue* queue,
        apusr::profile::QueueOperation operation,
        ID3D12Fence* fence = nullptr,
        const std::uint64_t value = 0u,
        ID3D12CommandQueue* relatedQueue = nullptr,
        const std::uint32_t objectCount = 0u) const noexcept
    {
        if (!stream || queue == nullptr)
            return;
        apusr::profile::recordQueueEvent(apusr::profile::QueueEvent{
            .stream = stream,
            .frameId = frameId,
            .sampleSequence = sampleSequence,
            .name = name,
            .nativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(queue)),
            .syncObject = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fence)),
            .syncValue = value,
            .relatedQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(relatedQueue)),
            .submitSequence = submitSequence,
            .operation = operation,
            .objectCount = objectCount,
        });
    }
};

HRESULT PresentPath::presentWithUiComposition(
    FrameInterpolationSwapChainDX12& swapchain, UINT syncInterval, UINT flags, const ProfileContext& profile)
{
    auto& presentInfo = swapchain.presentInfo;
    auto& replacementSwapBuffers = swapchain.replacementSwapBuffers;
    auto& replacementSwapBufferIndex = swapchain.replacementSwapBufferIndex;
    auto& uiReplacementBuffer = swapchain.uiReplacementBuffer;
    auto& presentCallback = swapchain.presentCallback;
    auto& presentCallbackContext = swapchain.presentCallbackContext;
    auto& currentFrameID = swapchain.currentFrameID;
    auto& framesSentForPresentation = swapchain.framesSentForPresentation;

    auto uiCompositionList = presentInfo.commandPool.get(presentInfo.presentQueue, L"APUSR UI composition");
    auto* list = uiCompositionList->reset();

    ID3D12Resource* dx12SwapchainBuffer = nullptr;
    const UINT currentBackBufferIndex = presentInfo.swapChain->GetCurrentBackBufferIndex();
    const HRESULT getBufferResult =
        presentInfo.swapChain->GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&dx12SwapchainBuffer));
    if (FAILED(getBufferResult)) {
        uiCompositionList->drop(true);
        return getBufferResult;
    }

    const FfxApiResourceDescription outBufferDesc = apusrFramegenResourceDescriptionDX12(dx12SwapchainBuffer);
    const FfxApiResourceDescription inBufferDesc =
        apusrFramegenResourceDescriptionDX12(replacementSwapBuffers[replacementSwapBufferIndex].resource);

    FfxPresentCallbackDescription desc{};
    desc.commandList = apusrFramegenCommandListDX12(list);
    desc.device = presentInfo.device;
    desc.isInterpolatedFrame = false;
    desc.outputSwapChainBuffer =
        apusrFramegenResourceDX12(dx12SwapchainBuffer, outBufferDesc, nullptr, FFX_API_RESOURCE_STATE_PRESENT);
    desc.currentBackBuffer = apusrFramegenResourceDX12(replacementSwapBuffers[replacementSwapBufferIndex].resource,
                                               inBufferDesc,
                                               nullptr,
                                               FFX_API_RESOURCE_STATE_PRESENT);
    if ((presentInfo.uiCompositionFlags & APUSR_FRAMEGEN_UI_COMPOSITION_INTERNAL_DOUBLE_BUFFER) &&
        uiReplacementBuffer.resource) {
        const FfxApiResourceDescription uiBufferDesc = apusrFramegenResourceDescriptionDX12(uiReplacementBuffer.resource);
        desc.currentUI = apusrFramegenResourceDX12(
            uiReplacementBuffer.resource, uiBufferDesc, nullptr, presentInfo.currentUiSurface.state);
    } else {
        desc.currentUI = presentInfo.currentUiSurface;
    }
    desc.usePremulAlpha = (presentInfo.uiCompositionFlags & APUSR_FRAMEGEN_UI_COMPOSITION_USE_PREMUL_ALPHA) != 0;
    desc.frameID = currentFrameID;

    if (!presentCallback) {
        uiCompositionList->drop(true);
        SafeRelease(dx12SwapchainBuffer);
        return E_POINTER;
    }
    if (presentCallback(&desc, presentCallbackContext) != FFX_OK) {
        uiCompositionList->drop(true);
        SafeRelease(dx12SwapchainBuffer);
        return E_FAIL;
    }

    profile.recordQueue("present.ui_composition", presentInfo.presentQueue, apusr::profile::QueueOperation::Submit,
                        nullptr, 0u, nullptr, 1u);
    uiCompositionList->execute(true);

    presentInfo.presentQueue->Signal(presentInfo.replacementBufferFence, ++framesSentForPresentation);
    presentInfo.presentQueue->Signal(presentInfo.compositionFenceGPU, framesSentForPresentation);
    presentInfo.compositionFenceCPU->Signal(framesSentForPresentation);

    markPresentedBuffer(presentInfo.swapChain, false);
    const std::uint64_t dxgiPresentBeginTick = profile.stream ? apusr::profile::cpuTick() : 0u;
    const HRESULT presentResult = presentInfo.swapChain->Present(syncInterval, flags);
    const std::uint64_t dxgiPresentEndTick = profile.stream ? apusr::profile::cpuTick() : 0u;
    recordDxgiPresentTiming(
        presentInfo, profile.stream, profile.frameId, profile.sampleSequence,
        dxgiPresentBeginTick, dxgiPresentEndTick, false);
    profile.flow(FAILED(presentResult) ? "real_present_failed" : "real_present",
                 FAILED(presentResult) ? apusr::profile::FlowPhase::Cancel : apusr::profile::FlowPhase::End,
                 static_cast<std::uint64_t>(static_cast<std::int64_t>(presentResult)));

    profile.recordQueue("present.present_fence", presentInfo.presentQueue, apusr::profile::QueueOperation::Signal,
                        presentInfo.presentFence, framesSentForPresentation, presentInfo.gameQueue);
    presentInfo.presentQueue->Signal(presentInfo.presentFence, framesSentForPresentation);
    profile.recordQueue("present.present_fence", presentInfo.gameQueue, apusr::profile::QueueOperation::Wait,
                        presentInfo.presentFence, framesSentForPresentation, presentInfo.presentQueue);
    presentInfo.gameQueue->Wait(presentInfo.presentFence, framesSentForPresentation);

    SafeRelease(dx12SwapchainBuffer);
    return presentResult;
}

void PresentPath::dispatchInterpolationCommands(FrameInterpolationSwapChainDX12& swapchain,
                                                FfxApiResource* interpolatedFrame,
                                                FfxApiResource* realFrame,
                                                const ProfileContext& profile)
{
    FFX_ASSERT(interpolatedFrame);
    FFX_ASSERT(realFrame);
    if (!interpolatedFrame || !realFrame)
        return;

    auto& presentInfo = swapchain.presentInfo;
    auto& replacementSwapBuffers = swapchain.replacementSwapBuffers;
    auto& interpolationOutputs = swapchain.interpolationOutputs;
    auto& registeredInterpolationCommandLists = swapchain.registeredInterpolationCommandLists;
    auto& interpolationBufferIndex = swapchain.interpolationBufferIndex;
    auto& interpolationFenceValue = swapchain.interpolationFenceValue;
    auto& frameInterpolationResetCondition = swapchain.frameInterpolationResetCondition;
    auto& frameGenerationCallback = swapchain.frameGenerationCallback;
    auto& frameGenerationCallbackContext = swapchain.frameGenerationCallbackContext;

    const UINT currentBackBufferIndex = swapchain.GetCurrentBackBufferIndex();
    ID3D12Resource* currentBackBuffer = replacementSwapBuffers[currentBackBufferIndex].resource;
    const FfxApiResourceDescription gameFrameDesc = apusrFramegenResourceDescriptionDX12(currentBackBuffer);
    const FfxApiResource backbuffer =
        apusrFramegenResourceDX12(currentBackBuffer, gameFrameDesc, nullptr, FFX_API_RESOURCE_STATE_PRESENT);

    *realFrame = backbuffer;

    const UINT64 outputAvailability = interpolationOutputs[interpolationBufferIndex].availabilityFenceValue;
    if (outputAvailability != 0) {
        profile.recordQueue("interpolation.output_available", presentInfo.interpolationQueue,
                            apusr::profile::QueueOperation::Wait, presentInfo.compositionFenceGPU,
                            outputAvailability, presentInfo.presentQueue);
        presentInfo.interpolationQueue->Wait(presentInfo.compositionFenceGPU, outputAvailability);
    }

    if (Dx12Commands* registeredCommandList = registeredInterpolationCommandLists[currentBackBufferIndex]) {
        profile.recordQueue("interpolation.registered", presentInfo.interpolationQueue,
                            apusr::profile::QueueOperation::Submit, nullptr, 0u, nullptr, 1u);
        registeredCommandList->execute(true);
        profile.recordQueue("interpolation.complete", presentInfo.interpolationQueue,
                            apusr::profile::QueueOperation::Signal, presentInfo.interpolationFence,
                            interpolationFenceValue + 1u, presentInfo.presentQueue);
        presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);
        profile.flow("interpolation.submitted", apusr::profile::FlowPhase::Step, interpolationFenceValue);
        *interpolatedFrame = swapchain.interpolationOutput();
        frameInterpolationResetCondition = false;
        return;
    }

    Dx12Commands* interpolationCommandList =
        presentInfo.commandPool.get(presentInfo.interpolationQueue, L"APUSR interpolation");
    auto* dx12CommandList = interpolationCommandList->reset();
    const bool captureGpuDispatch = interpolationCommandList->beginGpuProfile(
        profile.stream, profile.frameId, profile.sampleSequence, "framegen.dispatch_total");

    FfxFrameGenerationDispatchDescription desc{};
    desc.commandList = dx12CommandList;
    desc.outputs[0] = swapchain.interpolationOutput();
    desc.presentColor = backbuffer;
    desc.reset = frameInterpolationResetCondition;
    desc.numInterpolatedFrames = 1;
    desc.backBufferTransferFunction =
        static_cast<FfxApiBackbufferTransferFunction>(swapchain.backBufferTransferFunction);
    desc.minMaxLuminance[0] = swapchain.minLuminance;
    desc.minMaxLuminance[1] = swapchain.maxLuminance;
    desc.interpolationRect = swapchain.interpolationRect;
    desc.frameID = swapchain.currentFrameID;

    const bool dispatchSucceeded =
        frameGenerationCallback && frameGenerationCallback(&desc, frameGenerationCallbackContext) == FFX_OK;
    if (dispatchSucceeded) {
        if (captureGpuDispatch)
            interpolationCommandList->endGpuProfile();
        profile.recordQueue("interpolation.generated", presentInfo.interpolationQueue,
                            apusr::profile::QueueOperation::Submit, nullptr, 0u, nullptr, 1u);
        interpolationCommandList->execute(true);
        profile.recordQueue("interpolation.complete", presentInfo.interpolationQueue,
                            apusr::profile::QueueOperation::Signal, presentInfo.interpolationFence,
                            interpolationFenceValue + 1u, presentInfo.presentQueue);
        presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);
        profile.flow("interpolation.submitted", apusr::profile::FlowPhase::Step, interpolationFenceValue);
    } else {
        if (captureGpuDispatch)
            interpolationCommandList->cancelGpuProfile();
        interpolationCommandList->drop();
        desc.numInterpolatedFrames = 0;
        profile.flow("interpolation.dispatch_failed");
    }

    if (desc.numInterpolatedFrames > 0) {
        frameInterpolationResetCondition = false;
        *interpolatedFrame = swapchain.interpolationOutput();
    }
}

void PresentPath::presentInterpolated(
    FrameInterpolationSwapChainDX12& swapchain, UINT syncInterval, const ProfileContext& profile)
{
    auto& presentInfo = swapchain.presentInfo;
    auto& interpolationOutputs = swapchain.interpolationOutputs;
    auto& interpolationBufferIndex = swapchain.interpolationBufferIndex;
    auto& gameFenceValue = swapchain.gameFenceValue;
    auto& framesSentForPresentation = swapchain.framesSentForPresentation;

    const bool vsync = syncInterval > 0;

    profile.recordQueue("interpolation.game_ready", presentInfo.gameQueue, apusr::profile::QueueOperation::Signal,
                        presentInfo.gameFence, gameFenceValue + 1u, presentInfo.interpolationQueue);
    presentInfo.gameQueue->Signal(presentInfo.gameFence, ++gameFenceValue);
    profile.recordQueue("interpolation.game_ready", presentInfo.interpolationQueue, apusr::profile::QueueOperation::Wait,
                        presentInfo.gameFence, gameFenceValue, presentInfo.gameQueue);
    presentInfo.interpolationQueue->Wait(presentInfo.gameFence, gameFenceValue);

    FfxApiResource interpolatedFrame{};
    FfxApiResource realFrame{};
    dispatchInterpolationCommands(swapchain, &interpolatedFrame, &realFrame, profile);

    EnterCriticalSection(&presentInfo.criticalSectionScheduledFrame);

    PacingData entry{};
    entry.presentCallback = swapchain.presentCallback;
    entry.presentCallbackContext = swapchain.presentCallbackContext;
    entry.schedule.drawDebugPacingLines = swapchain.drawDebugPacingLines;

    if ((presentInfo.uiCompositionFlags & APUSR_FRAMEGEN_UI_COMPOSITION_INTERNAL_DOUBLE_BUFFER) &&
        swapchain.uiReplacementBuffer.resource) {
        const FfxApiResourceDescription uiBufferDesc =
            apusrFramegenResourceDescriptionDX12(swapchain.uiReplacementBuffer.resource);
        entry.uiSurface = apusrFramegenResourceDX12(
            swapchain.uiReplacementBuffer.resource, uiBufferDesc, nullptr, presentInfo.currentUiSurface.state);
    } else {
        entry.uiSurface = presentInfo.currentUiSurface;
    }

    entry.schedule.vsync = vsync;
    entry.schedule.tearingSupported = swapchain.tearingSupported;
    entry.schedule.numFramesSentForPresentationBase = framesSentForPresentation;
    entry.schedule.interpolationCompletedFenceValue = swapchain.interpolationFenceValue;
    entry.schedule.usePremulAlphaComposite =
        (presentInfo.uiCompositionFlags & APUSR_FRAMEGEN_UI_COMPOSITION_USE_PREMUL_ALPHA) != 0;
    entry.schedule.frameId = swapchain.currentFrameID;
    entry.profileFlowStream = profile.stream;
    entry.profileFlowId = profile.flowId;
    entry.profileOccurrenceId = profile.occurrenceId;

    ScheduledFrame& interpolated = entry.schedule.frame(PresentFrameType::Generated);
    if (interpolatedFrame.resource) {
        interpolated.enabled = true;
        interpolated.resource.value = interpolatedFrame.resource;
        interpolated.interpolationCompletedFenceValue = swapchain.interpolationFenceValue;
        interpolated.presentIndex = ++framesSentForPresentation;
    }

    if (!swapchain.presentInterpolatedOnly) {
        ScheduledFrame& real = entry.schedule.frame(PresentFrameType::Real);
        if (realFrame.resource) {
            real.enabled = true;
            real.resource.value = realFrame.resource;
            real.presentIndex = ++framesSentForPresentation;
        }
    }

    entry.schedule.replacementBufferFenceSignal = framesSentForPresentation;
    entry.schedule.numFramesToPresent = static_cast<UINT32>(
        framesSentForPresentation - entry.schedule.numFramesSentForPresentationBase);
    if (entry.schedule.numFramesToPresent == 0u)
        profile.flow("no_physical_present", apusr::profile::FlowPhase::Cancel);

    interpolationOutputs[interpolationBufferIndex].availabilityFenceValue =
        entry.schedule.numFramesSentForPresentationBase + interpolated.enabled;

    presentInfo.resetTimer = swapchain.frameInterpolationResetCondition;
    presentInfo.scheduledInterpolations = entry;
    LeaveCriticalSection(&presentInfo.criticalSectionScheduledFrame);

    SetEvent(presentInfo.presentEvent);
    swapchain.nextPresentWaitValue = entry.schedule.numFramesSentForPresentationBase;

}

bool PresentPath::verifyUiDuplicateResource(FrameInterpolationSwapChainDX12& swapchain)
{
    auto& presentInfo = swapchain.presentInfo;
    auto& uiReplacementBuffer = swapchain.uiReplacementBuffer;
    auto& totalUsageInBytes = swapchain.totalUsageInBytes;
    auto& framesSentForPresentation = swapchain.framesSentForPresentation;

    HRESULT hr = S_OK;
    ID3D12Device* device = nullptr;
    auto* uiResource = reinterpret_cast<ID3D12Resource*>(presentInfo.currentUiSurface.resource);

    if (!(presentInfo.uiCompositionFlags & APUSR_FRAMEGEN_UI_COMPOSITION_INTERNAL_DOUBLE_BUFFER) ||
        !uiResource) {
        if (uiReplacementBuffer.resource) {
            const uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
            totalUsageInBytes -= resourceSize;
            waitForFenceValue(
                presentInfo.compositionFenceGPU, framesSentForPresentation, INFINITE, presentInfo.waitCallback);
            SafeRelease(uiReplacementBuffer.resource);
            uiReplacementBuffer = {};
        }
        return true;
    }

    const auto uiResourceDesc = uiResource->GetDesc();
    if (uiReplacementBuffer.resource) {
        const auto internalDesc = uiReplacementBuffer.resource->GetDesc();
        if (uiResourceDesc.Format != internalDesc.Format || uiResourceDesc.Width != internalDesc.Width ||
            uiResourceDesc.Height != internalDesc.Height) {
            waitForFenceValue(
                presentInfo.compositionFenceGPU, framesSentForPresentation, INFINITE, presentInfo.waitCallback);
            const uint64_t oldResourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
            totalUsageInBytes -= oldResourceSize;
            SafeRelease(uiReplacementBuffer.resource);
        }
    }

    if (!uiReplacementBuffer.resource) {
        hr = uiResource->GetDevice(IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) {
            D3D12_HEAP_PROPERTIES heapProperties{};
            D3D12_HEAP_FLAGS heapFlags{};
            hr = uiResource->GetHeapProperties(&heapProperties, &heapFlags);

            if (SUCCEEDED(hr)) {
                heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
                heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
                heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
                heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

                hr = device->CreateCommittedResource(
                    &heapProperties,
                    heapFlags,
                    &uiResourceDesc,
                    apusr::framegen::dx12::toDx12ResourceState(
                        static_cast<FfxApiResourceState>(presentInfo.currentUiSurface.state)),
                    nullptr,
                    IID_PPV_ARGS(&uiReplacementBuffer.resource));
                if (SUCCEEDED(hr)) {
                    const uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
                    totalUsageInBytes += resourceSize;
                    uiReplacementBuffer.resource->SetName(L"APUSR internal UI resource");
                }
            }
            SafeRelease(device);
        }
    }

    return SUCCEEDED(hr);
}

void PresentPath::copyUiResource(FrameInterpolationSwapChainDX12& swapchain)
{
    auto& presentInfo = swapchain.presentInfo;
    auto copyList = presentInfo.commandPool.get(presentInfo.gameQueue, L"APUSR UI resource copy");
    auto* dx12List = copyList->reset();

    auto* source = reinterpret_cast<ID3D12Resource*>(presentInfo.currentUiSurface.resource);
    ID3D12Resource* destination = swapchain.uiReplacementBuffer.resource;

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = source;
    barriers[0].Transition.StateBefore =
        apusr::framegen::dx12::toDx12ResourceState(static_cast<FfxApiResourceState>(presentInfo.currentUiSurface.state));
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = destination;
    barriers[1].Transition.StateBefore =
        apusr::framegen::dx12::toDx12ResourceState(static_cast<FfxApiResourceState>(presentInfo.currentUiSurface.state));
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    dx12List->ResourceBarrier(_countof(barriers), barriers);

    dx12List->CopyResource(destination, source);

    for (auto& barrier : barriers)
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);

    dx12List->ResourceBarrier(_countof(barriers), barriers);
    copyList->execute(true);

    // The copy now owns this frame's UI contents; the external resource is not
    // retained across the asynchronous presentation interval.
    presentInfo.currentUiSurface.resource = nullptr;
}

HRESULT PresentPath::present(FrameInterpolationSwapChainDX12& swapchain, UINT syncInterval, UINT flags)
{
    auto& presentInfo = swapchain.presentInfo;
    auto& replacementSwapBuffers = swapchain.replacementSwapBuffers;
    auto& registeredInterpolationCommandLists = swapchain.registeredInterpolationCommandLists;
    auto& framesSentForPresentation = swapchain.framesSentForPresentation;
    auto& criticalSection = swapchain.criticalSection;

    const UINT64 previousFramesSentForPresentation = framesSentForPresentation;

    if (flags & DXGI_PRESENT_TEST)
        return presentInfo.swapChain->Present(syncInterval, flags);

    apusr::profile::SampleDecision profileDecision{};
    apusr::profile::StreamHandle sampledStream{};
    const std::uint64_t profileFrameId = swapchain.nextFrameGenerationConfig.frameID;
    const std::uint64_t profileOccurrenceId = ++swapchain.profilePresentSerial;
    std::uint64_t configurationHash = 0u;
    if ((apusr::profile::cpuEnabled() || apusr::profile::gpuEnabled()) && swapchain.profileStream) {
        configurationHash = 0x4150555350524553ull; // "APUSPRES"
        configurationHash = apusr::profile::hashCombine(configurationHash, syncInterval);
        configurationHash = apusr::profile::hashCombine(configurationHash, flags & ~DXGI_PRESENT_TEST);
        configurationHash = apusr::profile::hashCombine(configurationHash, swapchain.gameBufferCount);
        configurationHash = apusr::profile::hashCombine(configurationHash, presentInfo.uiCompositionFlags);
        configurationHash = apusr::profile::hashCombine(configurationHash, swapchain.nextFrameGenerationConfig.flags);
        configurationHash = apusr::profile::hashCombine(configurationHash,
            (swapchain.nextFrameGenerationConfig.frameGenerationEnabled ? 1ull : 0ull) |
            (swapchain.nextFrameGenerationConfig.onlyPresentInterpolated ? 2ull : 0ull) |
            (swapchain.nextFrameGenerationConfig.allowAsyncWorkloads ? 4ull : 0ull) |
            (swapchain.nextFrameGenerationConfig.drawDebugPacingLines ? 8ull : 0ull));
        profileDecision = swapchain.profileSampleGate.decide(configurationHash, profileFrameId);
        if (profileDecision.capture)
            sampledStream = swapchain.profileStream;
    }
    const std::uint64_t presentBeginTick = sampledStream && apusr::profile::cpuEnabled()
        ? apusr::profile::cpuTick() : 0u;
    std::uint64_t profileFlowId = 0u;
    if (sampledStream) {
        profileFlowId = apusr::profile::hashCombine(
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&swapchain)),
            profileOccurrenceId);
        if (profileFlowId == 0u)
            profileFlowId = profileOccurrenceId != 0u ? profileOccurrenceId : 1u;
    }
    const ProfileContext queueProfile{
        .stream = sampledStream,
        .frameId = profileFrameId,
        .occurrenceId = profileOccurrenceId,
        .flowId = profileFlowId,
        .sampleSequence = profileDecision.sampleSequence,
        .submitSequence = profileDecision.sampleSequence,
    };
    queueProfile.flow("source_present", apusr::profile::FlowPhase::Begin);
    apusr::profile::CpuScope presentScope(
        sampledStream, profileFrameId, profileDecision.sampleSequence, "present.total");
    const auto recordPresentFrame = [&](const std::uint32_t extraFlags = 0u) noexcept {
        if (!sampledStream)
            return;
        const std::uint64_t endTick = presentBeginTick != 0u ? apusr::profile::cpuTick() : 0u;
        const std::uint64_t elapsed = presentBeginTick != 0u
            ? apusr::profile::durationNs(presentBeginTick, endTick) : 0u;
        recordVideoMemoryBudget(presentInfo, sampledStream, profileFrameId, profileDecision.burstFrame);
        apusr::profile::recordFrameSample(apusr::profile::FrameSample{
            .stream = sampledStream,
            .frameId = profileFrameId,
            .sampleSequence = profileDecision.sampleSequence,
            .burstSequence = profileDecision.burstSequence,
            .configurationHash = configurationHash,
            .burstFrame = profileDecision.burstFrame,
            .cpuTotalNs = elapsed,
            .cpuRecordNs = elapsed,
            .flags = (profileDecision.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u) | extraFlags,
        });
    };

    {
        apusr::profile::CpuScope scope(
            sampledStream, profileFrameId, profileDecision.sampleSequence, "present.config_apply");
        swapchain.setFrameGenerationConfig(&swapchain.nextFrameGenerationConfig);
    }

    {
        apusr::profile::WaitScope wait(
            sampledStream, profileFrameId, profileDecision.sampleSequence,
            "present.wait_critical_section", apusr::profile::WaitKind::CpuPrimitive,
            reinterpret_cast<std::uintptr_t>(&criticalSection), 0u, 0u, "critical_section");
        EnterCriticalSection(&criticalSection);
        wait.setResult(apusr::profile::WaitResult::Satisfied);
    }

    const UINT currentBackBufferIndex = swapchain.GetCurrentBackBufferIndex();
    const bool callbackConfigured = swapchain.frameGenerationCallback != nullptr;
    const bool commandListConfigured = registeredInterpolationCommandLists[currentBackBufferIndex] != nullptr;
    const bool runInterpolation = swapchain.interpolationEnabled && (callbackConfigured || commandListConfigured);
    if (swapchain.profileStream) {
        const char* const decisionReason = runInterpolation
            ? "interpolation_ready"
            : (!swapchain.interpolationEnabled ? "interpolation_disabled" : "no_interpolation_work");
        apusr::profile::recordDecision({
            .stream = swapchain.profileStream,
            .frameId = profileFrameId,
            .name = "framegen.source_disposition",
            .reason = decisionReason,
            .outcome = runInterpolation
                ? apusr::profile::DecisionOutcome::Accepted
                : apusr::profile::DecisionOutcome::SourceOnly,
            .value0 = callbackConfigured ? 1u : 0u,
            .value1 = commandListConfigured ? 1u : 0u,
            .value2 = syncInterval,
            .value3 = swapchain.interpolationEnabled ? 1u : 0u,
            .flags = (!runInterpolation && swapchain.interpolationEnabled)
                ? apusr::profile::DecisionTriggerBurst : 0u,
        });
    }

    {
        apusr::profile::WaitScope wait(
            sampledStream, profileFrameId, profileDecision.sampleSequence,
            "present.wait_composition_cpu", apusr::profile::WaitKind::GpuFence,
            reinterpret_cast<std::uintptr_t>(presentInfo.compositionFenceCPU),
            previousFramesSentForPresentation, 0u, "composition_cpu");
        const bool satisfied = waitForFenceValue(
            presentInfo.compositionFenceCPU, previousFramesSentForPresentation);
        wait.setResult(satisfied ? apusr::profile::WaitResult::Satisfied
                                 : apusr::profile::WaitResult::Failed,
                       presentInfo.compositionFenceCPU
                           ? presentInfo.compositionFenceCPU->GetCompletedValue() : 0u);
    }
    if (previousFramesSentForPresentation != 0) {
        queueProfile.recordQueue("present.composition_ready", presentInfo.gameQueue,
                                 apusr::profile::QueueOperation::Wait, presentInfo.compositionFenceGPU,
                                 previousFramesSentForPresentation, presentInfo.presentQueue);
        presentInfo.gameQueue->Wait(presentInfo.compositionFenceGPU, previousFramesSentForPresentation);
    }

    {
        apusr::profile::CpuScope scope(
            sampledStream, profileFrameId, profileDecision.sampleSequence, "present.ui_prepare");
        if (verifyUiDuplicateResource(swapchain) &&
            (presentInfo.uiCompositionFlags & APUSR_FRAMEGEN_UI_COMPOSITION_INTERNAL_DOUBLE_BUFFER) &&
            presentInfo.currentUiSurface.resource) {
            copyUiResource(swapchain);
        }
    }

    swapchain.previousFrameWasInterpolated = runInterpolation;
    if (runInterpolation) {
        {
            apusr::profile::WaitScope wait(
                sampledStream, profileFrameId, profileDecision.sampleSequence,
                "present.wait_interpolation_event", apusr::profile::WaitKind::WorkerWake,
                reinterpret_cast<std::uintptr_t>(presentInfo.interpolationEvent), 0u, 0u,
                "interpolation_event");
            const DWORD waitResult = WaitForSingleObject(presentInfo.interpolationEvent, INFINITE);
            wait.setResult(waitResult == WAIT_OBJECT_0 ? apusr::profile::WaitResult::Satisfied
                                                       : apusr::profile::WaitResult::Failed);
        }
        {
            apusr::profile::CpuScope scope(
                sampledStream, profileFrameId, profileDecision.sampleSequence, "present.interpolated_submit");
            presentInterpolated(swapchain, syncInterval, queueProfile);
        }

        if (swapchain.frameLatencyWaitable.active())
            FFX_ASSERT(SUCCEEDED(swapchain.frameLatencyWaitable.signalOnCompletion(
                presentInfo.compositionFenceGPU, framesSentForPresentation)));
    } else {
        queueProfile.recordQueue("present.game_ready", presentInfo.gameQueue,
                                 apusr::profile::QueueOperation::Signal, presentInfo.gameFence,
                                 swapchain.gameFenceValue + 1u, presentInfo.presentQueue);
        presentInfo.gameQueue->Signal(presentInfo.gameFence, ++swapchain.gameFenceValue);
        queueProfile.recordQueue("present.game_ready", presentInfo.presentQueue,
                                 apusr::profile::QueueOperation::Wait, presentInfo.gameFence,
                                 swapchain.gameFenceValue, presentInfo.gameQueue);
        presentInfo.presentQueue->Wait(presentInfo.gameFence, swapchain.gameFenceValue);

        FFX_ASSERT(swapchain.presentCallback != nullptr);
        HRESULT presentResult = S_OK;
        {
            apusr::profile::CpuScope scope(
                sampledStream, profileFrameId, profileDecision.sampleSequence, "present.real_submit");
            presentResult = presentWithUiComposition(swapchain, syncInterval, flags, queueProfile);
        }
        if (FAILED(presentResult)) {
            // Error capture is independent of performance sampling.  A failed
            // Present must survive even when this frame was outside the sampled
            // burst that collects the expensive timing detail.
            if (swapchain.profileStream) {
                apusr::profile::recordError(
                    swapchain.profileStream, profileFrameId, "present.dxgi", apusr::StatusCode::RuntimeFailure,
                    "DXGI present failed in the APUSR FrameGen presentation path");
            }
            if (sampledStream) {
                apusr::profile::recordCounter(apusr::profile::Counter{
                    .stream = sampledStream, .frameId = profileFrameId, .name = "present.hresult",
                    .value = static_cast<std::uint32_t>(presentResult)});
            }
            // If composition reached the present queue, keep the replacement
            // buffer and latency timelines consistent even though DXGI failed.
            if (framesSentForPresentation != previousFramesSentForPresentation) {
                replacementSwapBuffers[currentBackBufferIndex].availabilityFenceValue = framesSentForPresentation;
                if (swapchain.frameLatencyWaitable.active())
                    FFX_ASSERT(SUCCEEDED(swapchain.frameLatencyWaitable.signalOnCompletion(
                        presentInfo.presentFence, framesSentForPresentation)));
            }
            recordPresentFrame(apusr::profile::FrameFailed);
            LeaveCriticalSection(&criticalSection);
            return presentResult;
        }

        if (swapchain.frameLatencyWaitable.active())
            FFX_ASSERT(SUCCEEDED(swapchain.frameLatencyWaitable.signalOnCompletion(
                presentInfo.presentFence, framesSentForPresentation)));
    }

    replacementSwapBuffers[currentBackBufferIndex].availabilityFenceValue = framesSentForPresentation;
    registeredInterpolationCommandLists[currentBackBufferIndex] = nullptr;
    ++swapchain.presentCount;
    swapchain.interpolationBufferIndex =
        static_cast<int>(swapchain.presentCount % _countof(swapchain.interpolationOutputs));
    swapchain.replacementSwapBufferIndex =
        static_cast<int>(swapchain.presentCount % swapchain.gameBufferCount);

    LeaveCriticalSection(&criticalSection);

    {
        const std::uint64_t replacementValue =
            replacementSwapBuffers[swapchain.replacementSwapBufferIndex].availabilityFenceValue;
        apusr::profile::WaitScope wait(
            sampledStream, profileFrameId, profileDecision.sampleSequence,
            "present.wait_replacement_buffer", apusr::profile::WaitKind::GpuFence,
            reinterpret_cast<std::uintptr_t>(presentInfo.replacementBufferFence),
            replacementValue, 0u, "replacement_buffer");
        const bool satisfied = waitForFenceValue(
            presentInfo.replacementBufferFence, replacementValue, INFINITE, presentInfo.waitCallback);
        wait.setResult(satisfied ? apusr::profile::WaitResult::Satisfied
                                 : apusr::profile::WaitResult::Failed,
                       presentInfo.replacementBufferFence
                           ? presentInfo.replacementBufferFence->GetCompletedValue() : 0u);
    }

    if (sampledStream) {
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = sampledStream, .frameId = profileFrameId, .name = "framegen.memory_bytes",
            .value = swapchain.totalUsageInBytes, .value2 = swapchain.aliasableUsageInBytes});
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = sampledStream, .frameId = profileFrameId, .name = "present.progress",
            .value = framesSentForPresentation, .value2 = swapchain.presentCount});
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = sampledStream, .frameId = profileFrameId, .name = "present.mode",
            .value = runInterpolation ? 1u : 0u, .value2 = syncInterval,
            .flags = profileDecision.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u});
        recordPresentFrame();
    }
    return S_OK;
}

} // namespace apusr::framegen::dx12
