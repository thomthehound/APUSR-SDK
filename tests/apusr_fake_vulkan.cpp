// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "../src/backends/gfx_vulkan/apusr_vulkan_minimal.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

using namespace apusr::backends;

namespace {
std::atomic<std::uintptr_t> g_nextHandle{0x10000};
std::atomic<std::uint64_t> g_dispatches{0};
std::atomic<std::uint64_t> g_pipelines{0};
std::atomic<std::uint64_t> g_requiredSubgroup32Pipelines{0};
std::atomic<std::uint64_t> g_descriptorWrites{0};
std::atomic<std::uint64_t> g_updateBytes{0};
std::atomic<std::uint64_t> g_clears{0};
std::atomic<std::uint64_t> g_barriers{0};
std::atomic<std::uint64_t> g_barrierCalls{0};
std::atomic<std::uint64_t> g_computeBufferBarriers{0};
std::atomic<std::uint64_t> g_invalidDescriptorLayouts{0};
std::atomic<std::uint64_t> g_invalidOwnedSharing{0};
std::atomic<std::uint64_t> g_deviceWaitIdleCalls{0};
std::atomic<std::uint64_t> g_timestampWrites{0};
std::atomic<std::uint64_t> g_timestampClock{1000};
std::mutex g_queryMutex;
struct FakeQueryPoolState {
    std::array<std::uint64_t, 64> values{};
    std::array<std::uint64_t, 64> available{};
};
std::unordered_map<std::uintptr_t, FakeQueryPoolState> g_queryPools;

template <typename T>
T fakeHandle()
{
    return reinterpret_cast<T>(g_nextHandle.fetch_add(0x10, std::memory_order_relaxed));
}

bool validOwnedSharing(const vk::SharingMode mode, const std::uint32_t count, const std::uint32_t* families)
{
    if (mode != vk::SharingModeConcurrent || count != 2u || families == nullptr) {
        return false;
    }
    return (families[0] == 3u && families[1] == 5u) || (families[0] == 5u && families[1] == 3u);
}

extern "C" vk::Result fakeCreateBuffer(vk::Device, const vk::BufferCreateInfo* info, const void*, vk::Buffer* out)
{
    if (info == nullptr || !validOwnedSharing(info->sharingMode, info->queueFamilyIndexCount, info->pQueueFamilyIndices)) {
        g_invalidOwnedSharing.fetch_add(1, std::memory_order_relaxed);
    }
    *out = fakeHandle<vk::Buffer>();
    return vk::Success;
}
extern "C" void fakeDestroyBuffer(vk::Device, vk::Buffer, const void*) {}
extern "C" void fakeGetBufferMemoryRequirements(vk::Device, vk::Buffer, vk::MemoryRequirements* out)
{
    *out = vk::MemoryRequirements{.size = 4096, .alignment = 256, .memoryTypeBits = 1};
}
extern "C" vk::Result fakeBindBufferMemory(vk::Device, vk::Buffer, vk::DeviceMemory, vk::DeviceSize) { return vk::Success; }
extern "C" vk::Result fakeCreateImage(vk::Device, const vk::ImageCreateInfo* info, const void*, vk::Image* out)
{
    if (info == nullptr || !validOwnedSharing(info->sharingMode, info->queueFamilyIndexCount, info->pQueueFamilyIndices)) {
        g_invalidOwnedSharing.fetch_add(1, std::memory_order_relaxed);
    }
    *out = fakeHandle<vk::Image>();
    return vk::Success;
}
extern "C" void fakeDestroyImage(vk::Device, vk::Image, const void*) {}
extern "C" void fakeGetImageMemoryRequirements(vk::Device, vk::Image, vk::MemoryRequirements* out)
{
    *out = vk::MemoryRequirements{.size = 4096, .alignment = 256, .memoryTypeBits = 1};
}
extern "C" vk::Result fakeBindImageMemory(vk::Device, vk::Image, vk::DeviceMemory, vk::DeviceSize) { return vk::Success; }
extern "C" vk::Result fakeAllocateMemory(vk::Device, const vk::MemoryAllocateInfo*, const void*, vk::DeviceMemory* out)
{
    *out = fakeHandle<vk::DeviceMemory>();
    return vk::Success;
}
extern "C" void fakeFreeMemory(vk::Device, vk::DeviceMemory, const void*) {}
extern "C" vk::Result fakeCreateImageView(vk::Device, const vk::ImageViewCreateInfo*, const void*, vk::ImageView* out)
{
    *out = fakeHandle<vk::ImageView>();
    return vk::Success;
}
extern "C" void fakeDestroyImageView(vk::Device, vk::ImageView, const void*) {}
extern "C" vk::Result fakeCreateShaderModule(vk::Device, const vk::ShaderModuleCreateInfo*, const void*, vk::ShaderModule* out)
{
    *out = fakeHandle<vk::ShaderModule>();
    return vk::Success;
}
extern "C" void fakeDestroyShaderModule(vk::Device, vk::ShaderModule, const void*) {}
extern "C" vk::Result fakeCreateDescriptorSetLayout(vk::Device, const vk::DescriptorSetLayoutCreateInfo*, const void*, vk::DescriptorSetLayout* out)
{
    *out = fakeHandle<vk::DescriptorSetLayout>();
    return vk::Success;
}
extern "C" void fakeDestroyDescriptorSetLayout(vk::Device, vk::DescriptorSetLayout, const void*) {}
extern "C" vk::Result fakeCreatePipelineLayout(vk::Device, const vk::PipelineLayoutCreateInfo*, const void*, vk::PipelineLayout* out)
{
    *out = fakeHandle<vk::PipelineLayout>();
    return vk::Success;
}
extern "C" void fakeDestroyPipelineLayout(vk::Device, vk::PipelineLayout, const void*) {}
extern "C" vk::Result fakeCreateComputePipelines(
    vk::Device,
    vk::PipelineCache,
    std::uint32_t count,
    const vk::ComputePipelineCreateInfo* infos,
    const void*,
    vk::Pipeline* out)
{
    for (std::uint32_t i = 0; i < count; ++i) {
        if (infos != nullptr && infos[i].stage.pNext != nullptr) {
            const auto* subgroup = static_cast<const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo*>(
                infos[i].stage.pNext);
            if (subgroup->sType == vk::StructureTypePipelineShaderStageRequiredSubgroupSizeCreateInfo &&
                subgroup->requiredSubgroupSize == 32u) {
                g_requiredSubgroup32Pipelines.fetch_add(1u, std::memory_order_relaxed);
            }
        }
        out[i] = fakeHandle<vk::Pipeline>();
    }
    g_pipelines.fetch_add(count, std::memory_order_relaxed);
    return vk::Success;
}
extern "C" void fakeDestroyPipeline(vk::Device, vk::Pipeline, const void*) {}
extern "C" vk::Result fakeCreateDescriptorPool(vk::Device, const vk::DescriptorPoolCreateInfo*, const void*, vk::DescriptorPool* out)
{
    *out = fakeHandle<vk::DescriptorPool>();
    return vk::Success;
}
extern "C" void fakeDestroyDescriptorPool(vk::Device, vk::DescriptorPool, const void*) {}
extern "C" vk::Result fakeAllocateDescriptorSets(vk::Device, const vk::DescriptorSetAllocateInfo* info, vk::DescriptorSet* out)
{
    for (std::uint32_t i = 0; i < info->descriptorSetCount; ++i) {
        out[i] = fakeHandle<vk::DescriptorSet>();
    }
    return vk::Success;
}
extern "C" void fakeUpdateDescriptorSets(vk::Device, std::uint32_t count, const vk::WriteDescriptorSet* writes, std::uint32_t, const void*)
{
    g_descriptorWrites.fetch_add(count, std::memory_order_relaxed);
    for (std::uint32_t index = 0; index < count; ++index) {
        const vk::WriteDescriptorSet& write = writes[index];
        if (write.pImageInfo == nullptr) {
            continue;
        }
        const std::uintptr_t view = reinterpret_cast<std::uintptr_t>(write.pImageInfo->imageView);
        const bool externalSampled = view == 0x5100u || view == 0x5300u || view == 0x5500u;
        const bool externalStorage = view == 0x5700u;
        if (externalSampled && write.pImageInfo->imageLayout != vk::ImageLayoutShaderReadOnlyOptimal) {
            g_invalidDescriptorLayouts.fetch_add(1, std::memory_order_relaxed);
        }
        if (externalStorage && write.pImageInfo->imageLayout != vk::ImageLayoutGeneral) {
            g_invalidDescriptorLayouts.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
extern "C" vk::Result fakeCreateSampler(vk::Device, const vk::SamplerCreateInfo*, const void*, vk::Sampler* out)
{
    *out = fakeHandle<vk::Sampler>();
    return vk::Success;
}
extern "C" void fakeDestroySampler(vk::Device, vk::Sampler, const void*) {}
extern "C" vk::Result fakeCreateQueryPool(vk::Device, const vk::QueryPoolCreateInfo* info, const void*, vk::QueryPool* out)
{
    if (info == nullptr || info->queryType != vk::QueryTypeTimestamp || info->queryCount > 64u) {
        return -1;
    }
    *out = fakeHandle<vk::QueryPool>();
    std::lock_guard<std::mutex> lock(g_queryMutex);
    g_queryPools.emplace(reinterpret_cast<std::uintptr_t>(*out), FakeQueryPoolState{});
    return vk::Success;
}
extern "C" void fakeDestroyQueryPool(vk::Device, vk::QueryPool pool, const void*)
{
    std::lock_guard<std::mutex> lock(g_queryMutex);
    g_queryPools.erase(reinterpret_cast<std::uintptr_t>(pool));
}
extern "C" vk::Result fakeGetQueryPoolResults(
    vk::Device,
    vk::QueryPool pool,
    std::uint32_t first,
    std::uint32_t count,
    std::size_t dataSize,
    void* data,
    vk::DeviceSize stride,
    vk::QueryResultFlags flags)
{
    if (data == nullptr || (flags & vk::QueryResult64Bit) == 0u || stride < sizeof(std::uint64_t)) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_queryMutex);
    const auto it = g_queryPools.find(reinterpret_cast<std::uintptr_t>(pool));
    if (it == g_queryPools.end() || first + count > it->second.values.size() || dataSize < count * stride) {
        return -1;
    }
    bool allAvailable = true;
    for (std::uint32_t index = 0; index < count; ++index) {
        std::byte* dst = static_cast<std::byte*>(data) + static_cast<std::size_t>(index) * stride;
        const std::uint32_t query = first + index;
        std::memcpy(dst, &it->second.values[query], sizeof(std::uint64_t));
        if ((flags & vk::QueryResultWithAvailabilityBit) != 0u) {
            std::memcpy(dst + sizeof(std::uint64_t), &it->second.available[query], sizeof(std::uint64_t));
        }
        allAvailable &= it->second.available[query] != 0u;
    }
    return allAvailable ? vk::Success : vk::NotReady;
}
extern "C" void fakeCmdResetQueryPool(vk::CommandBuffer, vk::QueryPool pool, std::uint32_t first, std::uint32_t count)
{
    std::lock_guard<std::mutex> lock(g_queryMutex);
    auto& state = g_queryPools[reinterpret_cast<std::uintptr_t>(pool)];
    for (std::uint32_t index = first; index < first + count && index < state.values.size(); ++index) {
        state.values[index] = 0u;
        state.available[index] = 0u;
    }
}
extern "C" void fakeCmdWriteTimestamp(vk::CommandBuffer, vk::PipelineStageFlags, vk::QueryPool pool, std::uint32_t query)
{
    std::lock_guard<std::mutex> lock(g_queryMutex);
    auto& state = g_queryPools[reinterpret_cast<std::uintptr_t>(pool)];
    if (query < state.values.size()) {
        state.values[query] = g_timestampClock.fetch_add(100u, std::memory_order_relaxed);
        state.available[query] = 1u;
        g_timestampWrites.fetch_add(1u, std::memory_order_relaxed);
    }
}

extern "C" vk::Result fakeDeviceWaitIdle(vk::Device)
{
    g_deviceWaitIdleCalls.fetch_add(1, std::memory_order_relaxed);
    return vk::Success;
}
extern "C" void fakeCmdPipelineBarrier(vk::CommandBuffer, vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage, vk::DependencyFlags, std::uint32_t, const void*, std::uint32_t buffers, const vk::BufferMemoryBarrier*, std::uint32_t images, const vk::ImageMemoryBarrier*)
{
    g_barrierCalls.fetch_add(1, std::memory_order_relaxed);
    g_barriers.fetch_add(buffers + images, std::memory_order_relaxed);
    if (srcStage == vk::PipelineStageComputeShaderBit && dstStage == vk::PipelineStageComputeShaderBit) {
        g_computeBufferBarriers.fetch_add(buffers, std::memory_order_relaxed);
    }
}
extern "C" void fakeCmdUpdateBuffer(vk::CommandBuffer, vk::Buffer, vk::DeviceSize, vk::DeviceSize size, const void*)
{
    g_updateBytes.fetch_add(size, std::memory_order_relaxed);
}
extern "C" void fakeCmdClearColorImage(vk::CommandBuffer, vk::Image, vk::ImageLayout, const vk::ClearColorValue*, std::uint32_t, const vk::ImageSubresourceRange*)
{
    g_clears.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void fakeCmdBindPipeline(vk::CommandBuffer, vk::PipelineBindPoint, vk::Pipeline) {}
extern "C" void fakeCmdBindDescriptorSets(vk::CommandBuffer, vk::PipelineBindPoint, vk::PipelineLayout, std::uint32_t, std::uint32_t, const vk::DescriptorSet*, std::uint32_t, const std::uint32_t*) {}
extern "C" void fakeCmdDispatch(vk::CommandBuffer, std::uint32_t, std::uint32_t, std::uint32_t)
{
    g_dispatches.fetch_add(1, std::memory_order_relaxed);
}

vk::PfnVoidFunction lookup(const char* name)
{
#define APUSR_FAKE_VK(nameLiteral, fn) if (std::strcmp(name, nameLiteral) == 0) return reinterpret_cast<vk::PfnVoidFunction>(fn)
    APUSR_FAKE_VK("vkCreateBuffer", fakeCreateBuffer);
    APUSR_FAKE_VK("vkDestroyBuffer", fakeDestroyBuffer);
    APUSR_FAKE_VK("vkGetBufferMemoryRequirements", fakeGetBufferMemoryRequirements);
    APUSR_FAKE_VK("vkBindBufferMemory", fakeBindBufferMemory);
    APUSR_FAKE_VK("vkCreateImage", fakeCreateImage);
    APUSR_FAKE_VK("vkDestroyImage", fakeDestroyImage);
    APUSR_FAKE_VK("vkGetImageMemoryRequirements", fakeGetImageMemoryRequirements);
    APUSR_FAKE_VK("vkBindImageMemory", fakeBindImageMemory);
    APUSR_FAKE_VK("vkAllocateMemory", fakeAllocateMemory);
    APUSR_FAKE_VK("vkFreeMemory", fakeFreeMemory);
    APUSR_FAKE_VK("vkCreateImageView", fakeCreateImageView);
    APUSR_FAKE_VK("vkDestroyImageView", fakeDestroyImageView);
    APUSR_FAKE_VK("vkCreateShaderModule", fakeCreateShaderModule);
    APUSR_FAKE_VK("vkDestroyShaderModule", fakeDestroyShaderModule);
    APUSR_FAKE_VK("vkCreateDescriptorSetLayout", fakeCreateDescriptorSetLayout);
    APUSR_FAKE_VK("vkDestroyDescriptorSetLayout", fakeDestroyDescriptorSetLayout);
    APUSR_FAKE_VK("vkCreatePipelineLayout", fakeCreatePipelineLayout);
    APUSR_FAKE_VK("vkDestroyPipelineLayout", fakeDestroyPipelineLayout);
    APUSR_FAKE_VK("vkCreateComputePipelines", fakeCreateComputePipelines);
    APUSR_FAKE_VK("vkDestroyPipeline", fakeDestroyPipeline);
    APUSR_FAKE_VK("vkCreateDescriptorPool", fakeCreateDescriptorPool);
    APUSR_FAKE_VK("vkDestroyDescriptorPool", fakeDestroyDescriptorPool);
    APUSR_FAKE_VK("vkAllocateDescriptorSets", fakeAllocateDescriptorSets);
    APUSR_FAKE_VK("vkUpdateDescriptorSets", fakeUpdateDescriptorSets);
    APUSR_FAKE_VK("vkCreateSampler", fakeCreateSampler);
    APUSR_FAKE_VK("vkDestroySampler", fakeDestroySampler);
    APUSR_FAKE_VK("vkCreateQueryPool", fakeCreateQueryPool);
    APUSR_FAKE_VK("vkDestroyQueryPool", fakeDestroyQueryPool);
    APUSR_FAKE_VK("vkGetQueryPoolResults", fakeGetQueryPoolResults);
    APUSR_FAKE_VK("vkCmdResetQueryPool", fakeCmdResetQueryPool);
    APUSR_FAKE_VK("vkCmdWriteTimestamp", fakeCmdWriteTimestamp);
    APUSR_FAKE_VK("vkDeviceWaitIdle", fakeDeviceWaitIdle);
    APUSR_FAKE_VK("vkCmdPipelineBarrier", fakeCmdPipelineBarrier);
    APUSR_FAKE_VK("vkCmdUpdateBuffer", fakeCmdUpdateBuffer);
    APUSR_FAKE_VK("vkCmdClearColorImage", fakeCmdClearColorImage);
    APUSR_FAKE_VK("vkCmdBindPipeline", fakeCmdBindPipeline);
    APUSR_FAKE_VK("vkCmdBindDescriptorSets", fakeCmdBindDescriptorSets);
    APUSR_FAKE_VK("vkCmdDispatch", fakeCmdDispatch);
#undef APUSR_FAKE_VK
    return nullptr;
}
} // namespace

extern "C" vk::PfnVoidFunction vkGetDeviceProcAddr(vk::Device, const char* name)
{
    return name != nullptr ? lookup(name) : nullptr;
}

extern "C" void vkGetPhysicalDeviceProperties(vk::PhysicalDevice, vk::PhysicalDeviceProperties* out)
{
    *out = {};
    out->vendorID = 0x1002u;
    out->deviceID = 0x1234u;
    out->limits.timestampComputeAndGraphics = 1u;
    out->limits.timestampPeriod = 1.0f;
}

extern "C" void vkGetPhysicalDeviceMemoryProperties(vk::PhysicalDevice, vk::PhysicalDeviceMemoryProperties* out)
{
    *out = {};
    out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags = vk::MemoryPropertyDeviceLocalBit;
    out->memoryTypes[0].heapIndex = 0;
    out->memoryHeapCount = 1;
    out->memoryHeaps[0].size = 8ull * 1024ull * 1024ull * 1024ull;
}

extern "C" void vkGetPhysicalDeviceFormatProperties(vk::PhysicalDevice, vk::Format, vk::FormatProperties* out)
{
    *out = {};
    out->optimalTilingFeatures = vk::FormatFeatureSampledImageBit | vk::FormatFeatureStorageImageBit | vk::FormatFeatureTransferDstBit;
}

extern "C" void vkGetPhysicalDeviceQueueFamilyProperties(vk::PhysicalDevice, std::uint32_t* count, vk::QueueFamilyProperties* out)
{
    if (count == nullptr) {
        return;
    }
    constexpr std::uint32_t kCount = 6u;
    if (out == nullptr) {
        *count = kCount;
        return;
    }
    const std::uint32_t writeCount = *count < kCount ? *count : kCount;
    for (std::uint32_t index = 0u; index < writeCount; ++index) {
        out[index] = {};
        out[index].queueCount = 8u;
        out[index].timestampValidBits = 64u;
    }
    *count = writeCount;
}

extern "C" vk::PfnVoidFunction vkGetInstanceProcAddr(vk::Instance, const char* name)
{
    if (name == nullptr) {
        return nullptr;
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<vk::PfnVoidFunction>(vkGetDeviceProcAddr);
    }
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties") == 0) {
        return reinterpret_cast<vk::PfnVoidFunction>(vkGetPhysicalDeviceProperties);
    }
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) {
        return reinterpret_cast<vk::PfnVoidFunction>(vkGetPhysicalDeviceMemoryProperties);
    }
    if (std::strcmp(name, "vkGetPhysicalDeviceFormatProperties") == 0) {
        return reinterpret_cast<vk::PfnVoidFunction>(vkGetPhysicalDeviceFormatProperties);
    }
    if (std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) {
        return reinterpret_cast<vk::PfnVoidFunction>(vkGetPhysicalDeviceQueueFamilyProperties);
    }
    return lookup(name);
}

extern "C" void apusrFakeVulkanResetCounters()
{
    g_dispatches = 0;
    g_pipelines = 0;
    g_requiredSubgroup32Pipelines = 0;
    g_descriptorWrites = 0;
    g_updateBytes = 0;
    g_clears = 0;
    g_barriers = 0;
    g_computeBufferBarriers = 0;
    g_invalidDescriptorLayouts = 0;
    g_invalidOwnedSharing = 0;
    g_deviceWaitIdleCalls = 0;
    g_timestampWrites = 0;
    g_timestampClock = 1000;
    {
        std::lock_guard<std::mutex> lock(g_queryMutex);
        g_queryPools.clear();
    }
}
extern "C" std::uint64_t apusrFakeVulkanDispatchCount() { return g_dispatches.load(); }
extern "C" std::uint64_t apusrFakeVulkanPipelineCount() { return g_pipelines.load(); }
extern "C" std::uint64_t apusrFakeVulkanRequiredSubgroup32PipelineCount()
{
    return g_requiredSubgroup32Pipelines.load();
}
extern "C" std::uint64_t apusrFakeVulkanDescriptorWriteCount() { return g_descriptorWrites.load(); }
extern "C" std::uint64_t apusrFakeVulkanUpdateBytes() { return g_updateBytes.load(); }
extern "C" std::uint64_t apusrFakeVulkanClearCount() { return g_clears.load(); }
extern "C" std::uint64_t apusrFakeVulkanBarrierCount() { return g_barriers.load(); }
extern "C" std::uint64_t apusrFakeVulkanBarrierCallCount() { return g_barrierCalls.load(); }
extern "C" std::uint64_t apusrFakeVulkanComputeBufferBarrierCount() { return g_computeBufferBarriers.load(); }
extern "C" std::uint64_t apusrFakeVulkanInvalidDescriptorLayoutCount() { return g_invalidDescriptorLayouts.load(); }
extern "C" std::uint64_t apusrFakeVulkanInvalidOwnedSharingCount() { return g_invalidOwnedSharing.load(); }
extern "C" std::uint64_t apusrFakeVulkanDeviceWaitIdleCount() { return g_deviceWaitIdleCalls.load(); }
extern "C" std::uint64_t apusrFakeVulkanTimestampWriteCount() { return g_timestampWrites.load(); }
