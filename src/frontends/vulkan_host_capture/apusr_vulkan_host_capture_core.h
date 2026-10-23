// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_vulkan_host_capture_abi.h"

#include <cstddef>
#include <cstdint>

namespace apusr::vulkan_host_capture {

#if defined(_WIN32)
#define APUSR_VK_CALL __stdcall
#else
#define APUSR_VK_CALL
#endif

using VkBool32 = std::uint32_t;
using VkResult = std::int32_t;
using VkInstance = void*;
using VkPhysicalDevice = void*;
using VkDevice = void*;
using VkQueue = void*;
using VkSurfaceKHR = void*;
using VkSwapchainKHR = void*;
using VkSemaphore = void*;
using VkFence = void*;
using VkImage = void*;
using VkAllocationCallbacks = void;
using VkVoidFunction = void(APUSR_VK_CALL*)();

inline constexpr VkResult VkSuccess = 0;
inline constexpr VkResult VkNotReady = 1;
inline constexpr VkResult VkTimeout = 2;
inline constexpr VkResult VkIncomplete = 5;
inline constexpr VkResult VkErrorInitializationFailed = -3;
inline constexpr VkResult VkErrorExtensionNotPresent = -7;
inline constexpr VkResult VkErrorFeatureNotPresent = -8;
inline constexpr VkResult VkErrorIncompatibleDriver = -9;
inline constexpr VkResult VkSuboptimalKhr = 1000001003;

inline constexpr std::int32_t VkStructureTypeApplicationInfo = 0;
inline constexpr std::int32_t VkStructureTypeInstanceCreateInfo = 1;
inline constexpr std::int32_t VkStructureTypeDeviceQueueCreateInfo = 2;
inline constexpr std::int32_t VkStructureTypeDeviceCreateInfo = 3;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceFeatures2 = 1000059000;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceProperties2 = 1000059001;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceSubgroupProperties = 1000094000;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceSubgroupSizeControlProperties = 1000225000;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceSubgroupSizeControlFeatures = 1000225002;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceGroupProperties = 1000070000;
inline constexpr std::int32_t VkStructureTypeDeviceGroupDeviceCreateInfo = 1000070001;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceVulkan12Features = 51;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceVulkan13Features = 53;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceShaderFloat16Int8Features = 1000082000;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceComputeShaderDerivativesFeaturesKhr = 1000201000;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceShaderIntegerDotProductFeatures = 1000280000;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceShaderIntegerDotProductProperties = 1000280001;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceTimelineSemaphoreFeatures = 1000207000;
inline constexpr std::int32_t VkStructureTypeDeviceQueueInfo2 = 1000145003;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceAntiLagFeaturesAmd = 1000476000;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceSwapchainMaintenance1FeaturesKhr = 1000275000;
inline constexpr std::int32_t VkStructureTypeAntiLagDataAmd = 1000476001;
inline constexpr std::int32_t VkStructureTypeAntiLagPresentationInfoAmd = 1000476002;
inline constexpr std::int32_t VkStructureTypeSwapchainCreateInfoKhr = 1000001000;
inline constexpr std::int32_t VkStructureTypePresentInfoKhr = 1000001001;
inline constexpr std::int32_t VkStructureTypeAcquireNextImageInfoKhr = 1000060010;
inline constexpr std::int32_t VkStructureTypeReleaseSwapchainImagesInfoKhr = 1000275005;
inline constexpr std::int32_t VkStructureTypePhysicalDeviceSurfaceInfo2Khr = 1000119000;
inline constexpr std::int32_t VkStructureTypeSurfaceCapabilities2Khr = 1000119001;
inline constexpr std::int32_t VkStructureTypePhysicalDevicePresentTimingFeaturesExt = 1000208000;
inline constexpr std::int32_t VkStructureTypeSwapchainTimeDomainPropertiesExt = 1000208002;
inline constexpr std::int32_t VkStructureTypePresentTimingsInfoExt = 1000208003;
inline constexpr std::int32_t VkStructureTypePresentTimingInfoExt = 1000208004;
inline constexpr std::int32_t VkStructureTypePresentTimingSurfaceCapabilitiesExt = 1000208008;
inline constexpr std::int32_t VkStructureTypePresentTimesInfoGoogle = 1000092000;

inline constexpr std::uint32_t VkQueueGraphicsBit = 0x00000001u;
inline constexpr std::uint32_t VkQueueComputeBit = 0x00000002u;
inline constexpr std::uint32_t VkQueueTransferBit = 0x00000004u;
inline constexpr std::uint32_t VkQueueOpticalFlowBitNv = 0x00000100u;
inline constexpr std::uint32_t VkShaderStageComputeBit = 0x00000020u;
inline constexpr std::uint32_t VkSubgroupFeatureQuadBit = 0x00000080u;
inline constexpr std::uint32_t VkMaxDeviceGroupSize = 32u;
inline constexpr std::uint32_t VkImageUsageTransferSrcBit = 0x00000001u;
inline constexpr std::uint32_t VkImageUsageTransferDstBit = 0x00000002u;
inline constexpr std::uint32_t VkImageUsageSampledBit = 0x00000004u;
inline constexpr std::uint32_t VkImageUsageStorageBit = 0x00000008u;
inline constexpr std::uint32_t VkSwapchainCreatePresentTimingBitExt = 0x00000200u;
inline constexpr std::uint32_t VkPresentModeFifoKhr = 2u;
inline constexpr std::uint32_t VkPresentModeFifoRelaxedKhr = 3u;
inline constexpr std::uint32_t VkPresentModeFifoLatestReadyKhr = 1000361000u;
inline constexpr std::uint32_t VkPresentTimingInfoPresentAtRelativeTimeBitExt = 0x00000001u;
inline constexpr std::uint32_t VkPresentStageImageFirstPixelVisibleBitExt = 0x00000008u;
inline constexpr std::int32_t VkTimeDomainPresentStageLocalExt = 1000208000;
inline constexpr std::int32_t VkTimeDomainSwapchainLocalExt = 1000208001;

struct VkBaseInStructureLoose {
    std::int32_t sType = 0;
    const VkBaseInStructureLoose* pNext = nullptr;
};

struct VkApplicationInfoLoose {
    std::int32_t sType = VkStructureTypeApplicationInfo;
    const void* pNext = nullptr;
    const char* pApplicationName = nullptr;
    std::uint32_t applicationVersion = 0;
    const char* pEngineName = nullptr;
    std::uint32_t engineVersion = 0;
    std::uint32_t apiVersion = 0;
};

struct VkInstanceCreateInfoLoose {
    std::int32_t sType = VkStructureTypeInstanceCreateInfo;
    const void* pNext = nullptr;
    std::uint32_t flags = 0;
    const VkApplicationInfoLoose* pApplicationInfo = nullptr;
    std::uint32_t enabledLayerCount = 0;
    const char* const* ppEnabledLayerNames = nullptr;
    std::uint32_t enabledExtensionCount = 0;
    const char* const* ppEnabledExtensionNames = nullptr;
};

struct VkPhysicalDeviceFeaturesLoose {
    VkBool32 values[55]{};
};

struct VkPhysicalDeviceFeatures2Loose {
    std::int32_t sType = VkStructureTypePhysicalDeviceFeatures2;
    void* pNext = nullptr;
    VkPhysicalDeviceFeaturesLoose features{};
};

struct VkPhysicalDeviceShaderFloat16Int8FeaturesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceShaderFloat16Int8Features;
    void* pNext = nullptr;
    VkBool32 shaderFloat16 = 0;
    VkBool32 shaderInt8 = 0;
};

