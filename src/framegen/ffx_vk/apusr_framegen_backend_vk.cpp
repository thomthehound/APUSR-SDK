// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_backend_vk.h"
#include <apusr/apusr_profiler.h>

#include "apusr_framegen_vk_spirv.h"
#include "../algorithm/apusr_framegen_algorithm_types.h"
#include "../../backends/gfx_vulkan/apusr_vulkan_minimal.h"
#include "../../backends/gfx_vulkan/apusr_spirv_metadata.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <new>
#include <string>

#if !defined(_WIN32)
#include <time.h>
#endif

namespace {

namespace vk = apusr::backends::vk;
namespace shaderpack = apusr::framegen::vk;

constexpr std::uint32_t kCbvBindingBase = 0;
constexpr std::uint32_t kSrvBindingBase = 32;
constexpr std::uint32_t kUavBindingBase = 64;
constexpr std::uint32_t kSamplerBindingBase = 96;
constexpr std::uint32_t kDescriptorSetsPerFrame = 64;
constexpr std::uint32_t kMaxImageMips = 32;
constexpr std::uint32_t kMaxLayoutBindings =
    APUSR_FRAMEGEN_MAX_SRVS * 2 + APUSR_FRAMEGEN_MAX_UAVS * 2 +
    APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS + APUSR_FRAMEGEN_MAX_SAMPLERS;

constexpr std::uint32_t kFiForceWave64 = 1u << 3;
constexpr std::uint32_t kOfForceWave64 = 1u << 0;
constexpr vk::ImageLayout kAutomaticImageLayout = std::numeric_limits<vk::ImageLayout>::min();

constexpr std::uint32_t kFramegenGpuProfileRingSlots = 8u;
constexpr std::uint32_t kFramegenGpuProfileQueriesPerSlot = 256u;
constexpr std::uint32_t kFramegenGpuProfileBurstFrames = 3u;
constexpr auto kFramegenGpuProfileSampleInterval = std::chrono::seconds(1);

std::atomic<std::uint64_t> gFramegenGpuProfileSequence{0u};
std::atomic<std::uint64_t> gFramegenGpuProfileBurstSequence{0u};
std::atomic<std::uint64_t> gFramegenGpuProfileRuntimeSequence{0u};
std::mutex gFramegenGpuProfileFileMutex;

bool framegenLegacyGpuProfileEnabled() noexcept
{
    static const bool enabled = []() noexcept {
        const char* value = std::getenv("APUSR_FRAMEGEN_VULKAN_GPU_TIMESTAMPS");
        if (!value || value[0] == '\0')
            return false;
        const char c = value[0];
        return c != '0' && c != 'n' && c != 'N' && c != 'f' && c != 'F' && c != 'o' && c != 'O';
    }();
    return enabled;
}

bool framegenGpuProfileEnabled() noexcept
{
    return framegenLegacyGpuProfileEnabled() || apusr::profile::gpuEnabled();
}

std::filesystem::path framegenGpuProfilePath()
{
    if (const char* explicitPath = std::getenv("APUSR_FRAMEGEN_VULKAN_GPU_TIMESTAMPS_FILE");
        explicitPath && explicitPath[0] != '\0')
        return std::filesystem::path(explicitPath);
    if (const char* dir = std::getenv("APUSR_LOG_DIR"); dir && dir[0] != '\0')
        return std::filesystem::path(dir) / "apusr_framegen_vulkan_gpu_timestamps.csv";
#if defined(_WIN32)
    return std::filesystem::path("C:/NGXLogs/APUSR/apusr_framegen_vulkan_gpu_timestamps.csv");
#else
    return std::filesystem::path("/tmp/APUSR/apusr_framegen_vulkan_gpu_timestamps.csv");
#endif
}

std::string narrowPipelineName(const wchar_t* name)
{
    std::string result;
    if (!name)
        return result;
    while (*name) {
        const wchar_t c = *name++;
        result.push_back(c >= 0x20 && c <= 0x7e ? static_cast<char>(c) : '?');
    }
    return result;
}

static_assert(sizeof(ApusrFramegenGpuJob) <= 1024, "Queued APUSR FrameGen GPU jobs must stay compact.");

std::size_t alignUp(const std::size_t value, const std::size_t alignment) noexcept
{
    return alignment > 1 ? (value + alignment - 1) / alignment * alignment : value;
}

[[nodiscard]] constexpr vk::TimeDomain profilerHostTimeDomain() noexcept
{
#if defined(_WIN32)
    return vk::TimeDomainQueryPerformanceCounter;
#elif defined(CLOCK_MONOTONIC_RAW)
    return vk::TimeDomainClockMonotonicRaw;
#else
    return vk::TimeDomainClockMonotonic;
#endif
}

[[nodiscard]] std::uint64_t timestampDelta(
    const std::uint64_t begin,
    const std::uint64_t end,
    const std::uint32_t validBits) noexcept
{
    if (validBits == 0u || validBits >= 64u)
        return end >= begin ? end - begin : 0u;
    const std::uint64_t mask = (1ull << validBits) - 1ull;
    return (end - begin) & mask;
}

struct VulkanFunctions {
    vk::PfnGetInstanceProcAddr getInstanceProcAddr = nullptr;
    vk::PfnGetDeviceProcAddr getDeviceProcAddr = nullptr;
    vk::PfnGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    vk::PfnGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    vk::PfnGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties = nullptr;
    vk::PfnGetPhysicalDeviceCalibrateableTimeDomains getPhysicalDeviceCalibrateableTimeDomains = nullptr;
    vk::PfnGetCalibratedTimestamps getCalibratedTimestamps = nullptr;
    vk::PfnCreateQueryPool createQueryPool = nullptr;
    vk::PfnDestroyQueryPool destroyQueryPool = nullptr;
    vk::PfnGetQueryPoolResults getQueryPoolResults = nullptr;
    vk::PfnCmdResetQueryPool cmdResetQueryPool = nullptr;
    vk::PfnCmdWriteTimestamp cmdWriteTimestamp = nullptr;
    vk::PfnCreateBuffer createBuffer = nullptr;
    vk::PfnDestroyBuffer destroyBuffer = nullptr;
    vk::PfnGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    vk::PfnBindBufferMemory bindBufferMemory = nullptr;
    vk::PfnCreateImage createImage = nullptr;
    vk::PfnDestroyImage destroyImage = nullptr;
    vk::PfnGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    vk::PfnBindImageMemory bindImageMemory = nullptr;
    vk::PfnAllocateMemory allocateMemory = nullptr;
    vk::PfnFreeMemory freeMemory = nullptr;
    vk::PfnMapMemory mapMemory = nullptr;
    vk::PfnUnmapMemory unmapMemory = nullptr;
    vk::PfnCreateImageView createImageView = nullptr;
    vk::PfnDestroyImageView destroyImageView = nullptr;
    vk::PfnCreateShaderModule createShaderModule = nullptr;
    vk::PfnDestroyShaderModule destroyShaderModule = nullptr;
    vk::PfnCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    vk::PfnDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    vk::PfnCreatePipelineLayout createPipelineLayout = nullptr;
    vk::PfnDestroyPipelineLayout destroyPipelineLayout = nullptr;
    vk::PfnCreateComputePipelines createComputePipelines = nullptr;
    vk::PfnDestroyPipeline destroyPipeline = nullptr;
    vk::PfnCreateDescriptorPool createDescriptorPool = nullptr;
    vk::PfnDestroyDescriptorPool destroyDescriptorPool = nullptr;
    vk::PfnResetDescriptorPool resetDescriptorPool = nullptr;
    vk::PfnAllocateDescriptorSets allocateDescriptorSets = nullptr;
    vk::PfnUpdateDescriptorSets updateDescriptorSets = nullptr;
    vk::PfnCreateSampler createSampler = nullptr;
    vk::PfnDestroySampler destroySampler = nullptr;
    vk::PfnCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    vk::PfnCmdClearColorImage cmdClearColorImage = nullptr;
    vk::PfnCmdCopyBuffer cmdCopyBuffer = nullptr;
    vk::PfnCmdCopyBufferToImage cmdCopyBufferToImage = nullptr;
    vk::PfnCmdCopyImage cmdCopyImage = nullptr;
    vk::PfnCmdFillBuffer cmdFillBuffer = nullptr;
    vk::PfnCmdBindPipeline cmdBindPipeline = nullptr;
    vk::PfnCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    vk::PfnCmdDispatch cmdDispatch = nullptr;
};

struct NativeResource {
    ApusrFramegenResourceDescription description{};
    ApusrFramegenResourceState initialState = APUSR_FRAMEGEN_RESOURCE_STATE_COMMON;
    ApusrFramegenResourceState currentState = APUSR_FRAMEGEN_RESOURCE_STATE_COMMON;
    vk::ImageLayout initialLayout = vk::ImageLayoutUndefined;
    vk::ImageLayout layout = vk::ImageLayoutUndefined;
    vk::Format nativeImageFormat = 0;
    vk::ImageSubresourceRange subresourceRange{vk::ImageAspectColorBit, 0, 1, 0, 1};
    vk::Buffer buffer = nullptr;
    vk::Image image = nullptr;
    vk::ImageView srvView = nullptr;
    std::array<vk::ImageView, kMaxImageMips> uavViews{};
    vk::DeviceMemory memory = nullptr;
    std::uint64_t allocationSize = 0;
    std::uint32_t mipCount = 1;
    bool owned = false;
    bool dynamic = false;
    bool upload = false;
    bool ownsSrvView = false;
    bool preserveGeneralReadLayout = false;
    ApusrFramegenVulkanExternalResourceMetadata exportMetadata{};
};

struct EffectContext {
    std::uint32_t nextStaticResource = 0;
    std::uint32_t nextDynamicResource = 0;
    std::uint32_t frameSlot = 0;
    bool active = false;
    ApusrFramegenMemoryUsage memoryUsage{};
};

struct SamplerCacheEntry {
    ApusrFramegenSamplerDescription description{};
    vk::Sampler sampler = nullptr;
    bool active = false;
};


constexpr std::uint32_t kFramegenGpuProfileMaxDispatches =
    (kFramegenGpuProfileQueriesPerSlot - 2u) / 3u;

struct FramegenGpuProfileDispatch {
    char name[96]{};
    std::uint32_t effect = 0u;
    std::uint32_t passId = 0u;
    std::uint32_t permutationOptions = 0u;
    std::uint32_t shaderBytes = 0u;
    std::uint32_t requiredSubgroupSize = 0u;
    std::uint32_t profileLevel = UINT32_MAX;
    std::uint32_t groupsX = 0u;
    std::uint32_t groupsY = 0u;
    std::uint32_t groupsZ = 0u;
    std::uint32_t barrierCalls = 0u;
    std::uint32_t imageBarriers = 0u;
    std::uint32_t bufferBarriers = 0u;
    std::uint32_t setupBeginQuery = 0u;
    std::uint32_t dispatchBeginQuery = 0u;
    std::uint32_t endQuery = 0u;
    std::uint64_t cpuSetupBeginNs = 0u;
    std::uint64_t cpuDispatchBeginNs = 0u;
    std::uint64_t cpuEndNs = 0u;
};

struct FramegenGpuProfileFrame {
    bool valid = false;
    std::uint64_t sequence = 0u;
    std::uint64_t sampleSequence = 0u;
    std::uint64_t configurationHash = 0u;
    bool unifiedProfile = false;
    bool configurationChanged = false;
    bool failed = false;
    std::uint64_t runtimeSequence = 0u;
    std::uint64_t burstSequence = 0u;
    std::uint32_t burstFrame = 0u;
    std::uint64_t frameId = 0u;
    std::uint32_t renderWidth = 0u;
    std::uint32_t renderHeight = 0u;
    std::uint32_t displayWidth = 0u;
    std::uint32_t displayHeight = 0u;
    std::int32_t interpolationX = 0;
    std::int32_t interpolationY = 0;
    std::int32_t interpolationWidth = 0;
    std::int32_t interpolationHeight = 0;
    float frameTimeDeltaMs = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionVectorScaleX = 0.0f;
    float motionVectorScaleY = 0.0f;
    std::uint32_t prepareFlags = 0u;
    std::uint32_t dispatchFlags = 0u;
    bool hasHudlessColor = false;
    bool motionVectorsAtDisplayResolution = false;
    bool motionVectorsJittered = false;
    bool hdr = false;
    bool reset = false;
    ApusrFramegenVulkanGpuProfilePhase phase = APUSR_FRAMEGEN_VULKAN_GPU_PROFILE_COMPUTE;
    apusr::profile::StreamHandle stream{};
    std::uint64_t cpuRecordNs = 0u;
    std::uint32_t frameBeginQuery = 0u;
    std::uint32_t frameEndQuery = 0u;
    std::uint32_t queryCount = 0u;
    std::uint32_t dispatchCount = 0u;
    std::uint32_t clearJobs = 0u;
    std::uint32_t copyJobs = 0u;
    std::uint32_t barrierJobs = 0u;
    std::uint32_t discardJobs = 0u;
    std::uint32_t totalBarrierCalls = 0u;
    std::uint32_t totalImageBarriers = 0u;
    std::uint32_t totalBufferBarriers = 0u;
    std::array<FramegenGpuProfileDispatch, kFramegenGpuProfileMaxDispatches> dispatches{};
};

struct FramegenGpuProfileQueryResult {
    std::uint64_t timestamp = 0u;
    std::uint64_t availability = 0u;
};

struct FramegenGpuProfileContext {
    bool active = false;
    std::uint64_t runtimeSequence = 0u;
    double timestampPeriodNs = 0.0;
    std::uint32_t timestampValidBits = 64u;
    vk::TimeDomain hostTimeDomain = profilerHostTimeDomain();
    bool clockCalibrationAvailable = false;
    std::uint32_t subgroupSize = 0u;
    std::uint32_t subgroupSizeMin = 0u;
    std::uint32_t subgroupSizeMax = 0u;
    bool subgroupSizeControlSupported = false;
    bool fp16Supported = false;
    std::uint32_t nextSlot = 0u;
    std::uint32_t activeSlot = UINT32_MAX;
    std::uint32_t activeQueryCursor = 0u;
    std::uint32_t burstRemaining = kFramegenGpuProfileBurstFrames;
    std::uint64_t currentBurstSequence = 0u;
    std::uint64_t nextSampleNs = 0u;
    std::uint64_t activeCpuStartNs = 0u;
    vk::CommandBuffer activeCommandBuffer = nullptr;
    FramegenGpuProfileDispatch* activeDispatch = nullptr;
    std::array<vk::QueryPool, kFramegenGpuProfileRingSlots> pools{};
    std::array<FramegenGpuProfileFrame, kFramegenGpuProfileRingSlots> frames{};
    std::filesystem::path path{};
    apusr::profile::StreamHandle stream{};
    apusr::profile::StreamHandle prepareStream{};
    apusr::profile::SampleGate sampleGate{};
    apusr::profile::SampleGate prepareSampleGate{};
};

struct BackendContext {
    ApusrFramegenVulkanDeviceContext create{};
    VulkanFunctions functions{};
    vk::PhysicalDeviceMemoryProperties memoryProperties{};
    std::uint32_t refCount = 0;
    std::uint32_t maxEffectContexts = 0;
    std::uint32_t gpuJobCount = 0;
    std::uint32_t stagingRingBase = 0;
    std::uint32_t samplerCount = 0;

    ApusrFramegenGpuJob* jobs = nullptr;
    NativeResource* resources = nullptr;
    std::uint8_t* stagingRing = nullptr;
    EffectContext* effects = nullptr;
    vk::DescriptorPool* descriptorPools = nullptr;

    vk::Buffer constantBuffer = nullptr;
    vk::DeviceMemory constantMemory = nullptr;
    std::uint8_t* constantMapped = nullptr;
    std::uint64_t constantBufferSize = 0;

    FramegenGpuProfileContext* gpuProfile = nullptr;
    apusr::profile::StreamHandle profileStream{};
    std::array<SamplerCacheEntry, APUSR_FRAMEGEN_MAX_SAMPLERS> samplers{};
};

void countFramegenGpuProfileBarrier(BackendContext& backend, bool image) noexcept;
void initializeFramegenGpuProfile(BackendContext& backend) noexcept;
void destroyFramegenGpuProfile(BackendContext& backend) noexcept;

void recordFramegenBackendFailure(
    BackendContext& backend,
    const char* stage,
    const ApusrFramegenResult result,
    const std::uint32_t jobIndex = UINT32_MAX) noexcept
{
    std::uint64_t frameId = 0u;
    if (backend.gpuProfile && backend.gpuProfile->activeSlot != UINT32_MAX) {
        FramegenGpuProfileFrame& frame = backend.gpuProfile->frames[backend.gpuProfile->activeSlot];
        frame.failed = true;
        frameId = frame.frameId;
    }
    if (!backend.profileStream)
        return;

    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = backend.profileStream,
        .frameId = frameId,
        .name = "backend.error_code",
        .value = static_cast<std::uint64_t>(result),
        .value2 = jobIndex,
    });
    apusr::profile::recordError(
        backend.profileStream,
        frameId,
        stage,
        apusr::StatusCode::RuntimeFailure,
        "Vulkan frame-generation GPU job recording failed",
        static_cast<std::uint64_t>(result),
        jobIndex);
}

std::size_t scratchSize(const std::size_t contexts) noexcept
{
    std::size_t size = alignUp(sizeof(BackendContext), alignof(std::max_align_t));
    size += alignUp(APUSR_FRAMEGEN_MAX_GPU_JOBS * sizeof(ApusrFramegenGpuJob), alignof(NativeResource));
    size += alignUp(contexts * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT * sizeof(NativeResource), alignof(std::uint64_t));
    size += alignUp(contexts * APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE, alignof(std::uint32_t));
    size += alignUp(contexts * sizeof(EffectContext), alignof(vk::DescriptorPool));
    size += contexts * APUSR_FRAMEGEN_MAX_QUEUED_FRAMES * sizeof(vk::DescriptorPool);
    return alignUp(size, alignof(std::max_align_t));
}

