// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_present_resources_vk.h"

#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"

#include <limits>
#include <new>

namespace {
namespace vk = apusr::backends::vk;

struct GeneratedOutput {
    vk::Image image = nullptr;
    vk::ImageView view = nullptr;
    vk::DeviceMemory memory = nullptr;
};

struct Functions {
    vk::PfnGetInstanceProcAddr getInstanceProcAddr = nullptr;
    vk::PfnGetDeviceProcAddr getDeviceProcAddr = nullptr;
    vk::PfnGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    vk::PfnGetPhysicalDeviceFormatProperties getPhysicalDeviceFormatProperties = nullptr;
    vk::PfnCreateImage createImage = nullptr;
    vk::PfnDestroyImage destroyImage = nullptr;
    vk::PfnGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    vk::PfnBindImageMemory bindImageMemory = nullptr;
    vk::PfnAllocateMemory allocateMemory = nullptr;
    vk::PfnFreeMemory freeMemory = nullptr;
    vk::PfnCreateImageView createImageView = nullptr;
    vk::PfnDestroyImageView destroyImageView = nullptr;
    vk::PfnCmdPipelineBarrier cmdPipelineBarrier = nullptr;
};

constexpr vk::ImageUsageFlags kGeneratedUsage =
    vk::ImageUsageTransferSrcBit |
    vk::ImageUsageTransferDstBit |
    vk::ImageUsageSampledBit |
    vk::ImageUsageStorageBit;

vk::ImageUsageFlags requestedUsage(const ApusrFramegenVulkanGeneratedOutputPoolDescription& description) noexcept
{
    return description.imageUsage != 0u
        ? static_cast<vk::ImageUsageFlags>(description.imageUsage)
        : kGeneratedUsage;
}

vk::FormatFeatureFlags requiredFormatFeatures(const vk::ImageUsageFlags usage) noexcept
{
    vk::FormatFeatureFlags required = 0u;
    if ((usage & vk::ImageUsageSampledBit) != 0u) required |= vk::FormatFeatureSampledImageBit;
    if ((usage & vk::ImageUsageStorageBit) != 0u) required |= vk::FormatFeatureStorageImageBit;
    if ((usage & vk::ImageUsageTransferSrcBit) != 0u) required |= vk::FormatFeatureTransferSrcBit;
    if ((usage & vk::ImageUsageTransferDstBit) != 0u) required |= vk::FormatFeatureTransferDstBit;
    return required;
}

std::uint32_t findDeviceLocalMemoryType(
    const vk::PhysicalDeviceMemoryProperties& properties,
    const std::uint32_t typeBits) noexcept
{
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((typeBits & (1u << index)) != 0u &&
            (properties.memoryTypes[index].propertyFlags & vk::MemoryPropertyDeviceLocalBit) != 0u) {
            return index;
        }
    }
    return std::numeric_limits<std::uint32_t>::max();
}

template <typename Fn>
bool loadDeviceFunction(const ApusrFramegenVulkanDeviceContext& context, Functions& functions, const char* name, Fn& out) noexcept
{
    out = reinterpret_cast<Fn>(functions.getDeviceProcAddr(
        reinterpret_cast<vk::Device>(context.device), name));
    return out != nullptr;
}