struct VkPhysicalDeviceShaderIntegerDotProductFeaturesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceShaderIntegerDotProductFeatures;
    void* pNext = nullptr;
    VkBool32 shaderIntegerDotProduct = 0;
};

struct VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceComputeShaderDerivativesFeaturesKhr;
    void* pNext = nullptr;
    VkBool32 computeDerivativeGroupQuads = 0;
    VkBool32 computeDerivativeGroupLinear = 0;
};

struct VkPhysicalDeviceAntiLagFeaturesAMDLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceAntiLagFeaturesAmd;
    void* pNext = nullptr;
    VkBool32 antiLag = 0;
};

struct VkPhysicalDeviceSubgroupSizeControlFeaturesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceSubgroupSizeControlFeatures;
    void* pNext = nullptr;
    VkBool32 subgroupSizeControl = 0;
    VkBool32 computeFullSubgroups = 0;
};

struct VkPhysicalDeviceTimelineSemaphoreFeaturesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceTimelineSemaphoreFeatures;
    void* pNext = nullptr;
    VkBool32 timelineSemaphore = 0;
};

struct VkPhysicalDeviceSwapchainMaintenance1FeaturesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceSwapchainMaintenance1FeaturesKhr;
    void* pNext = nullptr;
    VkBool32 swapchainMaintenance1 = 0;
};

struct VkPhysicalDevicePresentTimingFeaturesEXTLoose {
    std::int32_t sType = VkStructureTypePhysicalDevicePresentTimingFeaturesExt;
    void* pNext = nullptr;
    VkBool32 presentTiming = 0;
    VkBool32 presentAtAbsoluteTime = 0;
    VkBool32 presentAtRelativeTime = 0;
};

struct VkDeviceGroupDeviceCreateInfoLoose {
    std::int32_t sType = VkStructureTypeDeviceGroupDeviceCreateInfo;
    const void* pNext = nullptr;
    std::uint32_t physicalDeviceCount = 0;
    const VkPhysicalDevice* pPhysicalDevices = nullptr;
};

struct VkAntiLagPresentationInfoAMDLoose {
    std::int32_t sType = VkStructureTypeAntiLagPresentationInfoAmd;
    const void* pNext = nullptr;
    std::int32_t stage = 0;
    std::uint64_t frameIndex = 0;
};

