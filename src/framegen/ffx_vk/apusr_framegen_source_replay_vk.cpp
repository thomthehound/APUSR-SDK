// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_source_replay_vk.h"

#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"

#include <array>
#include <chrono>
#include <mutex>
#include <new>

namespace {
namespace vk = apusr::backends::vk;

constexpr std::uint64_t kInfiniteTimeout = ~std::uint64_t{0};
constexpr std::int32_t kVkSuccess = 0;
constexpr std::int32_t kVkNotReady = 1;
constexpr std::int32_t kVkTimeout = 2;
constexpr std::int32_t kVkSuboptimalKhr = 1000001003;

struct Functions {
    vk::PfnGetDeviceProcAddr getDeviceProcAddr = nullptr;
    vk::PfnCreateCommandPool createCommandPool = nullptr;
    vk::PfnDestroyCommandPool destroyCommandPool = nullptr;
    vk::PfnAllocateCommandBuffers allocateCommandBuffers = nullptr;
    vk::PfnResetCommandBuffer resetCommandBuffer = nullptr;
    vk::PfnBeginCommandBuffer beginCommandBuffer = nullptr;
    vk::PfnEndCommandBuffer endCommandBuffer = nullptr;
    vk::PfnCreateSemaphore createSemaphore = nullptr;
    vk::PfnDestroySemaphore destroySemaphore = nullptr;
    vk::PfnGetSemaphoreCounterValue getSemaphoreCounterValue = nullptr;
    vk::PfnCreateFence createFence = nullptr;
    vk::PfnDestroyFence destroyFence = nullptr;
    vk::PfnGetFenceStatus getFenceStatus = nullptr;
    vk::PfnWaitForFences waitForFences = nullptr;
    vk::PfnResetFences resetFences = nullptr;
    vk::PfnQueueSubmit queueSubmit = nullptr;
    vk::PfnQueueWaitIdle queueWaitIdle = nullptr;
};

template <typename Fn>
bool loadFunction(const ApusrFramegenVulkanDeviceContext& deviceContext, Functions& functions, const char* name, Fn& out) noexcept
{
    out = reinterpret_cast<Fn>(functions.getDeviceProcAddr(
        reinterpret_cast<vk::Device>(deviceContext.device), name));
    return out != nullptr;
}

bool loadFunctions(const ApusrFramegenVulkanDeviceContext& deviceContext, Functions& functions) noexcept
{
    functions.getDeviceProcAddr = reinterpret_cast<vk::PfnGetDeviceProcAddr>(deviceContext.getDeviceProcAddr);
    if (!functions.getDeviceProcAddr && deviceContext.getInstanceProcAddr && deviceContext.instance) {
        const auto getInstanceProcAddr = reinterpret_cast<vk::PfnGetInstanceProcAddr>(deviceContext.getInstanceProcAddr);
        functions.getDeviceProcAddr = reinterpret_cast<vk::PfnGetDeviceProcAddr>(
            getInstanceProcAddr(reinterpret_cast<vk::Instance>(deviceContext.instance), "vkGetDeviceProcAddr"));
    }
    if (!functions.getDeviceProcAddr || !deviceContext.device)
        return false;

#define APUSR_LOAD_VK(name, field) if (!loadFunction(deviceContext, functions, name, functions.field)) return false
    APUSR_LOAD_VK("vkCreateCommandPool", createCommandPool);
    APUSR_LOAD_VK("vkDestroyCommandPool", destroyCommandPool);
    APUSR_LOAD_VK("vkAllocateCommandBuffers", allocateCommandBuffers);
    APUSR_LOAD_VK("vkResetCommandBuffer", resetCommandBuffer);
    APUSR_LOAD_VK("vkBeginCommandBuffer", beginCommandBuffer);
    APUSR_LOAD_VK("vkEndCommandBuffer", endCommandBuffer);
    APUSR_LOAD_VK("vkCreateSemaphore", createSemaphore);
    APUSR_LOAD_VK("vkDestroySemaphore", destroySemaphore);
    APUSR_LOAD_VK("vkGetSemaphoreCounterValue", getSemaphoreCounterValue);
    APUSR_LOAD_VK("vkCreateFence", createFence);
    APUSR_LOAD_VK("vkDestroyFence", destroyFence);
    APUSR_LOAD_VK("vkGetFenceStatus", getFenceStatus);
    APUSR_LOAD_VK("vkWaitForFences", waitForFences);
    APUSR_LOAD_VK("vkResetFences", resetFences);
    APUSR_LOAD_VK("vkQueueSubmit", queueSubmit);
    APUSR_LOAD_VK("vkQueueWaitIdle", queueWaitIdle);
#undef APUSR_LOAD_VK
    return true;
}

bool acceptedAcquire(const std::int32_t result) noexcept
{
    return result == kVkSuccess || result == kVkSuboptimalKhr;
}

} // namespace

