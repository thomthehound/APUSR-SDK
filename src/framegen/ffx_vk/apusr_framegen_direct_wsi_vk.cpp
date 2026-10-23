// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_direct_wsi_vk.h"

#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"

#include <mutex>
#include <new>

namespace {
namespace vk = apusr::backends::vk;

constexpr std::int32_t kVkSuccess = 0;
constexpr std::int32_t kVkNotReady = 1;
constexpr std::int32_t kVkTimeout = 2;
constexpr std::int32_t kVkSuboptimalKhr = 1000001003;
constexpr std::uint32_t kNoImage = 0xffffffffu;

struct Functions {
    vk::PfnGetDeviceProcAddr getDeviceProcAddr = nullptr;
    vk::PfnCreateSemaphore createSemaphore = nullptr;
    vk::PfnDestroySemaphore destroySemaphore = nullptr;
    vk::PfnGetSemaphoreCounterValue getSemaphoreCounterValue = nullptr;
    vk::PfnQueueSubmit queueSubmit = nullptr;
    vk::PfnQueueWaitIdle queueWaitIdle = nullptr;
};

template <typename Fn>
bool loadFunction(
    const ApusrFramegenVulkanDeviceContext& deviceContext,
    Functions& functions,
    const char* name,
    Fn& out) noexcept
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
    APUSR_LOAD_VK("vkCreateSemaphore", createSemaphore);
    APUSR_LOAD_VK("vkDestroySemaphore", destroySemaphore);
    APUSR_LOAD_VK("vkGetSemaphoreCounterValue", getSemaphoreCounterValue);
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

struct ApusrFramegenVulkanDirectWsiContext {
    ApusrFramegenVulkanDirectWsiDescription description{};
    Functions functions{};
    vk::Semaphore acquireSemaphore = nullptr;
    vk::Semaphore* presentReadySemaphores = nullptr;
    std::uint32_t activeImageIndex = kNoImage;
    void* deferredTimeline = nullptr;
    std::uint64_t deferredValue = 0u;
    // Acquisition lives on the game thread while presentation/release lives on
    // the presenter thread. Keep one lock around the tiny ownership state and
    // bridge submit. Acquisition/reclaim use try_lock and therefore stay
    // strictly fail-open rather than blocking the game's present path.
    std::mutex mutex;
};

namespace {

void reclaimCompletedLocked(ApusrFramegenVulkanDirectWsiContext& context) noexcept
{
    if (context.activeImageIndex == kNoImage || !context.deferredTimeline || context.deferredValue == 0u)
        return;
    std::uint64_t completed = 0u;
    const auto device = reinterpret_cast<vk::Device>(context.description.deviceContext.device);
    if (context.functions.getSemaphoreCounterValue(
            device,
            reinterpret_cast<vk::Semaphore>(context.deferredTimeline),
            &completed) != vk::Success || completed < context.deferredValue)
        return;

    if (context.description.releaseImage(
            context.description.userContext,
            context.description.device,
            context.description.swapchain,
            context.activeImageIndex)) {
        context.activeImageIndex = kNoImage;
        context.deferredTimeline = nullptr;
        context.deferredValue = 0u;
    }
}

void destroyContents(
    ApusrFramegenVulkanDirectWsiContext& context,
    const ApusrFramegenAllocator& allocator) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(context.description.deviceContext.device);
    const auto queue = reinterpret_cast<vk::Queue>(context.description.bridgeQueue);
    if (queue && context.functions.queueWaitIdle)
        (void)context.functions.queueWaitIdle(queue);

    if (context.activeImageIndex != kNoImage && context.description.releaseImage) {
        // Device teardown/swapchain teardown is cold and has already drained the
        // bridge queue. Best effort avoids leaking a privately acquired image.
        (void)context.description.releaseImage(
            context.description.userContext,
            context.description.device,
            context.description.swapchain,
            context.activeImageIndex);
        context.activeImageIndex = kNoImage;
    }
    if (context.presentReadySemaphores) {
        for (std::uint32_t i = 0u; i < context.description.presentImageCount; ++i) {
            if (context.presentReadySemaphores[i])
                context.functions.destroySemaphore(device, context.presentReadySemaphores[i], nullptr);
        }
        allocator.dealloc(context.presentReadySemaphores);
        context.presentReadySemaphores = nullptr;
    }
    if (context.acquireSemaphore) {
        context.functions.destroySemaphore(device, context.acquireSemaphore, nullptr);
        context.acquireSemaphore = nullptr;
    }
}

} // namespace