struct VkAntiLagDataAMDLoose {
    std::int32_t sType = VkStructureTypeAntiLagDataAmd;
    const void* pNext = nullptr;
    std::int32_t mode = 0;
    std::uint32_t maxFPS = 0;
    const VkAntiLagPresentationInfoAMDLoose* pPresentationInfo = nullptr;
};

// Prefixes only through fields APUSR modifies. Vulkan consumes the host's full
// structures; APUSR temporarily changes these stable leading members and restores
// them before returning from vkCreateDevice.
struct VkPhysicalDeviceVulkan12FeaturesPrefix {
    std::int32_t sType = VkStructureTypePhysicalDeviceVulkan12Features;
    void* pNext = nullptr;
    VkBool32 samplerMirrorClampToEdge = 0;
    VkBool32 drawIndirectCount = 0;
    VkBool32 storageBuffer8BitAccess = 0;
    VkBool32 uniformAndStorageBuffer8BitAccess = 0;
    VkBool32 storagePushConstant8 = 0;
    VkBool32 shaderBufferInt64Atomics = 0;
    VkBool32 shaderSharedInt64Atomics = 0;
    VkBool32 shaderFloat16 = 0;
    VkBool32 shaderInt8 = 0;
    // Vulkan 1.2 promoted features between shaderInt8 and timelineSemaphore.
    // APUSR does not interpret them, but preserves their ABI positions so it
    // can temporarily enable timelineSemaphore in a caller-owned full struct.
    VkBool32 featuresBeforeTimelineSemaphore[28]{};
    VkBool32 timelineSemaphore = 0;
};

struct VkPhysicalDeviceVulkan13FeaturesPrefix {
    std::int32_t sType = VkStructureTypePhysicalDeviceVulkan13Features;
    void* pNext = nullptr;
    VkBool32 robustImageAccess = 0;
    VkBool32 inlineUniformBlock = 0;
    VkBool32 descriptorBindingInlineUniformBlockUpdateAfterBind = 0;
    VkBool32 pipelineCreationCacheControl = 0;
    VkBool32 privateData = 0;
    VkBool32 shaderDemoteToHelperInvocation = 0;
    VkBool32 shaderTerminateInvocation = 0;
    VkBool32 subgroupSizeControl = 0;
    VkBool32 computeFullSubgroups = 0;
    VkBool32 synchronization2 = 0;
    VkBool32 textureCompressionASTC_HDR = 0;
    VkBool32 shaderZeroInitializeWorkgroupMemory = 0;
    VkBool32 dynamicRendering = 0;
    VkBool32 shaderIntegerDotProduct = 0;
};

struct VkDeviceQueueCreateInfoLoose {
    std::int32_t sType = VkStructureTypeDeviceQueueCreateInfo;
    const void* pNext = nullptr;
    std::uint32_t flags = 0;
    std::uint32_t queueFamilyIndex = 0;
    std::uint32_t queueCount = 0;
    const float* pQueuePriorities = nullptr;
};

struct VkDeviceCreateInfoLoose {
    std::int32_t sType = VkStructureTypeDeviceCreateInfo;
    const void* pNext = nullptr;
    std::uint32_t flags = 0;
    std::uint32_t queueCreateInfoCount = 0;
    const VkDeviceQueueCreateInfoLoose* pQueueCreateInfos = nullptr;
    std::uint32_t enabledLayerCount = 0;
    const char* const* ppEnabledLayerNames = nullptr;
    std::uint32_t enabledExtensionCount = 0;
    const char* const* ppEnabledExtensionNames = nullptr;
    const VkPhysicalDeviceFeaturesLoose* pEnabledFeatures = nullptr;
};

struct VkExtensionPropertiesLoose {
    char extensionName[256]{};
    std::uint32_t specVersion = 0;
};

struct VkPhysicalDevicePropertiesLoose {
    std::uint32_t apiVersion = 0;
    std::uint32_t driverVersion = 0;
    std::uint32_t vendorID = 0;
    std::uint32_t deviceID = 0;
    std::uint32_t deviceType = 0;
    char deviceName[256]{};
    std::uint8_t pipelineCacheUUID[16]{};
    std::byte rest[1756]{};
};
static_assert(sizeof(VkPhysicalDevicePropertiesLoose) == 2048);
static_assert(offsetof(VkPhysicalDevicePropertiesLoose, vendorID) == 8);
static_assert(offsetof(VkPhysicalDevicePropertiesLoose, deviceID) == 12);
static_assert(offsetof(VkPhysicalDevicePropertiesLoose, deviceName) == 20);

struct VkExtent3DLoose {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
};

struct VkQueueFamilyPropertiesLoose {
    std::uint32_t queueFlags = 0;
    std::uint32_t queueCount = 0;
    std::uint32_t timestampValidBits = 0;
    VkExtent3DLoose minImageTransferGranularity{};
};

struct VkPhysicalDeviceSubgroupPropertiesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceSubgroupProperties;
    void* pNext = nullptr;
    std::uint32_t subgroupSize = 0;
    std::uint32_t supportedStages = 0;
    std::uint32_t supportedOperations = 0;
    std::uint32_t quadOperationsInAllStages = 0;
};

