// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/backends/apusr_fsr4_reference_vulkan.h>
#include <apusr/apusr_profiler.h>

#include "apusr_vulkan_minimal.h"
#include "apusr_spirv_metadata.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#endif

namespace apusr::backends {
namespace {

using namespace models::fsr4_int8;

constexpr std::size_t kResourceSlotCount = 32;
constexpr std::uint32_t kDescriptorSetsPerPool = 256;
constexpr std::uint64_t kMaxCmdUpdateBufferBytes = 65536;

constexpr std::uint32_t kVulkanGpuTimestampRingSlots = 12u;
constexpr std::uint32_t kVulkanGpuTimestampQueriesPerSlot = 64u;
constexpr auto kVulkanGpuTimestampSampleInterval = std::chrono::seconds(1);
constexpr std::uint32_t kVulkanGpuTimestampBurstFrames = 3u;

std::atomic<std::uint64_t> gVulkanGpuTimestampSequence{0u};
std::atomic<std::uint64_t> gVulkanGpuTimestampBurstSequence{0u};
std::atomic<std::uint64_t> gVulkanGpuTimestampRuntimeSequence{0u};
std::mutex gVulkanGpuTimestampFileMutex;
std::unordered_set<std::string> gVulkanGpuTimestampInitializedPaths;

bool vulkanLegacyGpuTimestampsEnabled() noexcept
{
    static const bool enabled = []() noexcept {
#if defined(_WIN32)
        char value[16]{};
        const DWORD size = GetEnvironmentVariableA("APUSR_VULKAN_GPU_TIMESTAMPS", value, static_cast<DWORD>(sizeof(value)));
        if (size == 0u || size >= sizeof(value)) {
            return false;
        }
        const char c = value[0];
#else
        const char* value = std::getenv("APUSR_VULKAN_GPU_TIMESTAMPS");
        if (value == nullptr || value[0] == '\0') {
            return false;
        }
        const char c = value[0];
#endif
        return c != '0' && c != 'n' && c != 'N' && c != 'f' && c != 'F' && c != 'o' && c != 'O';
    }();
    return enabled;
}

bool vulkanGpuTimestampsEnabled() noexcept
{
    return vulkanLegacyGpuTimestampsEnabled() || apusr::profile::gpuEnabled();
}

std::filesystem::path vulkanGpuTimestampPath()
{
#if defined(_WIN32)
    char buffer[32768]{};
    const DWORD size = GetEnvironmentVariableA("APUSR_LOG_DIR", buffer, static_cast<DWORD>(sizeof(buffer)));
    if (size != 0u && size < sizeof(buffer)) {
        return std::filesystem::path(std::string(buffer, size)) / "apusr_vulkan_gpu_timestamps.csv";
    }
    return std::filesystem::path("C:/NGXLogs/APUSR/apusr_vulkan_gpu_timestamps.csv");
#else
    if (const char* dir = std::getenv("APUSR_LOG_DIR"); dir != nullptr && dir[0] != '\0') {
        return std::filesystem::path(dir) / "apusr_vulkan_gpu_timestamps.csv";
    }
    return std::filesystem::path("/tmp/APUSR/apusr_vulkan_gpu_timestamps.csv");
#endif
}

struct VulkanGpuTimestampDispatchMetadata {
    std::string name;
    std::uint32_t groupsX = 0u;
    std::uint32_t groupsY = 0u;
    std::uint32_t groupsZ = 0u;
    std::uint64_t shaderBytes = 0u;
    std::uint32_t barrierCalls = 0u;
    std::uint32_t bufferBarriers = 0u;
    std::uint32_t imageBarriers = 0u;
    std::uint32_t clearCalls = 0u;
    bool pipelineCreated = false;
    bool descriptorSetCreated = false;
    std::uint32_t setupBeginQuery = 0u;
    std::uint32_t dispatchBeginQuery = 0u;
    std::uint32_t endQuery = 0u;
};

struct VulkanGpuTimestampFrameMetadata {
    bool valid = false;
    std::uint64_t sequence = 0u;
    std::uint64_t sampleSequence = 0u;
    std::uint64_t configurationHash = 0u;
    bool unifiedProfile = false;
    std::uint64_t runtimeSequence = 0u;
    std::uint32_t apiVersion = 0u;
    std::uint32_t driverVersion = 0u;
    std::uint32_t vendorId = 0u;
    std::uint32_t deviceId = 0u;
    std::uint64_t burstSequence = 0u;
    std::uint32_t burstFrame = 0u;
    bool configurationChanged = false;
    std::uint64_t frameIndex = 0u;
    std::uint32_t renderWidth = 0u;
    std::uint32_t renderHeight = 0u;
    std::uint32_t displayWidth = 0u;
    std::uint32_t displayHeight = 0u;
    std::uint32_t alignedDisplayWidth = 0u;
    std::uint32_t alignedDisplayHeight = 0u;
    std::uint32_t motionVectorWidth = 0u;
    std::uint32_t motionVectorHeight = 0u;
    std::uint64_t totalWorkgroups = 0u;
    std::uint64_t colorNativeFormat = 0u;
    std::uint64_t colorNativeLayout = kFrameInputUnknownNativeState;
    std::uint64_t depthNativeFormat = 0u;
    std::uint64_t depthNativeLayout = kFrameInputUnknownNativeState;
    std::uint64_t motionVectorNativeFormat = 0u;
    std::uint64_t motionVectorNativeLayout = kFrameInputUnknownNativeState;
    std::uint64_t exposureNativeFormat = 0u;
    std::uint64_t exposureNativeLayout = kFrameInputUnknownNativeState;
    std::uint64_t outputNativeFormat = 0u;
    std::uint64_t outputNativeLayout = kFrameInputUnknownNativeState;
    PipelineVariant pipelineVariant = PipelineVariant::StrictReference;
    Preset preset = Preset::Balanced;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    std::uint32_t permutationKey = 0u;
    PermutationDesc permutation{};
    bool reset = false;
    bool rcasEnabled = false;
    bool defaultExposure = false;
    float sharpness = 0.0f;
    float preExposure = 1.0f;
    double timestampPeriodNs = 0.0;
    std::uint32_t frameBeginQuery = 0u;
    std::uint32_t constantsEndQuery = 0u;
    std::uint32_t initializersEndQuery = 0u;
    std::uint32_t frameEndQuery = 0u;
    std::uint32_t queryCount = 0u;
    std::uint32_t timestampValidBits = 64u;
    std::uint32_t queueFamily = kFrameInputUnknownQueueFamily;
    std::uint64_t cpuFrameIntervalNs = 0u;
    std::uint64_t cpuPlanNs = 0u;
    std::uint64_t cpuBindingNs = 0u;
    std::uint64_t cpuCommandRecordNs = 0u;
    std::uint64_t cpuTotalNs = 0u;
    std::uint64_t constantUpdateBytes = 0u;
    std::uint64_t initializerUpdateBytes = 0u;
    bool initializerUploaded = false;
    bool defaultExposureInitialized = false;
    std::uint32_t pipelinesCreated = 0u;
    std::uint32_t descriptorSetsCreated = 0u;
    std::vector<VulkanGpuTimestampDispatchMetadata> dispatches;
};

struct VulkanGpuTimestampSampleSignature {
    PipelineVariant pipelineVariant = PipelineVariant::StrictReference;
    Preset preset = Preset::Balanced;
    FrameCapacity frameCapacity = FrameCapacity::R2160;
    std::uint32_t permutationKey = 0u;
    std::uint32_t renderWidth = 0u;
    std::uint32_t renderHeight = 0u;
    std::uint32_t displayWidth = 0u;
    std::uint32_t displayHeight = 0u;
    bool rcasEnabled = false;

    bool operator==(const VulkanGpuTimestampSampleSignature&) const noexcept = default;
};

class MappedShaderPackFile {
public:
    MappedShaderPackFile() = default;
    ~MappedShaderPackFile() { close(); }

    MappedShaderPackFile(const MappedShaderPackFile&) = delete;
    MappedShaderPackFile& operator=(const MappedShaderPackFile&) = delete;

    [[nodiscard]] Status open(const std::filesystem::path& path)
    {
        close();
        if (path.empty()) {
            return Status::invalidArgument("Vulkan FSR4 shader pack path is empty");
        }

#if defined(_WIN32)
        HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return Status::missingBackend("Vulkan FSR4 shader pack not found: " + path.string());
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart <= 0 ||
            static_cast<unsigned long long>(fileSize.QuadPart) > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            CloseHandle(file);
            return Status::runtimeFailure("Vulkan FSR4 shader pack has an invalid file size: " + path.string());
        }

        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) {
            CloseHandle(file);
            return Status::runtimeFailure("could not create read-only mapping for Vulkan FSR4 shader pack: " + path.string());
        }
        const void* mapped = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        CloseHandle(mapping);
        CloseHandle(file);
        if (mapped == nullptr) {
            return Status::runtimeFailure("could not map Vulkan FSR4 shader pack: " + path.string());
        }
        data_ = static_cast<const std::uint8_t*>(mapped);
        size_ = static_cast<std::size_t>(fileSize.QuadPart);
#else
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return Status::missingBackend("Vulkan FSR4 shader pack not found: " + path.string());
        }
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
            static_cast<unsigned long long>(st.st_size) > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max())) {
            ::close(fd);
            return Status::runtimeFailure("Vulkan FSR4 shader pack has an invalid file size: " + path.string());
        }
        void* mapped = mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (mapped == MAP_FAILED) {
            return Status::runtimeFailure("could not map Vulkan FSR4 shader pack: " + path.string());
        }
        data_ = static_cast<const std::uint8_t*>(mapped);
        size_ = static_cast<std::size_t>(st.st_size);
