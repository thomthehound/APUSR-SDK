// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_backend_vk.h"
#include "apusr_framegen_session_vk.h"
#include "apusr_framegen_present_resources_vk.h"
#include "apusr_framegen_present_images_vk.h"
#include "apusr_framegen_present_sync_vk.h"
#include "apusr_framegen_direct_wsi_vk.h"
#include "apusr_framegen_source_replay_vk.h"
#include "apusr_framegen_presentation_vk.h"
#include "apusr_framegen_streamline_vk.h"
#include "apusr_framegen_vk_spirv.h"
#include "ffx_frameinterpolation_shaderblobs.h"
#include "ffx_opticalflow_shaderblobs.h"
#include "../src/backends/gfx_vulkan/apusr_vulkan_minimal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <set>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
namespace vk = apusr::backends::vk;
namespace pack = apusr::framegen::vk;

struct FakeBuffer { std::uint64_t size; };
struct FakeImage { std::uint32_t width; std::uint32_t height; std::uint32_t mips; };
struct FakeMemory { std::vector<std::uint8_t> bytes; };
struct FakeCommandPool { std::vector<vk::CommandBuffer> commandBuffers; };
struct FakeFence { bool signaled = false; };
struct FakeSemaphore { bool timeline = false; std::uint64_t value = 0u; };
struct FakeQueryPool {
    std::array<std::uint64_t, 256> timestamps{};
    std::array<std::uint64_t, 256> availability{};
};
std::uint64_t g_fakeTimestamp = 1000u;

struct Probe {
    std::vector<std::uint32_t> layoutBindings;
    std::vector<std::uint32_t> writeBindings;
    std::vector<vk::DescriptorType> writeTypes;
    struct ImageTransitionRecord {
        std::uintptr_t commandBuffer = 0u;
        std::uintptr_t image = 0u;
        vk::ImageLayout oldLayout = vk::ImageLayoutUndefined;
        vk::ImageLayout newLayout = vk::ImageLayoutUndefined;
    };
    std::vector<std::pair<vk::ImageLayout, vk::ImageLayout>> imageTransitions;
    std::vector<ImageTransitionRecord> imageTransitionRecords;
    std::vector<vk::ImageSubresourceRange> imageTransitionRanges;
    std::vector<vk::ImageSubresourceRange> imageViewRanges;
    std::uint32_t descriptorPoolResets = 0;
    std::uint32_t dispatches = 0;
    std::array<std::uint32_t, 3> lastDispatch{};
    std::uint32_t pipelineCreates = 0;
    std::uint32_t copyBuffers = 0;
    std::uint32_t copyBufferToImage = 0;
    std::uint32_t copyImages = 0;
    std::uint32_t clearImages = 0;
    std::uint32_t fillBuffers = 0;
    std::uint32_t concurrentBufferCreates = 0;
    std::uint32_t concurrentImageCreates = 0;
    bool sawSpirvMagic = false;
    bool sawEntryCs = false;
    bool sawRequiredSubgroup64 = false;
    bool borrowedImageViewDestroyed = false;
    std::uint32_t destroyedOwnedImageViews = 0;
    std::uint32_t queueSubmits = 0;
    std::vector<std::uint32_t> queueSubmitInfoCounts;
    std::uint32_t queueWaitIdles = 0;
    std::vector<std::uintptr_t> submitWaitSemaphores;
    std::vector<std::uintptr_t> submitSignalSemaphores;
    std::vector<std::uint64_t> submitSignalValues;
    std::vector<std::vector<std::uintptr_t>> submitWaitBatches;
    std::vector<std::vector<std::uintptr_t>> submitSignalBatches;
    std::vector<std::vector<std::uint64_t>> submitWaitValueBatches;
    std::vector<std::vector<std::uint64_t>> submitSignalValueBatches;
    std::vector<std::vector<std::vector<std::uintptr_t>>> submitWaitBatchHistory;
    std::vector<std::vector<std::vector<std::uintptr_t>>> submitSignalBatchHistory;
    std::vector<std::vector<std::vector<std::uint64_t>>> submitWaitValueBatchHistory;
    std::vector<std::vector<std::vector<std::uint64_t>>> submitSignalValueBatchHistory;
    std::uintptr_t inputCompletionTimeline = 0u;
    bool failNextEndCommandBuffer = false;
} g;

struct PresentEvent {
    bool generated = false;
    std::uint32_t imageIndex = 0;
    std::vector<std::uintptr_t> waits;
};
std::vector<PresentEvent> g_presentEvents;
std::vector<int> g_presentationOrder;
bool g_sourceReplaySawUpstream = false;
bool g_sourceReplaySawProducerWait = false;
std::uint32_t g_sourceReplayDelayMs = 0u;
bool g_generationReady = true;
bool g_generationStartReady = true;
bool g_generationStartWaitSucceeds = false;
std::uint32_t g_generationStartWaitDelayMs = 0u;
bool g_timelineWaitSucceeds = false;
std::uint32_t g_timelineWaitDelayMs = 0u;
std::uint32_t g_timelineWaitCalls = 0u;
std::uint64_t g_lastTimelineWaitTimeoutNs = 0u;
std::uint64_t g_generationDecisionValue = 0u;
bool g_generationDecisionPresented = false;
std::uint64_t g_directDecisionValue = 0u;
std::uint32_t g_directDecisionImage = 0u;
bool g_directDecisionPresented = false;
bool g_directDecisionBridgePrepared = false;
bool g_relativeTimingSupported = true;
std::uint64_t g_relativeTimingDurationNs = 0u;
std::uint32_t g_relativeTimingCalls = 0u;
std::uint64_t g_generatedRelativeTimingDurationNs = 0u;
std::uint32_t g_generatedRelativeTimingCalls = 0u;
std::uint32_t g_presentDelayMs = 0u;

bool fakeTimelineReady(void*, void* semaphore, const std::uint64_t value) noexcept
{
    if (semaphore == reinterpret_cast<void*>(0xE00C) && value == 12u)
        return g_generationStartReady;
    return g_generationReady && semaphore == reinterpret_cast<void*>(0xE006) && value == 11u;
}

bool fakeTimelineWait(
    void*, void* semaphore, const std::uint64_t value, const std::uint64_t timeoutNanoseconds) noexcept
{
    ++g_timelineWaitCalls;
    g_lastTimelineWaitTimeoutNs = timeoutNanoseconds;
    if (semaphore == reinterpret_cast<void*>(0xE00C) && value == 12u) {
        if (g_generationStartWaitDelayMs != 0u)
            std::this_thread::sleep_for(std::chrono::milliseconds(g_generationStartWaitDelayMs));
        if (g_generationStartWaitSucceeds)
            g_generationStartReady = true;
        return g_generationStartWaitSucceeds;
    }
    if (g_timelineWaitDelayMs != 0u)
        std::this_thread::sleep_for(std::chrono::milliseconds(g_timelineWaitDelayMs));
    return g_timelineWaitSucceeds && semaphore == reinterpret_cast<void*>(0xE006) && value == 11u;
}

void fakeGenerationDecision(void*, const std::uint64_t, const std::uint32_t,
                            const std::uint64_t generationCompletionValue,
                            const bool generatedFramePresented) noexcept
{
    g_generationDecisionValue = generationCompletionValue;
    g_generationDecisionPresented = generatedFramePresented;
}

ApusrFramegenResult fakePrepareDirectPresent(
    void*, const std::uint32_t imageIndex, void* generationTimelineSemaphore,
    const std::uint64_t generationTimelineValue, void** outPresentWaitSemaphore) noexcept
{
    if (!outPresentWaitSemaphore || imageIndex != 1u ||
        generationTimelineSemaphore != reinterpret_cast<void*>(0xE006) || generationTimelineValue != 11u)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    *outPresentWaitSemaphore = reinterpret_cast<void*>(0xE009);
    return APUSR_FRAMEGEN_OK;
}

void fakeDirectGenerationDecision(
    void*, const std::uint64_t, const std::uint32_t imageIndex, void*,
    const std::uint64_t generationTimelineValue, const bool generatedFramePresented,
    const bool presentBridgePrepared) noexcept
{
    g_directDecisionImage = imageIndex;
    g_directDecisionValue = generationTimelineValue;
    g_directDecisionPresented = generatedFramePresented;
    g_directDecisionBridgePrepared = presentBridgePrepared;
}

std::int32_t fakePresentImage(
    void*, void*, void*, const std::uint32_t imageIndex, void* const* waits,
    const std::uint32_t waitCount, const std::uint64_t, const bool, const bool generated) noexcept
{
    if (g_presentDelayMs != 0u)
        std::this_thread::sleep_for(std::chrono::milliseconds(g_presentDelayMs));
    PresentEvent event{};
    event.generated = generated;
    event.imageIndex = imageIndex;
    for (std::uint32_t i = 0; i < waitCount; ++i)
        event.waits.push_back(reinterpret_cast<std::uintptr_t>(waits[i]));
    g_presentEvents.push_back(std::move(event));
    g_presentationOrder.push_back(generated ? 1 : 3);
    return 0;
}

std::int32_t fakePresentSourceRelative(
    void*, void*, void*, const std::uint32_t imageIndex, void* const* waits,
    const std::uint32_t waitCount, const std::uint64_t, const bool,
    const std::uint64_t minimumPreviousImageVisibleNs, bool* outScheduled) noexcept
{
    if (!outScheduled)
        return -3;
    ++g_relativeTimingCalls;
    if (!g_relativeTimingSupported) {
        *outScheduled = false;
        return 0;
    }
    *outScheduled = true;
    g_relativeTimingDurationNs = minimumPreviousImageVisibleNs;
    PresentEvent event{};
    event.generated = false;
    event.imageIndex = imageIndex;
    for (std::uint32_t i = 0; i < waitCount; ++i)
        event.waits.push_back(reinterpret_cast<std::uintptr_t>(waits[i]));
    g_presentEvents.push_back(std::move(event));
    g_presentationOrder.push_back(3);
    return 0;
}

std::int32_t fakePresentGeneratedRelative(
    void*, void*, void*, const std::uint32_t imageIndex, void* const* waits,
    const std::uint32_t waitCount, const std::uint64_t, const bool,
    const std::uint64_t minimumPreviousImageVisibleNs, bool* outScheduled) noexcept
{
    if (!outScheduled)
        return -3;
    ++g_generatedRelativeTimingCalls;
    if (!g_relativeTimingSupported) {
        *outScheduled = false;
        return 0;
    }
    *outScheduled = true;
    g_generatedRelativeTimingDurationNs = minimumPreviousImageVisibleNs;
    PresentEvent event{};
    event.generated = true;
    event.imageIndex = imageIndex;
    for (std::uint32_t i = 0; i < waitCount; ++i)
        event.waits.push_back(reinterpret_cast<std::uintptr_t>(waits[i]));
    g_presentEvents.push_back(std::move(event));
    g_presentationOrder.push_back(1);
    return 0;
}