struct VkPhysicalDeviceSubgroupSizeControlPropertiesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceSubgroupSizeControlProperties;
    void* pNext = nullptr;
    std::uint32_t minSubgroupSize = 0;
    std::uint32_t maxSubgroupSize = 0;
    std::uint32_t maxComputeWorkgroupSubgroups = 0;
    std::uint32_t requiredSubgroupSizeStages = 0;
};

struct VkPhysicalDeviceShaderIntegerDotProductPropertiesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceShaderIntegerDotProductProperties;
    void* pNext = nullptr;
    VkBool32 integerDotProduct8BitUnsignedAccelerated = 0;
    VkBool32 integerDotProduct8BitSignedAccelerated = 0;
    VkBool32 integerDotProduct8BitMixedSignednessAccelerated = 0;
    VkBool32 integerDotProduct4x8BitPackedUnsignedAccelerated = 0;
    VkBool32 integerDotProduct4x8BitPackedSignedAccelerated = 0;
    VkBool32 integerDotProduct4x8BitPackedMixedSignednessAccelerated = 0;
    // Preserve the promoted Vulkan ABI through the one additional property
    // consumed by APUSR's bounded signed-DOT accumulate lowering.
    VkBool32 propertiesBeforePackedSignedAccSat[13]{};
    VkBool32 integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated = 0;
    VkBool32 propertiesAfterPackedSignedAccSat[10]{};
};

struct VkPhysicalDeviceProperties2Loose {
    std::int32_t sType = VkStructureTypePhysicalDeviceProperties2;
    void* pNext = nullptr;
    VkPhysicalDevicePropertiesLoose properties{};
};

struct VkExtent2DLoose {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};


struct VkSurfaceCapabilitiesKHRLoose {
    std::uint32_t minImageCount = 0;
    std::uint32_t maxImageCount = 0;
    VkExtent2DLoose currentExtent{};
    VkExtent2DLoose minImageExtent{};
    VkExtent2DLoose maxImageExtent{};
    std::uint32_t maxImageArrayLayers = 0;
    std::uint32_t supportedTransforms = 0;
    std::uint32_t currentTransform = 0;
    std::uint32_t supportedCompositeAlpha = 0;
    std::uint32_t supportedUsageFlags = 0;
};

struct VkPhysicalDeviceSurfaceInfo2KHRLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceSurfaceInfo2Khr;
    const void* pNext = nullptr;
    VkSurfaceKHR surface = nullptr;
};

struct VkSurfaceCapabilities2KHRLoose {
    std::int32_t sType = VkStructureTypeSurfaceCapabilities2Khr;
    void* pNext = nullptr;
    VkSurfaceCapabilitiesKHRLoose surfaceCapabilities{};
};

struct VkPresentTimingSurfaceCapabilitiesEXTLoose {
    std::int32_t sType = VkStructureTypePresentTimingSurfaceCapabilitiesExt;
    void* pNext = nullptr;
    VkBool32 presentTimingSupported = 0;
    VkBool32 presentAtAbsoluteTimeSupported = 0;
    VkBool32 presentAtRelativeTimeSupported = 0;
    std::uint32_t presentStageQueries = 0;
};

struct VkSwapchainTimeDomainPropertiesEXTLoose {
    std::int32_t sType = VkStructureTypeSwapchainTimeDomainPropertiesExt;
    void* pNext = nullptr;
    std::uint32_t timeDomainCount = 0;
    std::int32_t* pTimeDomains = nullptr;
    std::uint64_t* pTimeDomainIds = nullptr;
};

struct VkPresentTimingInfoEXTLoose {
    std::int32_t sType = VkStructureTypePresentTimingInfoExt;
    const void* pNext = nullptr;
    std::uint32_t flags = 0;
    std::uint64_t targetTime = 0;
    std::uint64_t timeDomainId = 0;
    std::uint32_t presentStageQueries = 0;
    std::uint32_t targetTimeDomainPresentStage = 0;
};

struct VkPresentTimingsInfoEXTLoose {
    std::int32_t sType = VkStructureTypePresentTimingsInfoExt;
    const void* pNext = nullptr;
    std::uint32_t swapchainCount = 0;
    const VkPresentTimingInfoEXTLoose* pTimingInfos = nullptr;
};

struct VkSwapchainCreateInfoKHRLoose {
    std::int32_t sType = VkStructureTypeSwapchainCreateInfoKhr;
    const void* pNext = nullptr;
    std::uint32_t flags = 0;
    VkSurfaceKHR surface = nullptr;
    std::uint32_t minImageCount = 0;
    std::uint32_t imageFormat = 0;
    std::uint32_t imageColorSpace = 0;
    VkExtent2DLoose imageExtent{};
    std::uint32_t imageArrayLayers = 0;
    std::uint32_t imageUsage = 0;
    std::uint32_t imageSharingMode = 0;
    std::uint32_t queueFamilyIndexCount = 0;
    const std::uint32_t* pQueueFamilyIndices = nullptr;
    std::uint32_t preTransform = 0;
    std::uint32_t compositeAlpha = 0;
    std::uint32_t presentMode = 0;
    VkBool32 clipped = 0;
    VkSwapchainKHR oldSwapchain = nullptr;
};

