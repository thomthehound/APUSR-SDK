// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_vulkan_host_capture_core.h"
#include "apusr_vulkan_minimal.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define APUSR_TEST_VK_CALL __stdcall
#else
#define APUSR_TEST_VK_CALL
#endif

namespace {

using namespace apusr::vulkan_host_capture;
namespace vk = apusr::backends::vk;

constexpr std::uint32_t kApiVersion13 = (1u << 22u) | (3u << 12u);
constexpr char kAntiLagExtension[] = "VK_AMD_anti_lag";
constexpr char kComputeDerivativesExtension[] = "VK_KHR_compute_shader_derivatives";
constexpr char kSwapchainMaintenance1Extension[] = "VK_KHR_swapchain_maintenance1";
constexpr char kSurfaceExtension[] = "VK_KHR_surface";
constexpr char kGetSurfaceCapabilities2Extension[] = "VK_KHR_get_surface_capabilities2";
constexpr char kSurfaceMaintenance1Extension[] = "VK_KHR_surface_maintenance1";
constexpr char kPresentTimingExtension[] = "VK_EXT_present_timing";
constexpr char kPresentId2Extension[] = "VK_KHR_present_id2";
constexpr char kCalibratedTimestampsExtension[] = "VK_KHR_calibrated_timestamps";
constexpr char kExtCalibratedTimestampsExtension[] = "VK_EXT_calibrated_timestamps";
constexpr char kDisplayTimingExtension[] = "VK_GOOGLE_display_timing";
constexpr char kMemoryBudgetExtension[] = "VK_EXT_memory_budget";

VkInstance g_instance = reinterpret_cast<VkInstance>(0x9000);
VkPhysicalDevice g_physicalDevice = reinterpret_cast<VkPhysicalDevice>(0x1000);
VkPhysicalDevice g_groupPhysicalDevice = reinterpret_cast<VkPhysicalDevice>(0x1001);
VkPhysicalDevice g_unsupportedPhysicalDevice = reinterpret_cast<VkPhysicalDevice>(0x1002);
VkDevice g_device = reinterpret_cast<VkDevice>(0x2000);
VkDevice g_groupDevice = reinterpret_cast<VkDevice>(0x2001);
VkQueue g_queue = reinterpret_cast<VkQueue>(0x3000);
VkQueue g_secondaryQueue = reinterpret_cast<VkQueue>(0x3005);
VkQueue g_framegenInputQueue = reinterpret_cast<VkQueue>(0x3007);
VkQueue g_framegenQueue = reinterpret_cast<VkQueue>(0x3003);
VkQueue g_framegenPresentQueue = reinterpret_cast<VkQueue>(0x3004);
VkQueue g_framegenAcquireQueue = reinterpret_cast<VkQueue>(0x3006);
VkQueue g_framegenReplayQueue = reinterpret_cast<VkQueue>(0x3008);
VkQueue g_groupQueue = reinterpret_cast<VkQueue>(0x3001);
VkQueue g_untrackedQueue = reinterpret_cast<VkQueue>(0x3002);
VkSwapchainKHR g_swapchain = reinterpret_cast<VkSwapchainKHR>(0x4000);
std::array<VkImage, 3> g_swapchainImages{
    reinterpret_cast<VkImage>(0x4100),
    reinterpret_cast<VkImage>(0x4101),
    reinterpret_cast<VkImage>(0x4102),
};

struct AntiLagCall {
    std::int32_t mode = -1;
    std::int32_t stage = -1;
    std::uint64_t frameIndex = 0;
    std::uint32_t maxFps = 0;
};

std::vector<AntiLagCall> g_antiLagCalls;
std::uint32_t g_backingPresentCalls = 0;
std::uint32_t g_realPresentCalls = 0;
bool g_lastCreateHadComputeDerivativesExtension = false;
bool g_lastCreateHadComputeDerivativeGroupLinear = false;
bool g_lastCreateHadAntiLagExtension = false;
bool g_lastCreateHadAntiLagFeature = false;
bool g_lastCreateHadTimelineSemaphore = false;
bool g_lastCreateHadSubgroupSizeControl = false;
bool g_lastCreateHadSwapchainMaintenance1Extension = false;
bool g_lastCreateHadSwapchainMaintenance1Feature = false;
bool g_instanceCreateHadSurfaceCapabilities2Extension = false;
bool g_instanceCreateHadSurfaceMaintenance1Extension = false;
bool g_lastCreateHadPresentTimingExtension = false;
bool g_lastCreateHadPresentId2Extension = false;
bool g_lastCreateHadCalibratedTimestampsExtension = false;
bool g_lastCreateHadPresentTimingFeature = false;
bool g_lastCreateHadDisplayTimingExtension = false;
bool g_lastCreateHadMemoryBudgetExtension = false;
bool g_groupCreateHadAntiLagExtension = false;
bool g_groupCreateHadAntiLagFeature = false;
bool g_groupCreateHadKhrCalibratedTimestampsExtension = false;
bool g_groupCreateHadExtCalibratedTimestampsExtension = false;
bool g_exposeOnlyExtCalibratedTimestamps = false;
std::uint32_t g_destroySwapchainCalls = 0;
std::uint32_t g_swapchainDestroyInterceptorCalls = 0;
bool g_destroySawInterceptor = false;
std::uint32_t g_lastSwapchainCreateUsage = 0;
std::uint32_t g_lastSwapchainCreateFlags = 0u;
std::uint32_t g_relativeTimedPresentCalls = 0u;
std::uint64_t g_lastRelativeTargetNs = 0u;
std::uint64_t g_lastRelativeTimeDomainId = 0u;
std::uint32_t g_lastRelativeTargetStage = 0u;
std::uint32_t g_lastSwapchainCreateMinImageCount = 0;
std::uint32_t g_lastSwapchainCreateSharingMode = 0u;
std::vector<std::uint32_t> g_lastSwapchainCreateQueueFamilies;
std::uint32_t g_internalAcquireCalls = 0;
std::uint32_t g_releaseSwapchainImageCalls = 0u;
std::uint32_t g_lastReleasedSwapchainImage = APUSR_VK_HOST_INVALID_IMAGE_INDEX;
std::uint32_t g_lastCreateQueueInfoCount = 0u;
std::uint32_t g_lastCreateQueueCount = 0u;
std::uint32_t g_lastComputeCreateQueueCount = 0u;
float g_lastComputeCreateQueuePriority = -1.0f;
std::uint32_t g_lastTransferCreateQueueCount = 0u;
float g_lastTransferCreateQueuePriority = -1.0f;
std::array<float, 5> g_lastCreateQueuePriorities{};
std::uint32_t g_virtualImageCreates = 0u;
std::uint32_t g_virtualImageDestroys = 0u;
std::uint32_t g_virtualQueueSubmits = 0u;
std::uint32_t g_virtualQueueWaitIdles = 0u;
std::uint32_t g_virtualInitBarriers = 0u;
std::uint32_t g_semaphoreCounterQueries = 0u;
std::uint32_t g_pastDisplayTimingQueries = 0u;
std::uint32_t g_memoryBudgetQueries = 0u;
std::uint64_t g_fakeDisplayClockNs = 1'000'000'000u;
std::vector<std::uint32_t> g_observedDisplayTimingPresentIds;
std::vector<VkPastPresentationTimingGOOGLELoose> g_pendingDisplayTimings;

struct FakeVkImageObject { std::uint32_t marker = 0xA11E; };
struct FakeVkMemoryObject { std::uint32_t marker = 0xB00F; };
struct FakeVkCommandPoolObject { std::uint32_t marker = 0xC001; };
struct FakeVkCommandBufferObject { std::uint32_t marker = 0xC002; };
struct FakeVkSemaphoreObject { bool timeline = false; std::uint64_t value = 0u; };

[[nodiscard]] bool hasExtension(const VkDeviceCreateInfoLoose* createInfo, const char* name)
{
    if (createInfo == nullptr || name == nullptr || createInfo->ppEnabledExtensionNames == nullptr) {
        return false;
    }
    for (std::uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        if (createInfo->ppEnabledExtensionNames[i] != nullptr &&
            std::strcmp(createInfo->ppEnabledExtensionNames[i], name) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasInstanceExtension(const VkInstanceCreateInfoLoose* createInfo, const char* name)
{
    if (createInfo == nullptr || name == nullptr || createInfo->ppEnabledExtensionNames == nullptr)
        return false;
    for (std::uint32_t i = 0; i < createInfo->enabledExtensionCount; ++i) {
        if (createInfo->ppEnabledExtensionNames[i] != nullptr &&
            std::strcmp(createInfo->ppEnabledExtensionNames[i], name) == 0)
            return true;
    }
    return false;
}

[[nodiscard]] bool hasEnabledAntiLagFeature(const VkDeviceCreateInfoLoose* createInfo)
{
    for (auto* node = reinterpret_cast<const VkBaseInStructureLoose*>(createInfo != nullptr ? createInfo->pNext : nullptr);
         node != nullptr;
         node = node->pNext) {
        if (node->sType == VkStructureTypePhysicalDeviceAntiLagFeaturesAmd) {
            const auto* antiLag = reinterpret_cast<const VkPhysicalDeviceAntiLagFeaturesAMDLoose*>(node);
            return antiLag->antiLag != 0u;
        }
    }
    return false;
}

[[nodiscard]] bool hasEnabledComputeDerivativeGroupLinear(const VkDeviceCreateInfoLoose* createInfo)
{
    for (auto* node = reinterpret_cast<const VkBaseInStructureLoose*>(createInfo != nullptr ? createInfo->pNext : nullptr);
         node != nullptr;
         node = node->pNext) {
        if (node->sType == VkStructureTypePhysicalDeviceComputeShaderDerivativesFeaturesKhr) {
            const auto* derivatives = reinterpret_cast<const VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose*>(node);
            return derivatives->computeDerivativeGroupLinear != 0u;
        }
    }
    return false;
}

[[nodiscard]] bool hasEnabledSubgroupSizeControl(const VkDeviceCreateInfoLoose* createInfo)
{
    for (auto* node = reinterpret_cast<const VkBaseInStructureLoose*>(createInfo != nullptr ? createInfo->pNext : nullptr);
         node != nullptr;
         node = node->pNext) {
        if (node->sType == VkStructureTypePhysicalDeviceVulkan13Features) {
            const auto* vulkan13 = reinterpret_cast<const VkPhysicalDeviceVulkan13FeaturesPrefix*>(node);
            return vulkan13->subgroupSizeControl != 0u;
        }
        if (node->sType == VkStructureTypePhysicalDeviceSubgroupSizeControlFeatures) {
            const auto* subgroup = reinterpret_cast<const VkPhysicalDeviceSubgroupSizeControlFeaturesLoose*>(node);
            return subgroup->subgroupSizeControl != 0u;
        }
    }
    return false;
}

[[nodiscard]] bool hasEnabledSwapchainMaintenance1(const VkDeviceCreateInfoLoose* createInfo)
{
    for (auto* node = reinterpret_cast<const VkBaseInStructureLoose*>(createInfo != nullptr ? createInfo->pNext : nullptr);
         node != nullptr;
         node = node->pNext) {
        if (node->sType == VkStructureTypePhysicalDeviceSwapchainMaintenance1FeaturesKhr) {
            const auto* maintenance = reinterpret_cast<const VkPhysicalDeviceSwapchainMaintenance1FeaturesLoose*>(node);
            return maintenance->swapchainMaintenance1 != 0u;
        }
    }
    return false;
}

[[nodiscard]] bool hasEnabledPresentTiming(const VkDeviceCreateInfoLoose* createInfo)
{
    for (auto* node = reinterpret_cast<const VkBaseInStructureLoose*>(createInfo != nullptr ? createInfo->pNext : nullptr);
         node != nullptr; node = node->pNext) {
        if (node->sType == VkStructureTypePhysicalDevicePresentTimingFeaturesExt) {
            const auto* timing = reinterpret_cast<const VkPhysicalDevicePresentTimingFeaturesEXTLoose*>(node);
            return timing->presentTiming != 0u && timing->presentAtRelativeTime != 0u;
        }
    }
    return false;
}

[[nodiscard]] bool hasEnabledTimelineSemaphore(const VkDeviceCreateInfoLoose* createInfo)
{
    for (auto* node = reinterpret_cast<const VkBaseInStructureLoose*>(createInfo != nullptr ? createInfo->pNext : nullptr);
         node != nullptr;
         node = node->pNext) {
        if (node->sType == VkStructureTypePhysicalDeviceVulkan12Features) {
            const auto* vulkan12 = reinterpret_cast<const VkPhysicalDeviceVulkan12FeaturesPrefix*>(node);
            return vulkan12->timelineSemaphore != 0u;
        }
        if (node->sType == VkStructureTypePhysicalDeviceTimelineSemaphoreFeatures) {
            const auto* timeline = reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeaturesLoose*>(node);
            return timeline->timelineSemaphore != 0u;
        }
    }
    return false;
}

VkResult APUSR_TEST_VK_CALL fakeCreateInstance(
    const VkInstanceCreateInfoLoose* createInfo, const VkAllocationCallbacks*, VkInstance* outInstance)
{
    if (outInstance == nullptr)
        return VkErrorInitializationFailed;
    g_instanceCreateHadSurfaceCapabilities2Extension =
        hasInstanceExtension(createInfo, kGetSurfaceCapabilities2Extension);
    g_instanceCreateHadSurfaceMaintenance1Extension =
        hasInstanceExtension(createInfo, kSurfaceMaintenance1Extension);
    *outInstance = g_instance;
    return VkSuccess;
}

VkResult APUSR_TEST_VK_CALL fakeEnumerateInstanceExtensionProperties(
    const char*, std::uint32_t* count, VkExtensionPropertiesLoose* properties)
{
    if (count == nullptr) return VkErrorInitializationFailed;
    constexpr std::array<const char*, 2u> supported{
        kGetSurfaceCapabilities2Extension,
        kSurfaceMaintenance1Extension,
    };
    if (properties == nullptr) {
        *count = static_cast<std::uint32_t>(supported.size());
        return VkSuccess;
    }
    const std::uint32_t written = std::min<std::uint32_t>(*count, static_cast<std::uint32_t>(supported.size()));
    for (std::uint32_t index = 0u; index < written; ++index) {
        std::strncpy(properties[index].extensionName, supported[index],
                     sizeof(properties[index].extensionName) - 1u);
        properties[index].specVersion = 1u;
    }
    *count = written;
    return written == supported.size() ? VkSuccess : VkIncomplete;
}

VkResult APUSR_TEST_VK_CALL fakeEnumeratePhysicalDevices(
    VkInstance instance, std::uint32_t* count, VkPhysicalDevice* devices)
{
    if (instance != g_instance || count == nullptr) return VkErrorInitializationFailed;
    constexpr std::uint32_t total = 2u;
    if (devices == nullptr) { *count = total; return VkSuccess; }
    const std::uint32_t n = std::min(*count, total);
    if (n > 0u) devices[0] = g_physicalDevice;
    if (n > 1u) devices[1] = g_groupPhysicalDevice;
    *count = n;
    return n == total ? VkSuccess : VkIncomplete;
}

VkResult APUSR_TEST_VK_CALL fakeCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks*,
    VkDevice* outDevice)
{
    if (outDevice == nullptr) {
        return VkErrorInitializationFailed;
    }

    const bool extensionEnabled = hasExtension(createInfo, kAntiLagExtension);
    const bool featureEnabled = hasEnabledAntiLagFeature(createInfo);
    const bool computeDerivativesExtensionEnabled = hasExtension(createInfo, kComputeDerivativesExtension);
    const bool computeDerivativeGroupLinearEnabled = hasEnabledComputeDerivativeGroupLinear(createInfo);
    const bool timelineEnabled = hasEnabledTimelineSemaphore(createInfo);
    const bool subgroupSizeControlEnabled = hasEnabledSubgroupSizeControl(createInfo);
    const bool swapchainMaintenance1Enabled = hasEnabledSwapchainMaintenance1(createInfo);
    const bool presentTimingEnabled = hasEnabledPresentTiming(createInfo);
    if (hasExtension(createInfo, kSwapchainMaintenance1Extension) &&
        !g_instanceCreateHadSurfaceMaintenance1Extension) {
        *outDevice = nullptr;
        return VkErrorExtensionNotPresent;
    }
    if (hasExtension(createInfo, kPresentTimingExtension) &&
        !g_instanceCreateHadSurfaceCapabilities2Extension) {
        *outDevice = nullptr;
        return VkErrorExtensionNotPresent;
    }
    if (physicalDevice == g_physicalDevice && createInfo != nullptr &&
        createInfo->queueCreateInfoCount != 0u && createInfo->pQueueCreateInfos != nullptr) {
        g_lastCreateQueueInfoCount = createInfo->queueCreateInfoCount;
        g_lastCreateQueueCount = 0u;
        g_lastComputeCreateQueueCount = 0u;
        g_lastComputeCreateQueuePriority = -1.0f;
        g_lastTransferCreateQueueCount = 0u;
        g_lastTransferCreateQueuePriority = -1.0f;
        g_lastCreateQueuePriorities.fill(-1.0f);
        for (std::uint32_t q = 0u; q < createInfo->queueCreateInfoCount; ++q) {
            const auto& queues = createInfo->pQueueCreateInfos[q];
            if (queues.queueFamilyIndex == 0u) {
                g_lastCreateQueueCount = queues.queueCount;
                if (queues.pQueuePriorities != nullptr) {
                    for (std::uint32_t i = 0; i < std::min<std::uint32_t>(queues.queueCount, g_lastCreateQueuePriorities.size()); ++i)
                        g_lastCreateQueuePriorities[i] = queues.pQueuePriorities[i];
                }
            } else if (queues.queueFamilyIndex == 2u) {
                g_lastComputeCreateQueueCount = queues.queueCount;
                if (queues.queueCount != 0u && queues.pQueuePriorities != nullptr)
                    g_lastComputeCreateQueuePriority = queues.pQueuePriorities[0];
            } else if (queues.queueFamilyIndex == 1u) {
                g_lastTransferCreateQueueCount = queues.queueCount;
                if (queues.queueCount != 0u && queues.pQueuePriorities != nullptr)
                    g_lastTransferCreateQueuePriority = queues.pQueuePriorities[0];
            }
        }
    }
    if (physicalDevice == g_groupPhysicalDevice) {
        g_groupCreateHadAntiLagExtension = extensionEnabled;
        g_groupCreateHadAntiLagFeature = featureEnabled;
        g_groupCreateHadKhrCalibratedTimestampsExtension = hasExtension(createInfo, kCalibratedTimestampsExtension);
        g_groupCreateHadExtCalibratedTimestampsExtension = hasExtension(createInfo, kExtCalibratedTimestampsExtension);
        *outDevice = g_groupDevice;
    } else {
        g_lastCreateHadComputeDerivativesExtension = computeDerivativesExtensionEnabled;
        g_lastCreateHadComputeDerivativeGroupLinear = computeDerivativeGroupLinearEnabled;
        g_lastCreateHadAntiLagExtension = extensionEnabled;
        g_lastCreateHadAntiLagFeature = featureEnabled;
        g_lastCreateHadTimelineSemaphore = timelineEnabled;
        g_lastCreateHadSubgroupSizeControl = subgroupSizeControlEnabled;
        g_lastCreateHadSwapchainMaintenance1Extension = hasExtension(createInfo, kSwapchainMaintenance1Extension);
        g_lastCreateHadSwapchainMaintenance1Feature = swapchainMaintenance1Enabled;
        g_lastCreateHadPresentTimingExtension = hasExtension(createInfo, kPresentTimingExtension);
        g_lastCreateHadPresentId2Extension = hasExtension(createInfo, kPresentId2Extension);
        g_lastCreateHadCalibratedTimestampsExtension = hasExtension(createInfo, kCalibratedTimestampsExtension);
        g_lastCreateHadPresentTimingFeature = presentTimingEnabled;
        g_lastCreateHadDisplayTimingExtension = hasExtension(createInfo, kDisplayTimingExtension);
        g_lastCreateHadMemoryBudgetExtension = hasExtension(createInfo, kMemoryBudgetExtension);
        *outDevice = g_device;
    }
    return VkSuccess;
}

void APUSR_TEST_VK_CALL fakeVkGetPhysicalDeviceMemoryProperties(
    vk::PhysicalDevice, vk::PhysicalDeviceMemoryProperties* properties);
void APUSR_TEST_VK_CALL fakeVkGetPhysicalDeviceMemoryProperties2(
    vk::PhysicalDevice, vk::PhysicalDeviceMemoryProperties2* properties);

VkVoidFunction APUSR_TEST_VK_CALL fakeGetInstanceProcAddr(VkInstance, const char* name)
{
    if (name != nullptr && std::string_view(name) == "vkGetPhysicalDeviceMemoryProperties")
        return reinterpret_cast<VkVoidFunction>(&fakeVkGetPhysicalDeviceMemoryProperties);
    if (name != nullptr && (std::string_view(name) == "vkGetPhysicalDeviceMemoryProperties2" ||
                            std::string_view(name) == "vkGetPhysicalDeviceMemoryProperties2KHR"))
        return reinterpret_cast<VkVoidFunction>(&fakeVkGetPhysicalDeviceMemoryProperties2);
    return nullptr;
}

void APUSR_TEST_VK_CALL fakeGetDeviceQueue(
    VkDevice device, std::uint32_t familyIndex, std::uint32_t queueIndex, VkQueue* outQueue)
{
    if (outQueue != nullptr) {
        if (device == g_groupDevice) {
            *outQueue = g_groupQueue;
        } else if (familyIndex == 2u && queueIndex == 0u) {
            *outQueue = g_framegenQueue;
        } else if (familyIndex == 1u && queueIndex == 0u) {
            *outQueue = g_framegenReplayQueue;
        } else {
            *outQueue = queueIndex == 1u ? g_secondaryQueue :
                (queueIndex == 2u ? g_framegenInputQueue :
                (queueIndex == 3u ? g_framegenPresentQueue :
                (queueIndex == 4u ? g_framegenAcquireQueue : g_queue)));
        }
    }
}

VkResult APUSR_TEST_VK_CALL fakeCreateSwapchain(
    VkDevice device,
    const VkSwapchainCreateInfoKHRLoose* createInfo,
    const VkAllocationCallbacks*,
    VkSwapchainKHR* outSwapchain)
{
    if (device != g_device || createInfo == nullptr || outSwapchain == nullptr) {
        return VkErrorInitializationFailed;
    }
    g_lastSwapchainCreateUsage = createInfo->imageUsage;
    g_lastSwapchainCreateFlags = createInfo->flags;
    g_lastSwapchainCreateMinImageCount = createInfo->minImageCount;
    g_lastSwapchainCreateSharingMode = createInfo->imageSharingMode;
    g_lastSwapchainCreateQueueFamilies.clear();
    if (createInfo->queueFamilyIndexCount != 0u && createInfo->pQueueFamilyIndices != nullptr)
        g_lastSwapchainCreateQueueFamilies.assign(
            createInfo->pQueueFamilyIndices, createInfo->pQueueFamilyIndices + createInfo->queueFamilyIndexCount);
    *outSwapchain = g_swapchain;
    return VkSuccess;
}

void APUSR_TEST_VK_CALL fakeDestroySwapchain(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks*)
{
    if (device == g_device && swapchain == g_swapchain) {
        g_destroySawInterceptor = g_swapchainDestroyInterceptorCalls == 1u;
        ++g_destroySwapchainCalls;
    }
}

void APUSR_TEST_VK_CALL fakeSwapchainDestroyInterceptor(VkDevice device, VkSwapchainKHR swapchain)
{
    if (device == g_device && swapchain == g_swapchain)
        ++g_swapchainDestroyInterceptorCalls;
}

VkResult APUSR_TEST_VK_CALL fakeGetSwapchainImages(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t* count, VkImage* images)
{
    if (device != g_device || swapchain != g_swapchain || count == nullptr) {
        return VkErrorInitializationFailed;
    }
    if (images == nullptr) {
        *count = static_cast<std::uint32_t>(g_swapchainImages.size());
        return VkSuccess;
    }
    const std::uint32_t requested = *count;
    const std::uint32_t written = std::min(requested, static_cast<std::uint32_t>(g_swapchainImages.size()));
    for (std::uint32_t index = 0; index < written; ++index) {
        images[index] = g_swapchainImages[index];
    }
    *count = written;
    return written == g_swapchainImages.size() ? VkSuccess : VkIncomplete;
}

VkResult APUSR_TEST_VK_CALL fakeAcquireNextImage(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t,
    VkSemaphore, VkFence, std::uint32_t* imageIndex)
{
    if (device != g_device || swapchain != g_swapchain || imageIndex == nullptr) {
        return VkErrorInitializationFailed;
    }
    ++g_internalAcquireCalls;
    *imageIndex = 1u;
    return VkSuccess;
}

VkResult APUSR_TEST_VK_CALL fakeAcquireNextImage2(
    VkDevice device, const VkAcquireNextImageInfoKHRLoose* info, std::uint32_t* imageIndex)
{
    if (device != g_device || info == nullptr || info->swapchain != g_swapchain || imageIndex == nullptr) {
        return VkErrorInitializationFailed;
    }
    *imageIndex = 2u;
    return VkSuboptimalKhr;
}

VkResult APUSR_TEST_VK_CALL fakeGetPhysicalDeviceSurfaceCapabilities(
    VkPhysicalDevice physicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHRLoose* outCapabilities)
{
    if (physicalDevice != g_physicalDevice || outCapabilities == nullptr)
        return VkErrorInitializationFailed;
    *outCapabilities = {};
    outCapabilities->maxImageCount = 6u;
    outCapabilities->supportedUsageFlags =
        VkImageUsageTransferSrcBit | VkImageUsageTransferDstBit | VkImageUsageSampledBit |
        VkImageUsageStorageBit | 0x10u;
    return VkSuccess;
}

VkResult APUSR_TEST_VK_CALL fakeGetPhysicalDeviceSurfaceCapabilities2(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceSurfaceInfo2KHRLoose* info,
    VkSurfaceCapabilities2KHRLoose* outCapabilities)
{
    if (physicalDevice != g_physicalDevice || info == nullptr || outCapabilities == nullptr)
        return VkErrorInitializationFailed;
    outCapabilities->surfaceCapabilities = {};
    outCapabilities->surfaceCapabilities.maxImageCount = 6u;
    outCapabilities->surfaceCapabilities.supportedUsageFlags =
        VkImageUsageTransferSrcBit | VkImageUsageTransferDstBit | VkImageUsageSampledBit |
        VkImageUsageStorageBit | 0x10u;
    for (auto* node = reinterpret_cast<VkBaseInStructureLoose*>(outCapabilities->pNext);
         node != nullptr; node = const_cast<VkBaseInStructureLoose*>(node->pNext)) {
        if (node->sType == VkStructureTypePresentTimingSurfaceCapabilitiesExt) {
            auto* timing = reinterpret_cast<VkPresentTimingSurfaceCapabilitiesEXTLoose*>(node);
            timing->presentTimingSupported = 1u;
            timing->presentAtRelativeTimeSupported = 1u;
            timing->presentStageQueries = VkPresentStageImageFirstPixelVisibleBitExt;
        }
    }
    return VkSuccess;
}

VkResult APUSR_TEST_VK_CALL fakeQueuePresent(VkQueue, const void* rawInfo)
{
    ++g_realPresentCalls;
    const auto* info = static_cast<const VkPresentInfoKHRLoose*>(rawInfo);
    if (info != nullptr) {
        for (auto* node = reinterpret_cast<const VkBaseInStructureLoose*>(info->pNext);
             node != nullptr; node = node->pNext) {
            if (node->sType == VkStructureTypePresentTimingsInfoExt) {
                const auto* timings = reinterpret_cast<const VkPresentTimingsInfoEXTLoose*>(node);
                if (timings->swapchainCount == 1u && timings->pTimingInfos != nullptr) {
                    const auto& timing = timings->pTimingInfos[0];
                    if ((timing.flags & VkPresentTimingInfoPresentAtRelativeTimeBitExt) != 0u) {
                        ++g_relativeTimedPresentCalls;
                        g_lastRelativeTargetNs = timing.targetTime;
                        g_lastRelativeTimeDomainId = timing.timeDomainId;
                        g_lastRelativeTargetStage = timing.targetTimeDomainPresentStage;
                    }
                }
            } else if (node->sType == VkStructureTypePresentTimesInfoGoogle) {
                const auto* timings = reinterpret_cast<const VkPresentTimesInfoGOOGLELoose*>(node);
                if (timings->swapchainCount == 1u && timings->pTimes != nullptr) {
                    const auto& timing = timings->pTimes[0];
                    g_observedDisplayTimingPresentIds.push_back(timing.presentID);
                    g_fakeDisplayClockNs += 16'666'667u;
                    g_pendingDisplayTimings.push_back(VkPastPresentationTimingGOOGLELoose{
                        .presentID = timing.presentID,
                        .desiredPresentTime = timing.desiredPresentTime,
                        .actualPresentTime = g_fakeDisplayClockNs,
                        .earliestPresentTime = g_fakeDisplayClockNs - 300'000u,
                        .presentMargin = 700'000u,
                    });
                }
            }
        }
    }
    return VkSuccess;
}

VkResult APUSR_TEST_VK_CALL fakeGetPastPresentationTimingGOOGLE(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t* count,
    VkPastPresentationTimingGOOGLELoose* timings)
{
    if (device != g_device || swapchain != g_swapchain || count == nullptr)
        return VkErrorInitializationFailed;
    ++g_pastDisplayTimingQueries;
    if (timings == nullptr) {
        *count = static_cast<std::uint32_t>(g_pendingDisplayTimings.size());
        return VkSuccess;
    }
    const std::uint32_t written = std::min(*count, static_cast<std::uint32_t>(g_pendingDisplayTimings.size()));
    for (std::uint32_t i = 0u; i < written; ++i)
        timings[i] = g_pendingDisplayTimings[i];
    g_pendingDisplayTimings.erase(g_pendingDisplayTimings.begin(), g_pendingDisplayTimings.begin() + written);
    *count = written;
    return g_pendingDisplayTimings.empty() ? VkSuccess : VkIncomplete;
}

VkResult APUSR_TEST_VK_CALL fakeGetSwapchainTimeDomainProperties(
    VkDevice device, VkSwapchainKHR swapchain, VkSwapchainTimeDomainPropertiesEXTLoose* properties,
    std::uint64_t*)
{
    if (device != g_device || swapchain != g_swapchain || properties == nullptr)
        return VkErrorInitializationFailed;
    // VK_EXT_present_timing permits these domains to become available only
    // after presentation has begun. Exercise that real startup behavior so the
    // host cannot regress to permanently disabling timing after one early
    // zero-domain query at vkCreateSwapchainKHR time.
    if (g_realPresentCalls == 0u) {
        properties->timeDomainCount = 0u;
        return VkSuccess;
    }
    if (properties->pTimeDomains == nullptr || properties->pTimeDomainIds == nullptr) {
        properties->timeDomainCount = 2u;
        return VkSuccess;
    }
    const std::uint32_t n = std::min(properties->timeDomainCount, 2u);
    if (n > 0u) { properties->pTimeDomains[0] = VkTimeDomainPresentStageLocalExt; properties->pTimeDomainIds[0] = 0x111u; }
    if (n > 1u) { properties->pTimeDomains[1] = VkTimeDomainSwapchainLocalExt; properties->pTimeDomainIds[1] = 0x222u; }
    properties->timeDomainCount = n;
    return n == 2u ? VkSuccess : VkIncomplete;
}

void APUSR_TEST_VK_CALL fakeAntiLagUpdate(VkDevice, const VkAntiLagDataAMDLoose* data)
{
    if (data == nullptr || data->pPresentationInfo == nullptr) {
        return;
    }
    g_antiLagCalls.push_back(AntiLagCall{
        .mode = data->mode,
        .stage = data->pPresentationInfo->stage,
        .frameIndex = data->pPresentationInfo->frameIndex,
        .maxFps = data->maxFPS,
    });
}

void APUSR_TEST_VK_CALL fakeVkGetPhysicalDeviceMemoryProperties(
    vk::PhysicalDevice, vk::PhysicalDeviceMemoryProperties* properties)
{
    if (!properties) return;
    *properties = {};
    properties->memoryTypeCount = 1u;
    properties->memoryTypes[0].propertyFlags = vk::MemoryPropertyDeviceLocalBit;
    properties->memoryTypes[0].heapIndex = 0u;
    properties->memoryHeapCount = 1u;
    properties->memoryHeaps[0].size = 1ull << 30u;
    properties->memoryHeaps[0].flags = vk::MemoryHeapDeviceLocalBit;
}

void APUSR_TEST_VK_CALL fakeVkGetPhysicalDeviceMemoryProperties2(
    vk::PhysicalDevice physicalDevice, vk::PhysicalDeviceMemoryProperties2* properties)
{
    if (!properties) return;
    ++g_memoryBudgetQueries;
    fakeVkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties->memoryProperties);
    for (auto* node = reinterpret_cast<vk::PhysicalDeviceMemoryBudgetPropertiesExt*>(properties->pNext);
         node != nullptr;) {
        if (node->sType == vk::StructureTypePhysicalDeviceMemoryBudgetPropertiesExt) {
            node->heapBudget[0] = 768ull << 20u;
            node->heapUsage[0] = 256ull << 20u;
        }
        node = reinterpret_cast<vk::PhysicalDeviceMemoryBudgetPropertiesExt*>(node->pNext);
    }
}

vk::Result APUSR_TEST_VK_CALL fakeVkCreateImage(
    vk::Device, const vk::ImageCreateInfo*, const vk::AllocationCallbacks*, vk::Image* outImage)
{
    if (!outImage) return -3;
    *outImage = reinterpret_cast<vk::Image>(new FakeVkImageObject{});
    ++g_virtualImageCreates;
    return vk::Success;
}
void APUSR_TEST_VK_CALL fakeVkDestroyImage(vk::Device, vk::Image image, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeVkImageObject*>(image);
    ++g_virtualImageDestroys;
}
void APUSR_TEST_VK_CALL fakeVkGetImageMemoryRequirements(
    vk::Device, vk::Image, vk::MemoryRequirements* requirements)
{
    if (!requirements) return;
    requirements->size = 4096u;
    requirements->alignment = 256u;
    requirements->memoryTypeBits = 1u;
}
vk::Result APUSR_TEST_VK_CALL fakeVkAllocateMemory(
    vk::Device, const vk::MemoryAllocateInfo*, const vk::AllocationCallbacks*, vk::DeviceMemory* outMemory)
{
    if (!outMemory) return -3;
    *outMemory = reinterpret_cast<vk::DeviceMemory>(new FakeVkMemoryObject{});
    return vk::Success;
}
void APUSR_TEST_VK_CALL fakeVkFreeMemory(vk::Device, vk::DeviceMemory memory, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeVkMemoryObject*>(memory);
}
vk::Result APUSR_TEST_VK_CALL fakeVkBindImageMemory(vk::Device, vk::Image, vk::DeviceMemory, vk::DeviceSize)
{
    return vk::Success;
}
vk::Result APUSR_TEST_VK_CALL fakeVkCreateCommandPool(
    vk::Device, const vk::CommandPoolCreateInfo*, const vk::AllocationCallbacks*, vk::CommandPool* outPool)
{
    if (!outPool) return -3;
    *outPool = reinterpret_cast<vk::CommandPool>(new FakeVkCommandPoolObject{});
    return vk::Success;
}
void APUSR_TEST_VK_CALL fakeVkDestroyCommandPool(vk::Device, vk::CommandPool pool, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeVkCommandPoolObject*>(pool);
}
vk::Result APUSR_TEST_VK_CALL fakeVkAllocateCommandBuffers(
    vk::Device, const vk::CommandBufferAllocateInfo* info, vk::CommandBuffer* outBuffers)
{
    if (!info || !outBuffers) return -3;
    for (std::uint32_t i = 0; i < info->commandBufferCount; ++i)
        outBuffers[i] = reinterpret_cast<vk::CommandBuffer>(new FakeVkCommandBufferObject{});
    return vk::Success;
}
vk::Result APUSR_TEST_VK_CALL fakeVkBeginCommandBuffer(vk::CommandBuffer, const vk::CommandBufferBeginInfo*)
{
    return vk::Success;
}
vk::Result APUSR_TEST_VK_CALL fakeVkEndCommandBuffer(vk::CommandBuffer)
{
    return vk::Success;
}
void APUSR_TEST_VK_CALL fakeVkCmdPipelineBarrier(
    vk::CommandBuffer, vk::PipelineStageFlags, vk::PipelineStageFlags, vk::DependencyFlags,
    std::uint32_t, const vk::MemoryBarrier*, std::uint32_t, const vk::BufferMemoryBarrier*,
    std::uint32_t imageBarrierCount, const vk::ImageMemoryBarrier*)
{
    g_virtualInitBarriers += imageBarrierCount;
}
vk::Result APUSR_TEST_VK_CALL fakeVkCreateSemaphore(
    vk::Device, const vk::SemaphoreCreateInfo* info, const vk::AllocationCallbacks*, vk::Semaphore* outSemaphore)
{
    if (!outSemaphore) return -3;
    auto* semaphore = new FakeVkSemaphoreObject{};
    if (info && info->pNext) {
        const auto* type = static_cast<const vk::SemaphoreTypeCreateInfo*>(info->pNext);
        semaphore->timeline = type->semaphoreType == vk::SemaphoreTypeTimeline;
        semaphore->value = type->initialValue;
    }
    *outSemaphore = reinterpret_cast<vk::Semaphore>(semaphore);
    return vk::Success;
}
void APUSR_TEST_VK_CALL fakeVkDestroySemaphore(vk::Device, vk::Semaphore semaphore, const vk::AllocationCallbacks*)
{
    delete reinterpret_cast<FakeVkSemaphoreObject*>(semaphore);
}
vk::Result APUSR_TEST_VK_CALL fakeVkQueueSubmit(
    vk::Queue, std::uint32_t, const vk::SubmitInfo*, vk::Fence)
{
    ++g_virtualQueueSubmits;
    return vk::Success;
}
vk::Result APUSR_TEST_VK_CALL fakeVkQueueWaitIdle(vk::Queue)
{
    ++g_virtualQueueWaitIdles;
    return vk::Success;
}
vk::Result APUSR_TEST_VK_CALL fakeVkGetSemaphoreCounterValue(
    vk::Device, vk::Semaphore semaphore, std::uint64_t* value)
{
    if (!semaphore || !value)
        return -3;
    ++g_semaphoreCounterQueries;
    *value = reinterpret_cast<FakeVkSemaphoreObject*>(semaphore)->value;
    return vk::Success;
}
vk::Result APUSR_TEST_VK_CALL fakeVkWaitSemaphores(
    vk::Device, const vk::SemaphoreWaitInfo* info, std::uint64_t)
{
    if (!info || info->semaphoreCount == 0u || !info->pSemaphores || !info->pValues)
        return -3;
    for (std::uint32_t i = 0u; i < info->semaphoreCount; ++i) {
        auto* semaphore = reinterpret_cast<FakeVkSemaphoreObject*>(info->pSemaphores[i]);
        if (semaphore)
            semaphore->value = std::max(semaphore->value, info->pValues[i]);
    }
    return vk::Success;
}

VkResult APUSR_TEST_VK_CALL fakeReleaseSwapchainImages(
    VkDevice device, const VkReleaseSwapchainImagesInfoKHRLoose* info)
{
    if (device != g_device || info == nullptr || info->swapchain != g_swapchain ||
        info->imageIndexCount != 1u || info->pImageIndices == nullptr ||
        info->pImageIndices[0] >= g_swapchainImages.size())
        return VkErrorInitializationFailed;
    ++g_releaseSwapchainImageCalls;
    g_lastReleasedSwapchainImage = info->pImageIndices[0];
    return VkSuccess;
}

VkVoidFunction APUSR_TEST_VK_CALL fakeGetDeviceProcAddr(VkDevice device, const char* name)
{
    if (name == nullptr) {
        return nullptr;
    }
    if (std::string_view(name) == "vkGetDeviceQueue") {
        return reinterpret_cast<VkVoidFunction>(&fakeGetDeviceQueue);
    }
    if (std::string_view(name) == "vkQueuePresentKHR") {
        return reinterpret_cast<VkVoidFunction>(&fakeQueuePresent);
    }
    if (std::string_view(name) == "vkCreateSwapchainKHR") {
        return reinterpret_cast<VkVoidFunction>(&fakeCreateSwapchain);
    }
    if (std::string_view(name) == "vkDestroySwapchainKHR") {
        return reinterpret_cast<VkVoidFunction>(&fakeDestroySwapchain);
    }
    if (std::string_view(name) == "vkGetSwapchainImagesKHR") {
        return reinterpret_cast<VkVoidFunction>(&fakeGetSwapchainImages);
    }
    if (std::string_view(name) == "vkAcquireNextImageKHR") {
        return reinterpret_cast<VkVoidFunction>(&fakeAcquireNextImage);
    }
    if (std::string_view(name) == "vkAcquireNextImage2KHR") {
        return reinterpret_cast<VkVoidFunction>(&fakeAcquireNextImage2);
    }
    if (std::string_view(name) == "vkReleaseSwapchainImagesKHR") {
        return reinterpret_cast<VkVoidFunction>(&fakeReleaseSwapchainImages);
    }
    if (std::string_view(name) == "vkGetSwapchainTimeDomainPropertiesEXT") {
        return reinterpret_cast<VkVoidFunction>(&fakeGetSwapchainTimeDomainProperties);
    }
    if (std::string_view(name) == "vkGetPastPresentationTimingGOOGLE") {
        return reinterpret_cast<VkVoidFunction>(&fakeGetPastPresentationTimingGOOGLE);
    }
    if (std::string_view(name) == "vkCreateImage") return reinterpret_cast<VkVoidFunction>(&fakeVkCreateImage);
    if (std::string_view(name) == "vkDestroyImage") return reinterpret_cast<VkVoidFunction>(&fakeVkDestroyImage);
    if (std::string_view(name) == "vkGetImageMemoryRequirements") return reinterpret_cast<VkVoidFunction>(&fakeVkGetImageMemoryRequirements);
    if (std::string_view(name) == "vkAllocateMemory") return reinterpret_cast<VkVoidFunction>(&fakeVkAllocateMemory);
    if (std::string_view(name) == "vkFreeMemory") return reinterpret_cast<VkVoidFunction>(&fakeVkFreeMemory);
    if (std::string_view(name) == "vkBindImageMemory") return reinterpret_cast<VkVoidFunction>(&fakeVkBindImageMemory);
    if (std::string_view(name) == "vkCreateCommandPool") return reinterpret_cast<VkVoidFunction>(&fakeVkCreateCommandPool);
    if (std::string_view(name) == "vkDestroyCommandPool") return reinterpret_cast<VkVoidFunction>(&fakeVkDestroyCommandPool);
    if (std::string_view(name) == "vkAllocateCommandBuffers") return reinterpret_cast<VkVoidFunction>(&fakeVkAllocateCommandBuffers);
    if (std::string_view(name) == "vkBeginCommandBuffer") return reinterpret_cast<VkVoidFunction>(&fakeVkBeginCommandBuffer);
    if (std::string_view(name) == "vkEndCommandBuffer") return reinterpret_cast<VkVoidFunction>(&fakeVkEndCommandBuffer);
    if (std::string_view(name) == "vkCmdPipelineBarrier") return reinterpret_cast<VkVoidFunction>(&fakeVkCmdPipelineBarrier);
    if (std::string_view(name) == "vkCreateSemaphore") return reinterpret_cast<VkVoidFunction>(&fakeVkCreateSemaphore);
    if (std::string_view(name) == "vkDestroySemaphore") return reinterpret_cast<VkVoidFunction>(&fakeVkDestroySemaphore);
    if (std::string_view(name) == "vkQueueSubmit") return reinterpret_cast<VkVoidFunction>(&fakeVkQueueSubmit);
    if (std::string_view(name) == "vkQueueWaitIdle") return reinterpret_cast<VkVoidFunction>(&fakeVkQueueWaitIdle);
    if (std::string_view(name) == "vkGetSemaphoreCounterValue") return reinterpret_cast<VkVoidFunction>(&fakeVkGetSemaphoreCounterValue);
    if (std::string_view(name) == "vkWaitSemaphores") return reinterpret_cast<VkVoidFunction>(&fakeVkWaitSemaphores);
    if (std::string_view(name) == "vkAntiLagUpdateAMD" && device == g_device) {
        return reinterpret_cast<VkVoidFunction>(&fakeAntiLagUpdate);
    }
    return nullptr;
}

void APUSR_TEST_VK_CALL fakeGetPhysicalDeviceProperties(
    VkPhysicalDevice, VkPhysicalDevicePropertiesLoose* properties)
{
    if (properties != nullptr) {
        *properties = {};
        properties->apiVersion = kApiVersion13;
    }
}

void APUSR_TEST_VK_CALL fakeGetPhysicalDeviceFeatures2(
    VkPhysicalDevice, VkPhysicalDeviceFeatures2Loose* features)
{
    if (features == nullptr) {
        return;
    }
    features->features.values[29] = 1u; // shaderStorageImageExtendedFormats
    features->features.values[32] = 1u; // shaderStorageImageWriteWithoutFormat
    features->features.values[41] = 1u; // shaderInt16

    for (auto* node = reinterpret_cast<VkBaseInStructureLoose*>(features->pNext);
         node != nullptr;
         node = const_cast<VkBaseInStructureLoose*>(node->pNext)) {
        if (node->sType == VkStructureTypePhysicalDeviceShaderFloat16Int8Features) {
            auto* fp16Int8 = reinterpret_cast<VkPhysicalDeviceShaderFloat16Int8FeaturesLoose*>(node);
            fp16Int8->shaderFloat16 = 1u;
            fp16Int8->shaderInt8 = 1u;
        } else if (node->sType == VkStructureTypePhysicalDeviceShaderIntegerDotProductFeatures) {
            auto* dot = reinterpret_cast<VkPhysicalDeviceShaderIntegerDotProductFeaturesLoose*>(node);
            dot->shaderIntegerDotProduct = 1u;
        } else if (node->sType == VkStructureTypePhysicalDeviceComputeShaderDerivativesFeaturesKhr) {
            auto* derivatives = reinterpret_cast<VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose*>(node);
            derivatives->computeDerivativeGroupLinear = 1u;
        } else if (node->sType == VkStructureTypePhysicalDeviceAntiLagFeaturesAmd) {
            auto* antiLag = reinterpret_cast<VkPhysicalDeviceAntiLagFeaturesAMDLoose*>(node);
            antiLag->antiLag = 1u;
        } else if (node->sType == VkStructureTypePhysicalDeviceTimelineSemaphoreFeatures) {
            auto* timeline = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeaturesLoose*>(node);
            timeline->timelineSemaphore = 1u;
        } else if (node->sType == VkStructureTypePhysicalDeviceSubgroupSizeControlFeatures) {
            auto* subgroupSize = reinterpret_cast<VkPhysicalDeviceSubgroupSizeControlFeaturesLoose*>(node);
            subgroupSize->subgroupSizeControl = 1u;
            subgroupSize->computeFullSubgroups = 1u;
        } else if (node->sType == VkStructureTypePhysicalDeviceSwapchainMaintenance1FeaturesKhr) {
            auto* maintenance = reinterpret_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesLoose*>(node);
            maintenance->swapchainMaintenance1 = 1u;
        } else if (node->sType == VkStructureTypePhysicalDevicePresentTimingFeaturesExt) {
            auto* timing = reinterpret_cast<VkPhysicalDevicePresentTimingFeaturesEXTLoose*>(node);
            timing->presentTiming = 1u;
            timing->presentAtRelativeTime = 1u;
        }
    }
}

void APUSR_TEST_VK_CALL fakeGetPhysicalDeviceProperties2(
    VkPhysicalDevice, VkPhysicalDeviceProperties2Loose* properties)
{
    if (properties == nullptr) {
        return;
    }
    for (auto* node = reinterpret_cast<VkBaseInStructureLoose*>(properties->pNext);
         node != nullptr;
         node = const_cast<VkBaseInStructureLoose*>(node->pNext)) {
        if (node->sType == VkStructureTypePhysicalDeviceSubgroupProperties) {
            auto* subgroup = reinterpret_cast<VkPhysicalDeviceSubgroupPropertiesLoose*>(node);
            subgroup->supportedStages = VkShaderStageComputeBit;
            subgroup->supportedOperations = VkSubgroupFeatureQuadBit;
            subgroup->quadOperationsInAllStages = 1u;
        } else if (node->sType == VkStructureTypePhysicalDeviceShaderIntegerDotProductProperties) {
            auto* dot = reinterpret_cast<VkPhysicalDeviceShaderIntegerDotProductPropertiesLoose*>(node);
            dot->integerDotProduct4x8BitPackedSignedAccelerated = 1u;
            dot->integerDotProduct4x8BitPackedUnsignedAccelerated = 1u;
            dot->integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated = 1u;
        } else if (node->sType == VkStructureTypePhysicalDeviceSubgroupSizeControlProperties) {
            auto* subgroupSize = reinterpret_cast<VkPhysicalDeviceSubgroupSizeControlPropertiesLoose*>(node);
            subgroupSize->minSubgroupSize = 32u;
            subgroupSize->maxSubgroupSize = 64u;
            subgroupSize->maxComputeWorkgroupSubgroups = 32u;
            subgroupSize->requiredSubgroupSizeStages = VkShaderStageComputeBit;
        }
    }
}

VkResult APUSR_TEST_VK_CALL fakeEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice, const char*, std::uint32_t* count, VkExtensionPropertiesLoose* properties)
{
    if (count == nullptr) return VkErrorInitializationFailed;
    if (physicalDevice == g_unsupportedPhysicalDevice) { *count = 0u; return VkSuccess; }
    constexpr std::array<const char*, 8> kKhrNames{
        kComputeDerivativesExtension, kAntiLagExtension, kSwapchainMaintenance1Extension, kPresentTimingExtension,
        kPresentId2Extension, kCalibratedTimestampsExtension, kDisplayTimingExtension,
        kMemoryBudgetExtension,
    };
    constexpr std::array<const char*, 8> kExtNames{
        kComputeDerivativesExtension, kAntiLagExtension, kSwapchainMaintenance1Extension, kPresentTimingExtension,
        kPresentId2Extension, kExtCalibratedTimestampsExtension, kDisplayTimingExtension,
        kMemoryBudgetExtension,
    };
    const auto& names = g_exposeOnlyExtCalibratedTimestamps ? kExtNames : kKhrNames;
    if (properties == nullptr) { *count = static_cast<std::uint32_t>(names.size()); return VkSuccess; }
    const std::uint32_t capacity = *count;
    const std::uint32_t written = std::min(capacity, static_cast<std::uint32_t>(names.size()));
    for (std::uint32_t i = 0u; i < written; ++i) {
        std::strncpy(properties[i].extensionName, names[i], sizeof(properties[i].extensionName) - 1u);
        properties[i].specVersion = 1u;
    }
    *count = written;
    return written == names.size() ? VkSuccess : VkIncomplete;
}

void APUSR_TEST_VK_CALL fakeGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice, std::uint32_t* count, VkQueueFamilyPropertiesLoose* properties)
{
    if (count == nullptr)
        return;
    if (properties == nullptr) {
        *count = 3u;
        return;
    }
    const std::uint32_t provided = *count;
    if (provided > 0u) {
        properties[0].queueFlags = VkQueueGraphicsBit | VkQueueComputeBit | VkQueueTransferBit;
        properties[0].queueCount = 8u;
    }
    if (provided > 1u) {
        properties[1].queueFlags = VkQueueTransferBit;
        properties[1].queueCount = 1u;
    }
    if (provided > 2u) {
        properties[2].queueFlags = VkQueueComputeBit | VkQueueTransferBit;
        properties[2].queueCount = 2u;
    }
    *count = std::min(provided, 3u);
}