ApusrFramegenResult fakePrepareSourceReplay(
    void*, const std::uint32_t sourceSlotIndex, const std::uint64_t completionValue,
    void* const* producerWaitSemaphores, const std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore, const std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex, void** outWaitSemaphore, bool* outSourceCopied) noexcept
{
    const bool noUpstream = upstreamWaitSemaphore == nullptr && upstreamWaitValue == 0u;
    const bool expectedUpstream = upstreamWaitSemaphore == reinterpret_cast<void*>(0xE006) && upstreamWaitValue == 11u;
    const bool ordinaryReplay = completionValue == 9u &&
        producerWaitSemaphoreCount == 0u && producerWaitSemaphores == nullptr &&
        (noUpstream || expectedUpstream);
    const bool directProducerReplay = completionValue == 12u && noUpstream &&
        producerWaitSemaphoreCount == 1u && producerWaitSemaphores != nullptr &&
        producerWaitSemaphores[0] == reinterpret_cast<void*>(0xE008);
    if (!outImageIndex || !outWaitSemaphore || !outSourceCopied || sourceSlotIndex != 2u ||
        (!ordinaryReplay && !directProducerReplay))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    g_sourceReplaySawUpstream = expectedUpstream;
    g_sourceReplaySawProducerWait = directProducerReplay;
    if (g_sourceReplayDelayMs != 0u)
        std::this_thread::sleep_for(std::chrono::milliseconds(g_sourceReplayDelayMs));
    g_presentationOrder.push_back(2);
    *outImageIndex = 0u;
    *outWaitSemaphore = reinterpret_cast<void*>(0xE004);
    *outSourceCopied = true;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult fakePrepareGeneratedReplay(
    void*, const std::uint32_t sourceSlotIndex, const std::uint64_t completionValue,
    void* const* producerWaitSemaphores, const std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore, const std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex, void** outWaitSemaphore, bool* outCopied) noexcept
{
    if (!outImageIndex || !outWaitSemaphore || !outCopied || sourceSlotIndex != 2u || completionValue != 10u ||
        producerWaitSemaphoreCount != 0u || producerWaitSemaphores != nullptr ||
        upstreamWaitSemaphore != reinterpret_cast<void*>(0xE006) || upstreamWaitValue != 11u)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    g_presentationOrder.push_back(0);
    *outImageIndex = 1u;
    *outWaitSemaphore = reinterpret_cast<void*>(0xE007);
    *outCopied = true;
    return APUSR_FRAMEGEN_OK;
}

std::uint64_t g_lastReplayAcquireTimeout = 0u;
std::int32_t g_nextReplayAcquireResult = 0;
std::vector<std::uint32_t> g_releasedReplayImages;


std::int32_t fakeAcquireReplayImage(
    void*, void*, void*, const std::uint64_t timeout, void*, std::uint32_t* outImageIndex) noexcept
{
    if (!outImageIndex)
        return -1;
    g_lastReplayAcquireTimeout = timeout;
    const std::int32_t result = g_nextReplayAcquireResult;
    g_nextReplayAcquireResult = 0;
    if (result == 0)
        *outImageIndex = 2u;
    return result;
}

bool fakeReleaseReplayImage(
    void*, void*, void*, const std::uint32_t imageIndex) noexcept
{
    if (imageIndex >= 3u)
        return false;
    g_releasedReplayImages.push_back(imageIndex);
    return true;
}

template <typename T>
T handle(std::uintptr_t value) { return reinterpret_cast<T>(value); }

vk::Result fakeCreateCommandPool(vk::Device, const vk::CommandPoolCreateInfo*, const vk::AllocationCallbacks*, vk::CommandPool* out)
{
    *out = reinterpret_cast<vk::CommandPool>(new FakeCommandPool);
    return vk::Success;
}
void fakeDestroyCommandPool(vk::Device, vk::CommandPool pool, const vk::AllocationCallbacks*)
{
    auto* p = reinterpret_cast<FakeCommandPool*>(pool);
    for (vk::CommandBuffer commandBuffer : p->commandBuffers)
        delete reinterpret_cast<std::uint64_t*>(commandBuffer);
    delete p;
}
vk::Result fakeAllocateCommandBuffers(vk::Device, const vk::CommandBufferAllocateInfo* info, vk::CommandBuffer* out)
{
    auto* pool = reinterpret_cast<FakeCommandPool*>(info->commandPool);
    for (std::uint32_t i = 0; i < info->commandBufferCount; ++i) {
        out[i] = reinterpret_cast<vk::CommandBuffer>(new std::uint64_t(i + 1));
        pool->commandBuffers.push_back(out[i]);
    }
    return vk::Success;
}
vk::Result fakeResetCommandBuffer(vk::CommandBuffer, vk::CommandBufferResetFlags) { return vk::Success; }
vk::Result fakeBeginCommandBuffer(vk::CommandBuffer, const vk::CommandBufferBeginInfo*) { return vk::Success; }
vk::Result fakeEndCommandBuffer(vk::CommandBuffer)
{
    if (g.failNextEndCommandBuffer) {
        g.failNextEndCommandBuffer = false;
        return static_cast<vk::Result>(-1);
    }
    return vk::Success;
}
vk::Result fakeCreateSemaphore(vk::Device, const vk::SemaphoreCreateInfo* info, const vk::AllocationCallbacks*, vk::Semaphore* out)
{
    bool timeline = false;
    std::uint64_t initialValue = 0u;
    if (info && info->pNext) {
        const auto* type = static_cast<const vk::SemaphoreTypeCreateInfo*>(info->pNext);
        if (type->sType != vk::StructureTypeSemaphoreTypeCreateInfo ||
            type->semaphoreType != vk::SemaphoreTypeTimeline)
            return -1;
        timeline = true;
        initialValue = type->initialValue;
    }
    auto* semaphore = new FakeSemaphore{timeline, initialValue};
    *out = reinterpret_cast<vk::Semaphore>(semaphore);
    if (timeline && g.inputCompletionTimeline == 0u)
        g.inputCompletionTimeline = reinterpret_cast<std::uintptr_t>(*out);
    return vk::Success;
}
void fakeDestroySemaphore(vk::Device, vk::Semaphore semaphore, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeSemaphore*>(semaphore);
}
vk::Result fakeGetSemaphoreCounterValue(vk::Device, vk::Semaphore semaphore, std::uint64_t* out)
{
    if (!semaphore || !out) return -1;
    const auto* fake = reinterpret_cast<const FakeSemaphore*>(semaphore);
    if (!fake->timeline) return -1;
    *out = fake->value;
    return vk::Success;
}
vk::Result fakeWaitSemaphores(vk::Device, const vk::SemaphoreWaitInfo* info, std::uint64_t)
{
    if (!info || info->semaphoreCount != 1u || !info->pSemaphores || !info->pValues)
        return -1;
    const auto* fake = reinterpret_cast<const FakeSemaphore*>(info->pSemaphores[0]);
    if (!fake || !fake->timeline)
        return -1;
    return fake->value >= info->pValues[0] ? vk::Success : vk::Timeout;
}
vk::Result fakeCreateFence(vk::Device, const vk::FenceCreateInfo* info, const vk::AllocationCallbacks*, vk::Fence* out)
{
    *out = reinterpret_cast<vk::Fence>(new FakeFence{(info->flags & vk::FenceCreateSignaledBit) != 0u});
    return vk::Success;
}
void fakeDestroyFence(vk::Device, vk::Fence fence, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeFence*>(fence);
}
vk::Result fakeGetFenceStatus(vk::Device, vk::Fence fence)
{
    return reinterpret_cast<FakeFence*>(fence)->signaled ? vk::Success : vk::NotReady;
}
vk::Result fakeWaitForFences(vk::Device, std::uint32_t count, const vk::Fence* fences, vk::Bool32, std::uint64_t)
{
    for (std::uint32_t i = 0; i < count; ++i)
        reinterpret_cast<FakeFence*>(fences[i])->signaled = true;
    return vk::Success;
}
vk::Result fakeResetFences(vk::Device, std::uint32_t count, const vk::Fence* fences)
{
    for (std::uint32_t i = 0; i < count; ++i)
        reinterpret_cast<FakeFence*>(fences[i])->signaled = false;
    return vk::Success;
}
vk::Result fakeQueueSubmit(vk::Queue, std::uint32_t count, const vk::SubmitInfo* submits, vk::Fence fence)
{
    if (count == 0u || !submits) return -1;
    ++g.queueSubmits;
    g.queueSubmitInfoCounts.push_back(count);
    g.submitWaitBatches.clear();
    g.submitSignalBatches.clear();
    g.submitWaitValueBatches.clear();
    g.submitSignalValueBatches.clear();
    for (std::uint32_t submitIndex = 0; submitIndex < count; ++submitIndex) {
        const vk::SubmitInfo& submit = submits[submitIndex];
        std::vector<std::uintptr_t> waits;
        for (std::uint32_t i = 0; i < submit.waitSemaphoreCount; ++i)
            waits.push_back(reinterpret_cast<std::uintptr_t>(submit.pWaitSemaphores[i]));
        std::vector<std::uintptr_t> signals;
        for (std::uint32_t i = 0; i < submit.signalSemaphoreCount; ++i)
            signals.push_back(reinterpret_cast<std::uintptr_t>(submit.pSignalSemaphores[i]));
        std::vector<std::uint64_t> waitValues(submit.waitSemaphoreCount, 0u);
        std::vector<std::uint64_t> signalValues;
        if (submit.pNext) {
            const auto* timeline = static_cast<const vk::TimelineSemaphoreSubmitInfo*>(submit.pNext);
            if (timeline->sType != vk::StructureTypeTimelineSemaphoreSubmitInfo ||
                timeline->waitSemaphoreValueCount != submit.waitSemaphoreCount ||
                timeline->signalSemaphoreValueCount != submit.signalSemaphoreCount ||
                (timeline->waitSemaphoreValueCount != 0u && !timeline->pWaitSemaphoreValues) ||
                (timeline->signalSemaphoreValueCount != 0u && !timeline->pSignalSemaphoreValues))
                return -1;
            for (std::uint32_t i = 0; i < timeline->waitSemaphoreValueCount; ++i) {
                const std::uint64_t value = timeline->pWaitSemaphoreValues[i];
                waitValues[i] = value;
                if (value != 0u) {
                    const auto* semaphore = reinterpret_cast<const FakeSemaphore*>(submit.pWaitSemaphores[i]);
                    if (!semaphore || !semaphore->timeline || semaphore->value < value) return -1;
                }
            }
            for (std::uint32_t i = 0; i < timeline->signalSemaphoreValueCount; ++i) {
                const std::uint64_t value = timeline->pSignalSemaphoreValues[i];
                signalValues.push_back(value);
                if (value != 0u) {
                    auto* semaphore = reinterpret_cast<FakeSemaphore*>(submit.pSignalSemaphores[i]);
                    if (!semaphore || !semaphore->timeline) return -1;
                    semaphore->value = value;
                }
            }
        } else {
            signalValues.assign(submit.signalSemaphoreCount, 0u);
        }
        g.submitWaitBatches.push_back(std::move(waits));
        g.submitSignalBatches.push_back(std::move(signals));
        g.submitWaitValueBatches.push_back(std::move(waitValues));
        g.submitSignalValueBatches.push_back(std::move(signalValues));
    }
    g.submitWaitBatchHistory.push_back(g.submitWaitBatches);
    g.submitSignalBatchHistory.push_back(g.submitSignalBatches);
    g.submitWaitValueBatchHistory.push_back(g.submitWaitValueBatches);
    g.submitSignalValueBatchHistory.push_back(g.submitSignalValueBatches);
    g.submitWaitSemaphores = g.submitWaitBatches.back();
    g.submitSignalSemaphores = g.submitSignalBatches.back();
    g.submitSignalValues = g.submitSignalValueBatches.back();
    if (fence)
        reinterpret_cast<FakeFence*>(fence)->signaled = true;
    return vk::Success;
}

vk::Result fakeQueueWaitIdle(vk::Queue) { ++g.queueWaitIdles; return vk::Success; }

vk::Result fakeCreateBuffer(vk::Device, const vk::BufferCreateInfo* info, const vk::AllocationCallbacks*, vk::Buffer* out)
{
    if (info->sharingMode == vk::SharingModeConcurrent && info->queueFamilyIndexCount == 2u &&
        info->pQueueFamilyIndices && info->pQueueFamilyIndices[0] == 2u && info->pQueueFamilyIndices[1] == 3u)
        ++g.concurrentBufferCreates;
    *out = reinterpret_cast<vk::Buffer>(new FakeBuffer{info->size});
    return vk::Success;
}
void fakeDestroyBuffer(vk::Device, vk::Buffer buffer, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeBuffer*>(buffer);
}
void fakeGetBufferMemoryRequirements(vk::Device, vk::Buffer buffer, vk::MemoryRequirements* out)
{
    out->size = std::max<std::uint64_t>(reinterpret_cast<FakeBuffer*>(buffer)->size, 256);
    out->alignment = 256;
    out->memoryTypeBits = 0x3;
}
vk::Result fakeBindBufferMemory(vk::Device, vk::Buffer, vk::DeviceMemory, vk::DeviceSize) { return vk::Success; }

vk::Result fakeCreateImage(vk::Device, const vk::ImageCreateInfo* info, const vk::AllocationCallbacks*, vk::Image* out)
{
    if (info->sharingMode == vk::SharingModeConcurrent && info->queueFamilyIndexCount == 2u &&
        info->pQueueFamilyIndices && info->pQueueFamilyIndices[0] == 2u && info->pQueueFamilyIndices[1] == 3u)
        ++g.concurrentImageCreates;
    *out = reinterpret_cast<vk::Image>(new FakeImage{info->extent.width, info->extent.height, info->mipLevels});
    return vk::Success;
}
void fakeDestroyImage(vk::Device, vk::Image image, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeImage*>(image);
}
void fakeGetImageMemoryRequirements(vk::Device, vk::Image image, vk::MemoryRequirements* out)
{
    const auto* i = reinterpret_cast<FakeImage*>(image);
    out->size = std::max<std::uint64_t>(static_cast<std::uint64_t>(i->width) * i->height * 8, 4096);
    out->alignment = 256;
    out->memoryTypeBits = 0x1;
}
vk::Result fakeBindImageMemory(vk::Device, vk::Image, vk::DeviceMemory, vk::DeviceSize) { return vk::Success; }

vk::Result fakeAllocateMemory(vk::Device, const vk::MemoryAllocateInfo* info, const vk::AllocationCallbacks*, vk::DeviceMemory* out)
{
    auto* memory = new FakeMemory;
    memory->bytes.resize(static_cast<std::size_t>(info->allocationSize));
    *out = reinterpret_cast<vk::DeviceMemory>(memory);
    return vk::Success;
}
void fakeFreeMemory(vk::Device, vk::DeviceMemory memory, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeMemory*>(memory);
}
vk::Result fakeMapMemory(vk::Device, vk::DeviceMemory memory, vk::DeviceSize offset, vk::DeviceSize, vk::MemoryMapFlags, void** out)
{
    auto& bytes = reinterpret_cast<FakeMemory*>(memory)->bytes;
    if (offset > bytes.size()) return -1;
    *out = bytes.data() + static_cast<std::size_t>(offset);
    return vk::Success;
}
void fakeUnmapMemory(vk::Device, vk::DeviceMemory) {}

vk::Result fakeCreateImageView(vk::Device, const vk::ImageViewCreateInfo* info, const vk::AllocationCallbacks*, vk::ImageView* out)
{
    g.imageViewRanges.push_back(info->subresourceRange);
    *out = reinterpret_cast<vk::ImageView>(new std::uint64_t(1));
    return vk::Success;
}
void fakeDestroyImageView(vk::Device, vk::ImageView view, const vk::AllocationCallbacks*)
{
    if (reinterpret_cast<std::uintptr_t>(view) < 0x100000u) {
        g.borrowedImageViewDestroyed = true;
        return;
    }
    ++g.destroyedOwnedImageViews;
    delete reinterpret_cast<std::uint64_t*>(view);
}

vk::Result fakeCreateShaderModule(vk::Device, const vk::ShaderModuleCreateInfo* info, const vk::AllocationCallbacks*, vk::ShaderModule* out)
{
    g.sawSpirvMagic = info->codeSize >= 4 && info->pCode && info->pCode[0] == 0x07230203u;
    *out = handle<vk::ShaderModule>(0x1001);
    return vk::Success;
}
void fakeDestroyShaderModule(vk::Device, vk::ShaderModule, const vk::AllocationCallbacks*) {}

vk::Result fakeCreateDescriptorSetLayout(vk::Device, const vk::DescriptorSetLayoutCreateInfo* info, const vk::AllocationCallbacks*, vk::DescriptorSetLayout* out)
{
    g.layoutBindings.clear();
    for (std::uint32_t i = 0; i < info->bindingCount; ++i) g.layoutBindings.push_back(info->pBindings[i].binding);
    *out = handle<vk::DescriptorSetLayout>(0x2001);
    return vk::Success;
}
void fakeDestroyDescriptorSetLayout(vk::Device, vk::DescriptorSetLayout, const vk::AllocationCallbacks*) {}
vk::Result fakeCreatePipelineLayout(vk::Device, const vk::PipelineLayoutCreateInfo*, const vk::AllocationCallbacks*, vk::PipelineLayout* out)
{
    *out = handle<vk::PipelineLayout>(0x3001);
    return vk::Success;
}
void fakeDestroyPipelineLayout(vk::Device, vk::PipelineLayout, const vk::AllocationCallbacks*) {}
vk::Result fakeCreateComputePipelines(vk::Device, vk::PipelineCache, std::uint32_t count, const vk::ComputePipelineCreateInfo* infos, const vk::AllocationCallbacks*, vk::Pipeline* out)
{
    if (count != 1) return -1;
    g.sawEntryCs = std::string_view(infos[0].stage.pName) == "CS";
    if (infos[0].stage.pNext) {
        const auto* subgroup = static_cast<const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo*>(
            infos[0].stage.pNext);
        g.sawRequiredSubgroup64 =
            subgroup->sType == vk::StructureTypePipelineShaderStageRequiredSubgroupSizeCreateInfo &&
            subgroup->requiredSubgroupSize == 64u;
    }
    ++g.pipelineCreates;
    *out = handle<vk::Pipeline>(0x4001);
    return vk::Success;
}
void fakeDestroyPipeline(vk::Device, vk::Pipeline, const vk::AllocationCallbacks*) {}

vk::Result fakeCreateDescriptorPool(vk::Device, const vk::DescriptorPoolCreateInfo*, const vk::AllocationCallbacks*, vk::DescriptorPool* out)
{
    static std::uintptr_t next = 0x5000;
    *out = handle<vk::DescriptorPool>(++next);
    return vk::Success;
}
void fakeDestroyDescriptorPool(vk::Device, vk::DescriptorPool, const vk::AllocationCallbacks*) {}
vk::Result fakeResetDescriptorPool(vk::Device, vk::DescriptorPool, vk::DescriptorPoolResetFlags)
{
    ++g.descriptorPoolResets;
    return vk::Success;
}
vk::Result fakeAllocateDescriptorSets(vk::Device, const vk::DescriptorSetAllocateInfo*, vk::DescriptorSet* out)
{
    static std::uintptr_t next = 0x6000;
    *out = handle<vk::DescriptorSet>(++next);
    return vk::Success;
}
void fakeUpdateDescriptorSets(vk::Device, std::uint32_t count, const vk::WriteDescriptorSet* writes, std::uint32_t, const vk::CopyDescriptorSet*)
{
    g.writeBindings.clear();
    g.writeTypes.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        g.writeBindings.push_back(writes[i].dstBinding);
        g.writeTypes.push_back(writes[i].descriptorType);
    }
}

vk::Result fakeCreateSampler(vk::Device, const vk::SamplerCreateInfo*, const vk::AllocationCallbacks*, vk::Sampler* out)
{
    static std::uintptr_t next = 0x7000;
    *out = handle<vk::Sampler>(++next);
    return vk::Success;
}
void fakeDestroySampler(vk::Device, vk::Sampler, const vk::AllocationCallbacks*) {}

void fakeCmdPipelineBarrier(vk::CommandBuffer commandBuffer, vk::PipelineStageFlags, vk::PipelineStageFlags, vk::DependencyFlags,
                            std::uint32_t, const vk::MemoryBarrier*, std::uint32_t, const vk::BufferMemoryBarrier*,
                            std::uint32_t imageCount, const vk::ImageMemoryBarrier* images)
{
    for (std::uint32_t i = 0; i < imageCount; ++i) {
        g.imageTransitions.emplace_back(images[i].oldLayout, images[i].newLayout);
        g.imageTransitionRecords.push_back(Probe::ImageTransitionRecord{
            reinterpret_cast<std::uintptr_t>(commandBuffer),
            reinterpret_cast<std::uintptr_t>(images[i].image),
            images[i].oldLayout,
            images[i].newLayout});
        g.imageTransitionRanges.push_back(images[i].subresourceRange);
    }
}
void fakeCmdClearColorImage(vk::CommandBuffer, vk::Image, vk::ImageLayout, const vk::ClearColorValue*, std::uint32_t, const vk::ImageSubresourceRange*) { ++g.clearImages; }
void fakeCmdCopyBuffer(vk::CommandBuffer, vk::Buffer, vk::Buffer, std::uint32_t, const vk::BufferCopy*) { ++g.copyBuffers; }
void fakeCmdCopyBufferToImage(vk::CommandBuffer, vk::Buffer, vk::Image, vk::ImageLayout, std::uint32_t, const vk::BufferImageCopy*) { ++g.copyBufferToImage; }
void fakeCmdCopyImage(vk::CommandBuffer, vk::Image, vk::ImageLayout, vk::Image, vk::ImageLayout, std::uint32_t, const vk::ImageCopy*) { ++g.copyImages; }
void fakeCmdFillBuffer(vk::CommandBuffer, vk::Buffer, vk::DeviceSize, vk::DeviceSize, std::uint32_t) { ++g.fillBuffers; }
void fakeCmdBindPipeline(vk::CommandBuffer, vk::PipelineBindPoint, vk::Pipeline) {}
void fakeCmdBindDescriptorSets(vk::CommandBuffer, vk::PipelineBindPoint, vk::PipelineLayout, std::uint32_t, std::uint32_t, const vk::DescriptorSet*, std::uint32_t, const std::uint32_t*) {}
void fakeCmdDispatch(vk::CommandBuffer, std::uint32_t x, std::uint32_t y, std::uint32_t z)
{
    ++g.dispatches;
    g.lastDispatch = {x, y, z};
}

