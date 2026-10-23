// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_present_images_vk.h"

#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"

#include <new>

namespace {
namespace vk = apusr::backends::vk;

struct Functions {
    vk::PfnGetDeviceProcAddr getDeviceProcAddr = nullptr;
    vk::PfnCreateImageView createImageView = nullptr;
    vk::PfnDestroyImageView destroyImageView = nullptr;
    vk::PfnCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    vk::PfnCmdCopyImage cmdCopyImage = nullptr;
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
    APUSR_LOAD_VK("vkCreateImageView", createImageView);
    APUSR_LOAD_VK("vkDestroyImageView", destroyImageView);
    APUSR_LOAD_VK("vkCmdPipelineBarrier", cmdPipelineBarrier);
    APUSR_LOAD_VK("vkCmdCopyImage", cmdCopyImage);
#undef APUSR_LOAD_VK
    return true;
}


} // namespace

struct ApusrFramegenVulkanPresentImageContext {
    ApusrFramegenVulkanPresentImageDescription description{};
    ApusrFramegenAllocator allocator{};
    Functions functions{};
    vk::ImageView* swapchainViews = nullptr;
    vk::ImageView* presentationViews = nullptr;
    ApusrFramegenVulkanGeneratedOutputPool* generatedOutputPool = nullptr;
    bool initializationRecorded = false;
    bool initialized = false;
    bool directWsiStorage = false;
};

namespace {

vk::ImageSubresourceRange fullColorRange() noexcept
{
    return vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectColorBit,
        .baseMipLevel = 0u,
        .levelCount = 1u,
        .baseArrayLayer = 0u,
        .layerCount = 1u,
    };
}

void transitionImage(
    ApusrFramegenVulkanPresentImageContext& context,
    const vk::CommandBuffer commandBuffer,
    const vk::Image image,
    const vk::ImageLayout oldLayout,
    const vk::ImageLayout newLayout,
    const vk::PipelineStageFlags srcStage,
    const vk::AccessFlags srcAccess,
    const vk::PipelineStageFlags dstStage,
    const vk::AccessFlags dstAccess) noexcept
{
    if (oldLayout == newLayout)
        return;
    const vk::ImageMemoryBarrier barrier{
        .sType = vk::StructureTypeImageMemoryBarrier,
        .pNext = nullptr,
        .srcAccessMask = srcAccess,
        .dstAccessMask = dstAccess,
        .oldLayout = oldLayout,
        .newLayout = newLayout,
        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
        .image = image,
        .subresourceRange = fullColorRange(),
    };
    context.functions.cmdPipelineBarrier(
        commandBuffer,
        srcStage,
        dstStage,
        0u,
        0u, nullptr,
        0u, nullptr,
        1u, &barrier);
}

vk::Image presentationImage(
    const ApusrFramegenVulkanPresentImageContext& context,
    const std::uint32_t index) noexcept
{
    if (context.description.presentationImages && index < context.description.presentationImageCount)
        return reinterpret_cast<vk::Image>(context.description.presentationImages[index]);
    if (index < context.description.imageCount)
        return reinterpret_cast<vk::Image>(context.description.images[index]);
    return nullptr;
}

std::uint32_t presentationImageCount(const ApusrFramegenVulkanPresentImageContext& context) noexcept
{
    return context.description.presentationImages
        ? context.description.presentationImageCount
        : context.description.imageCount;
}

ApusrFramegenStreamlineVulkanResource swapchainResource(
    const ApusrFramegenVulkanPresentImageContext& context,
    const std::uint32_t index,
    const vk::ImageLayout layout) noexcept
{
    return ApusrFramegenStreamlineVulkanResource{
        .tagged = true,
        .image = context.description.images[index],
        .imageView = context.swapchainViews[index],
        .nativeFormat = context.description.nativeFormat,
        .nativeLayout = static_cast<std::uint32_t>(layout),
        .width = context.description.width,
        .height = context.description.height,
        .mipLevels = 1u,
        .arrayLayers = 1u,
        .nativeUsage = context.description.imageUsage,
        .extentLeft = 0u,
        .extentTop = 0u,
        .extentWidth = context.description.width,
        .extentHeight = context.description.height,
        .aspectMask = vk::ImageAspectColorBit,
        .baseMipLevel = 0u,
        .levelCount = 1u,
        .baseArrayLayer = 0u,
        .layerCount = 1u,
    };
}