#endif
        return Status::ok();
    }

    void close() noexcept
    {
#if defined(_WIN32)
        if (data_ != nullptr) {
            UnmapViewOfFile(data_);
        }
#else
        if (data_ != nullptr && size_ != 0) {
            munmap(const_cast<std::uint8_t*>(data_), size_);
        }
#endif
        data_ = nullptr;
        size_ = 0;
    }

    [[nodiscard]] VulkanFsr4ReferenceShaderPackView view() const noexcept
    {
        return VulkanFsr4ReferenceShaderPackView{data_, size_};
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

std::size_t resourceIndex(const ResourceId id) noexcept
{
    return static_cast<std::size_t>(id);
}

ResourceId resolveResource(const ResourcePlan& resources, const ReferenceFramePlan& frame, const ResourceId id) noexcept
{
    if (id == ResourceId::InputExposure && frame.constants.useDefaultExposure != 0) {
        return ResourceId::InternalDefaultExposure;
    }
    if (const ResourceAliasDesc* alias = findAlias(resources, id)) {
        return frame.constants.rcasEnabled != 0 ? alias->rcasEnabledTarget : alias->defaultTarget;
    }
    return id;
}

VulkanFsr4ReferenceDescriptorKind descriptorKind(const ReferenceBindingClass bindingClass)
{
    switch (bindingClass) {
    case ReferenceBindingClass::Sampler: return VulkanFsr4ReferenceDescriptorKind::Sampler;
    case ReferenceBindingClass::SrvTexture: return VulkanFsr4ReferenceDescriptorKind::SampledImage;
    case ReferenceBindingClass::UavTexture: return VulkanFsr4ReferenceDescriptorKind::StorageImage;
    case ReferenceBindingClass::ConstantBuffer: return VulkanFsr4ReferenceDescriptorKind::UniformBuffer;
    case ReferenceBindingClass::SrvBuffer:
    case ReferenceBindingClass::UavBuffer:
        return VulkanFsr4ReferenceDescriptorKind::StorageBuffer;
    }
    return VulkanFsr4ReferenceDescriptorKind::StorageBuffer;
}

vk::AccessFlags shaderAccessFlags(const ReferenceResourceAccess access) noexcept
{
    switch (access) {
    case ReferenceResourceAccess::Read:
        return vk::AccessShaderReadBit;
    case ReferenceResourceAccess::Write:
        return vk::AccessShaderWriteBit;
    case ReferenceResourceAccess::ReadWrite:
        return vk::AccessShaderReadBit | vk::AccessShaderWriteBit;
    }
    return 0u;
}

bool needsShaderMemoryDependency(const vk::AccessFlags prior, const vk::AccessFlags desired) noexcept
{
    const bool priorWrites = (prior & vk::AccessShaderWriteBit) != 0u;
    const bool nextWrites = (desired & vk::AccessShaderWriteBit) != 0u;
    return prior != 0u && (priorWrites || nextWrites);
}

std::uint32_t descriptorBindingNumber(const ReferenceResourceBindingDesc& binding)
{
    switch (binding.bindingClass) {
    case ReferenceBindingClass::SrvTexture:
    case ReferenceBindingClass::SrvBuffer:
        return kVulkanFsr4SrvBindingBase + binding.shaderRegister;
    case ReferenceBindingClass::UavTexture:
    case ReferenceBindingClass::UavBuffer:
        return kVulkanFsr4UavBindingBase + binding.shaderRegister;
    case ReferenceBindingClass::ConstantBuffer:
        return kVulkanFsr4CbvBindingBase + binding.shaderRegister;
    case ReferenceBindingClass::Sampler:
        return kVulkanFsr4SamplerBindingBase + binding.shaderRegister;
    }
    return binding.shaderRegister;
}

vk::DescriptorType nativeDescriptorType(const VulkanFsr4ReferenceDescriptorKind kind)
{
    switch (kind) {
    case VulkanFsr4ReferenceDescriptorKind::Sampler: return vk::DescriptorTypeSampler;
    case VulkanFsr4ReferenceDescriptorKind::SampledImage: return vk::DescriptorTypeSampledImage;
    case VulkanFsr4ReferenceDescriptorKind::StorageImage: return vk::DescriptorTypeStorageImage;
    case VulkanFsr4ReferenceDescriptorKind::UniformBuffer: return vk::DescriptorTypeUniformBuffer;
    case VulkanFsr4ReferenceDescriptorKind::StorageBuffer: return vk::DescriptorTypeStorageBuffer;
    }
    return vk::DescriptorTypeStorageBuffer;
}

vk::Format nativeFormat(const SurfaceFormat format)
{
    switch (format) {
    case SurfaceFormat::Rgba8Unorm: return vk::FormatR8G8B8A8Unorm;
    case SurfaceFormat::R16Unorm: return vk::FormatR16Unorm;
    case SurfaceFormat::Rg16Float: return vk::FormatR16G16Sfloat;
    case SurfaceFormat::Rgba16Float: return vk::FormatR16G16B16A16Sfloat;
    case SurfaceFormat::R32Uint: return vk::FormatR32Uint;
    case SurfaceFormat::R32Float: return vk::FormatR32Sfloat;
    case SurfaceFormat::Rg32Float: return vk::FormatR32G32Sfloat;
    default: return 0;
    }
}

FrameInputSurfaceRole inputRoleForResource(const ResourceId id)
{
    switch (id) {
    case ResourceId::InputColor: return FrameInputSurfaceRole::Color;
    case ResourceId::InputDepth: return FrameInputSurfaceRole::Depth;
    case ResourceId::InputMotionVectors: return FrameInputSurfaceRole::MotionVectors;
    case ResourceId::InputExposure: return FrameInputSurfaceRole::Exposure;
    case ResourceId::FinalOutput: return FrameInputSurfaceRole::Output;
    default: return FrameInputSurfaceRole::Color;
    }
}

bool isExternalResource(const ResourceId id) noexcept
{
    return id == ResourceId::InputColor || id == ResourceId::InputDepth ||
        id == ResourceId::InputMotionVectors || id == ResourceId::InputExposure ||
        id == ResourceId::FinalOutput;
}

std::string vkResultText(const char* operation, const vk::Result result)
{
    std::ostringstream out;
    out << operation << " failed with VkResult " << result;
    return out.str();
}

constexpr std::array<std::uint8_t, 8> kShaderPackMagic{{'A', 'P', 'U', 'S', 'R', 'V', 'K', 0}};

struct ShaderPackHeaderDisk {
    std::uint8_t magic[8];
    std::uint32_t headerBytes;
    std::uint32_t entryCount;
    std::uint32_t blobCount;
    std::uint32_t entriesOffset;
    std::uint32_t blobsOffset;
    std::uint32_t dataOffset;
    std::uint32_t totalBytes;
};

struct ShaderPackEntryDisk {
    std::uint32_t pipelineVariant;
    std::uint32_t preset;
    std::uint32_t frameCapacity;
    std::uint32_t permutationKey;
    std::uint32_t passKind;
    std::uint32_t logicalStage;
    std::uint32_t blobIndex;
};

struct ShaderPackBlobDisk {
    std::uint32_t offset;
    std::uint32_t size;
};

static_assert(sizeof(ShaderPackHeaderDisk) == 36);
static_assert(sizeof(ShaderPackEntryDisk) == 28);
static_assert(sizeof(ShaderPackBlobDisk) == 8);

template <typename T>
bool readShaderPackObject(const VulkanFsr4ReferenceShaderPackView pack, const std::size_t offset, T& out) noexcept
{
    if (pack.data == nullptr || offset > pack.size || sizeof(T) > pack.size - offset) {
        return false;
    }
    std::memcpy(&out, pack.data + offset, sizeof(T));
    return true;
}

bool shaderKeyLess(const ShaderPackEntryDisk& entry, const VulkanFsr4ReferenceShaderKey& key) noexcept
{
    const std::array<std::uint32_t, 6> lhs{{
        entry.pipelineVariant, entry.preset, entry.frameCapacity, entry.permutationKey, entry.passKind, entry.logicalStage}};
    const std::array<std::uint32_t, 6> rhs{{
        static_cast<std::uint32_t>(key.pipelineVariant),
        static_cast<std::uint32_t>(key.preset),
        static_cast<std::uint32_t>(key.frameCapacity),
        key.permutationKey,
        static_cast<std::uint32_t>(key.passKind),
        key.logicalStage}};
    return lhs < rhs;
}

bool shaderKeyEqual(const ShaderPackEntryDisk& entry, const VulkanFsr4ReferenceShaderKey& key) noexcept
{
    return entry.pipelineVariant == static_cast<std::uint32_t>(key.pipelineVariant) &&
        entry.preset == static_cast<std::uint32_t>(key.preset) &&
        entry.frameCapacity == static_cast<std::uint32_t>(key.frameCapacity) &&
        entry.permutationKey == key.permutationKey &&
        entry.passKind == static_cast<std::uint32_t>(key.passKind) &&
        entry.logicalStage == key.logicalStage;
}

struct VulkanShaderKeyHash {
    std::size_t operator()(const VulkanFsr4ReferenceShaderKey& key) const noexcept
    {
        std::size_t hash = 1469598103934665603ull;
        const std::array<std::uint32_t, 6> fields{{
            static_cast<std::uint32_t>(key.pipelineVariant),
            static_cast<std::uint32_t>(key.preset),
            static_cast<std::uint32_t>(key.frameCapacity),
            key.permutationKey,
            static_cast<std::uint32_t>(key.passKind),
            key.logicalStage}};
        for (const std::uint32_t value : fields) {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        return hash;
    }
};

struct VulkanShaderKeyEqual {
    bool operator()(const VulkanFsr4ReferenceShaderKey& lhs, const VulkanFsr4ReferenceShaderKey& rhs) const noexcept
    {
        return lhs.pipelineVariant == rhs.pipelineVariant && lhs.preset == rhs.preset &&
            lhs.frameCapacity == rhs.frameCapacity && lhs.permutationKey == rhs.permutationKey &&
            lhs.passKind == rhs.passKind && lhs.logicalStage == rhs.logicalStage;
    }
};

struct VulkanLibrary {
#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif

    ~VulkanLibrary()
    {
        close();
    }

    void close() noexcept
    {
#if defined(_WIN32)
        if (handle != nullptr) {
            FreeLibrary(handle);
            handle = nullptr;
        }
#else
        if (handle != nullptr) {
            dlclose(handle);
            handle = nullptr;
        }
#endif
    }

    Status open()
    {
        close();
#if defined(_WIN32)
        handle = LoadLibraryW(L"vulkan-1.dll");
        if (handle == nullptr) {
            return Status::missingBackend("Vulkan loader vulkan-1.dll is unavailable");
        }
#else
        handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            return Status::missingBackend("Vulkan loader libvulkan.so.1 is unavailable");
        }
#endif
        return Status::ok();
    }

    void* symbol(const char* name) const noexcept
    {
#if defined(_WIN32)
        return handle != nullptr ? reinterpret_cast<void*>(GetProcAddress(handle, name)) : nullptr;
#else
        return handle != nullptr ? dlsym(handle, name) : nullptr;
#endif
    }
};

struct VulkanFunctions {
    vk::PfnGetInstanceProcAddr getInstanceProcAddr = nullptr;
    vk::PfnGetDeviceProcAddr getDeviceProcAddr = nullptr;
    vk::PfnGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
    vk::PfnGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
    vk::PfnGetPhysicalDeviceFormatProperties getPhysicalDeviceFormatProperties = nullptr;
    vk::PfnGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties = nullptr;
    vk::PfnGetPhysicalDeviceCalibrateableTimeDomains getPhysicalDeviceCalibrateableTimeDomains = nullptr;
    vk::PfnGetCalibratedTimestamps getCalibratedTimestamps = nullptr;
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
    vk::PfnAllocateDescriptorSets allocateDescriptorSets = nullptr;
    vk::PfnUpdateDescriptorSets updateDescriptorSets = nullptr;
    vk::PfnCreateSampler createSampler = nullptr;
    vk::PfnDestroySampler destroySampler = nullptr;
    vk::PfnCreateQueryPool createQueryPool = nullptr;
    vk::PfnDestroyQueryPool destroyQueryPool = nullptr;
    vk::PfnGetQueryPoolResults getQueryPoolResults = nullptr;
    vk::PfnCmdResetQueryPool cmdResetQueryPool = nullptr;
    vk::PfnCmdWriteTimestamp cmdWriteTimestamp = nullptr;
    vk::PfnDeviceWaitIdle deviceWaitIdle = nullptr;
    vk::PfnCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    vk::PfnCmdUpdateBuffer cmdUpdateBuffer = nullptr;
    vk::PfnCmdClearColorImage cmdClearColorImage = nullptr;
    vk::PfnCmdBindPipeline cmdBindPipeline = nullptr;
    vk::PfnCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    vk::PfnCmdDispatch cmdDispatch = nullptr;
};

template <typename Fn>
Status loadDeviceFunction(const VulkanFunctions& base, const vk::Device device, const char* name, Fn& out)
{
    out = reinterpret_cast<Fn>(base.getDeviceProcAddr(device, name));
    if (out == nullptr) {
        return Status::missingBackend(std::string("Vulkan device function is unavailable: ") + name);
    }
    return Status::ok();
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

struct NativeResource {
    ResourceId id = ResourceId::Null;
    ResourceKind kind = ResourceKind::Texture2D;
    vk::Buffer buffer = nullptr;
    vk::Image image = nullptr;
    vk::ImageView view = nullptr;
    vk::DeviceMemory memory = nullptr;
    vk::ImageLayout layout = vk::ImageLayoutUndefined;
    vk::PipelineStageFlags imageStage = 0;
    vk::AccessFlags imageAccess = 0;
    vk::AccessFlags bufferAccess = 0;
    std::uint64_t byteSize = 0;
};

struct ExternalImageState {
    bool known = false;
    vk::ImageLayout layout = vk::ImageLayoutUndefined;
    vk::PipelineStageFlags stage = 0;
    vk::AccessFlags access = 0;
};

using ExternalImageStates = std::array<ExternalImageState, kFrameInputMaxSurfaces>;

std::size_t externalImageStateIndex(const FrameInputSurfaceRole role) noexcept
{
    return static_cast<std::size_t>(role);
}

ExternalImageStates makeExternalImageStates(const FrameInputDesc& frameInput) noexcept
{
    ExternalImageStates states{};
    for (std::uint32_t index = 0; index < frameInput.surfaceCount; ++index) {
        const FrameInputSurfaceDesc& surface = frameInput.surfaces[index];
        const std::size_t stateIndex = externalImageStateIndex(surface.role);
        if (stateIndex >= states.size() || surface.nativeStateOrLayout == kFrameInputUnknownNativeState) {
            continue;
        }
        states[stateIndex] = ExternalImageState{
            .known = true,
            .layout = static_cast<vk::ImageLayout>(surface.nativeStateOrLayout),
            .stage = surface.nativeStageMask != 0
                ? static_cast<vk::PipelineStageFlags>(surface.nativeStageMask)
                : vk::PipelineStageAllCommandsBit,
            .access = surface.nativeAccessMask != 0
                ? static_cast<vk::AccessFlags>(surface.nativeAccessMask)
                : 0u,
        };
    }
    return states;
}

struct ConstantResource {
    ReferenceConstantBufferRole role = ReferenceConstantBufferRole::None;
    vk::Buffer buffer = nullptr;
    vk::DeviceMemory memory = nullptr;
    std::uint64_t byteSize = 0;
};

struct ExternalDescriptorKey {
    std::array<std::uintptr_t, 5> views{};
    std::uint32_t variantBits = 0;

    bool operator==(const ExternalDescriptorKey& other) const noexcept
    {
        return views == other.views && variantBits == other.variantBits;
    }
};

struct ExternalDescriptorKeyHash {
    std::size_t operator()(const ExternalDescriptorKey& key) const noexcept
    {
        std::size_t hash = 1469598103934665603ull;
        for (const std::uintptr_t value : key.views) {
            hash ^= static_cast<std::size_t>(value);
            hash *= 1099511628211ull;
        }
        hash ^= key.variantBits;
        hash *= 1099511628211ull;
        return hash;
    }
};

struct PipelineState {
    vk::DescriptorSetLayout descriptorSetLayout = nullptr;
    std::uint64_t shaderByteSize = 0u;
    vk::PipelineLayout pipelineLayout = nullptr;
    vk::Pipeline pipeline = nullptr;
    std::unordered_map<ExternalDescriptorKey, vk::DescriptorSet, ExternalDescriptorKeyHash> descriptorSets;
};

struct DispatchPreparationStats {
    std::uint32_t barrierCalls = 0u;
    std::uint32_t bufferBarriers = 0u;
    std::uint32_t imageBarriers = 0u;
    std::uint32_t clearCalls = 0u;
};

class VulkanFsr4ReferenceRuntimeImpl final : public VulkanFsr4ReferenceRuntime {
public:
    ~VulkanFsr4ReferenceRuntimeImpl() override
    {
        destroy();
    }

    Status initialize(const VulkanFsr4ReferenceCreateDesc& desc) override
    {
        destroy();
        if (desc.instance.value == nullptr || desc.physicalDevice.value == nullptr || desc.device.value == nullptr) {
            return Status::invalidArgument("Vulkan FSR4 reference runtime requires instance, physical device, and device handles");
        }
        if (desc.runner.maxRenderSize.width == 0 || desc.runner.maxRenderSize.height == 0 ||
            desc.runner.maxUpscaleSize.width == 0 || desc.runner.maxUpscaleSize.height == 0) {
            return Status::invalidArgument("Vulkan FSR4 reference runtime requires non-zero maximum render and display sizes");
        }
        if (desc.requiredSubgroupSize != 0u &&
            desc.requiredSubgroupSize != 32u &&
            desc.requiredSubgroupSize != 64u) {
            return Status::invalidArgument("Vulkan FSR4 required subgroup size must be auto (0), 32, or 64");
        }
        VulkanFsr4ReferenceShaderPackView shaderPack = desc.shaderPack;
        if (shaderPack.data == nullptr || shaderPack.size == 0) {
            if (desc.shaderPackPath.empty()) {
                return Status::missingBackend("Vulkan FSR4 shader pack path is unavailable");
            }
            Status mapStatus = shaderPackFile_.open(desc.shaderPackPath);
            if (!mapStatus.succeeded()) {
                return mapStatus;
            }
            shaderPack = shaderPackFile_.view();
        }
        Status status = validateVulkanFsr4ReferenceShaderPack(shaderPack);
        if (!status.succeeded()) {
            if (!desc.shaderPackPath.empty()) {
                status.message += ": " + desc.shaderPackPath.string();
            }
            return status;
        }

        createDesc_ = desc;
        createDesc_.shaderPack = shaderPack;
        instance_ = reinterpret_cast<vk::Instance>(desc.instance.value);
        physicalDevice_ = reinterpret_cast<vk::PhysicalDevice>(desc.physicalDevice.value);
        device_ = reinterpret_cast<vk::Device>(desc.device.value);
        queueFamilyIndices_.clear();
        const auto appendQueueFamily = [&](const std::uint32_t family) {
            if (family != kFrameInputUnknownQueueFamily &&
                std::find(queueFamilyIndices_.begin(), queueFamilyIndices_.end(), family) == queueFamilyIndices_.end()) {
                queueFamilyIndices_.push_back(family);
            }
        };
        appendQueueFamily(desc.graphicsQueueFamilyIndex);
        appendQueueFamily(desc.computeQueueFamilyIndex);
        if (queueFamilyIndices_.empty()) {
            return Status::invalidArgument("Vulkan FSR4 reference runtime requires Streamline graphics/compute queue-family information");
        }

        status = library_.open();
        if (!status.succeeded()) {
            return status;
        }
        functions_.getInstanceProcAddr = reinterpret_cast<vk::PfnGetInstanceProcAddr>(library_.symbol("vkGetInstanceProcAddr"));
        if (functions_.getInstanceProcAddr == nullptr) {
            return Status::missingBackend("Vulkan loader does not export vkGetInstanceProcAddr");
        }
        functions_.getDeviceProcAddr = reinterpret_cast<vk::PfnGetDeviceProcAddr>(
            functions_.getInstanceProcAddr(instance_, "vkGetDeviceProcAddr"));
        functions_.getPhysicalDeviceProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceProperties>(
            functions_.getInstanceProcAddr(instance_, "vkGetPhysicalDeviceProperties"));
        functions_.getPhysicalDeviceMemoryProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceMemoryProperties>(
            functions_.getInstanceProcAddr(instance_, "vkGetPhysicalDeviceMemoryProperties"));
        functions_.getPhysicalDeviceFormatProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceFormatProperties>(
            functions_.getInstanceProcAddr(instance_, "vkGetPhysicalDeviceFormatProperties"));
        functions_.getPhysicalDeviceQueueFamilyProperties = reinterpret_cast<vk::PfnGetPhysicalDeviceQueueFamilyProperties>(
            functions_.getInstanceProcAddr(instance_, "vkGetPhysicalDeviceQueueFamilyProperties"));
        if (functions_.getDeviceProcAddr == nullptr || functions_.getPhysicalDeviceProperties == nullptr ||
            functions_.getPhysicalDeviceMemoryProperties == nullptr || functions_.getPhysicalDeviceFormatProperties == nullptr) {
            return Status::missingBackend("Vulkan loader is missing required instance/device discovery functions");
        }

        functions_.getPhysicalDeviceCalibrateableTimeDomains =
            reinterpret_cast<vk::PfnGetPhysicalDeviceCalibrateableTimeDomains>(
                functions_.getInstanceProcAddr(instance_, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"));
        if (functions_.getPhysicalDeviceCalibrateableTimeDomains == nullptr) {
            functions_.getPhysicalDeviceCalibrateableTimeDomains =
                reinterpret_cast<vk::PfnGetPhysicalDeviceCalibrateableTimeDomains>(
                    functions_.getInstanceProcAddr(instance_, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
        }
        functions_.getCalibratedTimestamps = reinterpret_cast<vk::PfnGetCalibratedTimestamps>(
            functions_.getDeviceProcAddr(device_, "vkGetCalibratedTimestampsKHR"));
        if (functions_.getCalibratedTimestamps == nullptr) {
            functions_.getCalibratedTimestamps = reinterpret_cast<vk::PfnGetCalibratedTimestamps>(
                functions_.getDeviceProcAddr(device_, "vkGetCalibratedTimestampsEXT"));
        }

#define APUSR_LOAD_VK(name, field) do { status = loadDeviceFunction(functions_, device_, name, functions_.field); if (!status.succeeded()) { return status; } } while (false)
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
        APUSR_LOAD_VK("vkAllocateDescriptorSets", allocateDescriptorSets);
        APUSR_LOAD_VK("vkUpdateDescriptorSets", updateDescriptorSets);
        APUSR_LOAD_VK("vkCreateSampler", createSampler);
        APUSR_LOAD_VK("vkDestroySampler", destroySampler);
        APUSR_LOAD_VK("vkDeviceWaitIdle", deviceWaitIdle);
        APUSR_LOAD_VK("vkCmdPipelineBarrier", cmdPipelineBarrier);
        APUSR_LOAD_VK("vkCmdUpdateBuffer", cmdUpdateBuffer);
        APUSR_LOAD_VK("vkCmdClearColorImage", cmdClearColorImage);
        APUSR_LOAD_VK("vkCmdBindPipeline", cmdBindPipeline);
        APUSR_LOAD_VK("vkCmdBindDescriptorSets", cmdBindDescriptorSets);
        APUSR_LOAD_VK("vkCmdDispatch", cmdDispatch);
#undef APUSR_LOAD_VK

        gpuTimestampRequested_ = vulkanGpuTimestampsEnabled();
        if (gpuTimestampRequested_) {
            functions_.createQueryPool = reinterpret_cast<vk::PfnCreateQueryPool>(functions_.getDeviceProcAddr(device_, "vkCreateQueryPool"));
            functions_.destroyQueryPool = reinterpret_cast<vk::PfnDestroyQueryPool>(functions_.getDeviceProcAddr(device_, "vkDestroyQueryPool"));
            functions_.getQueryPoolResults = reinterpret_cast<vk::PfnGetQueryPoolResults>(functions_.getDeviceProcAddr(device_, "vkGetQueryPoolResults"));
            functions_.cmdResetQueryPool = reinterpret_cast<vk::PfnCmdResetQueryPool>(functions_.getDeviceProcAddr(device_, "vkCmdResetQueryPool"));
            functions_.cmdWriteTimestamp = reinterpret_cast<vk::PfnCmdWriteTimestamp>(functions_.getDeviceProcAddr(device_, "vkCmdWriteTimestamp"));
            if (functions_.createQueryPool == nullptr || functions_.destroyQueryPool == nullptr ||
                functions_.getQueryPoolResults == nullptr || functions_.cmdResetQueryPool == nullptr ||
                functions_.cmdWriteTimestamp == nullptr) {
                gpuTimestampRequested_ = false;
            }
        }

        functions_.getPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);
        vk::PhysicalDeviceProperties physicalProperties{};
        functions_.getPhysicalDeviceProperties(physicalDevice_, &physicalProperties);
        gpuTimestampPeriodNs_ = physicalProperties.limits.timestampPeriod > 0.0f
            ? static_cast<double>(physicalProperties.limits.timestampPeriod)
            : 0.0;
        gpuTimestampApiVersion_ = physicalProperties.apiVersion;
        gpuTimestampDriverVersion_ = physicalProperties.driverVersion;
        gpuTimestampVendorId_ = physicalProperties.vendorID;
        gpuTimestampDeviceId_ = physicalProperties.deviceID;
        gpuClockCalibrationAvailable_ = false;
        gpuClockHostTimeDomain_ = profilerHostTimeDomain();
        if (functions_.getPhysicalDeviceCalibrateableTimeDomains != nullptr && functions_.getCalibratedTimestamps != nullptr) {
            std::array<vk::TimeDomain, 8> domains{};
            std::uint32_t domainCount = static_cast<std::uint32_t>(domains.size());
            if (functions_.getPhysicalDeviceCalibrateableTimeDomains(physicalDevice_, &domainCount, domains.data()) == vk::Success) {
                const std::uint32_t availableCount = std::min(domainCount, static_cast<std::uint32_t>(domains.size()));
                bool haveDevice = false;
                bool haveHost = false;
                for (std::uint32_t index = 0u; index < availableCount; ++index) {
                    haveDevice = haveDevice || domains[index] == vk::TimeDomainDevice;
                    haveHost = haveHost || domains[index] == gpuClockHostTimeDomain_;
                }
                gpuClockCalibrationAvailable_ = haveDevice && haveHost;
            }
        }
        profileStream_ = apusr::profile::registerStream(apusr::profile::StreamDesc{
            .backend = apusr::profile::BackendApi::Vulkan,
            .component = apusr::profile::Component::Upscaler,
            .name = "fsr4.reference",
            .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                apusr::profile::StreamCapabilityFrames |
                apusr::profile::StreamCapabilityGpuIntervals |
                apusr::profile::StreamCapabilityCalibration |
                apusr::profile::StreamCapabilityCounters |
                apusr::profile::StreamCapabilityErrors |
                apusr::profile::StreamCapabilityStateTransitions |
                apusr::profile::StreamCapabilityGpuWorkloadInfo,
            .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(device_)),
            .apiVersion = gpuTimestampApiVersion_,
            .driverVersion = gpuTimestampDriverVersion_,
            .vendorId = gpuTimestampVendorId_,
            .deviceId = gpuTimestampDeviceId_,
        });
        profileSampleGate_.reset();
        gpuTimestampRuntimeSequence_ = gVulkanGpuTimestampRuntimeSequence.fetch_add(1u, std::memory_order_relaxed);
        resourcePlan_ = makeReferenceResourcePlan(ResourcePlanDesc{
            .maxRenderSize = desc.runner.maxRenderSize,
            .maxUpscaleSize = desc.runner.maxUpscaleSize,
            .includeExternalResources = true,
            .includeDebugInformation = desc.runner.allowDebugVisualization,
        });

        status = prepareInitializerBytesForPreset(desc.runner.initialPreset);
        if (!status.succeeded()) {
            return status;
        }

        status = createInternalResources();
        if (!status.succeeded()) {
            return status;
        }
        status = createConstantResources();
        if (!status.succeeded()) {
            return status;
        }
        status = createSampler();
        if (!status.succeeded()) {
            return status;
        }
        status = createDescriptorPool();
        if (!status.succeeded()) {
            return status;
        }
        initializeGpuTimestampProfiler();

        initialized_ = true;
        return Status::ok();
    }

    Status prepareFrameCache(const VulkanFsr4ReferenceFrameDesc& desc) override
    {
        if (!initialized_) {
            return Status::runtimeFailure("Vulkan FSR4 reference runtime is not initialized");
        }
        const Status validation = validateFrame(desc);
        if (!validation.succeeded()) {
            return validation;
        }
        const VulkanFsr4ReferenceRecordPlan plan = makeVulkanFsr4ReferenceRecordPlan(createDesc_, desc);
        Status status = prepareInitializerBytesForPreset(plan.framePlan.preset);
        if (!status.succeeded()) {
            return status;
        }
        for (const VulkanFsr4ReferenceDispatchPlan& dispatch : plan.dispatches) {
            PipelineState* pipeline = nullptr;
            status = ensurePipeline(dispatch, plan.framePlan, pipeline);
            if (!status.succeeded()) {
                return status;
            }
        }
        return Status::ok();
    }

    Status recordFrame(const VulkanFsr4ReferenceFrameDesc& desc) override
    {
        const std::uint64_t profileFrameId = desc.frameInput.frameIndex;
        const auto fail = [&](const char* stage, Status failure) -> Status {
            apusr::profile::recordStatus(profileStream_, profileFrameId, stage, failure);
            return failure;
        };
        if (!initialized_) {
            return fail("record_frame.initialized", Status::runtimeFailure(
                "Vulkan FSR4 reference runtime is not initialized"));
        }
        const bool profileCpu = gpuTimestampActive_;
        const auto cpuFrameBegin = profileCpu ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const std::uint64_t cpuFrameIntervalNs = profileCpu && gpuTimestampLastRecordTime_ != std::chrono::steady_clock::time_point{}
            ? cpuDurationNs(gpuTimestampLastRecordTime_, cpuFrameBegin)
            : 0u;
        if (profileCpu) {
            gpuTimestampLastRecordTime_ = cpuFrameBegin;
        }

        Status status = validateFrame(desc);
        if (!status.succeeded()) {
            return fail("record_frame.validate", status);
        }
        const VulkanFsr4ReferenceRecordPlan plan = makeVulkanFsr4ReferenceRecordPlan(createDesc_, desc);
        status = prepareInitializerBytesForPreset(plan.framePlan.preset);
        if (!status.succeeded()) {
            return fail("record_frame.initializer_bytes", status);
        }
        const auto cpuPlanEnd = profileCpu ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

        // Resolve every pipeline and descriptor set before recording anything.
        // A missing shader/cache entry must not leave the game's borrowed
        // command buffer half-mutated.
        struct ReadyDispatch {
            const VulkanFsr4ReferenceDispatchPlan* plan = nullptr;
            PipelineState* pipeline = nullptr;
            vk::DescriptorSet descriptorSet = nullptr;
            bool pipelineCreated = false;
            bool descriptorSetCreated = false;
        };
        std::vector<ReadyDispatch> ready;
        ready.reserve(plan.dispatches.size());
        const ExternalDescriptorKey descriptorKey = makeExternalDescriptorKey(desc.frameInput, plan.framePlan);
        std::uint32_t pipelinesCreated = 0u;
        std::uint32_t descriptorSetsCreated = 0u;
        for (const VulkanFsr4ReferenceDispatchPlan& dispatch : plan.dispatches) {
            const std::size_t pipelineCountBefore = pipelines_.size();
            PipelineState* pipeline = nullptr;
            status = ensurePipeline(dispatch, plan.framePlan, pipeline);
            if (!status.succeeded()) {
                return fail("record_frame.ensure_pipeline", status);
            }
            const bool pipelineCreated = pipelines_.size() != pipelineCountBefore;
            const bool descriptorSetCreated = !pipeline->descriptorSets.contains(descriptorKey);
            vk::DescriptorSet set = nullptr;
            status = ensureDescriptorSet(*pipeline, dispatch, plan.framePlan, desc.frameInput, descriptorKey, set);
            if (!status.succeeded()) {
                return fail("record_frame.ensure_descriptor_set", status);
            }
            pipelinesCreated += pipelineCreated ? 1u : 0u;
            descriptorSetsCreated += descriptorSetCreated ? 1u : 0u;
            ready.push_back(ReadyDispatch{
                .plan = &dispatch,
                .pipeline = pipeline,
                .descriptorSet = set,
                .pipelineCreated = pipelineCreated,
                .descriptorSetCreated = descriptorSetCreated,
            });
        }
        const auto cpuBindingEnd = profileCpu ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

        const vk::CommandBuffer commandBuffer = reinterpret_cast<vk::CommandBuffer>(desc.commandBuffer.value);
        std::uint32_t gpuTimestampSlot = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t gpuTimestampCursor = 0u;
        (void)beginGpuTimestampFrame(
            commandBuffer,
            desc,
            plan,
            cpuFrameIntervalNs,
            profileCpu ? cpuDurationNs(cpuFrameBegin, cpuPlanEnd) : 0u,
            profileCpu ? cpuDurationNs(cpuPlanEnd, cpuBindingEnd) : 0u,
            pipelinesCreated,
            descriptorSetsCreated,
            gpuTimestampSlot,
            gpuTimestampCursor);

        const bool profileSampleCpu = apusr::profile::cpuEnabled() && profileStream_ &&
            gpuTimestampSlot < gpuTimestampFrames_.size();
        const std::uint64_t profileSampleSequence = profileSampleCpu
            ? gpuTimestampFrames_[gpuTimestampSlot].sampleSequence : 0u;

        {
            apusr::profile::CpuScope cpuScope(
                profileSampleCpu ? profileStream_ : apusr::profile::StreamHandle{},
                profileFrameId, profileSampleSequence, "vulkan.record.constant_updates");
            status = recordConstantUpdates(commandBuffer, plan.framePlan);
        }
        if (!status.succeeded()) {
            cancelGpuTimestampFrame(gpuTimestampSlot);
            return fail("record_frame.constant_updates", status);
        }
        markGpuTimestampConstantsEnd(commandBuffer, gpuTimestampSlot, gpuTimestampCursor);

        {
            apusr::profile::CpuScope cpuScope(
                profileSampleCpu ? profileStream_ : apusr::profile::StreamHandle{},
                profileFrameId, profileSampleSequence, "vulkan.record.initializers");
            status = recordOneTimeInitializers(commandBuffer);
        }
        if (!status.succeeded()) {
            cancelGpuTimestampFrame(gpuTimestampSlot);
            return fail("record_frame.initializers", status);
        }
        markGpuTimestampInitializersEnd(commandBuffer, gpuTimestampSlot, gpuTimestampCursor);

        ExternalImageStates externalImageStates = makeExternalImageStates(desc.frameInput);
        std::array<vk::AccessFlags, kResourceSlotCount> frameBufferAccess{};
        for (const NativeResource& resource : resources_) {
            if (resource.id != ResourceId::Null && resource.kind == ResourceKind::Buffer) {
                frameBufferAccess[resourceIndex(resource.id)] = resource.bufferAccess;
            }
        }
        for (const ReadyDispatch& dispatch : ready) {
            apusr::profile::CpuScope dispatchCpuScope(
                profileSampleCpu ? profileStream_ : apusr::profile::StreamHandle{},
                profileFrameId, profileSampleSequence,
                dispatch.plan->dispatchName != nullptr ? dispatch.plan->dispatchName : "fsr4.dispatch");
            beginGpuTimestampDispatch(
                commandBuffer,
                gpuTimestampSlot,
                gpuTimestampCursor,
                *dispatch.plan,
                dispatch.pipeline->shaderByteSize,
                dispatch.pipelineCreated,
                dispatch.descriptorSetCreated);
            DispatchPreparationStats preparationStats{};
            status = prepareDispatchResources(
                commandBuffer,
                *dispatch.plan,
                plan.framePlan,
                desc.frameInput,
                externalImageStates,
                frameBufferAccess,
                gpuTimestampSlot < gpuTimestampFrames_.size() ? &preparationStats : nullptr);
            if (!status.succeeded()) {
                cancelGpuTimestampFrame(gpuTimestampSlot);
                return fail("record_frame.prepare_dispatch_resources", status);
            }
            recordGpuTimestampDispatchPreparationStats(
                gpuTimestampSlot,
                preparationStats.barrierCalls,
                preparationStats.bufferBarriers,
                preparationStats.imageBarriers,
                preparationStats.clearCalls);
            functions_.cmdBindPipeline(commandBuffer, vk::PipelineBindPointCompute, dispatch.pipeline->pipeline);
            functions_.cmdBindDescriptorSets(
                commandBuffer,
                vk::PipelineBindPointCompute,
                dispatch.pipeline->pipelineLayout,
                0,
                1,
                &dispatch.descriptorSet,
                0,
                nullptr);
            markGpuTimestampDispatchBegin(commandBuffer, gpuTimestampSlot, gpuTimestampCursor);
            functions_.cmdDispatch(
                commandBuffer,
                dispatch.plan->grid.groupsX,
                dispatch.plan->grid.groupsY,
                dispatch.plan->grid.groupsZ);
            endGpuTimestampDispatch(commandBuffer, gpuTimestampSlot, gpuTimestampCursor);
        }
        const auto cpuCommandEnd = profileCpu ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        endGpuTimestampFrame(
            commandBuffer,
            gpuTimestampSlot,
            gpuTimestampCursor,
            profileCpu ? cpuDurationNs(cpuBindingEnd, cpuCommandEnd) : 0u,
            profileCpu ? cpuDurationNs(cpuFrameBegin, cpuCommandEnd) : 0u);
        for (NativeResource& resource : resources_) {
            if (resource.id != ResourceId::Null && resource.kind == ResourceKind::Buffer) {
                resource.bufferAccess = frameBufferAccess[resourceIndex(resource.id)];
            }
        }
        hasRecordedWork_ = true;
        return Status::ok();
    }

private:
    struct VulkanGpuTimestampQueryResult {
        std::uint64_t value = 0u;
        std::uint64_t availability = 0u;
    };

    static std::uint64_t gpuTimestampDelta(
        const std::uint64_t begin,
        const std::uint64_t end,
        const std::uint32_t validBits) noexcept
    {
        if (validBits == 0u) {
            return 0u;
        }
        if (validBits >= 64u) {
            return end - begin;
        }
        const std::uint64_t mask = (std::uint64_t{1} << validBits) - 1u;
        return (end - begin) & mask;
    }

    static std::uint64_t cpuDurationNs(
        const std::chrono::steady_clock::time_point begin,
        const std::chrono::steady_clock::time_point end) noexcept
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    }

    static double ticksToUs(const std::uint64_t ticks, const double timestampPeriodNs) noexcept
    {
        return timestampPeriodNs > 0.0 ? (static_cast<double>(ticks) * timestampPeriodNs / 1000.0) : 0.0;
    }

    void destroyGpuTimestampProfiler() noexcept
    {
        if (device_ != nullptr && functions_.destroyQueryPool != nullptr) {
            for (vk::QueryPool& pool : gpuTimestampPools_) {
                if (pool != nullptr) {
                    functions_.destroyQueryPool(device_, pool, nullptr);
                    pool = nullptr;
                }
            }
        }
        gpuTimestampFrames_ = {};
        gpuTimestampValidBitsByFamily_.clear();
        gpuTimestampActive_ = false;
        gpuTimestampHasSampleDeadline_ = false;
        gpuTimestampNextSampleTime_ = {};
        gpuTimestampBurstRemaining_ = 0u;
        gpuTimestampCurrentBurstSequence_ = 0u;
        gpuTimestampCurrentBurstConfigurationChanged_ = false;
        gpuTimestampSampleSignatureValid_ = false;
        gpuTimestampFile_.close();
    }

    void initializeGpuTimestampProfiler() noexcept
    {
        destroyGpuTimestampProfiler();
        if (!gpuTimestampRequested_ || device_ == nullptr || functions_.createQueryPool == nullptr ||
            functions_.getQueryPoolResults == nullptr || functions_.cmdResetQueryPool == nullptr ||
            functions_.cmdWriteTimestamp == nullptr) {
            return;
        }

        bool writeHeader = false;
        if (vulkanLegacyGpuTimestampsEnabled()) {
            const std::filesystem::path path = vulkanGpuTimestampPath();
            std::error_code ec;
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path(), ec);
            }
            {
                std::lock_guard lock(gVulkanGpuTimestampFileMutex);
                const std::string pathKey = path.lexically_normal().string();
                const auto [it, inserted] = gVulkanGpuTimestampInitializedPaths.insert(pathKey);
                writeHeader = inserted;
                gpuTimestampFile_.open(
                    path,
                    std::ios::out | (inserted ? std::ios::trunc : std::ios::app));
                if (!gpuTimestampFile_ && inserted) {
                    gVulkanGpuTimestampInitializedPaths.erase(it);
                }
            }
        }
        if (writeHeader && gpuTimestampFile_) {
            gpuTimestampFile_
                << "sequence,runtime_sequence,api_version,driver_version,vendor_id,device_id,"
                   "burst_sequence,burst_frame,configuration_changed,frame_index,"
                   "render_width,render_height,display_width,display_height,aligned_display_width,aligned_display_height,"
                   "motion_vector_width,motion_vector_height,total_workgroups,"
                   "color_native_format,color_native_layout,depth_native_format,depth_native_layout,"
                   "motion_vector_native_format,motion_vector_native_layout,exposure_native_format,exposure_native_layout,"
                   "output_native_format,output_native_layout,"
                   "pipeline_variant,preset,frame_capacity,permutation_key,"
                   "wmma,depth_inverted,low_res_motion_vectors,auto_exposure,jittered_motion_vectors,color_space,debug_visualize,"
                   "reset,rcas_enabled,default_exposure,sharpness,pre_exposure,timestamp_period_ns,queue_family,timestamp_valid_bits,"
                   "kind,dispatch_index,name,groups_x,groups_y,groups_z,shader_bytes,pipeline_created,descriptor_set_created,"
                   "barrier_calls,buffer_barriers,image_barriers,clear_calls,ticks,setup_ticks,dispatch_ticks,duration_us,setup_us,dispatch_us,"
                   "frame_ticks,frame_us,pct_frame,setup_pct_frame,dispatch_pct_frame,"
                   "constants_ticks,constants_us,initializers_ticks,initializers_us,prelude_ticks,prelude_us,"
                   "total_setup_ticks,total_setup_us,total_dispatch_ticks,total_dispatch_us,unattributed_ticks,unattributed_us,"
                   "constant_update_bytes,initializer_update_bytes,initializer_uploaded,default_exposure_initialized,"
                   "cpu_frame_interval_us,cpu_plan_us,cpu_binding_us,cpu_command_record_us,cpu_total_us,pipelines_created,descriptor_sets_created\n";
        }

        for (vk::QueryPool& pool : gpuTimestampPools_) {
            const vk::QueryPoolCreateInfo info{
                .sType = vk::StructureTypeQueryPoolCreateInfo,
                .pNext = nullptr,
                .flags = 0u,
                .queryType = vk::QueryTypeTimestamp,
                .queryCount = kVulkanGpuTimestampQueriesPerSlot,
                .pipelineStatistics = 0u,
            };
            if (functions_.createQueryPool(device_, &info, nullptr, &pool) != vk::Success || pool == nullptr) {
                destroyGpuTimestampProfiler();
                return;
            }
        }

        if (functions_.getPhysicalDeviceQueueFamilyProperties != nullptr) {
            std::uint32_t count = 0u;
            functions_.getPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, nullptr);
            if (count != 0u) {
                std::vector<vk::QueueFamilyProperties> properties(count);
                functions_.getPhysicalDeviceQueueFamilyProperties(physicalDevice_, &count, properties.data());
                gpuTimestampValidBitsByFamily_.resize(count, 64u);
                for (std::uint32_t index = 0u; index < count; ++index) {
                    gpuTimestampValidBitsByFamily_[index] = properties[index].timestampValidBits;
                }
            }
        }

        gpuTimestampActive_ = true;
        if (gpuTimestampFile_) {
            gpuTimestampFile_.flush();
        }
    }

    std::uint32_t gpuTimestampValidBitsForFamily(const std::uint32_t family) const noexcept
    {
        if (family < gpuTimestampValidBitsByFamily_.size()) {
            return gpuTimestampValidBitsByFamily_[family];
        }
        return 64u;
    }

    bool resolveGpuTimestampSlot(const std::uint32_t slot) noexcept
    {
        if (!gpuTimestampActive_ || slot >= gpuTimestampFrames_.size() || gpuTimestampPools_[slot] == nullptr) {
            return false;
        }
        VulkanGpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        if (!metadata.valid) {
            return true;
        }
        if (metadata.queryCount == 0u || metadata.queryCount > kVulkanGpuTimestampQueriesPerSlot) {
            metadata.valid = false;
            return true;
        }

        std::array<VulkanGpuTimestampQueryResult, kVulkanGpuTimestampQueriesPerSlot> results{};
        const vk::Result result = functions_.getQueryPoolResults(
            device_,
            gpuTimestampPools_[slot],
            0u,
            metadata.queryCount,
            metadata.queryCount * sizeof(VulkanGpuTimestampQueryResult),
            results.data(),
            sizeof(VulkanGpuTimestampQueryResult),
            vk::QueryResult64Bit | vk::QueryResultWithAvailabilityBit);
        if (result != vk::Success && result != vk::NotReady) {
            metadata.valid = false;
            return true;
        }
        for (std::uint32_t index = 0u; index < metadata.queryCount; ++index) {
            if (results[index].availability == 0u) {
                return false;
            }
        }

        const std::uint64_t frameTicks = gpuTimestampDelta(
            results[metadata.frameBeginQuery].value,
            results[metadata.frameEndQuery].value,
            metadata.timestampValidBits);
        const std::uint64_t constantsTicks = gpuTimestampDelta(
            results[metadata.frameBeginQuery].value,
            results[metadata.constantsEndQuery].value,
            metadata.timestampValidBits);
        const std::uint64_t initializersTicks = gpuTimestampDelta(
            results[metadata.constantsEndQuery].value,
            results[metadata.initializersEndQuery].value,
            metadata.timestampValidBits);
        const std::uint64_t preludeTicks = !metadata.dispatches.empty()
            ? gpuTimestampDelta(
                results[metadata.initializersEndQuery].value,
                results[metadata.dispatches.front().setupBeginQuery].value,
                metadata.timestampValidBits)
            : gpuTimestampDelta(
                results[metadata.initializersEndQuery].value,
                results[metadata.frameEndQuery].value,
                metadata.timestampValidBits);

        std::uint64_t totalSetupTicks = 0u;
        std::uint64_t totalDispatchTicks = 0u;
        for (const VulkanGpuTimestampDispatchMetadata& dispatch : metadata.dispatches) {
            totalSetupTicks += gpuTimestampDelta(
                results[dispatch.setupBeginQuery].value,
                results[dispatch.dispatchBeginQuery].value,
                metadata.timestampValidBits);
            totalDispatchTicks += gpuTimestampDelta(
                results[dispatch.dispatchBeginQuery].value,
                results[dispatch.endQuery].value,
                metadata.timestampValidBits);
        }
        const std::uint64_t attributedTicks = constantsTicks + initializersTicks + preludeTicks + totalSetupTicks + totalDispatchTicks;
        const std::uint64_t unattributedTicks = frameTicks > attributedTicks ? frameTicks - attributedTicks : 0u;
        const auto ticksToNs = [](const std::uint64_t ticks, const double periodNs) noexcept -> std::uint64_t {
            if (periodNs <= 0.0) {
                return 0u;
            }
            return static_cast<std::uint64_t>(static_cast<long double>(ticks) * static_cast<long double>(periodNs));
        };
        const std::uint64_t gpuFrequency = metadata.timestampPeriodNs > 0.0
            ? static_cast<std::uint64_t>(1000000000.0L / static_cast<long double>(metadata.timestampPeriodNs))
            : 0u;
        if (metadata.unifiedProfile && gpuClockCalibrationAvailable_ && gpuFrequency != 0u) {
            const std::array<vk::CalibratedTimestampInfo, 2> timestampInfos{{
                {.sType = vk::StructureTypeCalibratedTimestampInfo, .pNext = nullptr, .timeDomain = vk::TimeDomainDevice},
                {.sType = vk::StructureTypeCalibratedTimestampInfo, .pNext = nullptr, .timeDomain = gpuClockHostTimeDomain_},
            }};
            std::array<std::uint64_t, 2> timestamps{};
            std::uint64_t maxDeviationNs = 0u;
            if (functions_.getCalibratedTimestamps(
                    device_, static_cast<std::uint32_t>(timestampInfos.size()), timestampInfos.data(),
                    timestamps.data(), &maxDeviationNs) == vk::Success) {
                apusr::profile::recordCalibration(apusr::profile::ClockCalibration{
                    .stream = profileStream_,
                    .frameId = metadata.frameIndex,
                    .sampleSequence = metadata.sampleSequence,
                    .cpuTick = timestamps[1],
                    .gpuTick = timestamps[0],
                    .gpuFrequency = gpuFrequency,
                    .maxDeviationNs = maxDeviationNs,
                    .gpuValidBits = metadata.timestampValidBits,
                    .flags = 1u,
                });
            }
        }
        if (metadata.unifiedProfile) {
            apusr::profile::recordFrameSample(apusr::profile::FrameSample{
                .stream = profileStream_,
                .frameId = metadata.frameIndex,
                .sampleSequence = metadata.sampleSequence,
                .burstSequence = metadata.burstSequence,
                .configurationHash = metadata.configurationHash,
                .burstFrame = metadata.burstFrame,
                .renderWidth = metadata.renderWidth,
                .renderHeight = metadata.renderHeight,
                .displayWidth = metadata.displayWidth,
                .displayHeight = metadata.displayHeight,
                .gpuDurationNs = ticksToNs(frameTicks, metadata.timestampPeriodNs),
                .cpuTotalNs = metadata.cpuTotalNs,
                .cpuRecordNs = metadata.cpuCommandRecordNs,
                .totalWorkgroups = metadata.totalWorkgroups,
                .gpuBeginTick = results[metadata.frameBeginQuery].value,
                .gpuEndTick = results[metadata.frameEndQuery].value,
                .gpuFrequency = gpuFrequency,
                .flags = metadata.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u,
            });
            apusr::profile::recordCounter(apusr::profile::Counter{
                .stream = profileStream_, .frameId = metadata.frameIndex, .name = "cpu.plan_binding_ns",
                .value = metadata.cpuPlanNs, .value2 = metadata.cpuBindingNs});
            apusr::profile::recordCounter(apusr::profile::Counter{
                .stream = profileStream_, .frameId = metadata.frameIndex, .name = "gpu.unattributed_ticks",
                .value = unattributedTicks, .value2 = frameTicks});
        }

        const bool legacyProfile = vulkanLegacyGpuTimestampsEnabled();
        const auto writeCommon = [&]() {
            gpuTimestampFile_ << metadata.sequence << ',' << metadata.runtimeSequence << ','
                              << metadata.apiVersion << ',' << metadata.driverVersion << ','
                              << metadata.vendorId << ',' << metadata.deviceId << ','
                              << metadata.burstSequence << ',' << metadata.burstFrame << ','
                              << (metadata.configurationChanged ? 1 : 0) << ',' << metadata.frameIndex << ','
                              << metadata.renderWidth << ',' << metadata.renderHeight << ','
                              << metadata.displayWidth << ',' << metadata.displayHeight << ','
                              << metadata.alignedDisplayWidth << ',' << metadata.alignedDisplayHeight << ','
                              << metadata.motionVectorWidth << ',' << metadata.motionVectorHeight << ','
                              << metadata.totalWorkgroups << ','
                              << metadata.colorNativeFormat << ',' << metadata.colorNativeLayout << ','
                              << metadata.depthNativeFormat << ',' << metadata.depthNativeLayout << ','
                              << metadata.motionVectorNativeFormat << ',' << metadata.motionVectorNativeLayout << ','
                              << metadata.exposureNativeFormat << ',' << metadata.exposureNativeLayout << ','
                              << metadata.outputNativeFormat << ',' << metadata.outputNativeLayout << ','
                              << pipelineVariantName(metadata.pipelineVariant) << ',' << presetName(metadata.preset) << ','
                              << static_cast<std::uint32_t>(metadata.frameCapacity) << ',' << metadata.permutationKey << ','
                              << (metadata.permutation.wmma ? 1 : 0) << ','
                              << (metadata.permutation.depthInverted ? 1 : 0) << ','
                              << (metadata.permutation.lowResMotionVectors ? 1 : 0) << ','
                              << (metadata.permutation.autoExposure ? 1 : 0) << ','
                              << (metadata.permutation.jitteredMotionVectors ? 1 : 0) << ','
                              << static_cast<std::uint32_t>(metadata.permutation.colorSpace) << ','
                              << (metadata.permutation.debugVisualize ? 1 : 0) << ','
                              << (metadata.reset ? 1 : 0) << ',' << (metadata.rcasEnabled ? 1 : 0) << ','
                              << (metadata.defaultExposure ? 1 : 0) << ',' << metadata.sharpness << ','
                              << metadata.preExposure << ',' << metadata.timestampPeriodNs << ','
                              << metadata.queueFamily << ',' << metadata.timestampValidBits << ',';
        };

        if (legacyProfile) {
            writeCommon();
            gpuTimestampFile_ << "frame,-1,__frame__,0,0,0,0,0,0,0,0,0,0,"
                              << frameTicks << ",0,0," << ticksToUs(frameTicks, metadata.timestampPeriodNs) << ",0,0,"
                              << frameTicks << ',' << ticksToUs(frameTicks, metadata.timestampPeriodNs) << ",100,0,0,"
                              << constantsTicks << ',' << ticksToUs(constantsTicks, metadata.timestampPeriodNs) << ','
                              << initializersTicks << ',' << ticksToUs(initializersTicks, metadata.timestampPeriodNs) << ','
                              << preludeTicks << ',' << ticksToUs(preludeTicks, metadata.timestampPeriodNs) << ','
                              << totalSetupTicks << ',' << ticksToUs(totalSetupTicks, metadata.timestampPeriodNs) << ','
                              << totalDispatchTicks << ',' << ticksToUs(totalDispatchTicks, metadata.timestampPeriodNs) << ','
                              << unattributedTicks << ',' << ticksToUs(unattributedTicks, metadata.timestampPeriodNs) << ','
                              << metadata.constantUpdateBytes << ',' << metadata.initializerUpdateBytes << ','
                              << (metadata.initializerUploaded ? 1 : 0) << ',' << (metadata.defaultExposureInitialized ? 1 : 0) << ','
                              << (static_cast<double>(metadata.cpuFrameIntervalNs) / 1000.0) << ','
                              << (static_cast<double>(metadata.cpuPlanNs) / 1000.0) << ','
                              << (static_cast<double>(metadata.cpuBindingNs) / 1000.0) << ','
                              << (static_cast<double>(metadata.cpuCommandRecordNs) / 1000.0) << ','
                              << (static_cast<double>(metadata.cpuTotalNs) / 1000.0) << ','
                              << metadata.pipelinesCreated << ',' << metadata.descriptorSetsCreated << '\n';
        }

        for (std::size_t index = 0; index < metadata.dispatches.size(); ++index) {
            const VulkanGpuTimestampDispatchMetadata& dispatch = metadata.dispatches[index];
            const std::uint64_t setupTicks = gpuTimestampDelta(
                results[dispatch.setupBeginQuery].value,
                results[dispatch.dispatchBeginQuery].value,
                metadata.timestampValidBits);
            const std::uint64_t dispatchTicks = gpuTimestampDelta(
                results[dispatch.dispatchBeginQuery].value,
                results[dispatch.endQuery].value,
                metadata.timestampValidBits);
            const std::uint64_t ticks = gpuTimestampDelta(
                results[dispatch.setupBeginQuery].value,
                results[dispatch.endQuery].value,
                metadata.timestampValidBits);
            if (legacyProfile) {
                const double percentage = frameTicks != 0u
                    ? (100.0 * static_cast<double>(ticks) / static_cast<double>(frameTicks))
                    : 0.0;
                const double setupPercentage = frameTicks != 0u
                    ? (100.0 * static_cast<double>(setupTicks) / static_cast<double>(frameTicks))
                    : 0.0;
                const double dispatchPercentage = frameTicks != 0u
                    ? (100.0 * static_cast<double>(dispatchTicks) / static_cast<double>(frameTicks))
                    : 0.0;
                writeCommon();
                gpuTimestampFile_ << "dispatch," << index << ',' << dispatch.name << ','
                                  << dispatch.groupsX << ',' << dispatch.groupsY << ',' << dispatch.groupsZ << ','
                                  << dispatch.shaderBytes << ',' << (dispatch.pipelineCreated ? 1 : 0) << ','
                                  << (dispatch.descriptorSetCreated ? 1 : 0) << ','
                                  << dispatch.barrierCalls << ',' << dispatch.bufferBarriers << ','
                                  << dispatch.imageBarriers << ',' << dispatch.clearCalls << ','
                                  << ticks << ',' << setupTicks << ',' << dispatchTicks << ','
                                  << ticksToUs(ticks, metadata.timestampPeriodNs) << ','
                                  << ticksToUs(setupTicks, metadata.timestampPeriodNs) << ','
                                  << ticksToUs(dispatchTicks, metadata.timestampPeriodNs) << ','
                                  << frameTicks << ',' << ticksToUs(frameTicks, metadata.timestampPeriodNs) << ','
                                  << percentage << ',' << setupPercentage << ',' << dispatchPercentage << ','
                                  << constantsTicks << ',' << ticksToUs(constantsTicks, metadata.timestampPeriodNs) << ','
                                  << initializersTicks << ',' << ticksToUs(initializersTicks, metadata.timestampPeriodNs) << ','
                                  << preludeTicks << ',' << ticksToUs(preludeTicks, metadata.timestampPeriodNs) << ','
                                  << totalSetupTicks << ',' << ticksToUs(totalSetupTicks, metadata.timestampPeriodNs) << ','
                                  << totalDispatchTicks << ',' << ticksToUs(totalDispatchTicks, metadata.timestampPeriodNs) << ','
                                  << unattributedTicks << ',' << ticksToUs(unattributedTicks, metadata.timestampPeriodNs) << ','
                                  << metadata.constantUpdateBytes << ',' << metadata.initializerUpdateBytes << ','
                                  << (metadata.initializerUploaded ? 1 : 0) << ',' << (metadata.defaultExposureInitialized ? 1 : 0) << ','
                                  << (static_cast<double>(metadata.cpuFrameIntervalNs) / 1000.0) << ','
                                  << (static_cast<double>(metadata.cpuPlanNs) / 1000.0) << ','
                                  << (static_cast<double>(metadata.cpuBindingNs) / 1000.0) << ','
                                  << (static_cast<double>(metadata.cpuCommandRecordNs) / 1000.0) << ','
                                  << (static_cast<double>(metadata.cpuTotalNs) / 1000.0) << ','
                                  << metadata.pipelinesCreated << ',' << metadata.descriptorSetsCreated << '\n';
            }
            if (metadata.unifiedProfile) {
                apusr::profile::recordGpuInterval(apusr::profile::GpuInterval{
                    .stream = profileStream_,
                    .frameId = metadata.frameIndex,
                    .sampleSequence = metadata.sampleSequence,
                    .name = dispatch.name.c_str(),
                    .durationNs = ticksToNs(ticks, metadata.timestampPeriodNs),
                    .setupNs = ticksToNs(setupTicks, metadata.timestampPeriodNs),
                    .executionNs = ticksToNs(dispatchTicks, metadata.timestampPeriodNs),
                    .groupsX = dispatch.groupsX,
                    .groupsY = dispatch.groupsY,
                    .groupsZ = dispatch.groupsZ,
                    .barrierCount = dispatch.barrierCalls,
                    .descriptorCount = dispatch.descriptorSetCreated ? 1u : 0u,
                    .gpuBeginTick = results[dispatch.setupBeginQuery].value,
                    .gpuExecutionBeginTick = results[dispatch.dispatchBeginQuery].value,
                    .gpuEndTick = results[dispatch.endQuery].value,
                    .gpuFrequency = gpuFrequency,
                    .flags = (dispatch.pipelineCreated ? 1u : 0u) | (dispatch.descriptorSetCreated ? 2u : 0u),
                });
            }
        }
        if (gpuTimestampFile_) {
            gpuTimestampFile_.flush();
        }
        metadata.valid = false;
        metadata.dispatches.clear();
        return true;
    }

    void flushGpuTimestampProfiler() noexcept
    {
        if (!gpuTimestampActive_) {
            return;
        }
        for (std::uint32_t slot = 0u; slot < gpuTimestampFrames_.size(); ++slot) {
            (void)resolveGpuTimestampSlot(slot);
        }
    }

    bool beginGpuTimestampFrame(
        const vk::CommandBuffer commandBuffer,
        const VulkanFsr4ReferenceFrameDesc& desc,
        const VulkanFsr4ReferenceRecordPlan& plan,
        const std::uint64_t cpuFrameIntervalNs,
        const std::uint64_t cpuPlanNs,
        const std::uint64_t cpuBindingNs,
        const std::uint32_t pipelinesCreated,
        const std::uint32_t descriptorSetsCreated,
        std::uint32_t& outSlot,
        std::uint32_t& outCursor) noexcept
    {
        outSlot = std::numeric_limits<std::uint32_t>::max();
        outCursor = 0u;
        if (!gpuTimestampActive_ || commandBuffer == nullptr) {
            return false;
        }
        if (4u + 3u * plan.dispatches.size() > kVulkanGpuTimestampQueriesPerSlot) {
            return false;
        }

        const VulkanGpuTimestampSampleSignature signature{
            .pipelineVariant = plan.framePlan.pipelineVariant,
            .preset = plan.framePlan.preset,
            .frameCapacity = plan.framePlan.frameCapacity,
            .permutationKey = plan.framePlan.permutationKey,
            .renderWidth = desc.runner.frame.renderSize.width,
            .renderHeight = desc.runner.frame.renderSize.height,
            .displayWidth = desc.runner.frame.displaySize.width,
            .displayHeight = desc.runner.frame.displaySize.height,
            .rcasEnabled = plan.framePlan.constants.rcasEnabled != 0u,
        };
        std::uint64_t configurationHash = 0x4150555352465352ull;
        configurationHash = apusr::profile::hashCombine(configurationHash, static_cast<std::uint64_t>(signature.pipelineVariant));
        configurationHash = apusr::profile::hashCombine(configurationHash, static_cast<std::uint64_t>(signature.preset));
        configurationHash = apusr::profile::hashCombine(configurationHash, static_cast<std::uint64_t>(signature.frameCapacity));
        configurationHash = apusr::profile::hashCombine(configurationHash, signature.permutationKey);
        configurationHash = apusr::profile::hashCombine(configurationHash,
            (static_cast<std::uint64_t>(signature.renderWidth) << 32u) | signature.renderHeight);
        configurationHash = apusr::profile::hashCombine(configurationHash,
            (static_cast<std::uint64_t>(signature.displayWidth) << 32u) | signature.displayHeight);
        configurationHash = apusr::profile::hashCombine(configurationHash, signature.rcasEnabled ? 1u : 0u);
        if (profileStream_ && configurationHash != profileLastConfigurationHash_) {
            apusr::profile::recordStateTransition({
                .stream = profileStream_,
                .frameId = desc.frameInput.frameIndex,
                .name = "upscaler.configuration_hash",
                .reason = profileLastConfigurationHash_ == 0u ? "initial" : "configuration_change",
                .previousValue = profileLastConfigurationHash_,
                .nextValue = configurationHash,
            });
            profileLastConfigurationHash_ = configurationHash;
        }

        apusr::profile::SampleDecision profileDecision{};
        std::uint64_t burstSequence = 0u;
        std::uint32_t burstFrame = 0u;
        bool configurationChanged = false;
        const bool unifiedProfile = apusr::profile::gpuEnabled() && profileStream_;
        if (unifiedProfile) {
            profileDecision = profileSampleGate_.decide(configurationHash, desc.frameInput.frameIndex);
            if (!profileDecision.capture) {
                return false;
            }
            burstSequence = profileDecision.burstSequence;
            burstFrame = profileDecision.burstFrame;
            configurationChanged = profileDecision.configurationChanged;
        } else {
            configurationChanged = !gpuTimestampSampleSignatureValid_ || !(signature == gpuTimestampSampleSignature_);
            const auto now = std::chrono::steady_clock::now();
            if (configurationChanged) {
                gpuTimestampSampleSignature_ = signature;
                gpuTimestampSampleSignatureValid_ = true;
                gpuTimestampBurstRemaining_ = kVulkanGpuTimestampBurstFrames;
                gpuTimestampCurrentBurstSequence_ = gVulkanGpuTimestampBurstSequence.fetch_add(1u, std::memory_order_relaxed);
                gpuTimestampCurrentBurstConfigurationChanged_ = true;
                gpuTimestampHasSampleDeadline_ = false;
            } else if (gpuTimestampBurstRemaining_ == 0u) {
                if (gpuTimestampHasSampleDeadline_ && now < gpuTimestampNextSampleTime_) {
                    return false;
                }
                gpuTimestampBurstRemaining_ = kVulkanGpuTimestampBurstFrames;
                gpuTimestampCurrentBurstSequence_ = gVulkanGpuTimestampBurstSequence.fetch_add(1u, std::memory_order_relaxed);
                gpuTimestampCurrentBurstConfigurationChanged_ = false;
            }
            burstSequence = gpuTimestampCurrentBurstSequence_;
            burstFrame = kVulkanGpuTimestampBurstFrames - gpuTimestampBurstRemaining_;
            --gpuTimestampBurstRemaining_;
            if (gpuTimestampBurstRemaining_ == 0u) {
                gpuTimestampNextSampleTime_ = now + kVulkanGpuTimestampSampleInterval;
                gpuTimestampHasSampleDeadline_ = true;
            }
        }

        const std::uint64_t sequence = gVulkanGpuTimestampSequence.fetch_add(1u, std::memory_order_relaxed);
        const std::uint32_t slot = static_cast<std::uint32_t>(sequence % gpuTimestampPools_.size());
        if (!resolveGpuTimestampSlot(slot)) {
            return false;
        }

        functions_.cmdResetQueryPool(commandBuffer, gpuTimestampPools_[slot], 0u, kVulkanGpuTimestampQueriesPerSlot);
        VulkanGpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        metadata = VulkanGpuTimestampFrameMetadata{};
        metadata.valid = true;
        metadata.sequence = sequence;
        metadata.sampleSequence = profileDecision.sampleSequence;
        metadata.configurationHash = configurationHash;
        metadata.unifiedProfile = unifiedProfile;
        metadata.runtimeSequence = gpuTimestampRuntimeSequence_;
        metadata.apiVersion = gpuTimestampApiVersion_;
        metadata.driverVersion = gpuTimestampDriverVersion_;
        metadata.vendorId = gpuTimestampVendorId_;
        metadata.deviceId = gpuTimestampDeviceId_;
        metadata.burstSequence = burstSequence;
        metadata.burstFrame = burstFrame;
        metadata.configurationChanged = configurationChanged;
        metadata.frameIndex = desc.frameInput.frameIndex;
        metadata.renderWidth = desc.runner.frame.renderSize.width;
        metadata.renderHeight = desc.runner.frame.renderSize.height;
        metadata.displayWidth = desc.runner.frame.displaySize.width;
        metadata.displayHeight = desc.runner.frame.displaySize.height;
        metadata.alignedDisplayWidth = plan.framePlan.alignedUpscaleSize.width;
        metadata.alignedDisplayHeight = plan.framePlan.alignedUpscaleSize.height;
        metadata.motionVectorWidth = desc.frameInput.constants.motionVectorSize.width;
        metadata.motionVectorHeight = desc.frameInput.constants.motionVectorSize.height;
        metadata.totalWorkgroups = 0u;
        for (const auto& dispatch : plan.dispatches) {
            metadata.totalWorkgroups += static_cast<std::uint64_t>(dispatch.grid.groupsX) *
                static_cast<std::uint64_t>(dispatch.grid.groupsY) *
                static_cast<std::uint64_t>(dispatch.grid.groupsZ);
        }
        const auto captureSurface = [&](const FrameInputSurfaceRole role, std::uint64_t& format, std::uint64_t& layout) {
            if (const FrameInputSurfaceDesc* surface = findFrameInputSurface(desc.frameInput, role)) {
                format = surface->nativeFormat;
                layout = surface->nativeStateOrLayout;
            }
        };
        captureSurface(FrameInputSurfaceRole::Color, metadata.colorNativeFormat, metadata.colorNativeLayout);
        captureSurface(FrameInputSurfaceRole::Depth, metadata.depthNativeFormat, metadata.depthNativeLayout);
        captureSurface(
            FrameInputSurfaceRole::MotionVectors,
            metadata.motionVectorNativeFormat,
            metadata.motionVectorNativeLayout);
        captureSurface(FrameInputSurfaceRole::Exposure, metadata.exposureNativeFormat, metadata.exposureNativeLayout);
        captureSurface(FrameInputSurfaceRole::Output, metadata.outputNativeFormat, metadata.outputNativeLayout);
        metadata.pipelineVariant = plan.framePlan.pipelineVariant;
        metadata.preset = plan.framePlan.preset;
        metadata.frameCapacity = plan.framePlan.frameCapacity;
        metadata.permutationKey = plan.framePlan.permutationKey;
        metadata.permutation = plan.framePlan.permutation;
        metadata.reset = plan.framePlan.constants.reset != 0u;
        metadata.rcasEnabled = plan.framePlan.constants.rcasEnabled != 0u;
        metadata.defaultExposure = plan.framePlan.constants.useDefaultExposure != 0u;
        metadata.sharpness = desc.frameInput.options.sharpness;
        metadata.preExposure = desc.frameInput.options.preExposure;
        metadata.timestampPeriodNs = gpuTimestampPeriodNs_;
        metadata.cpuFrameIntervalNs = cpuFrameIntervalNs;
        metadata.cpuPlanNs = cpuPlanNs;
        metadata.cpuBindingNs = cpuBindingNs;
        metadata.pipelinesCreated = pipelinesCreated;
        metadata.descriptorSetsCreated = descriptorSetsCreated;
        metadata.constantUpdateBytes = sizeof(OptimizedConstants) + sizeof(AutoExposureSpdConstants) + sizeof(RcasConstants) + sizeof(Pass0WeightsConstants);
        metadata.initializerUploaded = !initializerRecorded_;
        metadata.initializerUpdateBytes = metadata.initializerUploaded ? initializerBytes_.size() : 0u;
        metadata.defaultExposureInitialized = !defaultExposureRecorded_;
        const std::uint32_t family = desc.frameInput.command.nativeQueueFamilyIndex != kFrameInputUnknownQueueFamily
            ? desc.frameInput.command.nativeQueueFamilyIndex
            : createDesc_.graphicsQueueFamilyIndex;
        metadata.queueFamily = family;
        metadata.timestampValidBits = gpuTimestampValidBitsForFamily(family);
        if (metadata.timestampValidBits == 0u) {
            metadata.valid = false;
            ++gpuTimestampBurstRemaining_;
            return false;
        }
        metadata.dispatches.reserve(plan.dispatches.size());
        metadata.frameBeginQuery = outCursor++;
        functions_.cmdWriteTimestamp(commandBuffer, vk::PipelineStageAllCommandsBit, gpuTimestampPools_[slot], metadata.frameBeginQuery);
        outSlot = slot;
        return true;
    }

    void markGpuTimestampConstantsEnd(
        const vk::CommandBuffer commandBuffer,
        const std::uint32_t slot,
        std::uint32_t& cursor) noexcept
    {
        if (!gpuTimestampActive_ || slot >= gpuTimestampFrames_.size() || cursor >= kVulkanGpuTimestampQueriesPerSlot) {
            return;
        }
        VulkanGpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        metadata.constantsEndQuery = cursor++;
        functions_.cmdWriteTimestamp(commandBuffer, vk::PipelineStageAllCommandsBit, gpuTimestampPools_[slot], metadata.constantsEndQuery);
    }

    void markGpuTimestampInitializersEnd(
        const vk::CommandBuffer commandBuffer,
        const std::uint32_t slot,
        std::uint32_t& cursor) noexcept
    {
        if (!gpuTimestampActive_ || slot >= gpuTimestampFrames_.size() || cursor >= kVulkanGpuTimestampQueriesPerSlot) {
            return;
        }
        VulkanGpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        metadata.initializersEndQuery = cursor++;
        functions_.cmdWriteTimestamp(commandBuffer, vk::PipelineStageAllCommandsBit, gpuTimestampPools_[slot], metadata.initializersEndQuery);
    }

    void cancelGpuTimestampFrame(const std::uint32_t slot) noexcept
    {
        if (slot < gpuTimestampFrames_.size()) {
            gpuTimestampFrames_[slot].valid = false;
            gpuTimestampFrames_[slot].dispatches.clear();
        }
    }

    void beginGpuTimestampDispatch(
        const vk::CommandBuffer commandBuffer,
        const std::uint32_t slot,
        std::uint32_t& cursor,
        const VulkanFsr4ReferenceDispatchPlan& dispatch,
        const std::uint64_t shaderBytes,
        const bool pipelineCreated,
        const bool descriptorSetCreated) noexcept
    {
        if (!gpuTimestampActive_ || slot >= gpuTimestampFrames_.size() || cursor >= kVulkanGpuTimestampQueriesPerSlot) {
            return;
        }
        VulkanGpuTimestampDispatchMetadata metadata{};
        metadata.name = dispatch.dispatchName != nullptr ? dispatch.dispatchName : "unknown";
        metadata.groupsX = dispatch.grid.groupsX;
        metadata.groupsY = dispatch.grid.groupsY;
        metadata.groupsZ = dispatch.grid.groupsZ;
        metadata.shaderBytes = shaderBytes;
        metadata.pipelineCreated = pipelineCreated;
        metadata.descriptorSetCreated = descriptorSetCreated;
        metadata.setupBeginQuery = cursor++;
        functions_.cmdWriteTimestamp(commandBuffer, vk::PipelineStageAllCommandsBit, gpuTimestampPools_[slot], metadata.setupBeginQuery);
        gpuTimestampFrames_[slot].dispatches.push_back(std::move(metadata));
    }

    void recordGpuTimestampDispatchPreparationStats(
        const std::uint32_t slot,
        const std::uint32_t barrierCalls,
        const std::uint32_t bufferBarriers,
        const std::uint32_t imageBarriers,
        const std::uint32_t clearCalls) noexcept
    {
        if (!gpuTimestampActive_ || slot >= gpuTimestampFrames_.size() || gpuTimestampFrames_[slot].dispatches.empty()) {
            return;
        }
        VulkanGpuTimestampDispatchMetadata& metadata = gpuTimestampFrames_[slot].dispatches.back();
        metadata.barrierCalls = barrierCalls;
        metadata.bufferBarriers = bufferBarriers;
        metadata.imageBarriers = imageBarriers;
        metadata.clearCalls = clearCalls;
    }

    void markGpuTimestampDispatchBegin(
        const vk::CommandBuffer commandBuffer,
        const std::uint32_t slot,
        std::uint32_t& cursor) noexcept
    {
        if (!gpuTimestampActive_ || slot >= gpuTimestampFrames_.size() ||
            gpuTimestampFrames_[slot].dispatches.empty() || cursor >= kVulkanGpuTimestampQueriesPerSlot) {
            return;
        }
        VulkanGpuTimestampDispatchMetadata& metadata = gpuTimestampFrames_[slot].dispatches.back();
        metadata.dispatchBeginQuery = cursor++;
        functions_.cmdWriteTimestamp(
            commandBuffer,
            vk::PipelineStageAllCommandsBit,
            gpuTimestampPools_[slot],
            metadata.dispatchBeginQuery);
    }

    void endGpuTimestampDispatch(
        const vk::CommandBuffer commandBuffer,
        const std::uint32_t slot,
        std::uint32_t& cursor) noexcept
    {
        if (!gpuTimestampActive_ || slot >= gpuTimestampFrames_.size() ||
            gpuTimestampFrames_[slot].dispatches.empty() || cursor >= kVulkanGpuTimestampQueriesPerSlot) {
            return;
        }
        VulkanGpuTimestampDispatchMetadata& metadata = gpuTimestampFrames_[slot].dispatches.back();
        metadata.endQuery = cursor++;
        functions_.cmdWriteTimestamp(commandBuffer, vk::PipelineStageAllCommandsBit, gpuTimestampPools_[slot], metadata.endQuery);
    }

    void endGpuTimestampFrame(
        const vk::CommandBuffer commandBuffer,
        const std::uint32_t slot,
        std::uint32_t& cursor,
        const std::uint64_t cpuCommandRecordNs,
        const std::uint64_t cpuTotalNs) noexcept
    {
        if (!gpuTimestampActive_ || slot >= gpuTimestampFrames_.size() || cursor >= kVulkanGpuTimestampQueriesPerSlot) {
            return;
        }
        VulkanGpuTimestampFrameMetadata& metadata = gpuTimestampFrames_[slot];
        metadata.cpuCommandRecordNs = cpuCommandRecordNs;
        metadata.cpuTotalNs = cpuTotalNs;
        metadata.frameEndQuery = cursor++;
        metadata.queryCount = cursor;
        functions_.cmdWriteTimestamp(commandBuffer, vk::PipelineStageAllCommandsBit, gpuTimestampPools_[slot], metadata.frameEndQuery);
    }

    Status validateFrame(const VulkanFsr4ReferenceFrameDesc& desc)
    {
        if (desc.commandBuffer.value == nullptr) {
            return Status::invalidArgument("Vulkan FSR4 frame requires a borrowed VkCommandBuffer");
        }
        if (desc.frameInput.command.api != FrameInputApi::Vulkan) {
            return Status::invalidArgument("Vulkan FSR4 frame requires FrameInputApi::Vulkan");
        }
        if (desc.frameInput.command.device.value != device_) {
            return Status::invalidArgument("Vulkan FSR4 frame device does not match the initialized runtime device");
        }
        if (desc.frameInput.command.nativeQueueFamilyIndex != kFrameInputUnknownQueueFamily &&
            std::find(queueFamilyIndices_.begin(), queueFamilyIndices_.end(), desc.frameInput.command.nativeQueueFamilyIndex) == queueFamilyIndices_.end()) {
            return Status::invalidArgument("Vulkan FSR4 borrowed command buffer uses a queue family outside the Streamline graphics/compute contract");
        }
        const Status validation = validateFrameInputForFsr4(desc.frameInput);
        if (!validation.succeeded()) {
            return validation;
        }
        for (std::uint32_t index = 0; index < desc.frameInput.surfaceCount; ++index) {
            const FrameInputSurfaceDesc& surface = desc.frameInput.surfaces[index];
            if (surface.resource.value != nullptr && surface.nativeStateOrLayout == kFrameInputUnknownNativeState) {
                return Status::invalidArgument(
                    std::string("Vulkan FSR4 requires a known VkImageLayout for ") + frameInputSurfaceRoleName(surface.role));
            }
        }
        return Status::ok();
    }

    Status prepareInitializerBytesForPreset(const Preset preset)
    {
        if (initializerPresetValid_ && initializerPreset_ == preset) {
            return Status::ok();
        }
        std::vector<std::uint8_t> bytes;
        Status status = loadReferenceInitializerBytes(createDesc_.repoRoot, preset, bytes);
        if (!status.succeeded()) {
            if (createDesc_.requireInitializerBytes) {
                return status;
            }
            bytes.clear();
        }
        if (bytes.size() > resourcePlan_.initializerByteSize) {
            return Status::runtimeFailure("Vulkan FSR4 initializer exceeds the resource plan allocation");
        }
        if ((bytes.size() & 3u) != 0u) {
            return Status::runtimeFailure("Vulkan FSR4 initializer byte count must be dword aligned");
        }
        initializerBytes_ = std::move(bytes);
        initializerPreset_ = preset;
        initializerPresetValid_ = true;
        initializerRecorded_ = false;
        return Status::ok();
    }

    Status createInternalResources()
    {
        for (const ResourceDesc& resource : resourcePlan_.resources) {
            if (resource.lifetime == ResourceLifetime::External || resource.lifetime == ResourceLifetime::Upload ||
                resource.id == ResourceId::InitializerUploadBuffer) {
                continue;
            }
            if (resource.kind == ResourceKind::Texture2D) {
                const Status status = createImageResource(resource);
                if (!status.succeeded()) {
                    return status;
                }
            } else {
                const Status status = createBufferResource(resource);
                if (!status.succeeded()) {
                    return status;
                }
            }
        }
        return Status::ok();
    }

    Status createConstantResources()
    {
        const std::array<std::pair<ReferenceConstantBufferRole, std::uint64_t>, 4> constants{{
            {ReferenceConstantBufferRole::Fsr4Upscaler, sizeof(OptimizedConstants)},
            {ReferenceConstantBufferRole::SpdAutoExposure, sizeof(AutoExposureSpdConstants)},
            {ReferenceConstantBufferRole::Rcas, sizeof(RcasConstants)},
            {ReferenceConstantBufferRole::PassWeights, sizeof(Pass0WeightsConstants)},
        }};
        for (const auto& [role, byteSize] : constants) {
            ConstantResource resource{};
            resource.role = role;
            resource.byteSize = byteSize;
            const Status status = createBufferAllocation(
                byteSize,
                vk::BufferUsageUniformBufferBit | vk::BufferUsageTransferDstBit,
                resource.buffer,
                resource.memory);
            if (!status.succeeded()) {
                return status;
            }
            constantResources_[constantIndex(role)] = resource;
        }
        return Status::ok();
    }

    Status createSampler()
    {
        const vk::SamplerCreateInfo info{
            .sType = vk::StructureTypeSamplerCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .magFilter = vk::FilterLinear,
            .minFilter = vk::FilterLinear,
            .mipmapMode = vk::SamplerMipmapModeLinear,
            .addressModeU = vk::SamplerAddressModeClampToEdge,
            .addressModeV = vk::SamplerAddressModeClampToEdge,
            .addressModeW = vk::SamplerAddressModeClampToEdge,
            .mipLodBias = 0.0f,
            .anisotropyEnable = 0,
            .maxAnisotropy = 1.0f,
            .compareEnable = 0,
            .compareOp = vk::CompareOpAlways,
            .minLod = 0.0f,
            .maxLod = 0.0f,
            .borderColor = vk::BorderColorFloatTransparentBlack,
            .unnormalizedCoordinates = 0,
        };
        const vk::Result result = functions_.createSampler(device_, &info, nullptr, &sampler_);
        return result == vk::Success ? Status::ok() : Status::runtimeFailure(vkResultText("vkCreateSampler", result));
    }

    Status createDescriptorPool()
    {
        const std::array<vk::DescriptorPoolSize, 5> sizes{{
            {vk::DescriptorTypeSampler, kDescriptorSetsPerPool},
            {vk::DescriptorTypeSampledImage, kDescriptorSetsPerPool * 8u},
            {vk::DescriptorTypeStorageImage, kDescriptorSetsPerPool * 8u},
            {vk::DescriptorTypeUniformBuffer, kDescriptorSetsPerPool * 3u},
            {vk::DescriptorTypeStorageBuffer, kDescriptorSetsPerPool * 4u},
        }};
        const vk::DescriptorPoolCreateInfo info{
            .sType = vk::StructureTypeDescriptorPoolCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .maxSets = kDescriptorSetsPerPool,
            .poolSizeCount = static_cast<std::uint32_t>(sizes.size()),
            .pPoolSizes = sizes.data(),
        };
        vk::DescriptorPool pool = nullptr;
        const vk::Result result = functions_.createDescriptorPool(device_, &info, nullptr, &pool);
        if (result != vk::Success) {
            return Status::runtimeFailure(vkResultText("vkCreateDescriptorPool", result));
        }
        descriptorPools_.push_back(pool);
        return Status::ok();
    }

    Status createBufferResource(const ResourceDesc& resource)
    {
        NativeResource native{};
        native.id = resource.id;
        native.kind = resource.kind;
        native.byteSize = resource.byteSize;
        vk::BufferUsageFlags usage = vk::BufferUsageStorageBufferBit;
        if (resource.initialContents == ResourceInitialContents::InitializerBytes) {
            usage |= vk::BufferUsageTransferDstBit;
        }
        const Status status = createBufferAllocation(resource.byteSize, usage, native.buffer, native.memory);
        if (!status.succeeded()) {
            return status;
        }
        resources_[resourceIndex(resource.id)] = native;
        return Status::ok();
    }

    Status createBufferAllocation(
        const std::uint64_t byteSize,
        const vk::BufferUsageFlags usage,
        vk::Buffer& outBuffer,
        vk::DeviceMemory& outMemory)
    {
        const vk::BufferCreateInfo bufferInfo{
            .sType = vk::StructureTypeBufferCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .size = byteSize,
            .usage = usage,
            .sharingMode = queueFamilyIndices_.size() > 1 ? vk::SharingModeConcurrent : vk::SharingModeExclusive,
            .queueFamilyIndexCount = queueFamilyIndices_.size() > 1 ? static_cast<std::uint32_t>(queueFamilyIndices_.size()) : 0u,
            .pQueueFamilyIndices = queueFamilyIndices_.size() > 1 ? queueFamilyIndices_.data() : nullptr,
        };
        outBuffer = nullptr;
        outMemory = nullptr;
        vk::Result result = functions_.createBuffer(device_, &bufferInfo, nullptr, &outBuffer);
        if (result != vk::Success) {
            return Status::runtimeFailure(vkResultText("vkCreateBuffer", result));
        }
        vk::MemoryRequirements requirements{};
        functions_.getBufferMemoryRequirements(device_, outBuffer, &requirements);
        std::uint32_t memoryTypeIndex = 0;
        if (!findDeviceLocalMemoryType(requirements.memoryTypeBits, memoryTypeIndex)) {
            functions_.destroyBuffer(device_, outBuffer, nullptr);
            outBuffer = nullptr;
            return Status::missingBackend("Vulkan device has no device-local memory type compatible with an APUSR buffer");
        }
        const vk::MemoryAllocateInfo alloc{
            .sType = vk::StructureTypeMemoryAllocateInfo,
            .pNext = nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memoryTypeIndex,
        };
        result = functions_.allocateMemory(device_, &alloc, nullptr, &outMemory);
        if (result != vk::Success) {
            functions_.destroyBuffer(device_, outBuffer, nullptr);
            outBuffer = nullptr;
            return Status::runtimeFailure(vkResultText("vkAllocateMemory(buffer)", result));
        }
        result = functions_.bindBufferMemory(device_, outBuffer, outMemory, 0);
        if (result != vk::Success) {
            functions_.freeMemory(device_, outMemory, nullptr);
            functions_.destroyBuffer(device_, outBuffer, nullptr);
            outMemory = nullptr;
            outBuffer = nullptr;
            return Status::runtimeFailure(vkResultText("vkBindBufferMemory", result));
        }
        return Status::ok();
    }

    Status createImageResource(const ResourceDesc& resource)
    {
        const vk::Format format = nativeFormat(resource.format);
        if (format == 0) {
            return Status::unsupportedBackend(std::string("Vulkan FSR4 backend has no native format mapping for ") + resourceIdName(resource.id));
        }
        vk::ImageUsageFlags usage = 0;
        vk::FormatFeatureFlags requiredFeatures = 0;
        if (resource.srv) {
            usage |= vk::ImageUsageSampledBit;
            requiredFeatures |= vk::FormatFeatureSampledImageBit;
        }
        if (resource.uav) {
            usage |= vk::ImageUsageStorageBit;
            requiredFeatures |= vk::FormatFeatureStorageImageBit;
        }
        if (resource.initialContents == ResourceInitialContents::DefaultExposureOneOne || resource.id == ResourceId::SpdAtomicCount) {
            usage |= vk::ImageUsageTransferDstBit;
            requiredFeatures |= vk::FormatFeatureTransferDstBit;
        }
        vk::FormatProperties properties{};
        functions_.getPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
        if ((properties.optimalTilingFeatures & requiredFeatures) != requiredFeatures) {
            return Status::unsupportedBackend(std::string("Vulkan device does not support required image features for ") + resourceIdName(resource.id));
        }

        const vk::ImageCreateInfo imageInfo{
            .sType = vk::StructureTypeImageCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .imageType = vk::ImageType2D,
            .format = format,
            .extent = vk::Extent3D{resource.width, resource.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCount1Bit,
            .tiling = vk::ImageTilingOptimal,
            .usage = usage,
            .sharingMode = queueFamilyIndices_.size() > 1 ? vk::SharingModeConcurrent : vk::SharingModeExclusive,
            .queueFamilyIndexCount = queueFamilyIndices_.size() > 1 ? static_cast<std::uint32_t>(queueFamilyIndices_.size()) : 0u,
            .pQueueFamilyIndices = queueFamilyIndices_.size() > 1 ? queueFamilyIndices_.data() : nullptr,
            .initialLayout = vk::ImageLayoutUndefined,
        };
        NativeResource native{};
        native.id = resource.id;
        native.kind = resource.kind;
        const auto discardNative = [&]() noexcept {
            if (native.view != nullptr) {
                functions_.destroyImageView(device_, native.view, nullptr);
                native.view = nullptr;
            }
            if (native.image != nullptr) {
                functions_.destroyImage(device_, native.image, nullptr);
                native.image = nullptr;
            }
            if (native.memory != nullptr) {
                functions_.freeMemory(device_, native.memory, nullptr);
                native.memory = nullptr;
            }
        };
        vk::Result result = functions_.createImage(device_, &imageInfo, nullptr, &native.image);
        if (result != vk::Success) {
            return Status::runtimeFailure(vkResultText("vkCreateImage", result));
        }
        vk::MemoryRequirements requirements{};
        functions_.getImageMemoryRequirements(device_, native.image, &requirements);
        std::uint32_t memoryTypeIndex = 0;
        if (!findDeviceLocalMemoryType(requirements.memoryTypeBits, memoryTypeIndex)) {
            discardNative();
            return Status::missingBackend("Vulkan device has no device-local memory type compatible with an APUSR image");
        }
        const vk::MemoryAllocateInfo alloc{
            .sType = vk::StructureTypeMemoryAllocateInfo,
            .pNext = nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memoryTypeIndex,
        };
        result = functions_.allocateMemory(device_, &alloc, nullptr, &native.memory);
        if (result != vk::Success) {
            discardNative();
            return Status::runtimeFailure(vkResultText("vkAllocateMemory(image)", result));
        }
        result = functions_.bindImageMemory(device_, native.image, native.memory, 0);
        if (result != vk::Success) {
            discardNative();
            return Status::runtimeFailure(vkResultText("vkBindImageMemory", result));
        }
        const vk::ImageViewCreateInfo viewInfo{
            .sType = vk::StructureTypeImageViewCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .image = native.image,
            .viewType = vk::ImageViewType2D,
            .format = format,
            .components = vk::ComponentMapping{
                vk::ComponentSwizzleIdentity,
                vk::ComponentSwizzleIdentity,
                vk::ComponentSwizzleIdentity,
                vk::ComponentSwizzleIdentity},
            .subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectColorBit, 0, 1, 0, 1},
        };
        result = functions_.createImageView(device_, &viewInfo, nullptr, &native.view);
        if (result != vk::Success) {
            discardNative();
            return Status::runtimeFailure(vkResultText("vkCreateImageView", result));
        }
        resources_[resourceIndex(resource.id)] = native;
        return Status::ok();
    }

    bool findDeviceLocalMemoryType(const std::uint32_t bits, std::uint32_t& outIndex) const noexcept
    {
        for (std::uint32_t index = 0; index < memoryProperties_.memoryTypeCount; ++index) {
            if ((bits & (1u << index)) != 0u &&
                (memoryProperties_.memoryTypes[index].propertyFlags & vk::MemoryPropertyDeviceLocalBit) != 0u) {
                outIndex = index;
                return true;
            }
        }
        return false;
    }

    static std::size_t constantIndex(const ReferenceConstantBufferRole role) noexcept
    {
        switch (role) {
        case ReferenceConstantBufferRole::Fsr4Upscaler: return 0;
        case ReferenceConstantBufferRole::SpdAutoExposure: return 1;
        case ReferenceConstantBufferRole::Rcas: return 2;
        case ReferenceConstantBufferRole::PassWeights: return 3;
        case ReferenceConstantBufferRole::None: break;
        }
        return 0;
    }

    ConstantResource* constantResource(const ReferenceConstantBufferRole role) noexcept
    {
        if (role == ReferenceConstantBufferRole::None) {
            return nullptr;
        }
        return &constantResources_[constantIndex(role)];
    }

    const NativeResource* ownedResource(const ResourceId id) const noexcept
    {
        const std::size_t index = resourceIndex(id);
        return index < resources_.size() && resources_[index].id == id ? &resources_[index] : nullptr;
    }

    NativeResource* ownedResource(const ResourceId id) noexcept
    {
        return const_cast<NativeResource*>(std::as_const(*this).ownedResource(id));
    }

    Status ensurePipeline(
        const VulkanFsr4ReferenceDispatchPlan& dispatch,
        const ReferenceFramePlan& framePlan,
        PipelineState*& outPipeline)
    {
        auto found = pipelines_.find(dispatch.shaderKey);
        if (found != pipelines_.end()) {
            outPipeline = &found->second;
            return Status::ok();
        }

        const VulkanFsr4ReferenceShaderBlobView blob = findVulkanFsr4ReferenceShaderBlob(createDesc_.shaderPack, dispatch.shaderKey);
        if (blob.data == nullptr || blob.size == 0) {
            std::ostringstream message;
            message << "missing Vulkan FSR4 shader pack entry for " << (dispatch.dispatchName != nullptr ? dispatch.dispatchName : "<unnamed>")
                << " preset=" << static_cast<std::uint32_t>(dispatch.shaderKey.preset)
                << " capacity=" << static_cast<std::uint32_t>(dispatch.shaderKey.frameCapacity)
                << " permutation=" << dispatch.shaderKey.permutationKey;
            return Status::invalidArgument(message.str());
        }
        if ((blob.size & 3u) != 0u) {
            return Status::runtimeFailure("Vulkan FSR4 shader pack entry byte size is not dword aligned");
        }
        constexpr std::uint32_t kSpirvMagic = 0x07230203u;
        constexpr std::uint32_t kSpirv15 = 0x00010500u;
        if (blob.size < 5u * sizeof(std::uint32_t)) {
            return Status::runtimeFailure("Vulkan FSR4 shader pack entry is truncated");
        }
        std::vector<std::uint32_t> alignedWords;
        const std::uint32_t* words = nullptr;
        if ((reinterpret_cast<std::uintptr_t>(blob.data) & (alignof(std::uint32_t) - 1u)) == 0u) {
            words = reinterpret_cast<const std::uint32_t*>(blob.data);
        } else {
            alignedWords.resize(blob.size / sizeof(std::uint32_t));
            std::memcpy(alignedWords.data(), blob.data, blob.size);
            words = alignedWords.data();
        }
        if (words[0] != kSpirvMagic || words[1] != kSpirv15) {
            return Status::runtimeFailure("Vulkan FSR4 shader pack entry is not SPIR-V 1.5");
        }

        std::vector<vk::DescriptorSetLayoutBinding> layoutBindings;
        layoutBindings.reserve(dispatch.bindings.size());
        for (const VulkanFsr4ReferenceDescriptorBinding& binding : dispatch.bindings) {
            layoutBindings.push_back(vk::DescriptorSetLayoutBinding{
                .binding = binding.binding,
                .descriptorType = nativeDescriptorType(binding.kind),
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageComputeBit,
                .pImmutableSamplers = nullptr,
            });
        }
        std::sort(layoutBindings.begin(), layoutBindings.end(), [](const auto& a, const auto& b) { return a.binding < b.binding; });
        const auto duplicate = std::adjacent_find(layoutBindings.begin(), layoutBindings.end(), [](const auto& a, const auto& b) {
            return a.binding == b.binding;
        });
        if (duplicate != layoutBindings.end()) {
            return Status::runtimeFailure(std::string("Vulkan FSR4 descriptor binding collision in ") + dispatch.dispatchName);
        }

        PipelineState pipeline{};
        pipeline.shaderByteSize = blob.size;
        const vk::DescriptorSetLayoutCreateInfo layoutInfo{
            .sType = vk::StructureTypeDescriptorSetLayoutCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = static_cast<std::uint32_t>(layoutBindings.size()),
            .pBindings = layoutBindings.data(),
        };
        vk::Result result = functions_.createDescriptorSetLayout(device_, &layoutInfo, nullptr, &pipeline.descriptorSetLayout);
        if (result != vk::Success) {
            return Status::runtimeFailure(vkResultText("vkCreateDescriptorSetLayout", result));
        }
        const vk::PipelineLayoutCreateInfo pipelineLayoutInfo{
            .sType = vk::StructureTypePipelineLayoutCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = &pipeline.descriptorSetLayout,
            .pushConstantRangeCount = 0,
            .pPushConstantRanges = nullptr,
        };
        result = functions_.createPipelineLayout(device_, &pipelineLayoutInfo, nullptr, &pipeline.pipelineLayout);
        if (result != vk::Success) {
            functions_.destroyDescriptorSetLayout(device_, pipeline.descriptorSetLayout, nullptr);
            return Status::runtimeFailure(vkResultText("vkCreatePipelineLayout", result));
        }
        const vk::ShaderModuleCreateInfo shaderInfo{
            .sType = vk::StructureTypeShaderModuleCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .codeSize = blob.size,
            .pCode = words,
        };
        vk::ShaderModule module = nullptr;
        result = functions_.createShaderModule(device_, &shaderInfo, nullptr, &module);
        if (result != vk::Success) {
            functions_.destroyPipelineLayout(device_, pipeline.pipelineLayout, nullptr);
            functions_.destroyDescriptorSetLayout(device_, pipeline.descriptorSetLayout, nullptr);
            return Status::runtimeFailure(vkResultText("vkCreateShaderModule", result));
        }
        const char* entryPoint = findDispatchEntryPoint(framePlan, dispatch.dispatchName);
        const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo requiredSubgroupSizeInfo{
            .sType = vk::StructureTypePipelineShaderStageRequiredSubgroupSizeCreateInfo,
            .pNext = nullptr,
            .requiredSubgroupSize = createDesc_.requiredSubgroupSize,
        };
        const vk::PipelineShaderStageCreateInfo stage{
            .sType = vk::StructureTypePipelineShaderStageCreateInfo,
            .pNext = createDesc_.requiredSubgroupSize != 0u
                ? static_cast<const void*>(&requiredSubgroupSizeInfo)
                : nullptr,
            .flags = 0,
            .stage = vk::ShaderStageComputeBit,
            .module = module,
            .pName = entryPoint,
            .pSpecializationInfo = nullptr,
        };
        const vk::ComputePipelineCreateInfo pipelineInfo{
            .sType = vk::StructureTypeComputePipelineCreateInfo,
            .pNext = nullptr,
            .flags = 0,
            .stage = stage,
            .layout = pipeline.pipelineLayout,
            .basePipelineHandle = nullptr,
            .basePipelineIndex = -1,
        };
        result = functions_.createComputePipelines(device_, nullptr, 1, &pipelineInfo, nullptr, &pipeline.pipeline);
        functions_.destroyShaderModule(device_, module, nullptr);
        if (result != vk::Success) {
            functions_.destroyPipelineLayout(device_, pipeline.pipelineLayout, nullptr);
            functions_.destroyDescriptorSetLayout(device_, pipeline.descriptorSetLayout, nullptr);
            std::ostringstream message;
            message << vkResultText("vkCreateComputePipelines", result)
                << " for " << (dispatch.dispatchName != nullptr ? dispatch.dispatchName : "<unnamed>")
                << " stage=" << dispatch.logicalStage
                << " permutation=" << dispatch.shaderKey.permutationKey;
            return Status::runtimeFailure(message.str());
        }
        auto [inserted, _] = pipelines_.emplace(dispatch.shaderKey, std::move(pipeline));
        outPipeline = &inserted->second;
        if (profileStream_ && apusr::profile::gpuEnabled()) {
            const char* workloadName = dispatch.dispatchName != nullptr ? dispatch.dispatchName : "fsr4.dispatch";
            std::uint64_t workloadId = apusr::profile::hashString(workloadName);
            workloadId = apusr::profile::hashCombine(workloadId, static_cast<std::uint64_t>(dispatch.shaderKey.preset));
            workloadId = apusr::profile::hashCombine(workloadId, static_cast<std::uint64_t>(dispatch.shaderKey.frameCapacity));
            workloadId = apusr::profile::hashCombine(workloadId, dispatch.shaderKey.permutationKey);
            apusr::backends::vk::SpirvComputeMetadata spirvMetadata{};
            (void)apusr::backends::vk::readSpirvComputeMetadata(blob.data, blob.size, spirvMetadata);
            apusr::profile::recordGpuWorkloadInfo(apusr::profile::GpuWorkloadInfo{
                .stream = profileStream_,
                .name = workloadName,
                .detail = entryPoint != nullptr ? entryPoint : "spirv.compute",
                .workloadId = workloadId,
                .nativePipeline = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(outPipeline->pipeline)),
                .codeHash = apusr::profile::hashString(std::string_view(
                    reinterpret_cast<const char*>(blob.data), blob.size)),
                .codeBytes = blob.size,
                .threadsX = spirvMetadata.localSizeX,
                .threadsY = spirvMetadata.localSizeY,
                .threadsZ = spirvMetadata.localSizeZ,
                .subgroupSize = createDesc_.requiredSubgroupSize,
                .flags = dispatch.shaderKey.permutationKey,
            });
        }
        return Status::ok();
    }

    const char* findDispatchEntryPoint(const ReferenceFramePlan& framePlan, const char* dispatchName) const noexcept
    {
        for (const ReferenceDispatchDesc& dispatch : framePlan.dispatches) {
            if (dispatch.name != nullptr && dispatchName != nullptr && std::strcmp(dispatch.name, dispatchName) == 0) {
                return dispatch.shader.entryPoint.c_str();
            }
        }
        return "main";
    }

    ExternalDescriptorKey makeExternalDescriptorKey(const FrameInputDesc& input, const ReferenceFramePlan& framePlan) const noexcept
    {
        ExternalDescriptorKey key{};
        const std::array<FrameInputSurfaceRole, 5> roles{{
            FrameInputSurfaceRole::Color,
            FrameInputSurfaceRole::Depth,
            FrameInputSurfaceRole::MotionVectors,
            FrameInputSurfaceRole::Exposure,
            FrameInputSurfaceRole::Output,
        }};
        for (std::size_t index = 0; index < roles.size(); ++index) {
            if (const FrameInputSurfaceDesc* surface = findFrameInputSurface(input, roles[index])) {
                key.views[index] = reinterpret_cast<std::uintptr_t>(surface->view.value);
            }
        }
        key.variantBits = (framePlan.constants.rcasEnabled != 0 ? 1u : 0u) |
            (framePlan.constants.useDefaultExposure != 0 ? 2u : 0u);
        return key;
    }

    Status allocateDescriptorSet(const vk::DescriptorSetLayout layout, vk::DescriptorSet& outSet)
    {
        for (std::size_t attempt = 0; attempt < 2; ++attempt) {
            if (descriptorPools_.empty()) {
                const Status status = createDescriptorPool();
                if (!status.succeeded()) {
                    return status;
                }
            }
            const vk::DescriptorSetAllocateInfo info{
                .sType = vk::StructureTypeDescriptorSetAllocateInfo,
                .pNext = nullptr,
                .descriptorPool = descriptorPools_.back(),
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
            };
            const vk::Result result = functions_.allocateDescriptorSets(device_, &info, &outSet);
            if (result == vk::Success) {
                return Status::ok();
            }
            if (result != vk::ErrorOutOfPoolMemory && result != vk::ErrorFragmentedPool) {
                return Status::runtimeFailure(vkResultText("vkAllocateDescriptorSets", result));
            }
            const Status status = createDescriptorPool();
            if (!status.succeeded()) {
                return status;
            }
        }
        return Status::runtimeFailure("Vulkan FSR4 descriptor allocation exhausted a fresh descriptor pool");
    }

    Status ensureDescriptorSet(
        PipelineState& pipeline,
        const VulkanFsr4ReferenceDispatchPlan& dispatch,
        const ReferenceFramePlan& framePlan,
        const FrameInputDesc& frameInput,
        const ExternalDescriptorKey& key,
        vk::DescriptorSet& outSet)
    {
        const auto found = pipeline.descriptorSets.find(key);
        if (found != pipeline.descriptorSets.end()) {
            outSet = found->second;
            return Status::ok();
        }
        Status status = allocateDescriptorSet(pipeline.descriptorSetLayout, outSet);
        if (!status.succeeded()) {
            return status;
        }

        std::vector<vk::DescriptorImageInfo> imageInfos;
        std::vector<vk::DescriptorBufferInfo> bufferInfos;
        std::vector<vk::WriteDescriptorSet> writes;
        imageInfos.reserve(dispatch.bindings.size());
        bufferInfos.reserve(dispatch.bindings.size());
        writes.reserve(dispatch.bindings.size());

        for (const VulkanFsr4ReferenceDescriptorBinding& binding : dispatch.bindings) {
            vk::WriteDescriptorSet write{
                .sType = vk::StructureTypeWriteDescriptorSet,
                .pNext = nullptr,
                .dstSet = outSet,
                .dstBinding = binding.binding,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = nativeDescriptorType(binding.kind),
                .pImageInfo = nullptr,
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            };
            if (binding.kind == VulkanFsr4ReferenceDescriptorKind::Sampler) {
                imageInfos.push_back(vk::DescriptorImageInfo{.sampler = sampler_, .imageView = nullptr, .imageLayout = vk::ImageLayoutUndefined});
                write.pImageInfo = &imageInfos.back();
            } else if (binding.kind == VulkanFsr4ReferenceDescriptorKind::UniformBuffer) {
                ConstantResource* constant = constantResource(binding.constantBuffer);
                if (constant == nullptr || constant->buffer == nullptr) {
                    return Status::runtimeFailure(std::string("Vulkan FSR4 missing constant buffer for ") + binding.shaderName);
                }
                bufferInfos.push_back(vk::DescriptorBufferInfo{.buffer = constant->buffer, .offset = 0, .range = constant->byteSize});
                write.pBufferInfo = &bufferInfos.back();
            } else if (binding.kind == VulkanFsr4ReferenceDescriptorKind::StorageBuffer) {
                const NativeResource* resource = ownedResource(binding.resource);
                if (resource == nullptr || resource->buffer == nullptr) {
                    if (binding.optional) {
                        continue;
                    }
                    return Status::runtimeFailure(std::string("Vulkan FSR4 missing storage buffer ") + resourceIdName(binding.resource));
                }
                bufferInfos.push_back(vk::DescriptorBufferInfo{.buffer = resource->buffer, .offset = 0, .range = resource->byteSize});
                write.pBufferInfo = &bufferInfos.back();
            } else {
                vk::ImageView view = nullptr;
                vk::ImageLayout layout = vk::ImageLayoutGeneral;
                status = resolveImageDescriptor(dispatch, binding.resource, framePlan, frameInput, view, layout);
                if (!status.succeeded()) {
                    if (binding.optional) {
                        continue;
                    }
                    return status;
                }
                imageInfos.push_back(vk::DescriptorImageInfo{.sampler = nullptr, .imageView = view, .imageLayout = layout});
                write.pImageInfo = &imageInfos.back();
            }
            writes.push_back(write);
        }
        if (!writes.empty()) {
            functions_.updateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
        pipeline.descriptorSets.emplace(key, outSet);
        return Status::ok();
    }

    vk::ImageLayout dispatchImageLayout(
        const VulkanFsr4ReferenceDispatchPlan& dispatch,
        const ResourceId requested,
        const ReferenceFramePlan& framePlan) const noexcept
    {
        const ResourceId resolved = resolveResource(resourcePlan_, framePlan, requested);
        const ResourceDesc* description = findResource(resourcePlan_, resolved);

        // Keep APUSR-owned read/write images in GENERAL for their entire
        // lifetime. Most FSR4 temporal/model intermediates alternate between
        // SRV and UAV use, so flipping them to SHADER_READ_ONLY_OPTIMAL for
        // individual read-only dispatches would create layout ping-pong and can
        // cost more than it saves. Truly read-only owned images (currently the
        // default exposure) can remain in the sampled-only layout.
        if (description != nullptr && description->lifetime != ResourceLifetime::External) {
            return description->uav ? vk::ImageLayoutGeneral : vk::ImageLayoutShaderReadOnlyOptimal;
        }

        // External images are owned by the application and their role can
        // legitimately change at a dispatch boundary. Merge aliases within the
        // dispatch so a sampled+storage view of the same concrete image never
        // receives contradictory descriptor layouts.
        for (const VulkanFsr4ReferenceDescriptorBinding& binding : dispatch.bindings) {
            if (binding.resource == ResourceId::Null ||
                (binding.kind != VulkanFsr4ReferenceDescriptorKind::SampledImage &&
                 binding.kind != VulkanFsr4ReferenceDescriptorKind::StorageImage)) {
                continue;
            }
            if (resolveResource(resourcePlan_, framePlan, binding.resource) == resolved &&
                binding.kind == VulkanFsr4ReferenceDescriptorKind::StorageImage) {
                return vk::ImageLayoutGeneral;
            }
        }
        return vk::ImageLayoutShaderReadOnlyOptimal;
    }

    Status resolveImageDescriptor(
        const VulkanFsr4ReferenceDispatchPlan& dispatch,
        const ResourceId requested,
        const ReferenceFramePlan& framePlan,
        const FrameInputDesc& frameInput,
        vk::ImageView& outView,
        vk::ImageLayout& outLayout) const
    {
        const ResourceId resolved = resolveResource(resourcePlan_, framePlan, requested);
        if (isExternalResource(resolved)) {
            const FrameInputSurfaceDesc* surface = findFrameInputSurface(frameInput, inputRoleForResource(resolved));
            if (surface == nullptr || surface->view.value == nullptr) {
                return Status::invalidArgument(std::string("Vulkan FSR4 frame is missing image view for ") + resourceIdName(resolved));
            }
            outView = reinterpret_cast<vk::ImageView>(surface->view.value);
        } else {
            const NativeResource* resource = ownedResource(resolved);
            if (resource == nullptr || resource->view == nullptr) {
                return Status::runtimeFailure(std::string("Vulkan FSR4 missing internal image ") + resourceIdName(resolved));
            }
            outView = resource->view;
        }
        // Descriptor layout describes APUSR's dispatch-time state rather than
        // the incoming Streamline state. Synchronization uses the same merged
        // per-resource rule below, including sampled+storage aliases.
        outLayout = dispatchImageLayout(dispatch, resolved, framePlan);
        return Status::ok();
    }

    Status recordConstantUpdates(const vk::CommandBuffer commandBuffer, const ReferenceFramePlan& framePlan)
    {
        Pass0WeightsConstants weights{};
        Status status = makeReferencePass0WeightsConstants(framePlan.preset, weights);
        if (!status.succeeded()) {
            return status;
        }
        const std::array<std::pair<ReferenceConstantBufferRole, std::pair<const void*, std::uint64_t>>, 4> updates{{
            {ReferenceConstantBufferRole::Fsr4Upscaler, {&framePlan.constants, sizeof(framePlan.constants)}},
            {ReferenceConstantBufferRole::SpdAutoExposure, {&framePlan.spdConstants, sizeof(framePlan.spdConstants)}},
            {ReferenceConstantBufferRole::Rcas, {&framePlan.rcasConstants, sizeof(framePlan.rcasConstants)}},
            {ReferenceConstantBufferRole::PassWeights, {&weights, sizeof(weights)}},
        }};
        std::array<vk::BufferMemoryBarrier, 4> before{};
        std::array<vk::BufferMemoryBarrier, 4> after{};
        for (std::size_t index = 0; index < updates.size(); ++index) {
            ConstantResource* resource = constantResource(updates[index].first);
            before[index] = vk::BufferMemoryBarrier{
                .sType = vk::StructureTypeBufferMemoryBarrier,
                .pNext = nullptr,
                .srcAccessMask = vk::AccessShaderReadBit,
                .dstAccessMask = vk::AccessTransferWriteBit,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .buffer = resource->buffer,
                .offset = 0,
                .size = resource->byteSize,
            };
            after[index] = before[index];
            after[index].srcAccessMask = vk::AccessTransferWriteBit;
            after[index].dstAccessMask = vk::AccessShaderReadBit;
        }
        if (constantsWritten_) {
            functions_.cmdPipelineBarrier(
                commandBuffer,
                vk::PipelineStageComputeShaderBit,
                vk::PipelineStageTransferBit,
                0,
                0,
                nullptr,
                static_cast<std::uint32_t>(before.size()),
                before.data(),
                0,
                nullptr);
        }
        for (const auto& update : updates) {
            ConstantResource* resource = constantResource(update.first);
            functions_.cmdUpdateBuffer(commandBuffer, resource->buffer, 0, update.second.second, update.second.first);
        }
        functions_.cmdPipelineBarrier(
            commandBuffer,
            vk::PipelineStageTransferBit,
            vk::PipelineStageComputeShaderBit,
            0,
            0,
            nullptr,
            static_cast<std::uint32_t>(after.size()),
            after.data(),
            0,
            nullptr);
        constantsWritten_ = true;
        return Status::ok();
    }

    Status recordOneTimeInitializers(const vk::CommandBuffer commandBuffer)
    {
        if (!initializerRecorded_) {
            NativeResource* initializer = ownedResource(ResourceId::InitializerBuffer);
            if (initializer == nullptr || initializer->buffer == nullptr) {
                return Status::runtimeFailure("Vulkan FSR4 initializer buffer allocation is missing");
            }
            if (initializer->bufferAccess != 0u) {
                const vk::BufferMemoryBarrier before{
                    .sType = vk::StructureTypeBufferMemoryBarrier,
                    .pNext = nullptr,
                    .srcAccessMask = initializer->bufferAccess,
                    .dstAccessMask = vk::AccessTransferWriteBit,
                    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .buffer = initializer->buffer,
                    .offset = 0,
                    .size = initializer->byteSize,
                };
                functions_.cmdPipelineBarrier(
                    commandBuffer,
                    vk::PipelineStageComputeShaderBit,
                    vk::PipelineStageTransferBit,
                    0,
                    0,
                    nullptr,
                    1,
                    &before,
                    0,
                    nullptr);
            }
            std::uint64_t offset = 0;
            while (offset < initializerBytes_.size()) {
                const std::uint64_t remaining = initializerBytes_.size() - offset;
                const std::uint64_t chunk = std::min<std::uint64_t>(remaining, kMaxCmdUpdateBufferBytes);
                const std::uint64_t alignedChunk = chunk & ~std::uint64_t{3};
                if (alignedChunk == 0) {
                    return Status::runtimeFailure("Vulkan FSR4 initializer byte count must be dword aligned");
                }
                functions_.cmdUpdateBuffer(
                    commandBuffer,
                    initializer->buffer,
                    offset,
                    alignedChunk,
                    initializerBytes_.data() + offset);
                offset += alignedChunk;
            }
            const vk::BufferMemoryBarrier barrier{
                .sType = vk::StructureTypeBufferMemoryBarrier,
                .pNext = nullptr,
                .srcAccessMask = vk::AccessTransferWriteBit,
                .dstAccessMask = vk::AccessShaderReadBit,
                .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                .buffer = initializer->buffer,
                .offset = 0,
                .size = initializer->byteSize,
            };
            functions_.cmdPipelineBarrier(
                commandBuffer,
                vk::PipelineStageTransferBit,
                vk::PipelineStageComputeShaderBit,
                0,
                0,
                nullptr,
                1,
                &barrier,
                0,
                nullptr);
            initializer->bufferAccess = vk::AccessShaderReadBit;
            initializerRecorded_ = true;
        }

        if (!defaultExposureRecorded_) {
            NativeResource* exposure = ownedResource(ResourceId::InternalDefaultExposure);
            if (exposure == nullptr || exposure->image == nullptr) {
                return Status::runtimeFailure("Vulkan FSR4 default exposure image allocation is missing");
            }
            transitionImage(
                commandBuffer,
                *exposure,
                vk::ImageLayoutTransferDstOptimal,
                vk::PipelineStageTopOfPipeBit,
                vk::PipelineStageTransferBit,
                0,
                vk::AccessTransferWriteBit);
            const vk::ClearColorValue clear{{1.0f, 1.0f, 0.0f, 0.0f}};
            const vk::ImageSubresourceRange range{vk::ImageAspectColorBit, 0, 1, 0, 1};
            functions_.cmdClearColorImage(commandBuffer, exposure->image, vk::ImageLayoutTransferDstOptimal, &clear, 1, &range);
            transitionImage(
                commandBuffer,
                *exposure,
                vk::ImageLayoutShaderReadOnlyOptimal,
                vk::PipelineStageTransferBit,
                vk::PipelineStageComputeShaderBit,
                vk::AccessTransferWriteBit,
                vk::AccessShaderReadBit);
            defaultExposureRecorded_ = true;
        }
        return Status::ok();
    }

    Status prepareDispatchResources(
        const vk::CommandBuffer commandBuffer,
        const VulkanFsr4ReferenceDispatchPlan& dispatch,
        const ReferenceFramePlan& framePlan,
        const FrameInputDesc& frameInput,
        ExternalImageStates& externalImageStates,
        std::array<vk::AccessFlags, kResourceSlotCount>& frameBufferAccess,
        DispatchPreparationStats* preparationStats)
    {
        if (preparationStats != nullptr) {
            *preparationStats = DispatchPreparationStats{};
        }
        if (dispatch.passKind == PassKind::SpdAutoExposure) {
            NativeResource* atomic = ownedResource(ResourceId::SpdAtomicCount);
            if (atomic == nullptr) {
                return Status::runtimeFailure("Vulkan FSR4 SPD atomic image is missing");
            }
            const vk::PipelineStageFlags srcStage = atomic->layout == vk::ImageLayoutUndefined
                ? vk::PipelineStageTopOfPipeBit
                : (atomic->imageStage != 0u ? atomic->imageStage : vk::PipelineStageComputeShaderBit);
            const vk::AccessFlags srcAccess = atomic->layout == vk::ImageLayoutUndefined ? 0u : atomic->imageAccess;
            if (transitionImage(
                    commandBuffer, *atomic, vk::ImageLayoutTransferDstOptimal,
                    srcStage, vk::PipelineStageTransferBit, srcAccess, vk::AccessTransferWriteBit) &&
                preparationStats != nullptr) {
                ++preparationStats->barrierCalls;
                ++preparationStats->imageBarriers;
            }
            const vk::ClearColorValue zero{};
            const vk::ImageSubresourceRange range{vk::ImageAspectColorBit, 0, 1, 0, 1};
            functions_.cmdClearColorImage(
                commandBuffer, atomic->image, vk::ImageLayoutTransferDstOptimal, &zero, 1, &range);
            if (preparationStats != nullptr) {
                ++preparationStats->clearCalls;
            }
            if (transitionImage(
                    commandBuffer, *atomic, vk::ImageLayoutGeneral,
                    vk::PipelineStageTransferBit, vk::PipelineStageComputeShaderBit,
                    vk::AccessTransferWriteBit, vk::AccessShaderReadBit | vk::AccessShaderWriteBit) &&
                preparationStats != nullptr) {
                ++preparationStats->barrierCalls;
                ++preparationStats->imageBarriers;
            }
            // The transfer->compute barrier already makes the clear visible to this dispatch.
            // Record the actual shader access below instead of manufacturing another dependency.
            atomic->imageAccess = 0u;
        }

        enum class UseKind : std::uint8_t { None, Buffer, InternalImage, ExternalImage };
        struct DispatchUse {
            UseKind kind = UseKind::None;
            vk::AccessFlags access = 0;
            vk::ImageLayout layout = vk::ImageLayoutUndefined;
            NativeResource* internal = nullptr;
            const FrameInputSurfaceDesc* surface = nullptr;
            ExternalImageState* externalState = nullptr;
        };

        std::array<DispatchUse, kResourceSlotCount> uses{};
        for (const VulkanFsr4ReferenceDescriptorBinding& binding : dispatch.bindings) {
            if (binding.resource == ResourceId::Null || binding.kind == VulkanFsr4ReferenceDescriptorKind::Sampler ||
                binding.kind == VulkanFsr4ReferenceDescriptorKind::UniformBuffer) {
                continue;
            }

            const ResourceId resolved = resolveResource(resourcePlan_, framePlan, binding.resource);
            const std::size_t index = resourceIndex(resolved);
            if (index >= uses.size()) {
                return Status::runtimeFailure("Vulkan FSR4 resource id exceeds synchronization state capacity");
            }

            DispatchUse candidate{};
            candidate.access = binding.kind == VulkanFsr4ReferenceDescriptorKind::SampledImage
                ? vk::AccessShaderReadBit
                : shaderAccessFlags(binding.access);
            if (isExternalResource(resolved)) {
                const FrameInputSurfaceRole role = inputRoleForResource(resolved);
                candidate.surface = findFrameInputSurface(frameInput, role);
                const std::size_t stateIndex = externalImageStateIndex(role);
                if (candidate.surface == nullptr || candidate.surface->resource.value == nullptr) {
                    return Status::invalidArgument(std::string("Vulkan FSR4 missing external surface ") + resourceIdName(resolved));
                }
                if (stateIndex >= externalImageStates.size() || !externalImageStates[stateIndex].known) {
                    return Status::invalidArgument(std::string("Vulkan FSR4 external image layout is unknown for ") + resourceIdName(resolved));
                }
                candidate.kind = UseKind::ExternalImage;
                candidate.externalState = &externalImageStates[stateIndex];
                candidate.layout = dispatchImageLayout(dispatch, resolved, framePlan);
            } else {
                candidate.internal = ownedResource(resolved);
                if (candidate.internal == nullptr) {
                    if (binding.optional) {
                        continue;
                    }
                    return Status::runtimeFailure(std::string("Vulkan FSR4 missing internal resource ") + resourceIdName(resolved));
                }
                if (candidate.internal->kind == ResourceKind::Texture2D) {
                    candidate.kind = UseKind::InternalImage;
                    candidate.layout = dispatchImageLayout(dispatch, resolved, framePlan);
                } else {
                    candidate.kind = UseKind::Buffer;
                }
            }

            DispatchUse& use = uses[index];
            if (use.kind == UseKind::None) {
                use = candidate;
                continue;
            }
            if (use.kind != candidate.kind ||
                ((use.kind == UseKind::InternalImage || use.kind == UseKind::ExternalImage) && use.layout != candidate.layout)) {
                return Status::runtimeFailure(std::string("Vulkan FSR4 dispatch binds resource with conflicting synchronization roles: ") + resourceIdName(resolved));
            }
            use.access |= candidate.access;
        }

        // At most one barrier is emitted per resolved resource. Keep this hot
        // per-dispatch path allocation-free; kResourceSlotCount is the exact
        // synchronization-state capacity used by the resource table above.
        std::array<vk::BufferMemoryBarrier, kResourceSlotCount> bufferBarriers{};
        std::array<vk::ImageMemoryBarrier, kResourceSlotCount> imageBarriers{};
        std::uint32_t bufferBarrierCount = 0u;
        std::uint32_t imageBarrierCount = 0u;
        vk::PipelineStageFlags sourceStages = 0u;

        for (std::size_t index = 0; index < uses.size(); ++index) {
            DispatchUse& use = uses[index];
            if (use.kind == UseKind::None) {
                continue;
            }
            if (use.kind == UseKind::Buffer) {
                const vk::AccessFlags prior = frameBufferAccess[index];
                if (needsShaderMemoryDependency(prior, use.access)) {
                    bufferBarriers[bufferBarrierCount++] = vk::BufferMemoryBarrier{
                        .sType = vk::StructureTypeBufferMemoryBarrier,
                        .pNext = nullptr,
                        .srcAccessMask = prior,
                        .dstAccessMask = use.access,
                        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                        .buffer = use.internal->buffer,
                        .offset = 0,
                        .size = use.internal->byteSize,
                    };
                    sourceStages |= vk::PipelineStageComputeShaderBit;
                }
                frameBufferAccess[index] = use.access;
                continue;
            }

            if (use.kind == UseKind::InternalImage) {
                NativeResource& resource = *use.internal;
                const bool layoutChange = resource.layout != use.layout;
                if (layoutChange || needsShaderMemoryDependency(resource.imageAccess, use.access)) {
                    imageBarriers[imageBarrierCount++] = vk::ImageMemoryBarrier{
                        .sType = vk::StructureTypeImageMemoryBarrier,
                        .pNext = nullptr,
                        .srcAccessMask = resource.layout == vk::ImageLayoutUndefined ? 0u : resource.imageAccess,
                        .dstAccessMask = use.access,
                        .oldLayout = resource.layout,
                        .newLayout = use.layout,
                        .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                        .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                        .image = resource.image,
                        .subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectColorBit, 0, 1, 0, 1},
                    };
                    sourceStages |= resource.layout == vk::ImageLayoutUndefined
                        ? vk::PipelineStageTopOfPipeBit
                        : (resource.imageStage != 0u ? resource.imageStage : vk::PipelineStageComputeShaderBit);
                }
                resource.layout = use.layout;
                resource.imageStage = vk::PipelineStageComputeShaderBit;
                resource.imageAccess = use.access;
                continue;
            }

            ExternalImageState& state = *use.externalState;
            const bool layoutChange = state.layout != use.layout;
            if (layoutChange || needsShaderMemoryDependency(state.access, use.access)) {
                imageBarriers[imageBarrierCount++] = vk::ImageMemoryBarrier{
                    .sType = vk::StructureTypeImageMemoryBarrier,
                    .pNext = nullptr,
                    .srcAccessMask = state.access,
                    .dstAccessMask = use.access,
                    .oldLayout = state.layout,
                    .newLayout = use.layout,
                    .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
                    .image = reinterpret_cast<vk::Image>(use.surface->resource.value),
                    .subresourceRange = vk::ImageSubresourceRange{
                        use.surface->nativeAspectMask != 0
                            ? static_cast<vk::ImageAspectFlags>(use.surface->nativeAspectMask)
                            : vk::ImageAspectColorBit,
                        use.surface->subresourceBaseMip,
                        use.surface->subresourceMipCount,
                        use.surface->subresourceBaseLayer,
                        use.surface->subresourceLayerCount},
                };
                sourceStages |= state.stage != 0u ? state.stage : vk::PipelineStageAllCommandsBit;
            }
            state.layout = use.layout;
            state.stage = vk::PipelineStageComputeShaderBit;
            state.access = use.access;
        }

        if (bufferBarrierCount != 0u || imageBarrierCount != 0u) {
            if (preparationStats != nullptr) {
                ++preparationStats->barrierCalls;
                preparationStats->bufferBarriers += bufferBarrierCount;
                preparationStats->imageBarriers += imageBarrierCount;
            }
            functions_.cmdPipelineBarrier(
                commandBuffer,
                sourceStages != 0u ? sourceStages : vk::PipelineStageTopOfPipeBit,
                vk::PipelineStageComputeShaderBit,
                0,
                0,
                nullptr,
                bufferBarrierCount,
                bufferBarriers.data(),
                imageBarrierCount,
                imageBarriers.data());
        }
        return Status::ok();
    }

    bool transitionImage(
        const vk::CommandBuffer commandBuffer,
        NativeResource& resource,
        const vk::ImageLayout newLayout,
        const vk::PipelineStageFlags srcStage,
        const vk::PipelineStageFlags dstStage,
        const vk::AccessFlags srcAccess,
        const vk::AccessFlags dstAccess)
    {
        if (resource.layout == newLayout && srcAccess == 0u && dstAccess == 0u) {
            return false;
        }
        const vk::ImageMemoryBarrier barrier{
            .sType = vk::StructureTypeImageMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = srcAccess,
            .dstAccessMask = dstAccess,
            .oldLayout = resource.layout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = vk::QueueFamilyIgnored,
            .dstQueueFamilyIndex = vk::QueueFamilyIgnored,
            .image = resource.image,
            .subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectColorBit, 0, 1, 0, 1},
        };
        functions_.cmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        resource.layout = newLayout;
        resource.imageStage = dstStage;
        resource.imageAccess = dstAccess;
        return true;
    }

    void destroy() noexcept
    {
        if (device_ != nullptr && hasRecordedWork_ && functions_.deviceWaitIdle != nullptr) {
            (void)functions_.deviceWaitIdle(device_);
        }
        flushGpuTimestampProfiler();
        destroyGpuTimestampProfiler();
        if (device_ != nullptr && functions_.destroyPipeline != nullptr) {
            for (auto& [_, pipeline] : pipelines_) {
                if (pipeline.pipeline != nullptr) { functions_.destroyPipeline(device_, pipeline.pipeline, nullptr); }
                if (pipeline.pipelineLayout != nullptr) { functions_.destroyPipelineLayout(device_, pipeline.pipelineLayout, nullptr); }
                if (pipeline.descriptorSetLayout != nullptr) { functions_.destroyDescriptorSetLayout(device_, pipeline.descriptorSetLayout, nullptr); }
            }
            for (vk::DescriptorPool pool : descriptorPools_) {
                if (pool != nullptr) { functions_.destroyDescriptorPool(device_, pool, nullptr); }
            }
            if (sampler_ != nullptr) { functions_.destroySampler(device_, sampler_, nullptr); }
            for (ConstantResource& resource : constantResources_) {
                if (resource.buffer != nullptr) { functions_.destroyBuffer(device_, resource.buffer, nullptr); }
                if (resource.memory != nullptr) { functions_.freeMemory(device_, resource.memory, nullptr); }
            }
            for (NativeResource& resource : resources_) {
                if (resource.view != nullptr) { functions_.destroyImageView(device_, resource.view, nullptr); }
                if (resource.image != nullptr) { functions_.destroyImage(device_, resource.image, nullptr); }
                if (resource.buffer != nullptr) { functions_.destroyBuffer(device_, resource.buffer, nullptr); }
                if (resource.memory != nullptr) { functions_.freeMemory(device_, resource.memory, nullptr); }
            }
        }
        pipelines_.clear();
        descriptorPools_.clear();
        resources_ = {};
        constantResources_ = {};
        initializerBytes_.clear();
        sampler_ = nullptr;
        instance_ = nullptr;
        physicalDevice_ = nullptr;
        device_ = nullptr;
        functions_ = {};
        memoryProperties_ = {};
        initializerRecorded_ = false;
        initializerPresetValid_ = false;
        defaultExposureRecorded_ = false;
        constantsWritten_ = false;
        queueFamilyIndices_.clear();
        gpuTimestampHasSampleDeadline_ = false;
        gpuTimestampNextSampleTime_ = {};
        gpuTimestampSampleSignature_ = {};
        gpuTimestampPeriodNs_ = 0.0;
        gpuTimestampApiVersion_ = 0u;
        gpuTimestampDriverVersion_ = 0u;
        gpuTimestampVendorId_ = 0u;
        gpuTimestampDeviceId_ = 0u;
        gpuTimestampLastRecordTime_ = {};
        gpuTimestampRuntimeSequence_ = 0u;
        gpuTimestampCurrentBurstSequence_ = 0u;
        gpuTimestampBurstRemaining_ = 0u;
        gpuTimestampCurrentBurstConfigurationChanged_ = false;
        gpuTimestampSampleSignatureValid_ = false;
        gpuTimestampRequested_ = false;
        gpuClockCalibrationAvailable_ = false;
        gpuClockHostTimeDomain_ = profilerHostTimeDomain();
        hasRecordedWork_ = false;
        initialized_ = false;
        library_.close();
        shaderPackFile_.close();
    }

    VulkanFsr4ReferenceCreateDesc createDesc_;
    MappedShaderPackFile shaderPackFile_;
    ResourcePlan resourcePlan_;
    VulkanLibrary library_;
    VulkanFunctions functions_;
    vk::Instance instance_ = nullptr;
    vk::PhysicalDevice physicalDevice_ = nullptr;
    vk::Device device_ = nullptr;
    vk::PhysicalDeviceMemoryProperties memoryProperties_{};
    std::array<NativeResource, kResourceSlotCount> resources_{};
    std::array<ConstantResource, 4> constantResources_{};
    std::vector<std::uint8_t> initializerBytes_;
    Preset initializerPreset_ = Preset::Balanced;
    bool initializerPresetValid_ = false;
    vk::Sampler sampler_ = nullptr;
    std::vector<vk::DescriptorPool> descriptorPools_;
    std::unordered_map<VulkanFsr4ReferenceShaderKey, PipelineState, VulkanShaderKeyHash, VulkanShaderKeyEqual> pipelines_;
    bool initializerRecorded_ = false;
    bool defaultExposureRecorded_ = false;
    bool constantsWritten_ = false;
    std::vector<std::uint32_t> queueFamilyIndices_;
    std::array<vk::QueryPool, kVulkanGpuTimestampRingSlots> gpuTimestampPools_{};
    std::array<VulkanGpuTimestampFrameMetadata, kVulkanGpuTimestampRingSlots> gpuTimestampFrames_{};
    std::vector<std::uint32_t> gpuTimestampValidBitsByFamily_;
    std::ofstream gpuTimestampFile_;
    std::chrono::steady_clock::time_point gpuTimestampNextSampleTime_{};
    VulkanGpuTimestampSampleSignature gpuTimestampSampleSignature_{};
    double gpuTimestampPeriodNs_ = 0.0;
    std::uint32_t gpuTimestampApiVersion_ = 0u;
    std::uint32_t gpuTimestampDriverVersion_ = 0u;
    std::uint32_t gpuTimestampVendorId_ = 0u;
    std::uint32_t gpuTimestampDeviceId_ = 0u;
    std::chrono::steady_clock::time_point gpuTimestampLastRecordTime_{};
    std::uint64_t gpuTimestampRuntimeSequence_ = 0u;
    std::uint64_t gpuTimestampCurrentBurstSequence_ = 0u;
    std::uint32_t gpuTimestampBurstRemaining_ = 0u;
    bool gpuTimestampCurrentBurstConfigurationChanged_ = false;
    bool gpuTimestampSampleSignatureValid_ = false;
    bool gpuTimestampHasSampleDeadline_ = false;
    bool gpuTimestampRequested_ = false;
    bool gpuTimestampActive_ = false;
    bool gpuClockCalibrationAvailable_ = false;
    vk::TimeDomain gpuClockHostTimeDomain_ = profilerHostTimeDomain();
    apusr::profile::StreamHandle profileStream_{};
    apusr::profile::SampleGate profileSampleGate_{};
    std::uint64_t profileLastConfigurationHash_ = 0u;
    bool hasRecordedWork_ = false;
    bool initialized_ = false;
};

} // namespace