std::int32_t APUSR_TEST_VK_CALL fakeBackingPresent(VkQueue queue, const void* presentInfo)
{
    ++g_backingPresentCalls;
    return queuePresent(queue, presentInfo);
}

VkDeviceCreateInfoLoose makeCreateInfo(
    VkPhysicalDeviceVulkan12FeaturesPrefix& vulkan12,
    VkPhysicalDeviceVulkan13FeaturesPrefix& vulkan13,
    VkDeviceQueueCreateInfoLoose& queueInfo)
{
    vulkan12.pNext = &vulkan13;
    queueInfo.queueFamilyIndex = 0u;
    queueInfo.queueCount = 2u;
    // Engines can deliberately give later auxiliary queues lower priority.
    // APUSR's private frame-generation queues must not inherit the final 0.0.
    static std::array<float, 2> priorities{1.0f, 0.0f};
    queueInfo.pQueuePriorities = priorities.data();

    VkDeviceCreateInfoLoose createInfo{};
    createInfo.pNext = &vulkan12;
    createInfo.queueCreateInfoCount = 1u;
    createInfo.pQueueCreateInfos = &queueInfo;
    return createInfo;
}

[[nodiscard]] bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
#if APUSR_ENABLE_PROFILER
#if defined(_WIN32)
    _putenv_s("APUSR_PROFILE", "full");
    _putenv_s("APUSR_PROFILE_DIR", "profiler-contract-output");
