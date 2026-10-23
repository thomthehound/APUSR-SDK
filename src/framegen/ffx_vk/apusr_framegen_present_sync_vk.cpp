// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_present_sync_vk.h"

#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"

#include <apusr/apusr_profiler.h>

#include <array>
#include <new>

namespace {
namespace vk = apusr::backends::vk;

constexpr std::uint32_t kMaxPresentWaitSemaphores = 64u;
constexpr vk::PipelineStageFlags kInputDependencyStages =
    vk::PipelineStageTransferBit | vk::PipelineStageComputeShaderBit;
constexpr vk::PipelineStageFlags kGenerationDependencyStage = vk::PipelineStageComputeShaderBit;

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
    vk::PfnWaitSemaphores waitSemaphores = nullptr;
    vk::PfnQueueSubmit queueSubmit = nullptr;
    vk::PfnQueueWaitIdle queueWaitIdle = nullptr;
};

struct Slot {
    vk::CommandBuffer inputCommandBuffer = nullptr;
    vk::CommandBuffer generationCommandBuffer = nullptr;
    std::uint64_t lastGenerationCompletionValue = 0u;
    bool recording = false;
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
    APUSR_LOAD_VK("vkWaitSemaphores", waitSemaphores);
    APUSR_LOAD_VK("vkQueueSubmit", queueSubmit);
    APUSR_LOAD_VK("vkQueueWaitIdle", queueWaitIdle);
#undef APUSR_LOAD_VK
    return true;
}

} // namespace

struct ApusrFramegenVulkanPresentSubmitContext {
    ApusrFramegenVulkanPresentSubmitDescription description{};
    Functions functions{};
    vk::CommandPool inputCommandPool = nullptr;
    // Null means generation command buffers were allocated from inputCommandPool.
    vk::CommandPool generationCommandPool = nullptr;
    Slot* slots = nullptr;
    vk::Semaphore producerReadyTimeline = nullptr;
    vk::Semaphore inputCompletionTimeline = nullptr;
    vk::Semaphore generationCompletionTimeline = nullptr;
    std::uint64_t lastProducerReadyValue = 0u;
    std::uint64_t lastInputCompletionValue = 0u;
    std::uint64_t lastGenerationCompletionValue = 0u;
    apusr::profile::StreamHandle profileStream{};
    apusr::profile::SampleGate profileSampleGate{};
    std::uint64_t profileSubmissionSequence = 0u;
};

namespace {

struct QueueProfileCapture final {
    apusr::profile::StreamHandle stream{};
    std::uint64_t frameId = 0u;
    std::uint64_t sampleSequence = 0u;
    std::uint32_t slotIndex = 0u;

    void event(
        const char* name,
        const vk::Queue queue,
        const apusr::profile::QueueOperation operation,
        const vk::Semaphore semaphore = nullptr,
        const std::uint64_t value = 0u,
        const vk::Queue relatedQueue = nullptr,
        const std::uint32_t stageMask = 0u,
        const std::uint32_t objectCount = 0u) const noexcept
    {
        if (!stream || !queue)
            return;
        apusr::profile::recordQueueEvent(apusr::profile::QueueEvent{
            .stream = stream,
            .frameId = frameId,
            .sampleSequence = sampleSequence,
            .name = name,
            .nativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(queue)),
            .syncObject = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(semaphore)),
            .syncValue = value,
            .relatedQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(relatedQueue)),
            .submitSequence = frameId,
            .operation = operation,
            .stageMask = stageMask,
            .objectCount = objectCount,
            .flags = slotIndex,
        });
    }
};

[[nodiscard]] QueueProfileCapture beginQueueProfile(
    ApusrFramegenVulkanPresentSubmitContext& context,
    const std::uint32_t slotIndex,
    const std::uint64_t topologyTag) noexcept
{
    if (!context.profileStream)
        return {};
    const std::uint64_t frameId = ++context.profileSubmissionSequence;
    std::uint64_t hash = 0x41505553564b5155ull; // "APUSVKQU"
    hash = apusr::profile::hashCombine(hash, topologyTag);
    hash = apusr::profile::hashCombine(hash, context.description.inputQueueFamilyIndex);
    hash = apusr::profile::hashCombine(hash, context.description.generationQueueFamilyIndex);
    hash = apusr::profile::hashCombine(
        hash, context.description.inputQueue == context.description.generationQueue ? 1u : 0u);
    const apusr::profile::SampleDecision decision = context.profileSampleGate.decide(hash, frameId);
    return QueueProfileCapture{
        .stream = decision.capture ? context.profileStream : apusr::profile::StreamHandle{},
        .frameId = frameId,
        .sampleSequence = decision.sampleSequence,
        .slotIndex = slotIndex,
    };
}