struct VkAcquireNextImageInfoKHRLoose {
    std::int32_t sType = VkStructureTypeAcquireNextImageInfoKhr;
    const void* pNext = nullptr;
    VkSwapchainKHR swapchain = nullptr;
    std::uint64_t timeout = 0;
    VkSemaphore semaphore = nullptr;
    VkFence fence = nullptr;
    std::uint32_t deviceMask = 0;
};

struct VkReleaseSwapchainImagesInfoKHRLoose {
    std::int32_t sType = VkStructureTypeReleaseSwapchainImagesInfoKhr;
    const void* pNext = nullptr;
    VkSwapchainKHR swapchain = nullptr;
    std::uint32_t imageIndexCount = 0;
    const std::uint32_t* pImageIndices = nullptr;
};

struct VkPresentInfoKHRLoose {
    std::int32_t sType = VkStructureTypePresentInfoKhr;
    const void* pNext = nullptr;
    std::uint32_t waitSemaphoreCount = 0;
    const VkSemaphore* pWaitSemaphores = nullptr;
    std::uint32_t swapchainCount = 0;
    const VkSwapchainKHR* pSwapchains = nullptr;
    const std::uint32_t* pImageIndices = nullptr;
    VkResult* pResults = nullptr;
};

struct VkPresentTimeGOOGLELoose {
    std::uint32_t presentID = 0u;
    std::uint64_t desiredPresentTime = 0u;
};

struct VkPresentTimesInfoGOOGLELoose {
    std::int32_t sType = VkStructureTypePresentTimesInfoGoogle;
    const void* pNext = nullptr;
    std::uint32_t swapchainCount = 0u;
    const VkPresentTimeGOOGLELoose* pTimes = nullptr;
};

struct VkPastPresentationTimingGOOGLELoose {
    std::uint32_t presentID = 0u;
    std::uint64_t desiredPresentTime = 0u;
    std::uint64_t actualPresentTime = 0u;
    std::uint64_t earliestPresentTime = 0u;
    std::uint64_t presentMargin = 0u;
};

struct VkDeviceQueueInfo2Loose {
    std::int32_t sType = VkStructureTypeDeviceQueueInfo2;
    const void* pNext = nullptr;
    std::uint32_t flags = 0;
    std::uint32_t queueFamilyIndex = 0;
    std::uint32_t queueIndex = 0;
};

struct VkPhysicalDeviceGroupPropertiesLoose {
    std::int32_t sType = VkStructureTypePhysicalDeviceGroupProperties;
    void* pNext = nullptr;
    std::uint32_t physicalDeviceCount = 0;
    VkPhysicalDevice physicalDevices[VkMaxDeviceGroupSize]{};
    VkBool32 subsetAllocation = 0;
};

// This capture boundary mirrors only the Vulkan ABI fields it reads or edits.
// Keep the touched offsets compile-time-checked so a packing or declaration
// regression fails the build instead of corrupting the host's pNext chain.
static_assert(sizeof(void*) == 8, "APUSR Vulkan host capture requires a 64-bit process");
static_assert(sizeof(VkPhysicalDeviceFeaturesLoose) == 220);
static_assert(offsetof(VkPhysicalDeviceVulkan12FeaturesPrefix, shaderFloat16) == 44);
static_assert(offsetof(VkPhysicalDeviceVulkan12FeaturesPrefix, shaderInt8) == 48);
static_assert(offsetof(VkPhysicalDeviceVulkan12FeaturesPrefix, timelineSemaphore) == 164);
static_assert(offsetof(VkPhysicalDeviceVulkan13FeaturesPrefix, subgroupSizeControl) == 44);
static_assert(offsetof(VkPhysicalDeviceVulkan13FeaturesPrefix, computeFullSubgroups) == 48);
static_assert(offsetof(VkPhysicalDeviceVulkan13FeaturesPrefix, shaderIntegerDotProduct) == 68);
static_assert(sizeof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose) == 24);
static_assert(offsetof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose, computeDerivativeGroupLinear) == 20);
static_assert(sizeof(VkPhysicalDeviceSubgroupSizeControlFeaturesLoose) == 24);
static_assert(sizeof(VkPhysicalDeviceSubgroupSizeControlPropertiesLoose) == 32);
static_assert(offsetof(
    VkPhysicalDeviceShaderIntegerDotProductPropertiesLoose,
    integerDotProduct4x8BitPackedSignedAccelerated) == 32);
static_assert(offsetof(
    VkPhysicalDeviceShaderIntegerDotProductPropertiesLoose,
    integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated) == 92);