#else
    setenv("APUSR_PROFILE", "full", 1);
    setenv("APUSR_PROFILE_DIR", "profiler-contract-output", 1);
#endif
#endif

    Dispatch dispatch{};
    dispatch.createInstance = &fakeCreateInstance;
    dispatch.createDevice = &fakeCreateDevice;
    dispatch.enumeratePhysicalDevices = &fakeEnumeratePhysicalDevices;
    dispatch.getInstanceProcAddr = &fakeGetInstanceProcAddr;
    dispatch.getDeviceProcAddr = &fakeGetDeviceProcAddr;
    dispatch.queuePresent = &fakeQueuePresent;
    dispatch.getPhysicalDeviceFeatures2 = &fakeGetPhysicalDeviceFeatures2;
    dispatch.getPhysicalDeviceProperties = &fakeGetPhysicalDeviceProperties;
    dispatch.getPhysicalDeviceProperties2 = &fakeGetPhysicalDeviceProperties2;
    dispatch.enumerateInstanceExtensionProperties = &fakeEnumerateInstanceExtensionProperties;
    dispatch.enumerateDeviceExtensionProperties = &fakeEnumerateDeviceExtensionProperties;
    dispatch.getPhysicalDeviceQueueFamilyProperties = &fakeGetPhysicalDeviceQueueFamilyProperties;
    dispatch.getPhysicalDeviceSurfaceCapabilities = &fakeGetPhysicalDeviceSurfaceCapabilities;
    dispatch.getPhysicalDeviceSurfaceCapabilities2 = &fakeGetPhysicalDeviceSurfaceCapabilities2;
    configureDispatch(dispatch);

    if (!expect(dispatchReady(), "Vulkan host-capture fake dispatch did not become ready")) {
        return 1;
    }

    VkQueueFamilyPropertiesLoose queueProperties{};
    std::uint32_t queuePropertyCount = 1u;
    requestFrameGeneration(false);
    getPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &queuePropertyCount, &queueProperties);
    if (!expect((queueProperties.queueFlags & VkQueueOpticalFlowBitNv) == 0u,
                "Vulkan host capture exposed the virtual optical-flow role while FrameGen was not requested")) {
        return 1;
    }
    queueProperties = {};
    queuePropertyCount = 1u;
    requestFrameGeneration(true);

    VkApplicationInfoLoose appInfo{};
    appInfo.apiVersion = kApiVersion13;

    // The Detours capture path must not rewrite the application's instance
    // extension contract. This is the startup regression guard for a previously working native Vulkan
    // fix: optional WSI dependencies stay unavailable unless the host asked for
    // them itself.
    VkInstanceCreateInfoLoose startupRestoreCreate{};
    startupRestoreCreate.pApplicationInfo = &appInfo;
    const char* startupRestoreExtensions[] = {kSurfaceExtension};
    startupRestoreCreate.enabledExtensionCount = 1u;
    startupRestoreCreate.ppEnabledExtensionNames = startupRestoreExtensions;
    VkInstance instance = nullptr;
    if (!expect(createInstance(&startupRestoreCreate, nullptr, &instance) == VkSuccess && instance == g_instance &&
                !g_instanceCreateHadSurfaceCapabilities2Extension &&
                !g_instanceCreateHadSurfaceMaintenance1Extension,
                "FrameGen rewrote the host VkInstanceCreateInfo and regressed the startup contract"))
        return 1;

    // The profiler clock contract is independent of the optional WSI timing
    // stack. Probe the AMD-relevant legacy EXT spelling while the instance has
    // no surface-capabilities2/maintenance1 extensions at all.
    std::uint32_t calibrationPhysicalDeviceCount = 2u;
    std::array<VkPhysicalDevice, 2> calibrationPhysicalDevices{};
    if (!expect(enumeratePhysicalDevices(instance, &calibrationPhysicalDeviceCount, calibrationPhysicalDevices.data()) == VkSuccess,
                "calibration probe could not associate the physical device with the captured instance"))
        return 1;
    g_exposeOnlyExtCalibratedTimestamps = true;
    VkPhysicalDeviceVulkan12FeaturesPrefix calibrationVulkan12{};
    VkPhysicalDeviceVulkan13FeaturesPrefix calibrationVulkan13{};
    VkDeviceQueueCreateInfoLoose calibrationQueueInfo{};
    VkDeviceCreateInfoLoose calibrationCreateInfo =
        makeCreateInfo(calibrationVulkan12, calibrationVulkan13, calibrationQueueInfo);
    VkDevice calibrationDevice = nullptr;
    if (!expect(createDevice(g_groupPhysicalDevice, &calibrationCreateInfo, nullptr, &calibrationDevice) == VkSuccess &&
                calibrationDevice == g_groupDevice,
                "EXT calibrated-timestamp profiler probe device creation failed"))
        return 1;