struct ApusrFramegenVulkanSourceReplayContext {
    ApusrFramegenVulkanSourceReplayDescription description{};
    Functions functions{};
    vk::CommandPool commandPool = nullptr;
    vk::CommandBuffer commandBuffer = nullptr;
    vk::Fence completionFence = nullptr;
    vk::Semaphore acquireSemaphore = nullptr;
    vk::Semaphore sourceConsumedTimeline = nullptr;
    vk::Semaphore* sourceReadySemaphores = nullptr;
    std::uint64_t lastReservedCompletionValue = 0u;
    bool submitted = false;
    // App-thread source prearm and worker-thread cold replay share one command
    // pool/buffer, fence, acquire semaphore, and VkQueue. Serialize ownership
    // explicitly; prearm uses try_lock so it can never stall the game thread.
    std::mutex prepareMutex;
};

namespace {

void destroyContents(ApusrFramegenVulkanSourceReplayContext& context, const ApusrFramegenAllocator& allocator) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(context.description.deviceContext.device);
    const auto queue = reinterpret_cast<vk::Queue>(context.description.queue);
    if (queue && context.functions.queueWaitIdle)
        (void)context.functions.queueWaitIdle(queue);

    if (context.sourceReadySemaphores) {
        for (std::uint32_t i = 0; i < context.description.presentImageCount; ++i) {
            if (context.sourceReadySemaphores[i])
                context.functions.destroySemaphore(device, context.sourceReadySemaphores[i], nullptr);
        }
        allocator.dealloc(context.sourceReadySemaphores);
        context.sourceReadySemaphores = nullptr;
    }
    if (context.sourceConsumedTimeline) {
        context.functions.destroySemaphore(device, context.sourceConsumedTimeline, nullptr);
        context.sourceConsumedTimeline = nullptr;
    }
    if (context.acquireSemaphore) {
        context.functions.destroySemaphore(device, context.acquireSemaphore, nullptr);
        context.acquireSemaphore = nullptr;
    }
    if (context.completionFence) {
        context.functions.destroyFence(device, context.completionFence, nullptr);
        context.completionFence = nullptr;
    }
    if (context.commandPool) {
        context.functions.destroyCommandPool(device, context.commandPool, nullptr);
        context.commandPool = nullptr;
        context.commandBuffer = nullptr;
    }
}

ApusrFramegenResult waitForPreviousSubmit(ApusrFramegenVulkanSourceReplayContext& context) noexcept
{
    if (!context.submitted)
        return APUSR_FRAMEGEN_OK;
    const auto device = reinterpret_cast<vk::Device>(context.description.deviceContext.device);
    const vk::Result status = context.functions.getFenceStatus(device, context.completionFence);
    if (status == vk::NotReady) {
        if (context.functions.waitForFences(
                device, 1u, &context.completionFence, 1u, kInfiniteTimeout) != vk::Success)
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    } else if (status != vk::Success) {
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    context.submitted = false;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult signalCompletionWithoutReplay(
    ApusrFramegenVulkanSourceReplayContext& context,
    const std::uint64_t completionValue) noexcept
{
    if (completionValue == 0u)
        return APUSR_FRAMEGEN_OK;
    const vk::Semaphore signal = context.sourceConsumedTimeline;
    const vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .sType = vk::StructureTypeTimelineSemaphoreSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreValueCount = 0u,
        .pWaitSemaphoreValues = nullptr,
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &completionValue,
    };
    const vk::SubmitInfo submit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = &timelineInfo,
        .waitSemaphoreCount = 0u,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 0u,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal,
    };
    return context.functions.queueSubmit(
        reinterpret_cast<vk::Queue>(context.description.queue), 1u, &submit, nullptr) == vk::Success
        ? APUSR_FRAMEGEN_OK
        : APUSR_FRAMEGEN_ERROR_BACKEND_API;
}