template <typename Fn>
bool loadDeviceFunction(BackendContext& backend, const char* name, Fn& out) noexcept
{
    out = reinterpret_cast<Fn>(backend.functions.getDeviceProcAddr(
        reinterpret_cast<vk::Device>(backend.create.device), name));
    return out != nullptr;
}

bool loadFunctions(BackendContext& backend) noexcept
{
    backend.functions.getInstanceProcAddr = reinterpret_cast<vk::PfnGetInstanceProcAddr>(backend.create.getInstanceProcAddr);
    backend.functions.getDeviceProcAddr = reinterpret_cast<vk::PfnGetDeviceProcAddr>(backend.create.getDeviceProcAddr);
    const auto instance = reinterpret_cast<vk::Instance>(backend.create.instance);
    const auto device = reinterpret_cast<vk::Device>(backend.create.device);
    if (!backend.functions.getDeviceProcAddr && backend.functions.getInstanceProcAddr) {
        backend.functions.getDeviceProcAddr = reinterpret_cast<vk::PfnGetDeviceProcAddr>(
            backend.functions.getInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
    }
    if (!backend.functions.getInstanceProcAddr || !backend.functions.getDeviceProcAddr || !device)
        return false;

    backend.functions.getPhysicalDeviceMemoryProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceMemoryProperties>(
        backend.functions.getInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties"));
    if (!backend.functions.getPhysicalDeviceMemoryProperties)
        return false;

#define APUSR_LOAD_VK(name, field) if (!loadDeviceFunction(backend, name, backend.functions.field)) return false
    APUSR_LOAD_VK("vkCreateBuffer", createBuffer);
    APUSR_LOAD_VK("vkDestroyBuffer", destroyBuffer);
    APUSR_LOAD_VK("vkGetBufferMemoryRequirements", getBufferMemoryRequirements);
    APUSR_LOAD_VK("vkBindBufferMemory", bindBufferMemory);
    APUSR_LOAD_VK("vkCreateImage", createImage);
    APUSR_LOAD_VK("vkDestroyImage", destroyImage);
    APUSR_LOAD_VK("vkGetImageMemoryRequirements", getImageMemoryRequirements);
    APUSR_LOAD_VK("vkBindImageMemory", bindImageMemory);
    APUSR_LOAD_VK("vkAllocateMemory", allocateMemory);
    APUSR_LOAD_VK("vkFreeMemory", freeMemory);
    APUSR_LOAD_VK("vkMapMemory", mapMemory);
    APUSR_LOAD_VK("vkUnmapMemory", unmapMemory);
    APUSR_LOAD_VK("vkCreateImageView", createImageView);
    APUSR_LOAD_VK("vkDestroyImageView", destroyImageView);
    APUSR_LOAD_VK("vkCreateShaderModule", createShaderModule);
    APUSR_LOAD_VK("vkDestroyShaderModule", destroyShaderModule);
    APUSR_LOAD_VK("vkCreateDescriptorSetLayout", createDescriptorSetLayout);
    APUSR_LOAD_VK("vkDestroyDescriptorSetLayout", destroyDescriptorSetLayout);
    APUSR_LOAD_VK("vkCreatePipelineLayout", createPipelineLayout);
    APUSR_LOAD_VK("vkDestroyPipelineLayout", destroyPipelineLayout);
    APUSR_LOAD_VK("vkCreateComputePipelines", createComputePipelines);
    APUSR_LOAD_VK("vkDestroyPipeline", destroyPipeline);
    APUSR_LOAD_VK("vkCreateDescriptorPool", createDescriptorPool);
    APUSR_LOAD_VK("vkDestroyDescriptorPool", destroyDescriptorPool);
    APUSR_LOAD_VK("vkResetDescriptorPool", resetDescriptorPool);
    APUSR_LOAD_VK("vkAllocateDescriptorSets", allocateDescriptorSets);
    APUSR_LOAD_VK("vkUpdateDescriptorSets", updateDescriptorSets);
    APUSR_LOAD_VK("vkCreateSampler", createSampler);
    APUSR_LOAD_VK("vkDestroySampler", destroySampler);
    APUSR_LOAD_VK("vkCmdPipelineBarrier", cmdPipelineBarrier);
    APUSR_LOAD_VK("vkCmdClearColorImage", cmdClearColorImage);
    APUSR_LOAD_VK("vkCmdCopyBuffer", cmdCopyBuffer);
    APUSR_LOAD_VK("vkCmdCopyBufferToImage", cmdCopyBufferToImage);
    APUSR_LOAD_VK("vkCmdCopyImage", cmdCopyImage);
    APUSR_LOAD_VK("vkCmdFillBuffer", cmdFillBuffer);
    APUSR_LOAD_VK("vkCmdBindPipeline", cmdBindPipeline);
    APUSR_LOAD_VK("vkCmdBindDescriptorSets", cmdBindDescriptorSets);
    APUSR_LOAD_VK("vkCmdDispatch", cmdDispatch);
#undef APUSR_LOAD_VK

    if (framegenGpuProfileEnabled() || apusr::profile::enabled()) {
        backend.functions.getPhysicalDeviceProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceProperties>(
            backend.functions.getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
        backend.functions.getPhysicalDeviceQueueFamilyProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceQueueFamilyProperties>(
            backend.functions.getInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        backend.functions.getPhysicalDeviceCalibrateableTimeDomains =
            reinterpret_cast<vk::PfnGetPhysicalDeviceCalibrateableTimeDomains>(
                backend.functions.getInstanceProcAddr(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
        if (!backend.functions.getPhysicalDeviceCalibrateableTimeDomains) {
            backend.functions.getPhysicalDeviceCalibrateableTimeDomains =
                reinterpret_cast<vk::PfnGetPhysicalDeviceCalibrateableTimeDomains>(
                    backend.functions.getInstanceProcAddr(instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
        }
        (void)loadDeviceFunction(backend, "vkGetCalibratedTimestampsKHR", backend.functions.getCalibratedTimestamps);
        if (!backend.functions.getCalibratedTimestamps)
            (void)loadDeviceFunction(backend, "vkGetCalibratedTimestampsEXT", backend.functions.getCalibratedTimestamps);
        (void)loadDeviceFunction(backend, "vkCreateQueryPool", backend.functions.createQueryPool);
        (void)loadDeviceFunction(backend, "vkDestroyQueryPool", backend.functions.destroyQueryPool);
        (void)loadDeviceFunction(backend, "vkGetQueryPoolResults", backend.functions.getQueryPoolResults);
        (void)loadDeviceFunction(backend, "vkCmdResetQueryPool", backend.functions.cmdResetQueryPool);
        (void)loadDeviceFunction(backend, "vkCmdWriteTimestamp", backend.functions.cmdWriteTimestamp);
    }
    return true;
}

vk::Format nativeFormat(const ApusrFramegenSurfaceFormat format) noexcept
{
    switch (format) {
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_FLOAT: return vk::FormatR32G32B32A32Sfloat;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_UINT: return vk::FormatR32G32B32A32Uint;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_FLOAT: return vk::FormatR16G16B16A16Sfloat;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32_FLOAT: return vk::FormatR32G32B32Sfloat;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_FLOAT: return vk::FormatR32G32Sfloat;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_UINT: return vk::FormatR32G32Uint;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT: return vk::FormatR8Uint;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT: return vk::FormatR32Sfloat;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT: return vk::FormatR32Uint;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_UNORM: return vk::FormatR8G8B8A8Unorm;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SNORM: return vk::FormatR8G8B8A8Snorm;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SRGB: return vk::FormatR8G8B8A8Srgb;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_UNORM: return vk::FormatB8G8R8A8Unorm;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_SRGB: return vk::FormatB8G8R8A8Srgb;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R11G11B10_FLOAT: return vk::FormatB10G11R11UfloatPack32;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_UNORM: return vk::FormatA2B10G10R10UnormPack32;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_FLOAT: return vk::FormatR16G16Sfloat;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_UINT: return vk::FormatR16G16Uint;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT: return vk::FormatR16G16Sint;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_FLOAT: return vk::FormatR16Sfloat;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UINT: return vk::FormatR16Uint;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UNORM: return vk::FormatR16Unorm;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_SNORM: return vk::FormatR16Snorm;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UNORM: return vk::FormatR8Unorm;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_SNORM: return vk::FormatR8Snorm;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM: return vk::FormatR8G8Unorm;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UINT: return vk::FormatR8G8Uint;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP: return vk::FormatE5B9G9R9UfloatPack32;
    default: return 0;
    }
}

ApusrFramegenSurfaceFormat surfaceFormatFromNative(const vk::Format format) noexcept
{
    switch (format) {
    case vk::FormatR32G32B32A32Sfloat: return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_FLOAT;
    case vk::FormatR32G32B32A32Uint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_UINT;
    case vk::FormatR16G16B16A16Sfloat: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case vk::FormatR32G32B32Sfloat: return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32_FLOAT;
    case vk::FormatR32G32Sfloat: return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_FLOAT;
    case vk::FormatR32G32Uint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_UINT;
    case vk::FormatR8Uint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT;
    case vk::FormatR32Uint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT;
    case vk::FormatR32Sfloat: return APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT;
    case vk::FormatR8G8B8A8Unorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_UNORM;
    case vk::FormatR8G8B8A8Snorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SNORM;
    case vk::FormatR8G8B8A8Srgb: return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SRGB;
    case vk::FormatB8G8R8A8Unorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_UNORM;
    case vk::FormatB8G8R8A8Srgb: return APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_SRGB;
    case vk::FormatB10G11R11UfloatPack32: return APUSR_FRAMEGEN_SURFACE_FORMAT_R11G11B10_FLOAT;
    case vk::FormatA2B10G10R10UnormPack32: return APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_UNORM;
    case vk::FormatR16G16Sfloat: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_FLOAT;
    case vk::FormatR16G16Uint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_UINT;
    case vk::FormatR16G16Sint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT;
    case vk::FormatR16Sfloat: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_FLOAT;
    case vk::FormatR16Uint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UINT;
    case vk::FormatR16Unorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UNORM;
    case vk::FormatR16Snorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_SNORM;
    case vk::FormatR8Unorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UNORM;
    case vk::FormatR8Snorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_R8_SNORM;
    case vk::FormatR8G8Unorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM;
    case vk::FormatR8G8Uint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UINT;
    case vk::FormatE5B9G9R9UfloatPack32: return APUSR_FRAMEGEN_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP;
    case vk::FormatD16Unorm: return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UNORM;
    case vk::FormatX8D24UnormPack32:
    case vk::FormatD24UnormS8Uint:
    case vk::FormatD32Sfloat:
    case vk::FormatD32SfloatS8Uint: return APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT;
    default: return APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    }
}

std::uint32_t resolvedMipCount(const ApusrFramegenResourceDescription& description) noexcept
{
    if (description.mipCount != 0)
        return description.mipCount;
    const std::uint32_t largest = std::max({description.width, description.height, std::max(1u, description.depth)});
    std::uint32_t count = 1;
    for (std::uint32_t dimension = largest; dimension > 1; dimension >>= 1)
        ++count;
    return count;
}

std::uint32_t findMemoryType(
    const BackendContext& backend,
    const std::uint32_t typeBits,
    const vk::MemoryPropertyFlags required) noexcept
{
    for (std::uint32_t index = 0; index < backend.memoryProperties.memoryTypeCount; ++index) {
        if ((typeBits & (1u << index)) != 0 &&
            (backend.memoryProperties.memoryTypes[index].propertyFlags & required) == required)
            return index;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

vk::ImageLayout stateLayout(const ApusrFramegenResourceState state) noexcept
{
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS) return vk::ImageLayoutGeneral;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ || state == APUSR_FRAMEGEN_RESOURCE_STATE_PIXEL_READ ||
        state == APUSR_FRAMEGEN_RESOURCE_STATE_PIXEL_COMPUTE_READ) return vk::ImageLayoutShaderReadOnlyOptimal;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_COPY_SRC) return vk::ImageLayoutTransferSrcOptimal;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST) return vk::ImageLayoutTransferDstOptimal;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_PRESENT) return vk::ImageLayoutPresentSrcKhr;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_RENDER_TARGET) return vk::ImageLayoutColorAttachmentOptimal;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_DEPTH_ATTACHMENT) return vk::ImageLayoutDepthStencilAttachmentOptimal;
    return vk::ImageLayoutGeneral;
}

vk::PipelineStageFlags stateStage(const ApusrFramegenResourceState state) noexcept
{
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_COPY_SRC || state == APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST)
        return vk::PipelineStageTransferBit;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ || state == APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS)
        return vk::PipelineStageComputeShaderBit;
    return vk::PipelineStageAllCommandsBit;
}

vk::AccessFlags stateAccess(const ApusrFramegenResourceState state) noexcept
{
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_COPY_SRC) return vk::AccessTransferReadBit;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST) return vk::AccessTransferWriteBit;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS) return vk::AccessShaderReadBit | vk::AccessShaderWriteBit;
    if (state == APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ || state == APUSR_FRAMEGEN_RESOURCE_STATE_PIXEL_READ ||
        state == APUSR_FRAMEGEN_RESOURCE_STATE_PIXEL_COMPUTE_READ || state == APUSR_FRAMEGEN_RESOURCE_STATE_GENERIC_READ)
        return vk::AccessShaderReadBit;
    return 0;
}

bool validResourceIndex(const BackendContext& backend, const int32_t index) noexcept
{
    return index > 0 && static_cast<std::uint32_t>(index) <
        backend.maxEffectContexts * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT;
}

ApusrFramegenResult transitionResource(
    BackendContext& backend,
    vk::CommandBuffer commandBuffer,
    NativeResource& resource,
    const ApusrFramegenResourceState desired,
    const bool discard = false,
    const vk::ImageLayout explicitImageLayout = kAutomaticImageLayout) noexcept
{
    if (!resource.buffer && !resource.image)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    const vk::PipelineStageFlags srcStage = discard ? vk::PipelineStageTopOfPipeBit : stateStage(resource.currentState);
    const vk::PipelineStageFlags dstStage = stateStage(desired);
    const vk::AccessFlags srcAccess = discard ? 0u : stateAccess(resource.currentState);
    const vk::AccessFlags dstAccess = stateAccess(desired);
    const bool generalRead = resource.image && resource.preserveGeneralReadLayout &&
        (desired == APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ ||
         desired == APUSR_FRAMEGEN_RESOURCE_STATE_PIXEL_READ ||
         desired == APUSR_FRAMEGEN_RESOURCE_STATE_PIXEL_COMPUTE_READ ||
         desired == APUSR_FRAMEGEN_RESOURCE_STATE_GENERIC_READ);
    const vk::ImageLayout desiredLayout = resource.image
        ? (explicitImageLayout != kAutomaticImageLayout ? explicitImageLayout
            : (generalRead ? vk::ImageLayoutGeneral : stateLayout(desired)))
        : vk::ImageLayoutUndefined;
    const bool sameState = resource.currentState == desired;
    const bool sameLayout = !resource.image || resource.layout == desiredLayout;
    const bool uavDependency = sameState && desired == APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS;
    if (sameState && sameLayout && !uavDependency && !discard)
        return APUSR_FRAMEGEN_OK;

    if (resource.image) {
        const vk::ImageMemoryBarrier barrier{
            .sType = vk::StructureTypeImageMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = srcAccess,
            .dstAccessMask = dstAccess,
            .oldLayout = discard ? vk::ImageLayoutUndefined : resource.layout,
            .newLayout = desiredLayout,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = resource.image,
            .subresourceRange = resource.subresourceRange,
        };
        backend.functions.cmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        countFramegenGpuProfileBarrier(backend, true);
        resource.layout = desiredLayout;
    } else {
        const vk::BufferMemoryBarrier barrier{
            .sType = vk::StructureTypeBufferMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = srcAccess,
            .dstAccessMask = dstAccess,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .buffer = resource.buffer,
            .offset = 0,
            .size = vk::WholeSize,
        };
        backend.functions.cmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 1, &barrier, 0, nullptr);
        countFramegenGpuProfileBarrier(backend, false);
    }
    resource.currentState = desired;
    return APUSR_FRAMEGEN_OK;
}

void destroyViews(BackendContext& backend, NativeResource& resource) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(backend.create.device);
    if (resource.srvView && resource.ownsSrvView)
        backend.functions.destroyImageView(device, resource.srvView, nullptr);
    resource.srvView = nullptr;
    resource.ownsSrvView = false;
    for (vk::ImageView& view : resource.uavViews) {
        if (view) {
            backend.functions.destroyImageView(device, view, nullptr);
            view = nullptr;
        }
    }
}

void destroyNativeResource(BackendContext& backend, NativeResource& resource) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(backend.create.device);
    destroyViews(backend, resource);
    if (resource.owned) {
        if (resource.image) backend.functions.destroyImage(device, resource.image, nullptr);
        if (resource.buffer) backend.functions.destroyBuffer(device, resource.buffer, nullptr);
        if (resource.memory) backend.functions.freeMemory(device, resource.memory, nullptr);
    }
    resource = {};
}

