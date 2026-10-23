// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

// Private Vulkan ABI subset used by the FSR4 recorder.  APUSR loads Vulkan at
// runtime, so product builds do not require Vulkan SDK headers or link import
// libraries.  Values/layouts mirror the Khronos Vulkan 1.2 core ABI on x64.

#include <cstddef>
#include <cstdint>

namespace apusr::backends::vk {

static_assert(sizeof(void*) == 8, "APUSR Vulkan backend currently targets x64 only");

#if defined(_WIN32)
#define APUSR_VK_CALL __stdcall
#else
#define APUSR_VK_CALL
#endif

using Bool32 = std::uint32_t;
using Flags = std::uint32_t;
using DeviceSize = std::uint64_t;
using Result = std::int32_t;
using StructureType = std::int32_t;
using Format = std::int32_t;
using ImageLayout = std::int32_t;
using ImageType = std::int32_t;
using ImageViewType = std::int32_t;
using ImageTiling = std::int32_t;
using SharingMode = std::int32_t;
using DescriptorType = std::int32_t;
using PipelineBindPoint = std::int32_t;
using Filter = std::int32_t;
using SamplerMipmapMode = std::int32_t;
using SamplerAddressMode = std::int32_t;
using CompareOp = std::int32_t;
using BorderColor = std::int32_t;
using ComponentSwizzle = std::int32_t;
using SampleCountFlagBits = Flags;
using SampleCountFlags = Flags;
using PhysicalDeviceType = std::int32_t;
using BufferUsageFlags = Flags;
using ImageUsageFlags = Flags;
using ImageAspectFlags = Flags;
using MemoryPropertyFlags = Flags;
using MemoryHeapFlags = Flags;
using ShaderStageFlags = Flags;
using ShaderStageFlagBits = std::int32_t;
using PipelineStageFlags = Flags;
using AccessFlags = Flags;
using FormatFeatureFlags = Flags;
using DescriptorPoolCreateFlags = Flags;
using DescriptorSetLayoutCreateFlags = Flags;
using PipelineLayoutCreateFlags = Flags;
using PipelineCreateFlags = Flags;
using ShaderStageCreateFlags = Flags;
using SamplerCreateFlags = Flags;
using BufferCreateFlags = Flags;
using ImageCreateFlags = Flags;
using ImageViewCreateFlags = Flags;
using ShaderModuleCreateFlags = Flags;
using DependencyFlags = Flags;
using MemoryMapFlags = Flags;
using CommandPoolCreateFlags = Flags;
using CommandBufferUsageFlags = Flags;
using CommandBufferResetFlags = Flags;
using FenceCreateFlags = Flags;
using SemaphoreCreateFlags = Flags;
using SemaphoreWaitFlags = Flags;
using SemaphoreType = std::int32_t;
using CommandBufferLevel = std::int32_t;
using DescriptorPoolResetFlags = Flags;
using QueryPoolCreateFlags = Flags;
using QueryResultFlags = Flags;
using QueryPipelineStatisticFlags = Flags;
using QueryType = std::int32_t;
using TimeDomain = std::int32_t;

struct Instance_T;
struct PhysicalDevice_T;
struct Device_T;
struct CommandBuffer_T;
struct CommandPool_T;
struct Semaphore_T;
struct Fence_T;
struct Queue_T;
struct Buffer_T;
struct Image_T;
struct ImageView_T;
struct DeviceMemory_T;
struct ShaderModule_T;
struct DescriptorSetLayout_T;
struct PipelineLayout_T;
struct Pipeline_T;
struct PipelineCache_T;
struct DescriptorPool_T;
struct DescriptorSet_T;
struct Sampler_T;
struct BufferView_T;
struct QueryPool_T;
struct AllocationCallbacks;
struct MemoryBarrier;
struct CopyDescriptorSet;
struct PushConstantRange;

using Instance = Instance_T*;
using PhysicalDevice = PhysicalDevice_T*;
using Device = Device_T*;
using CommandBuffer = CommandBuffer_T*;
using CommandPool = CommandPool_T*;
using Semaphore = Semaphore_T*;
using Fence = Fence_T*;
using Queue = Queue_T*;
using Buffer = Buffer_T*;
using Image = Image_T*;
using ImageView = ImageView_T*;
using DeviceMemory = DeviceMemory_T*;
using ShaderModule = ShaderModule_T*;
using DescriptorSetLayout = DescriptorSetLayout_T*;
using PipelineLayout = PipelineLayout_T*;
using Pipeline = Pipeline_T*;
using PipelineCache = PipelineCache_T*;
using DescriptorPool = DescriptorPool_T*;
using DescriptorSet = DescriptorSet_T*;
using Sampler = Sampler_T*;
using BufferView = BufferView_T*;
using QueryPool = QueryPool_T*;

inline constexpr Result Success = 0;
inline constexpr Result NotReady = 1;
inline constexpr Result Timeout = 2;
inline constexpr Result ErrorOutOfPoolMemory = -1000069000;
inline constexpr Result ErrorFragmentedPool = -12;

inline constexpr StructureType StructureTypeSubmitInfo = 4;
inline constexpr StructureType StructureTypeMemoryAllocateInfo = 5;
inline constexpr StructureType StructureTypeFenceCreateInfo = 8;
inline constexpr StructureType StructureTypeSemaphoreCreateInfo = 9;
inline constexpr StructureType StructureTypeBufferCreateInfo = 12;
inline constexpr StructureType StructureTypeImageCreateInfo = 14;
inline constexpr StructureType StructureTypeImageViewCreateInfo = 15;
inline constexpr StructureType StructureTypeShaderModuleCreateInfo = 16;
inline constexpr StructureType StructureTypePipelineShaderStageCreateInfo = 18;
inline constexpr StructureType StructureTypePipelineShaderStageRequiredSubgroupSizeCreateInfo = 1000225001;
inline constexpr StructureType StructureTypeQueryPoolCreateInfo = 28;
inline constexpr StructureType StructureTypeComputePipelineCreateInfo = 29;
inline constexpr StructureType StructureTypePipelineLayoutCreateInfo = 30;
inline constexpr StructureType StructureTypeSamplerCreateInfo = 31;
inline constexpr StructureType StructureTypeDescriptorSetLayoutCreateInfo = 32;
inline constexpr StructureType StructureTypeDescriptorPoolCreateInfo = 33;
inline constexpr StructureType StructureTypeDescriptorSetAllocateInfo = 34;
inline constexpr StructureType StructureTypeWriteDescriptorSet = 35;
inline constexpr StructureType StructureTypeCommandPoolCreateInfo = 39;
inline constexpr StructureType StructureTypeCommandBufferAllocateInfo = 40;
inline constexpr StructureType StructureTypeCommandBufferBeginInfo = 42;
inline constexpr StructureType StructureTypeBufferMemoryBarrier = 44;
inline constexpr StructureType StructureTypeImageMemoryBarrier = 45;
inline constexpr StructureType StructureTypeSemaphoreTypeCreateInfo = 1000207002;
inline constexpr StructureType StructureTypeTimelineSemaphoreSubmitInfo = 1000207003;
inline constexpr StructureType StructureTypeSemaphoreWaitInfo = 1000207004;
inline constexpr StructureType StructureTypeCalibratedTimestampInfo = 1000184000;
inline constexpr StructureType StructureTypePhysicalDeviceMemoryProperties2 = 1000059006;
inline constexpr StructureType StructureTypePhysicalDeviceMemoryBudgetPropertiesExt = 1000237000;
inline constexpr SemaphoreWaitFlags SemaphoreWaitAnyBit = 0x00000001u;

inline constexpr ImageType ImageType2D = 1;
inline constexpr ImageViewType ImageViewType2D = 1;
inline constexpr ImageTiling ImageTilingOptimal = 0;
inline constexpr SharingMode SharingModeExclusive = 0;
inline constexpr SharingMode SharingModeConcurrent = 1;
inline constexpr SampleCountFlagBits SampleCount1Bit = 0x00000001u;

inline constexpr DescriptorType DescriptorTypeSampler = 0;
inline constexpr DescriptorType DescriptorTypeSampledImage = 2;
inline constexpr DescriptorType DescriptorTypeStorageImage = 3;
inline constexpr DescriptorType DescriptorTypeUniformBuffer = 6;
inline constexpr DescriptorType DescriptorTypeStorageBuffer = 7;

inline constexpr QueryType QueryTypeTimestamp = 2;
inline constexpr QueryResultFlags QueryResult64Bit = 0x00000001u;
inline constexpr QueryResultFlags QueryResultWithAvailabilityBit = 0x00000004u;

inline constexpr TimeDomain TimeDomainDevice = 0;
inline constexpr TimeDomain TimeDomainClockMonotonic = 1;
inline constexpr TimeDomain TimeDomainClockMonotonicRaw = 2;
inline constexpr TimeDomain TimeDomainQueryPerformanceCounter = 3;

inline constexpr PipelineBindPoint PipelineBindPointCompute = 1;
inline constexpr ShaderStageFlags ShaderStageComputeBit = 0x00000020u;

inline constexpr ImageLayout ImageLayoutUndefined = 0;
inline constexpr ImageLayout ImageLayoutGeneral = 1;
inline constexpr ImageLayout ImageLayoutShaderReadOnlyOptimal = 5;
inline constexpr ImageLayout ImageLayoutTransferSrcOptimal = 6;
inline constexpr ImageLayout ImageLayoutTransferDstOptimal = 7;
inline constexpr ImageLayout ImageLayoutColorAttachmentOptimal = 2;
inline constexpr ImageLayout ImageLayoutDepthStencilAttachmentOptimal = 3;
inline constexpr ImageLayout ImageLayoutDepthStencilReadOnlyOptimal = 4;
inline constexpr ImageLayout ImageLayoutPresentSrcKhr = 1000001002;

inline constexpr BufferUsageFlags BufferUsageTransferSrcBit = 0x00000001u;
inline constexpr BufferUsageFlags BufferUsageTransferDstBit = 0x00000002u;
inline constexpr BufferUsageFlags BufferUsageUniformBufferBit = 0x00000010u;
inline constexpr BufferUsageFlags BufferUsageStorageBufferBit = 0x00000020u;
inline constexpr BufferUsageFlags BufferUsageIndirectBufferBit = 0x00000100u;
inline constexpr ImageUsageFlags ImageUsageTransferSrcBit = 0x00000001u;
inline constexpr ImageUsageFlags ImageUsageTransferDstBit = 0x00000002u;
inline constexpr ImageUsageFlags ImageUsageSampledBit = 0x00000004u;
inline constexpr ImageUsageFlags ImageUsageStorageBit = 0x00000008u;

inline constexpr ImageAspectFlags ImageAspectColorBit = 0x00000001u;
inline constexpr ImageAspectFlags ImageAspectDepthBit = 0x00000002u;
inline constexpr ImageAspectFlags ImageAspectStencilBit = 0x00000004u;
inline constexpr MemoryPropertyFlags MemoryPropertyDeviceLocalBit = 0x00000001u;
inline constexpr MemoryPropertyFlags MemoryPropertyHostVisibleBit = 0x00000002u;
inline constexpr MemoryPropertyFlags MemoryPropertyHostCoherentBit = 0x00000004u;

inline constexpr PipelineStageFlags PipelineStageTopOfPipeBit = 0x00000001u;
inline constexpr PipelineStageFlags PipelineStageTransferBit = 0x00001000u;
inline constexpr PipelineStageFlags PipelineStageComputeShaderBit = 0x00000800u;
inline constexpr PipelineStageFlags PipelineStageBottomOfPipeBit = 0x00002000u;
inline constexpr PipelineStageFlags PipelineStageAllCommandsBit = 0x00010000u;
inline constexpr AccessFlags AccessTransferReadBit = 0x00000800u;
inline constexpr AccessFlags AccessTransferWriteBit = 0x00001000u;
inline constexpr AccessFlags AccessShaderReadBit = 0x00000020u;
inline constexpr AccessFlags AccessShaderWriteBit = 0x00000040u;

inline constexpr FormatFeatureFlags FormatFeatureSampledImageBit = 0x00000001u;
inline constexpr FormatFeatureFlags FormatFeatureStorageImageBit = 0x00000002u;
inline constexpr FormatFeatureFlags FormatFeatureTransferSrcBit = 0x00004000u;
inline constexpr FormatFeatureFlags FormatFeatureTransferDstBit = 0x00008000u;

inline constexpr Filter FilterNearest = 0;
inline constexpr Filter FilterLinear = 1;
inline constexpr SamplerMipmapMode SamplerMipmapModeNearest = 0;
inline constexpr SamplerMipmapMode SamplerMipmapModeLinear = 1;
inline constexpr SamplerAddressMode SamplerAddressModeRepeat = 0;
inline constexpr SamplerAddressMode SamplerAddressModeMirroredRepeat = 1;
inline constexpr SamplerAddressMode SamplerAddressModeClampToEdge = 2;
inline constexpr SamplerAddressMode SamplerAddressModeClampToBorder = 3;
inline constexpr SamplerAddressMode SamplerAddressModeMirrorClampToEdge = 4;
inline constexpr CompareOp CompareOpAlways = 7;
inline constexpr BorderColor BorderColorFloatTransparentBlack = 0;
inline constexpr ComponentSwizzle ComponentSwizzleIdentity = 0;

inline constexpr std::uint32_t QueueFamilyIgnored = ~0u;
inline constexpr CommandPoolCreateFlags CommandPoolCreateResetCommandBufferBit = 0x00000002u;
inline constexpr CommandBufferUsageFlags CommandBufferUsageOneTimeSubmitBit = 0x00000001u;
inline constexpr FenceCreateFlags FenceCreateSignaledBit = 0x00000001u;
inline constexpr SemaphoreType SemaphoreTypeBinary = 0;
inline constexpr SemaphoreType SemaphoreTypeTimeline = 1;
inline constexpr CommandBufferLevel CommandBufferLevelPrimary = 0;
inline constexpr DeviceSize WholeSize = ~DeviceSize{0};
inline constexpr MemoryHeapFlags MemoryHeapDeviceLocalBit = 0x00000001u;
inline constexpr std::uint32_t MaxMemoryTypes = 32u;
inline constexpr std::uint32_t MaxMemoryHeaps = 16u;
inline constexpr std::uint32_t MaxPhysicalDeviceNameSize = 256u;
inline constexpr std::uint32_t UuidSize = 16u;

// Formats used by APUSR compute backends.
inline constexpr Format FormatR8Unorm = 9;
inline constexpr Format FormatR8Snorm = 10;
inline constexpr Format FormatR8Uint = 13;
inline constexpr Format FormatR8G8Unorm = 16;
inline constexpr Format FormatR8G8Uint = 20;
inline constexpr Format FormatR8G8B8A8Unorm = 37;
inline constexpr Format FormatR8G8B8A8Snorm = 38;
inline constexpr Format FormatR8G8B8A8Uint = 41;
inline constexpr Format FormatR8G8B8A8Srgb = 43;
inline constexpr Format FormatB8G8R8A8Unorm = 44;
inline constexpr Format FormatB8G8R8A8Srgb = 50;
inline constexpr Format FormatA2B10G10R10UnormPack32 = 64;
inline constexpr Format FormatR16Unorm = 70;
inline constexpr Format FormatR16Snorm = 71;
inline constexpr Format FormatR16Uint = 74;
inline constexpr Format FormatR16Sfloat = 76;
inline constexpr Format FormatR16G16Unorm = 77;
inline constexpr Format FormatR16G16Uint = 81;
inline constexpr Format FormatR16G16Sint = 82;
inline constexpr Format FormatR16G16Sfloat = 83;
inline constexpr Format FormatR16G16B16A16Sfloat = 97;
inline constexpr Format FormatR32Uint = 98;
inline constexpr Format FormatR32Sfloat = 100;
inline constexpr Format FormatR32G32Uint = 101;
inline constexpr Format FormatR32G32Sfloat = 103;
inline constexpr Format FormatR32G32B32Sfloat = 106;
inline constexpr Format FormatR32G32B32A32Uint = 107;
inline constexpr Format FormatR32G32B32A32Sfloat = 109;
inline constexpr Format FormatB10G11R11UfloatPack32 = 122;
inline constexpr Format FormatE5B9G9R9UfloatPack32 = 123;
inline constexpr Format FormatD16Unorm = 124;
inline constexpr Format FormatX8D24UnormPack32 = 125;
inline constexpr Format FormatD32Sfloat = 126;
inline constexpr Format FormatS8Uint = 127;
inline constexpr Format FormatD16UnormS8Uint = 128;
inline constexpr Format FormatD24UnormS8Uint = 129;
inline constexpr Format FormatD32SfloatS8Uint = 130;

struct Extent3D {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
};

struct ComponentMapping {
    ComponentSwizzle r;
    ComponentSwizzle g;
    ComponentSwizzle b;
    ComponentSwizzle a;
};

struct Offset3D {
    std::int32_t x;
    std::int32_t y;
    std::int32_t z;
};

struct ImageSubresourceLayers {
    ImageAspectFlags aspectMask;
    std::uint32_t mipLevel;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};

struct ImageSubresourceRange {
    ImageAspectFlags aspectMask;
    std::uint32_t baseMipLevel;
    std::uint32_t levelCount;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};

struct QueryPoolCreateInfo {
    StructureType sType;
    const void* pNext;
    QueryPoolCreateFlags flags;
    QueryType queryType;
    std::uint32_t queryCount;
    QueryPipelineStatisticFlags pipelineStatistics;
};

struct CalibratedTimestampInfo {
    StructureType sType;
    const void* pNext;
    TimeDomain timeDomain;
};

struct CommandPoolCreateInfo {
    StructureType sType;
    const void* pNext;
    CommandPoolCreateFlags flags;
    std::uint32_t queueFamilyIndex;
};

struct CommandBufferAllocateInfo {
    StructureType sType;
    const void* pNext;
    CommandPool commandPool;
    CommandBufferLevel level;
    std::uint32_t commandBufferCount;
};

struct CommandBufferBeginInfo {
    StructureType sType;
    const void* pNext;
    CommandBufferUsageFlags flags;
    const void* pInheritanceInfo;
};

struct FenceCreateInfo {
    StructureType sType;
    const void* pNext;
    FenceCreateFlags flags;
};

struct SemaphoreCreateInfo {
    StructureType sType;
    const void* pNext;
    SemaphoreCreateFlags flags;
};

struct SemaphoreTypeCreateInfo {
    StructureType sType;
    const void* pNext;
    SemaphoreType semaphoreType;
    std::uint64_t initialValue;
};

struct TimelineSemaphoreSubmitInfo {
    StructureType sType;
    const void* pNext;
    std::uint32_t waitSemaphoreValueCount;
    const std::uint64_t* pWaitSemaphoreValues;
    std::uint32_t signalSemaphoreValueCount;
    const std::uint64_t* pSignalSemaphoreValues;
};

struct SemaphoreWaitInfo {
    StructureType sType;
    const void* pNext;
    SemaphoreWaitFlags flags;
    std::uint32_t semaphoreCount;
    const Semaphore* pSemaphores;
    const std::uint64_t* pValues;
};

struct SubmitInfo {
    StructureType sType;
    const void* pNext;
    std::uint32_t waitSemaphoreCount;
    const Semaphore* pWaitSemaphores;
    const PipelineStageFlags* pWaitDstStageMask;
    std::uint32_t commandBufferCount;
    const CommandBuffer* pCommandBuffers;
    std::uint32_t signalSemaphoreCount;
    const Semaphore* pSignalSemaphores;
};

struct BufferCreateInfo {
    StructureType sType;
    const void* pNext;
    BufferCreateFlags flags;
    DeviceSize size;
    BufferUsageFlags usage;
    SharingMode sharingMode;
    std::uint32_t queueFamilyIndexCount;
    const std::uint32_t* pQueueFamilyIndices;
};

struct ImageCreateInfo {
    StructureType sType;
    const void* pNext;
    ImageCreateFlags flags;
    ImageType imageType;
    Format format;
    Extent3D extent;
    std::uint32_t mipLevels;
    std::uint32_t arrayLayers;
    SampleCountFlagBits samples;
    ImageTiling tiling;
    ImageUsageFlags usage;
    SharingMode sharingMode;
    std::uint32_t queueFamilyIndexCount;
    const std::uint32_t* pQueueFamilyIndices;
    ImageLayout initialLayout;
};

struct BufferCopy {
    DeviceSize srcOffset;
    DeviceSize dstOffset;
    DeviceSize size;
};

struct BufferImageCopy {
    DeviceSize bufferOffset;
    std::uint32_t bufferRowLength;
    std::uint32_t bufferImageHeight;
    ImageSubresourceLayers imageSubresource;
    Offset3D imageOffset;
    Extent3D imageExtent;
};

struct ImageCopy {
    ImageSubresourceLayers srcSubresource;
    Offset3D srcOffset;
    ImageSubresourceLayers dstSubresource;
    Offset3D dstOffset;
    Extent3D extent;
};

struct MemoryRequirements {
    DeviceSize size;
    DeviceSize alignment;
    std::uint32_t memoryTypeBits;
};

struct MemoryAllocateInfo {
    StructureType sType;
    const void* pNext;
    DeviceSize allocationSize;
    std::uint32_t memoryTypeIndex;
};

struct MemoryType {
    MemoryPropertyFlags propertyFlags;
    std::uint32_t heapIndex;
};

struct MemoryHeap {
    DeviceSize size;
    MemoryHeapFlags flags;
};

struct PhysicalDeviceMemoryProperties {
    std::uint32_t memoryTypeCount;
    MemoryType memoryTypes[MaxMemoryTypes];
    std::uint32_t memoryHeapCount;
    MemoryHeap memoryHeaps[MaxMemoryHeaps];
};

struct PhysicalDeviceMemoryProperties2 {
    StructureType sType;
    void* pNext;
    PhysicalDeviceMemoryProperties memoryProperties;
};

struct PhysicalDeviceMemoryBudgetPropertiesExt {
    StructureType sType;
    void* pNext;
    DeviceSize heapBudget[MaxMemoryHeaps];
    DeviceSize heapUsage[MaxMemoryHeaps];
};

struct PhysicalDeviceLimits {
    std::uint32_t maxImageDimension1D;
    std::uint32_t maxImageDimension2D;
    std::uint32_t maxImageDimension3D;
    std::uint32_t maxImageDimensionCube;
    std::uint32_t maxImageArrayLayers;
    std::uint32_t maxTexelBufferElements;
    std::uint32_t maxUniformBufferRange;
    std::uint32_t maxStorageBufferRange;
    std::uint32_t maxPushConstantsSize;
    std::uint32_t maxMemoryAllocationCount;
    std::uint32_t maxSamplerAllocationCount;
    DeviceSize bufferImageGranularity;
    DeviceSize sparseAddressSpaceSize;
    std::uint32_t maxBoundDescriptorSets;
    std::uint32_t maxPerStageDescriptorSamplers;
    std::uint32_t maxPerStageDescriptorUniformBuffers;
    std::uint32_t maxPerStageDescriptorStorageBuffers;
    std::uint32_t maxPerStageDescriptorSampledImages;
    std::uint32_t maxPerStageDescriptorStorageImages;
    std::uint32_t maxPerStageDescriptorInputAttachments;
    std::uint32_t maxPerStageResources;
    std::uint32_t maxDescriptorSetSamplers;
    std::uint32_t maxDescriptorSetUniformBuffers;
    std::uint32_t maxDescriptorSetUniformBuffersDynamic;
    std::uint32_t maxDescriptorSetStorageBuffers;
    std::uint32_t maxDescriptorSetStorageBuffersDynamic;
    std::uint32_t maxDescriptorSetSampledImages;
    std::uint32_t maxDescriptorSetStorageImages;
    std::uint32_t maxDescriptorSetInputAttachments;
    std::uint32_t maxVertexInputAttributes;
    std::uint32_t maxVertexInputBindings;
    std::uint32_t maxVertexInputAttributeOffset;
    std::uint32_t maxVertexInputBindingStride;
    std::uint32_t maxVertexOutputComponents;
    std::uint32_t maxTessellationGenerationLevel;
    std::uint32_t maxTessellationPatchSize;
    std::uint32_t maxTessellationControlPerVertexInputComponents;
    std::uint32_t maxTessellationControlPerVertexOutputComponents;
    std::uint32_t maxTessellationControlPerPatchOutputComponents;
    std::uint32_t maxTessellationControlTotalOutputComponents;
    std::uint32_t maxTessellationEvaluationInputComponents;
    std::uint32_t maxTessellationEvaluationOutputComponents;
    std::uint32_t maxGeometryShaderInvocations;
    std::uint32_t maxGeometryInputComponents;
    std::uint32_t maxGeometryOutputComponents;
    std::uint32_t maxGeometryOutputVertices;
    std::uint32_t maxGeometryTotalOutputComponents;
    std::uint32_t maxFragmentInputComponents;
    std::uint32_t maxFragmentOutputAttachments;
    std::uint32_t maxFragmentDualSrcAttachments;
    std::uint32_t maxFragmentCombinedOutputResources;
    std::uint32_t maxComputeSharedMemorySize;
    std::uint32_t maxComputeWorkGroupCount[3];
    std::uint32_t maxComputeWorkGroupInvocations;
    std::uint32_t maxComputeWorkGroupSize[3];
    std::uint32_t subPixelPrecisionBits;
    std::uint32_t subTexelPrecisionBits;
    std::uint32_t mipmapPrecisionBits;
    std::uint32_t maxDrawIndexedIndexValue;
    std::uint32_t maxDrawIndirectCount;
    float maxSamplerLodBias;
    float maxSamplerAnisotropy;
    std::uint32_t maxViewports;
    std::uint32_t maxViewportDimensions[2];
    float viewportBoundsRange[2];
    std::uint32_t viewportSubPixelBits;
    std::size_t minMemoryMapAlignment;
    DeviceSize minTexelBufferOffsetAlignment;
    DeviceSize minUniformBufferOffsetAlignment;
    DeviceSize minStorageBufferOffsetAlignment;
    std::int32_t minTexelOffset;
    std::uint32_t maxTexelOffset;
    std::int32_t minTexelGatherOffset;
    std::uint32_t maxTexelGatherOffset;
    float minInterpolationOffset;
    float maxInterpolationOffset;
    std::uint32_t subPixelInterpolationOffsetBits;
    std::uint32_t maxFramebufferWidth;
    std::uint32_t maxFramebufferHeight;
    std::uint32_t maxFramebufferLayers;
    SampleCountFlags framebufferColorSampleCounts;
    SampleCountFlags framebufferDepthSampleCounts;
    SampleCountFlags framebufferStencilSampleCounts;
    SampleCountFlags framebufferNoAttachmentsSampleCounts;
    std::uint32_t maxColorAttachments;
    SampleCountFlags sampledImageColorSampleCounts;
    SampleCountFlags sampledImageIntegerSampleCounts;
    SampleCountFlags sampledImageDepthSampleCounts;
    SampleCountFlags sampledImageStencilSampleCounts;
    SampleCountFlags storageImageSampleCounts;
    std::uint32_t maxSampleMaskWords;
    Bool32 timestampComputeAndGraphics;
    float timestampPeriod;
    std::uint32_t maxClipDistances;
    std::uint32_t maxCullDistances;
    std::uint32_t maxCombinedClipAndCullDistances;
    std::uint32_t discreteQueuePriorities;
    float pointSizeRange[2];
    float lineWidthRange[2];
    float pointSizeGranularity;
    float lineWidthGranularity;
    Bool32 strictLines;
    Bool32 standardSampleLocations;
    DeviceSize optimalBufferCopyOffsetAlignment;
    DeviceSize optimalBufferCopyRowPitchAlignment;
    DeviceSize nonCoherentAtomSize;
};

struct PhysicalDeviceSparseProperties {
    Bool32 residencyStandard2DBlockShape;
    Bool32 residencyStandard2DMultisampleBlockShape;
    Bool32 residencyStandard3DBlockShape;
    Bool32 residencyAlignedMipSize;
    Bool32 residencyNonResidentStrict;
};

struct PhysicalDeviceProperties {
    std::uint32_t apiVersion;
    std::uint32_t driverVersion;
    std::uint32_t vendorID;
    std::uint32_t deviceID;
    PhysicalDeviceType deviceType;
    char deviceName[MaxPhysicalDeviceNameSize];
    std::uint8_t pipelineCacheUUID[UuidSize];
    PhysicalDeviceLimits limits;
    PhysicalDeviceSparseProperties sparseProperties;
};

struct QueueFamilyProperties {
    Flags queueFlags;
    std::uint32_t queueCount;
    std::uint32_t timestampValidBits;
    Extent3D minImageTransferGranularity;
};

struct FormatProperties {
    FormatFeatureFlags linearTilingFeatures;
    FormatFeatureFlags optimalTilingFeatures;
    FormatFeatureFlags bufferFeatures;
};

struct ImageViewCreateInfo {
    StructureType sType;
    const void* pNext;
    ImageViewCreateFlags flags;
    Image image;
    ImageViewType viewType;
    Format format;
    ComponentMapping components;
    ImageSubresourceRange subresourceRange;
};

struct ShaderModuleCreateInfo {
    StructureType sType;
    const void* pNext;
    ShaderModuleCreateFlags flags;
    std::size_t codeSize;
    const std::uint32_t* pCode;
};

struct DescriptorSetLayoutBinding {
    std::uint32_t binding;
    DescriptorType descriptorType;
    std::uint32_t descriptorCount;
    ShaderStageFlags stageFlags;
    const Sampler* pImmutableSamplers;
};

struct DescriptorSetLayoutCreateInfo {
    StructureType sType;
    const void* pNext;
    DescriptorSetLayoutCreateFlags flags;
    std::uint32_t bindingCount;
    const DescriptorSetLayoutBinding* pBindings;
};

struct PipelineLayoutCreateInfo {
    StructureType sType;
    const void* pNext;
    PipelineLayoutCreateFlags flags;
    std::uint32_t setLayoutCount;
    const DescriptorSetLayout* pSetLayouts;
    std::uint32_t pushConstantRangeCount;
    const PushConstantRange* pPushConstantRanges;
};

struct SpecializationInfo;

struct PipelineShaderStageRequiredSubgroupSizeCreateInfo {
    StructureType sType;
    const void* pNext;
    std::uint32_t requiredSubgroupSize;
};

struct PipelineShaderStageCreateInfo {
    StructureType sType;
    const void* pNext;
    ShaderStageCreateFlags flags;
    ShaderStageFlagBits stage;
    ShaderModule module;
    const char* pName;
    const SpecializationInfo* pSpecializationInfo;
};

struct ComputePipelineCreateInfo {
    StructureType sType;
    const void* pNext;
    PipelineCreateFlags flags;
    PipelineShaderStageCreateInfo stage;
    PipelineLayout layout;
    Pipeline basePipelineHandle;
    std::int32_t basePipelineIndex;
};

struct DescriptorPoolSize {
    DescriptorType type;
    std::uint32_t descriptorCount;
};

struct DescriptorPoolCreateInfo {
    StructureType sType;
    const void* pNext;
    DescriptorPoolCreateFlags flags;
    std::uint32_t maxSets;
    std::uint32_t poolSizeCount;
    const DescriptorPoolSize* pPoolSizes;
};

struct DescriptorSetAllocateInfo {
    StructureType sType;
    const void* pNext;
    DescriptorPool descriptorPool;
    std::uint32_t descriptorSetCount;
    const DescriptorSetLayout* pSetLayouts;
};

struct DescriptorImageInfo {
    Sampler sampler;
    ImageView imageView;
    ImageLayout imageLayout;
};

struct DescriptorBufferInfo {
    Buffer buffer;
    DeviceSize offset;
    DeviceSize range;
};

struct WriteDescriptorSet {
    StructureType sType;
    const void* pNext;
    DescriptorSet dstSet;
    std::uint32_t dstBinding;
    std::uint32_t dstArrayElement;
    std::uint32_t descriptorCount;
    DescriptorType descriptorType;
    const DescriptorImageInfo* pImageInfo;
    const DescriptorBufferInfo* pBufferInfo;
    const BufferView* pTexelBufferView;
};

struct SamplerCreateInfo {
    StructureType sType;
    const void* pNext;
    SamplerCreateFlags flags;
    Filter magFilter;
    Filter minFilter;
    SamplerMipmapMode mipmapMode;
    SamplerAddressMode addressModeU;
    SamplerAddressMode addressModeV;
    SamplerAddressMode addressModeW;
    float mipLodBias;
    Bool32 anisotropyEnable;
    float maxAnisotropy;
    Bool32 compareEnable;
    CompareOp compareOp;
    float minLod;
    float maxLod;
    BorderColor borderColor;
    Bool32 unnormalizedCoordinates;
};

struct ImageMemoryBarrier {
    StructureType sType;
    const void* pNext;
    AccessFlags srcAccessMask;
    AccessFlags dstAccessMask;
    ImageLayout oldLayout;
    ImageLayout newLayout;
    std::uint32_t srcQueueFamilyIndex;
    std::uint32_t dstQueueFamilyIndex;
    Image image;
    ImageSubresourceRange subresourceRange;
};

struct BufferMemoryBarrier {
    StructureType sType;
    const void* pNext;
    AccessFlags srcAccessMask;
    AccessFlags dstAccessMask;
    std::uint32_t srcQueueFamilyIndex;
    std::uint32_t dstQueueFamilyIndex;
    Buffer buffer;
    DeviceSize offset;
    DeviceSize size;
};

union ClearColorValue {
    float float32[4];
    std::int32_t int32[4];
    std::uint32_t uint32[4];
};

using PfnVoidFunction = void(APUSR_VK_CALL*)();
using PfnGetInstanceProcAddr = PfnVoidFunction(APUSR_VK_CALL*)(Instance, const char*);
using PfnGetDeviceProcAddr = PfnVoidFunction(APUSR_VK_CALL*)(Device, const char*);
using PfnGetPhysicalDeviceProperties = void(APUSR_VK_CALL*)(PhysicalDevice, PhysicalDeviceProperties*);
using PfnGetPhysicalDeviceMemoryProperties = void(APUSR_VK_CALL*)(PhysicalDevice, PhysicalDeviceMemoryProperties*);
using PfnGetPhysicalDeviceMemoryProperties2 = void(APUSR_VK_CALL*)(PhysicalDevice, PhysicalDeviceMemoryProperties2*);
using PfnGetPhysicalDeviceFormatProperties = void(APUSR_VK_CALL*)(PhysicalDevice, Format, FormatProperties*);
using PfnGetPhysicalDeviceQueueFamilyProperties = void(APUSR_VK_CALL*)(PhysicalDevice, std::uint32_t*, QueueFamilyProperties*);
using PfnGetPhysicalDeviceCalibrateableTimeDomains = Result(APUSR_VK_CALL*)(PhysicalDevice, std::uint32_t*, TimeDomain*);
using PfnGetCalibratedTimestamps = Result(APUSR_VK_CALL*)(Device, std::uint32_t, const CalibratedTimestampInfo*, std::uint64_t*, std::uint64_t*);
using PfnCreateCommandPool = Result(APUSR_VK_CALL*)(Device, const CommandPoolCreateInfo*, const AllocationCallbacks*, CommandPool*);
using PfnDestroyCommandPool = void(APUSR_VK_CALL*)(Device, CommandPool, const AllocationCallbacks*);
using PfnAllocateCommandBuffers = Result(APUSR_VK_CALL*)(Device, const CommandBufferAllocateInfo*, CommandBuffer*);
using PfnResetCommandBuffer = Result(APUSR_VK_CALL*)(CommandBuffer, CommandBufferResetFlags);
using PfnBeginCommandBuffer = Result(APUSR_VK_CALL*)(CommandBuffer, const CommandBufferBeginInfo*);
using PfnEndCommandBuffer = Result(APUSR_VK_CALL*)(CommandBuffer);
using PfnCreateSemaphore = Result(APUSR_VK_CALL*)(Device, const SemaphoreCreateInfo*, const AllocationCallbacks*, Semaphore*);
using PfnDestroySemaphore = void(APUSR_VK_CALL*)(Device, Semaphore, const AllocationCallbacks*);
using PfnGetSemaphoreCounterValue = Result(APUSR_VK_CALL*)(Device, Semaphore, std::uint64_t*);
using PfnWaitSemaphores = Result(APUSR_VK_CALL*)(Device, const SemaphoreWaitInfo*, std::uint64_t);
using PfnCreateFence = Result(APUSR_VK_CALL*)(Device, const FenceCreateInfo*, const AllocationCallbacks*, Fence*);
using PfnDestroyFence = void(APUSR_VK_CALL*)(Device, Fence, const AllocationCallbacks*);
using PfnGetFenceStatus = Result(APUSR_VK_CALL*)(Device, Fence);
using PfnWaitForFences = Result(APUSR_VK_CALL*)(Device, std::uint32_t, const Fence*, Bool32, std::uint64_t);
using PfnResetFences = Result(APUSR_VK_CALL*)(Device, std::uint32_t, const Fence*);
using PfnQueueSubmit = Result(APUSR_VK_CALL*)(Queue, std::uint32_t, const SubmitInfo*, Fence);
using PfnQueueWaitIdle = Result(APUSR_VK_CALL*)(Queue);
using PfnCreateBuffer = Result(APUSR_VK_CALL*)(Device, const BufferCreateInfo*, const AllocationCallbacks*, Buffer*);
using PfnDestroyBuffer = void(APUSR_VK_CALL*)(Device, Buffer, const AllocationCallbacks*);
using PfnGetBufferMemoryRequirements = void(APUSR_VK_CALL*)(Device, Buffer, MemoryRequirements*);
using PfnBindBufferMemory = Result(APUSR_VK_CALL*)(Device, Buffer, DeviceMemory, DeviceSize);
using PfnCreateImage = Result(APUSR_VK_CALL*)(Device, const ImageCreateInfo*, const AllocationCallbacks*, Image*);
using PfnDestroyImage = void(APUSR_VK_CALL*)(Device, Image, const AllocationCallbacks*);
using PfnGetImageMemoryRequirements = void(APUSR_VK_CALL*)(Device, Image, MemoryRequirements*);
using PfnBindImageMemory = Result(APUSR_VK_CALL*)(Device, Image, DeviceMemory, DeviceSize);
using PfnAllocateMemory = Result(APUSR_VK_CALL*)(Device, const MemoryAllocateInfo*, const AllocationCallbacks*, DeviceMemory*);
using PfnFreeMemory = void(APUSR_VK_CALL*)(Device, DeviceMemory, const AllocationCallbacks*);
using PfnMapMemory = Result(APUSR_VK_CALL*)(Device, DeviceMemory, DeviceSize, DeviceSize, MemoryMapFlags, void**);
using PfnUnmapMemory = void(APUSR_VK_CALL*)(Device, DeviceMemory);
using PfnCreateImageView = Result(APUSR_VK_CALL*)(Device, const ImageViewCreateInfo*, const AllocationCallbacks*, ImageView*);
using PfnDestroyImageView = void(APUSR_VK_CALL*)(Device, ImageView, const AllocationCallbacks*);
using PfnCreateShaderModule = Result(APUSR_VK_CALL*)(Device, const ShaderModuleCreateInfo*, const AllocationCallbacks*, ShaderModule*);
using PfnDestroyShaderModule = void(APUSR_VK_CALL*)(Device, ShaderModule, const AllocationCallbacks*);
using PfnCreateDescriptorSetLayout = Result(APUSR_VK_CALL*)(Device, const DescriptorSetLayoutCreateInfo*, const AllocationCallbacks*, DescriptorSetLayout*);
using PfnDestroyDescriptorSetLayout = void(APUSR_VK_CALL*)(Device, DescriptorSetLayout, const AllocationCallbacks*);
using PfnCreatePipelineLayout = Result(APUSR_VK_CALL*)(Device, const PipelineLayoutCreateInfo*, const AllocationCallbacks*, PipelineLayout*);
using PfnDestroyPipelineLayout = void(APUSR_VK_CALL*)(Device, PipelineLayout, const AllocationCallbacks*);
using PfnCreateComputePipelines = Result(APUSR_VK_CALL*)(Device, PipelineCache, std::uint32_t, const ComputePipelineCreateInfo*, const AllocationCallbacks*, Pipeline*);
using PfnDestroyPipeline = void(APUSR_VK_CALL*)(Device, Pipeline, const AllocationCallbacks*);
using PfnCreateDescriptorPool = Result(APUSR_VK_CALL*)(Device, const DescriptorPoolCreateInfo*, const AllocationCallbacks*, DescriptorPool*);
using PfnDestroyDescriptorPool = void(APUSR_VK_CALL*)(Device, DescriptorPool, const AllocationCallbacks*);
using PfnResetDescriptorPool = Result(APUSR_VK_CALL*)(Device, DescriptorPool, DescriptorPoolResetFlags);
using PfnAllocateDescriptorSets = Result(APUSR_VK_CALL*)(Device, const DescriptorSetAllocateInfo*, DescriptorSet*);
using PfnUpdateDescriptorSets = void(APUSR_VK_CALL*)(Device, std::uint32_t, const WriteDescriptorSet*, std::uint32_t, const CopyDescriptorSet*);
using PfnCreateSampler = Result(APUSR_VK_CALL*)(Device, const SamplerCreateInfo*, const AllocationCallbacks*, Sampler*);
using PfnDestroySampler = void(APUSR_VK_CALL*)(Device, Sampler, const AllocationCallbacks*);
using PfnCreateQueryPool = Result(APUSR_VK_CALL*)(Device, const QueryPoolCreateInfo*, const AllocationCallbacks*, QueryPool*);
using PfnDestroyQueryPool = void(APUSR_VK_CALL*)(Device, QueryPool, const AllocationCallbacks*);
using PfnGetQueryPoolResults = Result(APUSR_VK_CALL*)(Device, QueryPool, std::uint32_t, std::uint32_t, std::size_t, void*, DeviceSize, QueryResultFlags);
using PfnCmdResetQueryPool = void(APUSR_VK_CALL*)(CommandBuffer, QueryPool, std::uint32_t, std::uint32_t);
using PfnCmdWriteTimestamp = void(APUSR_VK_CALL*)(CommandBuffer, PipelineStageFlags, QueryPool, std::uint32_t);
using PfnDeviceWaitIdle = Result(APUSR_VK_CALL*)(Device);
using PfnCmdPipelineBarrier = void(APUSR_VK_CALL*)(CommandBuffer, PipelineStageFlags, PipelineStageFlags, DependencyFlags, std::uint32_t, const MemoryBarrier*, std::uint32_t, const BufferMemoryBarrier*, std::uint32_t, const ImageMemoryBarrier*);
using PfnCmdUpdateBuffer = void(APUSR_VK_CALL*)(CommandBuffer, Buffer, DeviceSize, DeviceSize, const void*);
using PfnCmdClearColorImage = void(APUSR_VK_CALL*)(CommandBuffer, Image, ImageLayout, const ClearColorValue*, std::uint32_t, const ImageSubresourceRange*);
using PfnCmdCopyBuffer = void(APUSR_VK_CALL*)(CommandBuffer, Buffer, Buffer, std::uint32_t, const BufferCopy*);
using PfnCmdCopyBufferToImage = void(APUSR_VK_CALL*)(CommandBuffer, Buffer, Image, ImageLayout, std::uint32_t, const BufferImageCopy*);
using PfnCmdCopyImage = void(APUSR_VK_CALL*)(CommandBuffer, Image, ImageLayout, Image, ImageLayout, std::uint32_t, const ImageCopy*);
using PfnCmdFillBuffer = void(APUSR_VK_CALL*)(CommandBuffer, Buffer, DeviceSize, DeviceSize, std::uint32_t);
using PfnCmdBindPipeline = void(APUSR_VK_CALL*)(CommandBuffer, PipelineBindPoint, Pipeline);
using PfnCmdBindDescriptorSets = void(APUSR_VK_CALL*)(CommandBuffer, PipelineBindPoint, PipelineLayout, std::uint32_t, std::uint32_t, const DescriptorSet*, std::uint32_t, const std::uint32_t*);
using PfnCmdDispatch = void(APUSR_VK_CALL*)(CommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t);

// Keep this private ABI honest when building without Vulkan SDK headers.
static_assert(sizeof(Extent3D) == 12);
static_assert(sizeof(QueueFamilyProperties) == 24);
static_assert(sizeof(PhysicalDeviceLimits) == 504);
static_assert(offsetof(PhysicalDeviceLimits, timestampPeriod) == 424);
static_assert(sizeof(PhysicalDeviceProperties) == 824);
static_assert(sizeof(Offset3D) == 12);
static_assert(sizeof(ImageSubresourceLayers) == 16);
static_assert(sizeof(BufferCopy) == 24);
static_assert(sizeof(BufferImageCopy) == 56);
static_assert(sizeof(ImageCopy) == 68);
static_assert(sizeof(QueryPoolCreateInfo) == 32);
static_assert(sizeof(CalibratedTimestampInfo) == 24);
static_assert(sizeof(CommandPoolCreateInfo) == 24);
static_assert(sizeof(CommandBufferAllocateInfo) == 32);
static_assert(sizeof(CommandBufferBeginInfo) == 32);
static_assert(sizeof(FenceCreateInfo) == 24);
static_assert(sizeof(SemaphoreCreateInfo) == 24);
static_assert(sizeof(SemaphoreTypeCreateInfo) == 32);
static_assert(sizeof(TimelineSemaphoreSubmitInfo) == 48);
static_assert(sizeof(SubmitInfo) == 72);
static_assert(sizeof(BufferCreateInfo) == 56);
static_assert(sizeof(ImageCreateInfo) == 88);
static_assert(sizeof(MemoryRequirements) == 24);
static_assert(sizeof(MemoryAllocateInfo) == 32);
static_assert(sizeof(PhysicalDeviceMemoryProperties) == 520);
static_assert(sizeof(PhysicalDeviceMemoryProperties2) == 536);
static_assert(sizeof(PhysicalDeviceMemoryBudgetPropertiesExt) == 272);
static_assert(sizeof(ImageViewCreateInfo) == 80);
static_assert(sizeof(ShaderModuleCreateInfo) == 40);
static_assert(sizeof(PipelineShaderStageRequiredSubgroupSizeCreateInfo) == 24);
static_assert(sizeof(PipelineShaderStageCreateInfo) == 48);
static_assert(sizeof(ComputePipelineCreateInfo) == 96);
static_assert(sizeof(WriteDescriptorSet) == 64);
static_assert(sizeof(SamplerCreateInfo) == 80);
static_assert(sizeof(ImageMemoryBarrier) == 72);
static_assert(sizeof(BufferMemoryBarrier) == 56);

} // namespace apusr::backends::vk

#undef APUSR_VK_CALL
