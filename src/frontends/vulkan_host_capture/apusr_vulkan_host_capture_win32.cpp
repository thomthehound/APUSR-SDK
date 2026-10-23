// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "apusr_vulkan_host_capture_abi.h"
#include "apusr_vulkan_host_capture_core.h"
#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"
#include "../common/apusr_host_discovery_log.h"
#include <apusr/apusr_profiler.h>

#include <detours.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

using apusr::vulkan_host_capture::VkAllocationCallbacks;
using apusr::vulkan_host_capture::VkDevice;
using apusr::vulkan_host_capture::VkDeviceCreateInfoLoose;
using apusr::vulkan_host_capture::VkInstance;
using apusr::vulkan_host_capture::VkInstanceCreateInfoLoose;
using apusr::vulkan_host_capture::VkPhysicalDevice;
using apusr::vulkan_host_capture::VkPhysicalDeviceGroupPropertiesLoose;
using apusr::vulkan_host_capture::VkQueueFamilyPropertiesLoose;
using apusr::vulkan_host_capture::VkQueue;
using apusr::vulkan_host_capture::VkSwapchainKHR;
using apusr::vulkan_host_capture::VkSwapchainCreateInfoKHRLoose;
using apusr::vulkan_host_capture::VkAcquireNextImageInfoKHRLoose;
using apusr::vulkan_host_capture::VkSemaphore;
using apusr::vulkan_host_capture::VkFence;
using apusr::vulkan_host_capture::VkImage;
using apusr::vulkan_host_capture::VkResult;
using apusr::vulkan_host_capture::VkVoidFunction;

#define APUSR_VK_LOADER_ENTRY extern "C"

// These are the stable process-visible entry points used both as Detours targets
// and as return values from vkGet*ProcAddr. Keep their declarations at global
// scope so their C linkage exactly matches the exported definitions below.
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateInstance(
    const VkInstanceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDevice* device);
APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator);
APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkEnumeratePhysicalDevices(
    VkInstance instance, std::uint32_t* count, VkPhysicalDevice* devices);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkEnumeratePhysicalDeviceGroups(
    VkInstance instance, std::uint32_t* count, VkPhysicalDeviceGroupPropertiesLoose* groups);
APUSR_VK_LOADER_ENTRY void WINAPI vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice, std::uint32_t* count, VkQueueFamilyPropertiesLoose* properties);
APUSR_VK_LOADER_ENTRY VkVoidFunction WINAPI vkGetInstanceProcAddr(VkInstance instance, const char* name);
APUSR_VK_LOADER_ENTRY VkVoidFunction WINAPI vkGetDeviceProcAddr(VkDevice device, const char* name);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkQueuePresentKHR(VkQueue queue, const void* presentInfo);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHRLoose* createInfo,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain);
APUSR_VK_LOADER_ENTRY void WINAPI vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t* count, VkImage* images);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* imageIndex);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkAcquireNextImage2KHR(
    VkDevice device, const VkAcquireNextImageInfoKHRLoose* acquireInfo, std::uint32_t* imageIndex);

namespace vk = apusr::backends::vk;

struct ApusrVkPipelineCacheCreateInfoLoose {
    vk::StructureType sType;
    const void* pNext;
    vk::Flags flags;
    std::size_t initialDataSize;
    const void* pInitialData;
};
static_assert(sizeof(ApusrVkPipelineCacheCreateInfoLoose) == 40);
static_assert(offsetof(ApusrVkPipelineCacheCreateInfoLoose, initialDataSize) == 24);

using ApusrVkCreateGraphicsPipelinesFn = VkResult (WINAPI *)(
    vk::Device, vk::PipelineCache, std::uint32_t, const void*,
    const vk::AllocationCallbacks*, vk::Pipeline*);
using ApusrVkCreatePipelineCacheFn = VkResult (WINAPI *)(
    vk::Device, const ApusrVkPipelineCacheCreateInfoLoose*,
    const vk::AllocationCallbacks*, vk::PipelineCache*);
using ApusrVkDestroyPipelineCacheFn = void (WINAPI *)(
    vk::Device, vk::PipelineCache, const vk::AllocationCallbacks*);
using ApusrVkGetPipelineCacheDataFn = VkResult (WINAPI *)(
    vk::Device, vk::PipelineCache, std::size_t*, void*);
using ApusrVkMergePipelineCachesFn = VkResult (WINAPI *)(
    vk::Device, vk::PipelineCache, std::uint32_t, const vk::PipelineCache*);

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateShaderModule(
    vk::Device device, const vk::ShaderModuleCreateInfo* createInfo,
    const vk::AllocationCallbacks* allocator, vk::ShaderModule* shaderModule);
APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyShaderModule(
    vk::Device device, vk::ShaderModule shaderModule, const vk::AllocationCallbacks* allocator);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateComputePipelines(
    vk::Device device, vk::PipelineCache pipelineCache, std::uint32_t createInfoCount,
    const vk::ComputePipelineCreateInfo* createInfos, const vk::AllocationCallbacks* allocator,
    vk::Pipeline* pipelines);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateGraphicsPipelines(
    vk::Device device, vk::PipelineCache pipelineCache, std::uint32_t createInfoCount,
    const void* createInfos, const vk::AllocationCallbacks* allocator, vk::Pipeline* pipelines);
APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyPipeline(
    vk::Device device, vk::Pipeline pipeline, const vk::AllocationCallbacks* allocator);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreatePipelineCache(
    vk::Device device, const ApusrVkPipelineCacheCreateInfoLoose* createInfo,
    const vk::AllocationCallbacks* allocator, vk::PipelineCache* pipelineCache);
APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyPipelineCache(
    vk::Device device, vk::PipelineCache pipelineCache, const vk::AllocationCallbacks* allocator);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkGetPipelineCacheData(
    vk::Device device, vk::PipelineCache pipelineCache, std::size_t* dataSize, void* data);
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkMergePipelineCaches(
    vk::Device device, vk::PipelineCache dstCache, std::uint32_t srcCacheCount,
    const vk::PipelineCache* srcCaches);

APUSR_VK_LOADER_ENTRY void WINAPI vkCmdPipelineBarrier(
    vk::CommandBuffer commandBuffer,
    vk::PipelineStageFlags srcStageMask,
    vk::PipelineStageFlags dstStageMask,
    vk::DependencyFlags dependencyFlags,
    std::uint32_t memoryBarrierCount, const vk::MemoryBarrier* memoryBarriers,
    std::uint32_t bufferMemoryBarrierCount, const vk::BufferMemoryBarrier* bufferMemoryBarriers,
    std::uint32_t imageMemoryBarrierCount, const vk::ImageMemoryBarrier* imageMemoryBarriers);

struct ApusrVkAttachmentDescriptionLoose {
    std::uint32_t flags;
    std::int32_t format;
    std::uint32_t samples;
    std::int32_t loadOp;
    std::int32_t storeOp;
    std::int32_t stencilLoadOp;
    std::int32_t stencilStoreOp;
    std::int32_t initialLayout;
    std::int32_t finalLayout;
};
struct ApusrVkRenderPassCreateInfoLoose {
    std::int32_t sType;
    const void* pNext;
    std::uint32_t flags;
    std::uint32_t attachmentCount;
    const ApusrVkAttachmentDescriptionLoose* pAttachments;
    std::uint32_t subpassCount;
    const void* pSubpasses;
    std::uint32_t dependencyCount;
    const void* pDependencies;
};
using ApusrVkRenderPass = void*;
APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateRenderPass(
    VkDevice device, const ApusrVkRenderPassCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator, ApusrVkRenderPass* renderPass);