ApusrFramegenResult createImageViews(BackendContext& backend, NativeResource& resource) noexcept
{
    const vk::Format format = resource.nativeImageFormat != 0
        ? resource.nativeImageFormat : nativeFormat(resource.description.format);
    if (format == 0)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    const auto device = reinterpret_cast<vk::Device>(backend.create.device);
    const vk::ImageViewCreateInfo srvInfo{
        .sType = vk::StructureTypeImageViewCreateInfo,
        .pNext = nullptr,
        .flags = 0,
        .image = resource.image,
        .viewType = vk::ImageViewType2D,
        .format = format,
        .components = {vk::ComponentSwizzleIdentity, vk::ComponentSwizzleIdentity, vk::ComponentSwizzleIdentity, vk::ComponentSwizzleIdentity},
        .subresourceRange = resource.subresourceRange,
    };
    if (!resource.srvView) {
        if (backend.functions.createImageView(device, &srvInfo, nullptr, &resource.srvView) != vk::Success)
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        resource.ownsSrvView = true;
    }

    if ((resource.description.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_UAV) != 0) {
        const std::uint32_t viewMipCount = resource.subresourceRange.levelCount;
        if (viewMipCount == 0 || viewMipCount > kMaxImageMips) {
            destroyViews(backend, resource);
            return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
        }
        for (std::uint32_t mip = 0; mip < viewMipCount; ++mip) {
            vk::ImageViewCreateInfo uavInfo = srvInfo;
            uavInfo.subresourceRange.baseMipLevel = resource.subresourceRange.baseMipLevel + mip;
            uavInfo.subresourceRange.levelCount = 1;
            if (backend.functions.createImageView(device, &uavInfo, nullptr, &resource.uavViews[mip]) != vk::Success) {
                destroyViews(backend, resource);
                return APUSR_FRAMEGEN_ERROR_BACKEND_API;
            }
        }
    }
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult createBufferAllocation(
    BackendContext& backend,
    const vk::DeviceSize byteSize,
    const vk::BufferUsageFlags usage,
    const vk::MemoryPropertyFlags memoryFlags,
    vk::Buffer& buffer,
    vk::DeviceMemory& memory,
    std::uint64_t& allocationSize) noexcept
{
    const auto device = reinterpret_cast<vk::Device>(backend.create.device);
    const vk::BufferCreateInfo info{
        .sType = vk::StructureTypeBufferCreateInfo,
        .pNext = nullptr,
        .flags = 0,
        .size = std::max<vk::DeviceSize>(byteSize, 4),
        .usage = usage,
        .sharingMode = backend.create.internalQueueFamilyIndexCount > 1u
            ? vk::SharingModeConcurrent : vk::SharingModeExclusive,
        .queueFamilyIndexCount = backend.create.internalQueueFamilyIndexCount > 1u
            ? backend.create.internalQueueFamilyIndexCount : 0u,
        .pQueueFamilyIndices = backend.create.internalQueueFamilyIndexCount > 1u
            ? backend.create.internalQueueFamilyIndices.data() : nullptr,
    };
    if (backend.functions.createBuffer(device, &info, nullptr, &buffer) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    vk::MemoryRequirements requirements{};
    backend.functions.getBufferMemoryRequirements(device, buffer, &requirements);
    const std::uint32_t memoryType = findMemoryType(backend, requirements.memoryTypeBits, memoryFlags);
    if (memoryType == std::numeric_limits<std::uint32_t>::max()) {
        backend.functions.destroyBuffer(device, buffer, nullptr);
        buffer = nullptr;
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    const vk::MemoryAllocateInfo allocation{
        .sType = vk::StructureTypeMemoryAllocateInfo,
        .pNext = nullptr,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType,
    };
    if (backend.functions.allocateMemory(device, &allocation, nullptr, &memory) != vk::Success ||
        backend.functions.bindBufferMemory(device, buffer, memory, 0) != vk::Success) {
        if (memory) backend.functions.freeMemory(device, memory, nullptr);
        backend.functions.destroyBuffer(device, buffer, nullptr);
        buffer = nullptr;
        memory = nullptr;
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    allocationSize = requirements.size;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult createDescriptorPool(BackendContext& backend, vk::DescriptorPool& pool) noexcept
{
    const std::array<vk::DescriptorPoolSize, 5> sizes{{
        {vk::DescriptorTypeSampler, kDescriptorSetsPerFrame * APUSR_FRAMEGEN_MAX_SAMPLERS},
        {vk::DescriptorTypeSampledImage, kDescriptorSetsPerFrame * APUSR_FRAMEGEN_MAX_SRVS},
        {vk::DescriptorTypeStorageImage, kDescriptorSetsPerFrame * APUSR_FRAMEGEN_MAX_UAVS},
        {vk::DescriptorTypeStorageBuffer, kDescriptorSetsPerFrame * (APUSR_FRAMEGEN_MAX_SRVS + APUSR_FRAMEGEN_MAX_UAVS)},
        {vk::DescriptorTypeUniformBuffer, kDescriptorSetsPerFrame * APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS},
    }};
    const vk::DescriptorPoolCreateInfo info{
        .sType = vk::StructureTypeDescriptorPoolCreateInfo,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = kDescriptorSetsPerFrame,
        .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
        .pPoolSizes = sizes.data(),
    };
    return backend.functions.createDescriptorPool(reinterpret_cast<vk::Device>(backend.create.device), &info, nullptr, &pool) == vk::Success
        ? APUSR_FRAMEGEN_OK : APUSR_FRAMEGEN_ERROR_BACKEND_API;
}

vk::Filter nativeFilter(const ApusrFramegenFilter filter) noexcept
{
    return filter == APUSR_FRAMEGEN_FILTER_MINMAGMIP_POINT ? vk::FilterNearest : vk::FilterLinear;
}

vk::SamplerMipmapMode nativeMipmapMode(const ApusrFramegenFilter filter) noexcept
{
    return filter == APUSR_FRAMEGEN_FILTER_MINMAGMIP_LINEAR ? vk::SamplerMipmapModeLinear : vk::SamplerMipmapModeNearest;
}

vk::SamplerAddressMode nativeAddressMode(const ApusrFramegenAddressMode mode) noexcept
{
    switch (mode) {
    case APUSR_FRAMEGEN_ADDRESS_WRAP: return vk::SamplerAddressModeRepeat;
    case APUSR_FRAMEGEN_ADDRESS_MIRROR: return vk::SamplerAddressModeMirroredRepeat;
    case APUSR_FRAMEGEN_ADDRESS_CLAMP: return vk::SamplerAddressModeClampToEdge;
    case APUSR_FRAMEGEN_ADDRESS_BORDER: return vk::SamplerAddressModeClampToBorder;
    case APUSR_FRAMEGEN_ADDRESS_MIRROR_ONCE: return vk::SamplerAddressModeMirrorClampToEdge;
    default: return vk::SamplerAddressModeClampToEdge;
    }
}

bool samplerEqual(const ApusrFramegenSamplerDescription& a, const ApusrFramegenSamplerDescription& b) noexcept
{
    return a.filter == b.filter && a.addressModeU == b.addressModeU && a.addressModeV == b.addressModeV && a.addressModeW == b.addressModeW;
}

ApusrFramegenResult getSampler(BackendContext& backend, const ApusrFramegenSamplerDescription& description, vk::Sampler& out) noexcept
{
    for (const SamplerCacheEntry& entry : backend.samplers) {
        if (entry.active && samplerEqual(entry.description, description)) {
            out = entry.sampler;
            return APUSR_FRAMEGEN_OK;
        }
    }
    if (backend.samplerCount >= backend.samplers.size())
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    const vk::SamplerCreateInfo info{
        .sType = vk::StructureTypeSamplerCreateInfo,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = nativeFilter(description.filter),
        .minFilter = nativeFilter(description.filter),
        .mipmapMode = nativeMipmapMode(description.filter),
        .addressModeU = nativeAddressMode(description.addressModeU),
        .addressModeV = nativeAddressMode(description.addressModeV),
        .addressModeW = nativeAddressMode(description.addressModeW),
        .mipLodBias = 0.0f,
        .anisotropyEnable = 0,
        .maxAnisotropy = 1.0f,
        .compareEnable = 0,
        .compareOp = vk::CompareOpAlways,
        .minLod = 0.0f,
        .maxLod = 1000.0f,
        .borderColor = vk::BorderColorFloatTransparentBlack,
        .unnormalizedCoordinates = 0,
    };
    SamplerCacheEntry& entry = backend.samplers[backend.samplerCount];
    if (backend.functions.createSampler(reinterpret_cast<vk::Device>(backend.create.device), &info, nullptr, &entry.sampler) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    entry.description = description;
    entry.active = true;
    out = entry.sampler;
    ++backend.samplerCount;
    return APUSR_FRAMEGEN_OK;
}

void releaseSharedInfrastructure(BackendContext& backend) noexcept
{
    destroyFramegenGpuProfile(backend);
    const auto device = reinterpret_cast<vk::Device>(backend.create.device);
    if (backend.constantMapped && backend.constantMemory) {
        backend.functions.unmapMemory(device, backend.constantMemory);
        backend.constantMapped = nullptr;
    }
    if (backend.constantBuffer) backend.functions.destroyBuffer(device, backend.constantBuffer, nullptr);
    if (backend.constantMemory) backend.functions.freeMemory(device, backend.constantMemory, nullptr);
    backend.constantBuffer = nullptr;
    backend.constantMemory = nullptr;
    backend.constantBufferSize = 0;

    const std::size_t poolCount = static_cast<std::size_t>(backend.maxEffectContexts) * APUSR_FRAMEGEN_MAX_QUEUED_FRAMES;
    for (std::size_t i = 0; i < poolCount; ++i) {
        if (backend.descriptorPools[i]) {
            backend.functions.destroyDescriptorPool(device, backend.descriptorPools[i], nullptr);
            backend.descriptorPools[i] = nullptr;
        }
    }
    for (SamplerCacheEntry& entry : backend.samplers) {
        if (entry.active && entry.sampler)
            backend.functions.destroySampler(device, entry.sampler, nullptr);
        entry = {};
    }
    backend.samplerCount = 0;
    backend.gpuJobCount = 0;
}

ApusrFramegenResult ensureSharedInfrastructure(BackendContext& backend) noexcept
{
    if (backend.constantBuffer)
        return APUSR_FRAMEGEN_OK;
    backend.functions.getPhysicalDeviceMemoryProperties(
        reinterpret_cast<vk::PhysicalDevice>(backend.create.physicalDevice), &backend.memoryProperties);

    if (!backend.profileStream && apusr::profile::enabled()) {
        vk::PhysicalDeviceProperties properties{};
        if (backend.functions.getPhysicalDeviceProperties) {
            backend.functions.getPhysicalDeviceProperties(
                reinterpret_cast<vk::PhysicalDevice>(backend.create.physicalDevice), &properties);
        }
        backend.profileStream = apusr::profile::registerStream(apusr::profile::StreamDesc{
            .backend = apusr::profile::BackendApi::Vulkan,
            .component = apusr::profile::Component::FrameGeneration,
            .name = "framegen.compute",
            .capabilities = apusr::profile::StreamCapabilityFrames |
                apusr::profile::StreamCapabilityGpuIntervals |
                apusr::profile::StreamCapabilityCalibration |
                apusr::profile::StreamCapabilityCounters |
                apusr::profile::StreamCapabilityErrors |
                apusr::profile::StreamCapabilityResourceEvents |
                apusr::profile::StreamCapabilityGpuWorkloadInfo,
            .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(backend.create.device)),
            .vendorId = properties.vendorID,
            .deviceId = properties.deviceID,
        });
    }

    std::uint64_t allocationSize = 0;
    backend.constantBufferSize = static_cast<std::uint64_t>(backend.maxEffectContexts) * APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE;
    ApusrFramegenResult result = createBufferAllocation(
        backend,
        backend.constantBufferSize,
        vk::BufferUsageUniformBufferBit,
        vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit,
        backend.constantBuffer,
        backend.constantMemory,
        allocationSize);
    if (result != APUSR_FRAMEGEN_OK)
        return result;
    void* mapped = nullptr;
    if (backend.functions.mapMemory(
            reinterpret_cast<vk::Device>(backend.create.device), backend.constantMemory, 0, vk::WholeSize, 0, &mapped) != vk::Success) {
        releaseSharedInfrastructure(backend);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    backend.constantMapped = static_cast<std::uint8_t*>(mapped);

    const std::size_t poolCount = static_cast<std::size_t>(backend.maxEffectContexts) * APUSR_FRAMEGEN_MAX_QUEUED_FRAMES;
    for (std::size_t i = 0; i < poolCount; ++i) {
        result = createDescriptorPool(backend, backend.descriptorPools[i]);
        if (result != APUSR_FRAMEGEN_OK) {
            releaseSharedInfrastructure(backend);
            return result;
        }
    }
    initializeFramegenGpuProfile(backend);
    return APUSR_FRAMEGEN_OK;
}

std::uint32_t normalizedPermutationOptions(const ApusrFramegenShaderBlob& blob) noexcept
{
    if (blob.effect == APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION)
        return blob.permutationOptions & ~kFiForceWave64;
    if (blob.effect == APUSR_FRAMEGEN_SHADER_EFFECT_OPTICAL_FLOW)
        return blob.permutationOptions & ~kOfForceWave64;
    return blob.permutationOptions;
}

ApusrFramegenResult flattenBindings(const ApusrFramegenShaderBlob& blob, ApusrFramegenPipeline& pipeline) noexcept
{
    auto flatten = [](std::uint32_t sourceCount, const std::uint32_t* slots, const std::uint32_t* counts,
                      const std::uint32_t* spaces, const char** names, ApusrFramegenResourceBinding* out,
                      const std::uint32_t capacity, std::uint32_t& outCount) -> bool {
        outCount = 0;
        for (std::uint32_t source = 0; source < sourceCount; ++source) {
            if (spaces[source] != 0)
                return false;
            for (std::uint32_t arrayIndex = 0; arrayIndex < counts[source]; ++arrayIndex) {
                if (outCount >= capacity)
                    return false;
                out[outCount++] = {slots[source], arrayIndex, 0, names[source]};
            }
        }
        return true;
    };
    if (!flatten(blob.srvTextureCount, blob.boundSRVTextures, blob.boundSRVTextureCounts, blob.boundSRVTextureSpaces,
                 blob.boundSRVTextureNames, pipeline.srvTextureBindings, APUSR_FRAMEGEN_MAX_SRVS, pipeline.srvTextureCount) ||
        !flatten(blob.uavTextureCount, blob.boundUAVTextures, blob.boundUAVTextureCounts, blob.boundUAVTextureSpaces,
                 blob.boundUAVTextureNames, pipeline.uavTextureBindings, APUSR_FRAMEGEN_MAX_UAVS, pipeline.uavTextureCount) ||
        !flatten(blob.srvBufferCount, blob.boundSRVBuffers, blob.boundSRVBufferCounts, blob.boundSRVBufferSpaces,
                 blob.boundSRVBufferNames, pipeline.srvBufferBindings, APUSR_FRAMEGEN_MAX_SRVS, pipeline.srvBufferCount) ||
        !flatten(blob.uavBufferCount, blob.boundUAVBuffers, blob.boundUAVBufferCounts, blob.boundUAVBufferSpaces,
                 blob.boundUAVBufferNames, pipeline.uavBufferBindings, APUSR_FRAMEGEN_MAX_UAVS, pipeline.uavBufferCount))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    if (blob.cbvCount > APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    pipeline.constCount = blob.cbvCount;
    for (std::uint32_t i = 0; i < blob.cbvCount; ++i) {
        if (blob.boundConstantBufferSpaces[i] != 0)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        pipeline.constantBufferBindings[i] = {blob.boundConstantBuffers[i], 0, 0, blob.boundConstantBufferNames[i]};
    }
    pipeline.srvDescriptorTableSize = pipeline.srvTextureCount + pipeline.srvBufferCount;
    pipeline.uavDescriptorTableSize = pipeline.uavTextureCount + pipeline.uavBufferCount;
    return APUSR_FRAMEGEN_OK;
}

BackendContext* context(ApusrFramegenBackendInterface* interface) noexcept
{
    return interface ? reinterpret_cast<BackendContext*>(interface->scratchBuffer) : nullptr;
}

std::uint64_t steadyClockNs() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void writeFramegenGpuProfileCsv(
    FramegenGpuProfileContext& profile,
    const FramegenGpuProfileFrame& frame,
    const std::array<FramegenGpuProfileQueryResult, kFramegenGpuProfileQueriesPerSlot>& results) noexcept
{
    if (!framegenLegacyGpuProfileEnabled())
        return;
    if (!frame.valid || frame.queryCount < 2u || frame.frameEndQuery >= frame.queryCount)
        return;

    auto ticks = [&](const std::uint32_t begin, const std::uint32_t end) noexcept -> std::uint64_t {
        if (begin >= frame.queryCount || end >= frame.queryCount ||
            results[begin].availability == 0u || results[end].availability == 0u ||
            results[end].timestamp < results[begin].timestamp)
            return 0u;
        return results[end].timestamp - results[begin].timestamp;
    };
    auto us = [&](const std::uint64_t value) noexcept -> double {
        return static_cast<double>(value) * profile.timestampPeriodNs / 1000.0;
    };

    const std::uint64_t frameTicks = ticks(frame.frameBeginQuery, frame.frameEndQuery);
    std::uint64_t setupTicks = 0u;
    std::uint64_t dispatchTicks = 0u;
    for (std::uint32_t i = 0; i < frame.dispatchCount; ++i) {
        const FramegenGpuProfileDispatch& dispatch = frame.dispatches[i];
        setupTicks += ticks(dispatch.setupBeginQuery, dispatch.dispatchBeginQuery);
        dispatchTicks += ticks(dispatch.dispatchBeginQuery, dispatch.endQuery);
    }
    const std::uint64_t accountedTicks = setupTicks + dispatchTicks;
    const std::uint64_t unattributedTicks = frameTicks > accountedTicks ? frameTicks - accountedTicks : 0u;

    try {
        std::lock_guard<std::mutex> lock(gFramegenGpuProfileFileMutex);
        std::error_code ec;
        const std::filesystem::path parent = profile.path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        const bool needsHeader = !std::filesystem::exists(profile.path, ec) ||
            std::filesystem::file_size(profile.path, ec) == 0u;
        std::ofstream out(profile.path, std::ios::out | std::ios::app);
        if (!out)
            return;
        if (needsHeader) {
            out << "sequence,runtime_sequence,burst_sequence,burst_frame,frame_id,render_width,render_height,"
                   "display_width,display_height,interpolation_x,interpolation_y,interpolation_width,interpolation_height,"
                   "frame_time_delta_ms,jitter_x,jitter_y,motion_vector_scale_x,motion_vector_scale_y,"
                   "prepare_flags,dispatch_flags,has_hudless_color,motion_vectors_at_display_resolution,"
                   "motion_vectors_jittered,hdr,reset,row_type,name,effect,pass_id,profile_level,"
                   "permutation_options,shader_bytes,required_subgroup_size,groups_x,groups_y,groups_z,"
                   "frame_ticks,frame_us,cpu_record_us,total_setup_ticks,total_setup_us,total_dispatch_ticks,total_dispatch_us,"
                   "unattributed_ticks,unattributed_us,setup_ticks,setup_us,dispatch_ticks,dispatch_us,"
                   "total_ticks,total_us,cpu_setup_us,cpu_dispatch_record_us,cpu_total_us,pct_frame,"
                   "barrier_calls,image_barriers,buffer_barriers,"
                   "frame_barrier_calls,frame_image_barriers,frame_buffer_barriers,clear_jobs,copy_jobs,"
                   "barrier_jobs,discard_jobs,timestamp_period_ns,device_subgroup_size,device_subgroup_size_min,"
                   "device_subgroup_size_max,device_subgroup_size_control,device_fp16\n";
        }

        const auto writePrefix = [&](const char* rowType, const char* name) {
            out << frame.sequence << ',' << frame.runtimeSequence << ',' << frame.burstSequence << ','
                << frame.burstFrame << ',' << frame.frameId << ',' << frame.renderWidth << ','
                << frame.renderHeight << ',' << frame.displayWidth << ',' << frame.displayHeight << ','
                << frame.interpolationX << ',' << frame.interpolationY << ',' << frame.interpolationWidth << ','
                << frame.interpolationHeight << ',' << frame.frameTimeDeltaMs << ',' << frame.jitterX << ','
                << frame.jitterY << ',' << frame.motionVectorScaleX << ',' << frame.motionVectorScaleY << ','
                << frame.prepareFlags << ',' << frame.dispatchFlags << ',' << (frame.hasHudlessColor ? 1 : 0) << ','
                << (frame.motionVectorsAtDisplayResolution ? 1 : 0) << ',' << (frame.motionVectorsJittered ? 1 : 0) << ','
                << (frame.hdr ? 1 : 0) << ',' << (frame.reset ? 1 : 0) << ',' << rowType << ',' << name << ',';
        };
        out << std::fixed << std::setprecision(3);
        writePrefix("frame", "__frame__");
        out << "0,0,-1,0,0,0,0,0,0,"
            << frameTicks << ',' << us(frameTicks) << ',' << static_cast<double>(frame.cpuRecordNs) / 1000.0 << ','
            << setupTicks << ',' << us(setupTicks) << ','
            << dispatchTicks << ',' << us(dispatchTicks) << ','
            << unattributedTicks << ',' << us(unattributedTicks) << ','
            << "0,0,0,0," << frameTicks << ',' << us(frameTicks) << ",0,0,0,100.000,"
            << "0,0,0,"
            << frame.totalBarrierCalls << ',' << frame.totalImageBarriers << ',' << frame.totalBufferBarriers << ','
            << frame.clearJobs << ',' << frame.copyJobs << ',' << frame.barrierJobs << ',' << frame.discardJobs << ','
            << profile.timestampPeriodNs << ',' << profile.subgroupSize << ',' << profile.subgroupSizeMin << ','
            << profile.subgroupSizeMax << ',' << (profile.subgroupSizeControlSupported ? 1 : 0) << ','
            << (profile.fp16Supported ? 1 : 0) << '\n';

        for (std::uint32_t i = 0; i < frame.dispatchCount; ++i) {
            const FramegenGpuProfileDispatch& dispatch = frame.dispatches[i];
            const std::uint64_t dispatchSetupTicks = ticks(dispatch.setupBeginQuery, dispatch.dispatchBeginQuery);
            const std::uint64_t dispatchBodyTicks = ticks(dispatch.dispatchBeginQuery, dispatch.endQuery);
            const std::uint64_t dispatchTotalTicks = ticks(dispatch.setupBeginQuery, dispatch.endQuery);
            const double cpuSetupUs = dispatch.cpuDispatchBeginNs >= dispatch.cpuSetupBeginNs
                ? static_cast<double>(dispatch.cpuDispatchBeginNs - dispatch.cpuSetupBeginNs) / 1000.0 : 0.0;
            const double cpuDispatchRecordUs = dispatch.cpuEndNs >= dispatch.cpuDispatchBeginNs
                ? static_cast<double>(dispatch.cpuEndNs - dispatch.cpuDispatchBeginNs) / 1000.0 : 0.0;
            const double cpuTotalUs = dispatch.cpuEndNs >= dispatch.cpuSetupBeginNs
                ? static_cast<double>(dispatch.cpuEndNs - dispatch.cpuSetupBeginNs) / 1000.0 : 0.0;
            const double pct = frameTicks != 0u
                ? 100.0 * static_cast<double>(dispatchTotalTicks) / static_cast<double>(frameTicks) : 0.0;
            writePrefix("dispatch", dispatch.name);
            out << dispatch.effect << ',' << dispatch.passId << ',';
            if (dispatch.profileLevel == UINT32_MAX)
                out << "-1,";
            else
                out << dispatch.profileLevel << ',';
            out << dispatch.permutationOptions << ',' << dispatch.shaderBytes << ','
                << dispatch.requiredSubgroupSize << ',' << dispatch.groupsX << ',' << dispatch.groupsY << ','
                << dispatch.groupsZ << ','
                << frameTicks << ',' << us(frameTicks) << ',' << static_cast<double>(frame.cpuRecordNs) / 1000.0 << ','
                << setupTicks << ',' << us(setupTicks) << ','
                << dispatchTicks << ',' << us(dispatchTicks) << ','
                << unattributedTicks << ',' << us(unattributedTicks) << ','
                << dispatchSetupTicks << ',' << us(dispatchSetupTicks) << ','
                << dispatchBodyTicks << ',' << us(dispatchBodyTicks) << ','
                << dispatchTotalTicks << ',' << us(dispatchTotalTicks) << ','
                << cpuSetupUs << ',' << cpuDispatchRecordUs << ',' << cpuTotalUs << ',' << pct << ','
                << dispatch.barrierCalls << ',' << dispatch.imageBarriers << ',' << dispatch.bufferBarriers << ','
                << frame.totalBarrierCalls << ',' << frame.totalImageBarriers << ',' << frame.totalBufferBarriers << ','
                << frame.clearJobs << ',' << frame.copyJobs << ',' << frame.barrierJobs << ',' << frame.discardJobs << ','
                << profile.timestampPeriodNs << ',' << profile.subgroupSize << ',' << profile.subgroupSizeMin << ','
                << profile.subgroupSizeMax << ',' << (profile.subgroupSizeControlSupported ? 1 : 0) << ','
                << (profile.fp16Supported ? 1 : 0) << '\n';
        }
    } catch (...) {
        // Profiling is diagnostic-only; never perturb FrameGen if the filesystem
        // is unavailable or logging fails.
    }
}

bool resolveFramegenGpuProfileSlot(BackendContext& backend, const std::uint32_t slot) noexcept
{
    FramegenGpuProfileContext* profile = backend.gpuProfile;
    if (!profile || slot >= profile->frames.size())
        return true;
    FramegenGpuProfileFrame& frame = profile->frames[slot];
    if (!frame.valid)
        return true;
    if (!backend.functions.getQueryPoolResults || frame.queryCount == 0u ||
        frame.queryCount > kFramegenGpuProfileQueriesPerSlot)
        return false;

    std::array<FramegenGpuProfileQueryResult, kFramegenGpuProfileQueriesPerSlot> results{};
    const vk::Result result = backend.functions.getQueryPoolResults(
        reinterpret_cast<vk::Device>(backend.create.device),
        profile->pools[slot],
        0u,
        frame.queryCount,
        frame.queryCount * sizeof(FramegenGpuProfileQueryResult),
        results.data(),
        sizeof(FramegenGpuProfileQueryResult),
        vk::QueryResult64Bit | vk::QueryResultWithAvailabilityBit);
    if (result != vk::Success && result != vk::NotReady)
        return false;
    for (std::uint32_t i = 0; i < frame.queryCount; ++i) {
        if (results[i].availability == 0u)
            return false;
    }
    if (frame.unifiedProfile && frame.stream) {
        const auto deltaTicks = [&](const std::uint32_t begin, const std::uint32_t end) noexcept -> std::uint64_t {
            if (begin >= frame.queryCount || end >= frame.queryCount)
                return 0u;
            return timestampDelta(results[begin].timestamp, results[end].timestamp, profile->timestampValidBits);
        };
        const auto toNs = [&](const std::uint64_t ticks) noexcept -> std::uint64_t {
            return profile->timestampPeriodNs > 0.0
                ? static_cast<std::uint64_t>(static_cast<long double>(ticks) * static_cast<long double>(profile->timestampPeriodNs))
                : 0u;
        };
        const std::uint64_t gpuFrequency = profile->timestampPeriodNs > 0.0
            ? static_cast<std::uint64_t>(1000000000.0L / static_cast<long double>(profile->timestampPeriodNs))
            : 0u;
        const std::uint64_t frameTicks = deltaTicks(frame.frameBeginQuery, frame.frameEndQuery);
        std::uint64_t totalWorkgroups = 0u;
        for (std::uint32_t i = 0u; i < frame.dispatchCount; ++i) {
            const FramegenGpuProfileDispatch& dispatch = frame.dispatches[i];
            totalWorkgroups += static_cast<std::uint64_t>(dispatch.groupsX) * dispatch.groupsY * dispatch.groupsZ;
        }
        if (profile->clockCalibrationAvailable && gpuFrequency != 0u) {
            const std::array<vk::CalibratedTimestampInfo, 2> timestampInfos{{
                {.sType = vk::StructureTypeCalibratedTimestampInfo, .pNext = nullptr, .timeDomain = vk::TimeDomainDevice},
                {.sType = vk::StructureTypeCalibratedTimestampInfo, .pNext = nullptr, .timeDomain = profile->hostTimeDomain},
            }};
            std::array<std::uint64_t, 2> timestamps{};
            std::uint64_t maxDeviationNs = 0u;
            const auto device = reinterpret_cast<vk::Device>(backend.create.device);
            if (backend.functions.getCalibratedTimestamps(
                    device, static_cast<std::uint32_t>(timestampInfos.size()), timestampInfos.data(),
                    timestamps.data(), &maxDeviationNs) == vk::Success) {
                apusr::profile::recordCalibration(apusr::profile::ClockCalibration{
                    .stream = frame.stream,
                    .frameId = frame.frameId,
                    .sampleSequence = frame.sampleSequence,
                    .cpuTick = timestamps[1],
                    .gpuTick = timestamps[0],
                    .gpuFrequency = gpuFrequency,
                    .maxDeviationNs = maxDeviationNs,
                    .gpuValidBits = profile->timestampValidBits,
                    .flags = 1u,
                });
            }
        }
        apusr::profile::recordFrameSample(apusr::profile::FrameSample{
            .stream = frame.stream,
            .frameId = frame.frameId,
            .sampleSequence = frame.sampleSequence,
            .burstSequence = frame.burstSequence,
            .configurationHash = frame.configurationHash,
            .burstFrame = frame.burstFrame,
            .renderWidth = frame.renderWidth,
            .renderHeight = frame.renderHeight,
            .displayWidth = frame.displayWidth,
            .displayHeight = frame.displayHeight,
            .gpuDurationNs = toNs(frameTicks),
            .cpuTotalNs = frame.cpuRecordNs,
            .cpuRecordNs = frame.cpuRecordNs,
            .totalWorkgroups = totalWorkgroups,
            .gpuBeginTick = results[frame.frameBeginQuery].timestamp,
            .gpuEndTick = results[frame.frameEndQuery].timestamp,
            .gpuFrequency = gpuFrequency,
            .flags = (frame.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u) |
                (frame.failed ? apusr::profile::FrameFailed : 0u),
        });
        for (std::uint32_t i = 0u; i < frame.dispatchCount; ++i) {
            const FramegenGpuProfileDispatch& dispatch = frame.dispatches[i];
            const std::uint64_t setupTicks = deltaTicks(dispatch.setupBeginQuery, dispatch.dispatchBeginQuery);
            const std::uint64_t dispatchTicks = deltaTicks(dispatch.dispatchBeginQuery, dispatch.endQuery);
            const std::uint64_t totalTicks = deltaTicks(dispatch.setupBeginQuery, dispatch.endQuery);
            apusr::profile::recordGpuInterval(apusr::profile::GpuInterval{
                .stream = frame.stream,
                .frameId = frame.frameId,
                .sampleSequence = frame.sampleSequence,
                .name = dispatch.name,
                .durationNs = toNs(totalTicks),
                .setupNs = toNs(setupTicks),
                .executionNs = toNs(dispatchTicks),
                .groupsX = dispatch.groupsX,
                .groupsY = dispatch.groupsY,
                .groupsZ = dispatch.groupsZ,
                .profileLevel = dispatch.profileLevel,
                .barrierCount = dispatch.barrierCalls,
                .gpuBeginTick = results[dispatch.setupBeginQuery].timestamp,
                .gpuExecutionBeginTick = results[dispatch.dispatchBeginQuery].timestamp,
                .gpuEndTick = results[dispatch.endQuery].timestamp,
                .gpuFrequency = gpuFrequency,
                .flags = dispatch.permutationOptions,
            });
        }
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = frame.stream, .frameId = frame.frameId, .name = "jobs.clear_copy",
            .value = frame.clearJobs, .value2 = frame.copyJobs});
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = frame.stream, .frameId = frame.frameId, .name = "jobs.barrier_discard",
            .value = frame.barrierJobs, .value2 = frame.discardJobs});
    }
    writeFramegenGpuProfileCsv(*profile, frame, results);
    frame = {};
    return true;
}

void destroyFramegenGpuProfile(BackendContext& backend) noexcept
{
    FramegenGpuProfileContext* profile = backend.gpuProfile;
    if (!profile)
        return;
    for (std::uint32_t slot = 0u; slot < profile->frames.size(); ++slot)
        (void)resolveFramegenGpuProfileSlot(backend, slot);
    if (backend.functions.destroyQueryPool) {
        const auto device = reinterpret_cast<vk::Device>(backend.create.device);
        for (vk::QueryPool& pool : profile->pools) {
            if (pool) {
                backend.functions.destroyQueryPool(device, pool, nullptr);
                pool = nullptr;
            }
        }
    }
    delete profile;
    backend.gpuProfile = nullptr;
}

void initializeFramegenGpuProfile(BackendContext& backend) noexcept
{
    if (backend.gpuProfile || !framegenGpuProfileEnabled() ||
        !backend.functions.getPhysicalDeviceProperties || !backend.functions.createQueryPool ||
        !backend.functions.destroyQueryPool || !backend.functions.getQueryPoolResults ||
        !backend.functions.cmdResetQueryPool || !backend.functions.cmdWriteTimestamp)
        return;

    auto* profile = new (std::nothrow) FramegenGpuProfileContext{};
    if (!profile)
        return;
    profile->runtimeSequence = gFramegenGpuProfileRuntimeSequence.fetch_add(1u, std::memory_order_relaxed);
    profile->path = framegenGpuProfilePath();
    profile->currentBurstSequence = gFramegenGpuProfileBurstSequence.fetch_add(1u, std::memory_order_relaxed);
    profile->subgroupSize = backend.create.subgroupSize;
    profile->subgroupSizeMin = backend.create.subgroupSizeMin;
    profile->subgroupSizeMax = backend.create.subgroupSizeMax;
    profile->subgroupSizeControlSupported = backend.create.subgroupSizeControlSupported;
    profile->fp16Supported = backend.create.fp16Supported;

    vk::PhysicalDeviceProperties properties{};
    backend.functions.getPhysicalDeviceProperties(
        reinterpret_cast<vk::PhysicalDevice>(backend.create.physicalDevice), &properties);
    profile->timestampPeriodNs = static_cast<double>(properties.limits.timestampPeriod);
    profile->stream = backend.profileStream;
    if (profile->stream && apusr::profile::enabled()) {
        profile->prepareStream = apusr::profile::registerStream(apusr::profile::StreamDesc{
            .backend = apusr::profile::BackendApi::Vulkan,
            .component = apusr::profile::Component::FrameGeneration,
            .name = "framegen.prepare",
            .capabilities = apusr::profile::StreamCapabilityFrames |
                apusr::profile::StreamCapabilityGpuIntervals |
                apusr::profile::StreamCapabilityCalibration,
            .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(backend.create.device)),
            .vendorId = properties.vendorID,
            .deviceId = properties.deviceID,
        });
    }
    profile->sampleGate.reset();
    profile->prepareSampleGate.reset();
    profile->hostTimeDomain = profilerHostTimeDomain();

    if (backend.functions.getPhysicalDeviceQueueFamilyProperties && backend.create.internalQueueFamilyIndexCount != 0u) {
        std::uint32_t queueFamilyCount = 0u;
        const auto physicalDevice = reinterpret_cast<vk::PhysicalDevice>(backend.create.physicalDevice);
        backend.functions.getPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        if (queueFamilyCount != 0u) {
            auto* queueFamilies = new (std::nothrow) vk::QueueFamilyProperties[queueFamilyCount];
            if (queueFamilies) {
                backend.functions.getPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies);
                std::uint32_t validBits = 64u;
                bool found = false;
                const std::uint32_t internalCount = std::min(
                    backend.create.internalQueueFamilyIndexCount,
                    static_cast<std::uint32_t>(backend.create.internalQueueFamilyIndices.size()));
                for (std::uint32_t i = 0u; i < internalCount; ++i) {
                    const std::uint32_t family = backend.create.internalQueueFamilyIndices[i];
                    if (family >= queueFamilyCount || queueFamilies[family].timestampValidBits == 0u)
                        continue;
                    validBits = std::min(validBits, queueFamilies[family].timestampValidBits);
                    found = true;
                }
                if (found)
                    profile->timestampValidBits = validBits;
                delete[] queueFamilies;
            }
        }
    }

    if (backend.functions.getPhysicalDeviceCalibrateableTimeDomains && backend.functions.getCalibratedTimestamps) {
        std::array<vk::TimeDomain, 8> domains{};
        std::uint32_t domainCount = static_cast<std::uint32_t>(domains.size());
        const auto physicalDevice = reinterpret_cast<vk::PhysicalDevice>(backend.create.physicalDevice);
        if (backend.functions.getPhysicalDeviceCalibrateableTimeDomains(physicalDevice, &domainCount, domains.data()) == vk::Success) {
            const std::uint32_t availableCount = std::min(domainCount, static_cast<std::uint32_t>(domains.size()));
            bool haveDevice = false;
            bool haveHost = false;
            for (std::uint32_t i = 0u; i < availableCount; ++i) {
                haveDevice = haveDevice || domains[i] == vk::TimeDomainDevice;
                haveHost = haveHost || domains[i] == profile->hostTimeDomain;
            }
            profile->clockCalibrationAvailable = haveDevice && haveHost;
        }
    }

    if (!(profile->timestampPeriodNs > 0.0)) {
        delete profile;
        return;
    }

    const auto device = reinterpret_cast<vk::Device>(backend.create.device);
    const vk::QueryPoolCreateInfo info{
        vk::StructureTypeQueryPoolCreateInfo, nullptr, 0u, vk::QueryTypeTimestamp,
        kFramegenGpuProfileQueriesPerSlot, 0u};
    for (vk::QueryPool& pool : profile->pools) {
        if (backend.functions.createQueryPool(device, &info, nullptr, &pool) != vk::Success) {
            for (vk::QueryPool& created : profile->pools) {
                if (created)
                    backend.functions.destroyQueryPool(device, created, nullptr);
            }
            delete profile;
            return;
        }
    }
    profile->active = true;
    backend.gpuProfile = profile;
}

bool beginFramegenGpuProfile(
    BackendContext& backend,
    const vk::CommandBuffer commandBuffer,
    const ApusrFramegenVulkanGpuProfileFrameInfo& info) noexcept
{
    FramegenGpuProfileContext* profile = backend.gpuProfile;
    if (!profile || !profile->active || !commandBuffer || profile->activeSlot != UINT32_MAX)
        return false;

    // Opportunistically resolve completed samples. The real-time scheduler
    // already bounds heavy generation to one in flight, so this is normally
    // ready by the time the next generation is recorded; never wait if it is not.
    for (std::uint32_t slot = 0u; slot < profile->frames.size(); ++slot)
        (void)resolveFramegenGpuProfileSlot(backend, slot);

    const std::uint64_t now = steadyClockNs();
    std::uint64_t configurationHash = 0x415055534647564bull;
    configurationHash = apusr::profile::hashCombine(configurationHash,
        (static_cast<std::uint64_t>(info.renderWidth) << 32u) | info.renderHeight);
    configurationHash = apusr::profile::hashCombine(configurationHash,
        (static_cast<std::uint64_t>(info.displayWidth) << 32u) | info.displayHeight);
    configurationHash = apusr::profile::hashCombine(configurationHash, info.prepareFlags);
    configurationHash = apusr::profile::hashCombine(configurationHash, info.dispatchFlags);
    configurationHash = apusr::profile::hashCombine(configurationHash,
        (info.hasHudlessColor ? 1ull : 0ull) | (info.motionVectorsAtDisplayResolution ? 2ull : 0ull) |
        (info.motionVectorsJittered ? 4ull : 0ull) | (info.hdr ? 8ull : 0ull));

    apusr::profile::SampleDecision profileDecision{};
    const bool preparePhase = info.phase == APUSR_FRAMEGEN_VULKAN_GPU_PROFILE_PREPARE;
    const apusr::profile::StreamHandle phaseStream = preparePhase ? profile->prepareStream : profile->stream;
    const bool unifiedProfile = apusr::profile::gpuEnabled() && phaseStream;
    if (unifiedProfile) {
        apusr::profile::SampleGate& gate = preparePhase ? profile->prepareSampleGate : profile->sampleGate;
        profileDecision = gate.decide(configurationHash, info.frameId);
        if (!profileDecision.capture)
            return false;
    } else if (profile->burstRemaining == 0u) {
        if (now < profile->nextSampleNs)
            return false;
        profile->burstRemaining = kFramegenGpuProfileBurstFrames;
        profile->currentBurstSequence = gFramegenGpuProfileBurstSequence.fetch_add(1u, std::memory_order_relaxed);
    }

    const std::uint32_t slot = profile->nextSlot;
    if (!resolveFramegenGpuProfileSlot(backend, slot))
        return false;
    FramegenGpuProfileFrame& frame = profile->frames[slot];
    frame = {};
    frame.valid = true;
    frame.sequence = gFramegenGpuProfileSequence.fetch_add(1u, std::memory_order_relaxed);
    frame.sampleSequence = profileDecision.sampleSequence;
    frame.configurationHash = configurationHash;
    frame.unifiedProfile = unifiedProfile;
    frame.configurationChanged = unifiedProfile ? profileDecision.configurationChanged : false;
    frame.runtimeSequence = profile->runtimeSequence;
    frame.burstSequence = unifiedProfile ? profileDecision.burstSequence : profile->currentBurstSequence;
    frame.burstFrame = unifiedProfile ? profileDecision.burstFrame : (kFramegenGpuProfileBurstFrames - profile->burstRemaining);
    frame.frameId = info.frameId;
    frame.phase = info.phase;
    frame.stream = phaseStream;
    frame.renderWidth = info.renderWidth;
    frame.renderHeight = info.renderHeight;
    frame.displayWidth = info.displayWidth;
    frame.displayHeight = info.displayHeight;
    frame.interpolationX = info.interpolationX;
    frame.interpolationY = info.interpolationY;
    frame.interpolationWidth = info.interpolationWidth;
    frame.interpolationHeight = info.interpolationHeight;
    frame.frameTimeDeltaMs = info.frameTimeDeltaMs;
    frame.jitterX = info.jitterX;
    frame.jitterY = info.jitterY;
    frame.motionVectorScaleX = info.motionVectorScaleX;
    frame.motionVectorScaleY = info.motionVectorScaleY;
    frame.prepareFlags = info.prepareFlags;
    frame.dispatchFlags = info.dispatchFlags;
    frame.hasHudlessColor = info.hasHudlessColor;
    frame.motionVectorsAtDisplayResolution = info.motionVectorsAtDisplayResolution;
    frame.motionVectorsJittered = info.motionVectorsJittered;
    frame.hdr = info.hdr;
    frame.reset = info.reset;

    profile->activeSlot = slot;
    profile->activeQueryCursor = 0u;
    profile->activeCommandBuffer = commandBuffer;
    profile->activeDispatch = nullptr;
    profile->activeCpuStartNs = now;
    backend.functions.cmdResetQueryPool(
        commandBuffer, profile->pools[slot], 0u, kFramegenGpuProfileQueriesPerSlot);
    frame.frameBeginQuery = profile->activeQueryCursor++;
    backend.functions.cmdWriteTimestamp(
        commandBuffer, vk::PipelineStageAllCommandsBit, profile->pools[slot], frame.frameBeginQuery);
    return true;
}

void endFramegenGpuProfile(BackendContext& backend, const vk::CommandBuffer commandBuffer) noexcept
{
    FramegenGpuProfileContext* profile = backend.gpuProfile;
    if (!profile || profile->activeSlot == UINT32_MAX || profile->activeCommandBuffer != commandBuffer)
        return;
    const std::uint32_t slot = profile->activeSlot;
    FramegenGpuProfileFrame& frame = profile->frames[slot];
    const std::uint64_t cpuEndNs = steadyClockNs();
    frame.cpuRecordNs = cpuEndNs >= profile->activeCpuStartNs
        ? cpuEndNs - profile->activeCpuStartNs : 0u;
    if (profile->activeQueryCursor < kFramegenGpuProfileQueriesPerSlot) {
        frame.frameEndQuery = profile->activeQueryCursor++;
        backend.functions.cmdWriteTimestamp(
            commandBuffer, vk::PipelineStageAllCommandsBit, profile->pools[slot], frame.frameEndQuery);
        frame.queryCount = profile->activeQueryCursor;
    } else {
        frame = {};
    }
    profile->activeSlot = UINT32_MAX;
    profile->activeQueryCursor = 0u;
    profile->activeCommandBuffer = nullptr;
    profile->activeDispatch = nullptr;
    profile->activeCpuStartNs = 0u;
    profile->nextSlot = (slot + 1u) % kFramegenGpuProfileRingSlots;
    if (!frame.unifiedProfile) {
        if (profile->burstRemaining > 0u)
            --profile->burstRemaining;
        if (profile->burstRemaining == 0u)
            profile->nextSampleNs = steadyClockNs() +
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    kFramegenGpuProfileSampleInterval).count());
    }
}