#if APUSR_ENABLE_PROFILER
    if (!expect(!g_groupCreateHadKhrCalibratedTimestampsExtension &&
                g_groupCreateHadExtCalibratedTimestampsExtension,
                "GPU profiler did not negotiate VK_EXT_calibrated_timestamps when KHR was unavailable"))
        return 1;
#else
    if (!expect(!g_groupCreateHadKhrCalibratedTimestampsExtension &&
                !g_groupCreateHadExtCalibratedTimestampsExtension,
                "compiled-out profiler unexpectedly negotiated calibrated timestamps"))
        return 1;
#endif
    g_exposeOnlyExtCalibratedTimestamps = false;

    // Exercise the precise WSI path separately with a host that already owns
    // the required instance extensions. The fake returns the same handle; the
    // capture record is intentionally replaced with this host-owned contract.
    VkInstanceCreateInfoLoose instanceCreate{};
    instanceCreate.pApplicationInfo = &appInfo;
    const char* instanceExtensions[] = {
        kSurfaceExtension, kGetSurfaceCapabilities2Extension, kSurfaceMaintenance1Extension};
    instanceCreate.enabledExtensionCount = 3u;
    instanceCreate.ppEnabledExtensionNames = instanceExtensions;
    if (!expect(createInstance(&instanceCreate, nullptr, &instance) == VkSuccess && instance == g_instance &&
                g_instanceCreateHadSurfaceCapabilities2Extension &&
                g_instanceCreateHadSurfaceMaintenance1Extension,
                "host-owned WSI dependency stack was not retained for FrameGen capability discovery"))
        return 1;
    std::uint32_t physicalDeviceCount = 2u;
    std::array<VkPhysicalDevice, 2> physicalDevices{};
    if (!expect(enumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data()) == VkSuccess &&
                physicalDeviceCount == 2u && physicalDevices[0] == g_physicalDevice,
                "captured instance did not retain the physical-device association"))
        return 1;

    getPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &queuePropertyCount, &queueProperties);
    if (!expect((queueProperties.queueFlags & VkQueueOpticalFlowBitNv) == 0u,
                "Vulkan host capture exposed the legacy optical-flow role before DLSS-G requirements requested it")) {
        return 1;
    }
    queueProperties = {};
    queuePropertyCount = 1u;
    requestLegacyOpticalFlowQueueRole(true);
    getPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &queuePropertyCount, &queueProperties);
    if (!expect((queueProperties.queueFlags & VkQueueOpticalFlowBitNv) != 0u && queueProperties.queueCount == 8u,
                "Vulkan DLSS-G requirements did not expose the legacy Streamline optical-flow queue role")) {
        return 1;
    }

    requestAntiLag(true);
    if (!expect(antiLagSupported(g_physicalDevice),
                "VK_AMD_anti_lag support query rejected a supported physical device") ||
        !expect(!antiLagSupported(g_unsupportedPhysicalDevice),
                "VK_AMD_anti_lag support query fabricated support on an unsupported physical device")) {
        return 1;
    }

    VkPhysicalDeviceVulkan12FeaturesPrefix vulkan12{};
    VkPhysicalDeviceVulkan13FeaturesPrefix vulkan13{};
    VkPhysicalDeviceAntiLagFeaturesAMDLoose hostAntiLag{};
    VkDeviceQueueCreateInfoLoose queueInfo{};
    VkDeviceCreateInfoLoose createInfo = makeCreateInfo(vulkan12, vulkan13, queueInfo);
    vulkan13.pNext = &hostAntiLag;
    VkDevice device = nullptr;
    if (!expect(createDevice(g_physicalDevice, &createInfo, nullptr, &device) == VkSuccess && device == g_device,
                "single-GPU vkCreateDevice path failed")) {
        return 1;
    }
    if (!expect(g_lastCreateHadAntiLagExtension && g_lastCreateHadAntiLagFeature,
                "single-GPU Reflex request did not enable VK_AMD_anti_lag extension+feature")) {
        return 1;
    }
    if (!expect(g_lastCreateHadComputeDerivativesExtension && g_lastCreateHadComputeDerivativeGroupLinear,
                "FSR4 Vulkan device creation did not negotiate VK_KHR_compute_shader_derivatives linear-quad topology")) {
        return 1;
    }
    if (!expect(g_lastCreateHadTimelineSemaphore && vulkan12.timelineSemaphore == 0u,
                "FrameGen did not enable and restore the Vulkan 1.2 timeline-semaphore feature")) {
        return 1;
    }
    if (!expect(g_lastCreateHadSubgroupSizeControl && vulkan13.subgroupSizeControl == 0u,
                "FrameGen did not enable and restore Vulkan subgroup-size control for native wave64")) {
        return 1;
    }
    if (!expect(g_lastCreateHadSwapchainMaintenance1Extension && g_lastCreateHadSwapchainMaintenance1Feature,
                "FrameGen did not negotiate swapchain-maintenance1 for deadline-safe direct WSI output")) {
        return 1;
    }
    if (!expect(g_lastCreateHadPresentTimingExtension && g_lastCreateHadPresentId2Extension &&
                g_lastCreateHadCalibratedTimestampsExtension && g_lastCreateHadPresentTimingFeature,
                "FrameGen did not negotiate the complete VK_EXT_present_timing dependency stack")) {
        return 1;
    }