void recordSubmit(
    const QueueProfileCapture& profile,
    const char* name,
    const vk::Queue queue,
    const vk::Queue relatedQueue,
    const vk::SubmitInfo& submit,
    const vk::TimelineSemaphoreSubmitInfo* timeline) noexcept
{
    if (!profile.stream)
        return;
    profile.event(
        name, queue, apusr::profile::QueueOperation::Submit, nullptr, 0u, relatedQueue, 0u,
        submit.commandBufferCount);
    for (std::uint32_t i = 0u; i < submit.waitSemaphoreCount; ++i) {
        const std::uint64_t value = timeline && i < timeline->waitSemaphoreValueCount && timeline->pWaitSemaphoreValues
            ? timeline->pWaitSemaphoreValues[i]
            : 0u;
        const std::uint32_t stageMask = submit.pWaitDstStageMask ? submit.pWaitDstStageMask[i] : 0u;
        profile.event(
            "queue.wait", queue, apusr::profile::QueueOperation::Wait,
            submit.pWaitSemaphores[i], value, relatedQueue, stageMask);
    }
    for (std::uint32_t i = 0u; i < submit.signalSemaphoreCount; ++i) {
        const std::uint64_t value = timeline && i < timeline->signalSemaphoreValueCount && timeline->pSignalSemaphoreValues
            ? timeline->pSignalSemaphoreValues[i]
            : 0u;
        profile.event(
            "queue.signal", queue, apusr::profile::QueueOperation::Signal,
            submit.pSignalSemaphores[i], value, relatedQueue);
    }
}

void destroyContextContents(ApusrFramegenVulkanPresentSubmitContext& context, const ApusrFramegenAllocator& allocator) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(context.description.deviceContext.device);
    const auto inputQueue = reinterpret_cast<vk::Queue>(context.description.inputQueue);
    const auto generationQueue = reinterpret_cast<vk::Queue>(context.description.generationQueue);
    if (generationQueue && context.functions.queueWaitIdle)
        (void)context.functions.queueWaitIdle(generationQueue);
    if (inputQueue && inputQueue != generationQueue && context.functions.queueWaitIdle)
        (void)context.functions.queueWaitIdle(inputQueue);

    if (context.producerReadyTimeline) {
        context.functions.destroySemaphore(device, context.producerReadyTimeline, nullptr);
        context.producerReadyTimeline = nullptr;
    }
    if (context.inputCompletionTimeline) {
        context.functions.destroySemaphore(device, context.inputCompletionTimeline, nullptr);
        context.inputCompletionTimeline = nullptr;
    }
    if (context.generationCompletionTimeline) {
        context.functions.destroySemaphore(device, context.generationCompletionTimeline, nullptr);
        context.generationCompletionTimeline = nullptr;
    }
    if (context.slots) {
        allocator.dealloc(context.slots);
        context.slots = nullptr;
    }
    if (context.generationCommandPool) {
        context.functions.destroyCommandPool(device, context.generationCommandPool, nullptr);
        context.generationCommandPool = nullptr;
    }
    if (context.inputCommandPool) {
        context.functions.destroyCommandPool(device, context.inputCommandPool, nullptr);
        context.inputCommandPool = nullptr;
    }
}

bool createTimelineSemaphore(
    ApusrFramegenVulkanPresentSubmitContext& context,
    vk::Semaphore& outSemaphore) noexcept
{
    const vk::SemaphoreTypeCreateInfo timelineTypeInfo{
        .sType = vk::StructureTypeSemaphoreTypeCreateInfo,
        .pNext = nullptr,
        .semaphoreType = vk::SemaphoreTypeTimeline,
        .initialValue = 0u,
    };
    const vk::SemaphoreCreateInfo semaphoreInfo{
        .sType = vk::StructureTypeSemaphoreCreateInfo,
        .pNext = &timelineTypeInfo,
        .flags = 0u,
    };
    const auto device = reinterpret_cast<vk::Device>(context.description.deviceContext.device);
    return context.functions.createSemaphore(device, &semaphoreInfo, nullptr, &outSemaphore) == vk::Success && outSemaphore;
}