FramegenGpuProfileDispatch* beginFramegenGpuProfileDispatch(
    BackendContext& backend,
    const vk::CommandBuffer commandBuffer,
    const ApusrFramegenComputeJob& job) noexcept
{
    FramegenGpuProfileContext* profile = backend.gpuProfile;
    if (!profile || profile->activeSlot == UINT32_MAX || profile->activeCommandBuffer != commandBuffer)
        return nullptr;
    FramegenGpuProfileFrame& frame = profile->frames[profile->activeSlot];
    if (frame.dispatchCount >= frame.dispatches.size() ||
        profile->activeQueryCursor + 3u > kFramegenGpuProfileQueriesPerSlot) {
        frame.valid = false;
        return nullptr;
    }

    FramegenGpuProfileDispatch& dispatch = frame.dispatches[frame.dispatchCount++];
    dispatch = {};
    const ApusrFramegenPipeline& pipeline = *job.pipeline;
    const std::string name = narrowPipelineName(pipeline.name);
    std::strncpy(dispatch.name, name.c_str(), sizeof(dispatch.name) - 1u);
    dispatch.effect = pipeline.shaderEffect;
    dispatch.passId = pipeline.shaderPassId;
    dispatch.permutationOptions = pipeline.shaderPermutationOptions;
    dispatch.shaderBytes = pipeline.shaderByteSize;
    dispatch.requiredSubgroupSize = pipeline.requiredSubgroupSize;
    dispatch.profileLevel = job.profileLevel;
    dispatch.groupsX = job.dimensions[0];
    dispatch.groupsY = job.dimensions[1];
    dispatch.groupsZ = job.dimensions[2];
    dispatch.cpuSetupBeginNs = steadyClockNs();
    dispatch.setupBeginQuery = profile->activeQueryCursor++;
    backend.functions.cmdWriteTimestamp(
        commandBuffer, vk::PipelineStageAllCommandsBit, profile->pools[profile->activeSlot],
        dispatch.setupBeginQuery);
    profile->activeDispatch = &dispatch;
    return &dispatch;
}