namespace {

using namespace apusr::vulkan_host_capture;

HMODULE g_vulkanLoader = nullptr;
VkCreateInstanceFn g_vkCreateInstance = nullptr;
VkCreateDeviceFn g_vkCreateDevice = nullptr;
VkDestroyInstanceFn g_vkDestroyInstance = nullptr;
VkDestroyDeviceFn g_vkDestroyDevice = nullptr;
VkEnumeratePhysicalDevicesFn g_vkEnumeratePhysicalDevices = nullptr;
VkEnumeratePhysicalDeviceGroupsFn g_vkEnumeratePhysicalDeviceGroups = nullptr;
VkGetPhysicalDeviceQueueFamilyPropertiesFn g_vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
VkGetInstanceProcAddrFn g_vkGetInstanceProcAddr = nullptr;
VkGetDeviceProcAddrFn g_vkGetDeviceProcAddr = nullptr;
VkQueuePresentKHRFn g_vkQueuePresentKHR = nullptr;
VkCreateSwapchainKHRFn g_vkCreateSwapchainKHR = nullptr;
VkDestroySwapchainKHRFn g_vkDestroySwapchainKHR = nullptr;
VkGetSwapchainImagesKHRFn g_vkGetSwapchainImagesKHR = nullptr;
VkAcquireNextImageKHRFn g_vkAcquireNextImageKHR = nullptr;
VkAcquireNextImage2KHRFn g_vkAcquireNextImage2KHR = nullptr;
vk::PfnCmdPipelineBarrier g_vkCmdPipelineBarrier = nullptr;
using ApusrVkCreateRenderPassFn = VkResult (WINAPI *)(
    VkDevice, const ApusrVkRenderPassCreateInfoLoose*, const VkAllocationCallbacks*, ApusrVkRenderPass*);
ApusrVkCreateRenderPassFn g_vkCreateRenderPass = nullptr;
vk::PfnCreateShaderModule g_vkCreateShaderModule = nullptr;
vk::PfnDestroyShaderModule g_vkDestroyShaderModule = nullptr;
vk::PfnCreateComputePipelines g_vkCreateComputePipelines = nullptr;
ApusrVkCreateGraphicsPipelinesFn g_vkCreateGraphicsPipelines = nullptr;
vk::PfnDestroyPipeline g_vkDestroyPipeline = nullptr;
ApusrVkCreatePipelineCacheFn g_vkCreatePipelineCache = nullptr;
ApusrVkDestroyPipelineCacheFn g_vkDestroyPipelineCache = nullptr;
ApusrVkGetPipelineCacheDataFn g_vkGetPipelineCacheData = nullptr;
ApusrVkMergePipelineCachesFn g_vkMergePipelineCaches = nullptr;
std::atomic<std::uint32_t> g_virtualBarrierRewriteLogs{0u};
std::atomic<std::uint32_t> g_streamlineDlssBarrierNarrowLogs{0u};
std::atomic<std::uint32_t> g_virtualRenderPassRewriteLogs{0u};

using StreamlineVulkanDlssEvaluationActiveFn = bool(__cdecl*)();
std::atomic<StreamlineVulkanDlssEvaluationActiveFn> g_streamlineVulkanDlssEvaluationActiveFn{nullptr};

std::mutex g_installMutex;
std::atomic<bool> g_hooksInstalled{false};
std::atomic<bool> g_coreReady{false};

std::mutex g_pipelineProfileMutex;
std::unordered_map<VkDevice, apusr::profile::StreamHandle> g_pipelineProfileStreams;
std::unordered_set<std::uintptr_t> g_pipelineProfileCallsiteModules;
std::atomic<std::uint64_t> g_pipelineProfileEventSequence{1u};

[[nodiscard]] void* pipelineCallerAddress() noexcept
{
#if defined(_MSC_VER)
    return _ReturnAddress();
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_return_address(0);
#else
    return nullptr;
#endif
}

[[nodiscard]] StreamlineVulkanDlssEvaluationActiveFn streamlineVulkanDlssEvaluationActiveFn() noexcept
{
    if (auto fn = g_streamlineVulkanDlssEvaluationActiveFn.load(std::memory_order_acquire))
        return fn;

    const HMODULE attach = GetModuleHandleW(L"apusr_streamline_attach.dll");
    if (attach == nullptr)
        return nullptr;
    const auto fn = reinterpret_cast<StreamlineVulkanDlssEvaluationActiveFn>(
        GetProcAddress(attach, "apusrStreamlineVulkanDlssEvaluationActive"));
    if (fn != nullptr)
        g_streamlineVulkanDlssEvaluationActiveFn.store(fn, std::memory_order_release);
    return fn;
}

[[nodiscard]] bool streamlineVulkanDlssEvaluationActive() noexcept
{
    const auto fn = streamlineVulkanDlssEvaluationActiveFn();
    return fn != nullptr && fn();
}

enum class StreamlineDlssBarrierDirection : std::uint8_t {
    None,
    IntoEvaluation,
    OutOfEvaluation,
};

[[nodiscard]] bool streamlineDlssPreparedState(
    const vk::ImageLayout layout,
    const vk::AccessFlags access) noexcept
{
    return (layout == vk::ImageLayoutShaderReadOnlyOptimal &&
            access == vk::AccessShaderReadBit) ||
        (layout == vk::ImageLayoutGeneral &&
         access == (vk::AccessShaderReadBit | vk::AccessShaderWriteBit));
}

[[nodiscard]] StreamlineDlssBarrierDirection classifyStreamlineDlssBarrier(
    const vk::ImageMemoryBarrier& barrier) noexcept
{
    const bool into = streamlineDlssPreparedState(barrier.newLayout, barrier.dstAccessMask);
    const bool out = streamlineDlssPreparedState(barrier.oldLayout, barrier.srcAccessMask);
    if (into && !out)
        return StreamlineDlssBarrierDirection::IntoEvaluation;
    if (out && !into)
        return StreamlineDlssBarrierDirection::OutOfEvaluation;
    if (!into || !out)
        return StreamlineDlssBarrierDirection::None;

    // A host can legitimately hand Streamline an output image in a shader-read
    // state, producing SHADER_READ -> GENERAL/RW on ingress and the exact reverse
    // on egress. In that otherwise ambiguous case, the write-access direction
    // identifies which side of the NGX boundary owns the compute write.
    const bool srcWrites = (barrier.srcAccessMask & vk::AccessShaderWriteBit) != 0u;
    const bool dstWrites = (barrier.dstAccessMask & vk::AccessShaderWriteBit) != 0u;
    if (dstWrites && !srcWrites)
        return StreamlineDlssBarrierDirection::IntoEvaluation;
    if (srcWrites && !dstWrites)
        return StreamlineDlssBarrierDirection::OutOfEvaluation;
    return StreamlineDlssBarrierDirection::None;
}

[[nodiscard]] StreamlineDlssBarrierDirection classifyStreamlineDlssBarrierBatch(
    const std::uint32_t imageMemoryBarrierCount,
    const vk::ImageMemoryBarrier* imageMemoryBarriers) noexcept
{
    if (imageMemoryBarrierCount == 0u || imageMemoryBarriers == nullptr)
        return StreamlineDlssBarrierDirection::None;
    StreamlineDlssBarrierDirection direction = StreamlineDlssBarrierDirection::None;
    for (std::uint32_t index = 0; index < imageMemoryBarrierCount; ++index) {
        const StreamlineDlssBarrierDirection current =
            classifyStreamlineDlssBarrier(imageMemoryBarriers[index]);
        if (current == StreamlineDlssBarrierDirection::None)
            return StreamlineDlssBarrierDirection::None;
        if (direction == StreamlineDlssBarrierDirection::None)
            direction = current;
        else if (direction != current)
            return StreamlineDlssBarrierDirection::None;
    }
    return direction;
}

[[nodiscard]] HMODULE moduleFromAddress(const void* address) noexcept
{
    if (address == nullptr)
        return nullptr;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) == FALSE)
        return nullptr;
    return module;
}

[[nodiscard]] std::string moduleBaseName(const HMODULE module)
{
    if (module == nullptr)
        return {};
    wchar_t path[1024]{};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0u || length >= std::size(path))
        return {};
    const wchar_t* base = path;
    for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/')
            base = cursor + 1;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, base, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, base, -1, utf8.data(), bytes, nullptr, nullptr);
    if (!utf8.empty() && utf8.back() == '\0')
        utf8.pop_back();
    return utf8;
}

[[nodiscard]] apusr::profile::StreamHandle pipelineProfileStream(const VkDevice device)
{
    if (!apusr::profile::enabled() || device == nullptr)
        return {};
    std::lock_guard<std::mutex> lock(g_pipelineProfileMutex);
    if (const auto it = g_pipelineProfileStreams.find(device); it != g_pipelineProfileStreams.end())
        return it->second;
    const auto stream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::GraphicsInterop,
        .name = "vulkan.pipeline_activity",
        .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
            apusr::profile::StreamCapabilityCounters |
            apusr::profile::StreamCapabilityMetadata,
        .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(device)),
    });
    g_pipelineProfileStreams.emplace(device, stream);
    return stream;
}

[[nodiscard]] std::uint64_t nextPipelineProfileEvent() noexcept
{
    return g_pipelineProfileEventSequence.fetch_add(1u, std::memory_order_relaxed);
}