bool loadFunctions(const ApusrFramegenVulkanDeviceContext& context, Functions& functions) noexcept
{
    functions.getInstanceProcAddr = reinterpret_cast<vk::PfnGetInstanceProcAddr>(context.getInstanceProcAddr);
    functions.getDeviceProcAddr = reinterpret_cast<vk::PfnGetDeviceProcAddr>(context.getDeviceProcAddr);
    const auto instance = reinterpret_cast<vk::Instance>(context.instance);
    if (!functions.getDeviceProcAddr && functions.getInstanceProcAddr) {
        functions.getDeviceProcAddr = reinterpret_cast<vk::PfnGetDeviceProcAddr>(
            functions.getInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
    }
    if (!functions.getInstanceProcAddr || !functions.getDeviceProcAddr || !context.device || !context.physicalDevice)
        return false;

    functions.getPhysicalDeviceMemoryProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceMemoryProperties>(
        functions.getInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties"));
    functions.getPhysicalDeviceFormatProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceFormatProperties>(
        functions.getInstanceProcAddr(instance, "vkGetPhysicalDeviceFormatProperties"));
    if (!functions.getPhysicalDeviceMemoryProperties || !functions.getPhysicalDeviceFormatProperties)
        return false;

#define APUSR_LOAD_VK(name, field) if (!loadDeviceFunction(context, functions, name, functions.field)) return false
    APUSR_LOAD_VK("vkCreateImage", createImage);
    APUSR_LOAD_VK("vkDestroyImage", destroyImage);
    APUSR_LOAD_VK("vkGetImageMemoryRequirements", getImageMemoryRequirements);
    APUSR_LOAD_VK("vkBindImageMemory", bindImageMemory);
    APUSR_LOAD_VK("vkAllocateMemory", allocateMemory);
    APUSR_LOAD_VK("vkFreeMemory", freeMemory);
    APUSR_LOAD_VK("vkCreateImageView", createImageView);
    APUSR_LOAD_VK("vkDestroyImageView", destroyImageView);
    APUSR_LOAD_VK("vkCmdPipelineBarrier", cmdPipelineBarrier);
#undef APUSR_LOAD_VK
    return true;
}

} // namespace

struct ApusrFramegenVulkanGeneratedOutputPool {
    enum class InitializationState : std::uint8_t {
        Uninitialized,
        Recorded,
        Committed,
    };

    ApusrFramegenVulkanGeneratedOutputPoolDescription description{};
    Functions functions{};
    vk::PhysicalDeviceMemoryProperties memoryProperties{};
    GeneratedOutput* outputs = nullptr;
    InitializationState initializationState = InitializationState::Uninitialized;
};

namespace {

void destroyOutputs(ApusrFramegenVulkanGeneratedOutputPool& pool, const ApusrFramegenAllocator& allocator) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(pool.description.deviceContext.device);
    if (pool.outputs) {
        for (std::uint32_t i = 0; i < pool.description.imageCount; ++i) {
            GeneratedOutput& output = pool.outputs[i];
            if (output.view) pool.functions.destroyImageView(device, output.view, nullptr);
            if (output.image) pool.functions.destroyImage(device, output.image, nullptr);
            if (output.memory) pool.functions.freeMemory(device, output.memory, nullptr);
        }
        allocator.dealloc(pool.outputs);
        pool.outputs = nullptr;
    }
}

ApusrFramegenResult createOutput(
    ApusrFramegenVulkanGeneratedOutputPool& pool,
    GeneratedOutput& output) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(pool.description.deviceContext.device);
    const auto format = static_cast<vk::Format>(pool.description.nativeFormat);
    const vk::ImageCreateInfo imageInfo{
        .sType = vk::StructureTypeImageCreateInfo,
        .pNext = nullptr,
        .flags = 0,
        .imageType = vk::ImageType2D,
        .format = format,
        .extent = {pool.description.width, pool.description.height, 1u},
        .mipLevels = 1u,
        .arrayLayers = 1u,
        .samples = vk::SampleCount1Bit,
        .tiling = vk::ImageTilingOptimal,
        .usage = requestedUsage(pool.description),
        .sharingMode = pool.description.queueFamilyIndexCount > 1u
            ? vk::SharingModeConcurrent
            : vk::SharingModeExclusive,
        .queueFamilyIndexCount = pool.description.queueFamilyIndexCount > 1u
            ? pool.description.queueFamilyIndexCount
            : 0u,
        .pQueueFamilyIndices = pool.description.queueFamilyIndexCount > 1u
            ? pool.description.queueFamilyIndices.data()
            : nullptr,
        .initialLayout = vk::ImageLayoutUndefined,
    };
    if (pool.functions.createImage(device, &imageInfo, nullptr, &output.image) != vk::Success || !output.image)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    vk::MemoryRequirements requirements{};
    pool.functions.getImageMemoryRequirements(device, output.image, &requirements);
    const std::uint32_t memoryType = findDeviceLocalMemoryType(pool.memoryProperties, requirements.memoryTypeBits);
    if (memoryType == std::numeric_limits<std::uint32_t>::max())
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    const vk::MemoryAllocateInfo allocation{
        .sType = vk::StructureTypeMemoryAllocateInfo,
        .pNext = nullptr,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType,
    };
    if (pool.functions.allocateMemory(device, &allocation, nullptr, &output.memory) != vk::Success || !output.memory ||
        pool.functions.bindImageMemory(device, output.image, output.memory, 0) != vk::Success) {
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    const vk::ImageViewCreateInfo viewInfo{
        .sType = vk::StructureTypeImageViewCreateInfo,
        .pNext = nullptr,
        .flags = 0,
        .image = output.image,
        .viewType = vk::ImageViewType2D,
        .format = format,
        .components = {vk::ComponentSwizzleIdentity, vk::ComponentSwizzleIdentity,
                       vk::ComponentSwizzleIdentity, vk::ComponentSwizzleIdentity},
        .subresourceRange = {vk::ImageAspectColorBit, 0u, 1u, 0u, 1u},
    };
    if (pool.functions.createImageView(device, &viewInfo, nullptr, &output.view) != vk::Success || !output.view)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    return APUSR_FRAMEGEN_OK;
}

} // namespace