vk::Result fakeCreateQueryPool(
    vk::Device, const vk::QueryPoolCreateInfo* info, const vk::AllocationCallbacks*, vk::QueryPool* out)
{
    if (!info || !out || info->queryType != vk::QueryTypeTimestamp || info->queryCount > 256u)
        return -1;
    *out = reinterpret_cast<vk::QueryPool>(new FakeQueryPool{});
    return vk::Success;
}
void fakeDestroyQueryPool(vk::Device, vk::QueryPool pool, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeQueryPool*>(pool);
}
void fakeCmdResetQueryPool(vk::CommandBuffer, vk::QueryPool pool, std::uint32_t first, std::uint32_t count)
{
    auto* query = reinterpret_cast<FakeQueryPool*>(pool);
    for (std::uint32_t i = first; i < first + count && i < query->timestamps.size(); ++i) {
        query->timestamps[i] = 0u;
        query->availability[i] = 0u;
    }
}
void fakeCmdWriteTimestamp(vk::CommandBuffer, vk::PipelineStageFlags, vk::QueryPool pool, std::uint32_t index)
{
    auto* query = reinterpret_cast<FakeQueryPool*>(pool);
    if (index >= query->timestamps.size())
        return;
    query->timestamps[index] = g_fakeTimestamp;
    query->availability[index] = 1u;
    g_fakeTimestamp += 100u;
}
vk::Result fakeGetQueryPoolResults(
    vk::Device, vk::QueryPool pool, std::uint32_t first, std::uint32_t count,
    std::size_t dataSize, void* data, vk::DeviceSize stride, vk::QueryResultFlags flags)
{
    if (!pool || !data || (flags & vk::QueryResult64Bit) == 0u ||
        (flags & vk::QueryResultWithAvailabilityBit) == 0u || stride < 16u)
        return -1;
    auto* query = reinterpret_cast<FakeQueryPool*>(pool);
    if (dataSize < static_cast<std::size_t>(count) * static_cast<std::size_t>(stride))
        return -1;
    for (std::uint32_t i = 0u; i < count; ++i) {
        auto* row = reinterpret_cast<std::uint64_t*>(
            static_cast<std::uint8_t*>(data) + static_cast<std::size_t>(i) * static_cast<std::size_t>(stride));
        row[0] = query->timestamps[first + i];
        row[1] = query->availability[first + i];
    }
    return vk::Success;
}
void fakeGetPhysicalDeviceProperties(vk::PhysicalDevice, vk::PhysicalDeviceProperties* out)
{
    *out = {};
    out->limits.timestampPeriod = 1.0f;
}

void fakeGetPhysicalDeviceMemoryProperties(vk::PhysicalDevice, vk::PhysicalDeviceMemoryProperties* out)
{
    *out = {};
    out->memoryTypeCount = 2;
    out->memoryTypes[0].propertyFlags = vk::MemoryPropertyDeviceLocalBit;
    out->memoryTypes[1].propertyFlags = vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit;
}

void fakeGetPhysicalDeviceFormatProperties(vk::PhysicalDevice, vk::Format, vk::FormatProperties* out)
{
    *out = {};
    out->optimalTilingFeatures =
        vk::FormatFeatureSampledImageBit | vk::FormatFeatureStorageImageBit |
        vk::FormatFeatureTransferSrcBit | vk::FormatFeatureTransferDstBit;
}

vk::PfnVoidFunction fakeGetDeviceProcAddr(vk::Device, const char* name)
{
#define FN(n, f) if (std::strcmp(name, n) == 0) return reinterpret_cast<vk::PfnVoidFunction>(f)
    FN("vkCreateCommandPool", fakeCreateCommandPool); FN("vkDestroyCommandPool", fakeDestroyCommandPool);
    FN("vkAllocateCommandBuffers", fakeAllocateCommandBuffers); FN("vkResetCommandBuffer", fakeResetCommandBuffer);
    FN("vkBeginCommandBuffer", fakeBeginCommandBuffer); FN("vkEndCommandBuffer", fakeEndCommandBuffer);
    FN("vkCreateSemaphore", fakeCreateSemaphore); FN("vkDestroySemaphore", fakeDestroySemaphore);
    FN("vkGetSemaphoreCounterValue", fakeGetSemaphoreCounterValue);
    FN("vkWaitSemaphores", fakeWaitSemaphores);
    FN("vkCreateFence", fakeCreateFence); FN("vkDestroyFence", fakeDestroyFence); FN("vkGetFenceStatus", fakeGetFenceStatus);
    FN("vkWaitForFences", fakeWaitForFences); FN("vkResetFences", fakeResetFences);
    FN("vkQueueSubmit", fakeQueueSubmit); FN("vkQueueWaitIdle", fakeQueueWaitIdle);
    FN("vkCreateQueryPool", fakeCreateQueryPool); FN("vkDestroyQueryPool", fakeDestroyQueryPool);
    FN("vkGetQueryPoolResults", fakeGetQueryPoolResults); FN("vkCmdResetQueryPool", fakeCmdResetQueryPool);
    FN("vkCmdWriteTimestamp", fakeCmdWriteTimestamp);
    FN("vkCreateBuffer", fakeCreateBuffer); FN("vkDestroyBuffer", fakeDestroyBuffer);
    FN("vkGetBufferMemoryRequirements", fakeGetBufferMemoryRequirements); FN("vkBindBufferMemory", fakeBindBufferMemory);
    FN("vkCreateImage", fakeCreateImage); FN("vkDestroyImage", fakeDestroyImage);
    FN("vkGetImageMemoryRequirements", fakeGetImageMemoryRequirements); FN("vkBindImageMemory", fakeBindImageMemory);
    FN("vkAllocateMemory", fakeAllocateMemory); FN("vkFreeMemory", fakeFreeMemory); FN("vkMapMemory", fakeMapMemory); FN("vkUnmapMemory", fakeUnmapMemory);
    FN("vkCreateImageView", fakeCreateImageView); FN("vkDestroyImageView", fakeDestroyImageView);
    FN("vkCreateShaderModule", fakeCreateShaderModule); FN("vkDestroyShaderModule", fakeDestroyShaderModule);
    FN("vkCreateDescriptorSetLayout", fakeCreateDescriptorSetLayout); FN("vkDestroyDescriptorSetLayout", fakeDestroyDescriptorSetLayout);
    FN("vkCreatePipelineLayout", fakeCreatePipelineLayout); FN("vkDestroyPipelineLayout", fakeDestroyPipelineLayout);
    FN("vkCreateComputePipelines", fakeCreateComputePipelines); FN("vkDestroyPipeline", fakeDestroyPipeline);
    FN("vkCreateDescriptorPool", fakeCreateDescriptorPool); FN("vkDestroyDescriptorPool", fakeDestroyDescriptorPool); FN("vkResetDescriptorPool", fakeResetDescriptorPool);
    FN("vkAllocateDescriptorSets", fakeAllocateDescriptorSets); FN("vkUpdateDescriptorSets", fakeUpdateDescriptorSets);
    FN("vkCreateSampler", fakeCreateSampler); FN("vkDestroySampler", fakeDestroySampler);
    FN("vkCmdPipelineBarrier", fakeCmdPipelineBarrier); FN("vkCmdClearColorImage", fakeCmdClearColorImage);
    FN("vkCmdCopyBuffer", fakeCmdCopyBuffer); FN("vkCmdCopyBufferToImage", fakeCmdCopyBufferToImage); FN("vkCmdCopyImage", fakeCmdCopyImage); FN("vkCmdFillBuffer", fakeCmdFillBuffer);
    FN("vkCmdBindPipeline", fakeCmdBindPipeline); FN("vkCmdBindDescriptorSets", fakeCmdBindDescriptorSets); FN("vkCmdDispatch", fakeCmdDispatch);
#undef FN
    return nullptr;
}

vk::PfnVoidFunction fakeGetInstanceProcAddr(vk::Instance, const char* name)
{
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<vk::PfnVoidFunction>(fakeGetDeviceProcAddr);
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) return reinterpret_cast<vk::PfnVoidFunction>(fakeGetPhysicalDeviceMemoryProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0) return reinterpret_cast<vk::PfnVoidFunction>(fakeGetPhysicalDeviceProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceFormatProperties") == 0) return reinterpret_cast<vk::PfnVoidFunction>(fakeGetPhysicalDeviceFormatProperties);
    return nullptr;
}

bool hasBinding(const std::vector<std::uint32_t>& bindings, std::uint32_t value)
{
    return std::find(bindings.begin(), bindings.end(), value) != bindings.end();
}

bool spirvContainsOpcode(const std::uint8_t* bytes, std::uint32_t size, std::uint16_t requiredOpcode)
{
    if (!bytes || size < 20u || (size & 3u) != 0u) return false;
    const auto* words = reinterpret_cast<const std::uint32_t*>(bytes);
    const std::size_t wordCount = size / 4u;
    for (std::size_t i = 5u; i < wordCount;) {
        const std::uint32_t instruction = words[i];
        const std::uint32_t count = instruction >> 16u;
        if (count == 0u || i + count > wordCount) return false;
        if ((instruction & 0xffffu) == requiredOpcode) return true;
        i += count;
    }
    return false;
}

int validateShaderPack()
{
    if (pack::framegenSpirvPermutationCount() != 204 || pack::framegenSpirvWordCount() == 0) return 1;

    ApusrFramegenShaderBlob fiBlob{};
    constexpr std::uint32_t fiOptions = (1u << 0) | (1u << 3) | (1u << 4);
    if (frameInterpolationGetPermutationBlobByIndex(APUSR_FRAMEINTERPOLATION_PASS_SETUP, fiOptions, &fiBlob) != APUSR_FRAMEGEN_OK ||
        fiBlob.effect != APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION ||
        fiBlob.passId != APUSR_FRAMEINTERPOLATION_PASS_SETUP || fiBlob.permutationOptions != fiOptions || !fiBlob.data || fiBlob.size == 0) return 6;

    ApusrFramegenShaderBlob ofBlob{};
    constexpr std::uint32_t ofOptions = (1u << 0) | (1u << 1);
    if (opticalflowGetPermutationBlobByIndex(APUSR_OPTICALFLOW_PASS_PREPARE_LUMA, ofOptions, &ofBlob) != APUSR_FRAMEGEN_OK ||
        ofBlob.effect != APUSR_FRAMEGEN_SHADER_EFFECT_OPTICAL_FLOW ||
        ofBlob.passId != APUSR_OPTICALFLOW_PASS_PREPARE_LUMA || ofBlob.permutationOptions != ofOptions || !ofBlob.data || ofBlob.size == 0) return 7;
    std::set<std::array<std::uint32_t, 3>> keys;
    std::set<std::pair<std::uint32_t, std::uint32_t>> blobs;
    const auto* entries = pack::framegenSpirvPermutations();
    for (std::size_t i = 0; i < pack::framegenSpirvPermutationCount(); ++i) {
        const auto& e = entries[i];
        if (!keys.insert({e.effect, e.passId, e.options}).second) return 2;
        if ((e.effect == APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION && (e.options & (1u << 3)) != 0) ||
            (e.effect == APUSR_FRAMEGEN_SHADER_EFFECT_OPTICAL_FLOW && (e.options & 1u) != 0)) return 3;
        std::uint32_t size = 0;
        const std::uint8_t* bytes = pack::findFramegenSpirv(e.effect, e.passId, e.options, &size);
        if (!bytes || size != e.size || size < 20 || (size & 3u) != 0 || reinterpret_cast<const std::uint32_t*>(bytes)[0] != 0x07230203u) return 4;
        blobs.insert({e.offset, e.size});
    }
    if (blobs.size() != 27) return 5;

    // Vulkan optical flow must use the native packed unsigned integer-dot path
    // for byte SAD rather than the scalar byte-extract fallback.
    std::uint32_t opticalFlowSize = 0u;
    const std::uint8_t* opticalFlowSpirv = pack::findFramegenSpirv(
        APUSR_FRAMEGEN_SHADER_EFFECT_OPTICAL_FLOW, APUSR_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5,
        1u << 1, &opticalFlowSize);
    if (!spirvContainsOpcode(opticalFlowSpirv, opticalFlowSize, 4451u)) return 8; // OpUDot / OpUDotKHR
    return 0;
}

} // namespace

