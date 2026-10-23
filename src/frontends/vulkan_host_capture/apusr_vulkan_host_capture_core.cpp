// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_vulkan_host_capture_core.h"

#include "../common/apusr_host_discovery_log.h"
#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"
#include <apusr/apusr_profiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace apusr::vulkan_host_capture {
namespace {

namespace vk = apusr::backends::vk;

constexpr std::uint32_t kVkShaderStorageImageExtendedFormatsFeatureIndex = 29u;
constexpr std::uint32_t kVkShaderStorageImageWriteWithoutFormatFeatureIndex = 32u;
constexpr std::uint32_t kVkShaderInt16FeatureIndex = 41u;
constexpr char kVkKhrShaderIntegerDotProductExtension[] = "VK_KHR_shader_integer_dot_product";
#ifndef APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT
#define APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT 1
#endif
constexpr bool kFsr4RequiresPackedSignedAccSat =
    APUSR_VULKAN_FSR4_REQUIRES_PACKED_SIGNED_ACC_SAT != 0;
constexpr char kVkKhrComputeShaderDerivativesExtension[] = "VK_KHR_compute_shader_derivatives";
constexpr char kVkExtSubgroupSizeControlExtension[] = "VK_EXT_subgroup_size_control";
constexpr char kVkAmdAntiLagExtension[] = "VK_AMD_anti_lag";
constexpr char kVkKhrSwapchainMaintenance1Extension[] = "VK_KHR_swapchain_maintenance1";
constexpr char kVkExtSwapchainMaintenance1Extension[] = "VK_EXT_swapchain_maintenance1";
constexpr char kVkKhrGetSurfaceCapabilities2Extension[] = "VK_KHR_get_surface_capabilities2";
constexpr char kVkKhrGetPhysicalDeviceProperties2Extension[] = "VK_KHR_get_physical_device_properties2";
constexpr char kVkKhrSurfaceExtension[] = "VK_KHR_surface";
constexpr char kVkKhrWin32SurfaceExtension[] = "VK_KHR_win32_surface";
constexpr std::uint32_t kVkApiVersion11 = 0x00401000u;
constexpr std::uint32_t kVkApiVersion13 = 0x00403000u;
constexpr char kVkKhrSurfaceMaintenance1Extension[] = "VK_KHR_surface_maintenance1";
constexpr char kVkExtSurfaceMaintenance1Extension[] = "VK_EXT_surface_maintenance1";
constexpr char kVkExtPresentTimingExtension[] = "VK_EXT_present_timing";
constexpr char kVkKhrPresentId2Extension[] = "VK_KHR_present_id2";
constexpr char kVkKhrCalibratedTimestampsExtension[] = "VK_KHR_calibrated_timestamps";
constexpr char kVkExtCalibratedTimestampsExtension[] = "VK_EXT_calibrated_timestamps";
constexpr char kVkExtMemoryBudgetExtension[] = "VK_EXT_memory_budget";
constexpr char kVkGoogleDisplayTimingExtension[] = "VK_GOOGLE_display_timing";
constexpr std::int32_t kVkAntiLagStageInputAmd = 0;
constexpr std::int32_t kVkAntiLagStagePresentAmd = 1;
constexpr std::size_t kAntiLagFrameHistory = 16;

struct InstanceRecord {
    VkInstance instance = nullptr;
    std::uint32_t requestedApiVersion = 0;
    bool getPhysicalDeviceProperties2Enabled = false;
    bool surfaceCapabilities2Enabled = false;
    bool surfaceMaintenance1KhrEnabled = false;
    bool surfaceMaintenance1ExtEnabled = false;
};

struct QueueRecord {
    VkQueue queue = nullptr;
    std::uint32_t familyIndex = 0;
    std::uint32_t queueIndex = 0;
    std::uint32_t familyFlags = 0;
    std::uint32_t queueCreateFlags = 0;
    std::uint32_t kind = APUSR_VK_HOST_QUEUE_UNKNOWN;
    std::uint32_t ownershipFlags = 0;
};

struct ReservedQueue {
    std::uint32_t familyIndex = 0u;
    std::uint32_t queueIndex = 0u;
    std::uint32_t role = 0u;
};

struct AntiLagFrameRecord {
    std::uint64_t frameIndex = 0;
    std::uint32_t mode = APUSR_VK_HOST_ANTI_LAG_OFF;
    std::uint32_t maxFps = 0;
    bool valid = false;
    bool controlTransition = false;
};

struct SwapchainRecord {
    VkDevice device = nullptr;
    VkSwapchainKHR swapchain = nullptr;
    VkSurfaceKHR surface = nullptr;
    std::uint32_t minImageCount = 0;
    std::uint32_t imageFormat = 0;
    std::uint32_t imageColorSpace = 0;
    VkExtent2DLoose imageExtent{};
    std::uint32_t imageArrayLayers = 0;
    std::uint32_t imageUsage = 0;
    std::uint32_t realImageUsage = 0;
    std::uint32_t originalImageUsage = 0;
    std::uint32_t supportedImageUsage = 0;
    std::uint32_t framegenFlags = 0;
    std::uint32_t imageSharingMode = 0;
    std::uint32_t presentMode = 0;
    std::uint32_t latestAcquiredImageIndex = APUSR_VK_HOST_INVALID_IMAGE_INDEX;
    std::uint32_t latestPresentedImageIndex = APUSR_VK_HOST_INVALID_IMAGE_INDEX;
    std::uint64_t acquireSerial = 0;
    std::uint64_t presentSerial = 0;
    std::uint32_t imageCount = 0;
    std::uint32_t realImageCount = 0;
    // All APUSR-owned access to the private real WSI swapchain is externally
    // synchronized through this mutex.  Source prearm may acquire on the
    // application thread while the presentation worker presents/releases on a
    // different thread, and Vulkan makes host access to VkSwapchainKHR
    // externally synchronized for those operations.
    std::shared_ptr<std::mutex> realWsiHostMutex = std::make_shared<std::mutex>();
    // Relative present timing is a presentation-engine scheduling capability,
    // not GPU completion synchronization.  Some implementations expose the
    // time-domain IDs only after presentation has started, so capability and
    // resolved-domain state are deliberately distinct.
    bool presentTimingRelativeCapable = false;
    bool presentTimingRelative = false;
    std::int32_t presentTimingTimeDomain = 0;
    std::uint64_t presentTimingTimeDomainId = 0u;
    bool virtualized = false;
    std::uint32_t nextVirtualAcquire = 0u;
    apusr::profile::StreamHandle profileStream{};
    apusr::profile::SampleGate profileAcquireGate{};
    apusr::profile::SampleGate profilePresentGate{};
    std::uint64_t profileConfigurationHash = 0u;
    std::uint64_t profileLastAcquireTick = 0u;
    std::uint64_t profileLastPresentTick = 0u;
    std::uint64_t profileQueueSequence = 0u;
    std::vector<VkImage> images;
    std::vector<VkImage> realImages;
    std::vector<vk::DeviceMemory> virtualImageMemory;
    vk::CommandPool virtualInitCommandPool = nullptr;
    struct VirtualSlot {
        bool acquired = false;
        std::array<VkSemaphore, 2u> releaseSemaphores{};
        std::array<std::uint64_t, 2u> releaseValues{};
    };
    std::vector<VirtualSlot> virtualSlots;
};

struct DeviceRecord {
    VkInstance instance = nullptr;
    VkPhysicalDevice physicalDevice = nullptr;
    VkDevice device = nullptr;
    std::uint32_t negotiationFlags = 0;
    VkAntiLagUpdateAMDFn antiLagUpdate = nullptr;
    VkQueuePresentKHRFn queuePresent = nullptr;
    VkCreateSwapchainKHRFn createSwapchain = nullptr;
    VkDestroySwapchainKHRFn destroySwapchain = nullptr;
    VkGetSwapchainImagesKHRFn getSwapchainImages = nullptr;
    VkAcquireNextImageKHRFn acquireNextImage = nullptr;
    VkAcquireNextImage2KHRFn acquireNextImage2 = nullptr;
    VkReleaseSwapchainImagesKHRFn releaseSwapchainImages = nullptr;
    VkGetSwapchainTimeDomainPropertiesEXTFn getSwapchainTimeDomainProperties = nullptr;
    bool swapchainMaintenance1Enabled = false;
    bool presentTimingRelativeEnabled = false;
    vk::PfnQueueSubmit queueSubmit = nullptr;
    vk::PfnGetPhysicalDeviceMemoryProperties2 getPhysicalDeviceMemoryProperties2 = nullptr;
    bool memoryBudgetEnabled = false;
    VkGetPastPresentationTimingGOOGLEFn getPastPresentationTiming = nullptr;
    bool displayTimingProfilerOwned = false;
    std::array<AntiLagFrameRecord, kAntiLagFrameHistory> antiLagFrames{};
    std::array<std::uint64_t, kAntiLagFrameHistory> antiLagInputOrder{};
    std::size_t antiLagInputHead = 0;
    std::size_t antiLagInputCount = 0;
    std::array<std::uint64_t, kAntiLagFrameHistory> pendingPresentFrames{};
    std::size_t pendingPresentHead = 0;
    std::size_t pendingPresentCount = 0;
    // Native Anti-Lag state is device-scoped and survives swapchain recreation.
    // Keep an explicit paired OFF transition pending until a real physical
    // present can carry its matching PRESENT marker. Never use a mode-only OFF.
    std::uint32_t lastAntiLagInputMode = APUSR_VK_HOST_ANTI_LAG_OFF;
    bool antiLagDisablePairPending = false;
    std::uint64_t nextAntiLagControlFrameIndex = 0x8000000000000000ull;
    // Keep Anti-Lag telemetry on a device stream rather than a swapchain
    // stream: INPUT happens before any present, and the same device-level
    // pacing state survives swapchain recreation. This is diagnostic only and
    // does not alter the native VK_AMD_anti_lag contract.
    apusr::profile::StreamHandle antiLagProfileStream{};
    apusr::profile::SampleGate antiLagInputProfileGate{};
    apusr::profile::SampleGate antiLagPresentProfileGate{};
    std::vector<QueueRecord> queues;
};

struct State {
    std::mutex mutex;
    Dispatch dispatch{};
    bool dispatchConfigured = false;
    bool antiLagRequested = false;
    bool frameGenerationRequested = false;
    bool legacyOpticalFlowQueueRoleRequested = false;
    std::unordered_map<VkInstance, InstanceRecord> instances;
    std::unordered_map<VkPhysicalDevice, VkInstance> instanceByPhysicalDevice;
    std::unordered_map<VkDevice, DeviceRecord> devices;
    std::unordered_map<VkQueue, VkDevice> deviceByQueue;
    std::unordered_map<VkSwapchainKHR, SwapchainRecord> swapchains;
};

State& state()
{
    static State value;
    return value;
}

std::atomic<VkQueuePresentKHRFn> g_queuePresentInterceptor{nullptr};
std::atomic<VkSwapchainDestroyInterceptorFn> g_swapchainDestroyInterceptor{nullptr};
std::atomic<bool> g_frameGenerationActive{false};
std::atomic<std::uint32_t> g_slowApplicationAcquireLogs{0u};
// The overwhelmingly common case is one presentation queue. Keep that path
// lock-free; uncommon queue changes fall back to the authoritative map.
std::atomic<VkQueue> g_fastCapturedPresentQueue{nullptr};
std::atomic<std::uint32_t> g_virtualSwapchainCount{0u};
constexpr std::size_t kFastVirtualImageSlots = 32u;
std::array<std::atomic<VkImage>, kFastVirtualImageSlots> g_fastVirtualImages{};


[[nodiscard]] std::uint64_t wsiConfigurationHash(const SwapchainRecord& record) noexcept
{
    std::uint64_t hash = apusr::profile::hashString("vulkan.wsi.v1");
    hash = apusr::profile::hashCombine(
        hash, (static_cast<std::uint64_t>(record.imageExtent.width) << 32u) | record.imageExtent.height);
    hash = apusr::profile::hashCombine(
        hash, (static_cast<std::uint64_t>(record.imageFormat) << 32u) | record.imageColorSpace);
    hash = apusr::profile::hashCombine(
        hash, (static_cast<std::uint64_t>(record.presentMode) << 32u) | record.imageCount);
    hash = apusr::profile::hashCombine(
        hash, (static_cast<std::uint64_t>(record.imageUsage) << 32u) | record.framegenFlags);
    return apusr::profile::hashCombine(hash, record.virtualized ? 1u : 0u);
}

struct WsiProfileCapture {
    apusr::profile::StreamHandle stream{};
    apusr::profile::SampleDecision sample{};
    std::uint64_t frameId = 0u;
    std::uint64_t configurationHash = 0u;
    std::uint64_t previousTick = 0u;
    std::uint64_t submitSequence = 0u;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    VkPhysicalDevice physicalDevice = nullptr;
    vk::PfnGetPhysicalDeviceMemoryProperties2 getPhysicalDeviceMemoryProperties2 = nullptr;
    bool memoryBudgetEnabled = false;
    bool virtualized = false;

    [[nodiscard]] explicit operator bool() const noexcept { return stream && sample.capture; }
};

enum class WsiProfileOperation : std::uint32_t {
    Acquire,
    Present,
};

[[nodiscard]] WsiProfileCapture beginWsiProfile(
    const VkSwapchainKHR swapchain,
    const WsiProfileOperation operation)
{
    WsiProfileCapture capture{};
    if (!apusr::profile::enabled() || swapchain == nullptr)
        return capture;

    // Sampled profiling must not turn the process-wide host-capture state mutex
    // into an acquire/present serialization point.  Decide the ordinary burst
    // window before touching swapchain state; only selected WSI calls pay for
    // the detailed lookup.  Full/error modes deliberately remain exhaustive.
    apusr::profile::SampleDecision sampledDecision{};
    const bool sampledMode = apusr::profile::settings().mode == apusr::profile::Mode::Sampled;
    if (sampledMode) {
        struct ThreadWsiGates {
            apusr::profile::SampleGate acquire;
            apusr::profile::SampleGate present;
        };
        static thread_local ThreadWsiGates gates{};
        apusr::profile::SampleGate& gate = operation == WsiProfileOperation::Acquire
            ? gates.acquire
            : gates.present;
        sampledDecision = gate.decide(0u, 0u);
        if (!sampledDecision.capture)
            return capture;
    }

    State& stateRef = state();
    std::lock_guard<std::mutex> lock(stateRef.mutex);
    const auto it = stateRef.swapchains.find(swapchain);
    if (it == stateRef.swapchains.end())
        return capture;

    SwapchainRecord& record = it->second;
    capture.stream = record.profileStream;
    if (!capture.stream)
        return capture;

    if (operation == WsiProfileOperation::Acquire) {
        capture.frameId = record.acquireSerial + 1u;
        capture.sample = sampledMode
            ? sampledDecision
            : record.profileAcquireGate.decide(record.profileConfigurationHash, capture.frameId);
        capture.previousTick = sampledMode ? 0u : record.profileLastAcquireTick;
    } else {
        capture.frameId = record.presentSerial + 1u;
        capture.sample = sampledMode
            ? sampledDecision
            : record.profilePresentGate.decide(record.profileConfigurationHash, capture.frameId);
        capture.previousTick = sampledMode ? 0u : record.profileLastPresentTick;
    }
    capture.configurationHash = record.profileConfigurationHash;
    capture.submitSequence = capture.sample.capture ? ++record.profileQueueSequence : record.profileQueueSequence;
    capture.width = record.imageExtent.width;
    capture.height = record.imageExtent.height;
    capture.virtualized = record.virtualized;
    const auto deviceIt = stateRef.devices.find(record.device);
    if (deviceIt != stateRef.devices.end()) {
        capture.physicalDevice = deviceIt->second.physicalDevice;
        capture.getPhysicalDeviceMemoryProperties2 = deviceIt->second.getPhysicalDeviceMemoryProperties2;
        capture.memoryBudgetEnabled = deviceIt->second.memoryBudgetEnabled;
    }
    return capture;
}

void updateWsiProfileTick(
    const VkSwapchainKHR swapchain,
    const WsiProfileOperation operation,
    const std::uint64_t tick) noexcept
{
    if (swapchain == nullptr || tick == 0u)
        return;
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.swapchains.find(swapchain);
    if (it == capture.swapchains.end())
        return;
    if (operation == WsiProfileOperation::Acquire)
        it->second.profileLastAcquireTick = tick;
    else
        it->second.profileLastPresentTick = tick;
}

void recordWsiCpuInterval(
    const WsiProfileCapture& capture,
    const char* name,
    const std::uint64_t beginTick,
    const std::uint64_t endTick) noexcept
{
    if (!capture || !apusr::profile::cpuEnabled())
        return;
    apusr::profile::recordCpuInterval(apusr::profile::CpuInterval{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .sampleSequence = capture.sample.sampleSequence,
        .startCpuTick = beginTick,
        .endCpuTick = endTick,
        .name = name,
    });
}

void recordWsiCadence(
    const WsiProfileCapture& capture,
    const char* name,
    const std::uint64_t nowTick) noexcept
{
    if (!capture || !apusr::profile::cpuEnabled() || capture.previousTick == 0u || nowTick <= capture.previousTick)
        return;
    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .name = name,
        .value = apusr::profile::durationNs(capture.previousTick, nowTick),
    });
}

void recordWsiResultError(
    const WsiProfileCapture& capture,
    const char* stage,
    const VkResult result,
    const std::uint64_t contextValue = 0u) noexcept
{
    if (!capture.stream || result >= 0)
        return;
    apusr::profile::recordError(
        capture.stream,
        capture.frameId,
        stage,
        apusr::StatusCode::RuntimeFailure,
        "Vulkan WSI operation failed",
        static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
        contextValue);
}

void recordWsiAcquire(
    const WsiProfileCapture& capture,
    const char* apiName,
    const std::uint64_t beginTick,
    const std::uint64_t endTick,
    const VkResult result,
    const std::uint64_t timeout,
    const VkSemaphore semaphore,
    const VkFence fence,
    const std::uint32_t imageIndex) noexcept
{
    recordWsiCpuInterval(capture, "wsi.acquire_call", beginTick, endTick);
    recordWsiCadence(capture, "wsi.acquire_interval_ns", beginTick);
    recordWsiResultError(capture, apiName, result, imageIndex);
    if (!capture)
        return;

    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .name = "wsi.acquire_result",
        .value = static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
        .value2 = imageIndex,
        .flags = capture.virtualized ? 1u : 0u,
    });
    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .name = "wsi.acquire_timeout_ns",
        .value = timeout,
    });
    const std::uint64_t syncObject = semaphore != nullptr
        ? static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(semaphore))
        : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(fence));
    apusr::profile::recordQueueEvent(apusr::profile::QueueEvent{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .sampleSequence = capture.sample.sampleSequence,
        .name = "wsi.acquire",
        .nativeQueue = 0u,
        .syncObject = syncObject,
        .submitSequence = capture.submitSequence,
        .operation = apusr::profile::QueueOperation::Acquire,
        .objectCount = static_cast<std::uint32_t>((semaphore != nullptr) + (fence != nullptr)),
        .flags = capture.virtualized ? 1u : 0u,
    });
}

void recordWsiMemoryBudget(const WsiProfileCapture& capture) noexcept
{
    // Budget queries are driver calls, so sample them once per burst rather than
    // once per presented frame. This keeps the hot WSI path cheap while still
    // correlating memory pressure with the sampled performance window.
    if (!capture || capture.sample.burstFrame != 0u || !capture.memoryBudgetEnabled ||
        capture.physicalDevice == nullptr || capture.getPhysicalDeviceMemoryProperties2 == nullptr)
        return;

    vk::PhysicalDeviceMemoryBudgetPropertiesExt budget{
        .sType = vk::StructureTypePhysicalDeviceMemoryBudgetPropertiesExt,
        .pNext = nullptr,
    };
    vk::PhysicalDeviceMemoryProperties2 properties{
        .sType = vk::StructureTypePhysicalDeviceMemoryProperties2,
        .pNext = &budget,
    };
    capture.getPhysicalDeviceMemoryProperties2(reinterpret_cast<vk::PhysicalDevice>(capture.physicalDevice), &properties);

    const std::uint32_t heapCount = std::min(properties.memoryProperties.memoryHeapCount, vk::MaxMemoryHeaps);
    std::uint64_t localUsage = 0u;
    std::uint64_t localBudget = 0u;
    std::uint64_t nonlocalUsage = 0u;
    std::uint64_t nonlocalBudget = 0u;
    for (std::uint32_t heap = 0u; heap < heapCount; ++heap) {
        const bool deviceLocal =
            (properties.memoryProperties.memoryHeaps[heap].flags & vk::MemoryHeapDeviceLocalBit) != 0u;
        const std::uint64_t usage = budget.heapUsage[heap];
        const std::uint64_t heapBudget = budget.heapBudget[heap];
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = capture.stream,
            .frameId = capture.frameId,
            .name = "memory.budget.heap",
            .value = usage,
            .value2 = heapBudget,
            .flags = (heap & 0xffu) | (deviceLocal ? 0x100u : 0u),
        });
        if (deviceLocal) {
            localUsage += usage;
            localBudget += heapBudget;
        } else {
            nonlocalUsage += usage;
            nonlocalBudget += heapBudget;
        }
    }

    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .name = "memory.budget.local",
        .value = localUsage,
        .value2 = localBudget,
        .flags = heapCount,
    });
    if (nonlocalBudget != 0u || nonlocalUsage != 0u) {
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = capture.stream,
            .frameId = capture.frameId,
            .name = "memory.budget.nonlocal",
            .value = nonlocalUsage,
            .value2 = nonlocalBudget,
            .flags = heapCount,
        });
    }
}

void recordWsiPresent(
    const WsiProfileCapture& capture,
    const VkQueue queue,
    const VkPresentInfoKHRLoose* info,
    const std::uint64_t beginTick,
    const std::uint64_t endTick,
    const VkResult aggregateResult,
    const std::uint32_t swapchainIndex) noexcept
{
    if (info == nullptr)
        return;
    const VkResult result = info->pResults != nullptr ? info->pResults[swapchainIndex] : aggregateResult;
    recordWsiCpuInterval(capture, "wsi.present_call", beginTick, endTick);
    recordWsiCadence(capture, "wsi.present_interval_ns", beginTick);
    recordWsiResultError(capture, "vkQueuePresentKHR", result, swapchainIndex);
    if (!capture)
        return;

    recordWsiMemoryBudget(capture);

    const std::uint64_t duration = endTick >= beginTick
        ? apusr::profile::durationNs(beginTick, endTick)
        : 0u;
    apusr::profile::recordFrameSample(apusr::profile::FrameSample{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .sampleSequence = capture.sample.sampleSequence,
        .burstSequence = capture.sample.burstSequence,
        .configurationHash = capture.configurationHash,
        .burstFrame = capture.sample.burstFrame,
        .renderWidth = capture.width,
        .renderHeight = capture.height,
        .displayWidth = capture.width,
        .displayHeight = capture.height,
        .cpuTotalNs = duration,
        .flags = capture.sample.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u,
    });
    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .name = "wsi.present_result",
        .value = static_cast<std::uint64_t>(static_cast<std::int64_t>(result)),
        .value2 = swapchainIndex,
        .flags = capture.virtualized ? 1u : 0u,
    });

    const std::uint64_t nativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(queue));
    apusr::profile::recordQueueEvent(apusr::profile::QueueEvent{
        .stream = capture.stream,
        .frameId = capture.frameId,
        .sampleSequence = capture.sample.sampleSequence,
        .name = "wsi.present",
        .nativeQueue = nativeQueue,
        .submitSequence = capture.submitSequence,
        .operation = apusr::profile::QueueOperation::Present,
        .objectCount = info->swapchainCount,
        .flags = capture.virtualized ? 1u : 0u,
    });
    for (std::uint32_t waitIndex = 0u; waitIndex < info->waitSemaphoreCount; ++waitIndex) {
        const VkSemaphore semaphore = info->pWaitSemaphores != nullptr ? info->pWaitSemaphores[waitIndex] : nullptr;
        if (semaphore == nullptr)
            continue;
        apusr::profile::recordQueueEvent(apusr::profile::QueueEvent{
            .stream = capture.stream,
            .frameId = capture.frameId,
            .sampleSequence = capture.sample.sampleSequence,
            .name = "wsi.present_wait",
            .nativeQueue = nativeQueue,
            .syncObject = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(semaphore)),
            .submitSequence = capture.submitSequence,
            .operation = apusr::profile::QueueOperation::Wait,
            .objectCount = info->waitSemaphoreCount,
        });
    }
}

constexpr std::size_t kInlineWsiPresentProfiles = 4u;

struct WsiPresentProfileBatch {
    std::array<WsiProfileCapture, kInlineWsiPresentProfiles> inlineCaptures{};
    std::vector<WsiProfileCapture> overflowCaptures;
    std::uint32_t count = 0u;

    [[nodiscard]] WsiProfileCapture& at(const std::uint32_t index) noexcept
    {
        if (index < kInlineWsiPresentProfiles)
            return inlineCaptures[index];
        return overflowCaptures[index - kInlineWsiPresentProfiles];
    }
};

[[nodiscard]] WsiPresentProfileBatch beginWsiPresentProfileBatch(const VkPresentInfoKHRLoose* info)
{
    WsiPresentProfileBatch batch{};
    if (!apusr::profile::enabled() || info == nullptr || info->sType != VkStructureTypePresentInfoKhr ||
        info->swapchainCount == 0u || info->pSwapchains == nullptr)
        return batch;

    batch.count = info->swapchainCount;
    if (batch.count > kInlineWsiPresentProfiles)
        batch.overflowCaptures.resize(batch.count - kInlineWsiPresentProfiles);
    for (std::uint32_t index = 0u; index < batch.count; ++index)
        batch.at(index) = beginWsiProfile(info->pSwapchains[index], WsiProfileOperation::Present);
    return batch;
}

void finishWsiPresentProfileBatch(
    WsiPresentProfileBatch& batch,
    const VkQueue queue,
    const VkPresentInfoKHRLoose* info,
    const std::uint64_t beginTick,
    const std::uint64_t endTick,
    const VkResult result) noexcept
{
    if (info == nullptr || batch.count == 0u)
        return;
    const std::uint32_t count = std::min(batch.count, info->swapchainCount);
    for (std::uint32_t index = 0u; index < count; ++index) {
        updateWsiProfileTick(info->pSwapchains[index], WsiProfileOperation::Present, beginTick);
        recordWsiPresent(batch.at(index), queue, info, beginTick, endTick, result, index);
    }
}


std::mutex& createMutex()
{
    static std::mutex value;
    return value;
}

std::mutex& virtualAcquireQueueMutex()
{
    static std::mutex value;
    return value;
}

[[nodiscard]] Dispatch dispatchSnapshot()
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    return capture.dispatch;
}

[[nodiscard]] bool antiLagRequestedSnapshot()
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    return capture.antiLagRequested;
}

[[nodiscard]] bool frameGenerationRequestedSnapshot()
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    return capture.frameGenerationRequested;
}

[[nodiscard]] bool queryRelativePresentTimingDomain(
    VkGetSwapchainTimeDomainPropertiesEXTFn getTimeDomains,
    VkDevice device,
    VkSwapchainKHR swapchain,
    const std::shared_ptr<std::mutex>& hostMutex,
    std::int32_t& outTimeDomain,
    std::uint64_t& outTimeDomainId)
{
    if (getTimeDomains == nullptr || device == nullptr || swapchain == nullptr || !hostMutex)
        return false;

    std::lock_guard<std::mutex> hostLock(*hostMutex);
    VkSwapchainTimeDomainPropertiesEXTLoose domains{};
    std::uint64_t domainsCounter = 0u;
    VkResult result = getTimeDomains(device, swapchain, &domains, &domainsCounter);
    if ((result != VkSuccess && result != VkIncomplete) || domains.timeDomainCount == 0u)
        return false;

    std::vector<std::int32_t> timeDomains(domains.timeDomainCount);
    std::vector<std::uint64_t> timeDomainIds(domains.timeDomainCount);
    domains.pTimeDomains = timeDomains.data();
    domains.pTimeDomainIds = timeDomainIds.data();
    result = getTimeDomains(device, swapchain, &domains, &domainsCounter);
    if (result != VkSuccess && result != VkIncomplete)
        return false;

    const std::uint32_t written = std::min<std::uint32_t>(
        domains.timeDomainCount, static_cast<std::uint32_t>(timeDomains.size()));
    if (written == 0u)
        return false;

    // A swapchain-local domain avoids calibrated timestamp work for the
    // relative scheduling path. If it is not exposed, any domain returned by
    // the implementation is legal; stage-local domains are handled explicitly
    // when building VkPresentTimingInfoEXT.
    std::uint32_t selected = 0u;
    for (std::uint32_t index = 0u; index < written; ++index) {
        if (timeDomains[index] == VkTimeDomainSwapchainLocalExt) {
            selected = index;
            break;
        }
    }
    outTimeDomain = timeDomains[selected];
    outTimeDomainId = timeDomainIds[selected];
    return true;
}