[[nodiscard]] std::uint64_t hashPipelineBytes(const void* data, const std::size_t bytes) noexcept
{
    if (data == nullptr || bytes == 0u)
        return 0u;
    constexpr std::uint64_t kOffset = 1469598103934665603ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffset;
    const auto* ptr = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < bytes; ++index) {
        hash ^= ptr[index];
        hash *= kPrime;
    }
    return hash;
}

void recordPipelineCallsite(
    const apusr::profile::StreamHandle stream,
    const std::uint64_t eventId,
    const void* callerAddress) noexcept
{
    if (!stream || callerAddress == nullptr)
        return;
    const HMODULE module = moduleFromAddress(callerAddress);
    const std::uintptr_t moduleValue = reinterpret_cast<std::uintptr_t>(module);
    apusr::profile::recordCounter({
        .stream = stream,
        .frameId = eventId,
        .name = "vulkan.pipeline.callsite",
        .value = static_cast<std::uint64_t>(moduleValue),
    });
    if (module == nullptr)
        return;
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_pipelineProfileMutex);
        first = g_pipelineProfileCallsiteModules.insert(moduleValue).second;
    }
    if (!first)
        return;
    const std::string name = moduleBaseName(module);
    apusr::profile::recordMetadata({
        .stream = stream,
        .frameId = eventId,
        .name = "vulkan.pipeline.callsite_module",
        .detail = name.empty() ? "unknown" : name.c_str(),
        .value0 = static_cast<std::uint64_t>(moduleValue),
    });
}

template <typename Fn>
[[nodiscard]] Fn resolveDeviceFunction(const VkDevice device, Fn cached, const char* name) noexcept
{
    if (cached != nullptr)
        return cached;
    if (g_vkGetDeviceProcAddr == nullptr || device == nullptr || name == nullptr)
        return nullptr;
    return reinterpret_cast<Fn>(g_vkGetDeviceProcAddr(device, name));
}

[[nodiscard]] HMODULE loadVulkanLoader()
{
    if (g_vulkanLoader != nullptr) {
        return g_vulkanLoader;
    }

    // Prefer the module already selected by the host. This catches direct
    // imports and renderer-DLL imports without assuming the main executable
    // owns Vulkan. If Vulkan has not been loaded yet, seed the normal Windows
    // loader name so later imports/proc lookups converge on the same module.
    g_vulkanLoader = GetModuleHandleW(L"vulkan-1.dll");
    if (g_vulkanLoader == nullptr) {
        g_vulkanLoader = LoadLibraryW(L"vulkan-1.dll");
    }
    return g_vulkanLoader;
}

[[nodiscard]] bool pinModuleFromAddress(const void* address)
{
    if (address == nullptr) {
        return false;
    }
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(
               GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
               reinterpret_cast<LPCWSTR>(address),
               &pinned) != FALSE;
}

template <typename Fn>
[[nodiscard]] Fn moduleFn(HMODULE module, const char* name, const char* fallback = nullptr)
{
    if (module == nullptr || name == nullptr) {
        return nullptr;
    }
    FARPROC proc = GetProcAddress(module, name);
    if (proc == nullptr && fallback != nullptr) {
        proc = GetProcAddress(module, fallback);
    }
    return reinterpret_cast<Fn>(proc);
}

[[nodiscard]] VkVoidFunction interceptProc(const char* name) noexcept
{
    if (name == nullptr) {
        return nullptr;
    }
    if (std::strcmp(name, "vkCreateInstance") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCreateInstance);
    }
    if (std::strcmp(name, "vkCreateDevice") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCreateDevice);
    }
    if (std::strcmp(name, "vkDestroyInstance") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkDestroyInstance);
    }
    if (std::strcmp(name, "vkDestroyDevice") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkDestroyDevice);
    }
    if (std::strcmp(name, "vkEnumeratePhysicalDevices") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkEnumeratePhysicalDevices);
    }
    if (std::strcmp(name, "vkEnumeratePhysicalDeviceGroups") == 0 ||
        std::strcmp(name, "vkEnumeratePhysicalDeviceGroupsKHR") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkEnumeratePhysicalDeviceGroups);
    }
    if (std::strcmp(name, "vkGetPhysicalDeviceQueueFamilyProperties") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkGetPhysicalDeviceQueueFamilyProperties);
    }
    if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkGetInstanceProcAddr);
    }
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkGetDeviceProcAddr);
    }
    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkQueuePresentKHR);
    }
    if (std::strcmp(name, "vkCreateSwapchainKHR") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCreateSwapchainKHR);
    }
    if (std::strcmp(name, "vkDestroySwapchainKHR") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkDestroySwapchainKHR);
    }
    if (std::strcmp(name, "vkGetSwapchainImagesKHR") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkGetSwapchainImagesKHR);
    }
    if (std::strcmp(name, "vkAcquireNextImageKHR") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkAcquireNextImageKHR);
    }
    if (std::strcmp(name, "vkAcquireNextImage2KHR") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkAcquireNextImage2KHR);
    }
    if (std::strcmp(name, "vkCmdPipelineBarrier") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCmdPipelineBarrier);
    }
    if (std::strcmp(name, "vkCreateRenderPass") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCreateRenderPass);
    }
    if (std::strcmp(name, "vkCreateShaderModule") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCreateShaderModule);
    }
    if (std::strcmp(name, "vkDestroyShaderModule") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkDestroyShaderModule);
    }
    if (std::strcmp(name, "vkCreateComputePipelines") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCreateComputePipelines);
    }
    if (std::strcmp(name, "vkCreateGraphicsPipelines") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCreateGraphicsPipelines);
    }
    if (std::strcmp(name, "vkDestroyPipeline") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkDestroyPipeline);
    }
    if (std::strcmp(name, "vkCreatePipelineCache") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkCreatePipelineCache);
    }
    if (std::strcmp(name, "vkDestroyPipelineCache") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkDestroyPipelineCache);
    }
    if (std::strcmp(name, "vkGetPipelineCacheData") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkGetPipelineCacheData);
    }
    if (std::strcmp(name, "vkMergePipelineCaches") == 0) {
        return reinterpret_cast<VkVoidFunction>(&vkMergePipelineCaches);
    }
    return nullptr;
}