VulkanFsr4ReferenceRecordPlan makeVulkanFsr4ReferenceRecordPlan(
    const VulkanFsr4ReferenceCreateDesc& createDesc,
    const VulkanFsr4ReferenceFrameDesc& frameDesc)
{
    VulkanFsr4ReferenceRecordPlan plan{};
    plan.framePlan = makeReferenceFramePlan(createDesc.runner, frameDesc.runner);
    plan.resourcePlan = makeReferenceResourcePlan(ResourcePlanDesc{
        .maxRenderSize = createDesc.runner.maxRenderSize,
        .maxUpscaleSize = createDesc.runner.maxUpscaleSize,
        .includeExternalResources = true,
        .includeDebugInformation = createDesc.runner.allowDebugVisualization,
    });
    plan.dispatches.reserve(plan.framePlan.dispatches.size());
    for (const ReferenceDispatchDesc& dispatch : plan.framePlan.dispatches) {
        VulkanFsr4ReferenceDispatchPlan native{};
        native.dispatchName = dispatch.name;
        native.passKind = dispatch.kind;
        native.logicalStage = dispatch.logicalStage;
        native.grid = dispatch.grid;
        native.shaderKey = VulkanFsr4ReferenceShaderKey{
            .pipelineVariant = plan.framePlan.pipelineVariant,
            .preset = plan.framePlan.preset,
            .frameCapacity = plan.framePlan.frameCapacity,
            .permutationKey = plan.framePlan.permutationKey,
            .passKind = dispatch.kind,
            .logicalStage = dispatch.logicalStage,
        };
        const bool vectorScratchStage = dispatch.kind == PassKind::ModelMiddle &&
            (dispatch.logicalStage == 3u || dispatch.logicalStage == 6u || dispatch.logicalStage == 7u ||
             dispatch.logicalStage == 8u || dispatch.logicalStage == 9u);
        const bool vectorInitializerStage = vectorScratchStage && dispatch.logicalStage != 3u;
        const bool vectorScratchWriteStage = dispatch.kind == PassKind::ModelMiddle &&
            (dispatch.logicalStage == 3u || dispatch.logicalStage == 6u || dispatch.logicalStage == 9u);
        native.bindings.reserve(
            dispatch.bindings.size() + (vectorInitializerStage ? 1u : 0u) + (vectorScratchStage ? 1u : 0u));
        for (const ReferenceResourceBindingDesc& binding : dispatch.bindings) {
            native.bindings.push_back(VulkanFsr4ReferenceDescriptorBinding{
                .binding = descriptorBindingNumber(binding),
                .kind = descriptorKind(binding.bindingClass),
                .resource = binding.resource == ResourceId::Null
                    ? ResourceId::Null
                    : resolveResource(plan.resourcePlan, plan.framePlan, binding.resource),
                .constantBuffer = binding.constantBuffer,
                .access = binding.access,
                .shaderName = binding.shaderName,
                .optional = binding.optional,
            });
        }
        if (vectorScratchStage) {
            // DXC's SPIR-V backend scalarizes descriptor-backed ByteAddressBuffer.Load4/Store4.
            // Audited 16-byte-aligned stages use uint4-typed aliases to keep those accesses vectorized.
            // Vulkan storage-buffer descriptors do not encode an element type, so the same allocations
            // can be rebound at the alias slots without a copy or an ownership/synchronization boundary.
            if (vectorInitializerStage) {
                native.bindings.push_back(VulkanFsr4ReferenceDescriptorBinding{
                    .binding = kVulkanFsr4SrvBindingBase + kVulkanFsr4VectorInitializerSrvRegister,
                    .kind = VulkanFsr4ReferenceDescriptorKind::StorageBuffer,
                    .resource = resolveResource(plan.resourcePlan, plan.framePlan, ResourceId::InitializerBuffer),
                    .access = ReferenceResourceAccess::Read,
                    .shaderName = "ApusrMl2cInitializerVector",
                });
            }
            native.bindings.push_back(VulkanFsr4ReferenceDescriptorBinding{
                .binding = kVulkanFsr4UavBindingBase + kVulkanFsr4VectorScratchUavRegister,
                .kind = VulkanFsr4ReferenceDescriptorKind::StorageBuffer,
                .resource = resolveResource(plan.resourcePlan, plan.framePlan, ResourceId::ScratchBuffer),
                .access = vectorScratchWriteStage ? ReferenceResourceAccess::ReadWrite : ReferenceResourceAccess::Read,
                .shaderName = "ApusrMl2cScratchVector",
            });
        }
        plan.dispatches.push_back(std::move(native));
    }
    return plan;
}