[[nodiscard]] std::shared_ptr<std::mutex> realWsiHostMutexForSwapchain(VkSwapchainKHR swapchain)
{
    if (swapchain == nullptr)
        return {};
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.swapchains.find(swapchain);
    return it != capture.swapchains.end() ? it->second.realWsiHostMutex : std::shared_ptr<std::mutex>{};
}

[[nodiscard]] bool frameGenerationActiveSnapshot() noexcept
{
    return g_frameGenerationActive.load(std::memory_order_relaxed);
}

void registerFastVirtualImage(VkImage image) noexcept
{
    if (!image)
        return;
    for (auto& slot : g_fastVirtualImages) {
        VkImage expected = nullptr;
        if (slot.compare_exchange_strong(expected, image, std::memory_order_release, std::memory_order_relaxed))
            return;
    }
    apusr::host_discovery::eventf(
        "vulkan", "framegen.virtual_image_registry_full", "image=%p capacity=%zu", image, kFastVirtualImageSlots);
}

void unregisterFastVirtualImage(VkImage image) noexcept
{
    if (!image)
        return;
    for (auto& slot : g_fastVirtualImages) {
        VkImage current = slot.load(std::memory_order_acquire);
        if (current == image) {
            slot.compare_exchange_strong(current, nullptr, std::memory_order_release, std::memory_order_relaxed);
            return;
        }
    }
}

struct VirtualImageFunctions {
    vk::PfnGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    vk::PfnCreateImage createImage = nullptr;
    vk::PfnDestroyImage destroyImage = nullptr;
    vk::PfnGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    vk::PfnAllocateMemory allocateMemory = nullptr;
    vk::PfnFreeMemory freeMemory = nullptr;
    vk::PfnBindImageMemory bindImageMemory = nullptr;
    vk::PfnCreateCommandPool createCommandPool = nullptr;
    vk::PfnDestroyCommandPool destroyCommandPool = nullptr;
    vk::PfnAllocateCommandBuffers allocateCommandBuffers = nullptr;
    vk::PfnBeginCommandBuffer beginCommandBuffer = nullptr;
    vk::PfnEndCommandBuffer endCommandBuffer = nullptr;
    vk::PfnCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    vk::PfnCreateSemaphore createSemaphore = nullptr;
    vk::PfnDestroySemaphore destroySemaphore = nullptr;
    vk::PfnGetSemaphoreCounterValue getSemaphoreCounterValue = nullptr;
    vk::PfnWaitSemaphores waitSemaphores = nullptr;
    vk::PfnQueueWaitIdle queueWaitIdle = nullptr;
};

struct VirtualImageDeviceContext {
    VkInstance instance = nullptr;
    VkPhysicalDevice physicalDevice = nullptr;
    VkDevice device = nullptr;
    VkQueue acquireQueue = nullptr;
    std::uint32_t acquireQueueFamily = 0u;
    std::uint32_t graphicsQueueFamily = ~0u;
    std::uint32_t generationQueueFamily = ~0u;
    std::uint32_t replayQueueFamily = ~0u;
    vk::PfnQueueSubmit queueSubmit = nullptr;
    VirtualImageFunctions functions{};
};

[[nodiscard]] bool loadVirtualImageDeviceContext(VkDevice device, VirtualImageDeviceContext& out)
{
    if (!device)
        return false;
    Dispatch dispatch{};
    std::vector<QueueRecord> queues;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.devices.find(device);
        if (it == capture.devices.end())
            return false;
        out.instance = it->second.instance;
        out.physicalDevice = it->second.physicalDevice;
        out.device = it->second.device;
        out.queueSubmit = it->second.queueSubmit;
        queues = it->second.queues;
        dispatch = capture.dispatch;
    }
    if (!out.instance || !out.physicalDevice || !dispatch.getDeviceProcAddr || !dispatch.getInstanceProcAddr || !out.queueSubmit)
        return false;

    for (const QueueRecord& q : queues) {
        if (!q.queue || (q.ownershipFlags & APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR) == 0u)
            continue;
        if ((q.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_ACQUIRE) != 0u) {
            out.acquireQueue = q.queue;
            out.acquireQueueFamily = q.familyIndex;
            out.graphicsQueueFamily = q.familyIndex;
        }
        if ((q.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_GENERATION) != 0u)
            out.generationQueueFamily = q.familyIndex;
        if ((q.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_REPLAY_TRANSFER) != 0u)
            out.replayQueueFamily = q.familyIndex;
        if ((q.ownershipFlags & (APUSR_VK_HOST_QUEUE_ROLE_INPUT | APUSR_VK_HOST_QUEUE_ROLE_PRESENTATION)) != 0u)
            out.graphicsQueueFamily = q.familyIndex;
    }
    if (!out.acquireQueue)
        return false;
    if (out.generationQueueFamily == ~0u)
        out.generationQueueFamily = out.graphicsQueueFamily;

    out.functions.getPhysicalDeviceMemoryProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceMemoryProperties>(
        dispatch.getInstanceProcAddr(out.instance, "vkGetPhysicalDeviceMemoryProperties"));
#define APUSR_LOAD_VIRTUAL_DEVICE_FN(field, name) \
    out.functions.field = reinterpret_cast<decltype(out.functions.field)>(dispatch.getDeviceProcAddr(device, name))
    APUSR_LOAD_VIRTUAL_DEVICE_FN(createImage, "vkCreateImage");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(destroyImage, "vkDestroyImage");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(getImageMemoryRequirements, "vkGetImageMemoryRequirements");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(allocateMemory, "vkAllocateMemory");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(freeMemory, "vkFreeMemory");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(bindImageMemory, "vkBindImageMemory");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(createCommandPool, "vkCreateCommandPool");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(destroyCommandPool, "vkDestroyCommandPool");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(allocateCommandBuffers, "vkAllocateCommandBuffers");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(beginCommandBuffer, "vkBeginCommandBuffer");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(endCommandBuffer, "vkEndCommandBuffer");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(cmdPipelineBarrier, "vkCmdPipelineBarrier");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(createSemaphore, "vkCreateSemaphore");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(destroySemaphore, "vkDestroySemaphore");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(getSemaphoreCounterValue, "vkGetSemaphoreCounterValue");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(waitSemaphores, "vkWaitSemaphores");
    APUSR_LOAD_VIRTUAL_DEVICE_FN(queueWaitIdle, "vkQueueWaitIdle");
#undef APUSR_LOAD_VIRTUAL_DEVICE_FN
    return out.functions.getPhysicalDeviceMemoryProperties && out.functions.createImage &&
        out.functions.destroyImage && out.functions.getImageMemoryRequirements && out.functions.allocateMemory &&
        out.functions.freeMemory && out.functions.bindImageMemory && out.functions.createCommandPool &&
        out.functions.destroyCommandPool && out.functions.allocateCommandBuffers && out.functions.beginCommandBuffer &&
        out.functions.endCommandBuffer && out.functions.cmdPipelineBarrier && out.functions.createSemaphore &&
        out.functions.destroySemaphore && out.functions.getSemaphoreCounterValue && out.functions.waitSemaphores &&
        out.functions.queueWaitIdle && out.acquireQueue;
}

[[nodiscard]] std::uint32_t chooseDeviceLocalMemoryType(
    const vk::PhysicalDeviceMemoryProperties& properties,
    const std::uint32_t typeBits) noexcept
{
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0u &&
            (properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyDeviceLocalBit) != 0u)
            return i;
    }
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0u)
            return i;
    }
    return ~0u;
}

void destroyVirtualImages(SwapchainRecord& record) noexcept
{
    const bool ownsVirtualResources = record.virtualized || !record.virtualImageMemory.empty() ||
        record.virtualInitCommandPool != nullptr;
    if (!ownsVirtualResources)
        return;
    const bool wasVirtualized = record.virtualized;
    if (record.images.empty() && record.virtualImageMemory.empty() && !record.virtualInitCommandPool) {
        record.virtualized = false;
        if (wasVirtualized)
            g_virtualSwapchainCount.fetch_sub(1u, std::memory_order_acq_rel);
        return;
    }
    VirtualImageDeviceContext context{};
    if (loadVirtualImageDeviceContext(record.device, context)) {
        const auto device = reinterpret_cast<vk::Device>(record.device);
        // The init command and acquire-signal submits use the dedicated third
        // private queue.  Swapchain destruction is a cold path; waiting here
        // makes command-pool and image retirement unambiguous.
        (void)context.functions.queueWaitIdle(reinterpret_cast<vk::Queue>(context.acquireQueue));
        if (record.virtualInitCommandPool) {
            context.functions.destroyCommandPool(device, record.virtualInitCommandPool, nullptr);
            record.virtualInitCommandPool = nullptr;
        }
        for (std::size_t i = 0; i < record.images.size(); ++i) {
            unregisterFastVirtualImage(record.images[i]);
            if (record.images[i])
                context.functions.destroyImage(device, reinterpret_cast<vk::Image>(record.images[i]), nullptr);
            if (i < record.virtualImageMemory.size() && record.virtualImageMemory[i])
                context.functions.freeMemory(device, record.virtualImageMemory[i], nullptr);
        }
    } else {
        for (VkImage image : record.images)
            unregisterFastVirtualImage(image);
    }
    record.images.clear();
    record.virtualImageMemory.clear();
    record.virtualSlots.clear();
    record.imageCount = 0u;
    record.virtualized = false;
    if (wasVirtualized)
        g_virtualSwapchainCount.fetch_sub(1u, std::memory_order_acq_rel);
}

[[nodiscard]] bool createVirtualImages(
    SwapchainRecord& record,
    const VkSwapchainCreateInfoKHRLoose& appCreateInfo,
    const std::uint32_t imageCount)
{
    if (imageCount < 2u)
        return false;
    VirtualImageDeviceContext context{};
    if (!loadVirtualImageDeviceContext(record.device, context))
        return false;

    vk::PhysicalDeviceMemoryProperties memoryProperties{};
    context.functions.getPhysicalDeviceMemoryProperties(
        reinterpret_cast<vk::PhysicalDevice>(context.physicalDevice), &memoryProperties);

    // Virtual backbuffers are retained source frames. APUSR samples the image
    // directly for OF/FI and can concurrently copy it to the private WSI path;
    // no full-resolution source copy is required.
    const std::uint32_t virtualUsage =
        appCreateInfo.imageUsage | vk::ImageUsageTransferSrcBit | vk::ImageUsageSampledBit;
    record.images.assign(imageCount, nullptr);
    record.virtualImageMemory.assign(imageCount, nullptr);
    record.virtualSlots.assign(imageCount, {});
    record.imageUsage = virtualUsage;

    std::vector<std::uint32_t> sharingFamilies;
    if (appCreateInfo.imageSharingMode == static_cast<std::uint32_t>(vk::SharingModeConcurrent) &&
        appCreateInfo.pQueueFamilyIndices && appCreateInfo.queueFamilyIndexCount != 0u) {
        sharingFamilies.assign(
            appCreateInfo.pQueueFamilyIndices,
            appCreateInfo.pQueueFamilyIndices + appCreateInfo.queueFamilyIndexCount);
    }
    const auto addSharingFamily = [&](const std::uint32_t family) {
        if (family == ~0u || std::find(sharingFamilies.begin(), sharingFamilies.end(), family) != sharingFamilies.end())
            return;
        sharingFamilies.push_back(family);
    };
    addSharingFamily(context.graphicsQueueFamily);
    addSharingFamily(context.generationQueueFamily);
    addSharingFamily(context.replayQueueFamily);
    const bool concurrentSharing = sharingFamilies.size() > 1u;

    for (std::uint32_t i = 0; i < imageCount; ++i) {
        const vk::ImageCreateInfo info{
            .sType = vk::StructureTypeImageCreateInfo,
            .pNext = nullptr,
            .flags = 0u,
            .imageType = vk::ImageType2D,
            .format = static_cast<vk::Format>(appCreateInfo.imageFormat),
            .extent = {appCreateInfo.imageExtent.width, appCreateInfo.imageExtent.height, 1u},
            .mipLevels = 1u,
            .arrayLayers = appCreateInfo.imageArrayLayers,
            .samples = vk::SampleCount1Bit,
            .tiling = vk::ImageTilingOptimal,
            .usage = virtualUsage,
            .sharingMode = concurrentSharing ? vk::SharingModeConcurrent : vk::SharingModeExclusive,
            .queueFamilyIndexCount = concurrentSharing ? static_cast<std::uint32_t>(sharingFamilies.size()) : 0u,
            .pQueueFamilyIndices = concurrentSharing ? sharingFamilies.data() : nullptr,
            .initialLayout = vk::ImageLayoutUndefined,
        };
        vk::Image image = nullptr;
        if (context.functions.createImage(
                reinterpret_cast<vk::Device>(record.device), &info, nullptr, &image) != vk::Success || !image) {
            destroyVirtualImages(record);
            return false;
        }
        record.images[i] = reinterpret_cast<VkImage>(image);
        vk::MemoryRequirements requirements{};
        context.functions.getImageMemoryRequirements(
            reinterpret_cast<vk::Device>(record.device), image, &requirements);
        const std::uint32_t memoryType = chooseDeviceLocalMemoryType(memoryProperties, requirements.memoryTypeBits);
        if (memoryType == ~0u) {
            destroyVirtualImages(record);
            return false;
        }
        const vk::MemoryAllocateInfo allocation{
            .sType = vk::StructureTypeMemoryAllocateInfo,
            .pNext = nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memoryType,
        };
        if (context.functions.allocateMemory(
                reinterpret_cast<vk::Device>(record.device), &allocation, nullptr,
                &record.virtualImageMemory[i]) != vk::Success || !record.virtualImageMemory[i] ||
            context.functions.bindImageMemory(
                reinterpret_cast<vk::Device>(record.device), image, record.virtualImageMemory[i], 0u) != vk::Success) {
            destroyVirtualImages(record);
            return false;
        }
        registerFastVirtualImage(record.images[i]);
    }
    // Ordinary VkImage objects start in UNDEFINED.  The application sees
    // them as swapchain-like backbuffers and may legally describe its first
    // use as PRESENT->render.  Initialize every virtual image to GENERAL once
    // on APUSR's private acquire queue; all acquire-signal submissions on that
    // queue are FIFO-after this initialization, so no CPU wait is needed.
    const vk::CommandPoolCreateInfo poolInfo{
        .sType = vk::StructureTypeCommandPoolCreateInfo,
        .pNext = nullptr,
        .flags = vk::CommandPoolCreateResetCommandBufferBit,
        .queueFamilyIndex = context.acquireQueueFamily,
    };
    if (context.functions.createCommandPool(
            reinterpret_cast<vk::Device>(record.device), &poolInfo, nullptr, &record.virtualInitCommandPool) != vk::Success ||
        !record.virtualInitCommandPool) {
        destroyVirtualImages(record);
        return false;
    }
    vk::CommandBuffer initCommand = nullptr;
    const vk::CommandBufferAllocateInfo allocateInfo{
        .sType = vk::StructureTypeCommandBufferAllocateInfo,
        .pNext = nullptr,
        .commandPool = record.virtualInitCommandPool,
        .level = vk::CommandBufferLevelPrimary,
        .commandBufferCount = 1u,
    };
    const vk::CommandBufferBeginInfo beginInfo{
        .sType = vk::StructureTypeCommandBufferBeginInfo,
        .pNext = nullptr,
        .flags = vk::CommandBufferUsageOneTimeSubmitBit,
        .pInheritanceInfo = nullptr,
    };
    if (context.functions.allocateCommandBuffers(
            reinterpret_cast<vk::Device>(record.device), &allocateInfo, &initCommand) != vk::Success || !initCommand ||
        context.functions.beginCommandBuffer(initCommand, &beginInfo) != vk::Success) {
        destroyVirtualImages(record);
        return false;
    }
    std::vector<vk::ImageMemoryBarrier> initBarriers;
    initBarriers.reserve(imageCount);
    for (VkImage image : record.images) {
        initBarriers.push_back(vk::ImageMemoryBarrier{
            .sType = vk::StructureTypeImageMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = 0u,
            .dstAccessMask = 0u,
            .oldLayout = vk::ImageLayoutUndefined,
            .newLayout = vk::ImageLayoutGeneral,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = reinterpret_cast<vk::Image>(image),
            .subresourceRange = {
                vk::ImageAspectColorBit, 0u, 1u, 0u, appCreateInfo.imageArrayLayers,
            },
        });
    }
    context.functions.cmdPipelineBarrier(
        initCommand, vk::PipelineStageTopOfPipeBit, vk::PipelineStageAllCommandsBit, 0u,
        0u, nullptr, 0u, nullptr, static_cast<std::uint32_t>(initBarriers.size()), initBarriers.data());
    if (context.functions.endCommandBuffer(initCommand) != vk::Success) {
        destroyVirtualImages(record);
        return false;
    }
    const vk::SubmitInfo initSubmit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreCount = 0u,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1u,
        .pCommandBuffers = &initCommand,
        .signalSemaphoreCount = 0u,
        .pSignalSemaphores = nullptr,
    };
    {
        std::lock_guard<std::mutex> queueLock(virtualAcquireQueueMutex());
        if (context.queueSubmit(reinterpret_cast<vk::Queue>(context.acquireQueue), 1u, &initSubmit, nullptr) != vk::Success) {
            destroyVirtualImages(record);
            return false;
        }
    }

    record.imageCount = imageCount;
    record.virtualized = true;
    record.framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_VIRTUALIZED;
    g_virtualSwapchainCount.fetch_add(1u, std::memory_order_acq_rel);
    return true;
}

VkResult acquireVirtualImage(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const std::uint64_t timeout,
    VkSemaphore signalSemaphore,
    VkFence signalFence,
    std::uint32_t* imageIndex)
{
    if (!device || !swapchain || !imageIndex || (!signalSemaphore && !signalFence))
        return VkErrorInitializationFailed;

    VirtualImageDeviceContext deviceContext{};
    if (!loadVirtualImageDeviceContext(device, deviceContext))
        return VkErrorInitializationFailed;

    const auto waitStart = std::chrono::steady_clock::now();
    std::uint32_t selected = APUSR_VK_HOST_INVALID_IMAGE_INDEX;
    std::array<VkSemaphore, 2u> selectedReleaseSemaphores{};
    std::array<std::uint64_t, 2u> selectedReleaseValues{};

    for (;;) {
        std::vector<vk::Semaphore> waitSemaphores;
        std::vector<std::uint64_t> waitValues;
        std::vector<std::pair<vk::Semaphore, std::uint64_t>> completedCounters;
        const auto completedValue = [&](const vk::Semaphore semaphore, std::uint64_t& out) -> bool {
            const auto cached = std::find_if(
                completedCounters.begin(), completedCounters.end(),
                [semaphore](const auto& entry) { return entry.first == semaphore; });
            if (cached != completedCounters.end()) {
                out = cached->second;
                return true;
            }
            if (deviceContext.functions.getSemaphoreCounterValue(
                    reinterpret_cast<vk::Device>(device), semaphore, &out) != vk::Success)
                return false;
            completedCounters.emplace_back(semaphore, out);
            return true;
        };
        const auto addWait = [&](const vk::Semaphore semaphore, const std::uint64_t value) {
            const auto existing = std::find(waitSemaphores.begin(), waitSemaphores.end(), semaphore);
            if (existing == waitSemaphores.end()) {
                waitSemaphores.push_back(semaphore);
                waitValues.push_back(value);
                return;
            }
            const auto index = static_cast<std::size_t>(existing - waitSemaphores.begin());
            waitValues[index] = std::min(waitValues[index], value);
        };
        {
            State& capture = state();
            std::lock_guard<std::mutex> lock(capture.mutex);
            const auto it = capture.swapchains.find(swapchain);
            if (it == capture.swapchains.end() || !it->second.virtualized || it->second.virtualSlots.empty())
                return VkErrorInitializationFailed;
            SwapchainRecord& record = it->second;
            const std::uint32_t count = static_cast<std::uint32_t>(record.virtualSlots.size());
            for (std::uint32_t step = 0; step < count; ++step) {
                const std::uint32_t candidate = (record.nextVirtualAcquire + step) % count;
                auto& slot = record.virtualSlots[candidate];
                if (slot.acquired)
                    continue;

                bool ready = true;
                for (std::size_t dependency = 0; dependency < slot.releaseValues.size(); ++dependency) {
                    const std::uint64_t required = slot.releaseValues[dependency];
                    if (required == 0u)
                        continue;
                    const auto semaphore = reinterpret_cast<vk::Semaphore>(slot.releaseSemaphores[dependency]);
                    if (!semaphore)
                        return VkErrorInitializationFailed;
                    std::uint64_t completed = 0u;
                    if (!completedValue(semaphore, completed))
                        return VkErrorInitializationFailed;
                    if (completed < required) {
                        ready = false;
                        addWait(semaphore, required);
                    }
                }
                if (!ready)
                    continue;

                selected = candidate;
                selectedReleaseSemaphores = slot.releaseSemaphores;
                selectedReleaseValues = slot.releaseValues;
                slot.acquired = true;
                slot.releaseSemaphores = {};
                slot.releaseValues = {};
                record.nextVirtualAcquire = (selected + 1u) % count;
                break;
            }
        }

        if (selected != APUSR_VK_HOST_INVALID_IMAGE_INDEX)
            break;
        if (timeout == 0u)
            return VkNotReady;
        if (waitSemaphores.empty())
            return VkTimeout;

        std::uint64_t remaining = timeout;
        if (timeout != ~std::uint64_t{0}) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - waitStart).count();
            if (elapsed < 0 || static_cast<std::uint64_t>(elapsed) >= timeout)
                return VkTimeout;
            remaining = timeout - static_cast<std::uint64_t>(elapsed);
        }
        const vk::SemaphoreWaitInfo waitInfo{
            .sType = vk::StructureTypeSemaphoreWaitInfo,
            .pNext = nullptr,
            .flags = vk::SemaphoreWaitAnyBit,
            .semaphoreCount = static_cast<std::uint32_t>(waitSemaphores.size()),
            .pSemaphores = waitSemaphores.data(),
            .pValues = waitValues.data(),
        };
        const vk::Result waitResult = deviceContext.functions.waitSemaphores(
            reinterpret_cast<vk::Device>(device), &waitInfo, remaining);
        if (waitResult == vk::Timeout)
            return VkTimeout;
        if (waitResult != vk::Success)
            return VkErrorInitializationFailed;
    }

    const vk::Semaphore acquireSignal = reinterpret_cast<vk::Semaphore>(signalSemaphore);
    const vk::SubmitInfo submit{
        .sType = vk::StructureTypeSubmitInfo,
        .pNext = nullptr,
        .waitSemaphoreCount = 0u,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 0u,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = acquireSignal ? 1u : 0u,
        .pSignalSemaphores = acquireSignal ? &acquireSignal : nullptr,
    };

    vk::Result submitResult = vk::Success;
    {
        std::lock_guard<std::mutex> queueLock(virtualAcquireQueueMutex());
        submitResult = deviceContext.queueSubmit(
            reinterpret_cast<vk::Queue>(deviceContext.acquireQueue),
            1u, &submit, reinterpret_cast<vk::Fence>(signalFence));
    }
    if (submitResult != vk::Success) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(swapchain);
        if (it != capture.swapchains.end() && selected < it->second.virtualSlots.size()) {
            auto& slot = it->second.virtualSlots[selected];
            slot.acquired = false;
            slot.releaseSemaphores = selectedReleaseSemaphores;
            slot.releaseValues = selectedReleaseValues;
        }
        return submitResult;
    }

    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(swapchain);
        if (it != capture.swapchains.end()) {
            it->second.latestAcquiredImageIndex = selected;
            ++it->second.acquireSerial;
        }
    }
    *imageIndex = selected;
    return VkSuccess;
}

[[nodiscard]] bool legacyOpticalFlowQueueRoleRequestedSnapshot()
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    return capture.legacyOpticalFlowQueueRoleRequested;
}

void removeAntiLagInputFromOrder(DeviceRecord& record, std::uint64_t frameIndex);

[[nodiscard]] bool rememberAntiLagInput(DeviceRecord& record, const AntiLagFrameRecord& frame)
{
    // VK_AMD_anti_lag defines one INPUT point for each frame. Streamline/PCL
    // can surface the same SimulationStart token more than once through an
    // interposer stack; forwarding every duplicate makes the driver's pacing
    // sleep run repeatedly for one source frame. Keep the first INPUT contract
    // authoritative until its matching PRESENT consumes it.
    AntiLagFrameRecord& slot = record.antiLagFrames[frame.frameIndex % kAntiLagFrameHistory];
    if (slot.valid && slot.frameIndex == frame.frameIndex)
        return false;

    removeAntiLagInputFromOrder(record, frame.frameIndex);
    slot = frame;

    if (record.antiLagInputCount == kAntiLagFrameHistory) {
        record.antiLagInputHead = (record.antiLagInputHead + 1u) % kAntiLagFrameHistory;
        --record.antiLagInputCount;
    }
    const std::size_t tail = (record.antiLagInputHead + record.antiLagInputCount) % kAntiLagFrameHistory;
    record.antiLagInputOrder[tail] = frame.frameIndex;
    ++record.antiLagInputCount;
    return true;
}

void removeAntiLagInputFromOrder(DeviceRecord& record, const std::uint64_t frameIndex)
{
    for (std::size_t i = 0; i < record.antiLagInputCount; ++i) {
        const std::size_t pos = (record.antiLagInputHead + i) % kAntiLagFrameHistory;
        if (record.antiLagInputOrder[pos] != frameIndex) {
            continue;
        }
        for (std::size_t j = i; j + 1u < record.antiLagInputCount; ++j) {
            const std::size_t from = (record.antiLagInputHead + j + 1u) % kAntiLagFrameHistory;
            const std::size_t to = (record.antiLagInputHead + j) % kAntiLagFrameHistory;
            record.antiLagInputOrder[to] = record.antiLagInputOrder[from];
        }
        --record.antiLagInputCount;
        return;
    }
}