static_assert(sizeof(VkPhysicalDeviceShaderIntegerDotProductPropertiesLoose) == 136);
static_assert(sizeof(VkDeviceCreateInfoLoose) == 72);
static_assert(offsetof(VkDeviceCreateInfoLoose, pEnabledFeatures) == 64);
static_assert(sizeof(VkPhysicalDeviceGroupPropertiesLoose) == 288);
static_assert(offsetof(VkPhysicalDeviceGroupPropertiesLoose, subsetAllocation) == 280);
static_assert(sizeof(VkPhysicalDeviceAntiLagFeaturesAMDLoose) == 24);
static_assert(sizeof(VkDeviceGroupDeviceCreateInfoLoose) == 32);
static_assert(sizeof(VkAntiLagPresentationInfoAMDLoose) == 32);
static_assert(offsetof(VkAntiLagPresentationInfoAMDLoose, frameIndex) == 24);
static_assert(sizeof(VkAntiLagDataAMDLoose) == 32);
static_assert(offsetof(VkAntiLagDataAMDLoose, pPresentationInfo) == 24);
static_assert(sizeof(VkSurfaceCapabilitiesKHRLoose) == 52);
static_assert(sizeof(VkPhysicalDevicePresentTimingFeaturesEXTLoose) == 32);
static_assert(sizeof(VkPhysicalDeviceSurfaceInfo2KHRLoose) == 24);
static_assert(sizeof(VkSurfaceCapabilities2KHRLoose) == 72);
static_assert(sizeof(VkPresentTimingSurfaceCapabilitiesEXTLoose) == 32);
static_assert(sizeof(VkSwapchainTimeDomainPropertiesEXTLoose) == 40);
static_assert(sizeof(VkPresentTimingInfoEXTLoose) == 48);
static_assert(sizeof(VkPresentTimingsInfoEXTLoose) == 32);
static_assert(sizeof(VkSwapchainCreateInfoKHRLoose) == 104);
static_assert(offsetof(VkSwapchainCreateInfoKHRLoose, surface) == 24);
static_assert(offsetof(VkSwapchainCreateInfoKHRLoose, imageExtent) == 44);
static_assert(offsetof(VkSwapchainCreateInfoKHRLoose, pQueueFamilyIndices) == 72);
static_assert(offsetof(VkSwapchainCreateInfoKHRLoose, oldSwapchain) == 96);
static_assert(sizeof(VkAcquireNextImageInfoKHRLoose) == 56);
static_assert(offsetof(VkAcquireNextImageInfoKHRLoose, swapchain) == 16);
static_assert(offsetof(VkAcquireNextImageInfoKHRLoose, deviceMask) == 48);
static_assert(sizeof(VkPresentInfoKHRLoose) == 64);
static_assert(offsetof(VkPresentInfoKHRLoose, pWaitSemaphores) == 24);
static_assert(offsetof(VkPresentInfoKHRLoose, pSwapchains) == 40);
static_assert(sizeof(VkPresentTimeGOOGLELoose) == 16);
static_assert(offsetof(VkPresentTimeGOOGLELoose, desiredPresentTime) == 8);
static_assert(sizeof(VkPresentTimesInfoGOOGLELoose) == 32);
static_assert(offsetof(VkPresentTimesInfoGOOGLELoose, pTimes) == 24);
static_assert(sizeof(VkPastPresentationTimingGOOGLELoose) == 40);
static_assert(offsetof(VkPastPresentationTimingGOOGLELoose, actualPresentTime) == 16);

using VkCreateInstanceFn = VkResult(APUSR_VK_CALL*)(
    const VkInstanceCreateInfoLoose*, const VkAllocationCallbacks*, VkInstance*);
using VkCreateDeviceFn = VkResult(APUSR_VK_CALL*)(
    VkPhysicalDevice, const VkDeviceCreateInfoLoose*, const VkAllocationCallbacks*, VkDevice*);
using VkDestroyInstanceFn = void(APUSR_VK_CALL*)(VkInstance, const VkAllocationCallbacks*);
using VkDestroyDeviceFn = void(APUSR_VK_CALL*)(VkDevice, const VkAllocationCallbacks*);
using VkEnumeratePhysicalDevicesFn = VkResult(APUSR_VK_CALL*)(VkInstance, std::uint32_t*, VkPhysicalDevice*);
using VkEnumeratePhysicalDeviceGroupsFn = VkResult(APUSR_VK_CALL*)(
    VkInstance, std::uint32_t*, VkPhysicalDeviceGroupPropertiesLoose*);
using VkGetInstanceProcAddrFn = VkVoidFunction(APUSR_VK_CALL*)(VkInstance, const char*);
using VkGetDeviceProcAddrFn = VkVoidFunction(APUSR_VK_CALL*)(VkDevice, const char*);
using VkGetPhysicalDeviceFeatures2Fn = void(APUSR_VK_CALL*)(VkPhysicalDevice, VkPhysicalDeviceFeatures2Loose*);
using VkGetPhysicalDevicePropertiesFn = void(APUSR_VK_CALL*)(VkPhysicalDevice, VkPhysicalDevicePropertiesLoose*);
using VkGetPhysicalDeviceProperties2Fn = void(APUSR_VK_CALL*)(VkPhysicalDevice, VkPhysicalDeviceProperties2Loose*);
using VkEnumerateInstanceExtensionPropertiesFn = VkResult(APUSR_VK_CALL*)(
    const char*, std::uint32_t*, VkExtensionPropertiesLoose*);