bool releaseAcquiredImageAfterColdFailure(
    ApusrFramegenVulkanSourceReplayContext& context,
    const std::uint32_t targetImageIndex) noexcept
{
    if (!context.description.releaseImage || targetImageIndex >= context.description.presentImageCount)
        return false;

    // vkAcquireNextImageKHR has signaled this binary semaphore. Consume that
    // signal before allowing either WSI or the next acquire to reuse it. This
    // path executes only after a backend failure, so draining APUSR's private
    // replay queue is preferable to carrying ambiguous binary-semaphore state.
    const vk::Semaphore wait = context.acquireSemaphore;
    const vk::PipelineStageFlags waitStage = vk::PipelineStageTransferBit;
    const vk::SubmitInfo submit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreCount = 1u,
        .pWaitSemaphores = &wait,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 0u,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 0u,
        .pSignalSemaphores = nullptr,
    };
    const auto queue = reinterpret_cast<vk::Queue>(context.description.queue);
    if (context.functions.queueSubmit(queue, 1u, &submit, nullptr) != vk::Success ||
        context.functions.queueWaitIdle(queue) != vk::Success)
        return false;
    return context.description.releaseImage(
        context.description.userContext,
        context.description.device,
        context.description.swapchain,
        targetImageIndex);
}

ApusrFramegenResult submitAcquiredImageRetirement(
    ApusrFramegenVulkanSourceReplayContext& context,
    const std::uint32_t targetImageIndex,
    const std::uint64_t completionValue) noexcept
{
    if (targetImageIndex >= context.description.presentImageCount)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    if (context.functions.resetCommandBuffer(context.commandBuffer, 0u) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    const vk::CommandBufferBeginInfo beginInfo{
        .sType = vk::StructureTypeCommandBufferBeginInfo,
        .pNext = nullptr,
        .flags = vk::CommandBufferUsageOneTimeSubmitBit,
        .pInheritanceInfo = nullptr,
    };
    if (context.functions.beginCommandBuffer(context.commandBuffer, &beginInfo) != vk::Success ||
        apusrFramegenRecordVulkanAcquiredImageRetirement(
            context.description.presentImages, context.commandBuffer, targetImageIndex) != APUSR_FRAMEGEN_OK ||
        context.functions.endCommandBuffer(context.commandBuffer) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    const vk::Semaphore wait = context.acquireSemaphore;
    const vk::PipelineStageFlags waitStage = vk::PipelineStageTransferBit;
    std::array<vk::Semaphore, 2u> signals{};
    std::array<std::uint64_t, 2u> signalValues{};
    std::uint32_t signalCount = 0u;
    signals[signalCount] = context.sourceReadySemaphores[targetImageIndex];
    signalValues[signalCount] = 0u;
    ++signalCount;
    if (completionValue != 0u) {
        signals[signalCount] = context.sourceConsumedTimeline;
        signalValues[signalCount] = completionValue;
        ++signalCount;
    }
    const std::uint64_t waitValue = 0u;
    const vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .sType = vk::StructureTypeTimelineSemaphoreSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreValueCount = 1u,
        .pWaitSemaphoreValues = &waitValue,
        .signalSemaphoreValueCount = signalCount,
        .pSignalSemaphoreValues = signalValues.data(),
    };
    const vk::SubmitInfo submit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = &timelineInfo,
        .waitSemaphoreCount = 1u,
        .pWaitSemaphores = &wait,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 1u,
        .pCommandBuffers = &context.commandBuffer,
        .signalSemaphoreCount = signalCount,
        .pSignalSemaphores = signals.data(),
    };
    if (context.functions.queueSubmit(
            reinterpret_cast<vk::Queue>(context.description.queue), 1u, &submit, context.completionFence) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    context.submitted = true;
    return APUSR_FRAMEGEN_OK;
}

} // namespace