[[nodiscard]] bool selectAntiLagPresentFrame(DeviceRecord& record, AntiLagFrameRecord& outFrame)
{
    if (record.pendingPresentCount != 0u) {
        const std::uint64_t frameIndex = record.pendingPresentFrames[record.pendingPresentHead];
        record.pendingPresentHead = (record.pendingPresentHead + 1u) % kAntiLagFrameHistory;
        --record.pendingPresentCount;
        AntiLagFrameRecord& slot = record.antiLagFrames[frameIndex % kAntiLagFrameHistory];
        if (slot.valid && slot.frameIndex == frameIndex) {
            outFrame = slot;
            slot.valid = false;
            if (outFrame.controlTransition)
                record.antiLagDisablePairPending = false;
            removeAntiLagInputFromOrder(record, frameIndex);
            return true;
        }
        // An explicit PCL present marker is authoritative. If it cannot be
        // paired, do not substitute an unrelated input frame on this present.
        return false;
    }

    while (record.antiLagInputCount != 0u) {
        const std::uint64_t frameIndex = record.antiLagInputOrder[record.antiLagInputHead];
        record.antiLagInputHead = (record.antiLagInputHead + 1u) % kAntiLagFrameHistory;
        --record.antiLagInputCount;
        AntiLagFrameRecord& slot = record.antiLagFrames[frameIndex % kAntiLagFrameHistory];
        if (slot.valid && slot.frameIndex == frameIndex) {
            outFrame = slot;
            slot.valid = false;
            if (outFrame.controlTransition)
                record.antiLagDisablePairPending = false;
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr bool vulkanApiAtLeast(const std::uint32_t apiVersion, const std::uint32_t requiredMinor) noexcept
{
    const std::uint32_t major = (apiVersion >> 22u) & 0x7fu;
    const std::uint32_t minor = (apiVersion >> 12u) & 0x3ffu;
    return major > 1u || (major == 1u && minor >= requiredMinor);
}

[[nodiscard]] bool extensionAlreadyEnabled(const VkInstanceCreateInfoLoose& createInfo, const char* extension)
{
    if (createInfo.ppEnabledExtensionNames == nullptr || extension == nullptr) {
        return false;
    }
    for (std::uint32_t index = 0; index < createInfo.enabledExtensionCount; ++index) {
        const char* name = createInfo.ppEnabledExtensionNames[index];
        if (name != nullptr && std::strcmp(name, extension) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool looksLikeStreamlineVulkanAdapterProbe(const VkInstanceCreateInfoLoose& createInfo) noexcept
{
    const auto* app = createInfo.pApplicationInfo;
    if (app == nullptr || app->apiVersion != kVkApiVersion11 ||
        app->pApplicationName != nullptr || app->pEngineName != nullptr ||
        createInfo.enabledLayerCount != 0u || createInfo.enabledExtensionCount != 2u) {
        return false;
    }
    return extensionAlreadyEnabled(createInfo, kVkKhrSurfaceExtension) &&
           extensionAlreadyEnabled(createInfo, kVkKhrWin32SurfaceExtension);
}

[[nodiscard]] bool extensionAlreadyEnabled(const VkDeviceCreateInfoLoose& createInfo, const char* extension)
{
    if (createInfo.ppEnabledExtensionNames == nullptr || extension == nullptr) {
        return false;
    }
    for (std::uint32_t index = 0; index < createInfo.enabledExtensionCount; ++index) {
        const char* name = createInfo.ppEnabledExtensionNames[index];
        if (name != nullptr && std::strcmp(name, extension) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool hasInstanceExtension(const Dispatch& dispatch, const char* required)
{
    if (dispatch.enumerateInstanceExtensionProperties == nullptr || required == nullptr) {
        return false;
    }
    std::uint32_t count = 0u;
    if (dispatch.enumerateInstanceExtensionProperties(nullptr, &count, nullptr) != VkSuccess || count == 0u) {
        return false;
    }
    std::vector<VkExtensionPropertiesLoose> properties(count);
    const VkResult result = dispatch.enumerateInstanceExtensionProperties(nullptr, &count, properties.data());
    if (result != VkSuccess && result != VkIncomplete) {
        return false;
    }
    const std::size_t valid = std::min<std::size_t>(properties.size(), count);
    return std::any_of(properties.begin(), properties.begin() + valid, [required](const auto& property) {
        return std::strcmp(property.extensionName, required) == 0;
    });
}

[[nodiscard]] bool hasDeviceExtension(
    const Dispatch& dispatch,
    VkPhysicalDevice physicalDevice,
    const char* required)
{
    if (dispatch.enumerateDeviceExtensionProperties == nullptr || physicalDevice == nullptr || required == nullptr) {
        return false;
    }

    std::uint32_t count = 0;
    if (dispatch.enumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) != VkSuccess || count == 0) {
        return false;
    }
    std::vector<VkExtensionPropertiesLoose> properties(count);
    if (dispatch.enumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, properties.data()) != VkSuccess) {
        return false;
    }
    const std::size_t valid = std::min<std::size_t>(properties.size(), count);
    return std::any_of(properties.begin(), properties.begin() + valid, [required](const auto& property) {
        return std::strcmp(property.extensionName, required) == 0;
    });
}

struct VulkanSupport {
    std::uint32_t apiVersion = 0;
    bool vulkan12 = false;
    bool vulkan13 = false;
    bool coreFeatures = false;
    bool float16 = false;
    bool int8 = false;
    bool integerDot = false;
    bool integerDotExtension = false;
    bool computeShaderDerivativesExtension = false;
    bool computeDerivativeGroupLinear = false;
    bool packedSignedDotAccelerated = false;
    bool packedSignedDotAccSatAccelerated = false;
    bool packedUnsignedDotAccelerated = false;
    bool subgroupQuad = false;
    bool subgroupSizeControlExtension = false;
    bool subgroupSizeControl = false;
    bool subgroup32 = false;
    bool subgroup64 = false;
    std::uint32_t minSubgroupSize = 0;
    std::uint32_t maxSubgroupSize = 0;
    bool timelineSemaphore = false;
    bool antiLagExtension = false;
    bool antiLagFeature = false;
    bool swapchainMaintenance1KhrExtension = false;
    bool swapchainMaintenance1ExtExtension = false;
    bool swapchainMaintenance1Feature = false;
    bool presentTimingExtension = false;
    bool presentId2Extension = false;
    bool calibratedTimestampsKhrExtension = false;
    bool calibratedTimestampsExtExtension = false;
    bool presentTimingFeature = false;
    bool presentAtRelativeTimeFeature = false;
};

[[nodiscard]] VulkanSupport querySupport(
    const Dispatch& dispatch,
    VkPhysicalDevice physicalDevice,
    const bool hostMayUseCore13Dot,
    const bool antiLagRequested,
    const bool frameGenerationRequested,
    const bool profilerGpuRequested)
{
    VulkanSupport support{};
    if (dispatch.getPhysicalDeviceProperties == nullptr ||
        dispatch.getPhysicalDeviceProperties2 == nullptr ||
        dispatch.getPhysicalDeviceFeatures2 == nullptr) {
        return support;
    }

    VkPhysicalDevicePropertiesLoose baseProperties{};
    dispatch.getPhysicalDeviceProperties(physicalDevice, &baseProperties);
    support.apiVersion = baseProperties.apiVersion;
    support.vulkan12 = vulkanApiAtLeast(support.apiVersion, 2u);
    support.vulkan13 = vulkanApiAtLeast(support.apiVersion, 3u);
    support.integerDotExtension = hasDeviceExtension(dispatch, physicalDevice, kVkKhrShaderIntegerDotProductExtension);
    support.computeShaderDerivativesExtension =
        hasDeviceExtension(dispatch, physicalDevice, kVkKhrComputeShaderDerivativesExtension);
    support.subgroupSizeControlExtension =
        hasDeviceExtension(dispatch, physicalDevice, kVkExtSubgroupSizeControlExtension);
    support.antiLagExtension = antiLagRequested && hasDeviceExtension(dispatch, physicalDevice, kVkAmdAntiLagExtension);
    support.swapchainMaintenance1KhrExtension = frameGenerationRequested &&
        hasDeviceExtension(dispatch, physicalDevice, kVkKhrSwapchainMaintenance1Extension);
    support.swapchainMaintenance1ExtExtension = frameGenerationRequested &&
        hasDeviceExtension(dispatch, physicalDevice, kVkExtSwapchainMaintenance1Extension);
    support.presentTimingExtension = frameGenerationRequested &&
        hasDeviceExtension(dispatch, physicalDevice, kVkExtPresentTimingExtension);
    // VK_EXT_present_timing has two device-extension dependencies in addition
    // to VK_KHR_get_surface_capabilities2 on the instance.  We do not use
    // present IDs or calibrated-timestamp queries in the relative-time path,
    // but Vulkan still requires their extensions to be enabled with the
    // present-timing extension.
    support.presentId2Extension = frameGenerationRequested &&
        hasDeviceExtension(dispatch, physicalDevice, kVkKhrPresentId2Extension);
    support.calibratedTimestampsKhrExtension = (frameGenerationRequested || profilerGpuRequested) &&
        hasDeviceExtension(dispatch, physicalDevice, kVkKhrCalibratedTimestampsExtension);
    // VK_EXT_calibrated_timestamps is the older spelling promoted to KHR.
    // AMD drivers can expose either contract, and the profiler backend already
    // resolves both command names. Keep this telemetry capability independent
    // of the optional present-timing WSI stack.
    support.calibratedTimestampsExtExtension = profilerGpuRequested &&
        hasDeviceExtension(dispatch, physicalDevice, kVkExtCalibratedTimestampsExtension);

    // A 1.3-capable physical device alone does not make the host's effective API
    // 1.3. Query the promoted core route only when the captured instance version
    // or the host's own Vulkan13 feature structure proves that core 1.3 is legal.
    const bool integerDotQueryable =
        support.integerDotExtension || (support.vulkan13 && hostMayUseCore13Dot);
    const bool subgroupSizeControlQueryable =
        (support.subgroupSizeControlExtension || (support.vulkan13 && hostMayUseCore13Dot));

    VkPhysicalDeviceShaderIntegerDotProductFeaturesLoose dot{};
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose computeDerivatives{};
    computeDerivatives.pNext = integerDotQueryable ? static_cast<void*>(&dot) : nullptr;
    VkPhysicalDeviceShaderFloat16Int8FeaturesLoose float16Int8{};
    float16Int8.pNext = support.computeShaderDerivativesExtension
        ? static_cast<void*>(&computeDerivatives)
        : integerDotQueryable ? static_cast<void*>(&dot) : nullptr;
    VkPhysicalDeviceSubgroupSizeControlFeaturesLoose subgroupSizeControl{};
    subgroupSizeControl.pNext = &float16Int8;
    void* fsr4FeatureChainHead = subgroupSizeControlQueryable
        ? static_cast<void*>(&subgroupSizeControl)
        : static_cast<void*>(&float16Int8);
    VkPhysicalDeviceTimelineSemaphoreFeaturesLoose timeline{};
    timeline.pNext = fsr4FeatureChainHead;
    VkPhysicalDeviceAntiLagFeaturesAMDLoose antiLag{};
    antiLag.pNext = frameGenerationRequested ? static_cast<void*>(&timeline) : fsr4FeatureChainHead;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesLoose swapchainMaintenance1{};
    swapchainMaintenance1.pNext = support.antiLagExtension
        ? static_cast<void*>(&antiLag)
        : frameGenerationRequested
            ? static_cast<void*>(&timeline)
            : fsr4FeatureChainHead;
    void* featureChainHead = frameGenerationRequested &&
            (support.swapchainMaintenance1KhrExtension || support.swapchainMaintenance1ExtExtension)
        ? static_cast<void*>(&swapchainMaintenance1)
        : support.antiLagExtension
            ? static_cast<void*>(&antiLag)
            : frameGenerationRequested
                ? static_cast<void*>(&timeline)
                : fsr4FeatureChainHead;
    VkPhysicalDevicePresentTimingFeaturesEXTLoose presentTiming{};
    if (support.presentTimingExtension) {
        presentTiming.pNext = featureChainHead;
        featureChainHead = &presentTiming;
    }
    VkPhysicalDeviceFeatures2Loose features{};
    features.pNext = featureChainHead;
    dispatch.getPhysicalDeviceFeatures2(physicalDevice, &features);

    support.coreFeatures =
        features.features.values[kVkShaderStorageImageExtendedFormatsFeatureIndex] != 0u &&
        features.features.values[kVkShaderStorageImageWriteWithoutFormatFeatureIndex] != 0u &&
        features.features.values[kVkShaderInt16FeatureIndex] != 0u;
    support.float16 = float16Int8.shaderFloat16 != 0u;
    support.int8 = float16Int8.shaderInt8 != 0u;
    support.integerDot = integerDotQueryable && dot.shaderIntegerDotProduct != 0u;
    support.computeDerivativeGroupLinear = support.computeShaderDerivativesExtension &&
        computeDerivatives.computeDerivativeGroupLinear != 0u;
    support.subgroupSizeControl = subgroupSizeControlQueryable && subgroupSizeControl.subgroupSizeControl != 0u;
    support.timelineSemaphore = frameGenerationRequested && timeline.timelineSemaphore != 0u;
    support.antiLagFeature = support.antiLagExtension && antiLag.antiLag != 0u;
    support.swapchainMaintenance1Feature = frameGenerationRequested &&
        (support.swapchainMaintenance1KhrExtension || support.swapchainMaintenance1ExtExtension) &&
        swapchainMaintenance1.swapchainMaintenance1 != 0u;
    support.presentTimingFeature = support.presentTimingExtension && presentTiming.presentTiming != 0u;
    support.presentAtRelativeTimeFeature = support.presentTimingFeature && presentTiming.presentAtRelativeTime != 0u;

    VkPhysicalDeviceShaderIntegerDotProductPropertiesLoose dotProperties{};
    VkPhysicalDeviceSubgroupSizeControlPropertiesLoose subgroupSizeControlProperties{};
    subgroupSizeControlProperties.pNext = integerDotQueryable ? static_cast<void*>(&dotProperties) : nullptr;
    VkPhysicalDeviceSubgroupPropertiesLoose subgroup{};
    subgroup.pNext = subgroupSizeControlQueryable
        ? static_cast<void*>(&subgroupSizeControlProperties)
        : integerDotQueryable ? static_cast<void*>(&dotProperties) : nullptr;
    VkPhysicalDeviceProperties2Loose properties{};
    properties.pNext = &subgroup;
    dispatch.getPhysicalDeviceProperties2(physicalDevice, &properties);

    support.subgroupQuad =
        (subgroup.supportedStages & VkShaderStageComputeBit) != 0u &&
        (subgroup.supportedOperations & VkSubgroupFeatureQuadBit) != 0u;
    support.packedSignedDotAccelerated =
        integerDotQueryable && dotProperties.integerDotProduct4x8BitPackedSignedAccelerated != 0u;
    support.packedSignedDotAccSatAccelerated = integerDotQueryable &&
        dotProperties.integerDotProductAccumulatingSaturating4x8BitPackedSignedAccelerated != 0u;
    support.packedUnsignedDotAccelerated =
        integerDotQueryable && dotProperties.integerDotProduct4x8BitPackedUnsignedAccelerated != 0u;
    if (subgroupSizeControlQueryable) {
        support.minSubgroupSize = subgroupSizeControlProperties.minSubgroupSize;
        support.maxSubgroupSize = subgroupSizeControlProperties.maxSubgroupSize;
        const bool computeRequiredSubgroupSupported =
            (subgroupSizeControlProperties.requiredSubgroupSizeStages & VkShaderStageComputeBit) != 0u;
        support.subgroup32 =
            support.subgroupSizeControl && computeRequiredSubgroupSupported &&
            support.minSubgroupSize <= 32u && support.maxSubgroupSize >= 32u;
        support.subgroup64 =
            support.subgroupSizeControl && computeRequiredSubgroupSupported &&
            support.minSubgroupSize <= 64u && support.maxSubgroupSize >= 64u;
    }
    return support;
}

[[nodiscard]] std::vector<VkQueueFamilyPropertiesLoose> queryQueueFamilies(
    const Dispatch& dispatch,
    VkPhysicalDevice physicalDevice)
{
    if (dispatch.getPhysicalDeviceQueueFamilyProperties == nullptr || physicalDevice == nullptr) {
        return {};
    }
    std::uint32_t count = 0;
    dispatch.getPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
    if (count == 0) {
        return {};
    }
    std::vector<VkQueueFamilyPropertiesLoose> properties(count);
    dispatch.getPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, properties.data());
    properties.resize(std::min<std::size_t>(properties.size(), count));
    return properties;
}

[[nodiscard]] std::uint32_t queueKind(const std::uint32_t familyFlags) noexcept
{
    if ((familyFlags & VkQueueGraphicsBit) != 0u) {
        return APUSR_VK_HOST_QUEUE_GRAPHICS;
    }
    if ((familyFlags & VkQueueComputeBit) != 0u) {
        return APUSR_VK_HOST_QUEUE_COMPUTE;
    }
    if ((familyFlags & VkQueueTransferBit) != 0u) {
        return APUSR_VK_HOST_QUEUE_TRANSFER;
    }
    return APUSR_VK_HOST_QUEUE_UNKNOWN;
}

[[nodiscard]] std::uint32_t queueScore(
    const QueueRecord& queue,
    const std::uint32_t preferredKind) noexcept
{
    if (preferredKind == APUSR_VK_HOST_QUEUE_GRAPHICS) {
        return (queue.familyFlags & VkQueueGraphicsBit) != 0u ? 100u : 0u;
    }
    if (preferredKind == APUSR_VK_HOST_QUEUE_COMPUTE) {
        if ((queue.familyFlags & VkQueueComputeBit) == 0u) {
            return 0u;
        }
        return (queue.familyFlags & VkQueueGraphicsBit) == 0u ? 120u : 100u;
    }
    if (preferredKind == APUSR_VK_HOST_QUEUE_TRANSFER) {
        if ((queue.familyFlags & VkQueueTransferBit) == 0u) {
            return 0u;
        }
        const bool dedicated = (queue.familyFlags & (VkQueueGraphicsBit | VkQueueComputeBit)) == 0u;
        return dedicated ? 120u : 100u;
    }

    if ((queue.familyFlags & VkQueueGraphicsBit) != 0u) {
        return 60u;
    }
    if ((queue.familyFlags & VkQueueComputeBit) != 0u) {
        return 40u;
    }
    if ((queue.familyFlags & VkQueueTransferBit) != 0u) {
        return 20u;
    }
    return 1u;
}

void rememberPhysicalDevices(VkInstance instance, const std::uint32_t count, const VkPhysicalDevice* devices)
{
    if (instance == nullptr || count == 0 || devices == nullptr) {
        return;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (devices[index] != nullptr) {
            capture.instanceByPhysicalDevice[devices[index]] = instance;
        }
    }
}

struct PhysicalDeviceInstanceContext {
    VkInstance instance = nullptr;
    bool instanceCaptured = false;
    std::uint32_t requestedApiVersion = 0;
    bool getPhysicalDeviceProperties2Enabled = false;
    bool surfaceCapabilities2Enabled = false;
    bool surfaceMaintenance1KhrEnabled = false;
    bool surfaceMaintenance1ExtEnabled = false;
};

[[nodiscard]] PhysicalDeviceInstanceContext instanceContextForPhysicalDevice(VkPhysicalDevice physicalDevice)
{
    PhysicalDeviceInstanceContext context{};
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto physicalIt = capture.instanceByPhysicalDevice.find(physicalDevice);
    if (physicalIt == capture.instanceByPhysicalDevice.end()) {
        return context;
    }

    context.instance = physicalIt->second;
    const auto instanceIt = capture.instances.find(context.instance);
    if (instanceIt != capture.instances.end()) {
        context.instanceCaptured = true;
        context.requestedApiVersion = instanceIt->second.requestedApiVersion;
        context.getPhysicalDeviceProperties2Enabled = instanceIt->second.getPhysicalDeviceProperties2Enabled;
        context.surfaceCapabilities2Enabled = instanceIt->second.surfaceCapabilities2Enabled;
        context.surfaceMaintenance1KhrEnabled = instanceIt->second.surfaceMaintenance1KhrEnabled;
        context.surfaceMaintenance1ExtEnabled = instanceIt->second.surfaceMaintenance1ExtEnabled;
    }
    return context;
}

void captureCreatedQueues(
    const Dispatch& dispatch,
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    const VkDeviceCreateInfoLoose& createInfo,
    const std::vector<VkQueueFamilyPropertiesLoose>& families,
    const std::uint32_t negotiationFlags,
    const bool antiLagEnabled,
    const bool swapchainMaintenance1Enabled,
    const bool presentTimingRelativeEnabled,
    const bool displayTimingProfilerOwned,
    const std::vector<ReservedQueue>& apusrFramegenQueues)
{
    if (device == nullptr || dispatch.getDeviceProcAddr == nullptr) {
        return;
    }

    const auto getDeviceQueue = reinterpret_cast<VkGetDeviceQueueFn>(
        dispatch.getDeviceProcAddr(device, "vkGetDeviceQueue"));
    const auto getDeviceQueue2 = reinterpret_cast<VkGetDeviceQueue2Fn>(
        dispatch.getDeviceProcAddr(device, "vkGetDeviceQueue2"));

    DeviceRecord record{};
    record.instance = instance;
    record.physicalDevice = physicalDevice;
    record.device = device;
    record.negotiationFlags = negotiationFlags;
    record.queuePresent = reinterpret_cast<VkQueuePresentKHRFn>(
        dispatch.getDeviceProcAddr(device, "vkQueuePresentKHR"));
    record.createSwapchain = reinterpret_cast<VkCreateSwapchainKHRFn>(
        dispatch.getDeviceProcAddr(device, "vkCreateSwapchainKHR"));
    record.destroySwapchain = reinterpret_cast<VkDestroySwapchainKHRFn>(
        dispatch.getDeviceProcAddr(device, "vkDestroySwapchainKHR"));
    record.getSwapchainImages = reinterpret_cast<VkGetSwapchainImagesKHRFn>(
        dispatch.getDeviceProcAddr(device, "vkGetSwapchainImagesKHR"));
    record.acquireNextImage = reinterpret_cast<VkAcquireNextImageKHRFn>(
        dispatch.getDeviceProcAddr(device, "vkAcquireNextImageKHR"));
    record.acquireNextImage2 = reinterpret_cast<VkAcquireNextImage2KHRFn>(
        dispatch.getDeviceProcAddr(device, "vkAcquireNextImage2KHR"));
    if (swapchainMaintenance1Enabled) {
        record.releaseSwapchainImages = reinterpret_cast<VkReleaseSwapchainImagesKHRFn>(
            dispatch.getDeviceProcAddr(device, "vkReleaseSwapchainImagesKHR"));
        if (record.releaseSwapchainImages == nullptr) {
            record.releaseSwapchainImages = reinterpret_cast<VkReleaseSwapchainImagesKHRFn>(
                dispatch.getDeviceProcAddr(device, "vkReleaseSwapchainImagesEXT"));
        }
        record.swapchainMaintenance1Enabled = record.releaseSwapchainImages != nullptr;
    }
    record.presentTimingRelativeEnabled = presentTimingRelativeEnabled;
    if (presentTimingRelativeEnabled) {
        record.getSwapchainTimeDomainProperties = reinterpret_cast<VkGetSwapchainTimeDomainPropertiesEXTFn>(
            dispatch.getDeviceProcAddr(device, "vkGetSwapchainTimeDomainPropertiesEXT"));
        record.presentTimingRelativeEnabled = record.getSwapchainTimeDomainProperties != nullptr;
    }
    record.queueSubmit = reinterpret_cast<vk::PfnQueueSubmit>(
        dispatch.getDeviceProcAddr(device, "vkQueueSubmit"));
    record.memoryBudgetEnabled = extensionAlreadyEnabled(createInfo, kVkExtMemoryBudgetExtension);
    if (record.memoryBudgetEnabled && dispatch.getInstanceProcAddr != nullptr) {
        record.getPhysicalDeviceMemoryProperties2 = reinterpret_cast<vk::PfnGetPhysicalDeviceMemoryProperties2>(
            dispatch.getInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2"));
        if (record.getPhysicalDeviceMemoryProperties2 == nullptr) {
            record.getPhysicalDeviceMemoryProperties2 = reinterpret_cast<vk::PfnGetPhysicalDeviceMemoryProperties2>(
                dispatch.getInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
        }
        if (record.getPhysicalDeviceMemoryProperties2 == nullptr)
            record.memoryBudgetEnabled = false;
    }
    record.displayTimingProfilerOwned = displayTimingProfilerOwned;
    if (record.displayTimingProfilerOwned) {
        record.getPastPresentationTiming = reinterpret_cast<VkGetPastPresentationTimingGOOGLEFn>(
            dispatch.getDeviceProcAddr(device, "vkGetPastPresentationTimingGOOGLE"));
        if (record.getPastPresentationTiming == nullptr)
            record.displayTimingProfilerOwned = false;
    }
    if (antiLagEnabled) {
        record.antiLagUpdate = reinterpret_cast<VkAntiLagUpdateAMDFn>(
            dispatch.getDeviceProcAddr(device, "vkAntiLagUpdateAMD"));
        if (record.antiLagUpdate != nullptr) {
            record.negotiationFlags |= APUSR_VK_HOST_ANTI_LAG;
            record.antiLagProfileStream = apusr::profile::registerStream(apusr::profile::StreamDesc{
                .backend = apusr::profile::BackendApi::Vulkan,
                .component = apusr::profile::Component::Frontend,
                .name = "vulkan.antilag",
                .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                    apusr::profile::StreamCapabilityCounters,
                .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(device)),
            });
            if (record.antiLagProfileStream && apusr::profile::cpuEnabled()) {
                apusr::profile::recordCounter(apusr::profile::Counter{
                    .stream = record.antiLagProfileStream,
                    .name = "antilag.available",
                    .value = 1u,
                });
            }
        }
    }

    if (createInfo.pQueueCreateInfos != nullptr) {
        for (std::uint32_t createIndex = 0; createIndex < createInfo.queueCreateInfoCount; ++createIndex) {
            const VkDeviceQueueCreateInfoLoose& requested = createInfo.pQueueCreateInfos[createIndex];
            const std::uint32_t familyFlags = requested.queueFamilyIndex < families.size()
                ? families[requested.queueFamilyIndex].queueFlags
                : 0u;

            for (std::uint32_t queueIndex = 0; queueIndex < requested.queueCount; ++queueIndex) {
                VkQueue queue = nullptr;
                if (requested.flags == 0u && getDeviceQueue != nullptr) {
                    getDeviceQueue(device, requested.queueFamilyIndex, queueIndex, &queue);
                } else if (getDeviceQueue2 != nullptr) {
                    VkDeviceQueueInfo2Loose info{};
                    info.flags = requested.flags;
                    info.queueFamilyIndex = requested.queueFamilyIndex;
                    info.queueIndex = queueIndex;
                    getDeviceQueue2(device, &info, &queue);
                }

                if (queue == nullptr) {
                    apusr::host_discovery::eventf(
                        "vulkan", "queue.capture_failed",
                        "device=%p family=%u index=%u create_flags=0x%x get_queue=%u get_queue2=%u",
                        device,
                        requested.queueFamilyIndex,
                        queueIndex,
                        requested.flags,
                        getDeviceQueue != nullptr ? 1u : 0u,
                        getDeviceQueue2 != nullptr ? 1u : 0u);
                    continue;
                }

                record.queues.push_back(QueueRecord{
                    .queue = queue,
                    .familyIndex = requested.queueFamilyIndex,
                    .queueIndex = queueIndex,
                    .familyFlags = familyFlags,
                    .queueCreateFlags = requested.flags,
                    .kind = queueKind(familyFlags),
                     .ownershipFlags = [&]() noexcept {
                        for (const ReservedQueue& reserved : apusrFramegenQueues) {
                            if (reserved.familyIndex == requested.queueFamilyIndex && reserved.queueIndex == queueIndex)
                                return APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR | reserved.role;
                        }
                        return 0u;
                    }(),
                });
                const std::uint32_t ownershipFlags = record.queues.back().ownershipFlags;
                apusr::host_discovery::eventf(
                    "vulkan", "queue.capture",
                    "device=%p queue=%p family=%u index=%u family_flags=0x%x create_flags=0x%x kind=%u apusr_owned=%u apusr_roles=0x%x",
                    device,
                    queue,
                    requested.queueFamilyIndex,
                    queueIndex,
                    familyFlags,
                    requested.flags,
                    queueKind(familyFlags),
                    (ownershipFlags & APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR) != 0u ? 1u : 0u,
                    ownershipFlags & ~APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR);
            }
        }
    }

    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    for (const QueueRecord& queue : record.queues) {
        if (queue.queue != nullptr) {
            capture.deviceByQueue[queue.queue] = device;
            VkQueue expected = nullptr;
            (void)g_fastCapturedPresentQueue.compare_exchange_strong(
                expected, queue.queue, std::memory_order_release, std::memory_order_relaxed);
        }
    }
    capture.devices[device] = std::move(record);
}

} // namespace

void configureDispatch(const Dispatch& dispatch)
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.dispatch = dispatch;
    capture.dispatchConfigured =
        dispatch.createInstance != nullptr &&
        dispatch.createDevice != nullptr &&
        dispatch.getInstanceProcAddr != nullptr &&
        dispatch.getDeviceProcAddr != nullptr &&
        dispatch.getPhysicalDeviceFeatures2 != nullptr &&
        dispatch.getPhysicalDeviceProperties != nullptr &&
        dispatch.getPhysicalDeviceProperties2 != nullptr &&
        dispatch.enumerateDeviceExtensionProperties != nullptr &&
        dispatch.getPhysicalDeviceQueueFamilyProperties != nullptr;
}

bool dispatchReady()
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    return capture.dispatchConfigured;
}

VkResult createInstance(
    const VkInstanceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkInstance* instance)
{
    const Dispatch dispatch = dispatchSnapshot();
    if (dispatch.createInstance == nullptr || createInfo == nullptr || instance == nullptr) {
        if (instance != nullptr) {
            *instance = nullptr;
        }
        return VkErrorInitializationFailed;
    }

    const VkApplicationInfoLoose* appInfoForLog = createInfo->pApplicationInfo;
    apusr::host_discovery::eventf(
        "vulkan", "instance.create_begin",
        "create_info=%p requested_api=0x%08x app=%s engine=%s instance_extensions=%u layers=%u",
        createInfo,
        appInfoForLog != nullptr ? appInfoForLog->apiVersion : 0u,
        appInfoForLog != nullptr && appInfoForLog->pApplicationName != nullptr ? appInfoForLog->pApplicationName : "",
        appInfoForLog != nullptr && appInfoForLog->pEngineName != nullptr ? appInfoForLog->pEngineName : "",
        createInfo->enabledExtensionCount, createInfo->enabledLayerCount);

    // Preserve the host application's VkInstanceCreateInfo exactly. An earlier
    // frame-generation rewrite started opportunistically appending optional WSI
    // instance extensions here.  That crosses a particularly fragile loader/layer
    // boundary during process bring-up and regressed a previously working native Vulkan path
    // startup path to a pre-renderer crash.  Capability discovery is safe; mutating
    // the host's instance contract is not part of host capture.
    //
    // TODO(APUSR Vulkan FG): recover maintenance1/present-timing capabilities without
    // rewriting the application's instance extension list.  The standards-native
    // Vulkan-layer design is the clean place to negotiate extra instance extensions;
    // until that path is implemented/proven, these optional fast paths are enabled
    // only when the title already requested their instance dependencies.  Do not
    // remove this TODO merely because the fallback presenter works.
    const VkInstanceCreateInfoLoose forwarded = *createInfo;
    const bool framegenRequested = frameGenerationRequestedSnapshot();
    const bool getPhysicalDeviceProperties2Enabled =
        extensionAlreadyEnabled(*createInfo, kVkKhrGetPhysicalDeviceProperties2Extension);
    const bool surfaceAlreadyEnabled = extensionAlreadyEnabled(*createInfo, kVkKhrSurfaceExtension);
    const bool surfaceCapabilities2Enabled = surfaceAlreadyEnabled &&
        extensionAlreadyEnabled(*createInfo, kVkKhrGetSurfaceCapabilities2Extension);
    const bool surfaceMaintenance1KhrEnabled = surfaceCapabilities2Enabled &&
        extensionAlreadyEnabled(*createInfo, kVkKhrSurfaceMaintenance1Extension);
    const bool surfaceMaintenance1ExtEnabled = surfaceCapabilities2Enabled &&
        extensionAlreadyEnabled(*createInfo, kVkExtSurfaceMaintenance1Extension);

    if (framegenRequested && !surfaceCapabilities2Enabled) {
        apusr::host_discovery::event(
            "vulkan", "framegen.present_timing_surface_query_unavailable",
            "host did not enable VK_KHR_get_surface_capabilities2; precise WSI timing remains unavailable on the Detours capture path");
    }
    if (framegenRequested && !surfaceMaintenance1KhrEnabled && !surfaceMaintenance1ExtEnabled) {
        apusr::host_discovery::event(
            "vulkan", "framegen.surface_maintenance1_unavailable",
            "host did not enable a matching surface_maintenance1 instance dependency; direct WSI release remains unavailable on the Detours capture path");
    }

    const bool streamlineAdapterProbe = looksLikeStreamlineVulkanAdapterProbe(*createInfo);
    VkResult result = dispatch.createInstance(&forwarded, allocator, instance);
    std::uint32_t effectiveApiVersion = createInfo->pApplicationInfo != nullptr
        ? createInfo->pApplicationInfo->apiVersion
        : 0u;

    // Streamline 2.12's Vulkan adapter-correlation helper still creates a
    // transient Vulkan-1.1 instance directly from vulkan-1.dll, while its
    // interposer path normalizes application instances to Vulkan 1.3. The host
    // trace shows the AMD loader rejecting only that transient 1.1 probe with
    // VK_ERROR_INCOMPATIBLE_DRIVER and accepting the immediately following
    // 1.3 probe. Retry only this exact Streamline probe shape, only after the
    // loader has already rejected the original contract. Do not rewrite normal
    // application VkInstanceCreateInfo.
    //
    // TODO(Streamline Vulkan probe): remove this narrow compatibility retry if
    // a future Streamline revision stops issuing the legacy 1.1 adapter probe
    // or AMD's loader accepts it consistently. This is not the general Vulkan
    // instance-negotiation mechanism.
    if (result == VkErrorIncompatibleDriver && streamlineAdapterProbe) {
        VkApplicationInfoLoose retryApp = *createInfo->pApplicationInfo;
        retryApp.apiVersion = kVkApiVersion13;
        VkInstanceCreateInfoLoose retryInfo = forwarded;
        retryInfo.pApplicationInfo = &retryApp;
        *instance = nullptr;
        const VkResult retryResult = dispatch.createInstance(&retryInfo, allocator, instance);
        apusr::host_discovery::eventf(
            "vulkan", "streamline.adapter_probe.retry",
            "original_api=0x%08x retry_api=0x%08x original_result=%d retry_result=%d",
            kVkApiVersion11, kVkApiVersion13, result, retryResult);
        result = retryResult;
        if (result == VkSuccess) {
            effectiveApiVersion = kVkApiVersion13;
        }
    }

    if (result == VkSuccess && *instance != nullptr) {
        const std::uint32_t requestedApi = effectiveApiVersion;
        State& capture = state();
        {
            std::lock_guard<std::mutex> lock(capture.mutex);
            capture.instances[*instance] = InstanceRecord{
                .instance = *instance,
                .requestedApiVersion = requestedApi,
                .getPhysicalDeviceProperties2Enabled = getPhysicalDeviceProperties2Enabled,
                .surfaceCapabilities2Enabled = surfaceCapabilities2Enabled,
                .surfaceMaintenance1KhrEnabled = surfaceMaintenance1KhrEnabled,
                .surfaceMaintenance1ExtEnabled = surfaceMaintenance1ExtEnabled,
            };
        }
        apusr::host_discovery::eventf(
            "vulkan", "instance.created",
            "instance=%p requested_api=0x%08x get_physical_device_properties2=%u surface_capabilities2=%u surface_maintenance1_khr=%u surface_maintenance1_ext=%u",
            *instance, requestedApi, getPhysicalDeviceProperties2Enabled ? 1u : 0u, surfaceCapabilities2Enabled ? 1u : 0u,
            surfaceMaintenance1KhrEnabled ? 1u : 0u, surfaceMaintenance1ExtEnabled ? 1u : 0u);
    } else {
        apusr::host_discovery::eventf("vulkan", "instance.create_failed", "result=%d", result);
    }
    return result;
}

VkResult enumeratePhysicalDevices(VkInstance instance, std::uint32_t* count, VkPhysicalDevice* devices)
{
    const Dispatch dispatch = dispatchSnapshot();
    if (dispatch.enumeratePhysicalDevices == nullptr) {
        return VkErrorInitializationFailed;
    }
    const VkResult result = dispatch.enumeratePhysicalDevices(instance, count, devices);
    if ((result == VkSuccess || result == VkIncomplete) && count != nullptr && devices != nullptr) {
        rememberPhysicalDevices(instance, *count, devices);
        apusr::host_discovery::eventf(
            "vulkan", "physical_devices.enumerated",
            "instance=%p count=%u result=%d", instance, *count, result);
        if (dispatch.getPhysicalDeviceProperties != nullptr) {
            const std::uint32_t loggedCount = std::min<std::uint32_t>(*count, 4u);
            for (std::uint32_t index = 0; index < loggedCount; ++index) {
                VkPhysicalDevicePropertiesLoose properties{};
                dispatch.getPhysicalDeviceProperties(devices[index], &properties);
                properties.deviceName[sizeof(properties.deviceName) - 1] = '\0';
                apusr::host_discovery::eventf(
                    "vulkan", "physical_device.identity",
                    "instance=%p index=%u physical_device=%p api=0x%08x vendor=0x%04x device=0x%04x type=%u name=%s",
                    instance, index, devices[index], properties.apiVersion, properties.vendorID,
                    properties.deviceID, properties.deviceType, properties.deviceName);
            }
        }
    }
    return result;
}

VkResult enumeratePhysicalDeviceGroups(
    VkInstance instance,
    std::uint32_t* count,
    VkPhysicalDeviceGroupPropertiesLoose* groups)
{
    const Dispatch dispatch = dispatchSnapshot();
    if (dispatch.enumeratePhysicalDeviceGroups == nullptr) {
        return VkErrorInitializationFailed;
    }
    const VkResult result = dispatch.enumeratePhysicalDeviceGroups(instance, count, groups);
    if ((result == VkSuccess || result == VkIncomplete) && count != nullptr && groups != nullptr) {
        for (std::uint32_t groupIndex = 0; groupIndex < *count; ++groupIndex) {
            const std::uint32_t deviceCount = std::min(groups[groupIndex].physicalDeviceCount, VkMaxDeviceGroupSize);
            rememberPhysicalDevices(instance, deviceCount, groups[groupIndex].physicalDevices);
        }
        apusr::host_discovery::eventf(
            "vulkan", "physical_device_groups.enumerated",
            "instance=%p groups=%u result=%d", instance, *count, result);
    }
    return result;
}

VkResult createDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfoLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDevice* device)
{
    apusr::host_discovery::eventf(
        "vulkan", "device.create_begin",
        "physical_device=%p create_info=%p queues=%u",
        physicalDevice,
        createInfo,
        createInfo != nullptr ? createInfo->queueCreateInfoCount : 0u);

    const Dispatch dispatch = dispatchSnapshot();
    if (dispatch.createDevice == nullptr || physicalDevice == nullptr || createInfo == nullptr || device == nullptr) {
        if (device != nullptr) {
            *device = nullptr;
        }
        apusr::host_discovery::event("vulkan", "device.create_rejected", "reason=invalid_capture_boundary");
        return VkErrorInitializationFailed;
    }

    // Device creation is cold-path work. Serialize only the short interval in
    // which APUSR temporarily enables known host-owned feature members and then
    // restores them after the loader consumes VkDeviceCreateInfo.
    std::lock_guard<std::mutex> createLock(createMutex());

    const PhysicalDeviceInstanceContext instanceContext = instanceContextForPhysicalDevice(physicalDevice);
    if (instanceContext.instanceCaptured && !vulkanApiAtLeast(instanceContext.requestedApiVersion, 2u)) {
        *device = nullptr;
        apusr::host_discovery::eventf(
            "vulkan", "device.create_rejected",
            "reason=host_instance_api_below_vulkan12 instance=%p requested_api=0x%08x",
            instanceContext.instance,
            instanceContext.requestedApiVersion);
        return VkErrorFeatureNotPresent;
    }
    if (!instanceContext.instanceCaptured) {
        apusr::host_discovery::eventf(
            "vulkan", "device.instance_context_unknown",
            "physical_device=%p mapped_instance=%p",
            physicalDevice,
            instanceContext.instance);
    }

    VkDeviceCreateInfoLoose forwarded = *createInfo;
    auto* features2 = static_cast<VkPhysicalDeviceFeatures2Loose*>(nullptr);
    auto* vulkan12 = static_cast<VkPhysicalDeviceVulkan12FeaturesPrefix*>(nullptr);
    auto* float16Int8 = static_cast<VkPhysicalDeviceShaderFloat16Int8FeaturesLoose*>(nullptr);
    auto* vulkan13 = static_cast<VkPhysicalDeviceVulkan13FeaturesPrefix*>(nullptr);
    auto* integerDot = static_cast<VkPhysicalDeviceShaderIntegerDotProductFeaturesLoose*>(nullptr);
    auto* computeDerivatives = static_cast<VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose*>(nullptr);
    auto* subgroupSizeControl = static_cast<VkPhysicalDeviceSubgroupSizeControlFeaturesLoose*>(nullptr);
    auto* timelineSemaphore = static_cast<VkPhysicalDeviceTimelineSemaphoreFeaturesLoose*>(nullptr);
    auto* antiLag = static_cast<VkPhysicalDeviceAntiLagFeaturesAMDLoose*>(nullptr);
    auto* swapchainMaintenance1 = static_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesLoose*>(nullptr);
    auto* presentTiming = static_cast<VkPhysicalDevicePresentTimingFeaturesEXTLoose*>(nullptr);
    auto* deviceGroup = static_cast<const VkDeviceGroupDeviceCreateInfoLoose*>(nullptr);
    std::uint32_t features2Count = 0;
    std::uint32_t vulkan12Count = 0;
    std::uint32_t float16Int8Count = 0;
    std::uint32_t vulkan13Count = 0;
    std::uint32_t integerDotCount = 0;
    std::uint32_t computeDerivativesCount = 0;
    std::uint32_t subgroupSizeControlCount = 0;
    std::uint32_t timelineSemaphoreCount = 0;
    std::uint32_t antiLagCount = 0;
    std::uint32_t swapchainMaintenance1Count = 0;
    std::uint32_t presentTimingCount = 0;
    std::uint32_t deviceGroupCount = 0;
    std::uint32_t chainNodes = 0;

    for (auto* node = reinterpret_cast<const VkBaseInStructureLoose*>(createInfo->pNext);
         node != nullptr && chainNodes < 128u;
         node = node->pNext, ++chainNodes) {
        switch (node->sType) {
        case VkStructureTypePhysicalDeviceFeatures2:
            ++features2Count;
            features2 = reinterpret_cast<VkPhysicalDeviceFeatures2Loose*>(const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceVulkan12Features:
            ++vulkan12Count;
            vulkan12 = reinterpret_cast<VkPhysicalDeviceVulkan12FeaturesPrefix*>(const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceShaderFloat16Int8Features:
            ++float16Int8Count;
            float16Int8 = reinterpret_cast<VkPhysicalDeviceShaderFloat16Int8FeaturesLoose*>(const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceVulkan13Features:
            ++vulkan13Count;
            vulkan13 = reinterpret_cast<VkPhysicalDeviceVulkan13FeaturesPrefix*>(const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceShaderIntegerDotProductFeatures:
            ++integerDotCount;
            integerDot = reinterpret_cast<VkPhysicalDeviceShaderIntegerDotProductFeaturesLoose*>(const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceComputeShaderDerivativesFeaturesKhr:
            ++computeDerivativesCount;
            computeDerivatives = reinterpret_cast<VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose*>(
                const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceSubgroupSizeControlFeatures:
            ++subgroupSizeControlCount;
            subgroupSizeControl = reinterpret_cast<VkPhysicalDeviceSubgroupSizeControlFeaturesLoose*>(
                const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceTimelineSemaphoreFeatures:
            ++timelineSemaphoreCount;
            timelineSemaphore = reinterpret_cast<VkPhysicalDeviceTimelineSemaphoreFeaturesLoose*>(
                const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceAntiLagFeaturesAmd:
            ++antiLagCount;
            antiLag = reinterpret_cast<VkPhysicalDeviceAntiLagFeaturesAMDLoose*>(const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDeviceSwapchainMaintenance1FeaturesKhr:
            ++swapchainMaintenance1Count;
            swapchainMaintenance1 = reinterpret_cast<VkPhysicalDeviceSwapchainMaintenance1FeaturesLoose*>(
                const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypePhysicalDevicePresentTimingFeaturesExt:
            ++presentTimingCount;
            presentTiming = reinterpret_cast<VkPhysicalDevicePresentTimingFeaturesEXTLoose*>(
                const_cast<VkBaseInStructureLoose*>(node));
            break;
        case VkStructureTypeDeviceGroupDeviceCreateInfo:
            ++deviceGroupCount;
            deviceGroup = reinterpret_cast<const VkDeviceGroupDeviceCreateInfoLoose*>(node);
            break;
        default:
            break;
        }
    }

    const bool invalidFeatureChain =
        chainNodes >= 128u ||
        features2Count > 1u || vulkan12Count > 1u || float16Int8Count > 1u ||
        vulkan13Count > 1u || integerDotCount > 1u || computeDerivativesCount > 1u ||
        subgroupSizeControlCount > 1u || timelineSemaphoreCount > 1u ||
        antiLagCount > 1u || swapchainMaintenance1Count > 1u || presentTimingCount > 1u || deviceGroupCount > 1u ||
        (features2 != nullptr && createInfo->pEnabledFeatures != nullptr) ||
        (vulkan12 != nullptr && float16Int8 != nullptr) ||
        (vulkan13 != nullptr && integerDot != nullptr) ||
        (vulkan13 != nullptr && subgroupSizeControl != nullptr) ||
        (vulkan12 != nullptr && timelineSemaphore != nullptr);
    if (invalidFeatureChain) {
        *device = nullptr;
        apusr::host_discovery::eventf(
            "vulkan", "device.create_rejected",
            "reason=invalid_host_feature_chain features2=%u vulkan12=%u float16_int8=%u vulkan13=%u integer_dot=%u subgroup_size_control=%u timeline=%u anti_lag=%u swapchain_maintenance1=%u present_timing=%u device_group=%u legacy_core=%u nodes=%u",
            features2Count,
            vulkan12Count,
            float16Int8Count,
            vulkan13Count,
            integerDotCount,
            subgroupSizeControlCount,
            timelineSemaphoreCount,
            antiLagCount,
            swapchainMaintenance1Count,
            presentTimingCount,
            deviceGroupCount,
            createInfo->pEnabledFeatures != nullptr ? 1u : 0u,
            chainNodes);
        return VkErrorInitializationFailed;
    }

    const bool hostMayUseCore13Dot = instanceContext.instanceCaptured
        ? vulkanApiAtLeast(instanceContext.requestedApiVersion, 3u)
        : vulkan13 != nullptr;
    const bool antiLagRequested = antiLagRequestedSnapshot();
    const bool framegenRequested = frameGenerationRequestedSnapshot();
    const bool profilerGpuEnabled = apusr::profile::gpuEnabled();
    // Both KHR/EXT calibrated-timestamp device extensions depend on either
    // Vulkan 1.1 core physical-device-properties2 or the matching 1.0 instance
    // extension. Never mutate the host instance merely to make
    // profiler telemetry available.
    const bool profilerCalibrationInstanceDependencySatisfied =
        instanceContext.instanceCaptured &&
        (vulkanApiAtLeast(instanceContext.requestedApiVersion, 1u) ||
         instanceContext.getPhysicalDeviceProperties2Enabled);
    const bool profilerCalibrationCanRequest =
        profilerGpuEnabled && profilerCalibrationInstanceDependencySatisfied;
    const VulkanSupport support = querySupport(
        dispatch, physicalDevice, hostMayUseCore13Dot, antiLagRequested, framegenRequested, profilerCalibrationCanRequest);
    const bool useCore13Dot = support.vulkan13 && hostMayUseCore13Dot;
    const bool multiGpuDeviceGroup = deviceGroup != nullptr && deviceGroup->physicalDeviceCount > 1u;
    const bool antiLagCanEnable =
        antiLagRequested && support.antiLagExtension && support.antiLagFeature && !multiGpuDeviceGroup;
    const bool framegenTimelineCanEnable = framegenRequested && support.timelineSemaphore;
    const bool fsr4SubgroupSizeControlCanEnable = support.subgroup32 || support.subgroup64;
    const bool framegenSubgroup64CanEnable = framegenRequested && support.subgroup64;
    // VK_KHR/EXT_swapchain_maintenance1 is only legal when the matching
    // instance-side surface-maintenance extension was enabled. Prefer KHR,
    // but never advertise a device extension whose instance dependency is
    // absent.
    const bool swapchainMaintenance1KhrCanEnable = framegenRequested &&
        support.swapchainMaintenance1KhrExtension && support.swapchainMaintenance1Feature &&
        instanceContext.surfaceMaintenance1KhrEnabled;
    const bool swapchainMaintenance1ExtCanEnable = framegenRequested &&
        support.swapchainMaintenance1ExtExtension && support.swapchainMaintenance1Feature &&
        instanceContext.surfaceMaintenance1ExtEnabled;
    const bool swapchainMaintenance1CanEnable =
        swapchainMaintenance1KhrCanEnable || swapchainMaintenance1ExtCanEnable;
    const bool presentTimingRelativeCanEnable = framegenRequested &&
        instanceContext.surfaceCapabilities2Enabled &&
        support.presentTimingExtension && support.presentId2Extension &&
        support.calibratedTimestampsKhrExtension &&
        support.presentTimingFeature && support.presentAtRelativeTimeFeature;
    const char* swapchainMaintenance1Extension = swapchainMaintenance1KhrCanEnable
        ? kVkKhrSwapchainMaintenance1Extension
        : swapchainMaintenance1ExtCanEnable ? kVkExtSwapchainMaintenance1Extension : nullptr;
    const bool needsDotExtension = !useCore13Dot;
    if (!support.vulkan12 || !support.coreFeatures || !support.float16 || !support.int8 ||
        !support.integerDot || !support.packedSignedDotAccelerated ||
        (kFsr4RequiresPackedSignedAccSat && !support.packedSignedDotAccSatAccelerated) ||
        (framegenRequested && !support.packedUnsignedDotAccelerated) || !support.subgroupQuad ||
        !support.computeShaderDerivativesExtension || !support.computeDerivativeGroupLinear ||
        (needsDotExtension && !support.integerDotExtension)) {
        *device = nullptr;
        apusr::host_discovery::eventf(
            "vulkan", "device.create_rejected",
            "reason=required_native_fsr4_feature_missing api=0x%08x vulkan12=%u core=%u float16=%u int8=%u integer_dot=%u packed_i8_signed_accelerated=%u packed_i8_signed_accsat_accelerated=%u packed_i8_signed_accsat_required=%u packed_i8_unsigned_accelerated=%u integer_dot_extension=%u subgroup_quad=%u compute_derivatives_extension=%u derivative_group_linear=%u core13_dot=%u",
            support.apiVersion,
            support.vulkan12 ? 1u : 0u,
            support.coreFeatures ? 1u : 0u,
            support.float16 ? 1u : 0u,
            support.int8 ? 1u : 0u,
            support.integerDot ? 1u : 0u,
            support.packedSignedDotAccelerated ? 1u : 0u,
            support.packedSignedDotAccSatAccelerated ? 1u : 0u,
            kFsr4RequiresPackedSignedAccSat ? 1u : 0u,
            support.packedUnsignedDotAccelerated ? 1u : 0u,
            support.integerDotExtension ? 1u : 0u,
            support.subgroupQuad ? 1u : 0u,
            support.computeShaderDerivativesExtension ? 1u : 0u,
            support.computeDerivativeGroupLinear ? 1u : 0u,
            useCore13Dot ? 1u : 0u);
        return (!support.computeShaderDerivativesExtension || (needsDotExtension && !support.integerDotExtension))
            ? VkErrorExtensionNotPresent
            : VkErrorFeatureNotPresent;
    }

    VkPhysicalDeviceFeaturesLoose localCore{};
    std::array<VkBool32, 3> oldCore{};
    if (features2 != nullptr) {
        oldCore = {
            features2->features.values[kVkShaderStorageImageExtendedFormatsFeatureIndex],
            features2->features.values[kVkShaderStorageImageWriteWithoutFormatFeatureIndex],
            features2->features.values[kVkShaderInt16FeatureIndex],
        };
        features2->features.values[kVkShaderStorageImageExtendedFormatsFeatureIndex] = 1u;
        features2->features.values[kVkShaderStorageImageWriteWithoutFormatFeatureIndex] = 1u;
        features2->features.values[kVkShaderInt16FeatureIndex] = 1u;
    } else {
        if (createInfo->pEnabledFeatures != nullptr) {
            localCore = *createInfo->pEnabledFeatures;
        }
        localCore.values[kVkShaderStorageImageExtendedFormatsFeatureIndex] = 1u;
        localCore.values[kVkShaderStorageImageWriteWithoutFormatFeatureIndex] = 1u;
        localCore.values[kVkShaderInt16FeatureIndex] = 1u;
        forwarded.pEnabledFeatures = &localCore;
    }

    VkPhysicalDeviceShaderFloat16Int8FeaturesLoose localFloat16Int8{};
    std::array<VkBool32, 2> oldFloat16Int8{};
    if (vulkan12 != nullptr) {
        oldFloat16Int8 = {vulkan12->shaderFloat16, vulkan12->shaderInt8};
        vulkan12->shaderFloat16 = 1u;
        vulkan12->shaderInt8 = 1u;
    } else if (float16Int8 != nullptr) {
        oldFloat16Int8 = {float16Int8->shaderFloat16, float16Int8->shaderInt8};
        float16Int8->shaderFloat16 = 1u;
        float16Int8->shaderInt8 = 1u;
    } else {
        localFloat16Int8.shaderFloat16 = 1u;
        localFloat16Int8.shaderInt8 = 1u;
        localFloat16Int8.pNext = const_cast<void*>(forwarded.pNext);
        forwarded.pNext = &localFloat16Int8;
    }

    VkPhysicalDeviceShaderIntegerDotProductFeaturesLoose localDot{};
    VkBool32 oldDot = 0;
    bool modifiedVulkan13Dot = false;
    bool modifiedStandaloneDot = false;
    if (useCore13Dot && vulkan13 != nullptr) {
        oldDot = vulkan13->shaderIntegerDotProduct;
        vulkan13->shaderIntegerDotProduct = 1u;
        modifiedVulkan13Dot = true;
    } else if (integerDot != nullptr) {
        oldDot = integerDot->shaderIntegerDotProduct;
        integerDot->shaderIntegerDotProduct = 1u;
        modifiedStandaloneDot = true;
    } else {
        // The standalone structure is also the Vulkan 1.3 core structure for
        // this promoted feature, so it is valid without the extension when the
        // captured instance proves an effective 1.3 API.
        localDot.shaderIntegerDotProduct = 1u;
        localDot.pNext = const_cast<void*>(forwarded.pNext);
        forwarded.pNext = &localDot;
    }

    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHRLoose localComputeDerivatives{};
    VkBool32 oldComputeDerivativeGroupLinear = 0u;
    bool modifiedHostComputeDerivatives = false;
    if (computeDerivatives != nullptr) {
        oldComputeDerivativeGroupLinear = computeDerivatives->computeDerivativeGroupLinear;
        computeDerivatives->computeDerivativeGroupLinear = 1u;
        modifiedHostComputeDerivatives = true;
    } else {
        localComputeDerivatives.computeDerivativeGroupLinear = 1u;
        localComputeDerivatives.pNext = const_cast<void*>(forwarded.pNext);
        forwarded.pNext = &localComputeDerivatives;
    }

    VkPhysicalDeviceSubgroupSizeControlFeaturesLoose localSubgroupSizeControl{};
    VkBool32 oldSubgroupSizeControl = 0u;
    bool modifiedVulkan13SubgroupSizeControl = false;
    bool modifiedStandaloneSubgroupSizeControl = false;
    if (fsr4SubgroupSizeControlCanEnable) {
        if (vulkan13 != nullptr) {
            oldSubgroupSizeControl = vulkan13->subgroupSizeControl;
            vulkan13->subgroupSizeControl = 1u;
            modifiedVulkan13SubgroupSizeControl = true;
        } else if (subgroupSizeControl != nullptr) {
            oldSubgroupSizeControl = subgroupSizeControl->subgroupSizeControl;
            subgroupSizeControl->subgroupSizeControl = 1u;
            modifiedStandaloneSubgroupSizeControl = true;
        } else {
            localSubgroupSizeControl.subgroupSizeControl = 1u;
            localSubgroupSizeControl.pNext = const_cast<void*>(forwarded.pNext);
            forwarded.pNext = &localSubgroupSizeControl;
        }
    }

    VkPhysicalDeviceTimelineSemaphoreFeaturesLoose localTimelineSemaphore{};
    VkBool32 oldTimelineSemaphore = 0u;
    bool modifiedVulkan12TimelineSemaphore = false;
    bool modifiedStandaloneTimelineSemaphore = false;
    if (framegenTimelineCanEnable) {
        if (vulkan12 != nullptr) {
            oldTimelineSemaphore = vulkan12->timelineSemaphore;
            vulkan12->timelineSemaphore = 1u;
            modifiedVulkan12TimelineSemaphore = true;
        } else if (timelineSemaphore != nullptr) {
            oldTimelineSemaphore = timelineSemaphore->timelineSemaphore;
            timelineSemaphore->timelineSemaphore = 1u;
            modifiedStandaloneTimelineSemaphore = true;
        } else {
            localTimelineSemaphore.timelineSemaphore = 1u;
            localTimelineSemaphore.pNext = const_cast<void*>(forwarded.pNext);
            forwarded.pNext = &localTimelineSemaphore;
        }
    }

    if (framegenRequested && !framegenTimelineCanEnable) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.timeline_unavailable",
            "physical_device=%p api=0x%08x timeline_feature=%u",
            physicalDevice, support.apiVersion, support.timelineSemaphore ? 1u : 0u);
    }

    if (framegenRequested && !framegenSubgroup64CanEnable) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.subgroup64_unavailable",
            "physical_device=%p api=0x%08x subgroup_size_control=%u extension=%u min=%u max=%u",
            physicalDevice, support.apiVersion,
            support.subgroupSizeControl ? 1u : 0u,
            support.subgroupSizeControlExtension ? 1u : 0u,
            support.minSubgroupSize, support.maxSubgroupSize);
    }

    VkPhysicalDeviceSwapchainMaintenance1FeaturesLoose localSwapchainMaintenance1{};
    VkBool32 oldSwapchainMaintenance1 = 0u;
    bool modifiedHostSwapchainMaintenance1 = false;
    if (swapchainMaintenance1CanEnable) {
        if (swapchainMaintenance1 != nullptr) {
            oldSwapchainMaintenance1 = swapchainMaintenance1->swapchainMaintenance1;
            swapchainMaintenance1->swapchainMaintenance1 = 1u;
            modifiedHostSwapchainMaintenance1 = true;
        } else {
            localSwapchainMaintenance1.swapchainMaintenance1 = 1u;
            localSwapchainMaintenance1.pNext = const_cast<void*>(forwarded.pNext);
            forwarded.pNext = &localSwapchainMaintenance1;
        }
    }
    if (framegenRequested && !swapchainMaintenance1CanEnable) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.swapchain_maintenance1_unavailable",
            "physical_device=%p feature=%u khr_extension=%u ext_extension=%u surface_khr=%u surface_ext=%u",
            physicalDevice,
            support.swapchainMaintenance1Feature ? 1u : 0u,
            support.swapchainMaintenance1KhrExtension ? 1u : 0u,
            support.swapchainMaintenance1ExtExtension ? 1u : 0u,
            instanceContext.surfaceMaintenance1KhrEnabled ? 1u : 0u,
            instanceContext.surfaceMaintenance1ExtEnabled ? 1u : 0u);
    }

    VkPhysicalDevicePresentTimingFeaturesEXTLoose localPresentTiming{};
    std::array<VkBool32, 3u> oldPresentTiming{};
    bool modifiedHostPresentTiming = false;
    if (presentTimingRelativeCanEnable) {
        if (presentTiming != nullptr) {
            oldPresentTiming = {
                presentTiming->presentTiming,
                presentTiming->presentAtAbsoluteTime,
                presentTiming->presentAtRelativeTime,
            };
            presentTiming->presentTiming = 1u;
            presentTiming->presentAtRelativeTime = 1u;
            modifiedHostPresentTiming = true;
        } else {
            localPresentTiming.presentTiming = 1u;
            localPresentTiming.presentAtRelativeTime = 1u;
            localPresentTiming.pNext = const_cast<void*>(forwarded.pNext);
            forwarded.pNext = &localPresentTiming;
        }
    }
    if (framegenRequested && !presentTimingRelativeCanEnable) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.present_timing_relative_unavailable",
            "physical_device=%p extension=%u present_id2=%u calibrated_timestamps=%u feature=%u relative_feature=%u surface_caps2=%u",
            physicalDevice,
            support.presentTimingExtension ? 1u : 0u,
            support.presentId2Extension ? 1u : 0u,
            support.calibratedTimestampsKhrExtension ? 1u : 0u,
            support.presentTimingFeature ? 1u : 0u,
            support.presentAtRelativeTimeFeature ? 1u : 0u,
            instanceContext.surfaceCapabilities2Enabled ? 1u : 0u);
    }

    VkPhysicalDeviceAntiLagFeaturesAMDLoose localAntiLag{};
    VkBool32 oldAntiLag = 0;
    bool modifiedHostAntiLag = false;
    if (antiLagCanEnable) {
        if (antiLag != nullptr) {
            oldAntiLag = antiLag->antiLag;
            antiLag->antiLag = 1u;
            modifiedHostAntiLag = true;
        } else {
            localAntiLag.antiLag = 1u;
            localAntiLag.pNext = const_cast<void*>(forwarded.pNext);
            forwarded.pNext = &localAntiLag;
        }
    }

    if (antiLagRequested && !antiLagCanEnable) {
        apusr::host_discovery::eventf(
            "vulkan", "anti_lag.unavailable",
            "physical_device=%p extension=%u feature=%u multi_gpu_device_group=%u",
            physicalDevice,
            support.antiLagExtension ? 1u : 0u,
            support.antiLagFeature ? 1u : 0u,
            multiGpuDeviceGroup ? 1u : 0u);
    }

    std::vector<const char*> extensions;
    const bool dotExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkKhrShaderIntegerDotProductExtension);
    const bool dotExtensionAdded = needsDotExtension && !dotExtensionAlreadyEnabled;
    const bool computeDerivativesExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkKhrComputeShaderDerivativesExtension);
    const bool computeDerivativesExtensionAdded = !computeDerivativesExtensionAlreadyEnabled;
    const bool subgroupSizeControlUsesExtension = fsr4SubgroupSizeControlCanEnable && !useCore13Dot;
    const bool subgroupSizeControlExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkExtSubgroupSizeControlExtension);
    const bool subgroupSizeControlExtensionAdded =
        subgroupSizeControlUsesExtension && !subgroupSizeControlExtensionAlreadyEnabled;
    const bool antiLagExtensionAlreadyEnabled = extensionAlreadyEnabled(*createInfo, kVkAmdAntiLagExtension);
    const bool antiLagExtensionAdded = antiLagCanEnable && !antiLagExtensionAlreadyEnabled;
    const bool swapchainMaintenance1ExtensionAlreadyEnabled =
        swapchainMaintenance1Extension != nullptr && extensionAlreadyEnabled(*createInfo, swapchainMaintenance1Extension);
    const bool swapchainMaintenance1ExtensionAdded =
        swapchainMaintenance1CanEnable && swapchainMaintenance1Extension != nullptr &&
        !swapchainMaintenance1ExtensionAlreadyEnabled;
    const bool presentTimingExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkExtPresentTimingExtension);
    const bool presentId2ExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkKhrPresentId2Extension);
    const bool calibratedTimestampsKhrExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkKhrCalibratedTimestampsExtension);
    const bool calibratedTimestampsExtExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkExtCalibratedTimestampsExtension);
    const bool presentTimingExtensionAdded =
        presentTimingRelativeCanEnable && !presentTimingExtensionAlreadyEnabled;
    const bool presentId2ExtensionAdded =
        presentTimingRelativeCanEnable && !presentId2ExtensionAlreadyEnabled;
    // VK_EXT_present_timing specifically depends on the KHR calibration
    // extension, so keep that dependency exact. Profiling, however, can use
    // either the KHR spelling or its older EXT predecessor.
    const bool calibratedTimestampsKhrExtensionAddedForPresentTiming =
        presentTimingRelativeCanEnable && !calibratedTimestampsKhrExtensionAlreadyEnabled;
    const bool profilerCalibrationAlreadyEnabled =
        calibratedTimestampsKhrExtensionAlreadyEnabled ||
        calibratedTimestampsExtExtensionAlreadyEnabled ||
        calibratedTimestampsKhrExtensionAddedForPresentTiming;
    const char* profilerCalibrationExtension = nullptr;
    if (profilerCalibrationCanRequest && !profilerCalibrationAlreadyEnabled) {
        if (support.calibratedTimestampsKhrExtension)
            profilerCalibrationExtension = kVkKhrCalibratedTimestampsExtension;
        else if (support.calibratedTimestampsExtExtension)
            profilerCalibrationExtension = kVkExtCalibratedTimestampsExtension;
    }
    const bool calibratedTimestampsProfilerExtensionAdded = profilerCalibrationExtension != nullptr;
    if (profilerGpuEnabled && !profilerCalibrationInstanceDependencySatisfied) {
        apusr::host_discovery::event(
            "vulkan", "profiler.calibrated_timestamps_unavailable",
            "captured instance does not expose Vulkan 1.1 or VK_KHR_get_physical_device_properties2; preserving the host instance contract");
    } else if (profilerCalibrationCanRequest && !profilerCalibrationAlreadyEnabled &&
               !calibratedTimestampsProfilerExtensionAdded) {
        apusr::host_discovery::event(
            "vulkan", "profiler.calibrated_timestamps_unavailable",
            "physical device exposes neither VK_KHR_calibrated_timestamps nor VK_EXT_calibrated_timestamps");
    }
    const bool profilerGpuCpuEnabled = apusr::profile::cpuEnabled() || profilerGpuEnabled;
    const bool memoryBudgetWanted = profilerGpuCpuEnabled &&
        hasDeviceExtension(dispatch, physicalDevice, kVkExtMemoryBudgetExtension);
    const bool memoryBudgetExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkExtMemoryBudgetExtension);
    const bool memoryBudgetExtensionAdded = memoryBudgetWanted && !memoryBudgetExtensionAlreadyEnabled;
    const bool displayTimingWanted = profilerGpuCpuEnabled &&
        hasDeviceExtension(dispatch, physicalDevice, kVkGoogleDisplayTimingExtension);
    const bool displayTimingExtensionAlreadyEnabled =
        extensionAlreadyEnabled(*createInfo, kVkGoogleDisplayTimingExtension);
    // VK_GOOGLE_display_timing records are consumed by query. Only consume
    // them when APUSR itself enabled the extension for profiler use.
    const bool displayTimingExtensionAdded = displayTimingWanted && !displayTimingExtensionAlreadyEnabled;
    if (dotExtensionAdded || computeDerivativesExtensionAdded || subgroupSizeControlExtensionAdded || antiLagExtensionAdded ||
        swapchainMaintenance1ExtensionAdded || presentTimingExtensionAdded ||
        presentId2ExtensionAdded || calibratedTimestampsKhrExtensionAddedForPresentTiming ||
        calibratedTimestampsProfilerExtensionAdded || memoryBudgetExtensionAdded || displayTimingExtensionAdded) {
        extensions.reserve(
            static_cast<std::size_t>(createInfo->enabledExtensionCount) +
            (dotExtensionAdded ? 1u : 0u) +
            (computeDerivativesExtensionAdded ? 1u : 0u) +
            (subgroupSizeControlExtensionAdded ? 1u : 0u) +
            (antiLagExtensionAdded ? 1u : 0u) +
            (swapchainMaintenance1ExtensionAdded ? 1u : 0u) +
            (presentTimingExtensionAdded ? 1u : 0u) +
            (presentId2ExtensionAdded ? 1u : 0u) +
            (calibratedTimestampsKhrExtensionAddedForPresentTiming ? 1u : 0u) +
            (calibratedTimestampsProfilerExtensionAdded ? 1u : 0u) +
            (memoryBudgetExtensionAdded ? 1u : 0u) +
            (displayTimingExtensionAdded ? 1u : 0u));
        for (std::uint32_t index = 0; index < createInfo->enabledExtensionCount; ++index) {
            extensions.push_back(createInfo->ppEnabledExtensionNames[index]);
        }
        if (dotExtensionAdded) {
            extensions.push_back(kVkKhrShaderIntegerDotProductExtension);
        }
        if (computeDerivativesExtensionAdded) {
            extensions.push_back(kVkKhrComputeShaderDerivativesExtension);
        }
        if (subgroupSizeControlExtensionAdded) {
            extensions.push_back(kVkExtSubgroupSizeControlExtension);
        }
        if (antiLagExtensionAdded) {
            extensions.push_back(kVkAmdAntiLagExtension);
        }
        if (swapchainMaintenance1ExtensionAdded) {
            extensions.push_back(swapchainMaintenance1Extension);
        }
        if (presentId2ExtensionAdded) {
            extensions.push_back(kVkKhrPresentId2Extension);
        }
        if (calibratedTimestampsKhrExtensionAddedForPresentTiming) {
            extensions.push_back(kVkKhrCalibratedTimestampsExtension);
        }
        if (calibratedTimestampsProfilerExtensionAdded) {
            extensions.push_back(profilerCalibrationExtension);
        }
        if (presentTimingExtensionAdded) {
            extensions.push_back(kVkExtPresentTimingExtension);
        }
        if (memoryBudgetExtensionAdded) {
            extensions.push_back(kVkExtMemoryBudgetExtension);
        }
        if (displayTimingExtensionAdded) {
            extensions.push_back(kVkGoogleDisplayTimingExtension);
        }
        forwarded.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        forwarded.ppEnabledExtensionNames = extensions.data();
    }

    const std::vector<VkQueueFamilyPropertiesLoose> queueFamilies = queryQueueFamilies(dispatch, physicalDevice);

    // Reserve private APUSR queues without borrowing queues already requested
    // by the application. Prefer a graphics-side input/presentation/acquire set,
    // a non-graphics compute generation lane, and a transfer-only replay lane.
    // If a distinct input lane cannot be reserved, the frontend co-locates
    // Phase A with APUSR's generation queue; it never injects frame-generation
    // preprocessing onto the game's queue merely to manufacture concurrency.
    std::vector<VkDeviceQueueCreateInfoLoose> forwardedQueueInfos;
    std::array<std::vector<float>, 3u> privatePriorityStorage;
    std::vector<ReservedQueue> apusrFramegenQueues;
    bool privateInputQueueReserved = false;
    bool asyncComputeQueueReserved = false;
    if (framegenTimelineCanEnable && createInfo->pQueueCreateInfos != nullptr) {
        forwardedQueueInfos.assign(
            createInfo->pQueueCreateInfos,
            createInfo->pQueueCreateInfos + createInfo->queueCreateInfoCount);

        std::size_t graphicsCreateIndex = forwardedQueueInfos.size();
        for (std::size_t index = 0; index < forwardedQueueInfos.size(); ++index) {
            const auto& queueInfo = forwardedQueueInfos[index];
            if (queueInfo.queueFamilyIndex >= queueFamilies.size())
                continue;
            const auto& family = queueFamilies[queueInfo.queueFamilyIndex];
            if ((family.queueFlags & VkQueueGraphicsBit) == 0u || family.queueCount < queueInfo.queueCount + 3u)
                continue;
            graphicsCreateIndex = index;
            break;
        }

        if (graphicsCreateIndex != forwardedQueueInfos.size()) {
            auto& graphicsInfo = forwardedQueueInfos[graphicsCreateIndex];
            auto& priorities = privatePriorityStorage[0];
            priorities.reserve(static_cast<std::size_t>(graphicsInfo.queueCount) + 3u);
            if (graphicsInfo.pQueuePriorities) {
                priorities.insert(
                    priorities.end(), graphicsInfo.pQueuePriorities,
                    graphicsInfo.pQueuePriorities + graphicsInfo.queueCount);
            } else {
                priorities.resize(graphicsInfo.queueCount, 1.0f);
            }
            const float priority = priorities.empty()
                ? 1.0f
                : *std::max_element(priorities.begin(), priorities.end());
            const std::uint32_t graphicsFamilyIndex = graphicsInfo.queueFamilyIndex;
            const std::uint32_t firstGraphicsIndex = graphicsInfo.queueCount;
            priorities.insert(priorities.end(), 3u, priority);
            graphicsInfo.queueCount += 3u;
            graphicsInfo.pQueuePriorities = priorities.data();

            // Prefer a compute-capable family without graphics. This is the
            // queue class that can overlap OF/FI with the game's graphics work
            // instead of merely adding another logical queue to one universal
            // family.
            std::uint32_t computeFamily = ~0u;
            std::size_t computeCreateIndex = forwardedQueueInfos.size();
            for (std::uint32_t familyIndex = 0u; familyIndex < queueFamilies.size(); ++familyIndex) {
                const auto& family = queueFamilies[familyIndex];
                if ((family.queueFlags & VkQueueComputeBit) == 0u || (family.queueFlags & VkQueueGraphicsBit) != 0u)
                    continue;
                std::uint32_t requestedCount = 0u;
                std::size_t requestedIndex = forwardedQueueInfos.size();
                for (std::size_t index = 0; index < forwardedQueueInfos.size(); ++index) {
                    if (forwardedQueueInfos[index].queueFamilyIndex == familyIndex) {
                        requestedCount = forwardedQueueInfos[index].queueCount;
                        requestedIndex = index;
                        break;
                    }
                }
                if (family.queueCount <= requestedCount)
                    continue;
                computeFamily = familyIndex;
                computeCreateIndex = requestedIndex;
                break;
            }

            if (computeFamily != ~0u) {
                std::uint32_t computeQueueIndex = 0u;
                auto& priorities = privatePriorityStorage[1];
                if (computeCreateIndex != forwardedQueueInfos.size()) {
                    auto& computeInfo = forwardedQueueInfos[computeCreateIndex];
                    computeQueueIndex = computeInfo.queueCount;
                    priorities.reserve(static_cast<std::size_t>(computeInfo.queueCount) + 1u);
                    if (computeInfo.pQueuePriorities) {
                        priorities.insert(
                            priorities.end(), computeInfo.pQueuePriorities,
                            computeInfo.pQueuePriorities + computeInfo.queueCount);
                    } else {
                        priorities.resize(computeInfo.queueCount, 1.0f);
                    }
                    const float priority = priorities.empty()
                        ? 1.0f
                        : *std::max_element(priorities.begin(), priorities.end());
                    priorities.push_back(priority);
                    ++computeInfo.queueCount;
                    computeInfo.pQueuePriorities = priorities.data();
                } else {
                    priorities.push_back(1.0f);
                    VkDeviceQueueCreateInfoLoose computeInfo{};
                    computeInfo.queueFamilyIndex = computeFamily;
                    computeInfo.queueCount = 1u;
                    computeInfo.pQueuePriorities = priorities.data();
                    forwardedQueueInfos.push_back(computeInfo);
                }
                apusrFramegenQueues.push_back(ReservedQueue{
                    .familyIndex = computeFamily,
                    .queueIndex = computeQueueIndex,
                    .role = APUSR_VK_HOST_QUEUE_ROLE_GENERATION,
                });
                asyncComputeQueueReserved = true;
            }

            // A transfer-only queue is the ideal replay lane: the large
            // source/generated copies into private WSI images do not consume a
            // universal graphics queue at all. Reserve it only when the device
            // exposes capacity beyond what the application requested; never
            // borrow an application queue.
            std::uint32_t transferFamily = ~0u;
            std::size_t transferCreateIndex = forwardedQueueInfos.size();
            for (std::uint32_t familyIndex = 0u; familyIndex < queueFamilies.size(); ++familyIndex) {
                const auto& family = queueFamilies[familyIndex];
                if ((family.queueFlags & VkQueueTransferBit) == 0u ||
                    (family.queueFlags & (VkQueueGraphicsBit | VkQueueComputeBit)) != 0u)
                    continue;
                std::uint32_t requestedCount = 0u;
                std::size_t requestedIndex = forwardedQueueInfos.size();
                for (std::size_t index = 0; index < forwardedQueueInfos.size(); ++index) {
                    if (forwardedQueueInfos[index].queueFamilyIndex == familyIndex) {
                        requestedCount = forwardedQueueInfos[index].queueCount;
                        requestedIndex = index;
                        break;
                    }
                }
                if (family.queueCount <= requestedCount)
                    continue;
                transferFamily = familyIndex;
                transferCreateIndex = requestedIndex;
                break;
            }

            bool replayTransferQueueReserved = false;
            bool replayOnComputeFamily = false;
            if (transferFamily != ~0u) {
                std::uint32_t transferQueueIndex = 0u;
                auto& priorities = privatePriorityStorage[2];
                if (transferCreateIndex != forwardedQueueInfos.size()) {
                    auto& transferInfo = forwardedQueueInfos[transferCreateIndex];
                    transferQueueIndex = transferInfo.queueCount;
                    priorities.reserve(static_cast<std::size_t>(transferInfo.queueCount) + 1u);
                    if (transferInfo.pQueuePriorities) {
                        priorities.insert(
                            priorities.end(), transferInfo.pQueuePriorities,
                            transferInfo.pQueuePriorities + transferInfo.queueCount);
                    } else {
                        priorities.resize(transferInfo.queueCount, 1.0f);
                    }
                    const float priority = priorities.empty()
                        ? 1.0f
                        : *std::max_element(priorities.begin(), priorities.end());
                    priorities.push_back(priority);
                    ++transferInfo.queueCount;
                    transferInfo.pQueuePriorities = priorities.data();
                } else {
                    priorities.push_back(1.0f);
                    VkDeviceQueueCreateInfoLoose transferInfo{};
                    transferInfo.queueFamilyIndex = transferFamily;
                    transferInfo.queueCount = 1u;
                    transferInfo.pQueuePriorities = priorities.data();
                    forwardedQueueInfos.push_back(transferInfo);
                }
                apusrFramegenQueues.push_back(ReservedQueue{
                    .familyIndex = transferFamily,
                    .queueIndex = transferQueueIndex,
                    .role = APUSR_VK_HOST_QUEUE_ROLE_REPLAY_TRANSFER,
                });
                replayTransferQueueReserved = true;
            }

            // AMD desktop/APU drivers commonly expose a non-graphics
            // COMPUTE|TRANSFER family rather than a pure transfer family.  If
            // that family has another queue beyond the one reserved for FI/OF,
            // give replay its own queue there.  This keeps the full-resolution
            // source copy off the universal graphics family without allowing
            // the presenter thread to submit concurrently to the generation
            // queue (VkQueue access is externally synchronized).
            if (!replayTransferQueueReserved && asyncComputeQueueReserved &&
                computeFamily < queueFamilies.size() &&
                (queueFamilies[computeFamily].queueFlags & VkQueueTransferBit) != 0u) {
                std::size_t computeInfoIndex = forwardedQueueInfos.size();
                for (std::size_t index = 0u; index < forwardedQueueInfos.size(); ++index) {
                    if (forwardedQueueInfos[index].queueFamilyIndex == computeFamily) {
                        computeInfoIndex = index;
                        break;
                    }
                }
                if (computeInfoIndex != forwardedQueueInfos.size()) {
                    auto& computeInfo = forwardedQueueInfos[computeInfoIndex];
                    if (computeInfo.queueCount < queueFamilies[computeFamily].queueCount) {
                        const std::uint32_t replayQueueIndex = computeInfo.queueCount;
                        auto& priorities = privatePriorityStorage[1];
                        // The generation reservation already copied every
                        // existing priority for this family into this storage.
                        // Grow the same contiguous array and refresh the pointer
                        // in case vector growth moved it.
                        const float priority = priorities.empty()
                            ? 1.0f
                            : *std::max_element(priorities.begin(), priorities.end());
                        priorities.push_back(priority);
                        ++computeInfo.queueCount;
                        computeInfo.pQueuePriorities = priorities.data();
                        apusrFramegenQueues.push_back(ReservedQueue{
                            .familyIndex = computeFamily,
                            .queueIndex = replayQueueIndex,
                            .role = APUSR_VK_HOST_QUEUE_ROLE_REPLAY_TRANSFER,
                        });
                        replayTransferQueueReserved = true;
                        replayOnComputeFamily = true;
                    }
                }
            }

            const std::array<std::uint32_t, 3u> graphicsRoles = asyncComputeQueueReserved
                ? std::array<std::uint32_t, 3u>{
                    APUSR_VK_HOST_QUEUE_ROLE_INPUT,
                    APUSR_VK_HOST_QUEUE_ROLE_PRESENTATION,
                    APUSR_VK_HOST_QUEUE_ROLE_ACQUIRE,
                }
                : std::array<std::uint32_t, 3u>{
                    APUSR_VK_HOST_QUEUE_ROLE_GENERATION,
                    APUSR_VK_HOST_QUEUE_ROLE_PRESENTATION,
                    APUSR_VK_HOST_QUEUE_ROLE_ACQUIRE,
                };
            privateInputQueueReserved = asyncComputeQueueReserved;
            for (std::uint32_t offset = 0u; offset < graphicsRoles.size(); ++offset) {
                apusrFramegenQueues.push_back(ReservedQueue{
                    .familyIndex = graphicsFamilyIndex,
                    .queueIndex = firstGraphicsIndex + offset,
                    .role = graphicsRoles[offset],
                });
            }

            forwarded.queueCreateInfoCount = static_cast<std::uint32_t>(forwardedQueueInfos.size());
            forwarded.pQueueCreateInfos = forwardedQueueInfos.data();
            apusr::host_discovery::eventf(
                "vulkan", "framegen.queues_reserved",
                "physical_device=%p graphics_family=%u graphics_first_index=%u graphics_count=3 compute_family=%u compute_reserved=%u replay_family=%u replay_reserved=%u replay_on_compute=%u private_input=%u total_private=%u",
                physicalDevice, graphicsFamilyIndex, firstGraphicsIndex,
                asyncComputeQueueReserved ? computeFamily : ~0u,
                asyncComputeQueueReserved ? 1u : 0u,
                replayTransferQueueReserved ? (replayOnComputeFamily ? computeFamily : transferFamily) : ~0u,
                replayTransferQueueReserved ? 1u : 0u,
                replayOnComputeFamily ? 1u : 0u,
                privateInputQueueReserved ? 1u : 0u,
                static_cast<unsigned>(apusrFramegenQueues.size()));
        } else {
            apusr::host_discovery::eventf(
                "vulkan", "framegen.queue_unavailable",
                "physical_device=%p reason=fewer_than_three_spare_graphics_queues", physicalDevice);
        }
    }

    const VkResult result = dispatch.createDevice(physicalDevice, &forwarded, allocator, device);

    if (features2 != nullptr) {
        features2->features.values[kVkShaderStorageImageExtendedFormatsFeatureIndex] = oldCore[0];
        features2->features.values[kVkShaderStorageImageWriteWithoutFormatFeatureIndex] = oldCore[1];
        features2->features.values[kVkShaderInt16FeatureIndex] = oldCore[2];
    }
    if (vulkan12 != nullptr) {
        vulkan12->shaderFloat16 = oldFloat16Int8[0];
        vulkan12->shaderInt8 = oldFloat16Int8[1];
    } else if (float16Int8 != nullptr) {
        float16Int8->shaderFloat16 = oldFloat16Int8[0];
        float16Int8->shaderInt8 = oldFloat16Int8[1];
    }
    if (modifiedVulkan13Dot) {
        vulkan13->shaderIntegerDotProduct = oldDot;
    } else if (modifiedStandaloneDot) {
        integerDot->shaderIntegerDotProduct = oldDot;
    }
    if (modifiedHostComputeDerivatives) {
        computeDerivatives->computeDerivativeGroupLinear = oldComputeDerivativeGroupLinear;
    }
    if (modifiedVulkan13SubgroupSizeControl) {
        vulkan13->subgroupSizeControl = oldSubgroupSizeControl;
    } else if (modifiedStandaloneSubgroupSizeControl) {
        subgroupSizeControl->subgroupSizeControl = oldSubgroupSizeControl;
    }
    if (modifiedHostAntiLag) {
        antiLag->antiLag = oldAntiLag;
    }
    if (modifiedHostSwapchainMaintenance1) {
        swapchainMaintenance1->swapchainMaintenance1 = oldSwapchainMaintenance1;
    }
    if (modifiedHostPresentTiming) {
        presentTiming->presentTiming = oldPresentTiming[0];
        presentTiming->presentAtAbsoluteTime = oldPresentTiming[1];
        presentTiming->presentAtRelativeTime = oldPresentTiming[2];
    }
    if (modifiedVulkan12TimelineSemaphore) {
        vulkan12->timelineSemaphore = oldTimelineSemaphore;
    } else if (modifiedStandaloneTimelineSemaphore) {
        timelineSemaphore->timelineSemaphore = oldTimelineSemaphore;
    }

    if (result == VkSuccess && *device != nullptr) {
        {
            State& capture = state();
            std::lock_guard<std::mutex> lock(capture.mutex);
            capture.legacyOpticalFlowQueueRoleRequested = false;
        }
        const VkInstance instance = instanceContext.instance;
        captureCreatedQueues(
            dispatch,
            instance,
            physicalDevice,
            *device,
            forwarded,
            queueFamilies,
            APUSR_VK_HOST_REQUIRED_MASK |
                APUSR_VK_HOST_SHADER_FLOAT16 |
                (framegenTimelineCanEnable ? APUSR_VK_HOST_FRAMEGEN_TIMELINE : 0u) |
                (framegenSubgroup64CanEnable ? APUSR_VK_HOST_FRAMEGEN_SUBGROUP64 : 0u) |
                (support.subgroup32 ? APUSR_VK_HOST_FSR4_SUBGROUP32 : 0u) |
                (support.subgroup64 ? APUSR_VK_HOST_FSR4_SUBGROUP64 : 0u),
            antiLagCanEnable,
            swapchainMaintenance1CanEnable,
            presentTimingRelativeCanEnable,
            displayTimingExtensionAdded,
            apusrFramegenQueues);
        const bool antiLagActive = (deviceNegotiationFlags(*device) & APUSR_VK_HOST_ANTI_LAG) != 0u;
        apusr::host_discovery::eventf(
            "vulkan", "device.created",
            "instance=%p physical_device=%p device=%p api=0x%08x native_dot4=1 packed_i8_signed_accelerated=1 packed_i8_signed_accsat_accelerated=%u packed_i8_signed_accsat_required=%u compute_derivative_group_linear=1 dot_source=%s queues_requested=%u anti_lag_requested=%u anti_lag_enabled=%u framegen_timeline=%u framegen_subgroup64=%u fsr4_subgroup32=%u fsr4_subgroup64=%u swapchain_maintenance1=%u present_timing_relative=%u profiler_calibrated_timestamps=%u subgroup_min=%u subgroup_max=%u framegen_private_queues=%u",
            instance,
            physicalDevice,
            *device,
            support.apiVersion,
            support.packedSignedDotAccSatAccelerated ? 1u : 0u,
            kFsr4RequiresPackedSignedAccSat ? 1u : 0u,
            needsDotExtension ? "VK_KHR_shader_integer_dot_product" : "Vulkan13Core",
            createInfo->queueCreateInfoCount,
            antiLagRequested ? 1u : 0u,
            antiLagActive ? 1u : 0u,
            framegenTimelineCanEnable ? 1u : 0u,
            framegenSubgroup64CanEnable ? 1u : 0u,
            support.subgroup32 ? 1u : 0u,
            support.subgroup64 ? 1u : 0u,
            swapchainMaintenance1CanEnable ? 1u : 0u,
            presentTimingRelativeCanEnable ? 1u : 0u,
            (profilerCalibrationCanRequest &&
             (calibratedTimestampsKhrExtensionAlreadyEnabled || calibratedTimestampsExtExtensionAlreadyEnabled ||
              calibratedTimestampsKhrExtensionAddedForPresentTiming || calibratedTimestampsProfilerExtensionAdded)) ? 1u : 0u,
            support.minSubgroupSize,
            support.maxSubgroupSize,
            static_cast<unsigned>(apusrFramegenQueues.size()));
    } else {
        apusr::host_discovery::eventf(
            "vulkan", "device.create_failed",
            "result=%d api=0x%08x native_dot4_requested=1", result, support.apiVersion);
    }
    return result;
}