ApusrFramegenStreamlineVulkanResource presentationResource(
    const ApusrFramegenVulkanPresentImageContext& context,
    const std::uint32_t index,
    const vk::ImageLayout layout) noexcept
{
    return ApusrFramegenStreamlineVulkanResource{
        .tagged = true,
        .image = context.description.presentationImages[index],
        .imageView = context.presentationViews[index],
        .nativeFormat = context.description.nativeFormat,
        .nativeLayout = static_cast<std::uint32_t>(layout),
        .width = context.description.width,
        .height = context.description.height,
        .mipLevels = 1u,
        .arrayLayers = 1u,
        .nativeUsage = context.description.presentationImageUsage,
        .extentLeft = 0u,
        .extentTop = 0u,
        .extentWidth = context.description.width,
        .extentHeight = context.description.height,
        .aspectMask = vk::ImageAspectColorBit,
        .baseMipLevel = 0u,
        .levelCount = 1u,
        .baseArrayLayer = 0u,
        .layerCount = 1u,
    };
}

void destroyContents(ApusrFramegenVulkanPresentImageContext& context, const ApusrFramegenAllocator& allocator) noexcept
{
    if (context.generatedOutputPool) {
        apusrFramegenDestroyVulkanGeneratedOutputPool(context.generatedOutputPool, allocator);
        context.generatedOutputPool = nullptr;
    }
    const auto device = reinterpret_cast<vk::Device>(context.description.deviceContext.device);
    if (context.swapchainViews) {
        for (std::uint32_t i = 0; i < context.description.imageCount; ++i) {
            if (context.swapchainViews[i])
                context.functions.destroyImageView(device, context.swapchainViews[i], nullptr);
        }
        allocator.dealloc(context.swapchainViews);
        context.swapchainViews = nullptr;
    }
    if (context.presentationViews) {
        for (std::uint32_t i = 0; i < context.description.presentationImageCount; ++i) {
            if (context.presentationViews[i])
                context.functions.destroyImageView(device, context.presentationViews[i], nullptr);
        }
        allocator.dealloc(context.presentationViews);
        context.presentationViews = nullptr;
    }
}

} // namespace