ApusrFramegenResult allocateCommands(
    ApusrFramegenVulkanPresentSubmitContext& context,
    const ApusrFramegenAllocator& allocator) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(context.description.deviceContext.device);
    const std::uint32_t count = context.description.slotCount;
    auto* inputCommands = static_cast<vk::CommandBuffer*>(allocator.alloc(sizeof(vk::CommandBuffer) * count));
    auto* generationCommands = static_cast<vk::CommandBuffer*>(allocator.alloc(sizeof(vk::CommandBuffer) * count));
    if (!inputCommands || !generationCommands) {
        if (inputCommands) allocator.dealloc(inputCommands);
        if (generationCommands) allocator.dealloc(generationCommands);
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }

    const vk::CommandBufferAllocateInfo inputAllocateInfo{
        .sType = vk::StructureTypeCommandBufferAllocateInfo,
        .pNext = nullptr,
        .commandPool = context.inputCommandPool,
        .level = vk::CommandBufferLevelPrimary,
        .commandBufferCount = count,
    };
    const vk::CommandBufferAllocateInfo generationAllocateInfo{
        .sType = vk::StructureTypeCommandBufferAllocateInfo,
        .pNext = nullptr,
        .commandPool = context.generationCommandPool ? context.generationCommandPool : context.inputCommandPool,
        .level = vk::CommandBufferLevelPrimary,
        .commandBufferCount = count,
    };
    const bool ok =
        context.functions.allocateCommandBuffers(device, &inputAllocateInfo, inputCommands) == vk::Success &&
        context.functions.allocateCommandBuffers(device, &generationAllocateInfo, generationCommands) == vk::Success;
    if (ok) {
        for (std::uint32_t i = 0u; i < count; ++i) {
            context.slots[i].inputCommandBuffer = inputCommands[i];
            context.slots[i].generationCommandBuffer = generationCommands[i];
        }
    }
    allocator.dealloc(inputCommands);
    allocator.dealloc(generationCommands);
    return ok ? APUSR_FRAMEGEN_OK : APUSR_FRAMEGEN_ERROR_BACKEND_API;
}

} // namespace