void destroyDevice(VkDevice device, const VkAllocationCallbacks* allocator)
{
    const Dispatch dispatch = dispatchSnapshot();
    if (dispatch.destroyDevice == nullptr)
        return;

    // Vulkan requires child objects to be retired before the device.  Normal
    // applications destroy swapchains explicitly, but keep this path robust:
    // first let the frontend drain any FG work for surviving swapchains, then
    // release APUSR's virtual backbuffers while the device dispatch is valid.
    std::vector<VkSwapchainKHR> survivingSwapchains;
    if (device != nullptr) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        for (const auto& [handle, record] : capture.swapchains) {
            if (record.device == device)
                survivingSwapchains.push_back(handle);
        }
    }
    if (const auto interceptor = g_swapchainDestroyInterceptor.load(std::memory_order_acquire)) {
        for (VkSwapchainKHR swapchain : survivingSwapchains)
            interceptor(device, swapchain);
    }

    std::vector<SwapchainRecord> retirements;
    if (device != nullptr) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        for (auto it = capture.swapchains.begin(); it != capture.swapchains.end();) {
            if (it->second.device == device) {
                retirements.push_back(std::move(it->second));
                it = capture.swapchains.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (SwapchainRecord& record : retirements) {
        if (record.virtualized)
            destroyVirtualImages(record);
    }

    dispatch.destroyDevice(device, allocator);
    if (device != nullptr) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end()) {
            const VkQueue fastQueue = g_fastCapturedPresentQueue.load(std::memory_order_relaxed);
            bool clearFastQueue = false;
            for (const QueueRecord& queue : deviceIt->second.queues) {
                clearFastQueue = clearFastQueue || queue.queue == fastQueue;
                capture.deviceByQueue.erase(queue.queue);
            }
            if (clearFastQueue)
                g_fastCapturedPresentQueue.store(nullptr, std::memory_order_release);
            capture.devices.erase(deviceIt);
        }
    }
    apusr::host_discovery::eventf("vulkan", "device.destroyed", "device=%p", device);
}

void destroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator)
{
    const Dispatch dispatch = dispatchSnapshot();
    if (dispatch.destroyInstance == nullptr) {
        return;
    }
    dispatch.destroyInstance(instance, allocator);
    if (instance != nullptr) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        capture.instances.erase(instance);
        for (auto it = capture.instanceByPhysicalDevice.begin(); it != capture.instanceByPhysicalDevice.end();) {
            if (it->second == instance) {
                it = capture.instanceByPhysicalDevice.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& [device, record] : capture.devices) {
            if (record.instance == instance) {
                record.instance = nullptr;
            }
        }
    }
    apusr::host_discovery::eventf("vulkan", "instance.destroyed", "instance=%p", instance);
}

std::uint32_t deviceNegotiationFlags(VkDevice device)
{
    if (device == nullptr) {
        return 0u;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.devices.find(device);
    return it != capture.devices.end() ? it->second.negotiationFlags : 0u;
}

bool getDeviceInfo(VkDevice device, ApusrVulkanHostDeviceInfo* outInfo)
{
    if (device == nullptr || outInfo == nullptr || outInfo->structSize < sizeof(ApusrVulkanHostDeviceInfo)) {
        return false;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.devices.find(device);
    if (it == capture.devices.end()) {
        return false;
    }
    const DeviceRecord& record = it->second;
    outInfo->instance = record.instance;
    outInfo->physicalDevice = record.physicalDevice;
    outInfo->device = record.device;
    outInfo->negotiationFlags = record.negotiationFlags;
    outInfo->queueCount = static_cast<std::uint32_t>(record.queues.size());
    return true;
}

bool getQueueInfo(
    VkDevice device,
    const std::uint32_t queueOrdinal,
    ApusrVulkanHostQueueInfo* outInfo)
{
    if (device == nullptr || outInfo == nullptr || outInfo->structSize < sizeof(ApusrVulkanHostQueueInfo)) {
        return false;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto deviceIt = capture.devices.find(device);
    if (deviceIt == capture.devices.end() || queueOrdinal >= deviceIt->second.queues.size()) {
        return false;
    }

    const DeviceRecord& deviceRecord = deviceIt->second;
    const QueueRecord& queue = deviceRecord.queues[queueOrdinal];
    outInfo->instance = deviceRecord.instance;
    outInfo->physicalDevice = deviceRecord.physicalDevice;
    outInfo->device = deviceRecord.device;
    outInfo->queue = queue.queue;
    outInfo->familyIndex = queue.familyIndex;
    outInfo->queueIndex = queue.queueIndex;
    outInfo->familyFlags = queue.familyFlags;
    outInfo->queueCreateFlags = queue.queueCreateFlags;
    outInfo->kind = queue.kind;
    outInfo->ownershipFlags = queue.ownershipFlags;
    return true;
}

bool resolveQueue(
    VkDevice device,
    const std::uint32_t preferredKind,
    ApusrVulkanHostQueueInfo* outInfo)
{
    if (device == nullptr || outInfo == nullptr || outInfo->structSize < sizeof(ApusrVulkanHostQueueInfo)) {
        return false;
    }

    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.devices.find(device);
    if (it == capture.devices.end() || it->second.queues.empty()) {
        return false;
    }

    const DeviceRecord& deviceRecord = it->second;
    const QueueRecord* best = nullptr;
    std::uint32_t bestScore = 0;
    for (const QueueRecord& candidate : deviceRecord.queues) {
        const std::uint32_t score = queueScore(candidate, preferredKind);
        if (best == nullptr || score > bestScore) {
            best = &candidate;
            bestScore = score;
        }
    }
    if (best == nullptr || best->queue == nullptr ||
        (preferredKind != APUSR_VK_HOST_QUEUE_UNKNOWN && bestScore == 0u)) {
        return false;
    }

    outInfo->instance = deviceRecord.instance;
    outInfo->physicalDevice = deviceRecord.physicalDevice;
    outInfo->device = deviceRecord.device;
    outInfo->queue = best->queue;
    outInfo->familyIndex = best->familyIndex;
    outInfo->queueIndex = best->queueIndex;
    outInfo->familyFlags = best->familyFlags;
    outInfo->queueCreateFlags = best->queueCreateFlags;
    outInfo->kind = best->kind;
    outInfo->ownershipFlags = best->ownershipFlags;
    return true;
}

VkResult createSwapchain(
    VkDevice device,
    const VkSwapchainCreateInfoKHRLoose* createInfo,
    const VkAllocationCallbacks* allocator,
    VkSwapchainKHR* swapchain)
{
    if (device == nullptr || createInfo == nullptr || swapchain == nullptr) {
        return VkErrorInitializationFailed;
    }

    VkCreateSwapchainKHRFn create = nullptr;
    VkGetDeviceProcAddrFn getDeviceProcAddr = nullptr;
    bool swapchainMaintenance1Enabled = false;
    bool presentTimingRelativeEnabled = false;
    VkInstance deviceInstance = nullptr;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end()) {
            create = deviceIt->second.createSwapchain;
            swapchainMaintenance1Enabled = deviceIt->second.swapchainMaintenance1Enabled;
            presentTimingRelativeEnabled = deviceIt->second.presentTimingRelativeEnabled;
            deviceInstance = deviceIt->second.instance;
        }
        getDeviceProcAddr = capture.dispatch.getDeviceProcAddr;
    }
    if (create == nullptr && getDeviceProcAddr != nullptr) {
        create = reinterpret_cast<VkCreateSwapchainKHRFn>(
            getDeviceProcAddr(device, "vkCreateSwapchainKHR"));
    }
    if (create == nullptr) {
        return VkErrorExtensionNotPresent;
    }

    VkSwapchainCreateInfoKHRLoose effectiveCreateInfo = *createInfo;
    std::uint32_t supportedUsage = createInfo->imageUsage;
    std::uint32_t framegenFlags = 0u;
    const bool framegenRequested = frameGenerationRequestedSnapshot();
    const bool framegenActive = frameGenerationActiveSnapshot();
    std::uint32_t supportedMaxImageCount = 0u;
    if (framegenRequested)
        framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_REQUESTED;
    if (framegenActive) {
        if (swapchainMaintenance1Enabled)
            framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_MAINTENANCE1;
        VkPhysicalDevice physicalDevice = nullptr;
        VkGetPhysicalDeviceSurfaceCapabilitiesKHRFn getSurfaceCapabilities = nullptr;
        VkGetPhysicalDeviceSurfaceCapabilities2KHRFn getSurfaceCapabilities2 = nullptr;
        {
            State& capture = state();
            std::lock_guard<std::mutex> lock(capture.mutex);
            const auto deviceIt = capture.devices.find(device);
            if (deviceIt != capture.devices.end()) {
                physicalDevice = deviceIt->second.physicalDevice;
                getSurfaceCapabilities = capture.dispatch.getPhysicalDeviceSurfaceCapabilities;
                getSurfaceCapabilities2 = capture.dispatch.getPhysicalDeviceSurfaceCapabilities2;
                if (capture.dispatch.getInstanceProcAddr != nullptr && deviceIt->second.instance != nullptr) {
                    if (getSurfaceCapabilities == nullptr) {
                        getSurfaceCapabilities = reinterpret_cast<VkGetPhysicalDeviceSurfaceCapabilitiesKHRFn>(
                            capture.dispatch.getInstanceProcAddr(
                                deviceIt->second.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
                    }
                    if (getSurfaceCapabilities2 == nullptr) {
                        getSurfaceCapabilities2 = reinterpret_cast<VkGetPhysicalDeviceSurfaceCapabilities2KHRFn>(
                            capture.dispatch.getInstanceProcAddr(
                                deviceIt->second.instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
                    }
                }
            }
        }

        VkSurfaceCapabilitiesKHRLoose capabilities{};
        if (physicalDevice != nullptr && getSurfaceCapabilities != nullptr &&
            getSurfaceCapabilities(physicalDevice, createInfo->surface, &capabilities) == VkSuccess) {
            supportedUsage = capabilities.supportedUsageFlags;
            supportedMaxImageCount = capabilities.maxImageCount;
            // Frame generation may hold one mandatory source target and one
            // generated target concurrently while the presentation engine owns
            // the previous image. Guarantee triple buffering when the surface
            // allows it; requesting more than three would buy little while
            // consuming another full-resolution allocation on an APU.
            constexpr std::uint32_t kFramegenMinimumRealImages = 3u;
            if (effectiveCreateInfo.minImageCount < kFramegenMinimumRealImages &&
                (capabilities.maxImageCount == 0u || capabilities.maxImageCount >= kFramegenMinimumRealImages)) {
                effectiveCreateInfo.minImageCount = kFramegenMinimumRealImages;
                framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_EXTRA_IMAGES;
            }
            // The real WSI images are private to APUSR while the application
            // sees virtual backbuffers.  Transfer-dst is the universal replay
            // path.  When swapchain-maintenance1 is available, request STORAGE
            // as well so FI can render a generated frame directly into an
            // opportunistically acquired real WSI image; a late generated frame
            // can then be released instead of poisoning presentation order.
            std::uint32_t usefulRealWsiUsage =
                VkImageUsageTransferSrcBit | VkImageUsageTransferDstBit;
            if (swapchainMaintenance1Enabled)
                usefulRealWsiUsage |= VkImageUsageStorageBit;
            effectiveCreateInfo.imageUsage |= supportedUsage & usefulRealWsiUsage;
        }

        // Let WSI own interpolation midpoint pacing whenever it can express a
        // relative minimum display duration. This avoids waking a CPU thread
        // merely to issue the mandatory source present at the midpoint. The EXT
        // scheduling semantics apply only to FIFO-family present modes.
        const bool fifoTimingMode =
            effectiveCreateInfo.presentMode == VkPresentModeFifoKhr ||
            effectiveCreateInfo.presentMode == VkPresentModeFifoRelaxedKhr ||
            effectiveCreateInfo.presentMode == VkPresentModeFifoLatestReadyKhr;
        if (presentTimingRelativeEnabled && fifoTimingMode && physicalDevice != nullptr &&
            getSurfaceCapabilities2 != nullptr) {
            VkPresentTimingSurfaceCapabilitiesEXTLoose timingCapabilities{};
            VkSurfaceCapabilities2KHRLoose capabilities2{};
            capabilities2.pNext = &timingCapabilities;
            VkPhysicalDeviceSurfaceInfo2KHRLoose surfaceInfo{};
            surfaceInfo.surface = createInfo->surface;
            if (getSurfaceCapabilities2(physicalDevice, &surfaceInfo, &capabilities2) == VkSuccess &&
                timingCapabilities.presentTimingSupported != 0u &&
                timingCapabilities.presentAtRelativeTimeSupported != 0u) {
                effectiveCreateInfo.flags |= VkSwapchainCreatePresentTimingBitExt;
                framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_PRESENT_TIMING_RELATIVE;
            } else {
                apusr::host_discovery::eventf(
                    "vulkan", "framegen.present_timing_surface_unavailable",
                    "device=%p surface=%p mode=%u timing=%u relative=%u",
                    device, createInfo->surface, effectiveCreateInfo.presentMode,
                    timingCapabilities.presentTimingSupported ? 1u : 0u,
                    timingCapabilities.presentAtRelativeTimeSupported ? 1u : 0u);
            }
        }
    }

    if ((effectiveCreateInfo.imageUsage & VkImageUsageTransferSrcBit) != 0u)
        framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_TRANSFER_SRC;
    if ((effectiveCreateInfo.imageUsage & VkImageUsageTransferDstBit) != 0u)
        framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_TRANSFER_DST;
    if ((effectiveCreateInfo.imageUsage & VkImageUsageSampledBit) != 0u)
        framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_SAMPLED;
    if ((effectiveCreateInfo.imageUsage & VkImageUsageStorageBit) != 0u)
        framegenFlags |= APUSR_VK_HOST_SWAPCHAIN_STORAGE;

    // Real WSI images are private to the presenter, so give them a concurrent
    // sharing contract that exactly covers the queues APUSR may use: graphics
    // presentation, transfer replay, and (for direct STORAGE output) generation.
    // This removes queue-family ownership-transfer ping-pong from the hot path.
    std::vector<std::uint32_t> realWsiSharingFamilies;
    if (framegenActive) {
        VirtualImageDeviceContext virtualContext{};
        if (loadVirtualImageDeviceContext(device, virtualContext) &&
            virtualContext.graphicsQueueFamily != ~0u) {
            if (effectiveCreateInfo.imageSharingMode == static_cast<std::uint32_t>(vk::SharingModeConcurrent) &&
                effectiveCreateInfo.pQueueFamilyIndices && effectiveCreateInfo.queueFamilyIndexCount != 0u) {
                realWsiSharingFamilies.assign(
                    effectiveCreateInfo.pQueueFamilyIndices,
                    effectiveCreateInfo.pQueueFamilyIndices + effectiveCreateInfo.queueFamilyIndexCount);
            }
            const auto addFamily = [&](const std::uint32_t family) {
                if (family == ~0u)
                    return;
                if (std::find(realWsiSharingFamilies.begin(), realWsiSharingFamilies.end(), family) ==
                    realWsiSharingFamilies.end())
                    realWsiSharingFamilies.push_back(family);
            };
            addFamily(virtualContext.graphicsQueueFamily);
            addFamily(virtualContext.replayQueueFamily);
            if ((effectiveCreateInfo.imageUsage & VkImageUsageStorageBit) != 0u)
                addFamily(virtualContext.generationQueueFamily);

            if (realWsiSharingFamilies.size() > 1u) {
                effectiveCreateInfo.imageSharingMode = static_cast<std::uint32_t>(vk::SharingModeConcurrent);
                effectiveCreateInfo.queueFamilyIndexCount = static_cast<std::uint32_t>(realWsiSharingFamilies.size());
                effectiveCreateInfo.pQueueFamilyIndices = realWsiSharingFamilies.data();
            }
            apusr::host_discovery::eventf(
                "vulkan", "framegen.real_wsi_sharing",
                "device=%p graphics_family=%u transfer_family=%u generation_family=%u storage=%u family_count=%u",
                device, virtualContext.graphicsQueueFamily, virtualContext.replayQueueFamily,
                virtualContext.generationQueueFamily,
                (effectiveCreateInfo.imageUsage & VkImageUsageStorageBit) != 0u ? 1u : 0u,
                effectiveCreateInfo.queueFamilyIndexCount);
        }
    }

    const VkResult result = create(device, &effectiveCreateInfo, allocator, swapchain);
    if (result != VkSuccess || *swapchain == nullptr) {
        return result;
    }

    SwapchainRecord record{};
    record.device = device;
    record.swapchain = *swapchain;
    record.surface = effectiveCreateInfo.surface;
    record.minImageCount = effectiveCreateInfo.minImageCount;
    record.imageFormat = effectiveCreateInfo.imageFormat;
    record.imageColorSpace = effectiveCreateInfo.imageColorSpace;
    record.imageExtent = effectiveCreateInfo.imageExtent;
    record.imageArrayLayers = effectiveCreateInfo.imageArrayLayers;
    record.imageUsage = effectiveCreateInfo.imageUsage;
    record.realImageUsage = effectiveCreateInfo.imageUsage;
    record.originalImageUsage = createInfo->imageUsage;
    record.supportedImageUsage = supportedUsage;
    record.framegenFlags = framegenFlags;
    record.imageSharingMode = effectiveCreateInfo.imageSharingMode;
    record.presentMode = effectiveCreateInfo.presentMode;
    record.presentTimingRelativeCapable =
        (framegenFlags & APUSR_VK_HOST_SWAPCHAIN_PRESENT_TIMING_RELATIVE) != 0u;

    if (record.presentTimingRelativeCapable) {
        VkGetSwapchainTimeDomainPropertiesEXTFn getTimeDomains = nullptr;
        {
            State& capture = state();
            std::lock_guard<std::mutex> lock(capture.mutex);
            const auto deviceIt = capture.devices.find(device);
            if (deviceIt != capture.devices.end())
                getTimeDomains = deviceIt->second.getSwapchainTimeDomainProperties;
        }
        record.presentTimingRelative = queryRelativePresentTimingDomain(
            getTimeDomains, device, *swapchain, record.realWsiHostMutex,
            record.presentTimingTimeDomain, record.presentTimingTimeDomainId);
        if (!record.presentTimingRelative) {
            // VK_EXT_present_timing explicitly allows timing properties/time
            // domains to appear only after presentation has started. Keep the
            // capability armed and retry lazily from the presenter; until then
            // the CPU deadline path remains the universal fallback.
            apusr::host_discovery::eventf(
                "vulkan", "framegen.present_timing_domain_deferred",
                "device=%p swapchain=%p retry_after_present=1", device, *swapchain);
        }
    }

    VkGetSwapchainImagesKHRFn getImages = nullptr;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end())
            getImages = deviceIt->second.getSwapchainImages;
    }
    if (getImages != nullptr) {
        std::uint32_t realCount = 0u;
        if (getImages(device, *swapchain, &realCount, nullptr) == VkSuccess && realCount != 0u) {
            record.realImages.resize(realCount);
            std::uint32_t fillCount = realCount;
            const VkResult imagesResult = getImages(device, *swapchain, &fillCount, record.realImages.data());
            if ((imagesResult == VkSuccess || imagesResult == VkIncomplete) && fillCount != 0u) {
                record.realImages.resize(fillCount);
                record.realImageCount = fillCount;
            } else {
                record.realImages.clear();
            }
        }
    }

    bool virtualized = false;
    if (framegenRequested && framegenActive && record.realImageCount >= 2u) {
        virtualized = createVirtualImages(record, *createInfo, record.realImageCount);
        if (!virtualized) {
            apusr::host_discovery::eventf(
                "vulkan", "framegen.swapchain_virtualization_failed",
                "device=%p swapchain=%p real_images=%u reason=virtual_image_pool_or_three_queue_contract",
                device, *swapchain, record.realImageCount);
        }
    }
    if (!virtualized) {
        record.images = record.realImages;
        record.imageCount = record.realImageCount;
        record.imageUsage = effectiveCreateInfo.imageUsage;
    }

    // Profiler registration is cold-path swapchain setup. The normal present/
    // acquire paths test profile::enabled() before taking profiler state locks.
    record.profileConfigurationHash = wsiConfigurationHash(record);
    record.profileStream = apusr::profile::registerStream(apusr::profile::StreamDesc{
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::Frontend,
        .name = "vulkan.wsi",
        .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
            apusr::profile::StreamCapabilityFrames |
            apusr::profile::StreamCapabilityCounters |
            apusr::profile::StreamCapabilityErrors |
            apusr::profile::StreamCapabilityQueueEvents,
        .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(device)),
    });
    if (record.profileStream && (apusr::profile::cpuEnabled() || apusr::profile::gpuEnabled())) {
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = record.profileStream,
            .name = "wsi.swapchain_handle",
            .value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(*swapchain)),
        });
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = record.profileStream,
            .name = "wsi.swapchain_extent",
            .value = (static_cast<std::uint64_t>(record.imageExtent.width) << 32u) | record.imageExtent.height,
            .value2 = record.imageCount,
        });
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = record.profileStream,
            .name = "wsi.swapchain_contract",
            .value = (static_cast<std::uint64_t>(record.imageFormat) << 32u) | record.presentMode,
            .value2 = record.imageUsage,
            .flags = record.virtualized ? 1u : 0u,
        });
    }

    const std::uint32_t publicUsage = record.imageUsage;
    const std::uint32_t realImageCount = record.realImageCount;
    const std::uint32_t publicImageCount = record.imageCount;
    const std::uint32_t finalFramegenFlags = record.framegenFlags;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        capture.swapchains[*swapchain] = std::move(record);
    }
    apusr::host_discovery::eventf(
        "vulkan", "swapchain.created",
        "device=%p swapchain=%p format=%u color_space=%u min_images_original=%u min_images_effective=%u max_images=%u extent=%ux%u present_mode=%u usage_original=0x%x usage_effective=0x%x usage_public=0x%x usage_supported=0x%x framegen_active=%u virtualized=%u real_images=%u public_images=%u framegen_flags=0x%x",
        device,
        *swapchain,
        effectiveCreateInfo.imageFormat,
        effectiveCreateInfo.imageColorSpace,
        createInfo->minImageCount,
        effectiveCreateInfo.minImageCount,
        supportedMaxImageCount,
        effectiveCreateInfo.imageExtent.width,
        effectiveCreateInfo.imageExtent.height,
        effectiveCreateInfo.presentMode,
        createInfo->imageUsage,
        effectiveCreateInfo.imageUsage,
        publicUsage,
        supportedUsage,
        framegenActive ? 1u : 0u,
        virtualized ? 1u : 0u,
        realImageCount,
        publicImageCount,
        finalFramegenFlags);
    return result;
}