[[nodiscard]] bool configureCoreDispatch()
{
    HMODULE vulkan = loadVulkanLoader();
    if (vulkan == nullptr) {
        return false;
    }

    Dispatch dispatch{};
    dispatch.createInstance = g_vkCreateInstance;
    dispatch.createDevice = g_vkCreateDevice;
    dispatch.destroyInstance = g_vkDestroyInstance;
    dispatch.destroyDevice = g_vkDestroyDevice;
    dispatch.enumeratePhysicalDevices = g_vkEnumeratePhysicalDevices;
    dispatch.enumeratePhysicalDeviceGroups = g_vkEnumeratePhysicalDeviceGroups;
    dispatch.getInstanceProcAddr = g_vkGetInstanceProcAddr;
    dispatch.getDeviceProcAddr = g_vkGetDeviceProcAddr;
    dispatch.queuePresent = g_vkQueuePresentKHR;
    dispatch.getPhysicalDeviceFeatures2 = moduleFn<VkGetPhysicalDeviceFeatures2Fn>(
        vulkan, "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR");
    dispatch.getPhysicalDeviceProperties = moduleFn<VkGetPhysicalDevicePropertiesFn>(
        vulkan, "vkGetPhysicalDeviceProperties");
    dispatch.getPhysicalDeviceProperties2 = moduleFn<VkGetPhysicalDeviceProperties2Fn>(
        vulkan, "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR");
    dispatch.enumerateInstanceExtensionProperties = moduleFn<VkEnumerateInstanceExtensionPropertiesFn>(
        vulkan, "vkEnumerateInstanceExtensionProperties");
    dispatch.enumerateDeviceExtensionProperties = moduleFn<VkEnumerateDeviceExtensionPropertiesFn>(
        vulkan, "vkEnumerateDeviceExtensionProperties");
    dispatch.getPhysicalDeviceQueueFamilyProperties = g_vkGetPhysicalDeviceQueueFamilyProperties != nullptr
        ? g_vkGetPhysicalDeviceQueueFamilyProperties
        : moduleFn<VkGetPhysicalDeviceQueueFamilyPropertiesFn>(vulkan, "vkGetPhysicalDeviceQueueFamilyProperties");
    dispatch.getPhysicalDeviceSurfaceCapabilities = moduleFn<VkGetPhysicalDeviceSurfaceCapabilitiesKHRFn>(
        vulkan, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    dispatch.getPhysicalDeviceSurfaceCapabilities2 = moduleFn<VkGetPhysicalDeviceSurfaceCapabilities2KHRFn>(
        vulkan, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");

    configureDispatch(dispatch);
    return dispatchReady();
}

[[nodiscard]] bool attachOne(PVOID* target, PVOID replacement, const char* name)
{
    if (target == nullptr || *target == nullptr || replacement == nullptr) {
        return true;
    }
    const LONG result = DetourAttach(target, replacement);
    if (result != NO_ERROR) {
        apusr::host_discovery::eventf(
            "vulkan", "capture.hook_failed", "name=%s error=%ld", name, result);
        return false;
    }
    return true;
}

[[nodiscard]] bool installHooksLocked()
{
    apusr::host_discovery::loggingReady("vulkan_capture");
    apusr::host_discovery::moduleLoaded("vulkan_capture", reinterpret_cast<HMODULE>(&__ImageBase));
    apusr::host_discovery::event("vulkan", "capture.install_begin", "mode=system_loader_detours");

    HMODULE vulkan = loadVulkanLoader();
    if (vulkan == nullptr) {
        apusr::host_discovery::eventf(
            "vulkan", "capture.install_failed", "reason=system_vulkan_load error=%lu", GetLastError());
        return false;
    }

    g_vkCreateInstance = moduleFn<VkCreateInstanceFn>(vulkan, "vkCreateInstance");
    g_vkCreateDevice = moduleFn<VkCreateDeviceFn>(vulkan, "vkCreateDevice");
    g_vkDestroyInstance = moduleFn<VkDestroyInstanceFn>(vulkan, "vkDestroyInstance");
    g_vkDestroyDevice = moduleFn<VkDestroyDeviceFn>(vulkan, "vkDestroyDevice");
    g_vkEnumeratePhysicalDevices = moduleFn<VkEnumeratePhysicalDevicesFn>(vulkan, "vkEnumeratePhysicalDevices");
    g_vkEnumeratePhysicalDeviceGroups = moduleFn<VkEnumeratePhysicalDeviceGroupsFn>(
        vulkan, "vkEnumeratePhysicalDeviceGroups", "vkEnumeratePhysicalDeviceGroupsKHR");
    g_vkGetPhysicalDeviceQueueFamilyProperties = moduleFn<VkGetPhysicalDeviceQueueFamilyPropertiesFn>(
        vulkan, "vkGetPhysicalDeviceQueueFamilyProperties");
    g_vkGetInstanceProcAddr = moduleFn<VkGetInstanceProcAddrFn>(vulkan, "vkGetInstanceProcAddr");
    g_vkGetDeviceProcAddr = moduleFn<VkGetDeviceProcAddrFn>(vulkan, "vkGetDeviceProcAddr");
    g_vkQueuePresentKHR = moduleFn<VkQueuePresentKHRFn>(vulkan, "vkQueuePresentKHR");
    g_vkCreateSwapchainKHR = moduleFn<VkCreateSwapchainKHRFn>(vulkan, "vkCreateSwapchainKHR");
    g_vkDestroySwapchainKHR = moduleFn<VkDestroySwapchainKHRFn>(vulkan, "vkDestroySwapchainKHR");
    g_vkGetSwapchainImagesKHR = moduleFn<VkGetSwapchainImagesKHRFn>(vulkan, "vkGetSwapchainImagesKHR");
    g_vkAcquireNextImageKHR = moduleFn<VkAcquireNextImageKHRFn>(vulkan, "vkAcquireNextImageKHR");
    g_vkAcquireNextImage2KHR = moduleFn<VkAcquireNextImage2KHRFn>(vulkan, "vkAcquireNextImage2KHR");
    g_vkCmdPipelineBarrier = moduleFn<vk::PfnCmdPipelineBarrier>(vulkan, "vkCmdPipelineBarrier");
    g_vkCreateRenderPass = moduleFn<ApusrVkCreateRenderPassFn>(vulkan, "vkCreateRenderPass");
    g_vkCreateShaderModule = moduleFn<vk::PfnCreateShaderModule>(vulkan, "vkCreateShaderModule");
    g_vkDestroyShaderModule = moduleFn<vk::PfnDestroyShaderModule>(vulkan, "vkDestroyShaderModule");
    g_vkCreateComputePipelines = moduleFn<vk::PfnCreateComputePipelines>(vulkan, "vkCreateComputePipelines");
    g_vkCreateGraphicsPipelines = moduleFn<ApusrVkCreateGraphicsPipelinesFn>(vulkan, "vkCreateGraphicsPipelines");
    g_vkDestroyPipeline = moduleFn<vk::PfnDestroyPipeline>(vulkan, "vkDestroyPipeline");
    g_vkCreatePipelineCache = moduleFn<ApusrVkCreatePipelineCacheFn>(vulkan, "vkCreatePipelineCache");
    g_vkDestroyPipelineCache = moduleFn<ApusrVkDestroyPipelineCacheFn>(vulkan, "vkDestroyPipelineCache");
    g_vkGetPipelineCacheData = moduleFn<ApusrVkGetPipelineCacheDataFn>(vulkan, "vkGetPipelineCacheData");
    g_vkMergePipelineCaches = moduleFn<ApusrVkMergePipelineCachesFn>(vulkan, "vkMergePipelineCaches");

    if (g_vkCreateInstance == nullptr || g_vkCreateDevice == nullptr ||
        g_vkGetInstanceProcAddr == nullptr || g_vkGetDeviceProcAddr == nullptr ||
        g_vkQueuePresentKHR == nullptr) {
        apusr::host_discovery::event(
            "vulkan", "capture.install_failed", "reason=required_loader_exports_missing");
        return false;
    }

    const bool selfPinned = pinModuleFromAddress(&__ImageBase);
    const bool loaderPinned = pinModuleFromAddress(reinterpret_cast<const void*>(g_vkCreateDevice));
    if (!selfPinned || !loaderPinned) {
        apusr::host_discovery::eventf(
            "vulkan", "capture.install_failed",
            "reason=pin_failed self=%u loader=%u", selfPinned ? 1u : 0u, loaderPinned ? 1u : 0u);
        return false;
    }

    LONG result = DetourTransactionBegin();
    if (result != NO_ERROR) {
        apusr::host_discovery::eventf(
            "vulkan", "capture.install_failed", "reason=detour_begin error=%ld", result);
        return false;
    }
    result = DetourUpdateThread(GetCurrentThread());
    if (result != NO_ERROR) {
        DetourTransactionAbort();
        apusr::host_discovery::eventf(
            "vulkan", "capture.install_failed", "reason=detour_update_thread error=%ld", result);
        return false;
    }

    bool ok = true;
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCreateInstance), reinterpret_cast<PVOID>(&vkCreateInstance), "vkCreateInstance");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCreateDevice), reinterpret_cast<PVOID>(&vkCreateDevice), "vkCreateDevice");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkDestroyInstance), reinterpret_cast<PVOID>(&vkDestroyInstance), "vkDestroyInstance");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkDestroyDevice), reinterpret_cast<PVOID>(&vkDestroyDevice), "vkDestroyDevice");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkEnumeratePhysicalDevices), reinterpret_cast<PVOID>(&vkEnumeratePhysicalDevices), "vkEnumeratePhysicalDevices");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkEnumeratePhysicalDeviceGroups), reinterpret_cast<PVOID>(&vkEnumeratePhysicalDeviceGroups), "vkEnumeratePhysicalDeviceGroups");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkGetPhysicalDeviceQueueFamilyProperties), reinterpret_cast<PVOID>(&vkGetPhysicalDeviceQueueFamilyProperties), "vkGetPhysicalDeviceQueueFamilyProperties");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkGetInstanceProcAddr), reinterpret_cast<PVOID>(&vkGetInstanceProcAddr), "vkGetInstanceProcAddr");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkGetDeviceProcAddr), reinterpret_cast<PVOID>(&vkGetDeviceProcAddr), "vkGetDeviceProcAddr");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkQueuePresentKHR), reinterpret_cast<PVOID>(&vkQueuePresentKHR), "vkQueuePresentKHR");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCreateSwapchainKHR), reinterpret_cast<PVOID>(&vkCreateSwapchainKHR), "vkCreateSwapchainKHR");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkDestroySwapchainKHR), reinterpret_cast<PVOID>(&vkDestroySwapchainKHR), "vkDestroySwapchainKHR");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkGetSwapchainImagesKHR), reinterpret_cast<PVOID>(&vkGetSwapchainImagesKHR), "vkGetSwapchainImagesKHR");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkAcquireNextImageKHR), reinterpret_cast<PVOID>(&vkAcquireNextImageKHR), "vkAcquireNextImageKHR");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkAcquireNextImage2KHR), reinterpret_cast<PVOID>(&vkAcquireNextImage2KHR), "vkAcquireNextImage2KHR");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCmdPipelineBarrier), reinterpret_cast<PVOID>(&vkCmdPipelineBarrier), "vkCmdPipelineBarrier");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCreateRenderPass), reinterpret_cast<PVOID>(&vkCreateRenderPass), "vkCreateRenderPass");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCreateShaderModule), reinterpret_cast<PVOID>(&vkCreateShaderModule), "vkCreateShaderModule");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkDestroyShaderModule), reinterpret_cast<PVOID>(&vkDestroyShaderModule), "vkDestroyShaderModule");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCreateComputePipelines), reinterpret_cast<PVOID>(&vkCreateComputePipelines), "vkCreateComputePipelines");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCreateGraphicsPipelines), reinterpret_cast<PVOID>(&vkCreateGraphicsPipelines), "vkCreateGraphicsPipelines");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkDestroyPipeline), reinterpret_cast<PVOID>(&vkDestroyPipeline), "vkDestroyPipeline");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkCreatePipelineCache), reinterpret_cast<PVOID>(&vkCreatePipelineCache), "vkCreatePipelineCache");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkDestroyPipelineCache), reinterpret_cast<PVOID>(&vkDestroyPipelineCache), "vkDestroyPipelineCache");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkGetPipelineCacheData), reinterpret_cast<PVOID>(&vkGetPipelineCacheData), "vkGetPipelineCacheData");
    ok = ok && attachOne(reinterpret_cast<PVOID*>(&g_vkMergePipelineCaches), reinterpret_cast<PVOID>(&vkMergePipelineCaches), "vkMergePipelineCaches");
    if (!ok) {
        DetourTransactionAbort();
        return false;
    }

    result = DetourTransactionCommit();
    if (result != NO_ERROR) {
        apusr::host_discovery::eventf(
            "vulkan", "capture.install_failed", "reason=detour_commit error=%ld", result);
        return false;
    }

    // From this point onward the process-wide Detours are live for the lifetime
    // of the process.  Record that separately from core readiness so a later
    // failure cannot cause us to attach a second set of detours on retry.
    g_hooksInstalled.store(true, std::memory_order_release);

    if (!configureCoreDispatch()) {
        apusr::host_discovery::event(
            "vulkan", "capture.install_failed", "reason=core_dispatch_incomplete hooks_installed=1");
        return false;
    }
    g_coreReady.store(true, std::memory_order_release);

    apusr::host_discovery::eventf(
        "vulkan", "capture.installed",
        "mode=system_loader_detours module=%p create_instance=%p create_device=%p gipa=%p gdpa=%p queue_present=%p enumerate_devices=%p enumerate_groups=%p",
        vulkan,
        reinterpret_cast<void*>(g_vkCreateInstance),
        reinterpret_cast<void*>(g_vkCreateDevice),
        reinterpret_cast<void*>(g_vkGetInstanceProcAddr),
        reinterpret_cast<void*>(g_vkGetDeviceProcAddr),
        reinterpret_cast<void*>(g_vkQueuePresentKHR),
        reinterpret_cast<void*>(g_vkEnumeratePhysicalDevices),
        reinterpret_cast<void*>(g_vkEnumeratePhysicalDeviceGroups));
    return true;
}