Status validateVulkanFsr4ReferenceShaderPack(const VulkanFsr4ReferenceShaderPackView pack)
{
    ShaderPackHeaderDisk header{};
    if (!readShaderPackObject(pack, 0, header)) {
        return Status::invalidArgument("Vulkan FSR4 shader pack is missing or truncated");
    }
    if (!std::equal(kShaderPackMagic.begin(), kShaderPackMagic.end(), header.magic)) {
        return Status::invalidArgument("Vulkan FSR4 shader pack has an invalid magic header");
    }
    if (header.headerBytes != sizeof(ShaderPackHeaderDisk) || header.totalBytes != pack.size) {
        return Status::invalidArgument("Vulkan FSR4 shader pack header size does not match the embedded payload");
    }
    const std::uint64_t entriesEnd = static_cast<std::uint64_t>(header.entriesOffset) +
        static_cast<std::uint64_t>(header.entryCount) * sizeof(ShaderPackEntryDisk);
    const std::uint64_t blobsEnd = static_cast<std::uint64_t>(header.blobsOffset) +
        static_cast<std::uint64_t>(header.blobCount) * sizeof(ShaderPackBlobDisk);
    if (header.entriesOffset < header.headerBytes || entriesEnd > header.blobsOffset ||
        header.blobsOffset < entriesEnd || blobsEnd > header.dataOffset || header.dataOffset > pack.size) {
        return Status::invalidArgument("Vulkan FSR4 shader pack index offsets are invalid");
    }
    if (header.entryCount == 0 || header.blobCount == 0) {
        return Status::invalidArgument("Vulkan FSR4 shader pack contains no shader entries");
    }

    ShaderPackEntryDisk previous{};
    bool havePrevious = false;
    for (std::uint32_t index = 0; index < header.entryCount; ++index) {
        ShaderPackEntryDisk entry{};
        if (!readShaderPackObject(pack, header.entriesOffset + static_cast<std::size_t>(index) * sizeof(entry), entry) ||
            entry.blobIndex >= header.blobCount) {
            return Status::invalidArgument("Vulkan FSR4 shader pack contains an invalid logical entry");
        }
        if (havePrevious) {
            VulkanFsr4ReferenceShaderKey previousKey{
                .pipelineVariant = static_cast<PipelineVariant>(previous.pipelineVariant),
                .preset = static_cast<Preset>(previous.preset),
                .frameCapacity = static_cast<FrameCapacity>(previous.frameCapacity),
                .permutationKey = previous.permutationKey,
                .passKind = static_cast<PassKind>(previous.passKind),
                .logicalStage = previous.logicalStage,
            };
            if (!shaderKeyLess(previous, VulkanFsr4ReferenceShaderKey{
                    .pipelineVariant = static_cast<PipelineVariant>(entry.pipelineVariant),
                    .preset = static_cast<Preset>(entry.preset),
                    .frameCapacity = static_cast<FrameCapacity>(entry.frameCapacity),
                    .permutationKey = entry.permutationKey,
                    .passKind = static_cast<PassKind>(entry.passKind),
                    .logicalStage = entry.logicalStage,
                }) || shaderKeyEqual(entry, previousKey)) {
                return Status::invalidArgument("Vulkan FSR4 shader pack logical entries are not strictly ordered");
            }
        }
        previous = entry;
        havePrevious = true;
    }

    constexpr std::uint32_t kSpirvMagic = 0x07230203u;
    constexpr std::uint32_t kSpirv15 = 0x00010500u;
    for (std::uint32_t index = 0; index < header.blobCount; ++index) {
        ShaderPackBlobDisk blob{};
        if (!readShaderPackObject(pack, header.blobsOffset + static_cast<std::size_t>(index) * sizeof(blob), blob) ||
            blob.offset < header.dataOffset || blob.size < 5u * sizeof(std::uint32_t) || (blob.size & 3u) != 0u ||
            static_cast<std::uint64_t>(blob.offset) + blob.size > pack.size) {
            return Status::invalidArgument("Vulkan FSR4 shader pack contains an invalid physical blob");
        }
        std::uint32_t words[2]{};
        std::memcpy(words, pack.data + blob.offset, sizeof(words));
        if (words[0] != kSpirvMagic || words[1] != kSpirv15) {
            return Status::invalidArgument("Vulkan FSR4 shader pack contains bytecode that is not SPIR-V 1.5");
        }
    }
    return Status::ok();
}