#if APUSR_ENABLE_PROFILER
    if (!expect(g_lastCreateHadDisplayTimingExtension && g_lastCreateHadMemoryBudgetExtension,
                "enabled profiler did not negotiate optional Vulkan display-timing and memory-budget telemetry")) {
        return 1;
    }
#else
    if (!expect(!g_lastCreateHadDisplayTimingExtension && !g_lastCreateHadMemoryBudgetExtension,
                "compiled-out profiler unexpectedly changed Vulkan extension negotiation")) {
        return 1;
    }
#endif
    if (!expect(hostAntiLag.antiLag == 0u,
                "host-provided VK_AMD_anti_lag feature state was not restored after vkCreateDevice")) {
        return 1;
    }
    if (!expect(queueInfo.queueCount == 2u && g_lastCreateQueueInfoCount == 3u &&
                g_lastCreateQueueCount == 5u && g_lastComputeCreateQueueCount == 1u &&
                g_lastTransferCreateQueueCount == 1u &&
                g_lastCreateQueuePriorities[0] == 1.0f && g_lastCreateQueuePriorities[1] == 0.0f &&
                g_lastCreateQueuePriorities[2] == 1.0f && g_lastCreateQueuePriorities[3] == 1.0f &&
                g_lastCreateQueuePriorities[4] == 1.0f && g_lastComputeCreateQueuePriority == 1.0f &&
                g_lastTransferCreateQueuePriority == 1.0f,
                "FrameGen clean-sheet queue reservation did not preserve app priorities and reserve private compute/transfer lanes")) {
        return 1;
    }
    queueProperties = {};
    queuePropertyCount = 1u;
    getPhysicalDeviceQueueFamilyProperties(g_physicalDevice, &queuePropertyCount, &queueProperties);
    if (!expect((queueProperties.queueFlags & VkQueueOpticalFlowBitNv) == 0u,
                "legacy optical-flow queue role remained visible after logical-device creation")) {
        return 1;
    }
    ApusrVulkanHostDeviceInfo capturedDevice{};
    ApusrVulkanHostQueueInfo capturedInputQueue{};
    ApusrVulkanHostQueueInfo capturedFramegenPresentQueue{};
    ApusrVulkanHostQueueInfo capturedFramegenAcquireQueue{};
    ApusrVulkanHostQueueInfo capturedFramegenQueue{};
    ApusrVulkanHostQueueInfo capturedReplayQueue{};
    if (!expect(getDeviceInfo(device, &capturedDevice) && capturedDevice.queueCount == 7u &&
                (capturedDevice.negotiationFlags & APUSR_VK_HOST_FRAMEGEN_TIMELINE) != 0u &&
                (capturedDevice.negotiationFlags & APUSR_VK_HOST_FRAMEGEN_SUBGROUP64) != 0u &&
                (capturedDevice.negotiationFlags & APUSR_VK_HOST_FSR4_SUBGROUP32) != 0u &&
                (capturedDevice.negotiationFlags & APUSR_VK_HOST_FSR4_SUBGROUP64) != 0u &&
                getQueueInfo(device, 2u, &capturedInputQueue) && capturedInputQueue.queue == g_framegenInputQueue &&
                capturedInputQueue.familyIndex == 0u && capturedInputQueue.queueIndex == 2u &&
                (capturedInputQueue.ownershipFlags & (APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR | APUSR_VK_HOST_QUEUE_ROLE_INPUT)) ==
                    (APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR | APUSR_VK_HOST_QUEUE_ROLE_INPUT) &&
                getQueueInfo(device, 3u, &capturedFramegenPresentQueue) &&
                capturedFramegenPresentQueue.queue == g_framegenPresentQueue &&
                capturedFramegenPresentQueue.familyIndex == 0u && capturedFramegenPresentQueue.queueIndex == 3u &&
                (capturedFramegenPresentQueue.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_PRESENTATION) != 0u &&
                getQueueInfo(device, 4u, &capturedFramegenAcquireQueue) &&
                capturedFramegenAcquireQueue.queue == g_framegenAcquireQueue &&
                capturedFramegenAcquireQueue.familyIndex == 0u && capturedFramegenAcquireQueue.queueIndex == 4u &&
                (capturedFramegenAcquireQueue.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_ACQUIRE) != 0u &&
                getQueueInfo(device, 5u, &capturedFramegenQueue) && capturedFramegenQueue.queue == g_framegenQueue &&
                capturedFramegenQueue.familyIndex == 2u && capturedFramegenQueue.queueIndex == 0u &&
                (capturedFramegenQueue.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_GENERATION) != 0u &&
                getQueueInfo(device, 6u, &capturedReplayQueue) && capturedReplayQueue.queue == g_framegenReplayQueue &&
                capturedReplayQueue.familyIndex == 1u && capturedReplayQueue.queueIndex == 0u &&
                (capturedReplayQueue.ownershipFlags & (APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR | APUSR_VK_HOST_QUEUE_ROLE_REPLAY_TRANSFER)) ==
                    (APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR | APUSR_VK_HOST_QUEUE_ROLE_REPLAY_TRANSFER),
                "FrameGen did not capture private input/presentation/acquire, async-compute generation, and transfer replay queues")) {
        return 1;
    }
    if (!expect(antiLagAvailable(device), "enabled VK_AMD_anti_lag device was not reported available")) {
        return 1;
    }

    // Merely advertising FrameGen capability must not mutate the application's
    // ordinary real swapchain. Some hosts create their startup swapchain while FG is
    // inactive and recreates it when FG is enabled; changing WSI allocation,
    // sharing, STORAGE usage, or timing flags before that transition is both
    // unnecessary and a compatibility hazard.
    setFrameGenerationActive(false);
    VkSwapchainCreateInfoKHRLoose inactiveSwapchainCreate{};
    inactiveSwapchainCreate.surface = reinterpret_cast<VkSurfaceKHR>(0x4200);
    inactiveSwapchainCreate.minImageCount = 2u;
    inactiveSwapchainCreate.imageFormat = 44u;
    inactiveSwapchainCreate.imageColorSpace = 0u;
    inactiveSwapchainCreate.imageExtent = {1920u, 1080u};
    inactiveSwapchainCreate.imageArrayLayers = 1u;
    inactiveSwapchainCreate.imageUsage = 0x10u;
    inactiveSwapchainCreate.imageSharingMode = 0u;
    inactiveSwapchainCreate.presentMode = 2u;
    VkSwapchainKHR inactiveSwapchain = nullptr;
    if (!expect(createSwapchain(device, &inactiveSwapchainCreate, nullptr, &inactiveSwapchain) == VkSuccess &&
                inactiveSwapchain == g_swapchain &&
                g_lastSwapchainCreateUsage == inactiveSwapchainCreate.imageUsage &&
                g_lastSwapchainCreateMinImageCount == inactiveSwapchainCreate.minImageCount &&
                g_lastSwapchainCreateSharingMode == inactiveSwapchainCreate.imageSharingMode &&
                g_lastSwapchainCreateQueueFamilies.empty() &&
                (g_lastSwapchainCreateFlags & VkSwapchainCreatePresentTimingBitExt) == 0u,
                "FrameGen-requested but inactive startup WSI contract was mutated")) {
        return 1;
    }
    destroySwapchain(device, inactiveSwapchain, nullptr);
    g_destroySwapchainCalls = 0u;
    g_swapchainDestroyInterceptorCalls = 0u;
    g_destroySawInterceptor = false;

    setFrameGenerationActive(true);

    VkSwapchainCreateInfoKHRLoose swapchainCreate{};
    swapchainCreate.surface = reinterpret_cast<VkSurfaceKHR>(0x4200);
    swapchainCreate.minImageCount = 2u;
    swapchainCreate.imageFormat = 44u;
    swapchainCreate.imageColorSpace = 0u;
    swapchainCreate.imageExtent = {1920u, 1080u};
    swapchainCreate.imageArrayLayers = 1u;
    swapchainCreate.imageUsage = 0x10u;
    swapchainCreate.imageSharingMode = 0u;
    swapchainCreate.presentMode = 2u;
    VkSwapchainKHR swapchain = nullptr;
    if (!expect(createSwapchain(device, &swapchainCreate, nullptr, &swapchain) == VkSuccess &&
                swapchain == g_swapchain,
                "swapchain creation was not captured")) {
        return 1;
    }
    if (!expect((g_lastSwapchainCreateFlags & VkSwapchainCreatePresentTimingBitExt) != 0u,
                "FrameGen real WSI did not enable VK_EXT_present_timing on a capable FIFO swapchain") ||
        !expect(g_lastSwapchainCreateUsage ==
                    (0x10u | VkImageUsageTransferSrcBit | VkImageUsageTransferDstBit | VkImageUsageStorageBit),
                "FrameGen real WSI did not negotiate transfer replay plus direct-storage generation") ||
        !expect(g_lastSwapchainCreateMinImageCount == 3u,
                "FrameGen did not guarantee the minimum triple-buffer real WSI contract") ||
        !expect(g_lastSwapchainCreateSharingMode == 1u &&
                g_lastSwapchainCreateQueueFamilies == std::vector<std::uint32_t>({0u, 1u, 2u}),
                "private real WSI images were not concurrent-shared with graphics, transfer replay, and async generation")) {
        return 1;
    }

    std::uint32_t swapchainImageCount = 0u;
    if (!expect(getSwapchainImages(device, swapchain, &swapchainImageCount, nullptr) == VkSuccess &&
                swapchainImageCount == g_swapchainImages.size(),
                "swapchain image-count query was not captured")) {
        return 1;
    }
    std::array<VkImage, 3> swapchainImages{};
    swapchainImageCount = static_cast<std::uint32_t>(swapchainImages.size());
    if (!expect(getSwapchainImages(device, swapchain, &swapchainImageCount, swapchainImages.data()) == VkSuccess &&
                swapchainImageCount == swapchainImages.size() && g_virtualImageCreates == swapchainImages.size() &&
                swapchainImages[0] != nullptr && swapchainImages[1] != nullptr && swapchainImages[2] != nullptr &&
                swapchainImages != g_swapchainImages,
                "FrameGen did not expose APUSR-owned off-screen backbuffers")) {
        return 1;
    }

    std::uint32_t acquiredIndex = APUSR_VK_HOST_INVALID_IMAGE_INDEX;
    const VkSemaphore publicAcquire0 = reinterpret_cast<VkSemaphore>(0x4300);
    if (!expect(acquireNextImage(device, swapchain, 0u, publicAcquire0, nullptr, &acquiredIndex) == VkSuccess &&
                acquiredIndex == 0u && g_internalAcquireCalls == 0u,
                "virtual vkAcquireNextImageKHR touched the real WSI acquire path")) {
        return 1;
    }
    VkAcquireNextImageInfoKHRLoose acquire2{};
    acquire2.swapchain = swapchain;
    acquire2.semaphore = reinterpret_cast<VkSemaphore>(0x4301);
    if (!expect(acquireNextImage2(device, &acquire2, &acquiredIndex) == VkSuccess && acquiredIndex == 1u &&
                g_internalAcquireCalls == 0u,
                "virtual vkAcquireNextImage2KHR touched the real WSI acquire path")) {
        return 1;
    }

    ApusrVulkanHostSwapchainInfo capturedSwapchain{};
    void* capturedImage = nullptr;
    void* capturedRealImage = nullptr;
    if (!expect(getSwapchainInfo(swapchain, &capturedSwapchain) &&
                capturedSwapchain.device == device && capturedSwapchain.width == 1920u &&
                capturedSwapchain.height == 1080u && capturedSwapchain.imageFormat == 44u &&
                capturedSwapchain.imageColorSpace == 0u && capturedSwapchain.imageCount == 3u &&
                capturedSwapchain.realImageCount == 3u && capturedSwapchain.latestAcquiredImageIndex == 1u &&
                capturedSwapchain.acquireSerial == 2u && capturedSwapchain.originalImageUsage == 0x10u &&
                capturedSwapchain.realImageUsage ==
                    (0x10u | VkImageUsageTransferSrcBit | VkImageUsageTransferDstBit | VkImageUsageStorageBit) &&
                capturedSwapchain.imageUsage == (0x10u | VkImageUsageTransferSrcBit | VkImageUsageSampledBit) &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_REQUESTED) != 0u &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_VIRTUALIZED) != 0u &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_TRANSFER_SRC) != 0u &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_TRANSFER_DST) != 0u &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_SAMPLED) == 0u &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_STORAGE) != 0u &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_MAINTENANCE1) != 0u &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_EXTRA_IMAGES) != 0u &&
                (capturedSwapchain.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_PRESENT_TIMING_RELATIVE) != 0u &&
                capturedSwapchain.minImageCount == 3u && g_virtualInitBarriers == 3u,
                "captured virtual swapchain metadata/acquire state was incomplete") ||
        !expect(getSwapchainImage(swapchain, 1u, &capturedImage) && capturedImage == swapchainImages[1],
                "public virtual swapchain image lookup failed") ||
        !expect(getRealSwapchainImage(swapchain, 1u, &capturedRealImage) && capturedRealImage == g_swapchainImages[1],
                "private real WSI image lookup failed")) {
        return 1;
    }

    FakeVkSemaphoreObject retainedSourceDone{true, 0u};
    const std::uint32_t releaseSubmitsBefore = g_virtualQueueSubmits;
    if (!expect(releaseVirtualSwapchainImage(
                    swapchain, 0u, &retainedSourceDone, 1u, nullptr, 0u) &&
                releaseVirtualSwapchainImage(
                    swapchain, 1u, &retainedSourceDone, 2u, nullptr, 0u) &&
                g_virtualQueueSubmits == releaseSubmitsBefore,
                "virtual-source release unexpectedly submitted a GPU bridge")) {
        return 1;
    }
    if (!expect(acquireNextImage(device, swapchain, 0u, reinterpret_cast<VkSemaphore>(0x4302), nullptr, &acquiredIndex) == VkSuccess &&
                acquiredIndex == 2u,
                "virtual acquire did not select the only dependency-free retained-source slot")) {
        return 1;
    }
    const std::uint32_t counterQueriesBeforeBlockedAcquire = g_semaphoreCounterQueries;
    if (!expect(acquireNextImage(device, swapchain, 0u, reinterpret_cast<VkSemaphore>(0x4303), nullptr, &acquiredIndex) == VkNotReady,
                "virtual acquire reused a source slot before its consumer timeline completed") ||
        !expect(g_semaphoreCounterQueries == counterQueriesBeforeBlockedAcquire + 1u,
                "virtual acquire re-queried a shared consumer timeline for multiple blocked slots")) {
        return 1;
    }
    retainedSourceDone.value = 1u;
    if (!expect(acquireNextImage(device, swapchain, 0u, reinterpret_cast<VkSemaphore>(0x4304), nullptr, &acquiredIndex) == VkSuccess &&
                acquiredIndex == 0u,
                "virtual acquire did not reuse a retained source immediately after its dependency completed") ||
        !expect(waitVirtualSwapchainReleaseDependencies(swapchain) && retainedSourceDone.value >= 2u,
                "virtual swapchain teardown did not drain retained-source timeline dependencies") ||
        !expect(acquireNextImage(device, swapchain, 0u, reinterpret_cast<VkSemaphore>(0x4305), nullptr, &acquiredIndex) == VkSuccess &&
                acquiredIndex == 1u,
                "drained retained-source slot did not become reusable")) {
        return 1;
    }

    const std::uint32_t acquireCallsBeforeInternal = g_internalAcquireCalls;
    std::uint32_t generatedImageIndex = APUSR_VK_HOST_INVALID_IMAGE_INDEX;
    if (!expect(acquireNextImageForFrameGeneration(
                    device, swapchain, 0u, reinterpret_cast<VkSemaphore>(0x4300), &generatedImageIndex) == VkSuccess &&
                generatedImageIndex == 1u && g_internalAcquireCalls == acquireCallsBeforeInternal + 1u,
                "APUSR-owned nonblocking frame-generation acquire did not bypass the public acquire path")) {
        return 1;
    }
    if (!expect(releaseRealSwapchainImage(device, swapchain, generatedImageIndex) &&
                g_releaseSwapchainImageCalls == 1u && g_lastReleasedSwapchainImage == generatedImageIndex,
                "swapchain-maintenance1 did not release an unpresented private generated WSI target")) {
        return 1;
    }

    const std::size_t antiLagBeforeFrame77 = g_antiLagCalls.size();
    if (!expect(antiLagInput(device, 77u, APUSR_VK_HOST_ANTI_LAG_ON, 144u) &&
                antiLagInput(device, 77u, APUSR_VK_HOST_ANTI_LAG_ON, 144u) &&
                antiLagInput(device, 77u, APUSR_VK_HOST_ANTI_LAG_ON, 144u),
                "Anti-Lag input update failed") ||
        !expect(g_antiLagCalls.size() == antiLagBeforeFrame77 + 1u,
                "duplicate SimulationStart markers invoked VK_AMD_anti_lag INPUT more than once for one frame")) {
        return 1;
    }
    antiLagSetPresentFrame(device, 77u);
    antiLagSetPresentFrame(device, 77u);
    setQueuePresentInterceptor(reinterpret_cast<void*>(&fakeBackingPresent));

    const std::uint32_t backingBeforeUntracked = g_backingPresentCalls;
    const std::uint32_t realBeforeUntracked = g_realPresentCalls;
    VkPresentInfoKHRLoose untrackedPresentInfo{};
    if (!expect(queuePresent(g_untrackedQueue, &untrackedPresentInfo) == VkSuccess,
                "untracked Vulkan queue did not pass through to the system present") ||
        !expect(g_backingPresentCalls == backingBeforeUntracked,
                "untracked Vulkan queue was incorrectly routed through backing Streamline") ||
        !expect(g_realPresentCalls == realBeforeUntracked + 1u,
                "untracked Vulkan queue was not presented exactly once")) {
        return 1;
    }
    const std::uint32_t backingBeforeTracked = g_backingPresentCalls;
    const std::uint32_t realBeforeTracked = g_realPresentCalls;
    const std::uint32_t presentedImageIndex = 2u;
    VkPresentInfoKHRLoose presentPayload{};
    presentPayload.swapchainCount = 1u;
    presentPayload.pSwapchains = &swapchain;
    presentPayload.pImageIndices = &presentedImageIndex;
    if (!expect(queuePresent(g_queue, &presentPayload) == VkSuccess,
                "nested backing-present path failed")) {
        return 1;
    }

    if (!expect(g_backingPresentCalls == backingBeforeTracked + 1u &&
                g_realPresentCalls == realBeforeTracked + 1u,
                "present seam did not execute exactly one backing hook pass and one real present")) {
        return 1;
    }
    if (!expect(g_antiLagCalls.size() == 2u &&
                g_antiLagCalls[0].stage == 0 && g_antiLagCalls[0].frameIndex == 77u &&
                g_antiLagCalls[1].stage == 1 && g_antiLagCalls[1].frameIndex == 77u &&
                g_antiLagCalls[1].mode == static_cast<std::int32_t>(APUSR_VK_HOST_ANTI_LAG_ON) &&
                g_antiLagCalls[1].maxFps == 144u,
                "Anti-Lag INPUT/PRESENT updates did not preserve the same frame contract")) {
        return 1;
    }

    // Teardown must not use the rejected mode-only OFF update. Arm a complete OFF
    // INPUT/PRESENT contract instead: INPUT is emitted immediately and the same
    // synthetic frame is paired with PRESENT on the next real source present.
    const std::size_t beforePairedDisable = g_antiLagCalls.size();
    if (!expect(antiLagInput(device, 78u, APUSR_VK_HOST_ANTI_LAG_ON, 300u),
                "pre-disable Anti-Lag input failed") ||
        !expect(antiLagArmPairedDisable(device), "paired Anti-Lag disable could not be armed") ||
        !expect(g_antiLagCalls.size() == beforePairedDisable + 2u &&
                g_antiLagCalls[beforePairedDisable].stage == 0 &&
                g_antiLagCalls[beforePairedDisable].frameIndex == 78u &&
                g_antiLagCalls.back().stage == 0 &&
                g_antiLagCalls.back().mode == static_cast<std::int32_t>(APUSR_VK_HOST_ANTI_LAG_OFF) &&
                g_antiLagCalls.back().frameIndex >= 0x8000000000000000ull,
                "paired Anti-Lag disable did not emit exactly one OFF INPUT control contract")) {
        return 1;
    }
    const std::uint64_t disableFrameIndex = g_antiLagCalls.back().frameIndex;
    if (!expect(antiLagArmPairedDisable(device) && g_antiLagCalls.size() == beforePairedDisable + 2u,
                "duplicate paired Anti-Lag disable emitted a second INPUT")) {
        return 1;
    }
    if (!expect(queuePresent(g_queue, &presentPayload) == VkSuccess &&
                g_antiLagCalls.size() == beforePairedDisable + 3u &&
                g_antiLagCalls.back().stage == 1 &&
                g_antiLagCalls.back().mode == static_cast<std::int32_t>(APUSR_VK_HOST_ANTI_LAG_OFF) &&
                g_antiLagCalls.back().maxFps == 0u &&
                g_antiLagCalls.back().frameIndex == disableFrameIndex,
                "paired Anti-Lag disable did not bind matching OFF PRESENT to the next real present")) {
        return 1;
    }

    const std::uint32_t directRealBefore = g_realPresentCalls;
    const std::uint32_t directBackingBefore = g_backingPresentCalls;
    const std::size_t directAntiLagBefore = g_antiLagCalls.size();
    const std::uint32_t generatedPresentedIndex = 1u;
    VkPresentInfoKHRLoose generatedPresentPayload{};
    generatedPresentPayload.swapchainCount = 1u;
    generatedPresentPayload.pSwapchains = &swapchain;
    generatedPresentPayload.pImageIndices = &generatedPresentedIndex;
    if (!expect(queuePresentDirect(g_queue, &generatedPresentPayload) == VkSuccess &&
                g_realPresentCalls == directRealBefore + 1u && g_backingPresentCalls == directBackingBefore &&
                g_antiLagCalls.size() == directAntiLagBefore,
                "generated-frame direct present did not bypass backing Streamline and Anti-Lag")) {
        return 1;
    }