[[nodiscard]] bool ensureInstalled()
{
    if (g_coreReady.load(std::memory_order_acquire)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_installMutex);
    if (g_coreReady.load(std::memory_order_relaxed)) {
        return true;
    }

    // A successful DetourTransactionCommit is irreversible by design for this
    // pinned process-lifetime module.  Never attach the hooks twice.  If the
    // core dispatch was incomplete after the commit, retry only dispatch
    // configuration; this also allows a late-resolved optional loader export to
    // make the capture core ready without touching the Detours again.
    if (g_hooksInstalled.load(std::memory_order_acquire)) {
        if (!configureCoreDispatch()) {
            apusr::host_discovery::event(
                "vulkan", "capture.dispatch_retry_failed", "hooks_installed=1");
            return false;
        }
        g_coreReady.store(true, std::memory_order_release);
        apusr::host_discovery::event(
            "vulkan", "capture.dispatch_ready", "hooks_installed=1 retry=1");
        return true;
    }

    return installHooksLocked();
}

} // namespace

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateInstance(
    const VkInstanceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        if (g_vkCreateInstance != nullptr) {
            return g_vkCreateInstance(createInfo, allocator, instance);
        }
        if (instance != nullptr) {
            *instance = nullptr;
        }
        return VkErrorInitializationFailed;
    }
    return apusr::vulkan_host_capture::createInstance(createInfo, allocator, instance);
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDevice* device)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        if (g_vkCreateDevice != nullptr) {
            return g_vkCreateDevice(physicalDevice, createInfo, allocator, device);
        }
        if (device != nullptr) {
            *device = nullptr;
        }
        return VkErrorInitializationFailed;
    }
    return apusr::vulkan_host_capture::createDevice(physicalDevice, createInfo, allocator, device);
}

APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* allocator)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        if (g_vkDestroyInstance != nullptr) {
            g_vkDestroyInstance(instance, allocator);
        }
        return;
    }
    apusr::vulkan_host_capture::destroyInstance(instance, allocator);
}

APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* allocator)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        if (g_vkDestroyDevice != nullptr) {
            g_vkDestroyDevice(device, allocator);
        }
        return;
    }
    apusr::vulkan_host_capture::destroyDevice(device, allocator);
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkEnumeratePhysicalDevices(
    VkInstance instance,
    std::uint32_t* count,
    VkPhysicalDevice* devices)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        return g_vkEnumeratePhysicalDevices != nullptr
            ? g_vkEnumeratePhysicalDevices(instance, count, devices)
            : VkErrorInitializationFailed;
    }
    return apusr::vulkan_host_capture::enumeratePhysicalDevices(instance, count, devices);
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkEnumeratePhysicalDeviceGroups(
    VkInstance instance,
    std::uint32_t* count,
    VkPhysicalDeviceGroupPropertiesLoose* groups)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        return g_vkEnumeratePhysicalDeviceGroups != nullptr
            ? g_vkEnumeratePhysicalDeviceGroups(instance, count, groups)
            : VkErrorInitializationFailed;
    }
    return apusr::vulkan_host_capture::enumeratePhysicalDeviceGroups(instance, count, groups);
}

APUSR_VK_LOADER_ENTRY void WINAPI vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice,
    std::uint32_t* count,
    VkQueueFamilyPropertiesLoose* properties)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        if (g_vkGetPhysicalDeviceQueueFamilyProperties != nullptr) {
            g_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, count, properties);
        } else if (count != nullptr) {
            *count = 0u;
        }
        return;
    }
    apusr::vulkan_host_capture::getPhysicalDeviceQueueFamilyProperties(
        physicalDevice, count, properties);
}

APUSR_VK_LOADER_ENTRY VkVoidFunction WINAPI vkGetInstanceProcAddr(
    VkInstance instance,
    const char* name)
{
    const bool ready = ensureInstalled();
    if (g_vkGetInstanceProcAddr == nullptr) {
        return nullptr;
    }
    const VkVoidFunction original = g_vkGetInstanceProcAddr(instance, name);
    if (original == nullptr || !ready || !g_coreReady.load(std::memory_order_acquire)) {
        return original;
    }
    if (const VkVoidFunction intercept = interceptProc(name)) {
        return intercept;
    }
    return original;
}