void markFramegenGpuProfileDispatchBody(
    BackendContext& backend,
    const vk::CommandBuffer commandBuffer,
    FramegenGpuProfileDispatch* dispatch) noexcept
{
    FramegenGpuProfileContext* profile = backend.gpuProfile;
    if (!profile || !dispatch || profile->activeSlot == UINT32_MAX ||
        profile->activeCommandBuffer != commandBuffer ||
        profile->activeQueryCursor >= kFramegenGpuProfileQueriesPerSlot)
        return;
    dispatch->cpuDispatchBeginNs = steadyClockNs();
    dispatch->dispatchBeginQuery = profile->activeQueryCursor++;
    backend.functions.cmdWriteTimestamp(
        commandBuffer, vk::PipelineStageAllCommandsBit, profile->pools[profile->activeSlot],
        dispatch->dispatchBeginQuery);
}

void endFramegenGpuProfileDispatch(
    BackendContext& backend,
    const vk::CommandBuffer commandBuffer,
    FramegenGpuProfileDispatch* dispatch) noexcept
{
    FramegenGpuProfileContext* profile = backend.gpuProfile;
    if (!profile || !dispatch || profile->activeSlot == UINT32_MAX ||
        profile->activeCommandBuffer != commandBuffer ||
        profile->activeQueryCursor >= kFramegenGpuProfileQueriesPerSlot)
        return;
    dispatch->cpuEndNs = steadyClockNs();
    dispatch->endQuery = profile->activeQueryCursor++;
    backend.functions.cmdWriteTimestamp(
        commandBuffer, vk::PipelineStageAllCommandsBit, profile->pools[profile->activeSlot],
        dispatch->endQuery);
    profile->activeDispatch = nullptr;
}

void countFramegenGpuProfileBarrier(BackendContext& backend, const bool image) noexcept
{
    FramegenGpuProfileContext* profile = backend.gpuProfile;
    if (!profile || profile->activeSlot == UINT32_MAX)
        return;
    FramegenGpuProfileFrame& frame = profile->frames[profile->activeSlot];
    ++frame.totalBarrierCalls;
    if (image)
        ++frame.totalImageBarriers;
    else
        ++frame.totalBufferBarriers;
    if (profile->activeDispatch) {
        ++profile->activeDispatch->barrierCalls;
        if (image)
            ++profile->activeDispatch->imageBarriers;
        else
            ++profile->activeDispatch->bufferBarriers;
    }
}

ApusrFramegenVersion getVersion(ApusrFramegenBackendInterface*)
{
    return APUSR_FRAMEGEN_BACKEND_VERSION;
}