void destroySwapchain(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator)
{
    if (device == nullptr || swapchain == nullptr) {
        return;
    }

    VkDestroySwapchainKHRFn destroy = nullptr;
    VkGetDeviceProcAddrFn getDeviceProcAddr = nullptr;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end()) {
            destroy = deviceIt->second.destroySwapchain;
        }
        getDeviceProcAddr = capture.dispatch.getDeviceProcAddr;
    }
    if (destroy == nullptr && getDeviceProcAddr != nullptr) {
        destroy = reinterpret_cast<VkDestroySwapchainKHRFn>(
            getDeviceProcAddr(device, "vkDestroySwapchainKHR"));
    }

    // APUSR frontends can hold views and synchronization objects that refer to
    // real WSI images. Tear those down while the swapchain and its images are
    // still valid, before forwarding destruction to the loader. The callback
    // runs outside host-capture's state lock so frontend teardown may drain its
    // own presentation worker/queue without lock inversion.
    if (const auto interceptor = g_swapchainDestroyInterceptor.load(std::memory_order_acquire)) {
        interceptor(device, swapchain);
    }

    // Remove the public record while the device remains alive, then retire
    // APUSR-owned off-screen images outside the state lock.  Virtual-image
    // destruction consults the device dispatch table, so it must precede the
    // underlying swapchain/device lifetime boundary.
    SwapchainRecord retirement{};
    bool haveRetirement = false;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(swapchain);
        if (it != capture.swapchains.end()) {
            retirement = std::move(it->second);
            capture.swapchains.erase(it);
            haveRetirement = true;
        }
    }
    if (haveRetirement && retirement.virtualized)
        destroyVirtualImages(retirement);

    if (destroy != nullptr) {
        if (retirement.realWsiHostMutex) {
            std::lock_guard<std::mutex> hostLock(*retirement.realWsiHostMutex);
            destroy(device, swapchain, allocator);
        } else {
            destroy(device, swapchain, allocator);
        }
    }

    apusr::host_discovery::eventf(
        "vulkan", "swapchain.destroyed", "device=%p swapchain=%p", device, swapchain);
}

VkResult getSwapchainImages(
    VkDevice device,
    VkSwapchainKHR swapchain,
    std::uint32_t* count,
    VkImage* images)
{
    if (device == nullptr || swapchain == nullptr || count == nullptr) {
        return VkErrorInitializationFailed;
    }

    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(swapchain);
        if (it != capture.swapchains.end() && it->second.virtualized) {
            const std::uint32_t total = static_cast<std::uint32_t>(it->second.images.size());
            if (images == nullptr) {
                *count = total;
                return VkSuccess;
            }
            const std::uint32_t capacity = *count;
            const std::uint32_t copied = std::min(capacity, total);
            if (copied != 0u)
                std::copy_n(it->second.images.data(), copied, images);
            *count = copied;
            return copied < total ? VkIncomplete : VkSuccess;
        }
    }

    VkGetSwapchainImagesKHRFn getImages = nullptr;
    VkGetDeviceProcAddrFn getDeviceProcAddr = nullptr;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end()) {
            getImages = deviceIt->second.getSwapchainImages;
        }
        getDeviceProcAddr = capture.dispatch.getDeviceProcAddr;
    }
    if (getImages == nullptr && getDeviceProcAddr != nullptr) {
        getImages = reinterpret_cast<VkGetSwapchainImagesKHRFn>(
            getDeviceProcAddr(device, "vkGetSwapchainImagesKHR"));
    }
    if (getImages == nullptr) {
        return VkErrorExtensionNotPresent;
    }

    const VkResult result = getImages(device, swapchain, count, images);
    if (result != VkSuccess && result != VkIncomplete) {
        return result;
    }

    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.swapchains.find(swapchain);
    if (it != capture.swapchains.end()) {
        it->second.imageCount = *count;
        if (images != nullptr) {
            it->second.images.assign(images, images + *count);
            it->second.realImages = it->second.images;
            it->second.realImageCount = *count;
        }
    }
    return result;
}

VkResult acquireNextImage(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const std::uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    std::uint32_t* imageIndex)
{
    if (device == nullptr || swapchain == nullptr || imageIndex == nullptr) {
        return VkErrorInitializationFailed;
    }

    const WsiProfileCapture profile = beginWsiProfile(swapchain, WsiProfileOperation::Acquire);
    const std::uint64_t profileBeginTick = profile.stream && apusr::profile::cpuEnabled()
        ? apusr::profile::cpuTick() : 0u;

    bool virtualized = false;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(swapchain);
        virtualized = it != capture.swapchains.end() && it->second.virtualized;
    }
    if (virtualized) {
        const VkResult result = acquireVirtualImage(device, swapchain, timeout, semaphore, fence, imageIndex);
        if (profile.stream) {
            const std::uint64_t profileEndTick = apusr::profile::cpuEnabled() ? apusr::profile::cpuTick() : 0u;
            updateWsiProfileTick(swapchain, WsiProfileOperation::Acquire, profileBeginTick);
            recordWsiAcquire(
                profile, "vkAcquireNextImageKHR", profileBeginTick, profileEndTick, result, timeout,
                semaphore, fence,
                (result == VkSuccess || result == VkSuboptimalKhr) ? *imageIndex : APUSR_VK_HOST_INVALID_IMAGE_INDEX);
        }
        return result;
    }

    VkAcquireNextImageKHRFn acquire = nullptr;
    VkGetDeviceProcAddrFn getDeviceProcAddr = nullptr;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end()) {
            acquire = deviceIt->second.acquireNextImage;
        }
        getDeviceProcAddr = capture.dispatch.getDeviceProcAddr;
    }
    if (acquire == nullptr && getDeviceProcAddr != nullptr) {
        acquire = reinterpret_cast<VkAcquireNextImageKHRFn>(
            getDeviceProcAddr(device, "vkAcquireNextImageKHR"));
    }
    if (acquire == nullptr) {
        const VkResult result = VkErrorExtensionNotPresent;
        if (profile.stream) {
            const std::uint64_t profileEndTick = apusr::profile::cpuEnabled() ? apusr::profile::cpuTick() : 0u;
            updateWsiProfileTick(swapchain, WsiProfileOperation::Acquire, profileBeginTick);
            recordWsiAcquire(
                profile, "vkAcquireNextImageKHR", profileBeginTick, profileEndTick, result, timeout,
                semaphore, fence, APUSR_VK_HOST_INVALID_IMAGE_INDEX);
        }
        return result;
    }

    const auto acquireStart = std::chrono::steady_clock::now();
    const VkResult result = acquire(device, swapchain, timeout, semaphore, fence, imageIndex);
    if (profile.stream) {
        const std::uint64_t profileEndTick = apusr::profile::cpuEnabled() ? apusr::profile::cpuTick() : 0u;
        updateWsiProfileTick(swapchain, WsiProfileOperation::Acquire, profileBeginTick);
        recordWsiAcquire(
            profile, "vkAcquireNextImageKHR", profileBeginTick, profileEndTick, result, timeout,
            semaphore, fence,
            (result == VkSuccess || result == VkSuboptimalKhr) ? *imageIndex : APUSR_VK_HOST_INVALID_IMAGE_INDEX);
    }
    const auto acquireUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - acquireStart).count();
    if (acquireUs >= 5000 && frameGenerationActiveSnapshot() &&
        g_slowApplicationAcquireLogs.fetch_add(1u, std::memory_order_relaxed) < 8u) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.application_acquire_slow",
            "api=vkAcquireNextImageKHR swapchain=%p duration_us=%lld timeout=%llu result=%d image_index=%u",
            swapchain, static_cast<long long>(acquireUs), static_cast<unsigned long long>(timeout),
            result, (result == VkSuccess || result == VkSuboptimalKhr) ? *imageIndex : APUSR_VK_HOST_INVALID_IMAGE_INDEX);
    }
    if (result == VkSuccess || result == VkSuboptimalKhr) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(swapchain);
        if (it != capture.swapchains.end()) {
            it->second.latestAcquiredImageIndex = *imageIndex;
            ++it->second.acquireSerial;
        }
    }
    return result;
}