APUSR_VK_LOADER_ENTRY VkVoidFunction WINAPI vkGetDeviceProcAddr(
    VkDevice device,
    const char* name)
{
    const bool ready = ensureInstalled();
    if (g_vkGetDeviceProcAddr == nullptr) {
        return nullptr;
    }
    const VkVoidFunction original = g_vkGetDeviceProcAddr(device, name);
    if (original == nullptr || !ready || !g_coreReady.load(std::memory_order_acquire)) {
        return original;
    }
    if (const VkVoidFunction intercept = interceptProc(name)) {
        return intercept;
    }
    return original;
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkQueuePresentKHR(VkQueue queue, const void* presentInfo)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        return g_vkQueuePresentKHR != nullptr
            ? g_vkQueuePresentKHR(queue, presentInfo)
            : VkErrorInitializationFailed;
    }
    return apusr::vulkan_host_capture::queuePresent(queue, presentInfo);
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHRLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSwapchainKHR* swapchain)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        return g_vkCreateSwapchainKHR != nullptr
            ? g_vkCreateSwapchainKHR(device, createInfo, allocator, swapchain)
            : VkErrorExtensionNotPresent;
    }
    return apusr::vulkan_host_capture::createSwapchain(device, createInfo, allocator, swapchain);
}

APUSR_VK_LOADER_ENTRY void WINAPI vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        if (g_vkDestroySwapchainKHR != nullptr) {
            g_vkDestroySwapchainKHR(device, swapchain, allocator);
        }
        return;
    }
    apusr::vulkan_host_capture::destroySwapchain(device, swapchain, allocator);
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    std::uint32_t* count,
    VkImage* images)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        return g_vkGetSwapchainImagesKHR != nullptr
            ? g_vkGetSwapchainImagesKHR(device, swapchain, count, images)
            : VkErrorExtensionNotPresent;
    }
    return apusr::vulkan_host_capture::getSwapchainImages(device, swapchain, count, images);
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const std::uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    std::uint32_t* imageIndex)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        return g_vkAcquireNextImageKHR != nullptr
            ? g_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, imageIndex)
            : VkErrorExtensionNotPresent;
    }
    return apusr::vulkan_host_capture::acquireNextImage(
        device, swapchain, timeout, semaphore, fence, imageIndex);
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHRLoose* acquireInfo,
    std::uint32_t* imageIndex)
{
    if (!ensureInstalled() || !g_coreReady.load(std::memory_order_acquire)) {
        return g_vkAcquireNextImage2KHR != nullptr
            ? g_vkAcquireNextImage2KHR(device, acquireInfo, imageIndex)
            : VkErrorExtensionNotPresent;
    }
    return apusr::vulkan_host_capture::acquireNextImage2(device, acquireInfo, imageIndex);
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateShaderModule(
    vk::Device device,
    const vk::ShaderModuleCreateInfo* createInfo,
    const vk::AllocationCallbacks* allocator,
    vk::ShaderModule* shaderModule)
{
    const auto fn = resolveDeviceFunction(device, g_vkCreateShaderModule, "vkCreateShaderModule");
    if (fn == nullptr)
        return VkErrorInitializationFailed;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    const std::uint64_t codeHash = stream && createInfo != nullptr
        ? hashPipelineBytes(createInfo->pCode, createInfo->codeSize) : 0u;
    VkResult result = VkErrorInitializationFailed;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.create_shader_module");
        result = fn(device, createInfo, allocator, shaderModule);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.shader_module.create",
            .value = createInfo != nullptr ? createInfo->codeSize : 0u, .value2 = codeHash,
        });
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.shader_module.result",
            .value = static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
            .value2 = shaderModule != nullptr
                ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(*shaderModule)) : 0u,
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
    return result;
}

APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyShaderModule(
    vk::Device device,
    vk::ShaderModule shaderModule,
    const vk::AllocationCallbacks* allocator)
{
    const auto fn = resolveDeviceFunction(device, g_vkDestroyShaderModule, "vkDestroyShaderModule");
    if (fn == nullptr)
        return;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.destroy_shader_module");
        fn(device, shaderModule, allocator);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.shader_module.destroy",
            .value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(shaderModule)),
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateComputePipelines(
    vk::Device device,
    vk::PipelineCache pipelineCache,
    const std::uint32_t createInfoCount,
    const vk::ComputePipelineCreateInfo* createInfos,
    const vk::AllocationCallbacks* allocator,
    vk::Pipeline* pipelines)
{
    const auto fn = resolveDeviceFunction(device, g_vkCreateComputePipelines, "vkCreateComputePipelines");
    if (fn == nullptr)
        return VkErrorInitializationFailed;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    VkResult result = VkErrorInitializationFailed;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.create_compute_pipelines");
        result = fn(device, pipelineCache, createInfoCount, createInfos, allocator, pipelines);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline.compute.create",
            .value = createInfoCount,
            .value2 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipelineCache)),
        });
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline.compute.result",
            .value = static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
            .value2 = createInfoCount == 1u && pipelines != nullptr
                ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipelines[0])) : 0u,
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
    return result;
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateGraphicsPipelines(
    vk::Device device,
    vk::PipelineCache pipelineCache,
    const std::uint32_t createInfoCount,
    const void* createInfos,
    const vk::AllocationCallbacks* allocator,
    vk::Pipeline* pipelines)
{
    const auto fn = resolveDeviceFunction(device, g_vkCreateGraphicsPipelines, "vkCreateGraphicsPipelines");
    if (fn == nullptr)
        return VkErrorInitializationFailed;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    VkResult result = VkErrorInitializationFailed;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.create_graphics_pipelines");
        result = fn(device, pipelineCache, createInfoCount, createInfos, allocator, pipelines);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline.graphics.create",
            .value = createInfoCount,
            .value2 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipelineCache)),
        });
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline.graphics.result",
            .value = static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
            .value2 = createInfoCount == 1u && pipelines != nullptr
                ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipelines[0])) : 0u,
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
    return result;
}

APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyPipeline(
    vk::Device device,
    vk::Pipeline pipeline,
    const vk::AllocationCallbacks* allocator)
{
    const auto fn = resolveDeviceFunction(device, g_vkDestroyPipeline, "vkDestroyPipeline");
    if (fn == nullptr)
        return;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.destroy_pipeline");
        fn(device, pipeline, allocator);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline.destroy",
            .value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipeline)),
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreatePipelineCache(
    vk::Device device,
    const ApusrVkPipelineCacheCreateInfoLoose* createInfo,
    const vk::AllocationCallbacks* allocator,
    vk::PipelineCache* pipelineCache)
{
    const auto fn = resolveDeviceFunction(device, g_vkCreatePipelineCache, "vkCreatePipelineCache");
    if (fn == nullptr)
        return VkErrorInitializationFailed;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    VkResult result = VkErrorInitializationFailed;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.create_pipeline_cache");
        result = fn(device, createInfo, allocator, pipelineCache);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline_cache.create",
            .value = createInfo != nullptr ? createInfo->initialDataSize : 0u,
            .value2 = pipelineCache != nullptr
                ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(*pipelineCache)) : 0u,
            .flags = createInfo != nullptr && createInfo->pInitialData != nullptr ? 1u : 0u,
        });
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline_cache.result",
            .value = static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
    return result;
}

APUSR_VK_LOADER_ENTRY void WINAPI vkDestroyPipelineCache(
    vk::Device device,
    vk::PipelineCache pipelineCache,
    const vk::AllocationCallbacks* allocator)
{
    const auto fn = resolveDeviceFunction(device, g_vkDestroyPipelineCache, "vkDestroyPipelineCache");
    if (fn == nullptr)
        return;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.destroy_pipeline_cache");
        fn(device, pipelineCache, allocator);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline_cache.destroy",
            .value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipelineCache)),
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkGetPipelineCacheData(
    vk::Device device,
    vk::PipelineCache pipelineCache,
    std::size_t* dataSize,
    void* data)
{
    const auto fn = resolveDeviceFunction(device, g_vkGetPipelineCacheData, "vkGetPipelineCacheData");
    if (fn == nullptr)
        return VkErrorInitializationFailed;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    const std::size_t requestedBytes = dataSize != nullptr ? *dataSize : 0u;
    VkResult result = VkErrorInitializationFailed;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.get_pipeline_cache_data");
        result = fn(device, pipelineCache, dataSize, data);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline_cache.get_data",
            .value = requestedBytes, .value2 = dataSize != nullptr ? *dataSize : 0u,
            .flags = data != nullptr ? 1u : 0u,
        });
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline_cache.result",
            .value = static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
            .value2 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipelineCache)),
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
    return result;
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkMergePipelineCaches(
    vk::Device device,
    vk::PipelineCache dstCache,
    const std::uint32_t srcCacheCount,
    const vk::PipelineCache* srcCaches)
{
    const auto fn = resolveDeviceFunction(device, g_vkMergePipelineCaches, "vkMergePipelineCaches");
    if (fn == nullptr)
        return VkErrorInitializationFailed;
    const auto stream = pipelineProfileStream(reinterpret_cast<VkDevice>(device));
    const std::uint64_t eventId = stream ? nextPipelineProfileEvent() : 0u;
    const void* caller = stream ? pipelineCallerAddress() : nullptr;
    VkResult result = VkErrorInitializationFailed;
    {
        apusr::profile::CpuScope scope(stream, eventId, 0u, "vk.merge_pipeline_caches");
        result = fn(device, dstCache, srcCacheCount, srcCaches);
    }
    if (stream) {
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline_cache.merge",
            .value = srcCacheCount,
            .value2 = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(dstCache)),
        });
        apusr::profile::recordCounter({
            .stream = stream, .frameId = eventId, .name = "vulkan.pipeline_cache.result",
            .value = static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
        });
        recordPipelineCallsite(stream, eventId, caller);
    }
    return result;
}