ApusrFramegenResult apusrFramegenCreateVulkanSourceReplayContext(
    const ApusrFramegenVulkanSourceReplayDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanSourceReplayContext** outContext) noexcept
{
    if (!outContext)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outContext = nullptr;
    if (!description.deviceContext.device || !description.queue || !description.device || !description.swapchain ||
        description.presentImageCount == 0u || !description.presentImages || !description.acquireImage ||
        (description.content != APUSR_FRAMEGEN_VULKAN_REPLAY_SOURCE &&
         description.content != APUSR_FRAMEGEN_VULKAN_REPLAY_GENERATED))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    auto* context = allocator.construct<ApusrFramegenVulkanSourceReplayContext>();
    if (!context)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    context->description = description;
    if (context->description.sourceSlotCount == 0u)
        context->description.sourceSlotCount = description.presentImageCount;
    if (!loadFunctions(description.deviceContext, context->functions)) {
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;
    }

    const auto device = reinterpret_cast<vk::Device>(description.deviceContext.device);
    const vk::CommandPoolCreateInfo poolInfo{
        .sType = vk::StructureTypeCommandPoolCreateInfo,
        .pNext = nullptr,
        .flags = vk::CommandPoolCreateResetCommandBufferBit,
        .queueFamilyIndex = description.queueFamilyIndex,
    };
    if (context->functions.createCommandPool(device, &poolInfo, nullptr, &context->commandPool) != vk::Success ||
        !context->commandPool) {
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    const vk::CommandBufferAllocateInfo allocateInfo{
        .sType = vk::StructureTypeCommandBufferAllocateInfo,
        .pNext = nullptr,
        .commandPool = context->commandPool,
        .level = vk::CommandBufferLevelPrimary,
        .commandBufferCount = 1u,
    };
    if (context->functions.allocateCommandBuffers(device, &allocateInfo, &context->commandBuffer) != vk::Success ||
        !context->commandBuffer) {
        destroyContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    const vk::FenceCreateInfo fenceInfo{
        .sType = vk::StructureTypeFenceCreateInfo,
        .pNext = nullptr,
        .flags = vk::FenceCreateSignaledBit,
    };
    if (context->functions.createFence(device, &fenceInfo, nullptr, &context->completionFence) != vk::Success ||
        !context->completionFence) {
        destroyContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    const vk::SemaphoreCreateInfo semaphoreInfo{
        .sType = vk::StructureTypeSemaphoreCreateInfo,
        .pNext = nullptr,
        .flags = 0u,
    };
    if (context->functions.createSemaphore(device, &semaphoreInfo, nullptr, &context->acquireSemaphore) != vk::Success ||
        !context->acquireSemaphore) {
        destroyContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    const vk::SemaphoreTypeCreateInfo timelineType{
        .sType = vk::StructureTypeSemaphoreTypeCreateInfo,
        .pNext = nullptr,
        .semaphoreType = vk::SemaphoreTypeTimeline,
        .initialValue = 0u,
    };
    const vk::SemaphoreCreateInfo timelineInfo{
        .sType = vk::StructureTypeSemaphoreCreateInfo,
        .pNext = &timelineType,
        .flags = 0u,
    };
    if (context->functions.createSemaphore(
            device, &timelineInfo, nullptr, &context->sourceConsumedTimeline) != vk::Success ||
        !context->sourceConsumedTimeline) {
        destroyContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    context->sourceReadySemaphores = static_cast<vk::Semaphore*>(
        allocator.alloc(sizeof(vk::Semaphore) * description.presentImageCount));
    if (!context->sourceReadySemaphores) {
        destroyContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    for (std::uint32_t i = 0; i < description.presentImageCount; ++i)
        context->sourceReadySemaphores[i] = nullptr;
    for (std::uint32_t i = 0; i < description.presentImageCount; ++i) {
        if (context->functions.createSemaphore(
                device, &semaphoreInfo, nullptr, &context->sourceReadySemaphores[i]) != vk::Success ||
            !context->sourceReadySemaphores[i]) {
            destroyContents(*context, allocator);
            allocator.destroy(context);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
    }

    *outContext = context;
    return APUSR_FRAMEGEN_OK;
}

void* apusrFramegenVulkanReplayConsumedSemaphore(
    const ApusrFramegenVulkanSourceReplayContext* context) noexcept
{
    return context ? context->sourceConsumedTimeline : nullptr;
}

std::uint64_t apusrFramegenVulkanCompletedReplayValue(
    const ApusrFramegenVulkanSourceReplayContext* context) noexcept
{
    if (!context || !context->sourceConsumedTimeline || !context->functions.getSemaphoreCounterValue)
        return 0u;
    std::uint64_t value = 0u;
    const auto device = reinterpret_cast<vk::Device>(context->description.deviceContext.device);
    return context->functions.getSemaphoreCounterValue(device, context->sourceConsumedTimeline, &value) == vk::Success
        ? value
        : 0u;
}

std::uint64_t apusrFramegenReserveVulkanReplayCompletion(
    ApusrFramegenVulkanSourceReplayContext* context) noexcept
{
    if (!context || context->lastReservedCompletionValue == ~std::uint64_t{0})
        return 0u;
    return ++context->lastReservedCompletionValue;
}

static ApusrFramegenResult prepareVulkanReplayImpl(
    ApusrFramegenVulkanSourceReplayContext* context,
    const std::uint32_t sourceSlotIndex,
    const std::uint64_t completionValue,
    void* const* producerWaitSemaphores,
    const std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore,
    const std::uint64_t upstreamWaitValue,
    const bool nonblockingSourcePrearm,
    std::uint32_t* outImageIndex,
    void** outReadySemaphore,
    bool* outCopied,
    ApusrFramegenVulkanSourceReplayMetrics* outMetrics) noexcept
{
    if (!context || !outImageIndex || !outReadySemaphore || !outCopied)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outImageIndex = 0u;
    *outReadySemaphore = nullptr;
    *outCopied = false;
    if (outMetrics)
        *outMetrics = {};

    const bool sourceReplay = context->description.content == APUSR_FRAMEGEN_VULKAN_REPLAY_SOURCE;
    if (nonblockingSourcePrearm && !context->description.releaseImage)
        return APUSR_FRAMEGEN_OK;
    if ((nonblockingSourcePrearm && !sourceReplay) || sourceSlotIndex >= context->description.sourceSlotCount ||
        producerWaitSemaphoreCount > 64u ||
        (producerWaitSemaphoreCount != 0u && !producerWaitSemaphores) ||
        (upstreamWaitValue != 0u && !upstreamWaitSemaphore) ||
        (upstreamWaitSemaphore && upstreamWaitValue == 0u) ||
        completionValue == 0u || completionValue > context->lastReservedCompletionValue)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    for (std::uint32_t i = 0u; i < producerWaitSemaphoreCount; ++i) {
        if (!producerWaitSemaphores[i])
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }

    std::unique_lock<std::mutex> prepareLock(context->prepareMutex, std::defer_lock);
    if (nonblockingSourcePrearm) {
        if (!prepareLock.try_lock())
            return APUSR_FRAMEGEN_ERROR_BUSY;
    } else {
        prepareLock.lock();
    }

    auto* sharedQueueMutex = static_cast<std::mutex*>(context->description.queueSubmitMutex);
    std::unique_lock<std::mutex> queueLock;
    if (sharedQueueMutex) {
        queueLock = std::unique_lock<std::mutex>(*sharedQueueMutex, std::defer_lock);
        if (nonblockingSourcePrearm) {
            if (!queueLock.try_lock())
                return APUSR_FRAMEGEN_ERROR_BUSY;
        } else {
            queueLock.lock();
        }
    }

    const auto waitStart = std::chrono::steady_clock::now();
    ApusrFramegenResult result = APUSR_FRAMEGEN_OK;
    if (nonblockingSourcePrearm && context->submitted) {
        const auto device = reinterpret_cast<vk::Device>(context->description.deviceContext.device);
        const vk::Result status = context->functions.getFenceStatus(device, context->completionFence);
        if (status == vk::NotReady)
            result = APUSR_FRAMEGEN_ERROR_BUSY;
        else if (status == vk::Success)
            context->submitted = false;
        else
            result = APUSR_FRAMEGEN_ERROR_BACKEND_API;
    } else {
        result = waitForPreviousSubmit(*context);
    }
    if (outMetrics) {
        outMetrics->waitPreviousUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - waitStart).count());
    }
    if (result != APUSR_FRAMEGEN_OK) {
        if (!nonblockingSourcePrearm)
            (void)signalCompletionWithoutReplay(*context, completionValue);
        return result;
    }

    const auto device = reinterpret_cast<vk::Device>(context->description.deviceContext.device);
    if (context->functions.resetFences(device, 1u, &context->completionFence) != vk::Success ||
        context->functions.resetCommandBuffer(context->commandBuffer, 0u) != vk::Success) {
        if (!nonblockingSourcePrearm)
            (void)signalCompletionWithoutReplay(*context, completionValue);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    std::uint32_t targetImageIndex = 0u;
    const auto acquireStart = std::chrono::steady_clock::now();
    // Source presentation is mandatory and may wait for WSI availability. A
    // generated frame is opportunistic: never let a real-WSI acquire stall the
    // presenter after source replay has already secured the mandatory target.
    const std::uint64_t acquireTimeout = nonblockingSourcePrearm ? 0u : sourceReplay ? kInfiniteTimeout : 0u;
    const std::int32_t acquireResult = context->description.acquireImage(
        context->description.userContext,
        context->description.device,
        context->description.swapchain,
        acquireTimeout,
        context->acquireSemaphore,
        &targetImageIndex);
    if (outMetrics) {
        outMetrics->acquireUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - acquireStart).count());
    }
    if (nonblockingSourcePrearm && (acquireResult == kVkNotReady || acquireResult == kVkTimeout)) {
        return APUSR_FRAMEGEN_OK;
    }
    if (!sourceReplay && (acquireResult == kVkNotReady || acquireResult == kVkTimeout)) {
        // Clean real-time miss: free the generated-output slot immediately and
        // let the mandatory source frame proceed. This is not a backend error.
        (void)signalCompletionWithoutReplay(*context, completionValue);
        return APUSR_FRAMEGEN_OK;
    }
    if (!acceptedAcquire(acquireResult) || targetImageIndex >= context->description.presentImageCount) {
        if (!nonblockingSourcePrearm)
            (void)signalCompletionWithoutReplay(*context, completionValue);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    const vk::CommandBufferBeginInfo beginInfo{
        .sType = vk::StructureTypeCommandBufferBeginInfo,
        .pNext = nullptr,
        .flags = vk::CommandBufferUsageOneTimeSubmitBit,
        .pInheritanceInfo = nullptr,
    };
    const auto recordStart = std::chrono::steady_clock::now();
    ApusrFramegenResult recordResult = APUSR_FRAMEGEN_ERROR_BACKEND_API;
    if (context->functions.beginCommandBuffer(context->commandBuffer, &beginInfo) == vk::Success) {
        recordResult = sourceReplay
            ? apusrFramegenRecordVulkanSourceReplay(
                context->description.presentImages, context->commandBuffer,
                sourceSlotIndex, targetImageIndex)
            : apusrFramegenRecordVulkanGeneratedReplay(
                context->description.presentImages, context->commandBuffer,
                sourceSlotIndex, targetImageIndex);
        if (recordResult == APUSR_FRAMEGEN_OK &&
            context->functions.endCommandBuffer(context->commandBuffer) != vk::Success)
            recordResult = APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    if (outMetrics) {
        outMetrics->recordUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - recordStart).count());
    }
    if (recordResult != APUSR_FRAMEGEN_OK) {
        // With swapchain-maintenance1, discard the private acquisition instead
        // of ever presenting an undefined retirement image. A failed prearm
        // leaves its completion reservation untouched so the worker can retry
        // the same mandatory source normally.
        if (releaseAcquiredImageAfterColdFailure(*context, targetImageIndex)) {
            if (nonblockingSourcePrearm)
                return APUSR_FRAMEGEN_OK;
            (void)signalCompletionWithoutReplay(*context, completionValue);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
        // Compatibility recovery for devices without maintenance1: legally
        // transition and present the acquired image before surfacing failure.
        if (submitAcquiredImageRetirement(*context, targetImageIndex, completionValue) !=
            APUSR_FRAMEGEN_OK)
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        *outImageIndex = targetImageIndex;
        *outReadySemaphore = context->sourceReadySemaphores[targetImageIndex];
        return APUSR_FRAMEGEN_OK;
    }

    std::array<vk::Semaphore, 66u> waits{};
    std::array<vk::PipelineStageFlags, 66u> waitStages{};
    std::array<std::uint64_t, 66u> waitValues{};
    std::uint32_t waitCount = 0u;
    waits[waitCount] = context->acquireSemaphore;
    waitStages[waitCount] = vk::PipelineStageTransferBit;
    waitValues[waitCount] = 0u;
    ++waitCount;
    for (std::uint32_t i = 0u; i < producerWaitSemaphoreCount; ++i) {
        waits[waitCount] = reinterpret_cast<vk::Semaphore>(producerWaitSemaphores[i]);
        waitStages[waitCount] = vk::PipelineStageTransferBit;
        waitValues[waitCount] = 0u;
        ++waitCount;
    }
    if (upstreamWaitSemaphore) {
        waits[waitCount] = reinterpret_cast<vk::Semaphore>(upstreamWaitSemaphore);
        waitStages[waitCount] = vk::PipelineStageTransferBit;
        waitValues[waitCount] = upstreamWaitValue;
        ++waitCount;
    }

    std::array<vk::Semaphore, 2u> signals{};
    std::array<std::uint64_t, 2u> signalValues{};
    std::uint32_t signalCount = 0u;
    signals[signalCount] = context->sourceReadySemaphores[targetImageIndex];
    signalValues[signalCount] = 0u;
    ++signalCount;
    signals[signalCount] = context->sourceConsumedTimeline;
    signalValues[signalCount] = completionValue;
    ++signalCount;

    const vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .sType = vk::StructureTypeTimelineSemaphoreSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreValueCount = waitCount,
        .pWaitSemaphoreValues = waitValues.data(),
        .signalSemaphoreValueCount = signalCount,
        .pSignalSemaphoreValues = signalValues.data(),
    };
    const vk::SubmitInfo submit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = &timelineInfo,
        .waitSemaphoreCount = waitCount,
        .pWaitSemaphores = waits.data(),
        .pWaitDstStageMask = waitStages.data(),
        .commandBufferCount = 1u,
        .pCommandBuffers = &context->commandBuffer,
        .signalSemaphoreCount = signalCount,
        .pSignalSemaphores = signals.data(),
    };
    const auto submitStart = std::chrono::steady_clock::now();
    if (context->functions.queueSubmit(
            reinterpret_cast<vk::Queue>(context->description.queue), 1u, &submit, context->completionFence) != vk::Success) {
        if (outMetrics) {
            outMetrics->submitUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - submitStart).count());
        }
        if (releaseAcquiredImageAfterColdFailure(*context, targetImageIndex)) {
            if (nonblockingSourcePrearm)
                return APUSR_FRAMEGEN_OK;
            (void)signalCompletionWithoutReplay(*context, completionValue);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
        if (submitAcquiredImageRetirement(*context, targetImageIndex, completionValue) !=
            APUSR_FRAMEGEN_OK)
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        *outImageIndex = targetImageIndex;
        *outReadySemaphore = context->sourceReadySemaphores[targetImageIndex];
        return APUSR_FRAMEGEN_OK;
    }

    if (outMetrics) {
        outMetrics->submitUs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - submitStart).count());
    }
    context->submitted = true;
    *outImageIndex = targetImageIndex;
    *outReadySemaphore = signals[0];
    *outCopied = true;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenPrepareVulkanReplay(
    ApusrFramegenVulkanSourceReplayContext* context,
    const std::uint32_t sourceSlotIndex,
    const std::uint64_t completionValue,
    void* const* producerWaitSemaphores,
    const std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore,
    const std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex,
    void** outReadySemaphore,
    bool* outCopied,
    ApusrFramegenVulkanSourceReplayMetrics* outMetrics) noexcept
{
    return prepareVulkanReplayImpl(
        context, sourceSlotIndex, completionValue,
        producerWaitSemaphores, producerWaitSemaphoreCount,
        upstreamWaitSemaphore, upstreamWaitValue, false,
        outImageIndex, outReadySemaphore, outCopied, outMetrics);
}

ApusrFramegenResult apusrFramegenTryPrepareVulkanSourceReplay(
    ApusrFramegenVulkanSourceReplayContext* context,
    const std::uint32_t sourceSlotIndex,
    const std::uint64_t completionValue,
    void* const* producerWaitSemaphores,
    const std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore,
    const std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex,
    void** outReadySemaphore,
    bool* outPrepared,
    ApusrFramegenVulkanSourceReplayMetrics* outMetrics) noexcept
{
    if (!outImageIndex || !outReadySemaphore || !outPrepared)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outPrepared = false;
    bool copied = false;
    const ApusrFramegenResult result = prepareVulkanReplayImpl(
        context, sourceSlotIndex, completionValue,
        producerWaitSemaphores, producerWaitSemaphoreCount,
        upstreamWaitSemaphore, upstreamWaitValue, true,
        outImageIndex, outReadySemaphore, &copied, outMetrics);
    *outPrepared = result == APUSR_FRAMEGEN_OK && copied && *outReadySemaphore != nullptr;
    return result == APUSR_FRAMEGEN_ERROR_BUSY ? APUSR_FRAMEGEN_OK : result;
}

void apusrFramegenDestroyVulkanSourceReplayContext(
    ApusrFramegenVulkanSourceReplayContext* context,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!context)
        return;
    destroyContents(*context, allocator);
    allocator.destroy(context);
}