ApusrFramegenResult apusrFramegenCreateVulkanGeneratedOutputPool(
    const ApusrFramegenVulkanGeneratedOutputPoolDescription& description,
    const ApusrFramegenAllocator& allocator,
    ApusrFramegenVulkanGeneratedOutputPool** outPool) noexcept
{
    if (!outPool)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outPool = nullptr;
    if (!description.deviceContext.device || !description.deviceContext.physicalDevice ||
        description.width == 0u || description.height == 0u || description.nativeFormat == 0u ||
        description.imageCount == 0u ||
        description.queueFamilyIndexCount > description.queueFamilyIndices.size())
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    auto* pool = allocator.construct<ApusrFramegenVulkanGeneratedOutputPool>();
    if (!pool)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    pool->description = description;
    if (!loadFunctions(description.deviceContext, pool->functions)) {
        allocator.destroy(pool);
        return APUSR_FRAMEGEN_ERROR_INCOMPLETE_INTERFACE;
    }

    vk::FormatProperties formatProperties{};
    pool->functions.getPhysicalDeviceFormatProperties(
        reinterpret_cast<vk::PhysicalDevice>(description.deviceContext.physicalDevice),
        static_cast<vk::Format>(description.nativeFormat), &formatProperties);
    const vk::FormatFeatureFlags requiredFeatures = requiredFormatFeatures(requestedUsage(description));
    if ((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures) {
        allocator.destroy(pool);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    pool->functions.getPhysicalDeviceMemoryProperties(
        reinterpret_cast<vk::PhysicalDevice>(description.deviceContext.physicalDevice), &pool->memoryProperties);
    pool->outputs = static_cast<GeneratedOutput*>(allocator.alloc(sizeof(GeneratedOutput) * description.imageCount));
    if (!pool->outputs) {
        allocator.destroy(pool);
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    }
    for (std::uint32_t i = 0; i < description.imageCount; ++i)
        ::new (&pool->outputs[i]) GeneratedOutput{};

    for (std::uint32_t i = 0; i < description.imageCount; ++i) {
        const ApusrFramegenResult result = createOutput(*pool, pool->outputs[i]);
        if (result != APUSR_FRAMEGEN_OK) {
            destroyOutputs(*pool, allocator);
            allocator.destroy(pool);
            return result;
        }
    }
    *outPool = pool;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenInitializeVulkanGeneratedOutputPool(
    ApusrFramegenVulkanGeneratedOutputPool* pool,
    void* commandBuffer) noexcept
{
    if (!pool || !commandBuffer)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (pool->initializationState != ApusrFramegenVulkanGeneratedOutputPool::InitializationState::Uninitialized)
        return APUSR_FRAMEGEN_OK;

    const vk::ImageSubresourceRange range{vk::ImageAspectColorBit, 0u, 1u, 0u, 1u};
    for (std::uint32_t i = 0; i < pool->description.imageCount; ++i) {
        const vk::ImageMemoryBarrier barrier{
            .sType = vk::StructureTypeImageMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = 0u,
            .dstAccessMask = vk::AccessShaderWriteBit,
            .oldLayout = vk::ImageLayoutUndefined,
            .newLayout = vk::ImageLayoutGeneral,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = pool->outputs[i].image,
            .subresourceRange = range,
        };
        pool->functions.cmdPipelineBarrier(
            reinterpret_cast<vk::CommandBuffer>(commandBuffer),
            vk::PipelineStageTopOfPipeBit, vk::PipelineStageComputeShaderBit, 0u,
            0u, nullptr, 0u, nullptr, 1u, &barrier);
    }
    pool->initializationState = ApusrFramegenVulkanGeneratedOutputPool::InitializationState::Recorded;
    return APUSR_FRAMEGEN_OK;
}

void apusrFramegenCommitVulkanGeneratedOutputPoolInitialization(
    ApusrFramegenVulkanGeneratedOutputPool* pool) noexcept
{
    if (pool && pool->initializationState == ApusrFramegenVulkanGeneratedOutputPool::InitializationState::Recorded)
        pool->initializationState = ApusrFramegenVulkanGeneratedOutputPool::InitializationState::Committed;
}

void apusrFramegenRollbackVulkanGeneratedOutputPoolInitialization(
    ApusrFramegenVulkanGeneratedOutputPool* pool) noexcept
{
    if (pool && pool->initializationState == ApusrFramegenVulkanGeneratedOutputPool::InitializationState::Recorded)
        pool->initializationState = ApusrFramegenVulkanGeneratedOutputPool::InitializationState::Uninitialized;
}

ApusrFramegenResult apusrFramegenGetVulkanGeneratedOutput(
    const ApusrFramegenVulkanGeneratedOutputPool* pool,
    const std::uint32_t imageIndex,
    ApusrFramegenStreamlineVulkanResource* outResource) noexcept
{
    if (!pool || !outResource)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    *outResource = {};
    if (pool->initializationState == ApusrFramegenVulkanGeneratedOutputPool::InitializationState::Uninitialized ||
        imageIndex >= pool->description.imageCount)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    const GeneratedOutput& output = pool->outputs[imageIndex];
    outResource->tagged = true;
    outResource->image = output.image;
    outResource->imageView = output.view;
    outResource->nativeFormat = pool->description.nativeFormat;
    outResource->nativeLayout = static_cast<std::uint32_t>(vk::ImageLayoutGeneral);
    outResource->width = pool->description.width;
    outResource->height = pool->description.height;
    outResource->mipLevels = 1u;
    outResource->arrayLayers = 1u;
    outResource->nativeUsage = requestedUsage(pool->description);
    outResource->extentWidth = pool->description.width;
    outResource->extentHeight = pool->description.height;
    outResource->aspectMask = vk::ImageAspectColorBit;
    outResource->levelCount = 1u;
    outResource->layerCount = 1u;
    return APUSR_FRAMEGEN_OK;
}

void apusrFramegenDestroyVulkanGeneratedOutputPool(
    ApusrFramegenVulkanGeneratedOutputPool* pool,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!pool)
        return;
    destroyOutputs(*pool, allocator);
    allocator.destroy(pool);
}