APUSR_VK_LOADER_ENTRY void WINAPI vkCmdPipelineBarrier(
    vk::CommandBuffer commandBuffer,
    const vk::PipelineStageFlags srcStageMask,
    const vk::PipelineStageFlags dstStageMask,
    const vk::DependencyFlags dependencyFlags,
    const std::uint32_t memoryBarrierCount,
    const vk::MemoryBarrier* memoryBarriers,
    const std::uint32_t bufferMemoryBarrierCount,
    const vk::BufferMemoryBarrier* bufferMemoryBarriers,
    const std::uint32_t imageMemoryBarrierCount,
    const vk::ImageMemoryBarrier* imageMemoryBarriers)
{
    if (!g_vkCmdPipelineBarrier)
        return;

    vk::PipelineStageFlags effectiveSrcStageMask = srcStageMask;
    vk::PipelineStageFlags effectiveDstStageMask = dstStageMask;

    // Stock Streamline's Vulkan CHI owns the external-image transitions around
    // stock sl.dlss evaluation, but its generic transition helper brackets every
    // batch with ALL_COMMANDS -> ALL_COMMANDS. At this exact boundary APUSR knows
    // one side precisely: NGX consumes/produces these resources in compute. Keep
    // the host side conservative because Streamline tags carry image layout but
    // no producer/consumer pipeline-stage provenance.
    //
    // This deliberately does not touch arbitrary game barriers or APUSR's own
    // backend barriers. The rewrite is active only while the private attachment
    // is inside stock Vulkan DLSS evaluation, only for Streamline's canonical
    // prepared image states, and only when the original batch is the generic
    // ALL_COMMANDS/ALL_COMMANDS form with no memory/buffer barriers mixed in.
    StreamlineDlssBarrierDirection streamlineDirection = StreamlineDlssBarrierDirection::None;
    if (srcStageMask == vk::PipelineStageAllCommandsBit &&
        dstStageMask == vk::PipelineStageAllCommandsBit &&
        memoryBarrierCount == 0u && bufferMemoryBarrierCount == 0u &&
        imageMemoryBarrierCount != 0u && imageMemoryBarriers != nullptr &&
        streamlineVulkanDlssEvaluationActive()) {
        streamlineDirection = classifyStreamlineDlssBarrierBatch(
            imageMemoryBarrierCount, imageMemoryBarriers);
        if (streamlineDirection == StreamlineDlssBarrierDirection::IntoEvaluation) {
            effectiveDstStageMask = vk::PipelineStageComputeShaderBit;
        } else if (streamlineDirection == StreamlineDlssBarrierDirection::OutOfEvaluation) {
            effectiveSrcStageMask = vk::PipelineStageComputeShaderBit;
        }

        if (streamlineDirection != StreamlineDlssBarrierDirection::None &&
            g_streamlineDlssBarrierNarrowLogs.load(std::memory_order_relaxed) < 4u) {
            const std::uint32_t ordinal =
                g_streamlineDlssBarrierNarrowLogs.fetch_add(1u, std::memory_order_relaxed);
            if (ordinal < 4u) {
                apusr::host_discovery::eventf(
                    "vulkan", "dlss.streamline_barrier_narrowed",
                    "direction=%s command_buffer=%p images=%u stages=0x%08x->0x%08x/0x%08x->0x%08x",
                    streamlineDirection == StreamlineDlssBarrierDirection::IntoEvaluation ? "into" : "out",
                    commandBuffer, imageMemoryBarrierCount,
                    srcStageMask, effectiveSrcStageMask,
                    dstStageMask, effectiveDstStageMask);
            }
        }
    }

    bool needsVirtualLayoutRewrite = false;
    if (apusr::vulkan_host_capture::virtualSwapchainActive() &&
        imageMemoryBarrierCount != 0u && imageMemoryBarriers != nullptr) {
        for (std::uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
            const vk::ImageMemoryBarrier& barrier = imageMemoryBarriers[i];
            if (apusr::vulkan_host_capture::isVirtualSwapchainImage(
                    reinterpret_cast<VkImage>(barrier.image)) &&
                (barrier.oldLayout == vk::ImageLayoutPresentSrcKhr ||
                 barrier.newLayout == vk::ImageLayoutPresentSrcKhr)) {
                needsVirtualLayoutRewrite = true;
                break;
            }
        }
    }

    if (!needsVirtualLayoutRewrite) {
        g_vkCmdPipelineBarrier(
            commandBuffer, effectiveSrcStageMask, effectiveDstStageMask, dependencyFlags,
            memoryBarrierCount, memoryBarriers,
            bufferMemoryBarrierCount, bufferMemoryBarriers,
            imageMemoryBarrierCount, imageMemoryBarriers);
        return;
    }

    // Some hosts record the swapchain final transition through the legacy barrier API.
    // APUSR's virtual backbuffers are ordinary images, so PRESENT_SRC_KHR is
    // neither meaningful nor legal for them. Keep their process-visible state
    // in GENERAL while preserving every access mask/stage/ownership edge.
    thread_local std::vector<vk::ImageMemoryBarrier> rewritten;
    rewritten.assign(imageMemoryBarriers, imageMemoryBarriers + imageMemoryBarrierCount);
    std::uint32_t rewriteCount = 0u;
    for (vk::ImageMemoryBarrier& barrier : rewritten) {
        if (!apusr::vulkan_host_capture::isVirtualSwapchainImage(
                reinterpret_cast<VkImage>(barrier.image)))
            continue;
        if (barrier.oldLayout == vk::ImageLayoutPresentSrcKhr) {
            barrier.oldLayout = vk::ImageLayoutGeneral;
            ++rewriteCount;
        }
        if (barrier.newLayout == vk::ImageLayoutPresentSrcKhr) {
            barrier.newLayout = vk::ImageLayoutGeneral;
            ++rewriteCount;
        }
    }
    if (g_virtualBarrierRewriteLogs.fetch_add(1u, std::memory_order_relaxed) < 4u) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.virtual_present_layout_barrier_rewritten",
            "command_buffer=%p image_barriers=%u layout_fields=%u",
            commandBuffer, imageMemoryBarrierCount, rewriteCount);
    }
    g_vkCmdPipelineBarrier(
        commandBuffer, effectiveSrcStageMask, effectiveDstStageMask, dependencyFlags,
        memoryBarrierCount, memoryBarriers,
        bufferMemoryBarrierCount, bufferMemoryBarriers,
        imageMemoryBarrierCount, rewritten.data());
}