ApusrFramegenResult apusrFramegenCreateVulkanPresentImageContext(
    const ApusrFramegenVulkanPresentImageDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanPresentImageContext** outContext) noexcept
{
    if (!outContext)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outContext = nullptr;
    if (!description.deviceContext.device || !description.offscreenProxy ||
        !description.images || description.imageCount < 2u ||
        !description.presentationImages || description.presentationImageCount < 2u ||
        description.width == 0u || description.height == 0u || description.nativeFormat == 0u ||
        description.internalQueueFamilyIndexCount > description.internalQueueFamilyIndices.size())
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    // Clean-sheet proxy invariant: the virtual backbuffer is simultaneously
    // the immutable FI source and the mandatory physical-replay source. There
    // is intentionally no staging fallback for either use.
    if ((description.imageUsage & (vk::ImageUsageSampledBit | vk::ImageUsageTransferSrcBit)) !=
        (vk::ImageUsageSampledBit | vk::ImageUsageTransferSrcBit) ||
        (description.presentationImageUsage & vk::ImageUsageTransferDstBit) == 0u)
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;
    const bool directWsiStorage =
        (description.presentationImageUsage & vk::ImageUsageStorageBit) != 0u;

    auto* context = allocator.construct<ApusrFramegenVulkanPresentImageContext>();
    if (!context)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    context->description = description;
    context->allocator = allocator;
    context->directWsiStorage = directWsiStorage;
    if (!loadFunctions(description.deviceContext, context->functions)) {
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;
    }

    context->swapchainViews = static_cast<vk::ImageView*>(allocator.alloc(sizeof(vk::ImageView) * description.imageCount));
    if (!context->swapchainViews) {
        destroyContents(*context, allocator);
        allocator.destroy(context);
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    for (std::uint32_t i = 0; i < description.imageCount; ++i)
        context->swapchainViews[i] = nullptr;
    if (directWsiStorage) {
        context->presentationViews = static_cast<vk::ImageView*>(
            allocator.alloc(sizeof(vk::ImageView) * description.presentationImageCount));
        if (!context->presentationViews) {
            destroyContents(*context, allocator);
            allocator.destroy(context);
            return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
        }
        for (std::uint32_t i = 0; i < description.presentationImageCount; ++i)
            context->presentationViews[i] = nullptr;
    }

    const auto device = reinterpret_cast<vk::Device>(description.deviceContext.device);
    for (std::uint32_t i = 0; i < description.imageCount; ++i) {
        const vk::ImageViewCreateInfo viewInfo{
            .sType = vk::StructureTypeImageViewCreateInfo,
            .pNext = nullptr,
            .flags = 0u,
            .image = reinterpret_cast<vk::Image>(description.images[i]),
            .viewType = vk::ImageViewType2D,
            .format = static_cast<vk::Format>(description.nativeFormat),
            .components = {
                vk::ComponentSwizzleIdentity,
                vk::ComponentSwizzleIdentity,
                vk::ComponentSwizzleIdentity,
                vk::ComponentSwizzleIdentity,
            },
            .subresourceRange = fullColorRange(),
        };
        if (context->functions.createImageView(device, &viewInfo, nullptr, &context->swapchainViews[i]) != vk::Success ||
            !context->swapchainViews[i]) {
            destroyContents(*context, allocator);
            allocator.destroy(context);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
    }

    if (directWsiStorage) {
        for (std::uint32_t i = 0; i < description.presentationImageCount; ++i) {
            const vk::ImageViewCreateInfo viewInfo{
                .sType = vk::StructureTypeImageViewCreateInfo,
                .pNext = nullptr,
                .flags = 0u,
                .image = reinterpret_cast<vk::Image>(description.presentationImages[i]),
                .viewType = vk::ImageViewType2D,
                .format = static_cast<vk::Format>(description.nativeFormat),
                .components = {
                    vk::ComponentSwizzleIdentity,
                    vk::ComponentSwizzleIdentity,
                    vk::ComponentSwizzleIdentity,
                    vk::ComponentSwizzleIdentity,
                },
                .subresourceRange = fullColorRange(),
            };
            if (context->functions.createImageView(
                    device, &viewInfo, nullptr, &context->presentationViews[i]) != vk::Success ||
                !context->presentationViews[i]) {
                destroyContents(*context, allocator);
                allocator.destroy(context);
                return APUSR_FRAMEGEN_ERROR_BACKEND_API;
            }
        }
    }

    const ApusrFramegenVulkanGeneratedOutputPoolDescription poolDescription{
        .deviceContext = description.deviceContext,
        .width = description.width,
        .height = description.height,
        .nativeFormat = description.nativeFormat,
        .imageCount = APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS,
        .queueFamilyIndices = description.internalQueueFamilyIndices,
        .queueFamilyIndexCount = description.internalQueueFamilyIndexCount,
    };
    const ApusrFramegenResult poolResult = apusrFramegenCreateVulkanGeneratedOutputPool(
        poolDescription, allocator, &context->generatedOutputPool);
    if (poolResult != APUSR_FRAMEGEN_OK) {
        destroyContents(*context, allocator);
        allocator.destroy(context);
        return poolResult;
    }

    *outContext = context;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenInitializeVulkanPresentImageContext(
    ApusrFramegenVulkanPresentImageContext* context,
    void* commandBuffer) noexcept
{
    if (!context || !commandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (context->initialized || context->initializationRecorded)
        return APUSR_FRAMEGEN_OK;
    if (context->generatedOutputPool) {
        const ApusrFramegenResult result = apusrFramegenInitializeVulkanGeneratedOutputPool(
            context->generatedOutputPool, commandBuffer);
        if (result != APUSR_FRAMEGEN_OK)
            return result;
    }
    context->initializationRecorded = true;
    return APUSR_FRAMEGEN_OK;
}

void apusrFramegenCommitVulkanPresentImageInitialization(
    ApusrFramegenVulkanPresentImageContext* context) noexcept
{
    if (!context)
        return;
    apusrFramegenCommitVulkanGeneratedOutputPoolInitialization(context->generatedOutputPool);
    context->initializationRecorded = false;
    context->initialized = true;
}

void apusrFramegenRollbackVulkanPresentImageInitialization(
    ApusrFramegenVulkanPresentImageContext* context) noexcept
{
    if (!context)
        return;
    apusrFramegenRollbackVulkanGeneratedOutputPoolInitialization(context->generatedOutputPool);
    context->initializationRecorded = false;
}

bool apusrFramegenVulkanDirectWsiStorageSupported(
    const ApusrFramegenVulkanPresentImageContext* context) noexcept
{
    return context && context->directWsiStorage && context->presentationViews;
}

ApusrFramegenResult apusrFramegenPrepareVulkanOffscreenGenerationImages(
    ApusrFramegenVulkanPresentImageContext* context,
    void* inputCommandBuffer,
    void* generationCommandBuffer,
    const std::uint32_t sourceImageIndex,
    const std::uint32_t generationSlotIndex,
    const std::uint32_t directPresentationImageIndex,
    ApusrFramegenVulkanPresentFrameResources* outResources) noexcept
{
    if (!context || !inputCommandBuffer || !generationCommandBuffer || !outResources)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outResources = {};
    const bool directTarget = directPresentationImageIndex != APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE;
    if (sourceImageIndex >= context->description.imageCount ||
        (directTarget && (!context->directWsiStorage ||
            directPresentationImageIndex >= context->description.presentationImageCount)) ||
        (!directTarget && (generationSlotIndex >= APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS ||
            !context->generatedOutputPool)))
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    ApusrFramegenResult result = apusrFramegenInitializeVulkanPresentImageContext(
        context, inputCommandBuffer);
    if (result != APUSR_FRAMEGEN_OK)
        return result;

    // The virtual backbuffer itself is the immutable retained source. Its slot
    // remains live until both generation and physical source replay consume it,
    // so no full-resolution source copy belongs on the frame path.
    outResources->presentColor = swapchainResource(*context, sourceImageIndex, vk::ImageLayoutGeneral);
    outResources->presentColor.nativeUsage |= vk::ImageUsageSampledBit | vk::ImageUsageTransferSrcBit;
    outResources->sampledSwapchainDirectly = true;

    if (directTarget) {
        const auto generationCmd = reinterpret_cast<vk::CommandBuffer>(generationCommandBuffer);
        const auto target = presentationImage(*context, directPresentationImageIndex);
        if (!target)
            return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
        // Acquired WSI contents are deliberately discarded.  The acquire
        // semaphore establishes presentation-engine ownership; no prior pixels
        // need to survive, so UNDEFINED is the narrowest transition into FI.
        transitionImage(
            *context, generationCmd, target,
            vk::ImageLayoutUndefined, vk::ImageLayoutGeneral,
            vk::PipelineStageTopOfPipeBit, 0u,
            vk::PipelineStageComputeShaderBit, vk::AccessShaderWriteBit);
        outResources->generatedOutput = presentationResource(
            *context, directPresentationImageIndex, vk::ImageLayoutGeneral);
        outResources->storageSwapchainDirectly = true;
        return APUSR_FRAMEGEN_OK;
    }

    result = apusrFramegenGetVulkanGeneratedOutput(
        context->generatedOutputPool, generationSlotIndex, &outResources->generatedOutput);
    if (result != APUSR_FRAMEGEN_OK)
        return result;
    outResources->storageSwapchainDirectly = false;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenFinishVulkanOffscreenGenerationImages(
    ApusrFramegenVulkanPresentImageContext* context,
    void* generationCommandBuffer,
    const std::uint32_t directPresentationImageIndex) noexcept
{
    if (!context || !generationCommandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (directPresentationImageIndex == APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE)
        return APUSR_FRAMEGEN_OK;
    if (!context->directWsiStorage ||
        directPresentationImageIndex >= context->description.presentationImageCount)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    const auto cmd = reinterpret_cast<vk::CommandBuffer>(generationCommandBuffer);
    const auto target = presentationImage(*context, directPresentationImageIndex);
    if (!target)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    transitionImage(
        *context, cmd, target,
        vk::ImageLayoutGeneral, vk::ImageLayoutPresentSrcKhr,
        vk::PipelineStageComputeShaderBit, vk::AccessShaderWriteBit,
        vk::PipelineStageBottomOfPipeBit, 0u);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenRecordVulkanGeneratedReplay(
    ApusrFramegenVulkanPresentImageContext* context,
    void* commandBuffer,
    const std::uint32_t sourceImageIndex,
    const std::uint32_t targetImageIndex) noexcept
{
    if (!context || !commandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (sourceImageIndex >= APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS ||
        targetImageIndex >= presentationImageCount(*context) || !context->generatedOutputPool)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    ApusrFramegenStreamlineVulkanResource generated{};
    const ApusrFramegenResult result = apusrFramegenGetVulkanGeneratedOutput(
        context->generatedOutputPool, sourceImageIndex, &generated);
    if (result != APUSR_FRAMEGEN_OK)
        return result;
    const auto cmd = reinterpret_cast<vk::CommandBuffer>(commandBuffer);
    const auto source = reinterpret_cast<vk::Image>(generated.image);
    const auto target = presentationImage(*context, targetImageIndex);
    if (!target)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    // The replay queue can be transfer-only. Generation completion is imported
    // by the submit's timeline wait at TRANSFER, so this command buffer must
    // not name COMPUTE_SHADER merely because the source happens to be GENERAL.
    transitionImage(
        *context, cmd, source,
        vk::ImageLayoutGeneral, vk::ImageLayoutTransferSrcOptimal,
        vk::PipelineStageTransferBit, 0u,
        vk::PipelineStageTransferBit, vk::AccessTransferReadBit);
    transitionImage(
        *context, cmd, target,
        vk::ImageLayoutUndefined, vk::ImageLayoutTransferDstOptimal,
        vk::PipelineStageTopOfPipeBit, 0u,
        vk::PipelineStageTransferBit, vk::AccessTransferWriteBit);
    const vk::ImageCopy copy{
        .srcSubresource = {vk::ImageAspectColorBit, 0u, 0u, 1u},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {vk::ImageAspectColorBit, 0u, 0u, 1u},
        .dstOffset = {0, 0, 0},
        .extent = {context->description.width, context->description.height, 1u},
    };
    context->functions.cmdCopyImage(
        cmd, source, vk::ImageLayoutTransferSrcOptimal,
        target, vk::ImageLayoutTransferDstOptimal, 1u, &copy);
    transitionImage(
        *context, cmd, source,
        vk::ImageLayoutTransferSrcOptimal, vk::ImageLayoutGeneral,
        vk::PipelineStageTransferBit, vk::AccessTransferReadBit,
        vk::PipelineStageBottomOfPipeBit, 0u);
    transitionImage(
        *context, cmd, target,
        vk::ImageLayoutTransferDstOptimal, vk::ImageLayoutPresentSrcKhr,
        vk::PipelineStageTransferBit, vk::AccessTransferWriteBit,
        vk::PipelineStageBottomOfPipeBit, 0u);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenRecordVulkanSourceReplay(
    ApusrFramegenVulkanPresentImageContext* context,
    void* commandBuffer,
    const std::uint32_t sourceImageIndex,
    const std::uint32_t targetImageIndex) noexcept
{
    if (!context || !commandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (!context->description.offscreenProxy || sourceImageIndex >= context->description.imageCount ||
        targetImageIndex >= presentationImageCount(*context))
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    const auto cmd = reinterpret_cast<vk::CommandBuffer>(commandBuffer);
    const auto source = reinterpret_cast<vk::Image>(context->description.images[sourceImageIndex]);
    const auto target = presentationImage(*context, targetImageIndex);
    if (!source || !target)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    // Source stays in GENERAL for the complete retained-frame lifetime. OF/FI
    // and source replay are read-only consumers, so they can overlap without a
    // layout ownership ping-pong or a staging copy.
    transitionImage(
        *context, cmd, target,
        vk::ImageLayoutUndefined, vk::ImageLayoutTransferDstOptimal,
        vk::PipelineStageTopOfPipeBit, 0u,
        vk::PipelineStageTransferBit, vk::AccessTransferWriteBit);
    const vk::ImageCopy copy{
        .srcSubresource = {vk::ImageAspectColorBit, 0u, 0u, 1u},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {vk::ImageAspectColorBit, 0u, 0u, 1u},
        .dstOffset = {0, 0, 0},
        .extent = {context->description.width, context->description.height, 1u},
    };
    context->functions.cmdCopyImage(
        cmd, source, vk::ImageLayoutGeneral,
        target, vk::ImageLayoutTransferDstOptimal, 1u, &copy);
    transitionImage(
        *context, cmd, target,
        vk::ImageLayoutTransferDstOptimal, vk::ImageLayoutPresentSrcKhr,
        vk::PipelineStageTransferBit, vk::AccessTransferWriteBit,
        vk::PipelineStageBottomOfPipeBit, 0u);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenRecordVulkanAcquiredImageRetirement(
    ApusrFramegenVulkanPresentImageContext* context,
    void* commandBuffer,
    const std::uint32_t targetImageIndex) noexcept
{
    if (!context || !commandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (targetImageIndex >= presentationImageCount(*context))
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    // UNDEFINED is intentional even when this image has been presented before:
    // recovery does not preserve its contents. It only establishes the layout
    // required by vkQueuePresentKHR so an already-acquired WSI image is not
    // stranded when the source replay copy cannot be recorded.
    const auto cmd = reinterpret_cast<vk::CommandBuffer>(commandBuffer);
    const auto target = presentationImage(*context, targetImageIndex);
    transitionImage(
        *context, cmd, target,
        vk::ImageLayoutUndefined, vk::ImageLayoutPresentSrcKhr,
        vk::PipelineStageTopOfPipeBit, 0u,
        vk::PipelineStageBottomOfPipeBit, 0u);
    return APUSR_FRAMEGEN_OK;
}

void apusrFramegenDestroyVulkanPresentImageContext(
    ApusrFramegenVulkanPresentImageContext* context,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!context)
        return;
    destroyContents(*context, allocator);
    allocator.destroy(context);
}