VkResult acquireNextImage2(
    VkDevice device,
    const VkAcquireNextImageInfoKHRLoose* acquireInfo,
    std::uint32_t* imageIndex)
{
    if (device == nullptr || acquireInfo == nullptr || acquireInfo->swapchain == nullptr || imageIndex == nullptr) {
        return VkErrorInitializationFailed;
    }

    const WsiProfileCapture profile = beginWsiProfile(acquireInfo->swapchain, WsiProfileOperation::Acquire);
    const std::uint64_t profileBeginTick = profile.stream && apusr::profile::cpuEnabled()
        ? apusr::profile::cpuTick() : 0u;

    bool virtualized = false;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(acquireInfo->swapchain);
        virtualized = it != capture.swapchains.end() && it->second.virtualized;
    }
    if (virtualized) {
        const VkResult result = acquireVirtualImage(
            device, acquireInfo->swapchain, acquireInfo->timeout,
            acquireInfo->semaphore, acquireInfo->fence, imageIndex);
        if (profile.stream) {
            const std::uint64_t profileEndTick = apusr::profile::cpuEnabled() ? apusr::profile::cpuTick() : 0u;
            updateWsiProfileTick(acquireInfo->swapchain, WsiProfileOperation::Acquire, profileBeginTick);
            recordWsiAcquire(
                profile, "vkAcquireNextImage2KHR", profileBeginTick, profileEndTick, result, acquireInfo->timeout,
                acquireInfo->semaphore, acquireInfo->fence,
                (result == VkSuccess || result == VkSuboptimalKhr) ? *imageIndex : APUSR_VK_HOST_INVALID_IMAGE_INDEX);
        }
        return result;
    }

    VkAcquireNextImage2KHRFn acquire = nullptr;
    VkGetDeviceProcAddrFn getDeviceProcAddr = nullptr;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end()) {
            acquire = deviceIt->second.acquireNextImage2;
        }
        getDeviceProcAddr = capture.dispatch.getDeviceProcAddr;
    }
    if (acquire == nullptr && getDeviceProcAddr != nullptr) {
        acquire = reinterpret_cast<VkAcquireNextImage2KHRFn>(
            getDeviceProcAddr(device, "vkAcquireNextImage2KHR"));
    }
    if (acquire == nullptr) {
        const VkResult result = VkErrorExtensionNotPresent;
        if (profile.stream) {
            const std::uint64_t profileEndTick = apusr::profile::cpuEnabled() ? apusr::profile::cpuTick() : 0u;
            updateWsiProfileTick(acquireInfo->swapchain, WsiProfileOperation::Acquire, profileBeginTick);
            recordWsiAcquire(
                profile, "vkAcquireNextImage2KHR", profileBeginTick, profileEndTick, result, acquireInfo->timeout,
                acquireInfo->semaphore, acquireInfo->fence, APUSR_VK_HOST_INVALID_IMAGE_INDEX);
        }
        return result;
    }

    const auto acquireStart = std::chrono::steady_clock::now();
    const VkResult result = acquire(device, acquireInfo, imageIndex);
    if (profile.stream) {
        const std::uint64_t profileEndTick = apusr::profile::cpuEnabled() ? apusr::profile::cpuTick() : 0u;
        updateWsiProfileTick(acquireInfo->swapchain, WsiProfileOperation::Acquire, profileBeginTick);
        recordWsiAcquire(
            profile, "vkAcquireNextImage2KHR", profileBeginTick, profileEndTick, result, acquireInfo->timeout,
            acquireInfo->semaphore, acquireInfo->fence,
            (result == VkSuccess || result == VkSuboptimalKhr) ? *imageIndex : APUSR_VK_HOST_INVALID_IMAGE_INDEX);
    }
    const auto acquireUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - acquireStart).count();
    if (acquireUs >= 5000 && frameGenerationActiveSnapshot() &&
        g_slowApplicationAcquireLogs.fetch_add(1u, std::memory_order_relaxed) < 8u) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.application_acquire_slow",
            "api=vkAcquireNextImage2KHR swapchain=%p duration_us=%lld timeout=%llu result=%d image_index=%u",
            acquireInfo->swapchain, static_cast<long long>(acquireUs),
            static_cast<unsigned long long>(acquireInfo->timeout), result,
            (result == VkSuccess || result == VkSuboptimalKhr) ? *imageIndex : APUSR_VK_HOST_INVALID_IMAGE_INDEX);
    }
    if (result == VkSuccess || result == VkSuboptimalKhr) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(acquireInfo->swapchain);
        if (it != capture.swapchains.end()) {
            it->second.latestAcquiredImageIndex = *imageIndex;
            ++it->second.acquireSerial;
        }
    }
    return result;
}

bool getSwapchainInfo(VkSwapchainKHR swapchain, ApusrVulkanHostSwapchainInfo* outInfo)
{
    if (swapchain == nullptr || outInfo == nullptr || outInfo->structSize < sizeof(ApusrVulkanHostSwapchainInfo)) {
        return false;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.swapchains.find(swapchain);
    if (it == capture.swapchains.end()) {
        return false;
    }
    const SwapchainRecord& record = it->second;
    outInfo->device = record.device;
    outInfo->swapchain = record.swapchain;
    outInfo->surface = record.surface;
    outInfo->imageFormat = record.imageFormat;
    outInfo->imageColorSpace = record.imageColorSpace;
    outInfo->width = record.imageExtent.width;
    outInfo->height = record.imageExtent.height;
    outInfo->imageArrayLayers = record.imageArrayLayers;
    outInfo->imageUsage = record.imageUsage;
    outInfo->realImageUsage = record.realImageUsage;
    outInfo->originalImageUsage = record.originalImageUsage;
    outInfo->supportedImageUsage = record.supportedImageUsage;
    outInfo->framegenFlags = record.framegenFlags;
    outInfo->imageSharingMode = record.imageSharingMode;
    outInfo->presentMode = record.presentMode;
    outInfo->minImageCount = record.minImageCount;
    outInfo->imageCount = record.imageCount;
    outInfo->realImageCount = record.realImageCount;
    outInfo->latestAcquiredImageIndex = record.latestAcquiredImageIndex;
    outInfo->latestPresentedImageIndex = record.latestPresentedImageIndex;
    outInfo->acquireSerial = record.acquireSerial;
    outInfo->presentSerial = record.presentSerial;
    return true;
}

bool getSwapchainImage(
    VkSwapchainKHR swapchain,
    const std::uint32_t imageIndex,
    void** outImage)
{
    if (swapchain == nullptr || outImage == nullptr) {
        return false;
    }
    *outImage = nullptr;
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.swapchains.find(swapchain);
    if (it == capture.swapchains.end() || imageIndex >= it->second.images.size()) {
        return false;
    }
    *outImage = it->second.images[imageIndex];
    return *outImage != nullptr;
}

bool getRealSwapchainImage(
    VkSwapchainKHR swapchain,
    const std::uint32_t imageIndex,
    void** outImage)
{
    if (swapchain == nullptr || outImage == nullptr)
        return false;
    *outImage = nullptr;
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.swapchains.find(swapchain);
    if (it == capture.swapchains.end() || imageIndex >= it->second.realImages.size())
        return false;
    *outImage = it->second.realImages[imageIndex];
    return *outImage != nullptr;
}

bool releaseVirtualSwapchainImage(
    VkSwapchainKHR swapchain,
    const std::uint32_t imageIndex,
    VkSemaphore firstReleaseSemaphore,
    const std::uint64_t firstReleaseValue,
    VkSemaphore secondReleaseSemaphore,
    const std::uint64_t secondReleaseValue)
{
    if (!swapchain || (!firstReleaseSemaphore && !secondReleaseSemaphore) ||
        (firstReleaseValue != 0u && !firstReleaseSemaphore) ||
        (secondReleaseValue != 0u && !secondReleaseSemaphore) ||
        (firstReleaseSemaphore && firstReleaseValue == 0u) ||
        (secondReleaseSemaphore && secondReleaseValue == 0u))
        return false;

    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.swapchains.find(swapchain);
    if (it == capture.swapchains.end() || !it->second.virtualized ||
        imageIndex >= it->second.virtualSlots.size())
        return false;
    auto& slot = it->second.virtualSlots[imageIndex];
    if (!slot.acquired)
        return false;

    slot.acquired = false;
    slot.releaseSemaphores = {firstReleaseSemaphore, secondReleaseSemaphore};
    slot.releaseValues = {firstReleaseValue, secondReleaseValue};
    it->second.latestPresentedImageIndex = imageIndex;
    ++it->second.presentSerial;
    return true;
}

bool waitVirtualSwapchainReleaseDependencies(VkSwapchainKHR swapchain)
{
    if (!swapchain)
        return false;

    VkDevice device = nullptr;
    for (;;) {
        std::vector<VkSemaphore> semaphores;
        std::vector<std::uint64_t> values;
        {
            State& capture = state();
            std::lock_guard<std::mutex> lock(capture.mutex);
            const auto it = capture.swapchains.find(swapchain);
            if (it == capture.swapchains.end() || !it->second.virtualized)
                return false;
            device = it->second.device;
            for (const auto& slot : it->second.virtualSlots) {
                for (std::size_t dependency = 0; dependency < slot.releaseValues.size(); ++dependency) {
                    const VkSemaphore semaphore = slot.releaseSemaphores[dependency];
                    const std::uint64_t value = slot.releaseValues[dependency];
                    if (!semaphore || value == 0u)
                        continue;
                    auto existing = std::find(semaphores.begin(), semaphores.end(), semaphore);
                    if (existing == semaphores.end()) {
                        semaphores.push_back(semaphore);
                        values.push_back(value);
                    } else {
                        const auto index = static_cast<std::size_t>(existing - semaphores.begin());
                        values[index] = std::max(values[index], value);
                    }
                }
            }
        }
        if (semaphores.empty())
            return true;

        VirtualImageDeviceContext context{};
        if (!loadVirtualImageDeviceContext(device, context))
            return false;
        std::vector<vk::Semaphore> nativeSemaphores;
        nativeSemaphores.reserve(semaphores.size());
        for (VkSemaphore semaphore : semaphores)
            nativeSemaphores.push_back(reinterpret_cast<vk::Semaphore>(semaphore));
        const vk::SemaphoreWaitInfo waitInfo{
            .sType = vk::StructureTypeSemaphoreWaitInfo,
            .pNext = nullptr,
            .flags = 0u,
            .semaphoreCount = static_cast<std::uint32_t>(nativeSemaphores.size()),
            .pSemaphores = nativeSemaphores.data(),
            .pValues = values.data(),
        };
        if (context.functions.waitSemaphores(
                reinterpret_cast<vk::Device>(device), &waitInfo, ~std::uint64_t{0}) != vk::Success)
            return false;

        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(swapchain);
        if (it == capture.swapchains.end() || !it->second.virtualized)
            return false;
        for (auto& slot : it->second.virtualSlots) {
            for (std::size_t dependency = 0; dependency < slot.releaseValues.size(); ++dependency) {
                const VkSemaphore semaphore = slot.releaseSemaphores[dependency];
                const std::uint64_t value = slot.releaseValues[dependency];
                if (!semaphore || value == 0u)
                    continue;
                const auto waited = std::find(semaphores.begin(), semaphores.end(), semaphore);
                if (waited == semaphores.end())
                    continue;
                const auto index = static_cast<std::size_t>(waited - semaphores.begin());
                if (value <= values[index]) {
                    slot.releaseSemaphores[dependency] = nullptr;
                    slot.releaseValues[dependency] = 0u;
                }
            }
        }
    }
}

bool virtualSwapchainActive() noexcept
{
    return g_virtualSwapchainCount.load(std::memory_order_acquire) != 0u;
}

bool isVirtualSwapchainImage(VkImage image) noexcept
{
    if (!image || !virtualSwapchainActive())
        return false;
    for (auto& slot : g_fastVirtualImages) {
        if (slot.load(std::memory_order_acquire) == image)
            return true;
    }
    return false;
}

void associateInstance(VkDevice device, VkInstance instance, VkPhysicalDevice physicalDevice)
{
    if (device == nullptr) {
        return;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.devices.find(device);
    if (it == capture.devices.end()) {
        return;
    }
    if (instance != nullptr) {
        it->second.instance = instance;
    }
    if (physicalDevice != nullptr) {
        it->second.physicalDevice = physicalDevice;
        if (instance != nullptr) {
            capture.instanceByPhysicalDevice[physicalDevice] = instance;
        }
    }
    apusr::host_discovery::eventf(
        "vulkan", "device.associated",
        "device=%p instance=%p physical_device=%p source=frontend_metadata",
        device, instance, physicalDevice);
}

void requestAntiLag(const bool requested)
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.antiLagRequested = requested;
}

void requestFrameGeneration(const bool requested)
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.frameGenerationRequested = requested;
    if (!requested) {
        g_frameGenerationActive.store(false, std::memory_order_relaxed);
        capture.legacyOpticalFlowQueueRoleRequested = false;
    }
}

void setFrameGenerationActive(const bool active)
{
    if (!active) {
        g_frameGenerationActive.store(false, std::memory_order_relaxed);
        return;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    g_frameGenerationActive.store(capture.frameGenerationRequested, std::memory_order_relaxed);
}

void requestLegacyOpticalFlowQueueRole(const bool requested)
{
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    capture.legacyOpticalFlowQueueRoleRequested = requested && capture.frameGenerationRequested;
}

void getPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice,
    std::uint32_t* count,
    VkQueueFamilyPropertiesLoose* properties)
{
    const Dispatch dispatch = dispatchSnapshot();
    if (dispatch.getPhysicalDeviceQueueFamilyProperties == nullptr) {
        if (count != nullptr) {
            *count = 0u;
        }
        return;
    }

    dispatch.getPhysicalDeviceQueueFamilyProperties(physicalDevice, count, properties);
    if (properties == nullptr || count == nullptr || *count == 0u ||
        !frameGenerationRequestedSnapshot() || !legacyOpticalFlowQueueRoleRequestedSnapshot()) {
        return;
    }

    // Older Streamline Vulkan integrations can gate
    // DLSS-G state discovery on graphics, compute and optical-flow queue roles.
    // APUSR implements optical flow as ordinary Vulkan compute and does not
    // expose or execute VK_NV_optical_flow. The DLSS-G requirements path arms
    // this short pre-device compatibility window; expose one *selection role*
    // only on a graphics+compute family with at least four hardware queues.
    // vkCreateDevice independently re-checks the host's final requested count
    // and reserves APUSR's private presenter only if capacity still remains.
    // The NV extension stays absent and no NV optical-flow command becomes
    // callable through the alias.
    for (std::uint32_t i = 0; i < *count; ++i) {
        if ((properties[i].queueFlags & VkQueueOpticalFlowBitNv) != 0u) {
            return;
        }
    }

    std::uint32_t candidate = *count;
    std::uint32_t candidateCapacity = 0u;
    for (std::uint32_t i = 0; i < *count; ++i) {
        const std::uint32_t required = VkQueueGraphicsBit | VkQueueComputeBit;
        if ((properties[i].queueFlags & required) != required || properties[i].queueCount < 4u) {
            continue;
        }
        if (candidate == *count || properties[i].queueCount > candidateCapacity) {
            candidate = i;
            candidateCapacity = properties[i].queueCount;
        }
    }
    if (candidate == *count) {
        return;
    }

    properties[candidate].queueFlags |= VkQueueOpticalFlowBitNv;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.virtual_optical_flow_queue_role",
            "physical_device=%p family=%u capacity=%u nv_extension_advertised=0 role_only=1",
            physicalDevice, candidate, candidateCapacity);
    }
}

bool antiLagSupported(VkPhysicalDevice physicalDevice)
{
    if (physicalDevice == nullptr) {
        return false;
    }

    const Dispatch dispatch = dispatchSnapshot();
    if (dispatch.getPhysicalDeviceFeatures2 == nullptr ||
        dispatch.enumerateDeviceExtensionProperties == nullptr ||
        !hasDeviceExtension(dispatch, physicalDevice, kVkAmdAntiLagExtension)) {
        return false;
    }

    VkPhysicalDeviceAntiLagFeaturesAMDLoose antiLag{};
    VkPhysicalDeviceFeatures2Loose features{};
    features.pNext = &antiLag;
    dispatch.getPhysicalDeviceFeatures2(physicalDevice, &features);
    return antiLag.antiLag != 0u;
}

bool antiLagAvailable(VkDevice device)
{
    if (device == nullptr) {
        return false;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.devices.find(device);
    return it != capture.devices.end() && it->second.antiLagUpdate != nullptr;
}

bool antiLagInput(
    VkDevice device,
    const std::uint64_t frameIndex,
    const std::uint32_t mode,
    const std::uint32_t maxFps)
{
    if (device == nullptr || mode > APUSR_VK_HOST_ANTI_LAG_OFF) {
        return false;
    }

    VkAntiLagUpdateAMDFn update = nullptr;
    apusr::profile::StreamHandle profileStream{};
    apusr::profile::SampleDecision profileSample{};
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.devices.find(device);
        if (it == capture.devices.end() || it->second.antiLagUpdate == nullptr) {
            return false;
        }
        DeviceRecord& record = it->second;
        update = record.antiLagUpdate;
        if (!rememberAntiLagInput(record, AntiLagFrameRecord{
                .frameIndex = frameIndex,
                .mode = mode,
                .maxFps = maxFps,
                .valid = true,
            })) {
            return true;
        }
        record.lastAntiLagInputMode = mode;
        if (record.antiLagProfileStream && apusr::profile::cpuEnabled()) {
            const std::uint64_t config = apusr::profile::hashCombine(
                static_cast<std::uint64_t>(mode), static_cast<std::uint64_t>(maxFps));
            profileSample = record.antiLagInputProfileGate.decide(config, frameIndex);
            profileStream = profileSample.capture ? record.antiLagProfileStream : apusr::profile::StreamHandle{};
        }
    }

    VkAntiLagPresentationInfoAMDLoose presentation{};
    presentation.stage = kVkAntiLagStageInputAmd;
    presentation.frameIndex = frameIndex;
    VkAntiLagDataAMDLoose data{};
    data.mode = static_cast<std::int32_t>(mode);
    data.maxFPS = maxFps;
    data.pPresentationInfo = &presentation;
    if (profileStream) {
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = profileStream,
            .frameId = frameIndex,
            .name = "antilag.input_contract",
            .value = mode,
            .value2 = maxFps,
        });
    }
    {
        apusr::profile::CpuScope scope(
            profileStream, frameIndex, profileSample.sampleSequence, "antilag.input_update");
        update(device, &data);
    }
    return true;
}

bool antiLagArmPairedDisable(VkDevice device)
{
    if (device == nullptr)
        return false;

    VkAntiLagUpdateAMDFn update = nullptr;
    std::uint64_t frameIndex = 0u;
    apusr::profile::StreamHandle profileStream{};
    apusr::profile::SampleDecision profileSample{};
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.devices.find(device);
        if (it == capture.devices.end() || it->second.antiLagUpdate == nullptr)
            return false;

        DeviceRecord& record = it->second;
        if (record.antiLagDisablePairPending || record.lastAntiLagInputMode == APUSR_VK_HOST_ANTI_LAG_OFF)
            return true;

        update = record.antiLagUpdate;

        // A stale ON INPUT/PRESENT association must not survive the logical Off
        // transition and get emitted after the disabling contract. A prior
        // mode-only OFF experiment proved that pPresentationInfo == nullptr is
        // not safe on affected hosts, so replace the bookkeeping with one complete OFF pair:
        // INPUT now, PRESENT immediately before the next real vkQueuePresentKHR.
        record.antiLagFrames = {};
        record.antiLagInputOrder = {};
        record.antiLagInputHead = 0u;
        record.antiLagInputCount = 0u;
        record.pendingPresentFrames = {};
        record.pendingPresentHead = 0u;
        record.pendingPresentCount = 0u;

        frameIndex = record.nextAntiLagControlFrameIndex++;
        if (record.nextAntiLagControlFrameIndex < 0x8000000000000000ull)
            record.nextAntiLagControlFrameIndex = 0x8000000000000000ull;
        const AntiLagFrameRecord transition{
            .frameIndex = frameIndex,
            .mode = APUSR_VK_HOST_ANTI_LAG_OFF,
            .maxFps = 0u,
            .valid = true,
            .controlTransition = true,
        };
        (void)rememberAntiLagInput(record, transition);
        record.pendingPresentFrames[0] = frameIndex;
        record.pendingPresentHead = 0u;
        record.pendingPresentCount = 1u;
        record.lastAntiLagInputMode = APUSR_VK_HOST_ANTI_LAG_OFF;
        record.antiLagDisablePairPending = true;

        if (record.antiLagProfileStream && apusr::profile::cpuEnabled()) {
            const std::uint64_t config = apusr::profile::hashCombine(
                static_cast<std::uint64_t>(APUSR_VK_HOST_ANTI_LAG_OFF), 0u);
            profileSample = record.antiLagInputProfileGate.decide(config, frameIndex);
            profileStream = profileSample.capture ? record.antiLagProfileStream : apusr::profile::StreamHandle{};
        }
    }

    VkAntiLagPresentationInfoAMDLoose presentation{};
    presentation.stage = kVkAntiLagStageInputAmd;
    presentation.frameIndex = frameIndex;
    VkAntiLagDataAMDLoose data{};
    data.mode = static_cast<std::int32_t>(APUSR_VK_HOST_ANTI_LAG_OFF);
    data.maxFPS = 0u;
    data.pPresentationInfo = &presentation;
    if (profileStream) {
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = profileStream,
            .frameId = frameIndex,
            .name = "antilag.paired_disable_input",
            .value = APUSR_VK_HOST_ANTI_LAG_OFF,
        });
    }
    {
        apusr::profile::CpuScope scope(
            profileStream, frameIndex, profileSample.sampleSequence, "antilag.paired_disable_input_update");
        update(device, &data);
    }
    return true;
}

void antiLagSetPresentFrame(VkDevice device, const std::uint64_t frameIndex)
{
    if (device == nullptr) {
        return;
    }
    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    const auto it = capture.devices.find(device);
    if (it == capture.devices.end() || it->second.antiLagUpdate == nullptr) {
        return;
    }

    DeviceRecord& record = it->second;
    // Preserve marker order instead of storing only one pending frame, but do
    // not enqueue the same PCL PresentStart/OutOfBandPresentStart frame twice.
    // A duplicate would consume a later real vkQueuePresentKHR and break the
    // one INPUT -> one matching PRESENT contract.
    for (std::size_t i = 0; i < record.pendingPresentCount; ++i) {
        const std::size_t pos = (record.pendingPresentHead + i) % kAntiLagFrameHistory;
        if (record.pendingPresentFrames[pos] == frameIndex)
            return;
    }
    if (record.pendingPresentCount == kAntiLagFrameHistory) {
        record.pendingPresentHead = (record.pendingPresentHead + 1u) % kAntiLagFrameHistory;
        --record.pendingPresentCount;
    }
    const std::size_t tail = (record.pendingPresentHead + record.pendingPresentCount) % kAntiLagFrameHistory;
    record.pendingPresentFrames[tail] = frameIndex;
    ++record.pendingPresentCount;
}

namespace {

struct PresentQueueRoute {
    bool captured = false;
    VkQueuePresentKHRFn systemPresent = nullptr;
    VkDevice device = nullptr;
    VkGetPastPresentationTimingGOOGLEFn getPastPresentationTiming = nullptr;
    bool displayTimingProfilerOwned = false;
};

[[nodiscard]] PresentQueueRoute routePresentQueue(VkQueue queue)
{
    const bool fastCaptured = queue == g_fastCapturedPresentQueue.load(std::memory_order_acquire);
    if (fastCaptured && !apusr::profile::enabled()) {
        return {.captured = true};
    }

    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    PresentQueueRoute route{};
    route.systemPresent = capture.dispatch.queuePresent;
    const auto queueIt = capture.deviceByQueue.find(queue);
    if (queueIt == capture.deviceByQueue.end())
        return route;
    route.captured = true;
    route.device = queueIt->second;
    const auto deviceIt = capture.devices.find(route.device);
    if (deviceIt != capture.devices.end()) {
        route.getPastPresentationTiming = deviceIt->second.getPastPresentationTiming;
        route.displayTimingProfilerOwned = deviceIt->second.displayTimingProfilerOwned;
    }
    if (!fastCaptured) {
        // Adapt the fast path if a title legitimately presents on a different
        // captured queue. Single-queue titles without profiling still pay only
        // the atomic comparison.
        g_fastCapturedPresentQueue.store(queue, std::memory_order_release);
    }
    return route;
}

[[nodiscard]] bool shouldQueryDisplayTiming(const WsiPresentProfileBatch& batch) noexcept
{
    for (std::uint32_t index = 0u; index < batch.count; ++index) {
        const WsiProfileCapture& capture = index < kInlineWsiPresentProfiles
            ? batch.inlineCaptures[index]
            : batch.overflowCaptures[index - kInlineWsiPresentProfiles];
        if (capture.sample.capture && capture.sample.burstFrame == 0u)
            return true;
    }
    return false;
}

[[nodiscard]] constexpr std::uint32_t displayTimingPresentId(
    const std::uint64_t frameId,
    const bool generatedFrame) noexcept
{
    // Reserve bit 0 for generated/source identity and retain 31 bits of the
    // frame serial. At 60 Hz this remains unique for more than a year of
    // uninterrupted presentation, well beyond a meaningful timing window.
    return (static_cast<std::uint32_t>(frameId) & 0x7fffffffu) << 1u |
        (generatedFrame ? 1u : 0u);
}

void recordPastDisplayTimingForSwapchain(
    const PresentQueueRoute& route,
    const VkSwapchainKHR swapchain,
    const apusr::profile::StreamHandle stream) noexcept
{
    if (!route.displayTimingProfilerOwned || route.getPastPresentationTiming == nullptr ||
        route.device == nullptr || swapchain == nullptr || !stream)
        return;

    constexpr std::uint32_t kMaxTimingRecordsPerQuery = 256u;
    std::uint32_t count = 0u;
    VkResult result = route.getPastPresentationTiming(route.device, swapchain, &count, nullptr);
    if ((result != VkSuccess && result != VkIncomplete) || count == 0u)
        return;
    count = std::min(count, kMaxTimingRecordsPerQuery);
    std::vector<VkPastPresentationTimingGOOGLELoose> timings(count);
    result = route.getPastPresentationTiming(route.device, swapchain, &count, timings.data());
    if (result != VkSuccess && result != VkIncomplete)
        return;

    for (std::uint32_t timingIndex = 0u; timingIndex < count; ++timingIndex) {
        const auto& timing = timings[timingIndex];
        const bool generatedFrame = (timing.presentID & 1u) != 0u;
        const std::uint64_t frameId = timing.presentID >> 1u;
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = stream,
            .frameId = frameId,
            .name = "display.vk_actual_present_ns",
            .value = timing.actualPresentTime,
            .value2 = timing.earliestPresentTime,
            .flags = generatedFrame ? 1u : 0u,
        });
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = stream,
            .frameId = frameId,
            .name = "display.vk_present_margin_ns",
            .value = timing.presentMargin,
            .value2 = timing.desiredPresentTime,
            .flags = generatedFrame ? 1u : 0u,
        });
    }
}

void recordPastDisplayTiming(
    const PresentQueueRoute& route,
    const WsiPresentProfileBatch& batch,
    const VkPresentInfoKHRLoose* info) noexcept
{
    if (info == nullptr || info->pSwapchains == nullptr || !shouldQueryDisplayTiming(batch))
        return;

    const std::uint32_t swapchainCount = std::min(batch.count, info->swapchainCount);
    for (std::uint32_t index = 0u; index < swapchainCount; ++index) {
        const WsiProfileCapture& capture = index < kInlineWsiPresentProfiles
            ? batch.inlineCaptures[index]
            : batch.overflowCaptures[index - kInlineWsiPresentProfiles];
        recordPastDisplayTimingForSwapchain(route, info->pSwapchains[index], capture.stream);
    }
}