#if APUSR_ENABLE_PROFILER
    const std::uint32_t profiledGeneratedRealBefore = g_realPresentCalls;
    if (!expect(queuePresentFrameGeneration(
                    g_queue, &generatedPresentPayload, 0xA501u,
                    APUSR_VK_HOST_PRESENT_GENERATED | APUSR_VK_HOST_PRESENT_PROFILE_CAPTURE) == VkSuccess &&
                g_realPresentCalls == profiledGeneratedRealBefore + 1u &&
                !g_observedDisplayTimingPresentIds.empty() && g_pastDisplayTimingQueries != 0u,
                "profiled generated-frame present did not preserve direct WSI presentation while capturing actual display timing")) {
        return 1;
    }
#endif

    capturedSwapchain = {};
    if (!expect(getSwapchainInfo(swapchain, &capturedSwapchain) &&
                capturedSwapchain.latestPresentedImageIndex == 1u && capturedSwapchain.presentSerial == 2u,
                "private real WSI presents corrupted public virtual presentation state")) {
        return 1;
    }

    // An explicit PCL PresentStart correlation is authoritative. If its frame
    // has no matching SimulationStart/input record, do not silently substitute a
    // different pending frame on that same present.
    if (!expect(antiLagInput(device, 88u, APUSR_VK_HOST_ANTI_LAG_ON, 120u),
                "second Anti-Lag input update failed")) {
        return 1;
    }
    antiLagSetPresentFrame(device, 99u);
    const std::size_t beforeMismatchPresent = g_antiLagCalls.size();
    if (!expect(queuePresent(g_queue, &presentPayload) == VkSuccess,
                "explicit unmatched present path failed")) {
        return 1;
    }
    if (!expect(g_antiLagCalls.size() == beforeMismatchPresent,
                "explicit unmatched frame incorrectly fell back to another frame on the same present")) {
        return 1;
    }
    if (!expect(queuePresent(g_queue, &presentPayload) == VkSuccess &&
                g_antiLagCalls.back().stage == 1 && g_antiLagCalls.back().frameIndex == 88u,
                "FIFO fallback did not recover the unmatched input on a later unmarked present")) {
        return 1;
    }

    // Out-of-band/batched hosts can publish multiple PresentStart markers
    // before the corresponding queue presents. Preserve their order instead of
    // allowing the second marker to overwrite the first.
    if (!expect(antiLagInput(device, 100u, APUSR_VK_HOST_ANTI_LAG_ON, 90u) &&
                antiLagInput(device, 101u, APUSR_VK_HOST_ANTI_LAG_ON, 90u),
                "batched Anti-Lag input updates failed")) {
        return 1;
    }
    antiLagSetPresentFrame(device, 100u);
    antiLagSetPresentFrame(device, 101u);
    const std::size_t beforeBatchedPresents = g_antiLagCalls.size();
    if (!expect(queuePresent(g_queue, &presentPayload) == VkSuccess &&
                queuePresent(g_queue, &presentPayload) == VkSuccess,
                "queued present-marker path failed")) {
        return 1;
    }
    if (!expect(g_antiLagCalls.size() == beforeBatchedPresents + 2u &&
                g_antiLagCalls[beforeBatchedPresents].stage == 1 &&
                g_antiLagCalls[beforeBatchedPresents].frameIndex == 100u &&
                g_antiLagCalls[beforeBatchedPresents + 1u].stage == 1 &&
                g_antiLagCalls[beforeBatchedPresents + 1u].frameIndex == 101u,
                "queued present markers were not consumed in exact FIFO order")) {
        return 1;
    }

    // Relative timing must preserve the same generated/source Anti-Lag split as
    // ordinary physical presents. G is APUSR-owned and may not consume the
    // title's queued PRESENT marker; the following real/source present consumes
    // it exactly once. This specifically protects the two-sided pacing path.
    if (!expect(antiLagInput(device, 102u, APUSR_VK_HOST_ANTI_LAG_ON, 90u),
                "relative-pacing Anti-Lag input update failed")) {
        return 1;
    }
    antiLagSetPresentFrame(device, 102u);
    const std::size_t antiLagCallsBeforeRelative = g_antiLagCalls.size();
    const std::uint32_t timingCallsBefore = g_relativeTimedPresentCalls;
    constexpr std::uint64_t relativeDelayNs = 5'000'000u;
    bool generatedRelativeScheduled = false;
    if (!expect(queuePresentGeneratedRelative(
                    g_queue, &presentPayload, relativeDelayNs, &generatedRelativeScheduled) == VkSuccess &&
                generatedRelativeScheduled &&
                g_relativeTimedPresentCalls == timingCallsBefore + 1u &&
                g_antiLagCalls.size() == antiLagCallsBeforeRelative &&
                g_lastRelativeTargetNs == relativeDelayNs && g_lastRelativeTimeDomainId == 0x222u &&
                g_lastRelativeTargetStage == 0u,
                "generated relative WSI pacing consumed the title's Anti-Lag PRESENT marker or lost timing")) {
        return 1;
    }

    bool relativeScheduled = false;
    if (!expect(queuePresentSourceRelative(g_queue, &presentPayload, relativeDelayNs, &relativeScheduled) == VkSuccess &&
                relativeScheduled && g_relativeTimedPresentCalls == timingCallsBefore + 2u &&
                g_antiLagCalls.size() == antiLagCallsBeforeRelative + 1u &&
                g_antiLagCalls.back().stage == 1 && g_antiLagCalls.back().frameIndex == 102u &&
                g_lastRelativeTargetNs == relativeDelayNs && g_lastRelativeTimeDomainId == 0x222u &&
                g_lastRelativeTargetStage == 0u,
                "relative WSI source pacing did not consume exactly one queued Anti-Lag PRESENT marker")) {
        return 1;
    }