ApusrFramegenResult apusrFramegenCreateVulkanDirectWsiContext(
    const ApusrFramegenVulkanDirectWsiDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanDirectWsiContext** outContext) noexcept
{
    if (!outContext)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outContext = nullptr;
    if (!description.deviceContext.device || !description.bridgeQueue || !description.abortQueue || !description.device ||
        !description.swapchain || description.presentImageCount < 2u ||
        !description.acquireImage || !description.releaseImage)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    auto* context = allocator.construct<ApusrFramegenVulkanDirectWsiContext>();
    if (!context)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    context->description = description;
    if (!loadFunctions(description.deviceContext, context->functions)) {
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;
    }

    const auto device = reinterpret_cast<vk::Device>(description.deviceContext.device);
    const vk::SemaphoreCreateInfo semaphoreInfo{
        .sType = vk::StructureTypeSemaphoreCreateInfo,
        .pNext = nullptr,
        .flags = 0u,
    };
    if (context->functions.createSemaphore(device, &semaphoreInfo, nullptr, &context->acquireSemaphore) != vk::Success ||
        !context->acquireSemaphore) {
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    context->presentReadySemaphores = static_cast<vk::Semaphore*>(
        allocator.alloc(sizeof(vk::Semaphore) * description.presentImageCount));
    if (!context->presentReadySemaphores) {
        destroyContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    for (std::uint32_t i = 0u; i < description.presentImageCount; ++i)
        context->presentReadySemaphores[i] = nullptr;
    for (std::uint32_t i = 0u; i < description.presentImageCount; ++i) {
        if (context->functions.createSemaphore(
                device, &semaphoreInfo, nullptr, &context->presentReadySemaphores[i]) != vk::Success ||
            !context->presentReadySemaphores[i]) {
            destroyContents(*context, allocator);
            allocator.destroy(context);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
    }

    *outContext = context;
    return APUSR_FRAMEGEN_OK;
}

void apusrFramegenReclaimCompletedVulkanDirectWsiTarget(
    ApusrFramegenVulkanDirectWsiContext* context) noexcept
{
    if (!context)
        return;
    std::unique_lock<std::mutex> lock(context->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    reclaimCompletedLocked(*context);
}

ApusrFramegenResult apusrFramegenTryAcquireVulkanDirectWsiTarget(
    ApusrFramegenVulkanDirectWsiContext* context,
    std::uint32_t* outImageIndex,
    void** outAcquireSemaphore,
    bool* outAcquired) noexcept
{
    if (!context || !outImageIndex || !outAcquireSemaphore || !outAcquired)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outImageIndex = kNoImage;
    *outAcquireSemaphore = nullptr;
    *outAcquired = false;

    std::unique_lock<std::mutex> lock(context->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return APUSR_FRAMEGEN_OK;
    reclaimCompletedLocked(*context);
    if (context->activeImageIndex != kNoImage)
        return APUSR_FRAMEGEN_OK;

    std::uint32_t imageIndex = kNoImage;
    const std::int32_t result = context->description.acquireImage(
        context->description.userContext,
        context->description.device,
        context->description.swapchain,
        0u,
        context->acquireSemaphore,
        &imageIndex);
    if (result == kVkNotReady || result == kVkTimeout)
        return APUSR_FRAMEGEN_OK;
    if (!acceptedAcquire(result) || imageIndex >= context->description.presentImageCount)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    context->activeImageIndex = imageIndex;
    context->deferredTimeline = nullptr;
    context->deferredValue = 0u;
    *outImageIndex = imageIndex;
    *outAcquireSemaphore = context->acquireSemaphore;
    *outAcquired = true;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenPrepareVulkanDirectWsiPresent(
    ApusrFramegenVulkanDirectWsiContext* context,
    const std::uint32_t imageIndex,
    void* generationTimelineSemaphore,
    const std::uint64_t generationTimelineValue,
    void** outPresentWaitSemaphore) noexcept
{
    if (!context || !generationTimelineSemaphore || !outPresentWaitSemaphore)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outPresentWaitSemaphore = nullptr;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (imageIndex >= context->description.presentImageCount || imageIndex != context->activeImageIndex ||
        generationTimelineValue == 0u || context->deferredTimeline)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    const vk::Semaphore wait = reinterpret_cast<vk::Semaphore>(generationTimelineSemaphore);
    const vk::Semaphore signal = context->presentReadySemaphores[imageIndex];
    const vk::PipelineStageFlags waitStage = vk::PipelineStageAllCommandsBit;
    const std::uint64_t waitValue = generationTimelineValue;
    const std::uint64_t signalValue = 0u;
    const vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .sType = vk::StructureTypeTimelineSemaphoreSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreValueCount = 1u,
        .pWaitSemaphoreValues = &waitValue,
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signalValue,
    };
    const vk::SubmitInfo submit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = &timelineInfo,
        .waitSemaphoreCount = 1u,
        .pWaitSemaphores = &wait,
        .pWaitDstStageMask = &waitStage,
        .commandBufferCount = 0u,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal,
    };
    if (context->functions.queueSubmit(
            reinterpret_cast<vk::Queue>(context->description.bridgeQueue), 1u, &submit, nullptr) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    *outPresentWaitSemaphore = signal;
    return APUSR_FRAMEGEN_OK;
}

void apusrFramegenMarkVulkanDirectWsiPresented(
    ApusrFramegenVulkanDirectWsiContext* context,
    const std::uint32_t imageIndex) noexcept
{
    if (!context)
        return;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (imageIndex != context->activeImageIndex)
        return;
    context->activeImageIndex = kNoImage;
    context->deferredTimeline = nullptr;
    context->deferredValue = 0u;
}

void apusrFramegenDeferVulkanDirectWsiRelease(
    ApusrFramegenVulkanDirectWsiContext* context,
    const std::uint32_t imageIndex,
    void* generationTimelineSemaphore,
    const std::uint64_t generationTimelineValue) noexcept
{
    if (!context || !generationTimelineSemaphore || generationTimelineValue == 0u)
        return;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (imageIndex != context->activeImageIndex)
        return;
    context->deferredTimeline = generationTimelineSemaphore;
    context->deferredValue = generationTimelineValue;
    reclaimCompletedLocked(*context);
}

void apusrFramegenAbortVulkanDirectWsiAcquire(
    ApusrFramegenVulkanDirectWsiContext* context,
    const std::uint32_t imageIndex) noexcept
{
    if (!context)
        return;
    std::lock_guard<std::mutex> lock(context->mutex);
    if (imageIndex != context->activeImageIndex)
        return;

    // vkAcquireNextImageKHR has already signaled the binary semaphore. Consume
    // it before reusing/destroying it, then release the image. This is a cold
    // backend-failure path, so waiting the private bridge queue is preferable
    // to carrying ambiguous semaphore state into the next frame.
    const vk::Semaphore wait = context->acquireSemaphore;
    const vk::PipelineStageFlags waitStage = vk::PipelineStageAllCommandsBit;
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
    const auto queue = reinterpret_cast<vk::Queue>(context->description.abortQueue);
    if (context->functions.queueSubmit(queue, 1u, &submit, nullptr) == vk::Success)
        (void)context->functions.queueWaitIdle(queue);
    (void)context->description.releaseImage(
        context->description.userContext,
        context->description.device,
        context->description.swapchain,
        imageIndex);
    context->activeImageIndex = kNoImage;
    context->deferredTimeline = nullptr;
    context->deferredValue = 0u;
}

void apusrFramegenDestroyVulkanDirectWsiContext(
    ApusrFramegenVulkanDirectWsiContext* context,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!context)
        return;
    destroyContents(*context, allocator);
    allocator.destroy(context);
}