ApusrFramegenResult apusrFramegenCreateVulkanPresentSubmitContext(
    const ApusrFramegenVulkanPresentSubmitDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanPresentSubmitContext** outContext) noexcept
{
    if (!outContext)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outContext = nullptr;
    if (!description.deviceContext.device || !description.inputQueue || !description.generationQueue ||
        description.slotCount == 0u)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    auto* context = allocator.construct<ApusrFramegenVulkanPresentSubmitContext>();
    if (!context)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    context->description = description;
    if (!loadFunctions(description.deviceContext, context->functions)) {
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;
    }
    context->profileStream = apusr::profile::registerStream(apusr::profile::StreamDesc{
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::FrameGeneration,
        .name = "framegen.present_sync",
        .capabilities = apusr::profile::StreamCapabilityCounters |
            apusr::profile::StreamCapabilityErrors |
            apusr::profile::StreamCapabilityQueueEvents,
        .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(description.deviceContext.device)),
        .nativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(description.generationQueue)),
        .queueFamily = description.generationQueueFamilyIndex,
    });
    context->profileSampleGate.reset();
    if (context->profileStream) {
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = context->profileStream,
            .name = "queue.topology",
            .value = description.inputQueueFamilyIndex,
            .value2 = description.generationQueueFamilyIndex,
            .flags = description.inputQueue == description.generationQueue ? 1u : 0u,
        });
    }

    const auto device = reinterpret_cast<vk::Device>(description.deviceContext.device);
    const vk::CommandPoolCreateInfo inputPoolInfo{
        .sType = vk::StructureTypeCommandPoolCreateInfo,
        .pNext = nullptr,
        .flags = vk::CommandPoolCreateResetCommandBufferBit,
        .queueFamilyIndex = description.inputQueueFamilyIndex,
    };
    if (context->functions.createCommandPool(
            device, &inputPoolInfo, nullptr, &context->inputCommandPool) != vk::Success || !context->inputCommandPool) {
        destroyContextContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    if (description.generationQueueFamilyIndex != description.inputQueueFamilyIndex) {
        const vk::CommandPoolCreateInfo generationPoolInfo{
            .sType = vk::StructureTypeCommandPoolCreateInfo,
            .pNext = nullptr,
            .flags = vk::CommandPoolCreateResetCommandBufferBit,
            .queueFamilyIndex = description.generationQueueFamilyIndex,
        };
        if (context->functions.createCommandPool(
                device, &generationPoolInfo, nullptr, &context->generationCommandPool) != vk::Success ||
            !context->generationCommandPool) {
            destroyContextContents(*context, allocator);
            allocator.destroy(context);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
    }

    context->slots = static_cast<Slot*>(allocator.alloc(sizeof(Slot) * description.slotCount));
    if (!context->slots) {
        destroyContextContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    for (std::uint32_t i = 0u; i < description.slotCount; ++i)
        ::new (&context->slots[i]) Slot{};

    const ApusrFramegenResult commandResult = allocateCommands(*context, allocator);
    if (commandResult != APUSR_FRAMEGEN_OK ||
        !createTimelineSemaphore(*context, context->producerReadyTimeline) ||
        !createTimelineSemaphore(*context, context->inputCompletionTimeline) ||
        !createTimelineSemaphore(*context, context->generationCompletionTimeline)) {
        destroyContextContents(*context, allocator);
        allocator.destroy(context);
        return commandResult != APUSR_FRAMEGEN_OK ? commandResult : APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    *outContext = context;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenBeginVulkanSplitPresentSubmit(
    ApusrFramegenVulkanPresentSubmitContext* context,
    const std::uint32_t slotIndex,
    const std::uint64_t completedGenerationValue,
    void** outInputCommandBuffer,
    void** outGenerationCommandBuffer) noexcept
{
    if (!context || !outInputCommandBuffer || !outGenerationCommandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outInputCommandBuffer = nullptr;
    *outGenerationCommandBuffer = nullptr;
    if (slotIndex >= context->description.slotCount)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    Slot& slot = context->slots[slotIndex];
    if (slot.recording)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    if (slot.lastGenerationCompletionValue > completedGenerationValue)
        return APUSR_FRAMEGEN_ERROR_BUSY;

    if (context->functions.resetCommandBuffer(slot.inputCommandBuffer, 0u) != vk::Success ||
        context->functions.resetCommandBuffer(slot.generationCommandBuffer, 0u) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    const vk::CommandBufferBeginInfo beginInfo{
        .sType = vk::StructureTypeCommandBufferBeginInfo,
        .pNext = nullptr,
        .flags = vk::CommandBufferUsageOneTimeSubmitBit,
        .pInheritanceInfo = nullptr,
    };
    if (context->functions.beginCommandBuffer(slot.inputCommandBuffer, &beginInfo) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    if (context->functions.beginCommandBuffer(slot.generationCommandBuffer, &beginInfo) != vk::Success) {
        (void)context->functions.resetCommandBuffer(slot.inputCommandBuffer, 0u);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    slot.recording = true;
    *outInputCommandBuffer = slot.inputCommandBuffer;
    *outGenerationCommandBuffer = slot.generationCommandBuffer;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenSubmitVulkanSplitOffscreenPresentWork(
    ApusrFramegenVulkanPresentSubmitContext* context,
    const std::uint32_t slotIndex,
    void* const* applicationWaitSemaphores,
    const std::uint32_t applicationWaitSemaphoreCount,
    void* generatedReuseTimelineSemaphore,
    const std::uint64_t generatedReuseTimelineValue,
    const std::uint64_t priorGenerationCompletionValue,
    void* directTargetAcquireSemaphore,
    std::uint64_t* outProducerReadyValue,
    std::uint64_t* outInputCompletionValue,
    std::uint64_t* outGenerationCompletionValue) noexcept
{
    if (!context || !outProducerReadyValue || !outInputCompletionValue || !outGenerationCompletionValue)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outProducerReadyValue = 0u;
    *outInputCompletionValue = 0u;
    *outGenerationCompletionValue = 0u;
    if (slotIndex >= context->description.slotCount ||
        applicationWaitSemaphoreCount > kMaxPresentWaitSemaphores ||
        (applicationWaitSemaphoreCount != 0u && !applicationWaitSemaphores) ||
        (generatedReuseTimelineValue != 0u && !generatedReuseTimelineSemaphore) ||
        !context->producerReadyTimeline || !context->inputCompletionTimeline || !context->generationCompletionTimeline ||
        context->lastProducerReadyValue == ~std::uint64_t{0} ||
        context->lastInputCompletionValue == ~std::uint64_t{0} ||
        context->lastGenerationCompletionValue == ~std::uint64_t{0})
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    Slot& slot = context->slots[slotIndex];
    if (!slot.recording)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    const QueueProfileCapture profile = beginQueueProfile(*context, slotIndex, 1u);

    if (context->functions.endCommandBuffer(slot.inputCommandBuffer) != vk::Success) {
        slot.recording = false;
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    if (context->functions.endCommandBuffer(slot.generationCommandBuffer) != vk::Success) {
        slot.recording = false;
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    slot.recording = false;

    const std::uint64_t producerReadyValue = context->lastProducerReadyValue + 1u;
    const std::uint64_t inputCompletionValue = context->lastInputCompletionValue + 1u;
    const std::uint64_t generationCompletionValue = context->lastGenerationCompletionValue + 1u;

    std::array<vk::Semaphore, kMaxPresentWaitSemaphores> producerWaits{};
    std::array<vk::PipelineStageFlags, kMaxPresentWaitSemaphores> producerWaitStages{};
    std::array<std::uint64_t, kMaxPresentWaitSemaphores> producerWaitValues{};
    for (std::uint32_t i = 0u; i < applicationWaitSemaphoreCount; ++i) {
        producerWaits[i] = reinterpret_cast<vk::Semaphore>(applicationWaitSemaphores[i]);
        if (!producerWaits[i])
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        producerWaitStages[i] = kInputDependencyStages;
        producerWaitValues[i] = 0u;
    }

    const vk::Semaphore producerSignal = context->producerReadyTimeline;
    const vk::TimelineSemaphoreSubmitInfo producerTimelineInfo{
        .sType = vk::StructureTypeTimelineSemaphoreSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreValueCount = applicationWaitSemaphoreCount,
        .pWaitSemaphoreValues = applicationWaitSemaphoreCount ? producerWaitValues.data() : nullptr,
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &producerReadyValue,
    };
    const vk::SubmitInfo producerSubmit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = &producerTimelineInfo,
        .waitSemaphoreCount = applicationWaitSemaphoreCount,
        .pWaitSemaphores = applicationWaitSemaphoreCount ? producerWaits.data() : nullptr,
        .pWaitDstStageMask = applicationWaitSemaphoreCount ? producerWaitStages.data() : nullptr,
        .commandBufferCount = 0u,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &producerSignal,
    };

    std::array<vk::Semaphore, 2u> inputWaits{};
    std::array<vk::PipelineStageFlags, 2u> inputWaitStages{};
    std::array<std::uint64_t, 2u> inputWaitValues{};
    std::uint32_t inputWaitCount = 0u;
    inputWaits[inputWaitCount] = context->producerReadyTimeline;
    inputWaitStages[inputWaitCount] = kInputDependencyStages;
    inputWaitValues[inputWaitCount] = producerReadyValue;
    ++inputWaitCount;

    const bool crossQueue = context->description.inputQueue != context->description.generationQueue;
    if (crossQueue && priorGenerationCompletionValue != 0u) {
        // The current two-slot FI history makes Phase A(N+1) a writer of the
        // history that Phase B(N) can still be reading. Queue the new work
        // instead of discarding it: source replay is already independently
        // armed from producerReady, while only the history-mutating Phase A
        // waits for the preceding FI/OF dispatch to finish.
        inputWaits[inputWaitCount] = context->generationCompletionTimeline;
        inputWaitStages[inputWaitCount] = kInputDependencyStages;
        inputWaitValues[inputWaitCount] = priorGenerationCompletionValue;
        ++inputWaitCount;
    }

    const vk::Semaphore inputSignal = context->inputCompletionTimeline;
    const vk::TimelineSemaphoreSubmitInfo inputTimelineInfo{
        .sType = vk::StructureTypeTimelineSemaphoreSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreValueCount = inputWaitCount,
        .pWaitSemaphoreValues = inputWaitValues.data(),
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &inputCompletionValue,
    };
    const vk::SubmitInfo inputSubmit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = &inputTimelineInfo,
        .waitSemaphoreCount = inputWaitCount,
        .pWaitSemaphores = inputWaits.data(),
        .pWaitDstStageMask = inputWaitStages.data(),
        .commandBufferCount = 1u,
        .pCommandBuffers = &slot.inputCommandBuffer,
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &inputSignal,
    };

    std::array<vk::Semaphore, 3u> generationWaits{};
    std::array<vk::PipelineStageFlags, 3u> generationWaitStages{};
    std::array<std::uint64_t, 3u> generationWaitValues{};
    std::uint32_t generationWaitCount = 0u;
    if (crossQueue) {
        generationWaits[generationWaitCount] = context->inputCompletionTimeline;
        generationWaitStages[generationWaitCount] = kGenerationDependencyStage;
        generationWaitValues[generationWaitCount] = inputCompletionValue;
        ++generationWaitCount;
    }
    if (generatedReuseTimelineValue != 0u) {
        generationWaits[generationWaitCount] = reinterpret_cast<vk::Semaphore>(generatedReuseTimelineSemaphore);
        generationWaitStages[generationWaitCount] = kGenerationDependencyStage;
        generationWaitValues[generationWaitCount] = generatedReuseTimelineValue;
        ++generationWaitCount;
    }
    if (directTargetAcquireSemaphore) {
        generationWaits[generationWaitCount] = reinterpret_cast<vk::Semaphore>(directTargetAcquireSemaphore);
        generationWaitStages[generationWaitCount] = kGenerationDependencyStage;
        generationWaitValues[generationWaitCount] = 0u;
        ++generationWaitCount;
    }
    const vk::Semaphore generationSignal = context->generationCompletionTimeline;
    const vk::TimelineSemaphoreSubmitInfo generationTimelineInfo{
        .sType = vk::StructureTypeTimelineSemaphoreSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreValueCount = generationWaitCount,
        .pWaitSemaphoreValues = generationWaitCount ? generationWaitValues.data() : nullptr,
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &generationCompletionValue,
    };
    const vk::SubmitInfo generationSubmit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = &generationTimelineInfo,
        .waitSemaphoreCount = generationWaitCount,
        .pWaitSemaphores = generationWaitCount ? generationWaits.data() : nullptr,
        .pWaitDstStageMask = generationWaitCount ? generationWaitStages.data() : nullptr,
        .commandBufferCount = 1u,
        .pCommandBuffers = &slot.generationCommandBuffer,
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &generationSignal,
    };

    const auto inputQueue = reinterpret_cast<vk::Queue>(context->description.inputQueue);
    const auto generationQueue = reinterpret_cast<vk::Queue>(context->description.generationQueue);
    if (!crossQueue) {
        // A common topology: one driver call, three ordered submissions, no
        // same-queue timeline round-trip between A and B.
        const std::array<vk::SubmitInfo, 3u> submits{producerSubmit, inputSubmit, generationSubmit};
        recordSubmit(profile, "present.producer_ready", inputQueue, generationQueue, producerSubmit, &producerTimelineInfo);
        recordSubmit(profile, "present.phase_a", inputQueue, generationQueue, inputSubmit, &inputTimelineInfo);
        recordSubmit(profile, "present.phase_b", generationQueue, inputQueue, generationSubmit, &generationTimelineInfo);
        const vk::Result submitResult = context->functions.queueSubmit(
            inputQueue, static_cast<std::uint32_t>(submits.size()), submits.data(), nullptr);
        if (submitResult != vk::Success) {
            apusr::profile::recordError(
                context->profileStream, profile.frameId, "vkQueueSubmit(framegen.same_queue)",
                apusr::StatusCode::RuntimeFailure,
                "Vulkan FrameGen producer/Phase-A/Phase-B batch submission failed",
                static_cast<std::uint64_t>(static_cast<std::int64_t>(submitResult)), slotIndex);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
    } else {
        // True async compute: only the inter-queue A->B edge uses a timeline.
        const std::array<vk::SubmitInfo, 2u> inputSubmits{producerSubmit, inputSubmit};
        recordSubmit(profile, "present.producer_ready", inputQueue, generationQueue, producerSubmit, &producerTimelineInfo);
        recordSubmit(profile, "present.phase_a", inputQueue, generationQueue, inputSubmit, &inputTimelineInfo);
        const vk::Result inputSubmitResult = context->functions.queueSubmit(
            inputQueue, static_cast<std::uint32_t>(inputSubmits.size()), inputSubmits.data(), nullptr);
        if (inputSubmitResult != vk::Success) {
            apusr::profile::recordError(
                context->profileStream, profile.frameId, "vkQueueSubmit(framegen.phase_a)",
                apusr::StatusCode::RuntimeFailure,
                "Vulkan FrameGen producer/Phase-A queue submission failed",
                static_cast<std::uint64_t>(static_cast<std::int64_t>(inputSubmitResult)), slotIndex);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
        // The producer binary waits have now been consumed and Phase A is in
        // flight even if the following async-compute submit fails. Publish
        // those values immediately so the caller can still retire/present the
        // mandatory source frame without attempting to wait the application
        // binary semaphores a second time.
        context->lastProducerReadyValue = producerReadyValue;
        context->lastInputCompletionValue = inputCompletionValue;
        *outProducerReadyValue = producerReadyValue;
        *outInputCompletionValue = inputCompletionValue;
        recordSubmit(profile, "present.phase_b", generationQueue, inputQueue, generationSubmit, &generationTimelineInfo);
        const vk::Result generationSubmitResult = context->functions.queueSubmit(
            generationQueue, 1u, &generationSubmit, nullptr);
        if (generationSubmitResult != vk::Success) {
            apusr::profile::recordError(
                context->profileStream, profile.frameId, "vkQueueSubmit(framegen.phase_b)",
                apusr::StatusCode::RuntimeFailure,
                "Vulkan FrameGen Phase-B async-compute submission failed",
                static_cast<std::uint64_t>(static_cast<std::int64_t>(generationSubmitResult)), slotIndex);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
    }

    slot.lastGenerationCompletionValue = generationCompletionValue;
    context->lastProducerReadyValue = producerReadyValue;
    context->lastInputCompletionValue = inputCompletionValue;
    context->lastGenerationCompletionValue = generationCompletionValue;
    *outProducerReadyValue = producerReadyValue;
    *outInputCompletionValue = inputCompletionValue;
    *outGenerationCompletionValue = generationCompletionValue;
    return APUSR_FRAMEGEN_OK;
}

void* apusrFramegenVulkanProducerReadySemaphore(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept
{
    return context ? context->producerReadyTimeline : nullptr;
}

std::uint64_t apusrFramegenVulkanNextProducerReadyValue(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept
{
    if (!context || context->lastProducerReadyValue == ~std::uint64_t{0})
        return 0u;
    return context->lastProducerReadyValue + 1u;
}

ApusrFramegenResult apusrFramegenSignalVulkanProducerReadyOnly(
    ApusrFramegenVulkanPresentSubmitContext* context,
    void* const* applicationWaitSemaphores,
    const std::uint32_t applicationWaitSemaphoreCount,
    std::uint64_t* outProducerReadyValue) noexcept
{
    if (!context || !outProducerReadyValue)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outProducerReadyValue = 0u;
    if (applicationWaitSemaphoreCount > kMaxPresentWaitSemaphores ||
        (applicationWaitSemaphoreCount != 0u && !applicationWaitSemaphores) ||
        !context->producerReadyTimeline || context->lastProducerReadyValue == ~std::uint64_t{0})
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    std::array<vk::Semaphore, kMaxPresentWaitSemaphores> waits{};
    std::array<vk::PipelineStageFlags, kMaxPresentWaitSemaphores> waitStages{};
    std::array<std::uint64_t, kMaxPresentWaitSemaphores> waitValues{};
    for (std::uint32_t i = 0u; i < applicationWaitSemaphoreCount; ++i) {
        waits[i] = reinterpret_cast<vk::Semaphore>(applicationWaitSemaphores[i]);
        if (!waits[i])
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        waitStages[i] = kInputDependencyStages;
        waitValues[i] = 0u;
    }

    const std::uint64_t producerReadyValue = context->lastProducerReadyValue + 1u;
    const vk::Semaphore signal = context->producerReadyTimeline;
    const vk::TimelineSemaphoreSubmitInfo timelineInfo{
        .sType = vk::StructureTypeTimelineSemaphoreSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreValueCount = applicationWaitSemaphoreCount,
        .pWaitSemaphoreValues = applicationWaitSemaphoreCount ? waitValues.data() : nullptr,
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &producerReadyValue,
    };
    const vk::SubmitInfo submit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = &timelineInfo,
        .waitSemaphoreCount = applicationWaitSemaphoreCount,
        .pWaitSemaphores = applicationWaitSemaphoreCount ? waits.data() : nullptr,
        .pWaitDstStageMask = applicationWaitSemaphoreCount ? waitStages.data() : nullptr,
        .commandBufferCount = 0u,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal,
    };
    const QueueProfileCapture profile = beginQueueProfile(*context, 0u, 2u);
    const auto inputQueue = reinterpret_cast<vk::Queue>(context->description.inputQueue);
    recordSubmit(
        profile, "present.source_only.producer_ready", inputQueue,
        reinterpret_cast<vk::Queue>(context->description.generationQueue), submit, &timelineInfo);
    const vk::Result submitResult = context->functions.queueSubmit(inputQueue, 1u, &submit, nullptr);
    if (submitResult != vk::Success) {
        apusr::profile::recordError(
            context->profileStream, profile.frameId, "vkQueueSubmit(framegen.source_only)",
            apusr::StatusCode::RuntimeFailure,
            "Vulkan FrameGen source-only producer-ready submission failed",
            static_cast<std::uint64_t>(static_cast<std::int64_t>(submitResult)), 0u);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    context->lastProducerReadyValue = producerReadyValue;
    *outProducerReadyValue = producerReadyValue;
    return APUSR_FRAMEGEN_OK;
}

void* apusrFramegenVulkanInputCompletionSemaphore(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept
{
    return context ? context->inputCompletionTimeline : nullptr;
}

void* apusrFramegenVulkanGenerationCompletionSemaphore(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept
{
    return context ? context->generationCompletionTimeline : nullptr;
}

std::uint64_t apusrFramegenVulkanCompletedProducerReadyValue(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept
{
    if (!context || !context->producerReadyTimeline || !context->functions.getSemaphoreCounterValue)
        return 0u;
    std::uint64_t value = 0u;
    const auto device = reinterpret_cast<vk::Device>(context->description.deviceContext.device);
    return context->functions.getSemaphoreCounterValue(device, context->producerReadyTimeline, &value) == vk::Success
        ? value
        : 0u;
}

std::uint64_t apusrFramegenVulkanCompletedInputValue(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept
{
    if (!context || !context->inputCompletionTimeline || !context->functions.getSemaphoreCounterValue)
        return 0u;
    std::uint64_t value = 0u;
    const auto device = reinterpret_cast<vk::Device>(context->description.deviceContext.device);
    return context->functions.getSemaphoreCounterValue(device, context->inputCompletionTimeline, &value) == vk::Success
        ? value
        : 0u;
}

std::uint64_t apusrFramegenVulkanCompletedGenerationValue(
    const ApusrFramegenVulkanPresentSubmitContext* context) noexcept
{
    if (!context || !context->generationCompletionTimeline || !context->functions.getSemaphoreCounterValue)
        return 0u;
    std::uint64_t value = 0u;
    const auto device = reinterpret_cast<vk::Device>(context->description.deviceContext.device);
    return context->functions.getSemaphoreCounterValue(device, context->generationCompletionTimeline, &value) == vk::Success
        ? value
        : 0u;
}

bool apusrFramegenWaitVulkanProducerReadyValue(
    const ApusrFramegenVulkanPresentSubmitContext* context,
    const std::uint64_t value,
    const std::uint64_t timeoutNanoseconds) noexcept
{
    if (!context || !context->producerReadyTimeline || !context->functions.waitSemaphores || value == 0u)
        return false;
    const vk::Semaphore semaphore = context->producerReadyTimeline;
    const vk::SemaphoreWaitInfo waitInfo{
        .sType = vk::StructureTypeSemaphoreWaitInfo,
        .pNext = nullptr,
        .flags = 0u,
        .semaphoreCount = 1u,
        .pSemaphores = &semaphore,
        .pValues = &value,
    };
    return context->functions.waitSemaphores(
        reinterpret_cast<vk::Device>(context->description.deviceContext.device),
        &waitInfo,
        timeoutNanoseconds) == vk::Success;
}

bool apusrFramegenWaitVulkanGenerationValue(
    const ApusrFramegenVulkanPresentSubmitContext* context,
    const std::uint64_t value,
    const std::uint64_t timeoutNanoseconds) noexcept
{
    if (!context || !context->generationCompletionTimeline || !context->functions.waitSemaphores || value == 0u)
        return false;
    const vk::Semaphore semaphore = context->generationCompletionTimeline;
    const vk::SemaphoreWaitInfo waitInfo{
        .sType = vk::StructureTypeSemaphoreWaitInfo,
        .pNext = nullptr,
        .flags = 0u,
        .semaphoreCount = 1u,
        .pSemaphores = &semaphore,
        .pValues = &value,
    };
    return context->functions.waitSemaphores(
        reinterpret_cast<vk::Device>(context->description.deviceContext.device),
        &waitInfo,
        timeoutNanoseconds) == vk::Success;
}

void apusrFramegenAbortVulkanPresentSubmit(
    ApusrFramegenVulkanPresentSubmitContext* context,
    const std::uint32_t slotIndex) noexcept
{
    if (!context || slotIndex >= context->description.slotCount)
        return;
    Slot& slot = context->slots[slotIndex];
    if (!slot.recording)
        return;
    (void)context->functions.resetCommandBuffer(slot.inputCommandBuffer, 0u);
    (void)context->functions.resetCommandBuffer(slot.generationCommandBuffer, 0u);
    slot.recording = false;
}

void apusrFramegenDestroyVulkanPresentSubmitContext(
    ApusrFramegenVulkanPresentSubmitContext* context,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!context)
        return;
    destroyContextContents(*context, allocator);
    allocator.destroy(context);
}