using VkEnumerateDeviceExtensionPropertiesFn = VkResult(APUSR_VK_CALL*)(
    VkPhysicalDevice, const char*, std::uint32_t*, VkExtensionPropertiesLoose*);
using VkGetPhysicalDeviceQueueFamilyPropertiesFn = void(APUSR_VK_CALL*)(
    VkPhysicalDevice, std::uint32_t*, VkQueueFamilyPropertiesLoose*);
using VkGetPhysicalDeviceSurfaceCapabilitiesKHRFn = VkResult(APUSR_VK_CALL*)(
    VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHRLoose*);
using VkGetPhysicalDeviceSurfaceCapabilities2KHRFn = VkResult(APUSR_VK_CALL*)(
    VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHRLoose*, VkSurfaceCapabilities2KHRLoose*);
using VkGetDeviceQueueFn = void(APUSR_VK_CALL*)(VkDevice, std::uint32_t, std::uint32_t, VkQueue*);
using VkGetDeviceQueue2Fn = void(APUSR_VK_CALL*)(VkDevice, const VkDeviceQueueInfo2Loose*, VkQueue*);
using VkAntiLagUpdateAMDFn = void(APUSR_VK_CALL*)(VkDevice, const VkAntiLagDataAMDLoose*);
using VkQueuePresentKHRFn = VkResult(APUSR_VK_CALL*)(VkQueue, const void*);
using VkCreateSwapchainKHRFn = VkResult(APUSR_VK_CALL*)(
    VkDevice, const VkSwapchainCreateInfoKHRLoose*, const VkAllocationCallbacks*, VkSwapchainKHR*);
using VkDestroySwapchainKHRFn = void(APUSR_VK_CALL*)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
using VkSwapchainDestroyInterceptorFn = void(APUSR_VK_CALL*)(VkDevice, VkSwapchainKHR);
using VkGetSwapchainImagesKHRFn = VkResult(APUSR_VK_CALL*)(
    VkDevice, VkSwapchainKHR, std::uint32_t*, VkImage*);
using VkAcquireNextImageKHRFn = VkResult(APUSR_VK_CALL*)(
    VkDevice, VkSwapchainKHR, std::uint64_t, VkSemaphore, VkFence, std::uint32_t*);
using VkAcquireNextImage2KHRFn = VkResult(APUSR_VK_CALL*)(
    VkDevice, const VkAcquireNextImageInfoKHRLoose*, std::uint32_t*);
using VkReleaseSwapchainImagesKHRFn = VkResult(APUSR_VK_CALL*)(
    VkDevice, const VkReleaseSwapchainImagesInfoKHRLoose*);
using VkGetSwapchainTimeDomainPropertiesEXTFn = VkResult(APUSR_VK_CALL*)(
    VkDevice, VkSwapchainKHR, VkSwapchainTimeDomainPropertiesEXTLoose*, std::uint64_t*);
using VkGetPastPresentationTimingGOOGLEFn = VkResult(APUSR_VK_CALL*)(
    VkDevice, VkSwapchainKHR, std::uint32_t*, VkPastPresentationTimingGOOGLELoose*);

struct Dispatch {
    VkCreateInstanceFn createInstance = nullptr;
    VkCreateDeviceFn createDevice = nullptr;
    VkDestroyInstanceFn destroyInstance = nullptr;
    VkDestroyDeviceFn destroyDevice = nullptr;
    VkEnumeratePhysicalDevicesFn enumeratePhysicalDevices = nullptr;
    VkEnumeratePhysicalDeviceGroupsFn enumeratePhysicalDeviceGroups = nullptr;
    VkGetInstanceProcAddrFn getInstanceProcAddr = nullptr;
    VkGetDeviceProcAddrFn getDeviceProcAddr = nullptr;
    VkQueuePresentKHRFn queuePresent = nullptr;
    VkGetPhysicalDeviceFeatures2Fn getPhysicalDeviceFeatures2 = nullptr;
    VkGetPhysicalDevicePropertiesFn getPhysicalDeviceProperties = nullptr;
    VkGetPhysicalDeviceProperties2Fn getPhysicalDeviceProperties2 = nullptr;
    VkEnumerateInstanceExtensionPropertiesFn enumerateInstanceExtensionProperties = nullptr;
    VkEnumerateDeviceExtensionPropertiesFn enumerateDeviceExtensionProperties = nullptr;
    VkGetPhysicalDeviceQueueFamilyPropertiesFn getPhysicalDeviceQueueFamilyProperties = nullptr;
    VkGetPhysicalDeviceSurfaceCapabilitiesKHRFn getPhysicalDeviceSurfaceCapabilities = nullptr;
    VkGetPhysicalDeviceSurfaceCapabilities2KHRFn getPhysicalDeviceSurfaceCapabilities2 = nullptr;
};

void configureDispatch(const Dispatch& dispatch);
[[nodiscard]] bool dispatchReady();

VkResult createInstance(
    const VkInstanceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance);