APUSR_VK_LOADER_ENTRY VkResult WINAPI vkCreateRenderPass(
    VkDevice device,
    const ApusrVkRenderPassCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    ApusrVkRenderPass* renderPass)
{
    if (!g_vkCreateRenderPass)
        return VkErrorInitializationFailed;
    if (!createInfo || !apusr::vulkan_host_capture::virtualSwapchainActive() ||
        createInfo->attachmentCount == 0u || !createInfo->pAttachments)
        return g_vkCreateRenderPass(device, createInfo, allocator, renderPass);

    bool needsRewrite = false;
    constexpr std::int32_t kPresentLayout = static_cast<std::int32_t>(vk::ImageLayoutPresentSrcKhr);
    constexpr std::int32_t kGeneralLayout = static_cast<std::int32_t>(vk::ImageLayoutGeneral);
    for (std::uint32_t i = 0; i < createInfo->attachmentCount; ++i) {
        const auto& attachment = createInfo->pAttachments[i];
        if (attachment.initialLayout == kPresentLayout || attachment.finalLayout == kPresentLayout) {
            needsRewrite = true;
            break;
        }
    }
    if (!needsRewrite)
        return g_vkCreateRenderPass(device, createInfo, allocator, renderPass);

    std::vector<ApusrVkAttachmentDescriptionLoose> attachments(
        createInfo->pAttachments, createInfo->pAttachments + createInfo->attachmentCount);
    std::uint32_t rewriteCount = 0u;
    for (auto& attachment : attachments) {
        if (attachment.initialLayout == kPresentLayout) {
            attachment.initialLayout = kGeneralLayout;
            ++rewriteCount;
        }
        if (attachment.finalLayout == kPresentLayout) {
            attachment.finalLayout = kGeneralLayout;
            ++rewriteCount;
        }
    }
    ApusrVkRenderPassCreateInfoLoose rewritten = *createInfo;
    rewritten.pAttachments = attachments.data();
    if (g_virtualRenderPassRewriteLogs.fetch_add(1u, std::memory_order_relaxed) < 4u) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.virtual_present_renderpass_rewritten",
            "device=%p attachments=%u layout_fields=%u",
            device, createInfo->attachmentCount, rewriteCount);
    }
    return g_vkCreateRenderPass(device, &rewritten, allocator, renderPass);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrInstallVulkanHostCapture()
{
    return ensureInstalled();
}

APUSR_VK_HOST_API std::uint32_t APUSR_VK_HOST_CALL apusrVulkanHostDeviceNegotiationFlags(void* device)
{
    return apusr::vulkan_host_capture::deviceNegotiationFlags(device);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetDeviceInfo(
    void* device,
    ApusrVulkanHostDeviceInfo* outInfo)
{
    return apusr::vulkan_host_capture::getDeviceInfo(device, outInfo);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetQueueInfo(
    void* device,
    std::uint32_t queueOrdinal,
    ApusrVulkanHostQueueInfo* outInfo)
{
    return apusr::vulkan_host_capture::getQueueInfo(device, queueOrdinal, outInfo);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostResolveQueue(
    void* device,
    std::uint32_t preferredKind,
    ApusrVulkanHostQueueInfo* outInfo)
{
    return apusr::vulkan_host_capture::resolveQueue(device, preferredKind, outInfo);
}

APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostAssociateInstance(
    void* device,
    void* instance,
    void* physicalDevice)
{
    apusr::vulkan_host_capture::associateInstance(device, instance, physicalDevice);
}

APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostRequestAntiLag(bool requested)
{
    apusr::vulkan_host_capture::requestAntiLag(requested);
}

APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostRequestFrameGeneration(bool requested)
{
    apusr::vulkan_host_capture::requestFrameGeneration(requested);
}

APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostSetFrameGenerationActive(bool active)
{
    apusr::vulkan_host_capture::setFrameGenerationActive(active);
}

APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostRequestLegacyOpticalFlowQueueRole(bool requested)
{
    apusr::vulkan_host_capture::requestLegacyOpticalFlowQueueRole(requested);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostAntiLagSupported(void* physicalDevice)
{
    return apusr::vulkan_host_capture::antiLagSupported(physicalDevice);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostAntiLagAvailable(void* device)
{
    return apusr::vulkan_host_capture::antiLagAvailable(device);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostAntiLagInput(
    void* device,
    std::uint64_t frameIndex,
    std::uint32_t mode,
    std::uint32_t maxFps)
{
    return apusr::vulkan_host_capture::antiLagInput(device, frameIndex, mode, maxFps);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostAntiLagArmPairedDisable(void* device)
{
    return apusr::vulkan_host_capture::antiLagArmPairedDisable(device);
}

APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostAntiLagSetPresentFrame(
    void* device,
    std::uint64_t frameIndex)
{
    apusr::vulkan_host_capture::antiLagSetPresentFrame(device, frameIndex);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetSwapchainInfo(
    void* swapchain,
    ApusrVulkanHostSwapchainInfo* outInfo)
{
    return apusr::vulkan_host_capture::getSwapchainInfo(swapchain, outInfo);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetSwapchainImage(
    void* swapchain,
    const std::uint32_t imageIndex,
    void** outImage)
{
    return apusr::vulkan_host_capture::getSwapchainImage(swapchain, imageIndex, outImage);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostGetRealSwapchainImage(
    void* swapchain,
    const std::uint32_t imageIndex,
    void** outImage)
{
    return apusr::vulkan_host_capture::getRealSwapchainImage(swapchain, imageIndex, outImage);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostReleaseVirtualSwapchainImage(
    void* swapchain,
    const std::uint32_t imageIndex,
    void* firstReleaseSemaphore,
    const std::uint64_t firstReleaseValue,
    void* secondReleaseSemaphore,
    const std::uint64_t secondReleaseValue)
{
    return apusr::vulkan_host_capture::releaseVirtualSwapchainImage(
        swapchain, imageIndex, firstReleaseSemaphore, firstReleaseValue,
        secondReleaseSemaphore, secondReleaseValue);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostWaitVirtualSwapchainReleaseDependencies(
    void* swapchain)
{
    return apusr::vulkan_host_capture::waitVirtualSwapchainReleaseDependencies(swapchain);
}

APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostAcquireNextImageForFrameGeneration(
    void* device,
    void* swapchain,
    const std::uint64_t timeout,
    void* semaphore,
    std::uint32_t* outImageIndex)
{
    return apusr::vulkan_host_capture::acquireNextImageForFrameGeneration(
        device, swapchain, timeout, semaphore, outImageIndex);
}

APUSR_VK_HOST_API bool APUSR_VK_HOST_CALL apusrVulkanHostReleaseRealSwapchainImage(
    void* device,
    void* swapchain,
    const std::uint32_t imageIndex)
{
    return apusr::vulkan_host_capture::releaseRealSwapchainImage(device, swapchain, imageIndex);
}

APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentDirect(
    void* queue,
    const void* presentInfo)
{
    return apusr::vulkan_host_capture::queuePresentDirect(queue, presentInfo);
}

APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentSource(
    void* queue,
    const void* presentInfo)
{
    return apusr::vulkan_host_capture::queuePresentSource(queue, presentInfo);
}

APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentSourceRelative(
    void* queue,
    const void* presentInfo,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled)
{
    return apusr::vulkan_host_capture::queuePresentSourceRelative(
        queue, presentInfo, minimumPreviousImageVisibleNs, outScheduled);
}

APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentGeneratedRelative(
    void* queue,
    const void* presentInfo,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled)
{
    return apusr::vulkan_host_capture::queuePresentGeneratedRelative(
        queue, presentInfo, minimumPreviousImageVisibleNs, outScheduled);
}

APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentFrameGeneration(
    void* queue,
    const void* presentInfo,
    const std::uint64_t frameId,
    const std::uint32_t flags)
{
    return apusr::vulkan_host_capture::queuePresentFrameGeneration(
        queue, presentInfo, frameId, flags);
}

APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentSourceRelativeFrameGeneration(
    void* queue,
    const void* presentInfo,
    const std::uint64_t frameId,
    const std::uint32_t flags,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled)
{
    return apusr::vulkan_host_capture::queuePresentSourceRelativeFrameGeneration(
        queue, presentInfo, frameId, flags, minimumPreviousImageVisibleNs, outScheduled);
}

APUSR_VK_HOST_API std::int32_t APUSR_VK_HOST_CALL apusrVulkanHostQueuePresentGeneratedRelativeFrameGeneration(
    void* queue,
    const void* presentInfo,
    const std::uint64_t frameId,
    const std::uint32_t flags,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled)
{
    return apusr::vulkan_host_capture::queuePresentGeneratedRelativeFrameGeneration(
        queue, presentInfo, frameId, flags, minimumPreviousImageVisibleNs, outScheduled);
}

APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostSetQueuePresentInterceptor(void* queuePresentProc)
{
    apusr::vulkan_host_capture::setQueuePresentInterceptor(queuePresentProc);
}

APUSR_VK_HOST_API void APUSR_VK_HOST_CALL apusrVulkanHostSetSwapchainDestroyInterceptor(void* destroySwapchainProc)
{
    apusr::vulkan_host_capture::setSwapchainDestroyInterceptor(destroySwapchainProc);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    // Detours, Vulkan loading and file I/O are deliberately deferred until an
    // explicit frontend bootstrap calls apusrInstallVulkanHostCapture().
    return TRUE;
}

#undef APUSR_VK_LOADER_ENTRY

#endif // _WIN32