VulkanFsr4ReferenceShaderBlobView findVulkanFsr4ReferenceShaderBlob(
    const VulkanFsr4ReferenceShaderPackView pack,
    const VulkanFsr4ReferenceShaderKey& key) noexcept
{
    ShaderPackHeaderDisk header{};
    if (!readShaderPackObject(pack, 0, header) ||
        !std::equal(kShaderPackMagic.begin(), kShaderPackMagic.end(), header.magic) ||
        header.headerBytes != sizeof(ShaderPackHeaderDisk) || header.totalBytes != pack.size) {
        return {};
    }

    std::uint32_t low = 0;
    std::uint32_t high = header.entryCount;
    while (low < high) {
        const std::uint32_t mid = low + (high - low) / 2u;
        ShaderPackEntryDisk entry{};
        if (!readShaderPackObject(pack, header.entriesOffset + static_cast<std::size_t>(mid) * sizeof(entry), entry)) {
            return {};
        }
        if (shaderKeyLess(entry, key)) {
            low = mid + 1u;
        } else {
            high = mid;
        }
    }
    if (low >= header.entryCount) {
        return {};
    }
    ShaderPackEntryDisk entry{};
    if (!readShaderPackObject(pack, header.entriesOffset + static_cast<std::size_t>(low) * sizeof(entry), entry) ||
        !shaderKeyEqual(entry, key) || entry.blobIndex >= header.blobCount) {
        return {};
    }
    ShaderPackBlobDisk blob{};
    if (!readShaderPackObject(pack, header.blobsOffset + static_cast<std::size_t>(entry.blobIndex) * sizeof(blob), blob) ||
        blob.offset > pack.size || blob.size > pack.size - blob.offset) {
        return {};
    }
    return VulkanFsr4ReferenceShaderBlobView{.data = pack.data + blob.offset, .size = blob.size};
}

const char* vulkanFsr4ReferenceDescriptorKindName(const VulkanFsr4ReferenceDescriptorKind kind)
{
    switch (kind) {
    case VulkanFsr4ReferenceDescriptorKind::Sampler: return "sampler";
    case VulkanFsr4ReferenceDescriptorKind::SampledImage: return "sampled-image";
    case VulkanFsr4ReferenceDescriptorKind::StorageImage: return "storage-image";
    case VulkanFsr4ReferenceDescriptorKind::UniformBuffer: return "uniform-buffer";
    case VulkanFsr4ReferenceDescriptorKind::StorageBuffer: return "storage-buffer";
    }
    return "unknown";
}

std::unique_ptr<VulkanFsr4ReferenceRuntime> createVulkanFsr4ReferenceRuntime()
{
    return std::make_unique<VulkanFsr4ReferenceRuntimeImpl>();
}

} // namespace apusr::backends