void rememberPresentedSwapchains(const void* presentInfo, const VkResult result)
{
    const auto* info = static_cast<const VkPresentInfoKHRLoose*>(presentInfo);
    if (info == nullptr || info->sType != VkStructureTypePresentInfoKhr ||
        info->swapchainCount == 0u || info->pSwapchains == nullptr || info->pImageIndices == nullptr) {
        return;
    }

    State& capture = state();
    std::lock_guard<std::mutex> lock(capture.mutex);
    for (std::uint32_t index = 0; index < info->swapchainCount; ++index) {
        const VkResult swapchainResult = info->pResults != nullptr ? info->pResults[index] : result;
        if (swapchainResult != VkSuccess && swapchainResult != VkSuboptimalKhr) {
            continue;
        }
        const auto it = capture.swapchains.find(info->pSwapchains[index]);
        if (it == capture.swapchains.end() || it->second.virtualized) {
            // Physical generated/source replays use the same native swapchain
            // handle as the public proxy.  Their real-WSI indices are private
            // implementation detail and must not corrupt the host-visible
            // virtual present serial/index. releaseVirtualSwapchainImage() is
            // the single logical-present accounting point for proxy mode.
            continue;
        }
        it->second.latestPresentedImageIndex = info->pImageIndices[index];
        ++it->second.presentSerial;
    }
}

[[nodiscard]] VkResult callSystemQueuePresent(VkQueue queue, const void* presentInfo)
{
    VkDevice device = nullptr;
    VkAntiLagUpdateAMDFn update = nullptr;
    VkQueuePresentKHRFn present = nullptr;
    AntiLagFrameRecord frame{};
    bool emitAntiLagPresent = false;
    apusr::profile::StreamHandle antiLagProfileStream{};
    apusr::profile::SampleDecision antiLagProfileSample{};
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto queueIt = capture.deviceByQueue.find(queue);
        if (queueIt == capture.deviceByQueue.end()) {
            present = capture.dispatch.queuePresent;
        } else {
            const auto deviceIt = capture.devices.find(queueIt->second);
            if (deviceIt == capture.devices.end()) {
                present = capture.dispatch.queuePresent;
            } else {
                DeviceRecord& record = deviceIt->second;
                present = record.queuePresent != nullptr ? record.queuePresent : capture.dispatch.queuePresent;
                if (record.antiLagUpdate != nullptr && selectAntiLagPresentFrame(record, frame)) {
                    device = record.device;
                    update = record.antiLagUpdate;
                    emitAntiLagPresent = true;
                    if (record.antiLagProfileStream && apusr::profile::cpuEnabled()) {
                        const std::uint64_t config = apusr::profile::hashCombine(
                            static_cast<std::uint64_t>(frame.mode), static_cast<std::uint64_t>(frame.maxFps));
                        antiLagProfileSample = record.antiLagPresentProfileGate.decide(config, frame.frameIndex);
                        if (antiLagProfileSample.capture)
                            antiLagProfileStream = record.antiLagProfileStream;
                    }
                }
            }
        }
    }

    if (emitAntiLagPresent) {
        VkAntiLagPresentationInfoAMDLoose presentation{};
        presentation.stage = kVkAntiLagStagePresentAmd;
        presentation.frameIndex = frame.frameIndex;
        VkAntiLagDataAMDLoose data{};
        data.mode = static_cast<std::int32_t>(frame.mode);
        data.maxFPS = frame.maxFps;
        data.pPresentationInfo = &presentation;
        if (antiLagProfileStream) {
            apusr::profile::recordCounter(apusr::profile::Counter{
                .stream = antiLagProfileStream,
                .frameId = frame.frameIndex,
                .name = frame.controlTransition ? "antilag.paired_disable_present" : "antilag.present_contract",
                .value = frame.mode,
                .value2 = frame.maxFps,
            });
        }
        {
            apusr::profile::CpuScope scope(
                antiLagProfileStream, frame.frameIndex, antiLagProfileSample.sampleSequence,
                "antilag.present_update");
            update(device, &data);
        }
    }
    if (present == nullptr) {
        return VkErrorInitializationFailed;
    }
    VkResult result = VkErrorInitializationFailed;
    {
        apusr::profile::CpuScope scope(
            antiLagProfileStream, frame.frameIndex, antiLagProfileSample.sampleSequence,
            "antilag.system_present");
        result = present(queue, presentInfo);
    }
    rememberPresentedSwapchains(presentInfo, result);
    return result;
}

} // namespace

VkResult acquireNextImageForFrameGeneration(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const std::uint64_t timeout,
    VkSemaphore semaphore,
    std::uint32_t* imageIndex)
{
    if (device == nullptr || swapchain == nullptr || semaphore == nullptr || imageIndex == nullptr)
        return VkErrorInitializationFailed;

    VkAcquireNextImageKHRFn acquire = nullptr;
    std::shared_ptr<std::mutex> hostMutex;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end())
            acquire = deviceIt->second.acquireNextImage;
        const auto swapchainIt = capture.swapchains.find(swapchain);
        if (swapchainIt != capture.swapchains.end())
            hostMutex = swapchainIt->second.realWsiHostMutex;
    }
    if (acquire == nullptr || !hostMutex)
        return VkErrorExtensionNotPresent;

    // This deliberately bypasses the public acquire hook and backing Streamline:
    // the Vulkan FG presentation worker owns this real WSI target exclusively
    // while copying a retained source or generated frame into it.
    VkResult result = VkErrorInitializationFailed;
    if (timeout == 0u) {
        // Zero-timeout acquisition is part of APUSR's fail-open contract. Do
        // not turn it into an application-thread stall merely because another
        // APUSR thread is inside a host-synchronized WSI call for this
        // swapchain; report the same "not immediately available" outcome.
        std::unique_lock<std::mutex> hostLock(*hostMutex, std::try_to_lock);
        if (!hostLock.owns_lock())
            return VkNotReady;
        result = acquire(device, swapchain, timeout, semaphore, nullptr, imageIndex);
    } else {
        std::lock_guard<std::mutex> hostLock(*hostMutex);
        result = acquire(device, swapchain, timeout, semaphore, nullptr, imageIndex);
    }
    if (result == VkSuccess || result == VkSuboptimalKhr) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(swapchain);
        if (it != capture.swapchains.end() && !it->second.virtualized) {
            // In proxy mode this is a private real-WSI acquire by APUSR's
            // presenter. Public acquire state belongs exclusively to the
            // virtual backbuffers returned to the application.
            it->second.latestAcquiredImageIndex = *imageIndex;
            ++it->second.acquireSerial;
        }
    }
    return result;
}

bool releaseRealSwapchainImage(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const std::uint32_t imageIndex)
{
    if (device == nullptr || swapchain == nullptr)
        return false;

    VkReleaseSwapchainImagesKHRFn release = nullptr;
    std::shared_ptr<std::mutex> hostMutex;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto deviceIt = capture.devices.find(device);
        const auto swapchainIt = capture.swapchains.find(swapchain);
        if (deviceIt == capture.devices.end() || swapchainIt == capture.swapchains.end() ||
            swapchainIt->second.device != device ||
            imageIndex >= swapchainIt->second.realImageCount ||
            !deviceIt->second.swapchainMaintenance1Enabled) {
            return false;
        }
        release = deviceIt->second.releaseSwapchainImages;
        hostMutex = swapchainIt->second.realWsiHostMutex;
    }
    if (release == nullptr || !hostMutex)
        return false;

    VkReleaseSwapchainImagesInfoKHRLoose info{};
    info.swapchain = swapchain;
    info.imageIndexCount = 1u;
    info.pImageIndices = &imageIndex;
    VkResult result = VkErrorInitializationFailed;
    {
        std::lock_guard<std::mutex> hostLock(*hostMutex);
        result = release(device, &info);
    }
    if (result != VkSuccess) {
        apusr::host_discovery::eventf(
            "vulkan", "framegen.real_wsi_release_failed",
            "device=%p swapchain=%p image_index=%u result=%d",
            device, swapchain, imageIndex, result);
        return false;
    }
    return true;
}

VkResult queuePresentDirect(VkQueue queue, const void* presentInfo)
{
    if (queue == nullptr || presentInfo == nullptr)
        return VkErrorInitializationFailed;

    VkQueuePresentKHRFn present = nullptr;
    std::shared_ptr<std::mutex> hostMutex;
    const auto* info = static_cast<const VkPresentInfoKHRLoose*>(presentInfo);
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto queueIt = capture.deviceByQueue.find(queue);
        if (queueIt != capture.deviceByQueue.end()) {
            const auto deviceIt = capture.devices.find(queueIt->second);
            if (deviceIt != capture.devices.end())
                present = deviceIt->second.queuePresent;
        }
        if (present == nullptr)
            present = capture.dispatch.queuePresent;
        if (info->sType == VkStructureTypePresentInfoKhr && info->swapchainCount == 1u && info->pSwapchains) {
            const auto swapchainIt = capture.swapchains.find(info->pSwapchains[0]);
            if (swapchainIt != capture.swapchains.end())
                hostMutex = swapchainIt->second.realWsiHostMutex;
        }
    }
    if (present == nullptr || !hostMutex)
        return VkErrorInitializationFailed;

    // Generated-frame presentation is APUSR-owned and must not consume the
    // backing Streamline chain or the game's Anti-Lag PRESENT marker.
    VkResult result = VkErrorInitializationFailed;
    {
        std::lock_guard<std::mutex> hostLock(*hostMutex);
        result = present(queue, presentInfo);
    }
    rememberPresentedSwapchains(presentInfo, result);
    return result;
}

VkResult queuePresentSource(VkQueue queue, const void* presentInfo)
{
    if (queue == nullptr || presentInfo == nullptr)
        return VkErrorInitializationFailed;

    // The source/real frame has already traversed backing Streamline. Finish
    // that chain at the real loader while preserving the queued Anti-Lag
    // PRESENT marker. This is the asynchronous equivalent of the normal
    // nested Streamline -> host-capture re-entry path.
    const auto* info = static_cast<const VkPresentInfoKHRLoose*>(presentInfo);
    if (info->sType != VkStructureTypePresentInfoKhr || info->swapchainCount != 1u || !info->pSwapchains)
        return VkErrorInitializationFailed;
    const auto hostMutex = realWsiHostMutexForSwapchain(info->pSwapchains[0]);
    if (!hostMutex)
        return VkErrorInitializationFailed;
    std::lock_guard<std::mutex> hostLock(*hostMutex);
    return callSystemQueuePresent(queue, presentInfo);
}

namespace {

VkResult queuePresentRelative(
    VkQueue queue,
    const void* presentInfo,
    const std::uint64_t minimumPreviousImageVisibleNs,
    const bool generatedFrame,
    bool* outScheduled)
{
    if (outScheduled != nullptr)
        *outScheduled = false;
    if (queue == nullptr || presentInfo == nullptr || outScheduled == nullptr)
        return VkErrorInitializationFailed;
    if (minimumPreviousImageVisibleNs == 0u)
        return VkSuccess;

    const auto* sourceInfo = static_cast<const VkPresentInfoKHRLoose*>(presentInfo);
    // APUSR's private presenter submits exactly one real swapchain at a time.
    // Do not manufacture mixed timing semantics for unrelated multi-swapchain
    // callers; they simply retain the universal host-timer path.
    if (sourceInfo->sType != VkStructureTypePresentInfoKhr ||
        sourceInfo->swapchainCount != 1u || sourceInfo->pSwapchains == nullptr)
        return VkSuccess;

    VkDevice device = nullptr;
    VkGetSwapchainTimeDomainPropertiesEXTFn getTimeDomains = nullptr;
    VkQueuePresentKHRFn directPresent = nullptr;
    std::shared_ptr<std::mutex> hostMutex;
    bool timingCapable = false;
    bool timingReady = false;
    std::int32_t timeDomain = 0;
    std::uint64_t timeDomainId = 0u;
    {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto swapchainIt = capture.swapchains.find(sourceInfo->pSwapchains[0]);
        if (swapchainIt == capture.swapchains.end())
            return VkSuccess;
        const SwapchainRecord& record = swapchainIt->second;
        timingCapable = record.presentTimingRelativeCapable;
        timingReady = record.presentTimingRelative;
        device = record.device;
        hostMutex = record.realWsiHostMutex;
        timeDomain = record.presentTimingTimeDomain;
        timeDomainId = record.presentTimingTimeDomainId;
        const auto deviceIt = capture.devices.find(device);
        if (deviceIt != capture.devices.end()) {
            getTimeDomains = deviceIt->second.getSwapchainTimeDomainProperties;
            directPresent = deviceIt->second.queuePresent != nullptr
                ? deviceIt->second.queuePresent
                : capture.dispatch.queuePresent;
        } else {
            directPresent = capture.dispatch.queuePresent;
        }
    }
    if (!timingCapable || !hostMutex)
        return VkSuccess;

    if (!timingReady) {
        std::int32_t resolvedDomain = 0;
        std::uint64_t resolvedDomainId = 0u;
        if (!queryRelativePresentTimingDomain(
                getTimeDomains, device, sourceInfo->pSwapchains[0], hostMutex,
                resolvedDomain, resolvedDomainId)) {
            return VkSuccess;
        }
        {
            State& capture = state();
            std::lock_guard<std::mutex> lock(capture.mutex);
            const auto swapchainIt = capture.swapchains.find(sourceInfo->pSwapchains[0]);
            if (swapchainIt == capture.swapchains.end())
                return VkSuccess;
            swapchainIt->second.presentTimingRelative = true;
            swapchainIt->second.presentTimingTimeDomain = resolvedDomain;
            swapchainIt->second.presentTimingTimeDomainId = resolvedDomainId;
        }
        timeDomain = resolvedDomain;
        timeDomainId = resolvedDomainId;
        apusr::host_discovery::eventf(
            "vulkan", "framegen.present_timing_domain_ready",
            "device=%p swapchain=%p domain=%d domain_id=%llu",
            device, sourceInfo->pSwapchains[0], timeDomain,
            static_cast<unsigned long long>(timeDomainId));
    }

    VkPresentTimingInfoEXTLoose timing{};
    timing.flags = VkPresentTimingInfoPresentAtRelativeTimeBitExt;
    timing.targetTime = minimumPreviousImageVisibleNs;
    timing.timeDomainId = timeDomainId;
    timing.presentStageQueries = 0u;
    // Stage-local domains require a concrete stage even for relative target
    // durations. Scheduling itself targets first-pixel-visible, so use that
    // same stage and avoid any cross-domain calibration on the hot path.
    if (timeDomain == VkTimeDomainPresentStageLocalExt)
        timing.targetTimeDomainPresentStage = VkPresentStageImageFirstPixelVisibleBitExt;

    VkPresentTimingsInfoEXTLoose timings{};
    timings.pNext = sourceInfo->pNext;
    timings.swapchainCount = 1u;
    timings.pTimingInfos = &timing;

    VkPresentInfoKHRLoose timedPresent = *sourceInfo;
    timedPresent.pNext = &timings;
    *outScheduled = true;

    if (generatedFrame) {
        // Generated presentation is APUSR-owned. It must bypass the title's
        // queued Anti-Lag PRESENT marker exactly like queuePresentDirect(); only
        // the real/source present is allowed to consume that marker.
        if (directPresent == nullptr)
            return VkErrorInitializationFailed;
        VkResult result = VkErrorInitializationFailed;
        {
            std::lock_guard<std::mutex> hostLock(*hostMutex);
            result = directPresent(queue, &timedPresent);
        }
        rememberPresentedSwapchains(&timedPresent, result);
        return result;
    }

    std::lock_guard<std::mutex> hostLock(*hostMutex);
    return callSystemQueuePresent(queue, &timedPresent);
}

} // namespace

VkResult queuePresentSourceRelative(
    VkQueue queue,
    const void* presentInfo,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled)
{
    return queuePresentRelative(
        queue, presentInfo, minimumPreviousImageVisibleNs, false, outScheduled);
}

VkResult queuePresentGeneratedRelative(
    VkQueue queue,
    const void* presentInfo,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled)
{
    return queuePresentRelative(
        queue, presentInfo, minimumPreviousImageVisibleNs, true, outScheduled);
}

VkResult queuePresentFrameGeneration(
    VkQueue queue,
    const void* presentInfo,
    const std::uint64_t frameId,
    const std::uint32_t flags)
{
    if (queue == nullptr || presentInfo == nullptr)
        return VkErrorInitializationFailed;

    const bool generatedFrame = (flags & APUSR_VK_HOST_PRESENT_GENERATED) != 0u;
    const bool profileCapture = (flags & APUSR_VK_HOST_PRESENT_PROFILE_CAPTURE) != 0u &&
        apusr::profile::enabled();
    if (!profileCapture) {
        // Preserve the pre-profiler physical-present hot path exactly when this
        // frame is not part of a sampled burst. In particular, do not take the
        // host-capture state mutex or construct timing-chain scratch storage.
        return generatedFrame
            ? queuePresentDirect(queue, presentInfo)
            : queuePresentSource(queue, presentInfo);
    }

    const auto* info = static_cast<const VkPresentInfoKHRLoose*>(presentInfo);
    const PresentQueueRoute route = routePresentQueue(queue);

    std::array<apusr::profile::StreamHandle, kInlineWsiPresentProfiles> inlineStreams{};
    std::vector<apusr::profile::StreamHandle> overflowStreams;
    apusr::profile::StreamHandle* streams = inlineStreams.data();
    bool canProfileDisplayTiming = profileCapture && route.captured && route.displayTimingProfilerOwned &&
        info != nullptr && info->sType == VkStructureTypePresentInfoKhr && info->swapchainCount != 0u &&
        info->pSwapchains != nullptr;
    if (canProfileDisplayTiming && info->swapchainCount > kInlineWsiPresentProfiles) {
        overflowStreams.resize(info->swapchainCount);
        streams = overflowStreams.data();
    }
    if (canProfileDisplayTiming) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        for (std::uint32_t index = 0u; index < info->swapchainCount; ++index) {
            const auto it = capture.swapchains.find(info->pSwapchains[index]);
            if (it == capture.swapchains.end() || !it->second.profileStream) {
                canProfileDisplayTiming = false;
                break;
            }
            streams[index] = it->second.profileStream;
        }
    }

    std::array<VkPresentTimeGOOGLELoose, kInlineWsiPresentProfiles> inlinePresentTimes{};
    std::vector<VkPresentTimeGOOGLELoose> overflowPresentTimes;
    VkPresentTimesInfoGOOGLELoose profilePresentTimes{};
    VkPresentInfoKHRLoose forwardedInfo{};
    const void* effectivePresentInfo = presentInfo;
    if (canProfileDisplayTiming) {
        VkPresentTimeGOOGLELoose* times = inlinePresentTimes.data();
        if (info->swapchainCount > kInlineWsiPresentProfiles) {
            overflowPresentTimes.resize(info->swapchainCount);
            times = overflowPresentTimes.data();
        }
        const std::uint32_t presentId = displayTimingPresentId(frameId, generatedFrame);
        for (std::uint32_t index = 0u; index < info->swapchainCount; ++index) {
            times[index].presentID = presentId;
            times[index].desiredPresentTime = 0u;
        }
        profilePresentTimes.sType = VkStructureTypePresentTimesInfoGoogle;
        profilePresentTimes.pNext = info->pNext;
        profilePresentTimes.swapchainCount = info->swapchainCount;
        profilePresentTimes.pTimes = times;
        forwardedInfo = *info;
        forwardedInfo.pNext = &profilePresentTimes;
        effectivePresentInfo = &forwardedInfo;
    }

    // Generated frames intentionally bypass the title's backing Streamline and
    // Anti-Lag PRESENT marker. Source frames finish that chain exactly once.
    const VkResult result = generatedFrame
        ? queuePresentDirect(queue, effectivePresentInfo)
        : queuePresentSource(queue, effectivePresentInfo);

    if (canProfileDisplayTiming) {
        for (std::uint32_t index = 0u; index < info->swapchainCount; ++index)
            recordPastDisplayTimingForSwapchain(route, info->pSwapchains[index], streams[index]);
    }
    return result;
}

VkResult queuePresentRelativeFrameGeneration(
    VkQueue queue,
    const void* presentInfo,
    const std::uint64_t frameId,
    const std::uint32_t flags,
    const std::uint64_t minimumPreviousImageVisibleNs,
    const bool generatedFrame,
    bool* outScheduled)
{
    const bool profileCapture = (flags & APUSR_VK_HOST_PRESENT_PROFILE_CAPTURE) != 0u &&
        apusr::profile::enabled();
    if (!profileCapture) {
        return generatedFrame
            ? queuePresentGeneratedRelative(
                queue, presentInfo, minimumPreviousImageVisibleNs, outScheduled)
            : queuePresentSourceRelative(
                queue, presentInfo, minimumPreviousImageVisibleNs, outScheduled);
    }
    if (queue == nullptr || presentInfo == nullptr || outScheduled == nullptr)
        return VkErrorInitializationFailed;

    const auto* info = static_cast<const VkPresentInfoKHRLoose*>(presentInfo);
    const PresentQueueRoute route = routePresentQueue(queue);
    bool canProfileDisplayTiming = route.captured && route.displayTimingProfilerOwned &&
        info->sType == VkStructureTypePresentInfoKhr && info->swapchainCount == 1u &&
        info->pSwapchains != nullptr;
    apusr::profile::StreamHandle stream{};
    if (canProfileDisplayTiming) {
        State& capture = state();
        std::lock_guard<std::mutex> lock(capture.mutex);
        const auto it = capture.swapchains.find(info->pSwapchains[0]);
        if (it == capture.swapchains.end() || !it->second.profileStream) {
            canProfileDisplayTiming = false;
        } else {
            stream = it->second.profileStream;
        }
    }

    VkPresentTimeGOOGLELoose presentTime{};
    VkPresentTimesInfoGOOGLELoose profilePresentTimes{};
    VkPresentInfoKHRLoose forwardedInfo{};
    const void* effectivePresentInfo = presentInfo;
    if (canProfileDisplayTiming) {
        presentTime.presentID = displayTimingPresentId(frameId, generatedFrame);
        presentTime.desiredPresentTime = 0u;
        profilePresentTimes.sType = VkStructureTypePresentTimesInfoGoogle;
        profilePresentTimes.pNext = info->pNext;
        profilePresentTimes.swapchainCount = 1u;
        profilePresentTimes.pTimes = &presentTime;
        forwardedInfo = *info;
        forwardedInfo.pNext = &profilePresentTimes;
        effectivePresentInfo = &forwardedInfo;
    }

    const VkResult result = generatedFrame
        ? queuePresentGeneratedRelative(
            queue, effectivePresentInfo, minimumPreviousImageVisibleNs, outScheduled)
        : queuePresentSourceRelative(
            queue, effectivePresentInfo, minimumPreviousImageVisibleNs, outScheduled);
    if (canProfileDisplayTiming && *outScheduled)
        recordPastDisplayTimingForSwapchain(route, info->pSwapchains[0], stream);
    return result;
}


VkResult queuePresentSourceRelativeFrameGeneration(
    VkQueue queue,
    const void* presentInfo,
    const std::uint64_t frameId,
    const std::uint32_t flags,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled)
{
    return queuePresentRelativeFrameGeneration(
        queue, presentInfo, frameId, flags, minimumPreviousImageVisibleNs, false, outScheduled);
}

VkResult queuePresentGeneratedRelativeFrameGeneration(
    VkQueue queue,
    const void* presentInfo,
    const std::uint64_t frameId,
    const std::uint32_t flags,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled)
{
    return queuePresentRelativeFrameGeneration(
        queue, presentInfo, frameId, flags, minimumPreviousImageVisibleNs, true, outScheduled);
}

void setQueuePresentInterceptor(void* queuePresentProc)
{
    g_queuePresentInterceptor.store(
        reinterpret_cast<VkQueuePresentKHRFn>(queuePresentProc),
        std::memory_order_release);
}

void setSwapchainDestroyInterceptor(void* destroySwapchainProc)
{
    g_swapchainDestroyInterceptor.store(
        reinterpret_cast<VkSwapchainDestroyInterceptorFn>(destroySwapchainProc),
        std::memory_order_release);
}

VkResult queuePresent(VkQueue queue, const void* presentInfo)
{
    if (queue == nullptr || presentInfo == nullptr) {
        return VkErrorInitializationFailed;
    }

    static thread_local bool insideBackingPresent = false;
    if (insideBackingPresent) {
        return callSystemQueuePresent(queue, presentInfo);
    }

    struct BackingPresentScope {
        bool& flag;
        explicit BackingPresentScope(bool& value) : flag(value) { flag = true; }
        ~BackingPresentScope() { flag = false; }
    };

    const PresentQueueRoute route = routePresentQueue(queue);
    if (!route.captured) {
        // Host capture is process-wide. Never route unrelated Vulkan devices
        // through the Streamline instance that APUSR owns; pass them straight
        // to the original loader entry point instead.
        return route.systemPresent != nullptr
            ? route.systemPresent(queue, presentInfo)
            : VkErrorInitializationFailed;
    }

    const VkQueuePresentKHRFn interceptor =
        g_queuePresentInterceptor.load(std::memory_order_acquire);
    if (!apusr::profile::enabled()) {
        // Compiled-in-but-disabled profiling should retain the old present hot
        // path: one interceptor load plus the existing backing-present guard.
        if (interceptor == nullptr)
            return callSystemQueuePresent(queue, presentInfo);
        BackingPresentScope scope(insideBackingPresent);
        return interceptor(queue, presentInfo);
    }

    const auto* wsiPresentInfo = static_cast<const VkPresentInfoKHRLoose*>(presentInfo);
    WsiPresentProfileBatch wsiProfileBatch = beginWsiPresentProfileBatch(wsiPresentInfo);
    const std::uint64_t wsiProfileBeginTick = wsiProfileBatch.count != 0u && apusr::profile::cpuEnabled()
        ? apusr::profile::cpuTick() : 0u;

    // VK_GOOGLE_display_timing returns records only for presents carrying a
    // VkPresentTimesInfoGOOGLE identifier. Inject identifiers only when APUSR
    // itself enabled the extension; if the title enabled it, the one-shot
    // records remain entirely title-owned. desiredPresentTime=0 preserves the
    // presentation engine's normal scheduling policy. Keep the common <=4
    // swapchain path allocation-free.
    std::array<VkPresentTimeGOOGLELoose, kInlineWsiPresentProfiles> inlinePresentTimes{};
    std::vector<VkPresentTimeGOOGLELoose> overflowPresentTimes;
    VkPresentTimesInfoGOOGLELoose profilePresentTimes{};
    VkPresentInfoKHRLoose forwardedPresentInfo{};
    const VkPresentInfoKHRLoose* effectivePresentInfo = wsiPresentInfo;
    const void* effectivePresentInfoVoid = presentInfo;
    bool allPresentStreamsCaptured = wsiPresentInfo != nullptr &&
        wsiPresentInfo->swapchainCount == wsiProfileBatch.count;
    for (std::uint32_t index = 0u; allPresentStreamsCaptured && index < wsiProfileBatch.count; ++index) {
        const WsiProfileCapture& capture = wsiProfileBatch.at(index);
        allPresentStreamsCaptured = static_cast<bool>(capture.stream) && !capture.virtualized;
    }
    if (route.displayTimingProfilerOwned && wsiPresentInfo != nullptr &&
        wsiPresentInfo->sType == VkStructureTypePresentInfoKhr && wsiPresentInfo->swapchainCount != 0u &&
        allPresentStreamsCaptured) {
        VkPresentTimeGOOGLELoose* times = inlinePresentTimes.data();
        if (wsiPresentInfo->swapchainCount > kInlineWsiPresentProfiles) {
            overflowPresentTimes.resize(wsiPresentInfo->swapchainCount);
            times = overflowPresentTimes.data();
        }
        for (std::uint32_t index = 0u; index < wsiPresentInfo->swapchainCount; ++index) {
            const WsiProfileCapture& capture = wsiProfileBatch.at(index);
            times[index].presentID = displayTimingPresentId(capture.frameId, false);
            times[index].desiredPresentTime = 0u;
        }
        profilePresentTimes.sType = VkStructureTypePresentTimesInfoGoogle;
        profilePresentTimes.pNext = wsiPresentInfo->pNext;
        profilePresentTimes.swapchainCount = wsiPresentInfo->swapchainCount;
        profilePresentTimes.pTimes = times;
        forwardedPresentInfo = *wsiPresentInfo;
        forwardedPresentInfo.pNext = &profilePresentTimes;
        effectivePresentInfo = &forwardedPresentInfo;
        effectivePresentInfoVoid = effectivePresentInfo;
    }

    if (interceptor == nullptr) {
        const VkResult result = callSystemQueuePresent(queue, effectivePresentInfoVoid);
        const std::uint64_t wsiProfileEndTick = wsiProfileBatch.count != 0u && apusr::profile::cpuEnabled()
            ? apusr::profile::cpuTick() : 0u;
        finishWsiPresentProfileBatch(
            wsiProfileBatch, queue, effectivePresentInfo, wsiProfileBeginTick, wsiProfileEndTick, result);
        recordPastDisplayTiming(route, wsiProfileBatch, effectivePresentInfo);
        return result;
    }

    BackingPresentScope scope(insideBackingPresent);
    const VkResult result = interceptor(queue, effectivePresentInfoVoid);
    const std::uint64_t wsiProfileEndTick = wsiProfileBatch.count != 0u && apusr::profile::cpuEnabled()
        ? apusr::profile::cpuTick() : 0u;
    finishWsiPresentProfileBatch(
        wsiProfileBatch, queue, effectivePresentInfo, wsiProfileBeginTick, wsiProfileEndTick, result);
    recordPastDisplayTiming(route, wsiProfileBatch, effectivePresentInfo);
    return result;
}

} // namespace apusr::vulkan_host_capture