#if APUSR_ENABLE_PROFILER
    const std::uint32_t timingCallsBeforeProfiledRelative = g_relativeTimedPresentCalls;
    const std::size_t displayIdsBeforeProfiledRelative = g_observedDisplayTimingPresentIds.size();
    bool profiledRelativeScheduled = false;
    if (!expect(queuePresentSourceRelativeFrameGeneration(
                    g_queue, &presentPayload, 0xA502u, APUSR_VK_HOST_PRESENT_PROFILE_CAPTURE,
                    relativeDelayNs, &profiledRelativeScheduled) == VkSuccess &&
                profiledRelativeScheduled &&
                g_relativeTimedPresentCalls == timingCallsBeforeProfiledRelative + 1u &&
                g_observedDisplayTimingPresentIds.size() == displayIdsBeforeProfiledRelative + 1u &&
                g_memoryBudgetQueries != 0u,
                "profiled source-relative present did not preserve VK_EXT_present_timing while collecting display/memory telemetry")) {
        return 1;
    }
#endif

    // Vulkan explicitly forbids enabling this feature on a multi-GPU device
    // group. The SR device contract must still succeed without Anti-Lag.
    VkPhysicalDeviceVulkan12FeaturesPrefix groupVulkan12{};
    VkPhysicalDeviceVulkan13FeaturesPrefix groupVulkan13{};
    VkDeviceQueueCreateInfoLoose groupQueueInfo{};
    VkDeviceCreateInfoLoose groupCreateInfo = makeCreateInfo(groupVulkan12, groupVulkan13, groupQueueInfo);
    std::array<VkPhysicalDevice, 2> groupDevices{g_groupPhysicalDevice, g_physicalDevice};
    VkDeviceGroupDeviceCreateInfoLoose group{};
    group.physicalDeviceCount = static_cast<std::uint32_t>(groupDevices.size());
    group.pPhysicalDevices = groupDevices.data();
    group.pNext = groupCreateInfo.pNext;
    groupCreateInfo.pNext = &group;

    VkDevice groupDevice = nullptr;
    if (!expect(createDevice(g_groupPhysicalDevice, &groupCreateInfo, nullptr, &groupDevice) == VkSuccess &&
                groupDevice == g_groupDevice,
                "multi-GPU device group should keep the valid SR device creation path")) {
        return 1;
    }
    if (!expect(!g_groupCreateHadAntiLagExtension && !g_groupCreateHadAntiLagFeature &&
                !antiLagAvailable(groupDevice),
                "multi-GPU device group incorrectly enabled VK_AMD_anti_lag")) {
        return 1;
    }

    setSwapchainDestroyInterceptor(reinterpret_cast<void*>(&fakeSwapchainDestroyInterceptor));
    destroySwapchain(device, swapchain, nullptr);
    capturedSwapchain = {};
    if (!expect(g_swapchainDestroyInterceptorCalls == 1u && g_destroySawInterceptor,
                "swapchain teardown interceptor did not run before real WSI destruction") ||
        !expect(g_destroySwapchainCalls == 1u && !getSwapchainInfo(swapchain, &capturedSwapchain),
                "destroyed swapchain remained visible to host capture") ||
        !expect(g_virtualImageDestroys == g_virtualImageCreates,
                "virtual swapchain images leaked across swapchain teardown") ||
        !expect(g_virtualQueueWaitIdles >= 1u,
                "virtual swapchain teardown did not retire private queue work before resource destruction")) {
        return 1;
    }

    setSwapchainDestroyInterceptor(nullptr);
    setQueuePresentInterceptor(nullptr);
    requestAntiLag(false);
    requestFrameGeneration(false);
    VkPhysicalDeviceVulkan12FeaturesPrefix fsrOnlyVulkan12{};
    VkPhysicalDeviceVulkan13FeaturesPrefix fsrOnlyVulkan13{};
    VkDeviceQueueCreateInfoLoose fsrOnlyQueueInfo{};
    VkDeviceCreateInfoLoose fsrOnlyCreateInfo =
        makeCreateInfo(fsrOnlyVulkan12, fsrOnlyVulkan13, fsrOnlyQueueInfo);
    VkDevice fsrOnlyDevice = nullptr;
    if (!expect(createDevice(g_physicalDevice, &fsrOnlyCreateInfo, nullptr, &fsrOnlyDevice) == VkSuccess &&
                fsrOnlyDevice == g_device,
                "FSR4-only vkCreateDevice path failed")) {
        return 1;
    }
    ApusrVulkanHostDeviceInfo fsrOnlyCapturedDevice{};
    if (!expect(g_lastCreateHadSubgroupSizeControl && fsrOnlyVulkan13.subgroupSizeControl == 0u &&
                !g_lastCreateHadTimelineSemaphore &&
                getDeviceInfo(fsrOnlyDevice, &fsrOnlyCapturedDevice) &&
                (fsrOnlyCapturedDevice.negotiationFlags & APUSR_VK_HOST_FSR4_SUBGROUP32) != 0u &&
                (fsrOnlyCapturedDevice.negotiationFlags & APUSR_VK_HOST_FSR4_SUBGROUP64) != 0u &&
                (fsrOnlyCapturedDevice.negotiationFlags & APUSR_VK_HOST_FRAMEGEN_SUBGROUP64) == 0u,
                "FSR4-only device creation did not negotiate and restore subgroup-size control")) {
        return 1;
    }
    std::cout << "APUSR Vulkan host/present contract: async private queues, virtual swapchain lifetime, direct real-WSI output, relative WSI timing, Anti-Lag pairing, and teardown validated\n";
    return 0;
}

#undef APUSR_TEST_VK_CALL