VkResult enumeratePhysicalDevices(VkInstance instance, std::uint32_t* count, VkPhysicalDevice* devices);
VkResult enumeratePhysicalDeviceGroups(
    VkInstance instance,
    std::uint32_t* count,
    VkPhysicalDeviceGroupPropertiesLoose* groups);
VkResult createDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDevice* device);
void destroyDevice(VkDevice device, const VkAllocationCallbacks* allocator);
void destroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator);

[[nodiscard]] std::uint32_t deviceNegotiationFlags(VkDevice device);
[[nodiscard]] bool getDeviceInfo(VkDevice device, ApusrVulkanHostDeviceInfo* outInfo);
[[nodiscard]] bool getQueueInfo(
    VkDevice device,
    std::uint32_t queueOrdinal,
    ApusrVulkanHostQueueInfo* outInfo);
[[nodiscard]] bool resolveQueue(
    VkDevice device,
    std::uint32_t preferredKind,
    ApusrVulkanHostQueueInfo* outInfo);
void associateInstance(VkDevice device, VkInstance instance, VkPhysicalDevice physicalDevice);
void requestAntiLag(bool requested);
void requestFrameGeneration(bool requested);
void setFrameGenerationActive(bool active);
void requestLegacyOpticalFlowQueueRole(bool requested);
void getPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice,
    std::uint32_t* count,
    VkQueueFamilyPropertiesLoose* properties);
[[nodiscard]] bool antiLagSupported(VkPhysicalDevice physicalDevice);
[[nodiscard]] bool antiLagAvailable(VkDevice device);
[[nodiscard]] bool antiLagInput(
    VkDevice device,
    std::uint64_t frameIndex,
    std::uint32_t mode,
    std::uint32_t maxFps);
[[nodiscard]] bool antiLagArmPairedDisable(VkDevice device);
void antiLagSetPresentFrame(VkDevice device, std::uint64_t frameIndex);
VkResult createSwapchain(
    VkDevice device,
    const VkSwapchainCreateInfoKHRLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSwapchainKHR* swapchain);
void destroySwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* allocator);
VkResult getSwapchainImages(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t* count, VkImage* images);
VkResult acquireNextImage(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t* imageIndex);
VkResult acquireNextImage2(
    VkDevice device, const VkAcquireNextImageInfoKHRLoose* acquireInfo, std::uint32_t* imageIndex);
[[nodiscard]] bool getSwapchainInfo(VkSwapchainKHR swapchain, ApusrVulkanHostSwapchainInfo* outInfo);
[[nodiscard]] bool getSwapchainImage(VkSwapchainKHR swapchain, std::uint32_t imageIndex, void** outImage);
[[nodiscard]] bool getRealSwapchainImage(VkSwapchainKHR swapchain, std::uint32_t imageIndex, void** outImage);
[[nodiscard]] bool releaseVirtualSwapchainImage(
    VkSwapchainKHR swapchain,
    std::uint32_t imageIndex,
    VkSemaphore firstReleaseSemaphore,
    std::uint64_t firstReleaseValue,
    VkSemaphore secondReleaseSemaphore,
    std::uint64_t secondReleaseValue);
[[nodiscard]] bool waitVirtualSwapchainReleaseDependencies(VkSwapchainKHR swapchain);
[[nodiscard]] bool virtualSwapchainActive() noexcept;
[[nodiscard]] bool isVirtualSwapchainImage(VkImage image) noexcept;
VkResult acquireNextImageForFrameGeneration(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, std::uint32_t* imageIndex);
[[nodiscard]] bool releaseRealSwapchainImage(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t imageIndex);
VkResult queuePresentDirect(VkQueue queue, const void* presentInfo);
VkResult queuePresentSource(VkQueue queue, const void* presentInfo);
VkResult queuePresentSourceRelative(
    VkQueue queue, const void* presentInfo, std::uint64_t minimumPreviousImageVisibleNs, bool* outScheduled);
VkResult queuePresentGeneratedRelative(
    VkQueue queue, const void* presentInfo, std::uint64_t minimumPreviousImageVisibleNs, bool* outScheduled);
VkResult queuePresentFrameGeneration(
    VkQueue queue, const void* presentInfo, std::uint64_t frameId, std::uint32_t flags);
VkResult queuePresentSourceRelativeFrameGeneration(
    VkQueue queue, const void* presentInfo, std::uint64_t frameId, std::uint32_t flags,
    std::uint64_t minimumPreviousImageVisibleNs, bool* outScheduled);
VkResult queuePresentGeneratedRelativeFrameGeneration(
    VkQueue queue, const void* presentInfo, std::uint64_t frameId, std::uint32_t flags,
    std::uint64_t minimumPreviousImageVisibleNs, bool* outScheduled);
void setQueuePresentInterceptor(void* queuePresentProc);
void setSwapchainDestroyInterceptor(void* destroySwapchainProc);
VkResult queuePresent(VkQueue queue, const void* presentInfo);

#undef APUSR_VK_CALL

} // namespace apusr::vulkan_host_capture