ApusrFramegenResult getMemoryUsage(ApusrFramegenBackendInterface* interface, const std::uint32_t effectId, ApusrFramegenMemoryUsage* usage)
{
    BackendContext* backend = context(interface);
    if (!backend || !usage) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (effectId >= backend->maxEffectContexts) return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    *usage = backend->effects[effectId].memoryUsage;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult createContext(ApusrFramegenBackendInterface* interface, std::uint32_t* effectId)
{
    BackendContext* backend = context(interface);
    if (!backend || !effectId) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    ApusrFramegenResult result = ensureSharedInfrastructure(*backend);
    if (result != APUSR_FRAMEGEN_OK) return result;
    for (std::uint32_t i = 0; i < backend->maxEffectContexts; ++i) {
        EffectContext& effect = backend->effects[i];
        if (effect.active) continue;
        effect = {};
        effect.active = true;
        effect.nextStaticResource = i * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT + 1;
        effect.nextDynamicResource = (i + 1) * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT - 1;
        *effectId = i;
        ++backend->refCount;
        return APUSR_FRAMEGEN_OK;
    }
    return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
}

ApusrFramegenResult getCapabilities(ApusrFramegenBackendInterface* interface, ApusrFramegenDeviceCapabilities* capabilities)
{
    BackendContext* backend = context(interface);
    if (!backend || !capabilities) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    capabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_6;
    capabilities->waveLaneCountMin = std::max(1u, backend->create.subgroupSizeMin);
    capabilities->waveLaneCountMax = std::max(capabilities->waveLaneCountMin, backend->create.subgroupSizeMax);
    capabilities->fp16Supported = backend->create.fp16Supported;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult destroyResource(ApusrFramegenBackendInterface*, ApusrFramegenResource, std::uint32_t);

ApusrFramegenResult destroyContext(ApusrFramegenBackendInterface* interface, const std::uint32_t effectId)
{
    BackendContext* backend = context(interface);
    if (!backend) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (effectId >= backend->maxEffectContexts || !backend->effects[effectId].active)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    EffectContext& effect = backend->effects[effectId];
    const std::uint32_t base = effectId * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT;
    for (std::uint32_t index = base + 1; index < effect.nextStaticResource; ++index) {
        if (backend->resources[index].owned)
            destroyResource(interface, {static_cast<int32_t>(index)}, effectId);
    }
    const std::uint32_t end = (effectId + 1) * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT - 1;
    for (std::uint32_t index = effect.nextDynamicResource + 1; index <= end; ++index) {
        if (backend->resources[index].dynamic) {
            destroyViews(*backend, backend->resources[index]);
            backend->resources[index] = {};
        }
    }
    effect = {};
    if (backend->refCount > 0) --backend->refCount;
    if (backend->refCount == 0) releaseSharedInfrastructure(*backend);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult createResource(
    ApusrFramegenBackendInterface* interface,
    const ApusrFramegenCreateResourceDescription* description,
    const std::uint32_t effectId,
    ApusrFramegenResource* outResource)
{
    BackendContext* backend = context(interface);
    if (!backend || !description || !outResource) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    outResource->internalIndex = 0;
    if (description->initData.type == APUSR_FRAMEGEN_RESOURCE_INIT_INVALID || effectId >= backend->maxEffectContexts)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    if (description->heapType != APUSR_FRAMEGEN_HEAP_DEFAULT && description->heapType != APUSR_FRAMEGEN_HEAP_UPLOAD)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    const auto& resourceDescription = description->resourceDescription;
    const bool upload = description->heapType == APUSR_FRAMEGEN_HEAP_UPLOAD;
    if (upload) {
        if (resourceDescription.type != APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER || description->initData.size == 0)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    } else if (resourceDescription.type == APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER) {
        if (resourceDescription.size == 0) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    } else if (resourceDescription.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D) {
        if (resourceDescription.width == 0 || resourceDescription.height == 0 || resourceDescription.depth > 1 ||
            nativeFormat(resourceDescription.format) == 0 ||
            (((resourceDescription.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_UAV) != 0) && resolvedMipCount(resourceDescription) > kMaxImageMips))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    } else {
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }

    EffectContext& effect = backend->effects[effectId];
    if (!effect.active || effect.nextStaticResource + 1 >= effect.nextDynamicResource)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;

    const std::uint32_t index = effect.nextStaticResource++;
    NativeResource& resource = backend->resources[index];
    resource = {};
    resource.description = resourceDescription;
    resource.mipCount = resolvedMipCount(resource.description);
    resource.owned = true;
    resource.upload = upload;

    const auto failCreation = [&](const ApusrFramegenResult failure) noexcept {
        destroyNativeResource(*backend, resource);
        resource = {};
        effect.nextStaticResource = index;
        return failure;
    };

    const auto device = reinterpret_cast<vk::Device>(backend->create.device);
    ApusrFramegenResult result = APUSR_FRAMEGEN_OK;
    if (resource.upload || resource.description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER) {
        vk::BufferUsageFlags usage = vk::BufferUsageTransferSrcBit | vk::BufferUsageTransferDstBit | vk::BufferUsageStorageBufferBit;
        if ((resource.description.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_INDIRECT) != 0) usage |= vk::BufferUsageIndirectBufferBit;
        const vk::MemoryPropertyFlags memoryFlags = resource.upload
            ? (vk::MemoryPropertyHostVisibleBit | vk::MemoryPropertyHostCoherentBit)
            : vk::MemoryPropertyDeviceLocalBit;
        const std::uint64_t byteSize = resource.upload ? std::max<std::size_t>(description->initData.size, resource.description.size) : resource.description.size;
        result = createBufferAllocation(*backend, byteSize, usage, memoryFlags, resource.buffer, resource.memory, resource.allocationSize);
        if (result != APUSR_FRAMEGEN_OK) return failCreation(result);
        resource.initialState = resource.upload ? APUSR_FRAMEGEN_RESOURCE_STATE_GENERIC_READ : description->initialState;
        resource.currentState = resource.initialState;
        resource.layout = vk::ImageLayoutUndefined;
        if (resource.upload) {
            void* mapped = nullptr;
            if (backend->functions.mapMemory(device, resource.memory, 0, vk::WholeSize, 0, &mapped) != vk::Success)
                return failCreation(APUSR_FRAMEGEN_ERROR_BACKEND_API);
            if (description->initData.type == APUSR_FRAMEGEN_RESOURCE_INIT_BUFFER && description->initData.buffer && description->initData.size)
                std::memcpy(mapped, description->initData.buffer, description->initData.size);
            else if (description->initData.type == APUSR_FRAMEGEN_RESOURCE_INIT_VALUE && description->initData.size)
                std::memset(mapped, description->initData.value, description->initData.size);
            backend->functions.unmapMemory(device, resource.memory);
        }
    } else {
        const vk::Format format = nativeFormat(resource.description.format);
        vk::ImageUsageFlags usage = vk::ImageUsageSampledBit | vk::ImageUsageTransferSrcBit | vk::ImageUsageTransferDstBit;
        if ((resource.description.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_UAV) != 0) usage |= vk::ImageUsageStorageBit;
        const vk::ImageCreateInfo info{
            .sType = vk::StructureTypeImageCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .imageType = vk::ImageType2D,
            .format = format,
            .extent = {resource.description.width, resource.description.height, 1},
            .mipLevels = resource.mipCount,
            .arrayLayers = 1,
            .samples = vk::SampleCount1Bit,
            .tiling = vk::ImageTilingOptimal,
            .usage = usage,
            .sharingMode = backend->create.internalQueueFamilyIndexCount > 1u
                ? vk::SharingModeConcurrent : vk::SharingModeExclusive,
            .queueFamilyIndexCount = backend->create.internalQueueFamilyIndexCount > 1u
                ? backend->create.internalQueueFamilyIndexCount : 0u,
            .pQueueFamilyIndices = backend->create.internalQueueFamilyIndexCount > 1u
                ? backend->create.internalQueueFamilyIndices.data() : nullptr,
            .initialLayout = vk::ImageLayoutUndefined,
        };
        if (backend->functions.createImage(device, &info, nullptr, &resource.image) != vk::Success)
            return failCreation(APUSR_FRAMEGEN_ERROR_BACKEND_API);
        vk::MemoryRequirements requirements{};
        backend->functions.getImageMemoryRequirements(device, resource.image, &requirements);
        const std::uint32_t memoryType = findMemoryType(*backend, requirements.memoryTypeBits, vk::MemoryPropertyDeviceLocalBit);
        if (memoryType == std::numeric_limits<std::uint32_t>::max())
            return failCreation(APUSR_FRAMEGEN_ERROR_BACKEND_API);
        const vk::MemoryAllocateInfo allocation{vk::StructureTypeMemoryAllocateInfo, nullptr, requirements.size, memoryType};
        if (backend->functions.allocateMemory(device, &allocation, nullptr, &resource.memory) != vk::Success ||
            backend->functions.bindImageMemory(device, resource.image, resource.memory, 0) != vk::Success)
            return failCreation(APUSR_FRAMEGEN_ERROR_BACKEND_API);
        resource.allocationSize = requirements.size;
        resource.nativeImageFormat = format;
        resource.subresourceRange = {vk::ImageAspectColorBit, 0, resource.mipCount, 0, 1};
        resource.initialLayout = vk::ImageLayoutUndefined;
        resource.layout = vk::ImageLayoutUndefined;
        resource.initialState = description->initData.type == APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED
            ? description->initialState : APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST;
        resource.currentState = APUSR_FRAMEGEN_RESOURCE_STATE_COMMON;
        result = createImageViews(*backend, resource);
        if (result != APUSR_FRAMEGEN_OK) return failCreation(result);
    }

    outResource->internalIndex = static_cast<int32_t>(index);
    effect.memoryUsage.totalUsageInBytes += resource.allocationSize;
    if ((resource.description.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE) != 0)
        effect.memoryUsage.aliasableUsageInBytes += resource.allocationSize;
    if (backend->profileStream && resource.allocationSize != 0u) {
        apusr::profile::recordCounter({
            .stream = backend->profileStream,
            .name = "memory.resource_alloc_bytes",
            .value = resource.allocationSize,
            .value2 = index,
            .flags = effectId,
        });
        apusr::profile::recordCounter({
            .stream = backend->profileStream,
            .name = "memory.effect_usage_bytes",
            .value = effect.memoryUsage.totalUsageInBytes,
            .value2 = effect.memoryUsage.aliasableUsageInBytes,
            .flags = effectId,
        });
        apusr::profile::recordResourceEvent({
            .stream = backend->profileStream,
            .name = "framegen.effect_resource",
            .detail = resource.description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER ? "buffer" : "image",
            .operation = apusr::profile::ResourceOperation::Allocate,
            .resourceId = (static_cast<std::uint64_t>(effectId) << 32u) | index,
            .bytes = resource.allocationSize,
            .alignment = resource.description.alignment,
            .heapOrMemoryType = static_cast<std::uint32_t>(description->heapType),
            .flags = (resource.description.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE) != 0 ? 1u : 0u,
        });
    }

    if (!resource.upload && description->initData.type != APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED) {
        ApusrFramegenCreateResourceDescription upload = *description;
        upload.heapType = APUSR_FRAMEGEN_HEAP_UPLOAD;
        upload.resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER;
        upload.resourceDescription.size = static_cast<std::uint32_t>(description->initData.size);
        upload.resourceDescription.stride = 1;
        upload.resourceDescription.alignment = 0;
        upload.resourceDescription.mipCount = 1;
        upload.resourceDescription.usage = APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY;
        upload.initialState = APUSR_FRAMEGEN_RESOURCE_STATE_GENERIC_READ;
        ApusrFramegenResource source{};
        result = createResource(interface, &upload, effectId, &source);
        if (result != APUSR_FRAMEGEN_OK) return result;
        ApusrFramegenGpuJob job{};
        job.jobType = APUSR_FRAMEGEN_JOB_COPY;
        job.copyJob.src = source;
        job.copyJob.dst = *outResource;
        job.copyJob.size = static_cast<std::uint32_t>(description->initData.size);
        return interface->scheduleJob(interface, &job);
    }
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult destroyResource(
    ApusrFramegenBackendInterface* interface,
    const ApusrFramegenResource handle,
    const std::uint32_t effectId)
{
    BackendContext* backend = context(interface);
    if (!backend) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (!validResourceIndex(*backend, handle.internalIndex) || effectId >= backend->maxEffectContexts)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    EffectContext& effect = backend->effects[effectId];
    const std::uint32_t base = effectId * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT;
    const std::uint32_t index = static_cast<std::uint32_t>(handle.internalIndex);
    if (!effect.active || index <= base || index >= effect.nextStaticResource)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    NativeResource& resource = backend->resources[index];
    if (!resource.owned) return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    if (effect.memoryUsage.totalUsageInBytes >= resource.allocationSize)
        effect.memoryUsage.totalUsageInBytes -= resource.allocationSize;
    if ((resource.description.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE) != 0 && effect.memoryUsage.aliasableUsageInBytes >= resource.allocationSize)
        effect.memoryUsage.aliasableUsageInBytes -= resource.allocationSize;
    if (backend->profileStream && resource.allocationSize != 0u) {
        apusr::profile::recordCounter({
            .stream = backend->profileStream,
            .name = "memory.resource_free_bytes",
            .value = resource.allocationSize,
            .value2 = index,
            .flags = effectId,
        });
        apusr::profile::recordCounter({
            .stream = backend->profileStream,
            .name = "memory.effect_usage_bytes",
            .value = effect.memoryUsage.totalUsageInBytes,
            .value2 = effect.memoryUsage.aliasableUsageInBytes,
            .flags = effectId,
        });
        apusr::profile::recordResourceEvent({
            .stream = backend->profileStream,
            .name = "framegen.effect_resource",
            .detail = resource.description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER ? "buffer" : "image",
            .operation = apusr::profile::ResourceOperation::Free,
            .resourceId = (static_cast<std::uint64_t>(effectId) << 32u) | index,
            .bytes = resource.allocationSize,
            .alignment = resource.description.alignment,
            .flags = (resource.description.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE) != 0 ? 1u : 0u,
        });
    }
    destroyNativeResource(*backend, resource);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult registerResource(
    ApusrFramegenBackendInterface* interface,
    const ApusrFramegenExternalResource* external,
    const std::uint32_t effectId,
    ApusrFramegenResource* outResource)
{
    BackendContext* backend = context(interface);
    if (!backend || !external || !outResource) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    outResource->internalIndex = 0;
    if (!external->resource || effectId >= backend->maxEffectContexts) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    const auto* metadata = static_cast<const ApusrFramegenVulkanExternalResourceMetadata*>(external->backendMetadata);
    const vk::Format suppliedNativeFormat = metadata && metadata->nativeFormat != 0
        ? static_cast<vk::Format>(metadata->nativeFormat) : nativeFormat(external->description.format);
    if (external->description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER) {
        if (external->description.size == 0) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    } else if (external->description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D) {
        if (external->description.width == 0 || external->description.height == 0 || external->description.depth > 1 ||
            suppliedNativeFormat == 0)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    } else {
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }
    EffectContext& effect = backend->effects[effectId];
    if (!effect.active || effect.nextDynamicResource <= effect.nextStaticResource + 1)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    const std::uint32_t index = effect.nextDynamicResource--;
    NativeResource& resource = backend->resources[index];
    resource = {};
    resource.description = external->description;
    if (resource.description.format == APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN && suppliedNativeFormat != 0)
        resource.description.format = surfaceFormatFromNative(suppliedNativeFormat);
    resource.initialState = external->state;
    resource.currentState = external->state;
    resource.dynamic = true;
    resource.mipCount = resolvedMipCount(resource.description);
    if (resource.description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER) {
        resource.buffer = reinterpret_cast<vk::Buffer>(external->resource);
    } else if (resource.description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D) {
        resource.image = reinterpret_cast<vk::Image>(external->resource);
        resource.nativeImageFormat = suppliedNativeFormat;
        resource.subresourceRange.aspectMask = metadata && metadata->aspectMask != 0
            ? static_cast<vk::ImageAspectFlags>(metadata->aspectMask)
            : ((resource.description.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_DEPTHTARGET) != 0
                ? vk::ImageAspectDepthBit : vk::ImageAspectColorBit);
        resource.subresourceRange.baseMipLevel = metadata ? metadata->baseMipLevel : 0;
        resource.subresourceRange.levelCount = metadata && metadata->levelCount != 0
            ? metadata->levelCount : resource.mipCount;
        resource.subresourceRange.baseArrayLayer = metadata ? metadata->baseArrayLayer : 0;
        resource.subresourceRange.layerCount = metadata && metadata->layerCount != 0 ? metadata->layerCount : 1;
        if (resource.subresourceRange.levelCount > kMaxImageMips &&
            (resource.description.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_UAV) != 0) {
            resource = {};
            ++effect.nextDynamicResource;
            return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
        }
        resource.initialLayout = metadata && metadata->nativeLayout != 0xffffffffu
            ? static_cast<vk::ImageLayout>(metadata->nativeLayout) : stateLayout(external->state);
        resource.layout = resource.initialLayout;
        resource.preserveGeneralReadLayout = resource.initialLayout == vk::ImageLayoutGeneral &&
            (resource.description.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY) != 0u &&
            (resource.description.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_UAV) == 0u;
        if (metadata && metadata->imageView) {
            resource.srvView = reinterpret_cast<vk::ImageView>(metadata->imageView);
            resource.ownsSrvView = false;
        }
        ApusrFramegenResult result = createImageViews(*backend, resource);
        if (result != APUSR_FRAMEGEN_OK) {
            destroyViews(*backend, resource);
            resource = {};
            ++effect.nextDynamicResource;
            return result;
        }
    }
    outResource->internalIndex = static_cast<int32_t>(index);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenExternalResource getResource(ApusrFramegenBackendInterface* interface, const ApusrFramegenResource handle)
{
    BackendContext* backend = context(interface);
    if (!backend || !validResourceIndex(*backend, handle.internalIndex)) return {};
    NativeResource& resource = backend->resources[handle.internalIndex];
    void* native = resource.image ? reinterpret_cast<void*>(resource.image) : reinterpret_cast<void*>(resource.buffer);
    const void* metadata = nullptr;
    if (resource.image) {
        resource.exportMetadata.imageView = reinterpret_cast<void*>(resource.srvView);
        resource.exportMetadata.nativeFormat = static_cast<std::uint32_t>(resource.nativeImageFormat);
        resource.exportMetadata.nativeLayout = static_cast<std::uint32_t>(resource.layout);
        resource.exportMetadata.aspectMask = resource.subresourceRange.aspectMask;
        resource.exportMetadata.baseMipLevel = resource.subresourceRange.baseMipLevel;
        resource.exportMetadata.levelCount = resource.subresourceRange.levelCount;
        resource.exportMetadata.baseArrayLayer = resource.subresourceRange.baseArrayLayer;
        resource.exportMetadata.layerCount = resource.subresourceRange.layerCount;
        metadata = &resource.exportMetadata;
    }
    return {native, resource.description, resource.currentState, metadata};
}

ApusrFramegenResult unregisterResources(
    ApusrFramegenBackendInterface* interface,
    const ApusrFramegenCommandList commandList,
    const std::uint32_t effectId)
{
    BackendContext* backend = context(interface);
    if (!backend || !commandList) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (effectId >= backend->maxEffectContexts) return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    EffectContext& effect = backend->effects[effectId];
    if (!effect.active) return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    const std::uint32_t end = (effectId + 1) * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT - 1;
    const std::uint32_t first = effect.nextDynamicResource + 1;
    auto commandBuffer = reinterpret_cast<vk::CommandBuffer>(commandList);
    for (std::uint32_t index = first; index <= end; ++index) {
        NativeResource& resource = backend->resources[index];
        if (!resource.dynamic) continue;
        const ApusrFramegenResult result = transitionResource(
            *backend, commandBuffer, resource, resource.initialState, false, resource.initialLayout);
        if (result != APUSR_FRAMEGEN_OK) return result;
        destroyViews(*backend, resource);
        resource = {};
    }
    effect.nextDynamicResource = end;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResourceDescription getResourceDescription(ApusrFramegenBackendInterface* interface, const ApusrFramegenResource handle)
{
    BackendContext* backend = context(interface);
    if (!backend || !validResourceIndex(*backend, handle.internalIndex)) return {};
    return backend->resources[handle.internalIndex].description;
}

ApusrFramegenResult stageConstants(
    ApusrFramegenBackendInterface* interface,
    void* data,
    const std::uint32_t size,
    ApusrFramegenConstantBuffer* constantBuffer)
{
    BackendContext* backend = context(interface);
    if (!backend || !data || !constantBuffer) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (size == 0 || size > APUSR_FRAMEGEN_CONSTANT_BUFFER_ENTRY_SIZE || (size & 3u) != 0)
        return APUSR_FRAMEGEN_ERROR_INVALID_SIZE;
    const std::uint32_t total = backend->maxEffectContexts * APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE;
    const std::uint32_t aligned = static_cast<std::uint32_t>(alignUp(size, 4));
    if (backend->stagingRingBase + aligned > total) backend->stagingRingBase = 0;
    std::uint8_t* destination = backend->stagingRing + backend->stagingRingBase;
    std::memcpy(destination, data, size);
    constantBuffer->num32BitEntries = size / 4;
    constantBuffer->data = reinterpret_cast<std::uint32_t*>(destination);
    backend->stagingRingBase += aligned;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult createPipeline(
    ApusrFramegenBackendInterface* interface,
    ApusrFramegenShaderBlob* blob,
    const ApusrFramegenPipelineDescription* description,
    std::uint32_t,
    ApusrFramegenPipeline* outPipeline)
{
    BackendContext* backend = context(interface);
    if (!backend || !blob || !description || !outPipeline) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (description->samplerCount > APUSR_FRAMEGEN_MAX_SAMPLERS || blob->effect == APUSR_FRAMEGEN_SHADER_EFFECT_UNKNOWN)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    *outPipeline = {};
    ApusrFramegenResult result = flattenBindings(*blob, *outPipeline);
    if (result != APUSR_FRAMEGEN_OK) return result;

    std::uint32_t spirvSize = 0;
    const std::uint8_t* spirv = shaderpack::findFramegenSpirv(
        static_cast<std::uint8_t>(blob->effect), blob->passId, normalizedPermutationOptions(*blob), &spirvSize);
    if (!spirv || spirvSize < 20 || (spirvSize & 3u) != 0)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    std::array<vk::DescriptorSetLayoutBinding, kMaxLayoutBindings> layoutBindings{};
    std::array<vk::Sampler, APUSR_FRAMEGEN_MAX_SAMPLERS> immutableSamplers{};
    std::uint32_t bindingCount = 0;
    auto addBindings = [&](const std::uint32_t count, const std::uint32_t* registers, const std::uint32_t* descriptorCounts,
                           const std::uint32_t* spaces, const vk::DescriptorType type, const std::uint32_t base) -> bool {
        for (std::uint32_t i = 0; i < count; ++i) {
            if (spaces[i] != 0 || bindingCount >= layoutBindings.size()) return false;
            layoutBindings[bindingCount++] = {base + registers[i], type, descriptorCounts[i], vk::ShaderStageComputeBit, nullptr};
        }
        return true;
    };
    if (!addBindings(blob->cbvCount, blob->boundConstantBuffers, blob->boundConstantBufferCounts, blob->boundConstantBufferSpaces,
                     vk::DescriptorTypeUniformBuffer, kCbvBindingBase) ||
        !addBindings(blob->srvTextureCount, blob->boundSRVTextures, blob->boundSRVTextureCounts, blob->boundSRVTextureSpaces,
                     vk::DescriptorTypeSampledImage, kSrvBindingBase) ||
        !addBindings(blob->srvBufferCount, blob->boundSRVBuffers, blob->boundSRVBufferCounts, blob->boundSRVBufferSpaces,
                     vk::DescriptorTypeStorageBuffer, kSrvBindingBase) ||
        !addBindings(blob->uavTextureCount, blob->boundUAVTextures, blob->boundUAVTextureCounts, blob->boundUAVTextureSpaces,
                     vk::DescriptorTypeStorageImage, kUavBindingBase) ||
        !addBindings(blob->uavBufferCount, blob->boundUAVBuffers, blob->boundUAVBufferCounts, blob->boundUAVBufferSpaces,
                     vk::DescriptorTypeStorageBuffer, kUavBindingBase))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    for (std::uint32_t i = 0; i < description->samplerCount; ++i) {
        result = getSampler(*backend, description->samplers[i], immutableSamplers[i]);
        if (result != APUSR_FRAMEGEN_OK) return result;
        if (bindingCount >= layoutBindings.size()) return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
        layoutBindings[bindingCount++] = {
            kSamplerBindingBase + i, vk::DescriptorTypeSampler, 1, vk::ShaderStageComputeBit, &immutableSamplers[i]};
    }

    const auto device = reinterpret_cast<vk::Device>(backend->create.device);
    const vk::DescriptorSetLayoutCreateInfo layoutInfo{
        vk::StructureTypeDescriptorSetLayoutCreateInfo, nullptr, 0, bindingCount, layoutBindings.data()};
    vk::DescriptorSetLayout setLayout = nullptr;
    if (backend->functions.createDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout) != vk::Success)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    const vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
        vk::StructureTypePipelineLayoutCreateInfo, nullptr, 0, 1, &setLayout, 0, nullptr};
    vk::PipelineLayout pipelineLayout = nullptr;
    if (backend->functions.createPipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != vk::Success) {
        backend->functions.destroyDescriptorSetLayout(device, setLayout, nullptr);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    const vk::ShaderModuleCreateInfo shaderInfo{
        vk::StructureTypeShaderModuleCreateInfo, nullptr, 0, spirvSize, reinterpret_cast<const std::uint32_t*>(spirv)};
    vk::ShaderModule shader = nullptr;
    if (backend->functions.createShaderModule(device, &shaderInfo, nullptr, &shader) != vk::Success) {
        backend->functions.destroyPipelineLayout(device, pipelineLayout, nullptr);
        backend->functions.destroyDescriptorSetLayout(device, setLayout, nullptr);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    const bool forceWave64 = backend->create.subgroupSizeControlSupported &&
        ((blob->effect == APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION &&
          (blob->permutationOptions & kFiForceWave64) != 0u) ||
         (blob->effect == APUSR_FRAMEGEN_SHADER_EFFECT_OPTICAL_FLOW &&
          (blob->permutationOptions & kOfForceWave64) != 0u));
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo requiredSubgroupSize{
        vk::StructureTypePipelineShaderStageRequiredSubgroupSizeCreateInfo, nullptr, 64u};
    const vk::PipelineShaderStageCreateInfo stage{
        vk::StructureTypePipelineShaderStageCreateInfo,
        forceWave64 ? static_cast<const void*>(&requiredSubgroupSize) : nullptr,
        0, vk::ShaderStageComputeBit, shader, "CS", nullptr};
    const vk::ComputePipelineCreateInfo pipelineInfo{
        vk::StructureTypeComputePipelineCreateInfo, nullptr, 0, stage, pipelineLayout, nullptr, -1};
    vk::Pipeline pipeline = nullptr;
    const vk::Result vkResult = backend->functions.createComputePipelines(device, nullptr, 1, &pipelineInfo, nullptr, &pipeline);
    backend->functions.destroyShaderModule(device, shader, nullptr);
    if (vkResult != vk::Success) {
        backend->functions.destroyPipelineLayout(device, pipelineLayout, nullptr);
        backend->functions.destroyDescriptorSetLayout(device, setLayout, nullptr);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }
    outPipeline->rootSignature = reinterpret_cast<void*>(setLayout);
    outPipeline->cmdSignature = reinterpret_cast<void*>(pipelineLayout);
    outPipeline->pipeline = reinterpret_cast<void*>(pipeline);
    outPipeline->shaderEffect = static_cast<std::uint8_t>(blob->effect);
    outPipeline->shaderPassId = blob->passId;
    outPipeline->shaderPermutationOptions = blob->permutationOptions;
    outPipeline->shaderByteSize = spirvSize;
    outPipeline->requiredSubgroupSize = forceWave64 ? 64u : 0u;
    std::wcsncpy(outPipeline->name, description->name, APUSR_FRAMEGEN_RESOURCE_NAME_SIZE - 1);
    outPipeline->name[APUSR_FRAMEGEN_RESOURCE_NAME_SIZE - 1] = L'\0';

    if (backend->profileStream && apusr::profile::gpuEnabled()) {
        const std::string profileName = narrowPipelineName(description->name);
        std::uint64_t workloadId = apusr::profile::hashString(profileName);
        workloadId = apusr::profile::hashCombine(workloadId, static_cast<std::uint64_t>(blob->effect));
        workloadId = apusr::profile::hashCombine(workloadId, blob->passId);
        workloadId = apusr::profile::hashCombine(workloadId, normalizedPermutationOptions(*blob));
        apusr::backends::vk::SpirvComputeMetadata spirvMetadata{};
        (void)apusr::backends::vk::readSpirvComputeMetadata(spirv, spirvSize, spirvMetadata);
        apusr::profile::recordGpuWorkloadInfo(apusr::profile::GpuWorkloadInfo{
            .stream = backend->profileStream,
            .name = profileName.c_str(),
            .detail = "spirv.compute",
            .workloadId = workloadId,
            .nativePipeline = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pipeline)),
            .codeHash = apusr::profile::hashString(std::string_view(
                reinterpret_cast<const char*>(spirv), spirvSize)),
            .codeBytes = spirvSize,
            .threadsX = spirvMetadata.localSizeX,
            .threadsY = spirvMetadata.localSizeY,
            .threadsZ = spirvMetadata.localSizeZ,
            .subgroupSize = forceWave64 ? 64u : 0u,
            .flags = normalizedPermutationOptions(*blob),
        });
    }
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult destroyPipeline(
    ApusrFramegenBackendInterface* interface,
    ApusrFramegenPipeline* pipeline,
    std::uint32_t)
{
    BackendContext* backend = context(interface);
    if (!backend || !pipeline) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    const auto device = reinterpret_cast<vk::Device>(backend->create.device);
    if (pipeline->pipeline) backend->functions.destroyPipeline(device, reinterpret_cast<vk::Pipeline>(pipeline->pipeline), nullptr);
    if (pipeline->cmdSignature) backend->functions.destroyPipelineLayout(device, reinterpret_cast<vk::PipelineLayout>(pipeline->cmdSignature), nullptr);
    if (pipeline->rootSignature) backend->functions.destroyDescriptorSetLayout(device, reinterpret_cast<vk::DescriptorSetLayout>(pipeline->rootSignature), nullptr);
    *pipeline = {};
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult scheduleJob(ApusrFramegenBackendInterface* interface, const ApusrFramegenGpuJob* job)
{
    BackendContext* backend = context(interface);
    if (!backend || !job) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    if (backend->gpuJobCount >= APUSR_FRAMEGEN_MAX_GPU_JOBS) return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
    ApusrFramegenGpuJob& queued = backend->jobs[backend->gpuJobCount++];
    queued = {};
    queued.jobType = job->jobType;
    switch (job->jobType) {
    case APUSR_FRAMEGEN_JOB_COMPUTE: {
        if (!job->computeJob.pipeline) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        queued.computeJob = job->computeJob;
        break;
    }
    case APUSR_FRAMEGEN_JOB_CLEAR_FLOAT: queued.clearJob = job->clearJob; break;
    case APUSR_FRAMEGEN_JOB_COPY: queued.copyJob = job->copyJob; break;
    case APUSR_FRAMEGEN_JOB_BARRIER: queued.barrierJob = job->barrierJob; break;
    case APUSR_FRAMEGEN_JOB_DISCARD: queued.discardJob = job->discardJob; break;
    default: return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    }
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult allocateDescriptorSet(
    BackendContext& backend, const std::uint32_t effectId, const vk::DescriptorSetLayout layout, vk::DescriptorSet& set) noexcept
{
    EffectContext& effect = backend.effects[effectId];
    vk::DescriptorPool pool = backend.descriptorPools[effectId * APUSR_FRAMEGEN_MAX_QUEUED_FRAMES + effect.frameSlot];
    const vk::DescriptorSetAllocateInfo info{vk::StructureTypeDescriptorSetAllocateInfo, nullptr, pool, 1, &layout};
    return backend.functions.allocateDescriptorSets(reinterpret_cast<vk::Device>(backend.create.device), &info, &set) == vk::Success
        ? APUSR_FRAMEGEN_OK : APUSR_FRAMEGEN_ERROR_BACKEND_API;
}

[[nodiscard]] bool resolveComputeUavView(
    const ApusrFramegenPipeline& pipeline,
    const NativeResource& resource,
    const std::uint32_t requestedMip,
    vk::ImageView& outView) noexcept
{
    if (requestedMip < resource.mipCount && requestedMip < resource.uavViews.size() &&
        resource.uavViews[requestedMip]) {
        outView = resource.uavViews[requestedMip];
        return true;
    }

    // FidelityFX SPD shaders statically declare a fixed tail of inpainting
    // pyramid UAVs.  The runtime mip count in the SPD constants decides which
    // of those descriptors are actually touched.  Vulkan still requires every
    // statically declared storage-image descriptor to be valid, even when a
    // trailing slot is inactive for the current resolution.  A full mip chain
    // can therefore legitimately be shorter than the shader's descriptor tail
    // (for example a 1720x720 pyramid has mips 0..10 while the reflected SPD
    // pipeline declares rw_inpainting_pyramid0..11).  Bind the last real mip as
    // a harmless descriptor for those inactive tail slots, while keeping the
    // normal backend contract strict for every other compute pass.
    const bool inpaintingSpdPass =
        pipeline.shaderEffect == APUSR_FRAMEGEN_SHADER_EFFECT_FRAME_INTERPOLATION &&
        (pipeline.shaderPassId == APUSR_FRAMEINTERPOLATION_PASS_INPAINTING_PYRAMID ||
         pipeline.shaderPassId == APUSR_FRAMEINTERPOLATION_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID);
    if (!inpaintingSpdPass || resource.mipCount == 0u || resource.uavViews.empty())
        return false;

    const std::uint32_t lastRealMip = std::min<std::uint32_t>(
        resource.mipCount, static_cast<std::uint32_t>(resource.uavViews.size())) - 1u;
    if (!resource.uavViews[lastRealMip])
        return false;

    outView = resource.uavViews[lastRealMip];
    return true;
}

ApusrFramegenResult executeCompute(
    BackendContext& backend,
    vk::CommandBuffer commandBuffer,
    const std::uint32_t effectId,
    const ApusrFramegenComputeJob& job,
    std::uint64_t& constantCursor,
    const std::uint64_t constantEnd) noexcept
{
    const ApusrFramegenPipeline& pipeline = *job.pipeline;
    FramegenGpuProfileDispatch* profileDispatch =
        beginFramegenGpuProfileDispatch(backend, commandBuffer, job);
    const bool skipBarriers = (job.flags & APUSR_FRAMEGEN_JOB_FLAG_SKIP_BARRIERS) != 0;
    if (!skipBarriers) {
        for (std::uint32_t i = 0; i < pipeline.srvTextureCount; ++i) {
            const int32_t index = job.srvTextures[i].resource.internalIndex;
            if (!validResourceIndex(backend, index)) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
            const ApusrFramegenResult result = transitionResource(backend, commandBuffer, backend.resources[index], APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ);
            if (result != APUSR_FRAMEGEN_OK) return result;
        }
        for (std::uint32_t i = 0; i < pipeline.srvBufferCount; ++i) {
            const int32_t index = job.srvBuffers[i].resource.internalIndex;
            if (!validResourceIndex(backend, index)) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
            const ApusrFramegenResult result = transitionResource(backend, commandBuffer, backend.resources[index], APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ);
            if (result != APUSR_FRAMEGEN_OK) return result;
        }
        for (std::uint32_t i = 0; i < pipeline.uavTextureCount; ++i) {
            const int32_t index = job.uavTextures[i].resource.internalIndex;
            if (!validResourceIndex(backend, index)) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
            const ApusrFramegenResult result = transitionResource(backend, commandBuffer, backend.resources[index], APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS);
            if (result != APUSR_FRAMEGEN_OK) return result;
        }
        for (std::uint32_t i = 0; i < pipeline.uavBufferCount; ++i) {
            const int32_t index = job.uavBuffers[i].resource.internalIndex;
            if (!validResourceIndex(backend, index)) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
            const ApusrFramegenResult result = transitionResource(backend, commandBuffer, backend.resources[index], APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS);
            if (result != APUSR_FRAMEGEN_OK) return result;
        }
    }

    vk::DescriptorSet set = nullptr;
    ApusrFramegenResult result = allocateDescriptorSet(
        backend, effectId, reinterpret_cast<vk::DescriptorSetLayout>(pipeline.rootSignature), set);
    if (result != APUSR_FRAMEGEN_OK) return result;

    constexpr std::size_t kMaxWrites = APUSR_FRAMEGEN_MAX_SRVS * 2 + APUSR_FRAMEGEN_MAX_UAVS * 2 + APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS;
    std::array<vk::WriteDescriptorSet, kMaxWrites> writes{};
    std::array<vk::DescriptorImageInfo, APUSR_FRAMEGEN_MAX_SRVS + APUSR_FRAMEGEN_MAX_UAVS> imageInfos{};
    std::array<vk::DescriptorBufferInfo, APUSR_FRAMEGEN_MAX_SRVS + APUSR_FRAMEGEN_MAX_UAVS + APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS> bufferInfos{};
    std::uint32_t writeCount = 0, imageCount = 0, bufferCount = 0;

    auto imageWrite = [&](const ApusrFramegenResourceBinding& binding, const vk::ImageView view,
                          const vk::ImageLayout layout, const vk::DescriptorType type, const std::uint32_t base) {
        imageInfos[imageCount] = {nullptr, view, layout};
        writes[writeCount++] = {vk::StructureTypeWriteDescriptorSet, nullptr, set, base + binding.slotIndex,
            binding.arrayIndex, 1, type, &imageInfos[imageCount++], nullptr, nullptr};
    };
    auto bufferWrite = [&](const ApusrFramegenResourceBinding& binding, const NativeResource& resource,
                           const std::uint32_t offset, const std::uint32_t size, const vk::DescriptorType type, const std::uint32_t base) {
        const vk::DeviceSize range = size != 0 ? size : std::max<std::uint32_t>(resource.description.size - std::min(offset, resource.description.size), 4u);
        bufferInfos[bufferCount] = {resource.buffer, offset, range};
        writes[writeCount++] = {vk::StructureTypeWriteDescriptorSet, nullptr, set, base + binding.slotIndex,
            binding.arrayIndex, 1, type, nullptr, &bufferInfos[bufferCount++], nullptr};
    };

    for (std::uint32_t i = 0; i < pipeline.srvTextureCount; ++i) {
        const NativeResource& resource = backend.resources[job.srvTextures[i].resource.internalIndex];
        if (!resource.srvView) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        imageWrite(pipeline.srvTextureBindings[i], resource.srvView, resource.layout, vk::DescriptorTypeSampledImage, kSrvBindingBase);
    }
    for (std::uint32_t i = 0; i < pipeline.srvBufferCount; ++i) {
        const NativeResource& resource = backend.resources[job.srvBuffers[i].resource.internalIndex];
        if (!resource.buffer) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        bufferWrite(pipeline.srvBufferBindings[i], resource, job.srvBuffers[i].offset, job.srvBuffers[i].size, vk::DescriptorTypeStorageBuffer, kSrvBindingBase);
    }
    for (std::uint32_t i = 0; i < pipeline.uavTextureCount; ++i) {
        const NativeResource& resource = backend.resources[job.uavTextures[i].resource.internalIndex];
        vk::ImageView view = nullptr;
        if (!resolveComputeUavView(pipeline, resource, job.uavTextures[i].mip, view))
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        imageWrite(pipeline.uavTextureBindings[i], view, vk::ImageLayoutGeneral, vk::DescriptorTypeStorageImage, kUavBindingBase);
    }
    for (std::uint32_t i = 0; i < pipeline.uavBufferCount; ++i) {
        const NativeResource& resource = backend.resources[job.uavBuffers[i].resource.internalIndex];
        if (!resource.buffer) return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        bufferWrite(pipeline.uavBufferBindings[i], resource, job.uavBuffers[i].offset, job.uavBuffers[i].size, vk::DescriptorTypeStorageBuffer, kUavBindingBase);
    }
    const std::uint32_t alignment = std::max(4u, backend.create.minUniformBufferOffsetAlignment);
    for (std::uint32_t i = 0; i < pipeline.constCount; ++i) {
        const ApusrFramegenConstantBuffer& constant = job.constantBuffers[i];
        const std::uint32_t bytes = constant.num32BitEntries * 4;
        if (!constant.data || bytes == 0 || bytes > APUSR_FRAMEGEN_CONSTANT_BUFFER_ENTRY_SIZE)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        constantCursor = alignUp(constantCursor, alignment);
        if (constantCursor + bytes > constantEnd) return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
        std::memcpy(backend.constantMapped + constantCursor, constant.data, bytes);
        bufferInfos[bufferCount] = {backend.constantBuffer, constantCursor, bytes};
        writes[writeCount++] = {vk::StructureTypeWriteDescriptorSet, nullptr, set,
            kCbvBindingBase + pipeline.constantBufferBindings[i].slotIndex, 0, 1, vk::DescriptorTypeUniformBuffer,
            nullptr, &bufferInfos[bufferCount++], nullptr};
        constantCursor += alignUp(bytes, alignment);
    }

    backend.functions.updateDescriptorSets(reinterpret_cast<vk::Device>(backend.create.device), writeCount, writes.data(), 0, nullptr);
    const auto nativePipeline = reinterpret_cast<vk::Pipeline>(pipeline.pipeline);
    const auto pipelineLayout = reinterpret_cast<vk::PipelineLayout>(pipeline.cmdSignature);
    backend.functions.cmdBindPipeline(commandBuffer, vk::PipelineBindPointCompute, nativePipeline);
    backend.functions.cmdBindDescriptorSets(commandBuffer, vk::PipelineBindPointCompute, pipelineLayout, 0, 1, &set, 0, nullptr);
    markFramegenGpuProfileDispatchBody(backend, commandBuffer, profileDispatch);
    backend.functions.cmdDispatch(commandBuffer, job.dimensions[0], job.dimensions[1], job.dimensions[2]);
    endFramegenGpuProfileDispatch(backend, commandBuffer, profileDispatch);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult executeJobs(
    ApusrFramegenBackendInterface* interface,
    const ApusrFramegenCommandList commandList,
    const std::uint32_t effectId)
{
    BackendContext* backend = context(interface);
    if (!backend || !commandList) return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
    const auto fail = [&](const char* stage, const ApusrFramegenResult result,
                          const std::uint32_t jobIndex = UINT32_MAX) noexcept -> ApusrFramegenResult {
        recordFramegenBackendFailure(*backend, stage, result, jobIndex);
        backend->gpuJobCount = 0u;
        return result;
    };
    if (effectId >= backend->maxEffectContexts || !backend->effects[effectId].active)
        return fail("execute_jobs.effect", APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE);
    EffectContext& effect = backend->effects[effectId];
    const std::uint32_t poolIndex = effectId * APUSR_FRAMEGEN_MAX_QUEUED_FRAMES + effect.frameSlot;
    if (backend->functions.resetDescriptorPool(reinterpret_cast<vk::Device>(backend->create.device), backend->descriptorPools[poolIndex], 0) != vk::Success)
        return fail("execute_jobs.reset_descriptor_pool", APUSR_FRAMEGEN_ERROR_BACKEND_API);

    const std::uint64_t contextBase = static_cast<std::uint64_t>(effectId) * APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE;
    const std::uint64_t frameSpan = APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE / APUSR_FRAMEGEN_MAX_QUEUED_FRAMES;
    std::uint64_t constantCursor = contextBase + effect.frameSlot * frameSpan;
    const std::uint64_t constantEnd = constantCursor + frameSpan;
    const auto commandBuffer = reinterpret_cast<vk::CommandBuffer>(commandList);

    for (std::uint32_t i = 0; i < backend->gpuJobCount; ++i) {
        const ApusrFramegenGpuJob& job = backend->jobs[i];
        ApusrFramegenResult result = APUSR_FRAMEGEN_OK;
        FramegenGpuProfileFrame* profileFrame = nullptr;
        if (backend->gpuProfile && backend->gpuProfile->activeSlot != UINT32_MAX &&
            backend->gpuProfile->activeCommandBuffer == commandBuffer) {
            profileFrame = &backend->gpuProfile->frames[backend->gpuProfile->activeSlot];
            switch (job.jobType) {
            case APUSR_FRAMEGEN_JOB_CLEAR_FLOAT: ++profileFrame->clearJobs; break;
            case APUSR_FRAMEGEN_JOB_COPY: ++profileFrame->copyJobs; break;
            case APUSR_FRAMEGEN_JOB_BARRIER: ++profileFrame->barrierJobs; break;
            case APUSR_FRAMEGEN_JOB_DISCARD: ++profileFrame->discardJobs; break;
            default: break;
            }
        }
        switch (job.jobType) {
        case APUSR_FRAMEGEN_JOB_COMPUTE:
            result = executeCompute(*backend, commandBuffer, effectId, job.computeJob, constantCursor, constantEnd);
            break;
        case APUSR_FRAMEGEN_JOB_CLEAR_FLOAT: {
            const int32_t index = job.clearJob.target.internalIndex;
            if (!validResourceIndex(*backend, index))
                return fail("execute_jobs.clear.resource", APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT, i);
            NativeResource& resource = backend->resources[index];
            result = transitionResource(*backend, commandBuffer, resource, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS);
            if (result != APUSR_FRAMEGEN_OK) break;
            if (resource.image) {
                vk::ClearColorValue color{};
                std::memcpy(color.float32, job.clearJob.color, sizeof(color.float32));
                const vk::ImageSubresourceRange range{vk::ImageAspectColorBit, 0, resource.mipCount, 0, 1};
                backend->functions.cmdClearColorImage(commandBuffer, resource.image, vk::ImageLayoutGeneral, &color, 1, &range);
            } else if (resource.buffer) {
                std::uint32_t pattern = 0;
                std::memcpy(&pattern, job.clearJob.color, sizeof(pattern));
                backend->functions.cmdFillBuffer(commandBuffer, resource.buffer, 0, vk::WholeSize, pattern);
            } else return fail("execute_jobs.clear.resource_kind", APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT, i);
            break;
        }
        case APUSR_FRAMEGEN_JOB_COPY: {
            const int32_t srcIndex = job.copyJob.src.internalIndex;
            const int32_t dstIndex = job.copyJob.dst.internalIndex;
            if (!validResourceIndex(*backend, srcIndex) || !validResourceIndex(*backend, dstIndex))
                return fail("execute_jobs.copy.resource", APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT, i);
            NativeResource& src = backend->resources[srcIndex];
            NativeResource& dst = backend->resources[dstIndex];
            result = transitionResource(*backend, commandBuffer, src, APUSR_FRAMEGEN_RESOURCE_STATE_COPY_SRC);
            if (result != APUSR_FRAMEGEN_OK) break;
            result = transitionResource(*backend, commandBuffer, dst, APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST);
            if (result != APUSR_FRAMEGEN_OK) break;
            if (src.buffer && dst.buffer) {
                const vk::DeviceSize size = job.copyJob.size ? job.copyJob.size : std::min(src.allocationSize, dst.allocationSize);
                const vk::BufferCopy region{job.copyJob.srcOffset, job.copyJob.dstOffset, size};
                backend->functions.cmdCopyBuffer(commandBuffer, src.buffer, dst.buffer, 1, &region);
            } else if (src.buffer && dst.image) {
                const vk::BufferImageCopy region{
                    job.copyJob.srcOffset, 0, 0, {vk::ImageAspectColorBit, 0, 0, 1}, {0, 0, 0},
                    {dst.description.width, dst.description.height, 1}};
                backend->functions.cmdCopyBufferToImage(commandBuffer, src.buffer, dst.image, vk::ImageLayoutTransferDstOptimal, 1, &region);
            } else if (src.image && dst.image) {
                const vk::ImageCopy region{
                    {vk::ImageAspectColorBit, 0, 0, 1}, {0, 0, 0},
                    {vk::ImageAspectColorBit, 0, 0, 1}, {0, 0, 0},
                    {std::min(src.description.width, dst.description.width), std::min(src.description.height, dst.description.height), 1}};
                backend->functions.cmdCopyImage(commandBuffer, src.image, vk::ImageLayoutTransferSrcOptimal, dst.image, vk::ImageLayoutTransferDstOptimal, 1, &region);
            } else return fail("execute_jobs.copy.resource_kind", APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT, i);
            break;
        }
        case APUSR_FRAMEGEN_JOB_BARRIER: {
            const int32_t index = job.barrierJob.resource.internalIndex;
            if (!validResourceIndex(*backend, index))
                return fail("execute_jobs.barrier.resource", APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT, i);
            result = transitionResource(*backend, commandBuffer, backend->resources[index], job.barrierJob.newState);
            break;
        }
        case APUSR_FRAMEGEN_JOB_DISCARD: {
            const int32_t index = job.discardJob.target.internalIndex;
            if (!validResourceIndex(*backend, index))
                return fail("execute_jobs.discard.resource", APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT, i);
            result = transitionResource(*backend, commandBuffer, backend->resources[index], APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, true);
            break;
        }
        default:
            result = APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
            break;
        }
        if (result != APUSR_FRAMEGEN_OK)
            return fail("execute_jobs.job", result, i);
    }
    backend->gpuJobCount = 0;
    effect.frameSlot = (effect.frameSlot + 1) % APUSR_FRAMEGEN_MAX_QUEUED_FRAMES;
    return APUSR_FRAMEGEN_OK;
}

} // namespace

void apusrFramegenBeginVulkanGpuProfile(
    ApusrFramegenBackendInterface* backendInterface,
    void* commandBuffer,
    const ApusrFramegenVulkanGpuProfileFrameInfo& frame) noexcept
{
    BackendContext* backend = context(backendInterface);
    if (!backend || !commandBuffer)
        return;
    (void)beginFramegenGpuProfile(
        *backend, reinterpret_cast<vk::CommandBuffer>(commandBuffer), frame);
}

void apusrFramegenEndVulkanGpuProfile(
    ApusrFramegenBackendInterface* backendInterface,
    void* commandBuffer) noexcept
{
    BackendContext* backend = context(backendInterface);
    if (!backend || !commandBuffer)
        return;
    endFramegenGpuProfile(*backend, reinterpret_cast<vk::CommandBuffer>(commandBuffer));
}

void apusrFramegenCancelVulkanGpuProfile(
    ApusrFramegenBackendInterface* backendInterface,
    void* commandBuffer) noexcept
{
    BackendContext* backend = context(backendInterface);
    if (!backend || !backend->gpuProfile || !commandBuffer)
        return;
    FramegenGpuProfileContext& profile = *backend->gpuProfile;
    const auto native = reinterpret_cast<vk::CommandBuffer>(commandBuffer);
    if (profile.activeSlot == UINT32_MAX || profile.activeCommandBuffer != native)
        return;
    const std::uint32_t slot = profile.activeSlot;
    profile.frames[slot] = {};
    profile.activeSlot = UINT32_MAX;
    profile.activeQueryCursor = 0u;
    profile.activeCommandBuffer = nullptr;
    profile.activeDispatch = nullptr;
    profile.activeCpuStartNs = 0u;
    // A failed/aborted generation did not produce a sample; do not consume a
    // burst frame or advance the ring.
}

void apusrFramegenFlushVulkanGpuProfile(
    ApusrFramegenBackendInterface* backendInterface) noexcept
{
    BackendContext* backend = context(backendInterface);
    if (!backend || !backend->gpuProfile)
        return;
    for (std::uint32_t slot = 0u; slot < backend->gpuProfile->frames.size(); ++slot)
        (void)resolveFramegenGpuProfileSlot(*backend, slot);
}

ApusrFramegenResult apusrFramegenCreateVulkanBackend(
    const ApusrFramegenVulkanDeviceContext& deviceContext,
    ApusrFramegenBackendInterface* backendInterface,
    const std::size_t contexts,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!backendInterface || !deviceContext.instance || !deviceContext.physicalDevice || !deviceContext.device ||
        !deviceContext.getInstanceProcAddr || contexts == 0 || contexts > std::numeric_limits<std::uint32_t>::max())
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    const std::size_t required = scratchSize(contexts);
    void* scratch = allocator.alloc(required);
    if (!scratch) return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    std::memset(scratch, 0, required);
    auto* backend = reinterpret_cast<BackendContext*>(scratch);
    backend->create = deviceContext;
    backend->maxEffectContexts = static_cast<std::uint32_t>(contexts);
    if (!loadFunctions(*backend)) {
        allocator.dealloc(scratch);
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    std::uint8_t* memory = reinterpret_cast<std::uint8_t*>(backend + 1);
    memory = reinterpret_cast<std::uint8_t*>(alignUp(reinterpret_cast<std::uintptr_t>(memory), alignof(ApusrFramegenGpuJob)));
    backend->jobs = reinterpret_cast<ApusrFramegenGpuJob*>(memory);
    memory += APUSR_FRAMEGEN_MAX_GPU_JOBS * sizeof(ApusrFramegenGpuJob);
    memory = reinterpret_cast<std::uint8_t*>(alignUp(reinterpret_cast<std::uintptr_t>(memory), alignof(NativeResource)));
    backend->resources = reinterpret_cast<NativeResource*>(memory);
    memory += contexts * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT * sizeof(NativeResource);
    memory = reinterpret_cast<std::uint8_t*>(alignUp(reinterpret_cast<std::uintptr_t>(memory), alignof(std::uint32_t)));
    backend->stagingRing = memory;
    memory += contexts * APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE;
    memory = reinterpret_cast<std::uint8_t*>(alignUp(reinterpret_cast<std::uintptr_t>(memory), alignof(EffectContext)));
    backend->effects = reinterpret_cast<EffectContext*>(memory);
    memory += contexts * sizeof(EffectContext);
    memory = reinterpret_cast<std::uint8_t*>(alignUp(reinterpret_cast<std::uintptr_t>(memory), alignof(vk::DescriptorPool)));
    backend->descriptorPools = reinterpret_cast<vk::DescriptorPool*>(memory);

    backendInterface->getVersion = getVersion;
    backendInterface->getGpuMemoryUsage = getMemoryUsage;
    backendInterface->createContext = createContext;
    backendInterface->getDeviceCapabilities = getCapabilities;
    backendInterface->destroyContext = destroyContext;
    backendInterface->createResource = createResource;
    backendInterface->destroyResource = destroyResource;
    backendInterface->registerResource = registerResource;
    backendInterface->getResource = getResource;
    backendInterface->unregisterResources = unregisterResources;
    backendInterface->getResourceDescription = getResourceDescription;
    backendInterface->stageConstants = stageConstants;
    backendInterface->createPipeline = createPipeline;
    backendInterface->destroyPipeline = destroyPipeline;
    backendInterface->scheduleJob = scheduleJob;
    backendInterface->executeJobs = executeJobs;
    backendInterface->scratchBuffer = scratch;
    backendInterface->scratchBufferSize = required;
    backendInterface->device = deviceContext.device;
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenCommandList apusrFramegenCommandListVK(void* commandBuffer) noexcept
{
    return reinterpret_cast<ApusrFramegenCommandList>(commandBuffer);
}

ApusrFramegenExternalResource apusrFramegenExternalResourceVK(
    void* resource,
    const ApusrFramegenResourceDescription description,
    const ApusrFramegenResourceState state) noexcept
{
    return {resource, description, state, nullptr};
}

ApusrFramegenExternalResource apusrFramegenExternalResourceVK(
    void* resource,
    const ApusrFramegenResourceDescription description,
    const ApusrFramegenResourceState state,
    const ApusrFramegenVulkanExternalResourceMetadata* metadata) noexcept
{
    return {resource, description, state, metadata};
}

ApusrFramegenSurfaceFormat apusrFramegenSurfaceFormatFromVulkan(const std::uint32_t nativeFormatValue) noexcept
{
    return surfaceFormatFromNative(static_cast<vk::Format>(nativeFormatValue));
}