int runContract()
{
    static_assert(std::is_trivially_copyable_v<ApusrFramegenShaderBlob>);
    if (const int shaderPackResult = validateShaderPack(); shaderPackResult != 0) return 10 + shaderPackResult;

    const std::filesystem::path profilePath =
        std::filesystem::temp_directory_path() / "apusr_framegen_vulkan_gpu_timestamp_contract.csv";
    std::error_code profileEc;
    std::filesystem::remove(profilePath, profileEc);
#if defined(_WIN32)
    _putenv_s("APUSR_FRAMEGEN_VULKAN_GPU_TIMESTAMPS", "1");
    _putenv_s("APUSR_FRAMEGEN_VULKAN_GPU_TIMESTAMPS_FILE", profilePath.string().c_str());
#else
    setenv("APUSR_FRAMEGEN_VULKAN_GPU_TIMESTAMPS", "1", 1);
    setenv("APUSR_FRAMEGEN_VULKAN_GPU_TIMESTAMPS_FILE", profilePath.string().c_str(), 1);
#endif

    ApusrFramegenVulkanDeviceContext device{};
    device.instance = handle<void*>(0x1);
    device.physicalDevice = handle<void*>(0x2);
    device.device = handle<void*>(0x3);
    device.getInstanceProcAddr = reinterpret_cast<void*>(fakeGetInstanceProcAddr);
    device.getDeviceProcAddr = reinterpret_cast<void*>(fakeGetDeviceProcAddr);
    device.minUniformBufferOffsetAlignment = 256;
    device.subgroupSize = 32;
    device.subgroupSizeMin = 32;
    device.subgroupSizeMax = 64;
    device.subgroupSizeControlSupported = true;
    device.fp16Supported = true;
    device.internalQueueFamilyIndices = {2u, 3u};
    device.internalQueueFamilyIndexCount = 2u;

    ApusrFramegenBackendInterface backend{};
    if (apusrFramegenCreateVulkanBackend(device, &backend, 1, {}) != APUSR_FRAMEGEN_OK || !backend.scratchBuffer) return 20;
    std::uint32_t effect = 99;
    if (backend.createContext(&backend, &effect) != APUSR_FRAMEGEN_OK || effect != 0) return 21;
    ApusrFramegenDeviceCapabilities caps{};
    if (backend.getDeviceCapabilities(&backend, &caps) != APUSR_FRAMEGEN_OK || caps.waveLaneCountMin != 32 || caps.waveLaneCountMax != 64 || !caps.fp16Supported) return 22;

    std::array<std::uint8_t, 2> initPixels{{0, 0}};
    ApusrFramegenCreateResourceDescription initializedDesc{};
    initializedDesc.heapType = APUSR_FRAMEGEN_HEAP_DEFAULT;
    initializedDesc.resourceDescription = {APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM, 1, 1, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY};
    initializedDesc.initialState = APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ;
    initializedDesc.name = L"contract initialized texture";
    initializedDesc.initData = ApusrFramegenResourceInitData::fromBuffer(initPixels.size(), initPixels.data());
    ApusrFramegenResource initialized{};
    if (backend.createResource(&backend, &initializedDesc, effect, &initialized) != APUSR_FRAMEGEN_OK) return 23;

    ApusrFramegenCreateResourceDescription ownedDesc{};
    ownedDesc.heapType = APUSR_FRAMEGEN_HEAP_DEFAULT;
    ownedDesc.resourceDescription = {APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_FLOAT, 64, 32, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV};
    ownedDesc.initialState = APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS;
    ownedDesc.name = L"contract uav";
    ownedDesc.initData = {};
    ownedDesc.initData.type = APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED;
    ApusrFramegenResource uav{};
    if (backend.createResource(&backend, &ownedDesc, effect, &uav) != APUSR_FRAMEGEN_OK) return 24;

    ApusrFramegenResource copyTarget{};
    ownedDesc.name = L"contract copy target";
    if (backend.createResource(&backend, &ownedDesc, effect, &copyTarget) != APUSR_FRAMEGEN_OK) return 25;

    ApusrFramegenCreateResourceDescription bufferDesc{};
    bufferDesc.heapType = APUSR_FRAMEGEN_HEAP_DEFAULT;
    bufferDesc.resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER;
    bufferDesc.resourceDescription.size = 16;
    bufferDesc.resourceDescription.stride = 4;
    bufferDesc.resourceDescription.mipCount = 1;
    bufferDesc.resourceDescription.usage = APUSR_FRAMEGEN_RESOURCE_USAGE_UAV;
    bufferDesc.initialState = APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS;
    bufferDesc.name = L"contract buffer";
    bufferDesc.initData.type = APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED;
    ApusrFramegenResource bufferA{}, bufferB{};
    if (backend.createResource(&backend, &bufferDesc, effect, &bufferA) != APUSR_FRAMEGEN_OK ||
        backend.createResource(&backend, &bufferDesc, effect, &bufferB) != APUSR_FRAMEGEN_OK) return 26;

    FakeImage externalImage{64, 32, 4};
    ApusrFramegenVulkanExternalResourceMetadata externalMetadata{};
    externalMetadata.imageView = handle<void*>(0xBEEF);
    externalMetadata.nativeFormat = static_cast<std::uint32_t>(vk::FormatD32Sfloat);
    externalMetadata.nativeLayout = static_cast<std::uint32_t>(vk::ImageLayoutDepthStencilReadOnlyOptimal);
    externalMetadata.aspectMask = vk::ImageAspectDepthBit;
    externalMetadata.baseMipLevel = 2;
    externalMetadata.levelCount = 1;
    externalMetadata.baseArrayLayer = 3;
    externalMetadata.layerCount = 1;
    ApusrFramegenResourceDescription externalDescription{
        APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN,
        64, 32, 1, 4, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE,
        APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY | APUSR_FRAMEGEN_RESOURCE_USAGE_DEPTHTARGET};
    const std::size_t viewsBeforeExternalRegistration = g.imageViewRanges.size();
    ApusrFramegenExternalResource external = apusrFramegenExternalResourceVK(
        &externalImage, externalDescription, APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ, &externalMetadata);
    ApusrFramegenResource srv{};
    if (backend.registerResource(&backend, &external, effect, &srv) != APUSR_FRAMEGEN_OK) return 27;
    if (g.imageViewRanges.size() != viewsBeforeExternalRegistration ||
        apusrFramegenSurfaceFormatFromVulkan(static_cast<std::uint32_t>(vk::FormatD32Sfloat)) != APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT) return 271;

    const char* cbName[] = {"cb0"}; const std::uint32_t cbReg[] = {0}, one[] = {1}, zero[] = {0};
    const char* srvName[] = {"r_input"}; const std::uint32_t srvReg[] = {0};
    const char* uavName[] = {"rw_output"}; const std::uint32_t uavReg[] = {0};
    ApusrFramegenShaderBlob blob{};
    blob.data = reinterpret_cast<const std::uint8_t*>(pack::framegenSpirvWords());
    blob.size = 4;
    blob.cbvCount = 1; blob.srvTextureCount = 1; blob.uavTextureCount = 1;
    blob.boundConstantBufferNames = cbName; blob.boundConstantBuffers = cbReg; blob.boundConstantBufferCounts = one; blob.boundConstantBufferSpaces = zero;
    blob.boundSRVTextureNames = srvName; blob.boundSRVTextures = srvReg; blob.boundSRVTextureCounts = one; blob.boundSRVTextureSpaces = zero;
    blob.boundUAVTextureNames = uavName; blob.boundUAVTextures = uavReg; blob.boundUAVTextureCounts = one; blob.boundUAVTextureSpaces = zero;
    blob.effect = APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION;
    blob.passId = 0;
    blob.permutationOptions = 1u << 3; // The Vulkan backend must normalize the DX12-only wave64 bit.

    ApusrFramegenSamplerDescription sampler{APUSR_FRAMEGEN_FILTER_MINMAGMIP_LINEAR, APUSR_FRAMEGEN_ADDRESS_CLAMP, APUSR_FRAMEGEN_ADDRESS_CLAMP, APUSR_FRAMEGEN_ADDRESS_CLAMP};
    ApusrFramegenPipelineDescription pipelineDesc{};
    pipelineDesc.samplers = &sampler;
    pipelineDesc.samplerCount = 1;
    std::wcsncpy(pipelineDesc.name, L"contract pipeline", APUSR_FRAMEGEN_RESOURCE_NAME_SIZE - 1);
    ApusrFramegenPipeline pipeline{};
    if (backend.createPipeline(&backend, &blob, &pipelineDesc, effect, &pipeline) != APUSR_FRAMEGEN_OK || !g.sawSpirvMagic || !g.sawEntryCs || !g.sawRequiredSubgroup64 || g.pipelineCreates != 1) return 28;
    if (pipeline.shaderEffect != APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION ||
        pipeline.shaderPassId != 0u || pipeline.shaderPermutationOptions != (1u << 3) ||
        pipeline.shaderByteSize == 0u || pipeline.requiredSubgroupSize != 64u) return 281;
    if (!hasBinding(g.layoutBindings, 0) || !hasBinding(g.layoutBindings, 32) || !hasBinding(g.layoutBindings, 64) || !hasBinding(g.layoutBindings, 96)) return 29;

    std::array<std::uint32_t, 4> constants{{1, 2, 3, 4}};
    ApusrFramegenConstantBuffer staged{};
    if (backend.stageConstants(&backend, constants.data(), sizeof(constants), &staged) != APUSR_FRAMEGEN_OK) return 30;

    ApusrFramegenGpuJob compute{};
    compute.jobType = APUSR_FRAMEGEN_JOB_COMPUTE;
    compute.computeJob.pipeline = &pipeline;
    compute.computeJob.dimensions[0] = 7; compute.computeJob.dimensions[1] = 5; compute.computeJob.dimensions[2] = 1;
    compute.computeJob.srvTextures[0].resource = srv;
    compute.computeJob.uavTextures[0].resource = uav;
    compute.computeJob.uavTextures[0].mip = 0;
    compute.computeJob.constantBuffers[0] = staged;
    compute.computeJob.profileLevel = 3u;
    if (backend.scheduleJob(&backend, &compute) != APUSR_FRAMEGEN_OK) return 31;

    ApusrFramegenGpuJob clearImage{};
    clearImage.jobType = APUSR_FRAMEGEN_JOB_CLEAR_FLOAT;
    clearImage.clearJob.target = uav;
    clearImage.clearJob.color[0] = 0.25f;
    if (backend.scheduleJob(&backend, &clearImage) != APUSR_FRAMEGEN_OK) return 32;

    ApusrFramegenGpuJob discard{};
    discard.jobType = APUSR_FRAMEGEN_JOB_DISCARD;
    discard.discardJob.target = copyTarget;
    if (backend.scheduleJob(&backend, &discard) != APUSR_FRAMEGEN_OK) return 33;

    ApusrFramegenGpuJob copyImage{};
    copyImage.jobType = APUSR_FRAMEGEN_JOB_COPY;
    copyImage.copyJob.src = uav;
    copyImage.copyJob.dst = copyTarget;
    if (backend.scheduleJob(&backend, &copyImage) != APUSR_FRAMEGEN_OK) return 34;

    ApusrFramegenGpuJob clearBuffer{};
    clearBuffer.jobType = APUSR_FRAMEGEN_JOB_CLEAR_FLOAT;
    clearBuffer.clearJob.target = bufferA;
    clearBuffer.clearJob.color[0] = 0.0f;
    if (backend.scheduleJob(&backend, &clearBuffer) != APUSR_FRAMEGEN_OK) return 35;

    ApusrFramegenGpuJob copyBuffer{};
    copyBuffer.jobType = APUSR_FRAMEGEN_JOB_COPY;
    copyBuffer.copyJob.src = bufferA;
    copyBuffer.copyJob.dst = bufferB;
    copyBuffer.copyJob.size = 16;
    if (backend.scheduleJob(&backend, &copyBuffer) != APUSR_FRAMEGEN_OK) return 36;

    const auto commandBuffer = handle<ApusrFramegenCommandList>(0x9001);
    const ApusrFramegenVulkanGpuProfileFrameInfo profileInfo{
        .frameId = 77u,
        .renderWidth = 64u,
        .renderHeight = 32u,
        .displayWidth = 128u,
        .displayHeight = 64u,
        .interpolationX = 4,
        .interpolationY = 2,
        .interpolationWidth = 120,
        .interpolationHeight = 60,
        .frameTimeDeltaMs = 16.6667f,
        .jitterX = 0.25f,
        .jitterY = -0.125f,
        .motionVectorScaleX = 128.0f,
        .motionVectorScaleY = 64.0f,
        .prepareFlags = 0x12u,
        .dispatchFlags = 0x34u,
        .hasHudlessColor = true,
        .motionVectorsAtDisplayResolution = true,
        .motionVectorsJittered = true,
        .hdr = true,
        .reset = false,
    };
    apusrFramegenBeginVulkanGpuProfile(&backend, commandBuffer, profileInfo);
    if (backend.executeJobs(&backend, commandBuffer, effect) != APUSR_FRAMEGEN_OK || g.dispatches != 1 ||
        g.lastDispatch != std::array<std::uint32_t, 3>{7, 5, 1}) return 37;
    apusrFramegenEndVulkanGpuProfile(&backend, commandBuffer);
    apusrFramegenFlushVulkanGpuProfile(&backend);

    // FidelityFX SPD reflects a fixed inpainting-pyramid UAV tail even when a
    // particular resolution has fewer real mip levels. Vulkan requires those
    // inactive descriptor slots to contain valid image views. The FI SPD passes
    // may therefore alias an out-of-range tail slot to the last real mip, but an
    // ordinary compute pass must retain strict mip validation.
    ApusrFramegenPipeline spdTailPipeline = pipeline;
    spdTailPipeline.shaderPassId = APUSR_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID;
    ApusrFramegenGpuJob spdTailCompute = compute;
    spdTailCompute.computeJob.pipeline = &spdTailPipeline;
    spdTailCompute.computeJob.uavTextures[0].mip = 1u; // contract UAV has only mip 0
    if (backend.scheduleJob(&backend, &spdTailCompute) != APUSR_FRAMEGEN_OK ||
        backend.executeJobs(&backend, commandBuffer, effect) != APUSR_FRAMEGEN_OK || g.dispatches != 2u)
        return 372;

    ApusrFramegenPipeline strictPipeline = pipeline;
    strictPipeline.shaderPassId = APUSR_FRAMEINTERPOLATION_PASS_SETUP;
    ApusrFramegenGpuJob strictMipCompute = compute;
    strictMipCompute.computeJob.pipeline = &strictPipeline;
    strictMipCompute.computeJob.uavTextures[0].mip = 1u;
    if (backend.scheduleJob(&backend, &strictMipCompute) != APUSR_FRAMEGEN_OK ||
        backend.executeJobs(&backend, commandBuffer, effect) != APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT ||
        g.dispatches != 2u)
        return 373;

    // The production profiler records three consecutive generations and then
    // backs off for roughly one second. Exercise that behavior directly: two
    // more immediately adjacent generations must complete the initial burst,
    // while the fourth immediate generation must not produce a sample.
    for (std::uint64_t frameId = 78u; frameId <= 80u; ++frameId) {
        if (backend.scheduleJob(&backend, &compute) != APUSR_FRAMEGEN_OK) return 3701;
        ApusrFramegenVulkanGpuProfileFrameInfo adjacentProfileInfo = profileInfo;
        adjacentProfileInfo.frameId = frameId;
        apusrFramegenBeginVulkanGpuProfile(&backend, commandBuffer, adjacentProfileInfo);
        if (backend.executeJobs(&backend, commandBuffer, effect) != APUSR_FRAMEGEN_OK) return 3702;
        apusrFramegenEndVulkanGpuProfile(&backend, commandBuffer);
        apusrFramegenFlushVulkanGpuProfile(&backend);
    }
    {
        std::ifstream profileFile(profilePath);
        const std::string csv((std::istreambuf_iterator<char>(profileFile)), std::istreambuf_iterator<char>());
        const auto countOccurrences = [&](const std::string_view needle) {
            std::size_t count = 0;
            std::size_t offset = 0;
            while ((offset = csv.find(needle, offset)) != std::string::npos) {
                ++count;
                offset += needle.size();
            }
            return count;
        };
        if (csv.find("row_type,name,effect,pass_id,profile_level") == std::string::npos ||
            csv.find("dispatch,contract pipeline,1,0,3,") == std::string::npos ||
            csv.find("cpu_record_us") == std::string::npos ||
            csv.find("77,64,32,128,64,4,2,120,60,16.667,0.250,-0.125,128.000,64.000,18,52,1,1,1,1,0,frame,__frame__") == std::string::npos ||
            countOccurrences("frame,__frame__") != 3u ||
            csv.find("0,0,0,0,77,") == std::string::npos ||
            csv.find("1,0,0,1,78,") == std::string::npos ||
            csv.find("2,0,0,2,79,") == std::string::npos ||
            csv.find(",80,64,32,128,64,") != std::string::npos)
            return 371;
    }
    if (!hasBinding(g.writeBindings, 0) || !hasBinding(g.writeBindings, 32) || !hasBinding(g.writeBindings, 64) ||
        g.descriptorPoolResets != 6) return 38;
    if (g.copyBufferToImage != 1 || g.clearImages != 1 || g.copyImages != 1 || g.fillBuffers != 1 || g.copyBuffers != 1) return 39;
    if (std::find(g.imageTransitions.begin(), g.imageTransitions.end(),
                  std::pair{vk::ImageLayoutUndefined, vk::ImageLayoutGeneral}) == g.imageTransitions.end()) return 40;
    const auto borrowedTransition = std::find(
        g.imageTransitions.begin(), g.imageTransitions.end(),
        std::pair{vk::ImageLayoutDepthStencilReadOnlyOptimal, vk::ImageLayoutShaderReadOnlyOptimal});
    if (borrowedTransition == g.imageTransitions.end()) return 41;
    const std::size_t borrowedTransitionIndex = static_cast<std::size_t>(borrowedTransition - g.imageTransitions.begin());
    if (borrowedTransitionIndex >= g.imageTransitionRanges.size()) return 411;
    const vk::ImageSubresourceRange borrowedRange = g.imageTransitionRanges[borrowedTransitionIndex];
    if (borrowedRange.aspectMask != vk::ImageAspectDepthBit || borrowedRange.baseMipLevel != 2 ||
        borrowedRange.levelCount != 1 || borrowedRange.baseArrayLayer != 3 || borrowedRange.layerCount != 1) return 412;

    if (backend.unregisterResources(&backend, commandBuffer, effect) != APUSR_FRAMEGEN_OK) return 42;
    if (g.imageTransitions.back() != std::pair{vk::ImageLayoutShaderReadOnlyOptimal, vk::ImageLayoutDepthStencilReadOnlyOptimal}) return 43;
    if (g.borrowedImageViewDestroyed) return 431;

    if (backend.destroyPipeline(&backend, &pipeline, effect) != APUSR_FRAMEGEN_OK) return 44;
    if (backend.destroyResource(&backend, initialized, effect) != APUSR_FRAMEGEN_OK ||
        backend.destroyResource(&backend, uav, effect) != APUSR_FRAMEGEN_OK ||
        backend.destroyResource(&backend, copyTarget, effect) != APUSR_FRAMEGEN_OK ||
        backend.destroyResource(&backend, bufferA, effect) != APUSR_FRAMEGEN_OK ||
        backend.destroyResource(&backend, bufferB, effect) != APUSR_FRAMEGEN_OK) return 45;
    if (backend.destroyContext(&backend, effect) != APUSR_FRAMEGEN_OK) return 46;
    std::free(backend.scratchBuffer);

    // Exercise the Streamline-shaped Vulkan adapter and native FI/OF scheduler
    // together. The backbuffer contract deliberately carries only an extent,
    // matching the DLSS-G pattern where the live swapchain image arrives at
    // present time rather than in the tag itself.
    FakeImage sessionDepth{128, 128, 1};
    FakeImage sessionMotionVectors{128, 128, 1};
    FakeImage sessionHudless{128, 128, 1};
    FakeImage sessionDistortion{128, 128, 1};
    std::array<FakeImage, 3> swapchainImages{{
        {128, 128, 1}, {128, 128, 1}, {128, 128, 1},
    }};
    std::array<void*, 3> swapchainImageHandles{{
        &swapchainImages[0], &swapchainImages[1], &swapchainImages[2],
    }};

    ApusrFramegenStreamlineVulkanInputs streamlineInputs{};
    streamlineInputs.deviceContext = device;
    streamlineInputs.frameId = 1;
    streamlineInputs.frameTimeDelta = 16.6667f;
    streamlineInputs.minMaxLuminance[1] = 1.0f;
    streamlineInputs.backbufferContract.tagged = true;
    streamlineInputs.backbufferContract.extentWidth = 128;
    streamlineInputs.backbufferContract.extentHeight = 128;

    streamlineInputs.depth.tagged = true;
    streamlineInputs.depth.image = &sessionDepth;
    streamlineInputs.depth.imageView = handle<void*>(0xB101);
    streamlineInputs.depth.nativeFormat = static_cast<std::uint32_t>(vk::FormatD32Sfloat);
    streamlineInputs.depth.nativeLayout = static_cast<std::uint32_t>(vk::ImageLayoutDepthStencilReadOnlyOptimal);
    streamlineInputs.depth.width = 128;
    streamlineInputs.depth.height = 128;
    streamlineInputs.depth.aspectMask = vk::ImageAspectDepthBit;
    streamlineInputs.depth.levelCount = 1;
    streamlineInputs.depth.layerCount = 1;
    streamlineInputs.depth.extentWidth = 128;
    streamlineInputs.depth.extentHeight = 128;

    streamlineInputs.motionVectors.tagged = true;
    streamlineInputs.motionVectors.image = &sessionMotionVectors;
    streamlineInputs.motionVectors.imageView = handle<void*>(0xB102);
    streamlineInputs.motionVectors.nativeFormat = static_cast<std::uint32_t>(vk::FormatR16G16Sfloat);
    streamlineInputs.motionVectors.nativeLayout = static_cast<std::uint32_t>(vk::ImageLayoutShaderReadOnlyOptimal);
    streamlineInputs.motionVectors.width = 128;
    streamlineInputs.motionVectors.height = 128;
    streamlineInputs.motionVectors.aspectMask = vk::ImageAspectColorBit;
    streamlineInputs.motionVectors.levelCount = 1;
    streamlineInputs.motionVectors.layerCount = 1;
    streamlineInputs.motionVectors.extentWidth = 128;
    streamlineInputs.motionVectors.extentHeight = 128;

    streamlineInputs.hudlessColor.tagged = true;
    streamlineInputs.hudlessColor.image = &sessionHudless;
    streamlineInputs.hudlessColor.imageView = handle<void*>(0xB103);
    streamlineInputs.hudlessColor.nativeFormat = static_cast<std::uint32_t>(vk::FormatR8G8B8A8Unorm);
    streamlineInputs.hudlessColor.nativeLayout = static_cast<std::uint32_t>(vk::ImageLayoutShaderReadOnlyOptimal);
    streamlineInputs.hudlessColor.nativeUsage = vk::ImageUsageTransferSrcBit | vk::ImageUsageSampledBit;
    streamlineInputs.hudlessColor.width = 128;
    streamlineInputs.hudlessColor.height = 128;
    streamlineInputs.hudlessColor.levelCount = 1;
    streamlineInputs.hudlessColor.layerCount = 1;
    streamlineInputs.hudlessColor.extentWidth = 128;
    streamlineInputs.hudlessColor.extentHeight = 128;

    streamlineInputs.distortionField.tagged = true;
    streamlineInputs.distortionField.image = &sessionDistortion;
    streamlineInputs.distortionField.imageView = handle<void*>(0xB104);
    streamlineInputs.distortionField.nativeFormat = static_cast<std::uint32_t>(vk::FormatR8G8Unorm);
    streamlineInputs.distortionField.nativeLayout = static_cast<std::uint32_t>(vk::ImageLayoutShaderReadOnlyOptimal);
    streamlineInputs.distortionField.nativeUsage = vk::ImageUsageTransferSrcBit | vk::ImageUsageSampledBit;
    streamlineInputs.distortionField.width = 128;
    streamlineInputs.distortionField.height = 128;
    streamlineInputs.distortionField.levelCount = 1;
    streamlineInputs.distortionField.layerCount = 1;
    streamlineInputs.distortionField.extentWidth = 128;
    streamlineInputs.distortionField.extentHeight = 128;

    // From this point onward the contract intentionally covers only the
    // clean-sheet proxy architecture. Historical direct-WSI/staged bring-up
    // paths are not product fallbacks and do not constrain the final design.

    // Clean-sheet proxy contract: the application renders into APUSR-owned
    // virtual backbuffers that remain the immutable source until both physical
    // replay and generation finish. Phase B samples that source directly and
    // never touches real WSI images.
    std::array<FakeImage, 3> virtualBackbuffers{{
        {128, 128, 1}, {128, 128, 1}, {128, 128, 1},
    }};
    std::array<FakeImage, 3> privateWsiImages{{
        {128, 128, 1}, {128, 128, 1}, {128, 128, 1},
    }};
    std::array<void*, 3> virtualBackbufferHandles{{
        &virtualBackbuffers[0], &virtualBackbuffers[1], &virtualBackbuffers[2],
    }};
    std::array<void*, 3> privateWsiHandles{{
        &privateWsiImages[0], &privateWsiImages[1], &privateWsiImages[2],
    }};
    ApusrFramegenVulkanPresentImageDescription offscreenImageDescription{};
    offscreenImageDescription.deviceContext = device;
    offscreenImageDescription.width = 128u;
    offscreenImageDescription.height = 128u;
    offscreenImageDescription.nativeFormat = static_cast<std::uint32_t>(vk::FormatR8G8B8A8Unorm);
    // The virtual backbuffer is the retained source frame: presentation reads
    // it as TRANSFER_SRC while Phase B samples it directly. It never needs to
    // be a generated-output target.
    offscreenImageDescription.imageUsage = vk::ImageUsageTransferSrcBit | vk::ImageUsageSampledBit | 0x10u;
    offscreenImageDescription.internalQueueFamilyIndices = {2u, 3u, 0u};
    offscreenImageDescription.internalQueueFamilyIndexCount = 2u;
    offscreenImageDescription.imageCount = static_cast<std::uint32_t>(virtualBackbufferHandles.size());
    offscreenImageDescription.images = virtualBackbufferHandles.data();
    offscreenImageDescription.presentationImageCount = static_cast<std::uint32_t>(privateWsiHandles.size());
    offscreenImageDescription.presentationImages = privateWsiHandles.data();
    offscreenImageDescription.presentationImageUsage =
        vk::ImageUsageTransferSrcBit | vk::ImageUsageTransferDstBit | vk::ImageUsageStorageBit;
    offscreenImageDescription.offscreenProxy = true;
    ApusrFramegenVulkanPresentImageContext* offscreenPresentImages = nullptr;
    if (apusrFramegenCreateVulkanPresentImageContext(
            offscreenImageDescription, {}, &offscreenPresentImages) != APUSR_FRAMEGEN_OK || !offscreenPresentImages) return 520;

    ApusrFramegenVulkanPresentSubmitDescription offscreenSubmitDescription{};
    offscreenSubmitDescription.deviceContext = device;
    offscreenSubmitDescription.inputQueue = handle<void*>(0xD101);
    offscreenSubmitDescription.inputQueueFamilyIndex = 2u;
    offscreenSubmitDescription.generationQueue = handle<void*>(0xD102);
    offscreenSubmitDescription.generationQueueFamilyIndex = 3u;
    offscreenSubmitDescription.slotCount = 3u;
    ApusrFramegenVulkanPresentSubmitContext* offscreenSubmit = nullptr;
    if (apusrFramegenCreateVulkanPresentSubmitContext(
            offscreenSubmitDescription, {}, &offscreenSubmit) != APUSR_FRAMEGEN_OK || !offscreenSubmit) return 521;

    void* proxyInputCommand = nullptr;
    void* proxyGenerationCommand = nullptr;
    if (apusrFramegenBeginVulkanSplitPresentSubmit(
            offscreenSubmit, 0u, 0u, &proxyInputCommand, &proxyGenerationCommand) != APUSR_FRAMEGEN_OK ||
        !proxyInputCommand || !proxyGenerationCommand || proxyInputCommand == proxyGenerationCommand) return 522;
    const std::size_t proxyTransitionBegin = g.imageTransitionRecords.size();
    const std::uint32_t proxyCopiesBegin = g.copyImages;
    ApusrFramegenVulkanPresentFrameResources proxyResources{};
    if (apusrFramegenPrepareVulkanOffscreenGenerationImages(
            offscreenPresentImages, proxyInputCommand, proxyGenerationCommand, 0u, 0u,
            APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE, &proxyResources) != APUSR_FRAMEGEN_OK ||
        !proxyResources.presentColor.image || !proxyResources.generatedOutput.image ||
        proxyResources.presentColor.image != virtualBackbufferHandles[0] ||
        proxyResources.generatedOutput.image == virtualBackbufferHandles[0] ||
        proxyResources.generatedOutput.image == privateWsiHandles[0] ||
        !proxyResources.sampledSwapchainDirectly || proxyResources.storageSwapchainDirectly ||
        g.copyImages != proxyCopiesBegin) return 523;
    ApusrFramegenVulkanPresentFrameResources secondProxyOutput{};
    if (apusrFramegenPrepareVulkanOffscreenGenerationImages(
            offscreenPresentImages, proxyInputCommand, proxyGenerationCommand, 0u, 1u,
            APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE, &secondProxyOutput) != APUSR_FRAMEGEN_OK ||
        !secondProxyOutput.generatedOutput.image ||
        secondProxyOutput.generatedOutput.image == proxyResources.generatedOutput.image ||
        secondProxyOutput.presentColor.image != proxyResources.presentColor.image ||
        g.copyImages != proxyCopiesBegin) return 5231;

    // Preferred path: an opportunistically acquired real WSI image is the FI
    // storage target itself. No generated-output intermediate or full-screen
    // replay copy belongs on this path.
    if (!apusrFramegenVulkanDirectWsiStorageSupported(offscreenPresentImages)) return 5232;
    const std::size_t directTransitionBegin = g.imageTransitionRecords.size();
    const std::uint32_t directCopiesBegin = g.copyImages;
    ApusrFramegenVulkanPresentFrameResources directWsiResources{};
    if (apusrFramegenPrepareVulkanOffscreenGenerationImages(
            offscreenPresentImages, proxyInputCommand, proxyGenerationCommand, 0u, 0u, 1u,
            &directWsiResources) != APUSR_FRAMEGEN_OK ||
        directWsiResources.presentColor.image != virtualBackbufferHandles[0] ||
        directWsiResources.generatedOutput.image != privateWsiHandles[1] ||
        !directWsiResources.generatedOutput.imageView ||
        !directWsiResources.sampledSwapchainDirectly || !directWsiResources.storageSwapchainDirectly ||
        g.copyImages != directCopiesBegin ||
        apusrFramegenFinishVulkanOffscreenGenerationImages(
            offscreenPresentImages, proxyGenerationCommand, 1u) != APUSR_FRAMEGEN_OK) return 5233;
    const std::uintptr_t directWsiImage = reinterpret_cast<std::uintptr_t>(privateWsiHandles[1]);
    const bool directEnteredStorage = std::any_of(
        g.imageTransitionRecords.begin() + static_cast<std::ptrdiff_t>(directTransitionBegin),
        g.imageTransitionRecords.end(), [&](const Probe::ImageTransitionRecord& record) {
            return record.image == directWsiImage && record.oldLayout == vk::ImageLayoutUndefined &&
                record.newLayout == vk::ImageLayoutGeneral;
        });
    const bool directEnteredPresent = std::any_of(
        g.imageTransitionRecords.begin() + static_cast<std::ptrdiff_t>(directTransitionBegin),
        g.imageTransitionRecords.end(), [&](const Probe::ImageTransitionRecord& record) {
            return record.image == directWsiImage && record.oldLayout == vk::ImageLayoutGeneral &&
                record.newLayout == vk::ImageLayoutPresentSrcKhr;
        });
    if (!directEnteredStorage || !directEnteredPresent) return 5234;

    const std::uintptr_t proxyVirtualImage = reinterpret_cast<std::uintptr_t>(virtualBackbufferHandles[0]);
    const bool proxyVirtualTransitionedByPreparation = std::any_of(
        g.imageTransitionRecords.begin() + static_cast<std::ptrdiff_t>(proxyTransitionBegin),
        g.imageTransitionRecords.end(),
        [&](const Probe::ImageTransitionRecord& record) {
            return record.image == proxyVirtualImage;
        });
    if (proxyVirtualTransitionedByPreparation) return 524;

    auto* proxyGeneratedReuse = new FakeSemaphore{true, 7u};
    void* proxyAppWait = handle<void*>(0xD110);
    std::uint64_t proxyProducerReady = 0u;
    std::uint64_t proxyInputCompletion = 0u;
    std::uint64_t proxyGenerationCompletion = 0u;
    const std::uint32_t proxySubmitsBefore = g.queueSubmits;
    if (apusrFramegenSubmitVulkanSplitOffscreenPresentWork(
            offscreenSubmit, 0u, &proxyAppWait, 1u,
            proxyGeneratedReuse, 7u, 0u, nullptr,
            &proxyProducerReady, &proxyInputCompletion, &proxyGenerationCompletion) != APUSR_FRAMEGEN_OK ||
        proxyProducerReady != 1u || proxyInputCompletion != 1u || proxyGenerationCompletion != 1u ||
        g.queueSubmits != proxySubmitsBefore + 2u ||
        g.queueSubmitInfoCounts.size() < 2u ||
        g.queueSubmitInfoCounts[g.queueSubmitInfoCounts.size() - 2u] != 2u ||
        g.queueSubmitInfoCounts.back() != 1u ||
        g.submitWaitBatchHistory.size() < 2u || g.submitSignalBatchHistory.size() < 2u ||
        g.submitWaitValueBatchHistory.size() < 2u || g.submitSignalValueBatchHistory.size() < 2u ||
        g.submitWaitBatchHistory[g.submitWaitBatchHistory.size() - 2u].size() != 2u ||
        g.submitSignalBatchHistory[g.submitSignalBatchHistory.size() - 2u].size() != 2u ||
        g.submitWaitBatchHistory[g.submitWaitBatchHistory.size() - 2u][0] !=
            std::vector<std::uintptr_t>({reinterpret_cast<std::uintptr_t>(proxyAppWait)}) ||
        g.submitWaitValueBatchHistory[g.submitWaitValueBatchHistory.size() - 2u][0] !=
            std::vector<std::uint64_t>({0u}) ||
        g.submitSignalBatchHistory[g.submitSignalBatchHistory.size() - 2u][0] !=
            std::vector<std::uintptr_t>({reinterpret_cast<std::uintptr_t>(apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit))}) ||
        g.submitSignalValueBatchHistory[g.submitSignalValueBatchHistory.size() - 2u][0] !=
            std::vector<std::uint64_t>({proxyProducerReady}) ||
        g.submitWaitBatchHistory[g.submitWaitBatchHistory.size() - 2u][1] !=
            std::vector<std::uintptr_t>({reinterpret_cast<std::uintptr_t>(apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit))}) ||
        g.submitWaitValueBatchHistory[g.submitWaitValueBatchHistory.size() - 2u][1] !=
            std::vector<std::uint64_t>({proxyProducerReady}) ||
        !apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit) ||
        !apusrFramegenVulkanInputCompletionSemaphore(offscreenSubmit) ||
        !apusrFramegenVulkanGenerationCompletionSemaphore(offscreenSubmit)) return 525;
    delete proxyGeneratedReuse;

    // If the preceding FI dispatch is still running on the separate generation
    // queue, do not drop the next source frame. Its Phase A submission must be
    // queued behind the prior generation-completion value while producerReady
    // remains independently available to source replay. This is the regression
    // for the old skip -> history-gap -> forced-warmup two-frame loss cascade.
    void* chainedInputCommand = nullptr;
    void* chainedGenerationCommand = nullptr;
    if (apusrFramegenBeginVulkanSplitPresentSubmit(
            offscreenSubmit, 1u, 0u, &chainedInputCommand, &chainedGenerationCommand) != APUSR_FRAMEGEN_OK ||
        !chainedInputCommand || !chainedGenerationCommand) return 52501;
    void* chainedAppWait = handle<void*>(0xD111);
    std::uint64_t chainedProducerReady = 0u;
    std::uint64_t chainedInputCompletion = 0u;
    std::uint64_t chainedGenerationCompletion = 0u;
    const std::uint32_t chainedSubmitsBefore = g.queueSubmits;
    if (apusrFramegenSubmitVulkanSplitOffscreenPresentWork(
            offscreenSubmit, 1u, &chainedAppWait, 1u, nullptr, 0u, proxyGenerationCompletion, nullptr,
            &chainedProducerReady, &chainedInputCompletion, &chainedGenerationCompletion) != APUSR_FRAMEGEN_OK ||
        chainedProducerReady != 2u || chainedInputCompletion != 2u || chainedGenerationCompletion != 2u ||
        g.queueSubmits != chainedSubmitsBefore + 2u ||
        g.submitWaitBatchHistory.size() < 2u || g.submitWaitValueBatchHistory.size() < 2u ||
        g.submitWaitBatchHistory[g.submitWaitBatchHistory.size() - 2u].size() != 2u ||
        g.submitWaitValueBatchHistory[g.submitWaitValueBatchHistory.size() - 2u].size() != 2u ||
        g.submitWaitBatchHistory[g.submitWaitBatchHistory.size() - 2u][0] !=
            std::vector<std::uintptr_t>({reinterpret_cast<std::uintptr_t>(chainedAppWait)}) ||
        g.submitWaitValueBatchHistory[g.submitWaitValueBatchHistory.size() - 2u][0] !=
            std::vector<std::uint64_t>({0u}) ||
        g.submitWaitBatchHistory[g.submitWaitBatchHistory.size() - 2u][1] !=
            std::vector<std::uintptr_t>({
                reinterpret_cast<std::uintptr_t>(apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit)),
                reinterpret_cast<std::uintptr_t>(apusrFramegenVulkanGenerationCompletionSemaphore(offscreenSubmit))}) ||
        g.submitWaitValueBatchHistory[g.submitWaitValueBatchHistory.size() - 2u][1] !=
            std::vector<std::uint64_t>({chainedProducerReady, proxyGenerationCompletion})) return 52502;

    apusrFramegenCommitVulkanPresentImageInitialization(offscreenPresentImages);

    // A common single-family topology has no compute-only queue. Phase A and Phase B
    // therefore share one APUSR-private graphics queue. Vulkan queue order is
    // the A->B dependency; do not manufacture a timeline wait that stalls the
    // same queue on a value it just signaled.
    ApusrFramegenVulkanPresentSubmitDescription sameQueueSubmitDescription{};
    sameQueueSubmitDescription.deviceContext = device;
    sameQueueSubmitDescription.inputQueue = handle<void*>(0xD201);
    sameQueueSubmitDescription.inputQueueFamilyIndex = 2u;
    sameQueueSubmitDescription.generationQueue = sameQueueSubmitDescription.inputQueue;
    sameQueueSubmitDescription.generationQueueFamilyIndex = 2u;
    sameQueueSubmitDescription.slotCount = 1u;
    ApusrFramegenVulkanPresentSubmitContext* sameQueueSubmit = nullptr;
    if (apusrFramegenCreateVulkanPresentSubmitContext(
            sameQueueSubmitDescription, {}, &sameQueueSubmit) != APUSR_FRAMEGEN_OK || !sameQueueSubmit) return 5251;
    void* sameQueueInput = nullptr;
    void* sameQueueGeneration = nullptr;
    if (apusrFramegenBeginVulkanSplitPresentSubmit(
            sameQueueSubmit, 0u, 0u, &sameQueueInput, &sameQueueGeneration) != APUSR_FRAMEGEN_OK) return 5252;
    void* sameQueueAppWait = handle<void*>(0xD211);
    std::uint64_t sameQueueProducerValue = 0u;
    std::uint64_t sameQueueInputValue = 0u;
    std::uint64_t sameQueueGenerationValue = 0u;
    const std::uint32_t sameQueueSubmitsBefore = g.queueSubmits;
    if (apusrFramegenSubmitVulkanSplitOffscreenPresentWork(
            sameQueueSubmit, 0u, &sameQueueAppWait, 1u, nullptr, 0u, 0u, nullptr,
            &sameQueueProducerValue, &sameQueueInputValue, &sameQueueGenerationValue) != APUSR_FRAMEGEN_OK ||
        sameQueueProducerValue != 1u || sameQueueInputValue != 1u || sameQueueGenerationValue != 1u ||
        g.queueSubmits != sameQueueSubmitsBefore + 1u ||
        g.queueSubmitInfoCounts.empty() || g.queueSubmitInfoCounts.back() != 3u ||
        g.submitWaitBatches.size() != 3u || g.submitWaitValueBatches.size() != 3u ||
        g.submitWaitBatches[0] != std::vector<std::uintptr_t>({reinterpret_cast<std::uintptr_t>(sameQueueAppWait)}) ||
        g.submitWaitValueBatches[0] != std::vector<std::uint64_t>({0u}) ||
        g.submitWaitBatches[1] != std::vector<std::uintptr_t>({reinterpret_cast<std::uintptr_t>(apusrFramegenVulkanProducerReadySemaphore(sameQueueSubmit))}) ||
        g.submitWaitValueBatches[1] != std::vector<std::uint64_t>({sameQueueProducerValue}) ||
        !g.submitWaitBatches[2].empty() || !g.submitWaitValueBatches[2].empty()) return 5253;
    const std::uint64_t expectedRecoveredProducer = apusrFramegenVulkanNextProducerReadyValue(sameQueueSubmit);
    void* recoveryAppWait = handle<void*>(0xD212);
    std::uint64_t recoveredProducer = 0u;
    const std::uint32_t recoverySubmitsBefore = g.queueSubmits;
    if (expectedRecoveredProducer != 2u || apusrFramegenSignalVulkanProducerReadyOnly(
            sameQueueSubmit, &recoveryAppWait, 1u, &recoveredProducer) != APUSR_FRAMEGEN_OK ||
        recoveredProducer != expectedRecoveredProducer || g.queueSubmits != recoverySubmitsBefore + 1u ||
        g.submitWaitBatches.size() != 1u ||
        g.submitWaitBatches[0] != std::vector<std::uintptr_t>({reinterpret_cast<std::uintptr_t>(recoveryAppWait)}) ||
        g.submitSignalBatches[0] != std::vector<std::uintptr_t>({
            reinterpret_cast<std::uintptr_t>(apusrFramegenVulkanProducerReadySemaphore(sameQueueSubmit))}) ||
        g.submitSignalValueBatches[0] != std::vector<std::uint64_t>({recoveredProducer})) return 5254;
    apusrFramegenDestroyVulkanPresentSubmitContext(sameQueueSubmit, {});

    // Direct generated WSI ownership is fully fail-open: acquire never waits,
    // presentation bridges only an already-ready generation timeline to the
    // binary WSI semaphore, and missed deadlines release rather than present.
    g_releasedReplayImages.clear();
    ApusrFramegenVulkanDirectWsiContext* directWsiContext = nullptr;
    const ApusrFramegenVulkanDirectWsiDescription directWsiDescription{
        .deviceContext = device,
        .bridgeQueue = handle<void*>(0xD121),
        .abortQueue = handle<void*>(0xD122),
        .device = device.device,
        .swapchain = handle<void*>(0xD130),
        .presentImageCount = 3u,
        .userContext = nullptr,
        .acquireImage = &fakeAcquireReplayImage,
        .releaseImage = &fakeReleaseReplayImage,
    };
    if (apusrFramegenCreateVulkanDirectWsiContext(
            directWsiDescription, {}, &directWsiContext) != APUSR_FRAMEGEN_OK || !directWsiContext) return 5255;
    std::uint32_t directImageIndex = APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE;
    void* directAcquireSemaphore = nullptr;
    bool directAcquired = false;
    if (apusrFramegenTryAcquireVulkanDirectWsiTarget(
            directWsiContext, &directImageIndex, &directAcquireSemaphore, &directAcquired) != APUSR_FRAMEGEN_OK ||
        !directAcquired || directImageIndex != 2u || !directAcquireSemaphore || g_lastReplayAcquireTimeout != 0u) return 5256;
    FakeSemaphore directGenerationTimeline{true, 5u};
    void* directPresentWait = nullptr;
    if (apusrFramegenPrepareVulkanDirectWsiPresent(
            directWsiContext, directImageIndex, &directGenerationTimeline, 5u, &directPresentWait) != APUSR_FRAMEGEN_OK ||
        !directPresentWait || g.submitWaitValueBatches.size() != 1u ||
        g.submitWaitValueBatches[0] != std::vector<std::uint64_t>({5u})) return 5257;
    apusrFramegenMarkVulkanDirectWsiPresented(directWsiContext, directImageIndex);

    // A late image remains privately acquired until FI/OF has stopped writing
    // it, then maintenance1 returns it to WSI without disturbing source order.
    if (apusrFramegenTryAcquireVulkanDirectWsiTarget(
            directWsiContext, &directImageIndex, &directAcquireSemaphore, &directAcquired) != APUSR_FRAMEGEN_OK ||
        !directAcquired || directImageIndex != 2u) return 5258;
    directGenerationTimeline.value = 5u;
    apusrFramegenDeferVulkanDirectWsiRelease(directWsiContext, directImageIndex, &directGenerationTimeline, 6u);
    if (!g_releasedReplayImages.empty()) return 5259;
    directGenerationTimeline.value = 6u;
    apusrFramegenReclaimCompletedVulkanDirectWsiTarget(directWsiContext);
    if (g_releasedReplayImages != std::vector<std::uint32_t>({2u})) return 5260;

    // A CPU-side abort after WSI acquire consumes the acquire binary on the
    // private bridge queue before releasing the image; it never leaks into a
    // later acquire.
    g_releasedReplayImages.clear();
    if (apusrFramegenTryAcquireVulkanDirectWsiTarget(
            directWsiContext, &directImageIndex, &directAcquireSemaphore, &directAcquired) != APUSR_FRAMEGEN_OK ||
        !directAcquired || directImageIndex != 2u) return 5261;
    const std::uint32_t waitsBeforeDirectAbort = g.queueWaitIdles;
    apusrFramegenAbortVulkanDirectWsiAcquire(directWsiContext, directImageIndex);
    if (g_releasedReplayImages != std::vector<std::uint32_t>({2u}) ||
        g.queueWaitIdles != waitsBeforeDirectAbort + 1u) return 5262;
    apusrFramegenDestroyVulkanDirectWsiContext(directWsiContext, {});

    // Generated and source replay each acquire a private real WSI image. Their
    // consumed timelines are the lifetime boundary for the generated output
    // and retained virtual source respectively.
    ApusrFramegenVulkanSourceReplayContext* generatedReplayContext = nullptr;
    const ApusrFramegenVulkanSourceReplayDescription generatedReplayDescription{
        .deviceContext = device,
        .queue = handle<void*>(0xD120),
        .queueFamilyIndex = 2u,
        .device = device.device,
        .swapchain = handle<void*>(0xD130),
        .presentImageCount = 3u,
        .sourceSlotCount = APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS,
        .presentImages = offscreenPresentImages,
        .content = APUSR_FRAMEGEN_VULKAN_REPLAY_GENERATED,
        .userContext = nullptr,
        .acquireImage = &fakeAcquireReplayImage,
    };
    if (apusrFramegenCreateVulkanSourceReplayContext(
            generatedReplayDescription, {}, &generatedReplayContext) != APUSR_FRAMEGEN_OK || !generatedReplayContext) return 526;
    const std::uint64_t generatedConsumed = apusrFramegenReserveVulkanReplayCompletion(generatedReplayContext);
    std::uint32_t proxyReplayTarget = 0u;
    void* proxyReplayReady = nullptr;
    bool proxyReplayCopied = false;
    const std::uint32_t copiesBeforeGeneratedReplay = g.copyImages;
    if (generatedConsumed != 1u || apusrFramegenPrepareVulkanReplay(
            generatedReplayContext, 0u, generatedConsumed,
            nullptr, 0u, apusrFramegenVulkanGenerationCompletionSemaphore(offscreenSubmit), proxyGenerationCompletion,
            &proxyReplayTarget, &proxyReplayReady, &proxyReplayCopied) != APUSR_FRAMEGEN_OK ||
        proxyReplayTarget != 2u || !proxyReplayReady || !proxyReplayCopied ||
        g_lastReplayAcquireTimeout != 0u ||
        g.copyImages != copiesBeforeGeneratedReplay + 1u ||
        apusrFramegenVulkanCompletedReplayValue(generatedReplayContext) != generatedConsumed) return 527;

    // Generated replay is fail-open: if private WSI has no image immediately
    // available, drop only that generated frame, advance its output-slot
    // lifetime, and never block the mandatory source presentation.
    const std::uint64_t generatedMissConsumed = apusrFramegenReserveVulkanReplayCompletion(generatedReplayContext);
    g_nextReplayAcquireResult = 1; // VK_NOT_READY
    proxyReplayReady = reinterpret_cast<void*>(0x1);
    proxyReplayCopied = true;
    const std::uint32_t copiesBeforeGeneratedMiss = g.copyImages;
    if (apusrFramegenPrepareVulkanReplay(
            generatedReplayContext, 1u, generatedMissConsumed,
            nullptr, 0u, apusrFramegenVulkanGenerationCompletionSemaphore(offscreenSubmit), proxyGenerationCompletion,
            &proxyReplayTarget, &proxyReplayReady, &proxyReplayCopied) != APUSR_FRAMEGEN_OK ||
        proxyReplayReady != nullptr || proxyReplayCopied || g_lastReplayAcquireTimeout != 0u ||
        g.copyImages != copiesBeforeGeneratedMiss ||
        apusrFramegenVulkanCompletedReplayValue(generatedReplayContext) != generatedMissConsumed) return 5271;

    // Mandatory source replay can be prearmed on the application thread
    // against a future producer-ready timeline value. Both command-buffer
    // pressure and WSI pressure are clean misses; a post-acquire recording
    // failure is maintenance-released and the same completion reservation is
    // still usable by the normal worker path.
    ApusrFramegenVulkanSourceReplayContext* prearmSourceReplayContext = nullptr;
    const ApusrFramegenVulkanSourceReplayDescription prearmSourceReplayDescription{
        .deviceContext = device,
        .queue = handle<void*>(0xD120),
        .queueFamilyIndex = 2u,
        .device = device.device,
        .swapchain = handle<void*>(0xD130),
        .presentImageCount = 3u,
        .sourceSlotCount = 3u,
        .presentImages = offscreenPresentImages,
        .content = APUSR_FRAMEGEN_VULKAN_REPLAY_SOURCE,
        .userContext = nullptr,
        .acquireImage = &fakeAcquireReplayImage,
        .releaseImage = &fakeReleaseReplayImage,
    };
    if (apusrFramegenCreateVulkanSourceReplayContext(
            prearmSourceReplayDescription, {}, &prearmSourceReplayContext) != APUSR_FRAMEGEN_OK ||
        !prearmSourceReplayContext) return 5272;
    const std::uint64_t prearmCompletion1 = apusrFramegenReserveVulkanReplayCompletion(prearmSourceReplayContext);
    std::uint32_t prearmImageIndex = 0u;
    void* prearmReadySemaphore = reinterpret_cast<void*>(0x1);
    bool prearmed = true;
    g_nextReplayAcquireResult = 1; // VK_NOT_READY
    if (prearmCompletion1 != 1u || apusrFramegenTryPrepareVulkanSourceReplay(
            prearmSourceReplayContext, 0u, prearmCompletion1,
            nullptr, 0u,
            apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit), proxyProducerReady,
            &prearmImageIndex, &prearmReadySemaphore, &prearmed) != APUSR_FRAMEGEN_OK ||
        prearmed || prearmReadySemaphore != nullptr || g_lastReplayAcquireTimeout != 0u ||
        apusrFramegenVulkanCompletedReplayValue(prearmSourceReplayContext) != 0u) return 5273;
    void* prearmProducerWaits[1]{handle<void*>(0xD071)};
    if (apusrFramegenTryPrepareVulkanSourceReplay(
            prearmSourceReplayContext, 0u, prearmCompletion1,
            prearmProducerWaits, 1u,
            apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit), proxyProducerReady,
            &prearmImageIndex, &prearmReadySemaphore, &prearmed) != APUSR_FRAMEGEN_OK ||
        !prearmed || prearmImageIndex != 2u || !prearmReadySemaphore ||
        std::find(g.submitWaitSemaphores.begin(), g.submitWaitSemaphores.end(),
            reinterpret_cast<std::uintptr_t>(prearmProducerWaits[0])) == g.submitWaitSemaphores.end() ||
        apusrFramegenVulkanCompletedReplayValue(prearmSourceReplayContext) != prearmCompletion1) return 5274;

    const std::uint64_t prearmCompletion2 = apusrFramegenReserveVulkanReplayCompletion(prearmSourceReplayContext);
    g.failNextEndCommandBuffer = true;
    g_releasedReplayImages.clear();
    prearmReadySemaphore = reinterpret_cast<void*>(0x1);
    prearmed = true;
    if (prearmCompletion2 != 2u || apusrFramegenTryPrepareVulkanSourceReplay(
            prearmSourceReplayContext, 1u, prearmCompletion2,
            nullptr, 0u,
            apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit), proxyProducerReady,
            &prearmImageIndex, &prearmReadySemaphore, &prearmed) != APUSR_FRAMEGEN_OK ||
        prearmed || prearmReadySemaphore != nullptr ||
        g_releasedReplayImages != std::vector<std::uint32_t>({2u}) ||
        apusrFramegenVulkanCompletedReplayValue(prearmSourceReplayContext) != prearmCompletion1) return 5275;
    if (apusrFramegenPrepareVulkanReplay(
            prearmSourceReplayContext, 1u, prearmCompletion2,
            nullptr, 0u, apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit), proxyProducerReady,
            &prearmImageIndex, &prearmReadySemaphore, &prearmed) != APUSR_FRAMEGEN_OK ||
        !prearmReadySemaphore || !prearmed ||
        apusrFramegenVulkanCompletedReplayValue(prearmSourceReplayContext) != prearmCompletion2) return 5276;
    apusrFramegenDestroyVulkanSourceReplayContext(prearmSourceReplayContext, {});

    ApusrFramegenVulkanSourceReplayContext* proxySourceReplayContext = nullptr;
    const ApusrFramegenVulkanSourceReplayDescription proxySourceReplayDescription{
        .deviceContext = device,
        .queue = handle<void*>(0xD120),
        .queueFamilyIndex = 2u,
        .device = device.device,
        .swapchain = handle<void*>(0xD130),
        .presentImageCount = 3u,
        .sourceSlotCount = 3u,
        .presentImages = offscreenPresentImages,
        .content = APUSR_FRAMEGEN_VULKAN_REPLAY_SOURCE,
        .userContext = nullptr,
        .acquireImage = &fakeAcquireReplayImage,
        .releaseImage = &fakeReleaseReplayImage,
    };
    if (apusrFramegenCreateVulkanSourceReplayContext(
            proxySourceReplayDescription, {}, &proxySourceReplayContext) != APUSR_FRAMEGEN_OK || !proxySourceReplayContext) return 528;
    const std::uint64_t sourceConsumed = apusrFramegenReserveVulkanReplayCompletion(proxySourceReplayContext);
    const std::uint32_t copiesBeforeSourceProxyReplay = g.copyImages;
    void* directProducerWait = handle<void*>(0xD111);
    if (sourceConsumed != 1u || apusrFramegenPrepareVulkanReplay(
            proxySourceReplayContext, 0u, sourceConsumed,
            &directProducerWait, 1u, nullptr, 0u,
            &proxyReplayTarget, &proxyReplayReady, &proxyReplayCopied) != APUSR_FRAMEGEN_OK ||
        proxyReplayTarget != 2u || !proxyReplayReady || !proxyReplayCopied ||
        g_lastReplayAcquireTimeout != ~std::uint64_t{0} ||
        g.copyImages != copiesBeforeSourceProxyReplay + 1u ||
        g.submitWaitBatches.size() != 1u || g.submitWaitValueBatches.size() != 1u ||
        g.submitWaitBatches[0].size() != 2u || g.submitWaitValueBatches[0].size() != 2u ||
        g.submitWaitBatches[0][1] != reinterpret_cast<std::uintptr_t>(directProducerWait) ||
        g.submitWaitValueBatches[0][0] != 0u || g.submitWaitValueBatches[0][1] != 0u ||
        apusrFramegenVulkanCompletedReplayValue(proxySourceReplayContext) != sourceConsumed) return 529;

    // Once real WSI acquire succeeds, a recording failure is maintenance-
    // released rather than presenting an undefined retirement frame. The
    // retained-source lifetime still advances because the blocking worker has
    // surfaced a backend error and will tear down this runtime.
    const std::uint64_t sourceRecovery = apusrFramegenReserveVulkanReplayCompletion(proxySourceReplayContext);
    const std::uint32_t copiesBeforeSourceRecovery = g.copyImages;
    g.failNextEndCommandBuffer = true;
    g_releasedReplayImages.clear();
    proxyReplayTarget = 0u;
    proxyReplayReady = reinterpret_cast<void*>(0x1);
    proxyReplayCopied = true;
    if (sourceRecovery != 2u || apusrFramegenPrepareVulkanReplay(
            proxySourceReplayContext, 0u, sourceRecovery,
            nullptr, 0u, apusrFramegenVulkanProducerReadySemaphore(offscreenSubmit), proxyProducerReady,
            &proxyReplayTarget, &proxyReplayReady, &proxyReplayCopied) != APUSR_FRAMEGEN_ERROR_BACKEND_API ||
        proxyReplayReady != nullptr || proxyReplayCopied ||
        g_releasedReplayImages != std::vector<std::uint32_t>({2u}) ||
        g.copyImages != copiesBeforeSourceRecovery + 1u ||
        apusrFramegenVulkanCompletedReplayValue(proxySourceReplayContext) != sourceRecovery) return 5291;

    apusrFramegenDestroyVulkanSourceReplayContext(proxySourceReplayContext, {});
    apusrFramegenDestroyVulkanSourceReplayContext(generatedReplayContext, {});
    apusrFramegenDestroyVulkanPresentSubmitContext(offscreenSubmit, {});
    apusrFramegenDestroyVulkanPresentImageContext(offscreenPresentImages, {});

    if (g.borrowedImageViewDestroyed ||
        g.concurrentImageCreates == 0u || g.concurrentBufferCreates == 0u) return 52;
    if (std::none_of(g.imageTransitions.begin(), g.imageTransitions.end(), [](const auto& transition) {
            return transition.second == vk::ImageLayoutPresentSrcKhr;
        })) return 53;

    ApusrFramegenVulkanPresentation* presentation = nullptr;
    const ApusrFramegenVulkanPresentationDescription presentationDescription{
        .userContext = nullptr,
        .presentImage = &fakePresentImage,
        .presentGeneratedRelative = &fakePresentGeneratedRelative,
        .presentSourceRelative = &fakePresentSourceRelative,
        .prepareGeneratedReplay = &fakePrepareGeneratedReplay,
        .prepareSourceReplay = &fakePrepareSourceReplay,
        .timelineReady = &fakeTimelineReady,
        .waitTimeline = &fakeTimelineWait,
        .generationDecision = &fakeGenerationDecision,
        .prepareDirectPresent = &fakePrepareDirectPresent,
        .directGenerationDecision = &fakeDirectGenerationDecision,
        // Keep this semantic WSI-timing check independent of host thread scheduling jitter.
        .initialSourceFrameTimeMs = 1000.0f,
    };
    if (apusrFramegenCreateVulkanPresentation(presentationDescription, {}, &presentation) != APUSR_FRAMEGEN_OK || !presentation) return 531;
    void* pacedSourceWaits[1]{handle<void*>(0xE001)};
    ApusrFramegenVulkanPresentationSchedule pacedSchedule{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 77u,
        .sourceImageIndex = 2u,
        .sourceWaitSemaphores = pacedSourceWaits,
        .sourceWaitSemaphoreCount = 1u,
        .hasGeneratedFrame = true,
        .paceGeneratedFrame = true,
        .generatedImageIndex = 1u,
        .generatedWaitSemaphore = handle<void*>(0xE002),
    };
    if (apusrFramegenScheduleVulkanPresentation(presentation, pacedSchedule) != APUSR_FRAMEGEN_OK) return 532;
    pacedSourceWaits[0] = handle<void*>(0xE0FF);
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_presentEvents.size() != 2u || !g_presentEvents[0].generated || g_presentEvents[0].imageIndex != 1u ||
        g_presentEvents[0].waits.size() != 1u || g_presentEvents[0].waits[0] != 0xE002u ||
        g_presentEvents[1].generated || g_presentEvents[1].imageIndex != 2u ||
        g_presentEvents[1].waits.size() != 1u || g_presentEvents[1].waits[0] != 0xE001u ||
        g_relativeTimingCalls != 1u || g_relativeTimingDurationNs != 500'000'000u) return 533;
    void* sourceOnlyWait = handle<void*>(0xE003);
    const ApusrFramegenVulkanPresentationSchedule sourceOnlySchedule{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 78u,
        .sourceImageIndex = 0u,
        .sourceWaitSemaphores = &sourceOnlyWait,
        .sourceWaitSemaphoreCount = 1u,
    };
    if (apusrFramegenScheduleVulkanPresentation(presentation, sourceOnlySchedule) != APUSR_FRAMEGEN_OK) return 534;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_presentEvents.size() != 3u || g_presentEvents[2].generated || g_presentEvents[2].imageIndex != 0u ||
        g_presentEvents[2].waits.size() != 1u || g_presentEvents[2].waits[0] != 0xE003u) return 535;

    g_presentationOrder.clear();
    const ApusrFramegenVulkanPresentationSchedule replaySchedule{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 79u,
        .sourceImageIndex = 2u,
        .sourceWaitSemaphores = nullptr,
        .sourceWaitSemaphoreCount = 0u,
        .hasGeneratedFrame = true,
        .paceGeneratedFrame = false,
        .generatedImageIndex = 2u,
        .generatedWaitSemaphore = handle<void*>(0xE005),
        .replaySourceFrame = true,
        .sourceReplaySlotIndex = 2u,
        .sourceReplayCompletionValue = 9u,
    };
    if (apusrFramegenScheduleVulkanPresentation(presentation, replaySchedule) != APUSR_FRAMEGEN_OK) return 536;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_presentationOrder != std::vector<int>({2, 1, 3}) || g_presentEvents.size() != 5u ||
        !g_presentEvents[3].generated || g_presentEvents[3].imageIndex != 2u ||
        g_presentEvents[3].waits != std::vector<std::uintptr_t>({0xE005u}) ||
        g_presentEvents[4].generated || g_presentEvents[4].imageIndex != 0u ||
        g_presentEvents[4].waits != std::vector<std::uintptr_t>({0xE004u})) return 537;

    // A proxy source-only frame consumes the application's render-finished
    // semaphore in the real-WSI replay submit. It must not require an empty
    // Phase-A bridge submission merely to manufacture an upstream timeline.
    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_sourceReplaySawProducerWait = false;
    void* sourceProducerWait = handle<void*>(0xE008);
    const ApusrFramegenVulkanPresentationSchedule directSourceReplaySchedule{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 791u,
        .sourceImageIndex = 2u,
        .sourceWaitSemaphores = &sourceProducerWait,
        .sourceWaitSemaphoreCount = 1u,
        .replaySourceFrame = true,
        .sourceReplaySlotIndex = 2u,
        .sourceReplayCompletionValue = 12u,
    };
    if (apusrFramegenScheduleVulkanPresentation(presentation, directSourceReplaySchedule) != APUSR_FRAMEGEN_OK) return 5371;
    sourceProducerWait = handle<void*>(0xE0FF);
    apusrFramegenDrainVulkanPresentation(presentation);
    if (!g_sourceReplaySawProducerWait || g_presentationOrder != std::vector<int>({2, 3}) ||
        g_presentEvents.size() != 1u || g_presentEvents[0].generated ||
        g_presentEvents[0].imageIndex != 0u ||
        g_presentEvents[0].waits != std::vector<std::uintptr_t>({0xE004u})) return 5372;

    // Preferred physical path: FI already wrote the generated pixels into an
    // acquired WSI image, so presentation needs only the tiny timeline->binary
    // bridge. The generated target is still fail-open if its timeline is late.
    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_generationReady = true;
    g_directDecisionValue = 0u;
    g_directDecisionImage = 0u;
    g_directDecisionPresented = false;
    g_directDecisionBridgePrepared = false;
    const ApusrFramegenVulkanPresentationSchedule directWsiPresentationSchedule{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 792u,
        .sourceImageIndex = 0u,
        .hasGeneratedFrame = true,
        .paceGeneratedFrame = false,
        .generatedImageIndex = 1u,
        .directGeneratedFrame = true,
        .generationCompletionSemaphore = handle<void*>(0xE006),
        .generationCompletionValue = 11u,
    };
    if (apusrFramegenScheduleVulkanPresentation(presentation, directWsiPresentationSchedule) != APUSR_FRAMEGEN_OK) return 5373;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_presentEvents.size() != 2u || !g_presentEvents[0].generated ||
        g_presentEvents[0].imageIndex != 1u ||
        g_presentEvents[0].waits != std::vector<std::uintptr_t>({0xE009u}) ||
        g_presentEvents[1].generated || g_presentEvents[1].imageIndex != 0u ||
        g_directDecisionImage != 1u || g_directDecisionValue != 11u ||
        !g_directDecisionPresented || !g_directDecisionBridgePrepared) return 5374;

    // If FI/OF is not complete at the first poll, the presenter must block on
    // the timeline *until either completion or the full source-production
    // deadline*, not reuse the generated frame's half-source display interval
    // as a GPU completion budget. Delay completion beyond the old 20 ms
    // midpoint while keeping it safely inside this frame's 40 ms source budget;
    // the generated->source pair must still be presented.
    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_generationReady = false;
    g_timelineWaitSucceeds = true;
    g_timelineWaitDelayMs = 25u;
    const std::uint32_t timelineWaitCallsBefore = g_timelineWaitCalls;
    auto boundedWaitSchedule = directWsiPresentationSchedule;
    boundedWaitSchedule.frameId = 7921u;
    boundedWaitSchedule.paceGeneratedFrame = true;
    boundedWaitSchedule.sourceFrameTimeMs = 40.0f;
    if (apusrFramegenScheduleVulkanPresentation(presentation, boundedWaitSchedule) != APUSR_FRAMEGEN_OK) return 53741;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_timelineWaitCalls != timelineWaitCallsBefore + 1u || g_lastTimelineWaitTimeoutNs == 0u ||
        g_presentEvents.size() != 2u || !g_presentEvents[0].generated || g_presentEvents[1].generated ||
        !g_directDecisionPresented || !g_directDecisionBridgePrepared) return 53742;
    g_timelineWaitDelayMs = 0u;
    g_timelineWaitSucceeds = false;

    // CPU Present can arrive well before the application's render-finished
    // semaphore makes Phase A runnable. That upstream producer delay is not FG
    // execution time and must not consume the generation relevance budget.
    // Hold producerReady longer than the entire 20 ms source budget, then let
    // actual generation complete 5 ms after it becomes runnable. G must survive.
    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_generationReady = false;
    g_generationStartReady = false;
    g_generationStartWaitSucceeds = true;
    g_generationStartWaitDelayMs = 25u;
    g_timelineWaitSucceeds = true;
    g_timelineWaitDelayMs = 5u;
    const std::uint32_t startAnchoredWaitCallsBefore = g_timelineWaitCalls;
    auto startAnchoredSchedule = directWsiPresentationSchedule;
    startAnchoredSchedule.frameId = 79211u;
    startAnchoredSchedule.paceGeneratedFrame = true;
    startAnchoredSchedule.sourceFrameTimeMs = 20.0f;
    startAnchoredSchedule.generationStartSemaphore = handle<void*>(0xE00C);
    startAnchoredSchedule.generationStartValue = 12u;
    if (apusrFramegenScheduleVulkanPresentation(presentation, startAnchoredSchedule) != APUSR_FRAMEGEN_OK) return 537421;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_timelineWaitCalls != startAnchoredWaitCallsBefore + 2u ||
        g_presentEvents.size() != 2u || !g_presentEvents[0].generated || g_presentEvents[1].generated ||
        !g_directDecisionPresented || !g_directDecisionBridgePrepared) return 537422;
    g_generationStartReady = true;
    g_generationStartWaitSucceeds = false;
    g_generationStartWaitDelayMs = 0u;
    g_timelineWaitDelayMs = 0u;
    g_timelineWaitSucceeds = false;

    // Missing an ideal midpoint is not, by itself, permission to discard G.
    // Even when the preceding source has already been visible for much longer
    // than delta, an already-complete generation remains useful if this frame's
    // own work fits inside the source-period admission budget. Present G now and
    // give it one full delta before S; do not reintroduce the rejected late-fit drop.
    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_generationReady = true;
    g_directDecisionPresented = true;
    g_directDecisionBridgePrepared = true;
    // Use a fresh presentation clock so the check is independent of the long
    // synthetic timing intervals exercised above.
    ApusrFramegenVulkanPresentation* stalePresentation = nullptr;
    auto staleDescription = presentationDescription;
    staleDescription.initialSourceFrameTimeMs = 4.0f;
    if (apusrFramegenCreateVulkanPresentation(
            staleDescription, {}, &stalePresentation) != APUSR_FRAMEGEN_OK ||
        !stalePresentation) return 537431;
    void* staleAnchorWait = handle<void*>(0xE00A);
    ApusrFramegenVulkanPresentationSchedule staleAnchorSchedule{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 79215u,
        .sourceImageIndex = 0u,
        .sourceWaitSemaphores = &staleAnchorWait,
        .sourceWaitSemaphoreCount = 1u,
        .paceSourceFrame = true,
        .sourceFrameTimeMs = 4.0f,
    };
    if (apusrFramegenScheduleVulkanPresentation(
            stalePresentation, staleAnchorSchedule) != APUSR_FRAMEGEN_OK) return 537432;
    apusrFramegenDrainVulkanPresentation(stalePresentation);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    g_presentEvents.clear();
    g_presentationOrder.clear();
    g_sourceReplayDelayMs = 3u;
    auto staleReadySchedule = directWsiPresentationSchedule;
    staleReadySchedule.frameId = 7922u;
    staleReadySchedule.paceGeneratedFrame = true;
    staleReadySchedule.sourceFrameTimeMs = 4.0f;
    staleReadySchedule.sourceImageIndex = 2u;
    staleReadySchedule.replaySourceFrame = true;
    staleReadySchedule.sourceReplaySlotIndex = 2u;
    staleReadySchedule.sourceReplayCompletionValue = 9u;
    if (apusrFramegenScheduleVulkanPresentation(
            stalePresentation, staleReadySchedule) != APUSR_FRAMEGEN_OK) return 53743;
    apusrFramegenDrainVulkanPresentation(stalePresentation);
    g_sourceReplayDelayMs = 0u;
    if (g_presentEvents.size() != 2u || !g_presentEvents[0].generated ||
        g_presentEvents[1].generated || g_presentEvents[1].imageIndex != 0u ||
        !g_directDecisionPresented || !g_directDecisionBridgePrepared) return 53744;
    apusrFramegenDestroyVulkanPresentation(stalePresentation, {});

    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_generationReady = false;
    g_directDecisionPresented = true;
    g_directDecisionBridgePrepared = true;
    if (apusrFramegenScheduleVulkanPresentation(presentation, directWsiPresentationSchedule) != APUSR_FRAMEGEN_OK) return 5375;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_presentEvents.size() != 1u || g_presentEvents[0].generated ||
        g_directDecisionValue != 11u || g_directDecisionPresented || g_directDecisionBridgePrepared) return 5376;

    // Full proxy presentation: generated replay privately acquires/copies a
    // real WSI image after FI/OF completion, then source replay follows on the
    // same presentation queue. Neither path presents the virtual host image.
    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_generationReady = true;
    g_generationDecisionValue = 0u;
    g_generationDecisionPresented = false;
    const ApusrFramegenVulkanPresentationSchedule proxyPresentationSchedule{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 80u,
        .sourceImageIndex = 2u,
        .sourceWaitSemaphores = nullptr,
        .sourceWaitSemaphoreCount = 0u,
        .hasGeneratedFrame = true,
        .paceGeneratedFrame = false,
        .replayGeneratedFrame = true,
        .generationReplaySlotIndex = 2u,
        .generationCompletionSemaphore = handle<void*>(0xE006),
        .generationCompletionValue = 11u,
        .generatedReplayCompletionValue = 10u,
        .replaySourceFrame = true,
        .sourceReplaySlotIndex = 2u,
        .sourceReplayCompletionValue = 9u,
    };
    if (apusrFramegenScheduleVulkanPresentation(presentation, proxyPresentationSchedule) != APUSR_FRAMEGEN_OK) return 538;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_presentationOrder != std::vector<int>({2, 0, 1, 3}) || g_presentEvents.size() != 2u ||
        !g_presentEvents[0].generated || g_presentEvents[0].imageIndex != 1u ||
        g_presentEvents[0].waits != std::vector<std::uintptr_t>({0xE007u}) ||
        g_presentEvents[1].generated || g_presentEvents[1].imageIndex != 0u ||
        g_presentEvents[1].waits != std::vector<std::uintptr_t>({0xE004u}) ||
        g_generationDecisionValue != 11u || !g_generationDecisionPresented) return 539;

    // A late generation must be dropped without ever putting its unsatisfied
    // timeline wait on the WSI queue; the source replay still presents.
    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_generationReady = false;
    g_generationDecisionValue = 0u;
    g_generationDecisionPresented = true;
    auto lateGenerationSchedule = proxyPresentationSchedule;
    lateGenerationSchedule.frameId = 801u;
    lateGenerationSchedule.paceGeneratedFrame = false;
    if (apusrFramegenScheduleVulkanPresentation(presentation, lateGenerationSchedule) != APUSR_FRAMEGEN_OK) return 5391;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (g_presentationOrder != std::vector<int>({2, 3}) || g_presentEvents.size() != 1u ||
        g_presentEvents[0].generated || g_presentEvents[0].waits != std::vector<std::uintptr_t>({0xE004u}) ||
        g_generationDecisionValue != 11u || g_generationDecisionPresented) return 5392;
    g_generationReady = true;

    // Generated proxy mode carries the producer-ready fan-out timeline into
    // source replay, so the mandatory source copy may overlap Phase A without
    // attempting to consume the application's binary semaphore a second time.
    g_presentationOrder.clear();
    g_presentEvents.clear();
    g_sourceReplaySawUpstream = false;
    const ApusrFramegenVulkanPresentationSchedule sourceProxySchedule{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 81u,
        .sourceImageIndex = 2u,
        .sourceWaitSemaphores = nullptr,
        .sourceWaitSemaphoreCount = 0u,
        .replaySourceFrame = true,
        .sourceReplaySlotIndex = 2u,
        .sourceReplayCompletionValue = 9u,
        .sourceReplayUpstreamSemaphore = handle<void*>(0xE006),
        .sourceReplayUpstreamValue = 11u,
    };
    if (apusrFramegenScheduleVulkanPresentation(presentation, sourceProxySchedule) != APUSR_FRAMEGEN_OK) return 540;
    apusrFramegenDrainVulkanPresentation(presentation);
    if (!g_sourceReplaySawUpstream || g_presentationOrder != std::vector<int>({2, 3}) || g_presentEvents.size() != 1u ||
        g_presentEvents[0].generated || g_presentEvents[0].imageIndex != 0u ||
        g_presentEvents[0].waits != std::vector<std::uintptr_t>({0xE004u})) return 5401;

    apusrFramegenDestroyVulkanPresentation(presentation, {});

    // A virtual swapchain can otherwise let application Present run several
    // source frames ahead of physical FIFO WSI. The configured steady-state
    // lead bound must apply condition-variable back-pressure without draining
    // the whole presenter or moving midpoint pacing onto the application
    // thread. Hold each physical present long enough to prove that a third
    // schedule cannot be admitted while two source tasks remain outstanding.
    g_presentEvents.clear();
    g_presentationOrder.clear();
    g_presentDelayMs = 100u;
    ApusrFramegenVulkanPresentation* leadBoundPresentation = nullptr;
    auto leadBoundDescription = presentationDescription;
    leadBoundDescription.maxPendingPresentCount = 4u;
    leadBoundDescription.maxApplicationPresentQueueDepth = 2u;
    if (apusrFramegenCreateVulkanPresentation(
            leadBoundDescription, {}, &leadBoundPresentation) != APUSR_FRAMEGEN_OK ||
        !leadBoundPresentation) return 54011;
    auto leadBoundSchedule = sourceOnlySchedule;
    leadBoundSchedule.frameId = 820u;
    if (apusrFramegenScheduleVulkanPresentation(
            leadBoundPresentation, leadBoundSchedule) != APUSR_FRAMEGEN_OK) return 54012;
    leadBoundSchedule.frameId = 821u;
    if (apusrFramegenScheduleVulkanPresentation(
            leadBoundPresentation, leadBoundSchedule) != APUSR_FRAMEGEN_OK) return 54013;
    leadBoundSchedule.frameId = 822u;
    const auto leadWaitBegin = std::chrono::steady_clock::now();
    if (apusrFramegenScheduleVulkanPresentation(
            leadBoundPresentation, leadBoundSchedule) != APUSR_FRAMEGEN_OK) return 54014;
    const auto leadWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - leadWaitBegin).count();
    apusrFramegenDrainVulkanPresentation(leadBoundPresentation);
    apusrFramegenDestroyVulkanPresentation(leadBoundPresentation, {});
    g_presentDelayMs = 0u;
    if (leadWaitMs < 50 || g_presentEvents.size() != 3u) return 54015;

    // Pair-local pacing uses equal G->S spacing without making the ideal
    // midpoint an eligibility deadline. Bootstrap queues G immediately and S
    // at +delta; subsequent pairs use native relative timing for both halves.
    // Even a deliberately late third G survives if generation itself met the
    // full source-period budget, and S follows one delta later.
    g_presentEvents.clear();
    g_presentationOrder.clear();
    g_relativeTimingSupported = true;
    g_relativeTimingCalls = 0u;
    g_relativeTimingDurationNs = 0u;
    g_generatedRelativeTimingCalls = 0u;
    g_generatedRelativeTimingDurationNs = 0u;
    ApusrFramegenVulkanPresentation* pairLocalPresentation = nullptr;
    auto pairLocalDescription = presentationDescription;
    pairLocalDescription.initialSourceFrameTimeMs = 4.0f;
    if (apusrFramegenCreateVulkanPresentation(
            pairLocalDescription, {}, &pairLocalPresentation) != APUSR_FRAMEGEN_OK ||
        !pairLocalPresentation) return 5402;
    auto pairLocalSchedule = pacedSchedule;
    pairLocalSchedule.sourceFrameTimeMs = 4.0f;
    pairLocalSchedule.frameId = 900u;
    if (apusrFramegenScheduleVulkanPresentation(
            pairLocalPresentation, pairLocalSchedule) != APUSR_FRAMEGEN_OK) return 5403;
    apusrFramegenDrainVulkanPresentation(pairLocalPresentation);
    pairLocalSchedule.frameId = 901u;
    if (apusrFramegenScheduleVulkanPresentation(
            pairLocalPresentation, pairLocalSchedule) != APUSR_FRAMEGEN_OK) return 5404;
    apusrFramegenDrainVulkanPresentation(pairLocalPresentation);
    if (g_presentEvents.size() != 4u ||
        !g_presentEvents[0].generated || g_presentEvents[1].generated ||
        !g_presentEvents[2].generated || g_presentEvents[3].generated ||
        g_generatedRelativeTimingCalls != 1u ||
        g_generatedRelativeTimingDurationNs != 2'000'000u ||
        g_relativeTimingCalls != 2u || g_relativeTimingDurationNs != 2'000'000u) return 5405;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pairLocalSchedule.frameId = 902u;
    if (apusrFramegenScheduleVulkanPresentation(
            pairLocalPresentation, pairLocalSchedule) != APUSR_FRAMEGEN_OK) return 54051;
    apusrFramegenDrainVulkanPresentation(pairLocalPresentation);
    if (g_presentEvents.size() != 6u ||
        !g_presentEvents[4].generated || g_presentEvents[5].generated ||
        g_generatedRelativeTimingCalls != 2u || g_relativeTimingCalls != 3u ||
        g_generatedRelativeTimingDurationNs != 2'000'000u ||
        g_relativeTimingDurationNs != 2'000'000u) return 54052;
    apusrFramegenDestroyVulkanPresentation(pairLocalPresentation, {});

    // A startup hitch must not be converted directly into half-frame display
    // pacing. A previous implementation accepted an outlier first post-reset observation and
    // produced a 46.891 ms G/S interval. Warm-up now bounds each adjustment,
    // preserving a sane cadence until the 10-sample estimator is populated.
    g_presentEvents.clear();
    g_presentationOrder.clear();
    g_relativeTimingSupported = true;
    g_relativeTimingCalls = 0u;
    g_generatedRelativeTimingCalls = 0u;
    g_relativeTimingDurationNs = 0u;
    g_generatedRelativeTimingDurationNs = 0u;
    ApusrFramegenVulkanPresentation* hitchPresentation = nullptr;
    auto hitchDescription = presentationDescription;
    hitchDescription.initialSourceFrameTimeMs = 16.666668f;
    if (apusrFramegenCreateVulkanPresentation(
            hitchDescription, {}, &hitchPresentation) != APUSR_FRAMEGEN_OK ||
        !hitchPresentation) return 540521;
    void* hitchAnchorWait = handle<void*>(0xE011);
    ApusrFramegenVulkanPresentationSchedule hitchAnchor{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 905u,
        .sourceImageIndex = 0u,
        .sourceWaitSemaphores = &hitchAnchorWait,
        .sourceWaitSemaphoreCount = 1u,
        .paceSourceFrame = true,
        .sourceFrameTimeMs = 16.666668f,
    };
    if (apusrFramegenScheduleVulkanPresentation(
            hitchPresentation, hitchAnchor) != APUSR_FRAMEGEN_OK) return 540522;
    apusrFramegenDrainVulkanPresentation(hitchPresentation);
    auto hitchPair = pacedSchedule;
    hitchPair.frameId = 906u;
    hitchPair.sourceFrameTimeMs = 93.782096f;
    if (apusrFramegenScheduleVulkanPresentation(
            hitchPresentation, hitchPair) != APUSR_FRAMEGEN_OK) return 540523;
    apusrFramegenDrainVulkanPresentation(hitchPresentation);
    if (g_generatedRelativeTimingCalls != 1u || g_relativeTimingCalls != 1u ||
        g_generatedRelativeTimingDurationNs > 17'000'000u ||
        g_relativeTimingDurationNs > 17'000'000u) return 540524;
    apusrFramegenDestroyVulkanPresentation(hitchPresentation, {});

    // AMD's swapchain uses a separate interpolation/readiness thread precisely
    // so waiting for the next FI completion can overlap the current pair's
    // G->source display interval. A 60 ms FI readiness wait for pair N+1 must
    // overlap pair N's 45 ms source interval and complete within N+1's 90 ms
    // generation budget. A serial presenter would start that wait after 45 ms
    // and miss the 90 ms deadline. The 30 ms scheduling margin also prevents
    // host timer jitter from invalidating this concurrency contract.
    g_presentEvents.clear();
    g_presentationOrder.clear();
    g_relativeTimingSupported = false;
    g_generationReady = false;
    g_timelineWaitSucceeds = true;
    g_timelineWaitDelayMs = 60u;
    const std::uint32_t overlapWaitCallsBefore = g_timelineWaitCalls;
    ApusrFramegenVulkanPresentation* overlapPresentation = nullptr;
    auto overlapDescription = presentationDescription;
    overlapDescription.initialSourceFrameTimeMs = 90.0f;
    if (apusrFramegenCreateVulkanPresentation(
            overlapDescription, {}, &overlapPresentation) != APUSR_FRAMEGEN_OK ||
        !overlapPresentation) return 54053;
    auto overlapFirst = pacedSchedule;
    overlapFirst.frameId = 910u;
    overlapFirst.sourceFrameTimeMs = 90.0f;
    if (apusrFramegenScheduleVulkanPresentation(
            overlapPresentation, overlapFirst) != APUSR_FRAMEGEN_OK) return 54054;
    auto overlapSecond = directWsiPresentationSchedule;
    overlapSecond.frameId = 911u;
    overlapSecond.paceGeneratedFrame = true;
    overlapSecond.sourceFrameTimeMs = 90.0f;
    if (apusrFramegenScheduleVulkanPresentation(
            overlapPresentation, overlapSecond) != APUSR_FRAMEGEN_OK) return 54055;
    apusrFramegenDrainVulkanPresentation(overlapPresentation);
    if (g_timelineWaitCalls != overlapWaitCallsBefore + 1u ||
        g_presentEvents.size() != 4u ||
        !g_presentEvents[0].generated || g_presentEvents[1].generated ||
        !g_presentEvents[2].generated || g_presentEvents[3].generated ||
        !g_directDecisionPresented || !g_directDecisionBridgePrepared) {
        std::fprintf(stderr,
            "overlap: timeline_waits=%u expected=%u events=%zu direct_present=%d bridge=%d sequence=",
            g_timelineWaitCalls, overlapWaitCallsBefore + 1u, g_presentEvents.size(),
            g_directDecisionPresented, g_directDecisionBridgePrepared);
        for (const auto& event : g_presentEvents)
            std::fprintf(stderr, "%c", event.generated ? 'G' : 'S');
        std::fprintf(stderr, "\n");
        return 54056;
    }
    apusrFramegenDestroyVulkanPresentation(overlapPresentation, {});
    g_timelineWaitDelayMs = 0u;
    g_timelineWaitSucceeds = false;
    g_generationReady = true;
    g_relativeTimingSupported = true;

    // The universal timer path must remain single-present correct when the host
    // exposes the callback contract but the actual swapchain cannot schedule
    // VK_EXT_present_timing. The callback may be probed, but it must not consume
    // the frame when it leaves outScheduled=false; G and source then each fall
    // through to exactly one ordinary present.
    g_presentEvents.clear();
    g_presentationOrder.clear();
    g_relativeTimingSupported = false;
    g_relativeTimingCalls = 0u;
    g_generatedRelativeTimingCalls = 0u;
    ApusrFramegenVulkanPresentation* fallbackPresentation = nullptr;
    auto fallbackDescription = presentationDescription;
    fallbackDescription.initialSourceFrameTimeMs = 4.0f;
    if (apusrFramegenCreateVulkanPresentation(
            fallbackDescription, {}, &fallbackPresentation) != APUSR_FRAMEGEN_OK ||
        !fallbackPresentation) return 5406;
    void* fallbackSourceWait = handle<void*>(0xE010);
    ApusrFramegenVulkanPresentationSchedule fallbackAnchor{
        .queue = handle<void*>(0xE100),
        .swapchain = handle<void*>(0xE200),
        .frameId = 902u,
        .sourceImageIndex = 0u,
        .sourceWaitSemaphores = &fallbackSourceWait,
        .sourceWaitSemaphoreCount = 1u,
        .sourceFrameTimeMs = 4.0f,
    };
    if (apusrFramegenScheduleVulkanPresentation(
            fallbackPresentation, fallbackAnchor) != APUSR_FRAMEGEN_OK) return 5407;
    apusrFramegenDrainVulkanPresentation(fallbackPresentation);
    auto fallbackPair = pacedSchedule;
    fallbackPair.frameId = 903u;
    fallbackPair.sourceFrameTimeMs = 4.0f;
    if (apusrFramegenScheduleVulkanPresentation(
            fallbackPresentation, fallbackPair) != APUSR_FRAMEGEN_OK) return 5408;
    apusrFramegenDrainVulkanPresentation(fallbackPresentation);
    if (g_presentEvents.size() != 3u ||
        g_presentEvents[0].generated || !g_presentEvents[1].generated || g_presentEvents[2].generated ||
        g_generatedRelativeTimingCalls != 1u || g_relativeTimingCalls != 1u) return 5409;

    // Some hosts exercise the timer fallback rather than native present
    // timing, so regress the late-pair rule there too. Missing the ideal
    // midpoint must present G immediately and then hold it for delta before S.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    fallbackPair.frameId = 904u;
    if (apusrFramegenScheduleVulkanPresentation(
            fallbackPresentation, fallbackPair) != APUSR_FRAMEGEN_OK) return 5410;
    apusrFramegenDrainVulkanPresentation(fallbackPresentation);
    if (g_presentEvents.size() != 5u ||
        !g_presentEvents[3].generated || g_presentEvents[4].generated ||
        g_generatedRelativeTimingCalls != 2u || g_relativeTimingCalls != 2u) return 5411;
    apusrFramegenDestroyVulkanPresentation(fallbackPresentation, {});
    g_relativeTimingSupported = true;

    if (g.borrowedImageViewDestroyed) return 54;

    std::puts("APUSR Vulkan FrameGen contract: retained virtual source, split graphics/async-compute ownership, private-WSI replay, slot timelines, and recovery validated");
    return 0;
}

int main()
{
    const int result = runContract();
    if (result != 0) {
        std::fprintf(stderr, "APUSR Vulkan FrameGen contract failed at check %d\n", result);
    }
    return result;
}
