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
#include <unknwn.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3d12.h>

#if defined(APUSR_STREAMLINE_ATTACH_MODE)
#include <detours.h>
#endif

#if defined(APUSR_STREAMLINE_ATTACH_MODE)
#define APUSR_SL_EXPORT extern "C"
#else
#define APUSR_SL_EXPORT extern "C" __declspec(dllexport)
#endif

#include "../identity_spoof/apusr_identity_spoof_hooks.h"
#include "../common/apusr_host_discovery_log.h"
#include "../vulkan_host_capture/apusr_vulkan_host_capture_abi.h"
#include "apusr_streamline_core_abi.h"
#include "apusr_streamline_dlssg_abi.h"
#include "apusr_streamline_feature_ownership.h"
#include "../../framegen/ffx_vk/apusr_framegen_direct_wsi_vk.h"
#include "../../framegen/ffx_vk/apusr_framegen_presentation_vk.h"
#include "../../framegen/ffx_vk/apusr_framegen_present_images_vk.h"
#include "../../framegen/ffx_vk/apusr_framegen_present_sync_vk.h"
#include "../../framegen/ffx_vk/apusr_framegen_source_replay_vk.h"
#include "../../framegen/ffx_vk/apusr_framegen_streamline_vk.h"
#include "../../framegen/ffx_dx12/antilag2/ffx_antilag2_dx12.h"
#include <apusr/apusr_profiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

#ifndef APUSR_SL_ABI_PROFILE_NAME
#define APUSR_SL_ABI_PROFILE_NAME "sl2730"
#endif
#ifndef APUSR_SL_STOCK_MAX_ORDINAL
#define APUSR_SL_STOCK_MAX_ORDINAL 262
#endif
#ifndef APUSR_SL_PRIVATE_FIRST_ORDINAL
#define APUSR_SL_PRIVATE_FIRST_ORDINAL 263
#endif
#ifndef APUSR_SL_EXPECTS_TAG_FOR_FRAME
#define APUSR_SL_EXPECTS_TAG_FOR_FRAME 1
#endif
#ifndef APUSR_SL_EXPECTED_VERSION_MAJOR
#define APUSR_SL_EXPECTED_VERSION_MAJOR 2
#endif
#ifndef APUSR_SL_EXPECTED_VERSION_MINOR
#define APUSR_SL_EXPECTED_VERSION_MINOR 7
#endif
#ifndef APUSR_SL_EXPECTED_VERSION_PATCH
#define APUSR_SL_EXPECTED_VERSION_PATCH 30
#endif
#ifndef APUSR_SL_EXPECTED_VERSION_BUILD
#define APUSR_SL_EXPECTED_VERSION_BUILD 0
#endif

namespace {

constexpr wchar_t kOriginalInterposerName[] = L"sl_interposer_apusr_original.dll";
constexpr std::uint32_t kFeatureDLSS = 0u;
constexpr std::uint32_t kFeatureReflex = 3u;
constexpr std::uint32_t kFeaturePCL = 4u;
constexpr std::uint32_t kFeatureDLSS_G = 1000u;
constexpr std::uint32_t kFeatureDLSS_RR = 1001u;

constexpr bool apusrStreamlineFeatureSupported(std::uint32_t feature) noexcept
{
    return feature == kFeatureDLSS || feature == kFeatureReflex || feature == kFeatureDLSS_G;
}

constexpr bool apusrStreamlineFeatureExplicitlyUnsupported(std::uint32_t feature) noexcept
{
    return feature == kFeatureDLSS_RR;
}

constexpr bool apusrStreamlineFeatureShouldNoopIfSelected(std::uint32_t feature) noexcept
{
    return apusrStreamlineFeatureExplicitlyUnsupported(feature);
}

constexpr int kResultOk = 0;
constexpr int kResultAdapterNotSupported = 7;  // sl::Result::eErrorAdapterNotSupported
constexpr int kResultFeatureMissing = 31;       // sl::Result::eErrorFeatureMissing
constexpr int kResultFeatureNotSupported = 32;  // sl::Result::eErrorFeatureNotSupported
constexpr int kResultFeatureFailedToLoad = 34;  // sl::Result::eErrorFeatureFailedToLoad
constexpr int kResultInvalidParameter = 25;     // sl::Result::eErrorInvalidParameter
constexpr int kResultMissingInputParameter = 20; // sl::Result::eErrorMissingInputParameter
constexpr UINT kSpoofVendorNvidia = 0x10DEu;
constexpr UINT kSpoofDeviceRTX3090 = 0x2204u;
constexpr UINT kVendorMicrosoft = 0x1414u;
constexpr UINT64 kSpoofDedicatedVideoMemory = 16ull * 1024ull * 1024ull * 1024ull;

using StructType = apusr::streamline::core_abi::StructType;
using BaseStructure = apusr::streamline::core_abi::BaseStructure;

constexpr StructType kSlSubresourceRangeStructType{
    0x8d4c316cU, 0xd402U, 0x4524U, {0x89U, 0xa7U, 0x14U, 0xe7U, 0x9eU, 0x63U, 0x8eU, 0x3aU}};

constexpr StructType kSlVulkanInfoStructType{
    0x0eed6fd5U, 0x82cdU, 0x43a9U, {0xbdU, 0xb5U, 0x47U, 0xa5U, 0xbaU, 0x2fU, 0x45U, 0xd6U}};
constexpr std::size_t kSlVulkanInfoStructVersion = 3u;

// The leading layout matches Streamline's current sl::VulkanInfo. Queue
// family/index fields let queue acquisition be classified without wrapping the
// application's physical-device discovery path.
struct SlVulkanInfo : BaseStructure {
    void* device;
    void* instance;
    void* physicalDevice;
    std::uint32_t computeQueueIndex;
    std::uint32_t computeQueueFamily;
    std::uint32_t graphicsQueueIndex;
    std::uint32_t graphicsQueueFamily;
    std::uint32_t opticalFlowQueueIndex;
    std::uint32_t opticalFlowQueueFamily;
    bool useNativeOpticalFlowMode;
    std::uint32_t computeQueueCreateFlags;
    std::uint32_t graphicsQueueCreateFlags;
    std::uint32_t opticalFlowQueueCreateFlags;
};

// Streamline Preferences v1 layout through renderAPI. We only inspect this for
// the modern SDK ABI (identified by kSDKVersion's 0xfedc low word), so legacy
// Streamline hosts are never read through a newer structure layout.
struct SlPreferences : BaseStructure {
    bool showConsole;
    std::uint32_t logLevel;
    const wchar_t** pathsToPlugins;
    std::uint32_t numPathsToPlugins;
    const wchar_t* pathToLogsAndData;
    void* allocateCallback;
    void* releaseCallback;
    void* logMessageCallback;
    std::uint64_t flags;
    const std::uint32_t* featuresToLoad;
    std::uint32_t numFeaturesToLoad;
    std::uint32_t applicationId;
    std::uint32_t engine;
    const char* engineVersion;
    const char* projectId;
    std::uint32_t renderAPI;
};

constexpr std::uint32_t kSlRenderApiD3d11 = 0u;
constexpr std::uint32_t kSlRenderApiD3d12 = 1u;
constexpr std::uint32_t kSlRenderApiVulkan = 2u;
constexpr std::uint32_t kSlRenderApiUnknown = 0xffffffffu;

struct ViewportHandle : BaseStructure {
    std::uint32_t value;
};

struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t build;
};

struct FeatureRequirements : BaseStructure {
    std::uint32_t flags;
    std::uint32_t maxNumCPUThreads;
    std::uint32_t maxNumViewports;
    std::uint32_t numRequiredTags;
    const std::uint32_t* requiredTags;
    Version osVersionDetected;
    Version osVersionRequired;
    Version driverVersionDetected;
    Version driverVersionRequired;
    std::uint32_t vkNumComputeQueuesRequired;
    std::uint32_t vkNumGraphicsQueuesRequired;
    std::uint32_t vkNumDeviceExtensions;
    const char** vkDeviceExtensions;
    std::uint32_t vkNumInstanceExtensions;
    const char** vkInstanceExtensions;
    std::uint32_t vkNumFeatures12;
    const char** vkFeatures12;
    std::uint32_t vkNumFeatures13;
    const char** vkFeatures13;
    std::uint32_t vkNumOpticalFlowQueuesRequired;
};

static_assert(sizeof(BaseStructure) == 32, "Streamline x64 BaseStructure ABI drift");
static_assert(sizeof(SlPreferences) == 144, "Streamline x64 Preferences ABI drift");
static_assert(sizeof(SlVulkanInfo) == 96, "Streamline x64 VulkanInfo ABI drift");
static_assert(sizeof(FeatureRequirements) == 184, "Streamline x64 FeatureRequirements ABI drift");

struct FeatureVersion : BaseStructure {
    Version versionSL;
    Version versionNGX;
};

struct Extent {
    std::uint32_t left;
    std::uint32_t top;
    std::uint32_t width;
    std::uint32_t height;
};

struct SlFloat2 {
    float x;
    float y;
};

struct SlFloat3 {
    float x;
    float y;
    float z;
};

struct SlFloat4 {
    float x;
    float y;
    float z;
    float w;
};

struct SlFloat4x4 {
    SlFloat4 row[4];
};

// Streamline 2.x common constants layout through the fields APUSR consumes.
// Keeping this ABI mirror local avoids a build-time dependency on the SDK.
struct SlConstants : BaseStructure {
    SlFloat4x4 cameraViewToClip;
    SlFloat4x4 clipToCameraView;
    SlFloat4x4 clipToLensClip;
    SlFloat4x4 clipToPrevClip;
    SlFloat4x4 prevClipToClip;
    SlFloat2 jitterOffset;
    SlFloat2 mvecScale;
    SlFloat2 cameraPinholeOffset;
    SlFloat3 cameraPos;
    SlFloat3 cameraUp;
    SlFloat3 cameraRight;
    SlFloat3 cameraFwd;
    float cameraNear;
    float cameraFar;
    float cameraFOV;
    float cameraAspectRatio;
    float motionVectorsInvalidValue;
    char depthInverted;
    char cameraMotionIncluded;
    char motionVectors3D;
    char reset;
    char orthographicProjection;
    char motionVectorsDilated;
    char motionVectorsJittered;
    float minRelativeLinearDepthObjectSeparation;
};

struct SlResource : BaseStructure {
    char type;
    void* native;
    void* memory;
    void* view;
    std::uint32_t state;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t nativeFormat;
    std::uint32_t mipLevels;
    std::uint32_t arrayLayers;
    std::uint64_t gpuVirtualAddress;
    std::uint32_t flags;
    std::uint32_t usage;
    std::uint32_t reserved;
};

struct SlSubresourceRange : BaseStructure {
    std::uint32_t aspectMask;
    std::uint32_t baseMipLevel;
    std::uint32_t levelCount;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};

struct SlResourceTag : BaseStructure {
    SlResource* resource;
    std::uint32_t type;
    std::uint32_t lifecycle;
    Extent extent;
};

static_assert(sizeof(SlConstants) == 456, "Streamline x64 Constants ABI drift");
static_assert(offsetof(SlConstants, jitterOffset) == 352, "Streamline Constants jitter ABI drift");
static_assert(offsetof(SlConstants, motionVectorsJittered) == 450, "Streamline Constants tail ABI drift");
static_assert(sizeof(SlResource) == 112, "Streamline x64 Resource ABI drift");
static_assert(sizeof(SlSubresourceRange) == 56, "Streamline x64 SubresourceRange ABI drift");
static_assert(offsetof(SlResource, native) == 40, "Streamline Resource native ABI drift");
static_assert(offsetof(SlResource, state) == 64, "Streamline Resource state ABI drift");
static_assert(offsetof(SlResource, reserved) == 104, "Streamline Resource tail ABI drift");
static_assert(sizeof(SlResourceTag) == 64, "Streamline x64 ResourceTag ABI drift");

struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Handle;

struct NVSDK_NGX_Parameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;
    virtual std::uint32_t Get(const char* name, unsigned long long* outValue) const = 0;
    virtual std::uint32_t Get(const char* name, float* outValue) const = 0;
    virtual std::uint32_t Get(const char* name, double* outValue) const = 0;
    virtual std::uint32_t Get(const char* name, unsigned int* outValue) const = 0;
    virtual std::uint32_t Get(const char* name, int* outValue) const = 0;
    virtual std::uint32_t Get(const char* name, ID3D11Resource** outValue) const = 0;
    virtual std::uint32_t Get(const char* name, ID3D12Resource** outValue) const = 0;
    virtual std::uint32_t Get(const char* name, void** outValue) const = 0;
    virtual void Reset() = 0;
};

enum class DLSSMode : std::uint32_t {
    eOff = 0,
    eMaxPerformance = 1,
    eBalanced = 2,
    eMaxQuality = 3,
    eUltraPerformance = 4,
    eUltraQuality = 5,
    eDLAA = 6,
};

struct DLSSOptions : BaseStructure {
    DLSSMode mode;
    std::uint32_t outputWidth;
    std::uint32_t outputHeight;
    float sharpness;
    float preExposure;
    float exposureScale;
    char colorBuffersHDR;
    char indicatorInvertAxisX;
    char indicatorInvertAxisY;
    std::uint32_t dlaaPreset;
    std::uint32_t qualityPreset;
    std::uint32_t balancedPreset;
    std::uint32_t performancePreset;
    std::uint32_t ultraPerformancePreset;
    std::uint32_t ultraQualityPreset;
    char useAutoExposure;
    char alphaUpscalingEnabled;
};

static_assert(sizeof(DLSSOptions) == 88, "Streamline x64 DLSSOptions ABI drift");
static_assert(offsetof(DLSSOptions, useAutoExposure) == 84, "Streamline DLSSOptions v3 ABI drift");

struct DLSSOptimalSettings : BaseStructure {
    std::uint32_t optimalRenderWidth;
    std::uint32_t optimalRenderHeight;
    float optimalSharpness;
    std::uint32_t renderWidthMin;
    std::uint32_t renderHeightMin;
    std::uint32_t renderWidthMax;
    std::uint32_t renderHeightMax;
};

struct DLSSState : BaseStructure {
    std::uint64_t estimatedVRAMUsageInBytes;
};

enum class DLSSGMode : std::uint32_t {
    eOff = 0,
    eOn = 1,
    eAuto = 2,
    eDynamic = 3,
};

enum class DLSSGQueueParallelismMode : std::uint32_t {
    eBlockPresentingClientQueue = 0,
    eBlockNoClientQueues = 1,
};

enum : std::uint32_t {
    kDlssgStatusOk = 0u,
    kDlssgStatusResolutionTooLow = 1u << 0,
    kDlssgStatusReflexNotDetected = 1u << 1,
    kDlssgStatusHdrUnsupported = 1u << 2,
    kDlssgStatusConstantsInvalid = 1u << 3,
};

struct DLSSGOptions : BaseStructure {
    DLSSGMode mode = DLSSGMode::eOff;
    std::uint32_t numFramesToGenerate = 1u;
    std::uint32_t flags = 0u;
    std::uint32_t dynamicResWidth = 0u;
    std::uint32_t dynamicResHeight = 0u;
    std::uint32_t numBackBuffers = 0u;
    std::uint32_t mvecDepthWidth = 0u;
    std::uint32_t mvecDepthHeight = 0u;
    std::uint32_t colorWidth = 0u;
    std::uint32_t colorHeight = 0u;
    std::uint32_t colorBufferFormat = 0u;
    std::uint32_t mvecBufferFormat = 0u;
    std::uint32_t depthBufferFormat = 0u;
    std::uint32_t hudLessBufferFormat = 0u;
    std::uint32_t uiBufferFormat = 0u;
    void* onErrorCallback = nullptr;
    char bReserved15 = 2;
    DLSSGQueueParallelismMode queueParallelismMode = DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    char enableUserInterfaceRecomposition = 0;
    float dynamicTargetFrameRate = 0.0f;
};

struct DLSSGState : BaseStructure {
    std::uint64_t estimatedVRAMUsageInBytes = 0u;
    std::uint32_t status = kDlssgStatusOk;
    std::uint32_t minWidthOrHeight = 0u;
    std::uint32_t numFramesActuallyPresented = 0u;
    std::uint32_t numFramesToGenerateMax = 1u;
    char bReserved4 = 0;
    char bIsVsyncSupportAvailable = 0;
    void* inputsProcessingCompletionFence = nullptr;
    std::uint64_t lastPresentInputsProcessingCompletionFenceValue = 0u;
    char bIsDynamicMFGSupported = 0;
};

static_assert(sizeof(DLSSGOptions) == 120, "Streamline x64 DLSSGOptions ABI drift");
static_assert(offsetof(DLSSGOptions, onErrorCallback) == 96, "Streamline DLSSGOptions v5 ABI drift");
static_assert(offsetof(DLSSGOptions, queueParallelismMode) == 108, "Streamline DLSSGOptions v5 ABI drift");
static_assert(offsetof(DLSSGOptions, enableUserInterfaceRecomposition) == 112, "Streamline DLSSGOptions v5 ABI drift");
static_assert(sizeof(DLSSGState) == 88, "Streamline x64 DLSSGState ABI drift");
static_assert(offsetof(DLSSGState, inputsProcessingCompletionFence) == 64, "Streamline DLSSGState v4 ABI drift");

enum class ReflexMode : std::uint32_t {
    eOff = 0,
    eLowLatency = 1,
    eLowLatencyWithBoost = 2,
};

struct ReflexOptions : BaseStructure {
    ReflexMode mode = ReflexMode::eOff;
    std::uint32_t frameLimitUs = 0;
    bool useMarkersToOptimize = false;
    std::uint16_t virtualKey = 0;
    std::uint32_t idThread = 0;
};

struct ReflexStatePrefix : BaseStructure {
    bool lowLatencyAvailable = false;
    bool latencyReportAvailable = false;
    std::uint32_t statsWindowMessage = 0;
};

struct SlAdapterInfo : BaseStructure {
    std::uint8_t* deviceLuid = nullptr;
    std::uint32_t deviceLuidSizeInBytes = 0;
    void* vkPhysicalDevice = nullptr;
};

static_assert(sizeof(ReflexOptions) == 48, "Streamline x64 ReflexOptions ABI drift");
static_assert(offsetof(ReflexOptions, frameLimitUs) == 36, "Streamline ReflexOptions ABI drift");
static_assert(sizeof(ReflexStatePrefix) == 40, "Streamline x64 ReflexState prefix ABI drift");
static_assert(offsetof(ReflexStatePrefix, statsWindowMessage) == 36, "Streamline ReflexState prefix ABI drift");
static_assert(sizeof(SlAdapterInfo) == 56, "Streamline x64 AdapterInfo ABI drift");
static_assert(offsetof(SlAdapterInfo, vkPhysicalDevice) == 48, "Streamline AdapterInfo ABI drift");

float dlssScale(DLSSMode mode);

using SlSetD3DDeviceFn = int(__cdecl*)(void* d3dDevice);
using SlIsFeatureSupportedFn = int(__cdecl*)(std::uint32_t feature, const void* adapterInfo);
using SlIsFeatureLoadedFn = int(__cdecl*)(std::uint32_t feature, bool* loaded);
using SlSetFeatureLoadedFn = int(__cdecl*)(std::uint32_t feature, bool loaded);
using SlGetFeatureFunctionFn = int(__cdecl*)(std::uint32_t feature, const char* functionName, void** function);
using SlGetFeatureRequirementsFn = int(__cdecl*)(std::uint32_t feature, FeatureRequirements* requirements);
using SlGetFeatureVersionFn = int(__cdecl*)(std::uint32_t feature, FeatureVersion* version);
using SlAllocateResourcesFn = int(__cdecl*)(void* cmdBuffer, std::uint32_t feature, const ViewportHandle* viewport);
using SlFreeResourcesFn = int(__cdecl*)(std::uint32_t feature, const ViewportHandle* viewport);
using SlEvaluateFeatureFn = int(__cdecl*)(std::uint32_t feature, const void* frame, const BaseStructure* const* inputs, std::uint32_t numInputs, void* cmdBuffer);

using SlInitFn = int(__cdecl*)(const void* preferences, std::uint64_t sdkVersion);
using SlShutdownFn = int(__cdecl*)();
using SlGetNewFrameTokenFn = int(__cdecl*)(void** token, const std::uint32_t* frameIndex);
using SlSetConstantsFn = int(__cdecl*)(const void* values, const void* frame, const void* viewport);
using SlSetTagFn = int(__cdecl*)(const void* viewport, const void* tags, std::uint32_t numTags, void* cmdBuffer);
using SlSetTagForFrameFn = int(__cdecl*)(const void* frame, const void* viewport, const void* tags, std::uint32_t numTags, void* cmdBuffer);
using SlSetVulkanInfoFn = int(__cdecl*)(const void* info);
using SlUpgradeInterfaceFn = int(__cdecl*)(void** baseInterface);
using SlGetNativeInterfaceFn = int(__cdecl*)(void* proxyInterface, void** baseInterface);
using SlPclSetMarkerFn = int(__cdecl*)(std::uint32_t marker, const void* frame);

using CreateDXGIFactoryFn = HRESULT(WINAPI*)(REFIID riid, void** ppFactory);
using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID riid, void** ppFactory);
using CreateDXGIFactory2Fn = HRESULT(WINAPI*)(UINT flags, REFIID riid, void** ppFactory);
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(void* adapter,
                                             int driverType,
                                             HMODULE software,
                                             UINT flags,
                                             const void* featureLevels,
                                             UINT featureLevelsCount,
                                             UINT sdkVersion,
                                             void** device,
                                             void* featureLevel,
                                             void** immediateContext);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(void* adapter,
                                                         int driverType,
                                                         HMODULE software,
                                                         UINT flags,
                                                         const void* featureLevels,
                                                         UINT featureLevelsCount,
                                                         UINT sdkVersion,
                                                         const void* swapChainDesc,
                                                         void** swapChain,
                                                         void** device,
                                                         void* featureLevel,
                                                         void** immediateContext);
using D3D12CreateDeviceFn = HRESULT(WINAPI*)(void* adapter, int minimumFeatureLevel, REFIID riid, void** device);
using D3D12GetDebugInterfaceFn = HRESULT(WINAPI*)(REFIID riid, void** debugInterface);
using D3D12GetInterfaceFn = HRESULT(WINAPI*)(REFCLSID clsid, REFIID riid, void** outInterface);
using D3D12EnableExperimentalFeaturesFn = HRESULT(WINAPI*)(UINT numFeatures,
                                                           const IID* featureIIDs,
                                                           void* configurationStructs,
                                                           UINT* configurationStructSizes);
using D3D12CreateRootSignatureDeserializerFn = HRESULT(WINAPI*)(const void* srcData,
                                                                SIZE_T srcDataSizeInBytes,
                                                                REFIID riid,
                                                                void** deserializer);
using D3D12CreateVersionedRootSignatureDeserializerFn = HRESULT(WINAPI*)(const void* srcData,
                                                                         SIZE_T srcDataSizeInBytes,
                                                                         REFIID riid,
                                                                         void** deserializer);
using D3D12SerializeRootSignatureFn = HRESULT(WINAPI*)(const void* rootSignature,
                                                       int version,
                                                       void** blob,
                                                       void** errorBlob);
using D3D12SerializeVersionedRootSignatureFn = HRESULT(WINAPI*)(const void* rootSignature,
                                                                void** blob,
                                                                void** errorBlob);
using DXGIGetDebugInterface1Fn = HRESULT(WINAPI*)(UINT flags, REFIID riid, void** debugInterface);

template <typename Fn>
Fn originalFn(const char* name);


// Keep the Streamline proxy independent of a Vulkan SDK checkout. The stable
// host-capture DLL owns Vulkan creation/queue discovery; this frontend only
// needs device-proc lookup for its resource-layout boundary.
using VkVoidFunction = void(WINAPI*)();
using VkGetInstanceProcAddrFn = VkVoidFunction(WINAPI*)(void* instance, const char* name);
using VkGetDeviceProcAddrFn = VkVoidFunction(WINAPI*)(void* device, const char* name);
using VkCreateDeviceFn = std::int32_t(WINAPI*)(void* physicalDevice, const void* createInfo, const void* allocator, void** device);
using VkQueuePresentKHRFn = std::int32_t(WINAPI*)(void* queue, const void* presentInfo);
constexpr std::int32_t kVkSuccess = 0;
constexpr std::int32_t kVkErrorInitializationFailed = -3;

// Vulkan 1.x x64 ABI prefix used at the frontend-owned present seam. Keep this
// local to the Streamline facade: the host-capture module owns Vulkan dispatch,
// while this layer owns translating a game's presentation into APUSR feature
// work before the backing Streamline present hook runs.
struct VkPresentInfoAbi {
    std::int32_t sType;
    std::uint32_t _pad0;
    const void* pNext;
    std::uint32_t waitSemaphoreCount;
    std::uint32_t _pad1;
    void* const* pWaitSemaphores;
    std::uint32_t swapchainCount;
    std::uint32_t _pad2;
    void* const* pSwapchains;
    const std::uint32_t* pImageIndices;
    std::int32_t* pResults;
};

static_assert(sizeof(VkPresentInfoAbi) == 64, "Vulkan x64 VkPresentInfoKHR ABI drift");
static_assert(offsetof(VkPresentInfoAbi, pWaitSemaphores) == 24, "VkPresentInfoKHR wait ABI drift");
static_assert(offsetof(VkPresentInfoAbi, pSwapchains) == 40, "VkPresentInfoKHR swapchain ABI drift");

std::atomic<void*> g_lastObservedPresentSwapchain{nullptr};

struct VkImageSubresourceRangeAbi {
    std::uint32_t aspectMask;
    std::uint32_t baseMipLevel;
    std::uint32_t levelCount;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};

struct VkImageMemoryBarrierAbi {
    std::uint32_t sType;
    const void* pNext;
    std::uint32_t srcAccessMask;
    std::uint32_t dstAccessMask;
    std::uint32_t oldLayout;
    std::uint32_t newLayout;
    std::uint32_t srcQueueFamilyIndex;
    std::uint32_t dstQueueFamilyIndex;
    void* image;
    VkImageSubresourceRangeAbi subresourceRange;
};

static_assert(sizeof(VkImageMemoryBarrierAbi) == 72, "Vulkan x64 image-barrier ABI drift");

using VkCmdPipelineBarrierFn = void(WINAPI*)(
    void* commandBuffer,
    std::uint32_t srcStageMask,
    std::uint32_t dstStageMask,
    std::uint32_t dependencyFlags,
    std::uint32_t memoryBarrierCount,
    const void* memoryBarriers,
    std::uint32_t bufferMemoryBarrierCount,
    const void* bufferMemoryBarriers,
    std::uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrierAbi* imageMemoryBarriers);

template <typename Fn>
Fn vulkanSystemFn(const char* name);

using NgxD3D12InitFn = std::uint32_t(__cdecl*)(unsigned long long applicationId, const wchar_t* applicationDataPath, void* device, const void* featureInfo, unsigned int sdkVersion);
using NgxAllocateParametersFn = std::uint32_t(__cdecl*)(NVSDK_NGX_Parameter** outParameters);
using NgxDestroyParametersFn = std::uint32_t(__cdecl*)(NVSDK_NGX_Parameter* parameters);
using NgxCreateFeatureFn = std::uint32_t(__cdecl*)(ID3D12GraphicsCommandList* commandList, unsigned int feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** outHandle);
using NgxEvaluateFeatureFn = std::uint32_t(__cdecl*)(ID3D12GraphicsCommandList* commandList, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters, void* callbacks);
using NgxReleaseFeatureFn = std::uint32_t(__cdecl*)(NVSDK_NGX_Handle* handle);
using NgxVulkanInitFn = std::uint32_t(__cdecl*)(unsigned long long applicationId, const wchar_t* applicationDataPath, void* instance, void* physicalDevice, void* device);
using NgxVulkanCreateFeatureFn = std::uint32_t(__cdecl*)(void* commandBuffer, unsigned int feature, NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** outHandle);
using NgxVulkanEvaluateFeatureFn = std::uint32_t(__cdecl*)(void* commandBuffer, const NVSDK_NGX_Handle* handle, NVSDK_NGX_Parameter* parameters, void* callbacks);

HMODULE g_original = nullptr;
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
struct AttachedStreamlineExport {
    const char* name = nullptr;
    PVOID trampoline = nullptr;
};

// Named exports are the stable attachment boundary. The backing module is always
// the exact stock interposer PE, whether it is loader-visible under the canonical
// name or preserved behind the optional loader-only bootstrap. Detours rewrites
// only functions that exist in the loaded revision. Optional additions
// such as slSetTagForFrame therefore need no version profile.
std::array<AttachedStreamlineExport, 18> g_attachedStreamlineExports{{
    {"slInit", nullptr},
    {"slShutdown", nullptr},
    {"slSetConstants", nullptr},
    {"slSetTag", nullptr},
    {"slSetTagForFrame", nullptr},
    {"slSetVulkanInfo", nullptr},
    {"slSetD3DDevice", nullptr},
    {"slGetNativeInterface", nullptr},
    {"slIsFeatureSupported", nullptr},
    {"slIsFeatureLoaded", nullptr},
    {"slSetFeatureLoaded", nullptr},
    {"slGetFeatureFunction", nullptr},
    {"slGetFeatureRequirements", nullptr},
    {"slGetFeatureVersion", nullptr},
    {"slAllocateResources", nullptr},
    {"slFreeResources", nullptr},
    {"slEvaluateFeature", nullptr},
    {"vkCreateDevice", nullptr},
}};
std::mutex g_streamlineAttachMutex;
std::atomic<bool> g_streamlineAttachInstalled{false};
std::atomic<std::uint32_t> g_streamlineAttachHookCount{0u};
HMODULE g_attachedStreamlineModule = nullptr;
HMODULE g_streamlineAttachRequestedModule = nullptr;
HMODULE g_attachedStreamlineOwnedModuleRef = nullptr;

PVOID attachedStreamlineTrampoline(const char* name) noexcept
{
    if (name == nullptr) {
        return nullptr;
    }
    for (const auto& entry : g_attachedStreamlineExports) {
        if (entry.name != nullptr && std::strcmp(entry.name, name) == 0) {
            return entry.trampoline;
        }
    }
    return nullptr;
}
#endif
HMODULE g_systemDxgi = nullptr;
HMODULE g_systemD3d11 = nullptr;
HMODULE g_systemD3d12 = nullptr;
HMODULE g_systemVulkan = nullptr;
HMODULE g_ngx = nullptr;
std::atomic<bool> g_dlssLoaded{true};
std::atomic<bool> g_legacySlBoolApi{false};
std::atomic<bool> g_slInitSeen{false};
std::atomic<bool> g_proxyAbiLogged{false};
std::atomic<std::uint32_t> g_slRenderApi{kSlRenderApiUnknown};
std::atomic<std::uint32_t> g_hostSlMajor{2u};
std::atomic<std::uint32_t> g_hostSlMinor{9u};
std::atomic<std::uint32_t> g_hostSlBuild{0u};
std::atomic<std::uint64_t> g_streamlineDlssEvaluateCalls{0u};
std::atomic<std::uint32_t> g_lastStreamlineNgxFailure{0u};
void* g_d3dDevice = nullptr;
std::mutex g_adapterProxyMutex;
std::unordered_map<void*, IUnknown*> g_adapterProxyToInner;


struct VulkanQueueHints {
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    std::uint32_t graphicsFamily = 0;
    std::uint32_t graphicsIndex = 0;
    std::uint32_t computeFamily = 0;
    std::uint32_t computeIndex = 0;
    std::uint32_t deviceRequirementFlags = 0;
    bool valid = false;
};

std::mutex g_vulkanCaptureMutex;
std::unordered_map<void*, VulkanQueueHints> g_vulkanQueueHintsByDevice;
thread_local std::uint32_t g_vulkanDlssEvaluateDepth = 0;
std::atomic<void*> g_currentVulkanDevice{nullptr};
std::atomic<bool> g_reflexLoaded{false};
std::atomic<std::uint32_t> g_reflexMode{APUSR_VK_HOST_ANTI_LAG_OFF};
std::atomic<std::uint32_t> g_reflexMaxFps{0u};
std::mutex g_d3d12AntiLagMutex;
AMD::AntiLag2DX12::Context g_d3d12AntiLagContext{};
ID3D12Device* g_d3d12AntiLagDevice = nullptr;
std::atomic<bool> g_d3d12AntiLagProbed{false};
std::atomic<bool> g_d3d12AntiLagAvailable{false};
const void* g_d3d12AntiLagLastInputFrame = nullptr;
const void* g_d3d12AntiLagLastRenderEndFrame = nullptr;
std::atomic<bool> g_dlssgLoaded{false};
std::atomic<bool> g_dlssgEnabled{false};
std::atomic<std::uint32_t> g_dlssgPresentedSinceQuery{0u};
std::atomic<std::uint32_t> g_dlssgLastStatus{kDlssgStatusOk};
std::atomic<bool> g_dlssgFirstGeneratedLogged{false};
std::atomic<bool> g_dlssgWaitingForPclLogged{false};
std::atomic<bool> g_dlssgFirstPclPresentLogged{false};
std::atomic<bool> g_pclMarkerFunctionLogged{false};
std::atomic<bool> g_pclUnresolvedFrameLogged{false};
std::atomic<std::uint64_t> g_dlssgStateLogSignature{~std::uint64_t{0}};
std::atomic<SlPclSetMarkerFn> g_backingPclSetMarker{nullptr};

constexpr std::uint32_t kHostQueueKindUnknown = 0u;
constexpr std::uint32_t kHostQueueKindDirect = 1u;
constexpr std::uint32_t kHostQueueKindCompute = 2u;

constexpr std::uint32_t kRequiredDlssTags[] = {0u, 1u, 3u, 4u};
constexpr std::uint32_t kBufferTypeDepth = 0u;
constexpr std::uint32_t kBufferTypeMotionVectors = 1u;
constexpr std::uint32_t kBufferTypeHUDLessColor = 2u;
constexpr std::uint32_t kBufferTypeScalingInputColor = 3u;
constexpr std::uint32_t kBufferTypeScalingOutputColor = 4u;
constexpr std::uint32_t kBufferTypeExposure = 13u;
constexpr std::uint32_t kBufferTypeUIColorAndAlpha = 23u;
constexpr std::uint32_t kBufferTypeBiasCurrentColorHint = 29u;
constexpr std::uint32_t kBufferTypeReactiveMaskHint = 36u;
constexpr std::uint32_t kBufferTypeBidirectionalDistortionField = 50u;
constexpr std::uint32_t kBufferTypeBackbuffer = 53u;
constexpr std::uint32_t kBufferTypeUIAlpha = 68u;
constexpr std::uint32_t kNgxFeatureSuperSampling = 1u;
constexpr std::uint32_t kNgxSuccess = 0x00000001u;

struct StreamlineViewportKey {
    const void* frame = nullptr;
    std::uint32_t viewport = 0;

    [[nodiscard]] bool operator==(const StreamlineViewportKey& other) const noexcept
    {
        return frame == other.frame && viewport == other.viewport;
    }
};

struct StreamlineViewportKeyHash {
    [[nodiscard]] std::size_t operator()(const StreamlineViewportKey& key) const noexcept
    {
        return (reinterpret_cast<std::size_t>(key.frame) >> 4u) ^
               (static_cast<std::size_t>(key.viewport) * 0x9e3779b97f4a7c15ull);
    }
};

struct CapturedResource {
    bool tagged = false;
    std::uint32_t lifecycle = 0u;
    void* native = nullptr;
    void* memory = nullptr;
    void* view = nullptr;
    std::uint32_t state = 0xffffffffu;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t nativeFormat = 0;
    std::uint32_t mipLevels = 0;
    std::uint32_t arrayLayers = 0;
    std::uint32_t flags = 0;
    std::uint32_t usage = 0;
    std::uint32_t extentLeft = 0;
    std::uint32_t extentTop = 0;
    std::uint32_t extentWidth = 0;
    std::uint32_t extentHeight = 0;
    std::uint32_t subresourceAspectMask = 0;
    std::uint32_t subresourceBaseMipLevel = 0;
    std::uint32_t subresourceLevelCount = 0;
    std::uint32_t subresourceBaseArrayLayer = 0;
    std::uint32_t subresourceLayerCount = 0;
    bool hasSubresourceRange = false;
};

std::uint32_t activeWidth(const CapturedResource& resource) noexcept
{
    return resource.extentWidth != 0u ? resource.extentWidth : resource.width;
}

std::uint32_t activeHeight(const CapturedResource& resource) noexcept
{
    return resource.extentHeight != 0u ? resource.extentHeight : resource.height;
}

bool vulkanStorageCapable(const CapturedResource& resource) noexcept
{
    constexpr std::uint32_t kVkImageUsageStorageBit = 0x00000008u;
    return (resource.usage & kVkImageUsageStorageBit) != 0u;
}

VkImageSubresourceRangeAbi vulkanSubresourceRange(const CapturedResource& resource) noexcept;

struct CapturedTags {
    CapturedResource color;
    CapturedResource depth;
    CapturedResource motionVectors;
    CapturedResource output;
    CapturedResource exposure;
    CapturedResource reactive;
    CapturedResource hudlessColor;
    CapturedResource uiColorAndAlpha;
    CapturedResource uiAlpha;
    CapturedResource distortionField;
    CapturedResource backbuffer;
};

struct CapturedOptions {
    DLSSMode mode = DLSSMode::eBalanced;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    float sharpness = 0.0f;
    float preExposure = 1.0f;
    float exposureScale = 1.0f;
    bool highDynamicRange = false;
    bool useAutoExposure = false;
};

struct CapturedConstants {
    bool valid = false;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    bool reset = false;
    bool depthInverted = false;
    bool motionVectorsJittered = false;
    bool cameraMotionIncluded = true;
    bool motionVectorsDilated = false;
    bool orthographicProjection = false;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float cameraFOV = 0.0f;
    float cameraAspectRatio = 0.0f;
    float cameraPosition[3]{};
    float cameraUp[3]{};
    float cameraRight[3]{};
    float cameraForward[3]{};
};

std::mutex g_streamlineStateMutex;
std::unordered_map<std::uint32_t, CapturedOptions> g_optionsByViewport;
std::unordered_map<StreamlineViewportKey, CapturedTags, StreamlineViewportKeyHash> g_tagsByFrameViewport;
std::unordered_map<std::uint32_t, CapturedTags> g_legacyTagsByViewport;
std::unordered_map<StreamlineViewportKey, CapturedConstants, StreamlineViewportKeyHash> g_constantsByFrameViewport;
std::unordered_map<std::uint32_t, CapturedConstants> g_constantsByViewport;

struct PendingFramegenPresent {
    const void* frame = nullptr;
    std::uint64_t frameIndex = 0u;
    std::int64_t presentStartQpc = 0;
};

constexpr std::size_t kPendingFramegenPresentCount = 16u;

struct DlssgControlState {
    bool configured = false;
    std::uint32_t viewport = 0u;
    DLSSGOptions options{};
};

struct VulkanFramegenRuntime {
    void* swapchain = nullptr;
    void* device = nullptr;
    void* applicationQueue = nullptr;
    void* inputQueue = nullptr; // private capture/preparation queue when available
    void* queue = nullptr; // private FI/OF generation queue
    void* presentationQueue = nullptr;
    void* replayQueue = nullptr; // transfer-only DMA queue when reserved, else presentation queue
    void* imageAcquireQueue = nullptr; // host-capture-owned virtual-acquire signal queue
    std::uint32_t inputQueueFamilyIndex = 0u;
    std::uint32_t generationQueueFamilyIndex = 0u;
    std::uint32_t replayQueueFamilyIndex = 0u;
    ApusrVulkanHostSwapchainInfo swapchainInfo{};
    std::vector<void*> swapchainImages; // application-visible virtual backbuffers
    std::vector<void*> realSwapchainImages; // private WSI images
    ApusrFramegenVulkanDeviceContext deviceContext{};
    ApusrFramegenVulkanPresentImageContext* presentImages = nullptr;
    ApusrFramegenVulkanPresentSubmitContext* presentSubmit = nullptr;
    ApusrFramegenVulkanDirectWsiContext* directWsi = nullptr;
    // Generated replay is independently double-buffered to match the provider
    // output ring. A single replay context forced the presenter to host-wait on
    // the previous generated copy before it could even prepare the next one;
    // profiling showed that serialization reaching double-digit milliseconds.
    std::array<ApusrFramegenVulkanSourceReplayContext*, APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS>
        generatedReplay{};
    ApusrFramegenVulkanSourceReplayContext* sourceReplay = nullptr;
    // Source prearm runs on the game thread while generated fallback replay is
    // owned by the presenter. Both can target replayQueue, so their submits
    // share one explicit external-synchronization gate.
    std::unique_ptr<std::mutex> replayQueueSubmitMutex;
    // Heavy FI/OF work is intentionally bounded to one in-flight generation.
    // A source frame is never queued behind an already-running generation.
    std::uint64_t lastSubmittedGenerationCompletionValue = 0u;
    // Generated output is independently double-buffered from the virtual
    // backbuffer ring. This cursor is only touched by the application-present
    // thread while g_vulkanFramegenMutex is held.
    std::uint32_t nextGeneratedOutputSlot = 0u;
    // Counts every application source present while Vulkan FG is enabled, not
    // merely the subset for which heavy FI/OF work is submitted. FidelityFX
    // frame IDs therefore expose skipped source frames as disjoint history.
    std::uint64_t sourceFrameSequence = 0u;
    std::uint64_t lastGenerationSourceSequence = 0u;
    std::uint64_t generationLatencyLoggedValue = 0u;
    std::int64_t generationSubmitQpc = 0;
    void* inputsProcessingCompletionFence = nullptr;
    std::uint64_t lastPresentInputsProcessingCompletionFenceValue = 0u;
    ApusrFramegenVulkanPresentation* presentation = nullptr;
    ApusrFramegenVulkanSession* session = nullptr;
    ApusrFramegenVulkanSessionDescription sessionDescription{};
    std::int64_t qpcFrequency = 0;
    std::int64_t previousApplicationPresentQpc = 0;
    std::int64_t previousPclPresentStartQpc = 0;
};

std::mutex g_vulkanFramegenMutex;
std::mutex g_vulkanFramegenSharedQueueMutex;
std::atomic<std::uint32_t> g_vulkanFramegenSlowPresentLogs{0u};
std::atomic<std::uint32_t> g_vulkanFramegenSlowReplayLogs{0u};
std::atomic<bool> g_vulkanFramegenInputContractLogged{false};
std::atomic<bool> g_vulkanFramegenHudlessRetainedLogged{false};
std::atomic<bool> g_vulkanFramegenGeneratedReplayLogged{false};
std::atomic<bool> g_vulkanFramegenSourceReplayLogged{false};
std::array<std::atomic<std::uint64_t>, APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS>
    g_vulkanFramegenGeneratedSlotGenerationValue{};
std::array<std::atomic<std::uint64_t>, APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS>
    g_vulkanFramegenGeneratedSlotDecisionValue{};
std::array<std::atomic<std::uint64_t>, APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS>
    g_vulkanFramegenGeneratedSlotReplayValue{};
std::atomic<std::uint64_t> g_vulkanFramegenSkippedBusyCount{0u};
std::atomic<std::uint64_t> g_vulkanFramegenSkippedDeadlineCount{0u};
DlssgControlState g_dlssgControl{};
std::array<PendingFramegenPresent, kPendingFramegenPresentCount> g_pendingFramegenPresents{};
std::size_t g_pendingFramegenPresentHead = 0u;
std::size_t g_pendingFramegenPresentSize = 0u;
VulkanFramegenRuntime g_vulkanFramegenRuntime{};
thread_local bool g_vulkanFramegenBackingPresent = false;

[[nodiscard]] bool isVulkanStreamlinePath() noexcept;
[[nodiscard]] bool isD3d12StreamlinePath() noexcept;
[[nodiscard]] bool resolveFrameToken(const void* token, std::uint64_t& frameIndex) noexcept;

[[nodiscard]] apusr::profile::StreamHandle dlssgProfileStream() noexcept
{
    static const apusr::profile::StreamHandle stream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Vulkan,
        .component = apusr::profile::Component::Frontend,
        .name = "streamline.dlssg",
        .capabilities = apusr::profile::StreamCapabilityCounters |
            apusr::profile::StreamCapabilityDecisions |
            apusr::profile::StreamCapabilityStateTransitions,
    });
    return stream;
}

[[nodiscard]] apusr::profile::StreamHandle reflexProfileStream() noexcept
{
    if (!apusr::profile::enabled())
        return {};
    if (isD3d12StreamlinePath()) {
        static const apusr::profile::StreamHandle stream = apusr::profile::registerStream({
            .backend = apusr::profile::BackendApi::Dx12,
            .component = apusr::profile::Component::Frontend,
            .name = "streamline.reflex",
            .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                apusr::profile::StreamCapabilityCounters |
                apusr::profile::StreamCapabilityStateTransitions,
        });
        return stream;
    }
    if (isVulkanStreamlinePath()) {
        static const apusr::profile::StreamHandle stream = apusr::profile::registerStream({
            .backend = apusr::profile::BackendApi::Vulkan,
            .component = apusr::profile::Component::Frontend,
            .name = "streamline.reflex",
            .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                apusr::profile::StreamCapabilityCounters |
                apusr::profile::StreamCapabilityStateTransitions,
        });
        return stream;
    }
    return {};
}

[[nodiscard]] std::uint64_t reflexProfileFrameId(const void* frame) noexcept
{
    if (!apusr::profile::cpuEnabled() || frame == nullptr)
        return 0u;
    std::uint64_t frameIndex = 0u;
    return resolveFrameToken(frame, frameIndex) ? frameIndex : 0u;
}

void recordDlssgDecision(
    const std::uint64_t frameId,
    const apusr::profile::DecisionOutcome outcome,
    const char* reason,
    const std::uint64_t sourceSequence = 0u,
    const std::uint64_t value1 = 0u,
    const std::uint64_t value2 = 0u,
    const std::uint64_t value3 = 0u,
    const std::uint32_t flags = 0u) noexcept
{
    const auto stream = dlssgProfileStream();
    if (!stream)
        return;
    apusr::profile::recordDecision({
        .stream = stream,
        .frameId = frameId,
        .correlationId = sourceSequence,
        .name = "framegen.source_disposition",
        .reason = reason,
        .outcome = outcome,
        .value0 = sourceSequence,
        .value1 = value1,
        .value2 = value2,
        .value3 = value3,
        .flags = flags,
    });
}

std::mutex g_streamlineNgxMutex;
NVSDK_NGX_Handle* g_streamlineNgxFeature = nullptr;
std::uint32_t g_streamlineNgxRenderWidth = 0;
std::uint32_t g_streamlineNgxRenderHeight = 0;
std::uint32_t g_streamlineNgxOutputWidth = 0;
std::uint32_t g_streamlineNgxOutputHeight = 0;
DLSSMode g_streamlineNgxMode = DLSSMode::eBalanced;
NgxD3D12InitFn g_streamlineNgxInitFn = nullptr;
NgxAllocateParametersFn g_streamlineNgxAllocateParametersFn = nullptr;
NgxDestroyParametersFn g_streamlineNgxDestroyParametersFn = nullptr;
NgxCreateFeatureFn g_streamlineNgxCreateFeatureFn = nullptr;
NgxEvaluateFeatureFn g_streamlineNgxEvaluateFeatureFn = nullptr;
NgxReleaseFeatureFn g_streamlineNgxReleaseFeatureFn = nullptr;
NVSDK_NGX_Parameter* g_streamlineNgxEvalParams = nullptr;
NgxVulkanInitFn g_streamlineNgxVulkanInitFn = nullptr;
NgxAllocateParametersFn g_streamlineNgxVulkanAllocateParametersFn = nullptr;
NgxDestroyParametersFn g_streamlineNgxVulkanDestroyParametersFn = nullptr;
NgxVulkanCreateFeatureFn g_streamlineNgxVulkanCreateFeatureFn = nullptr;
NgxVulkanEvaluateFeatureFn g_streamlineNgxVulkanEvaluateFeatureFn = nullptr;
NgxReleaseFeatureFn g_streamlineNgxVulkanReleaseFeatureFn = nullptr;


std::uint32_t viewportValue(const void* viewport) noexcept
{
    if (viewport == nullptr) {
        return 0;
    }
    return static_cast<const ViewportHandle*>(viewport)->value;
}

void traceDebug(const char* message)
{
#if defined(_WIN32)
    if (message != nullptr) {
        OutputDebugStringA("[APUSR Streamline] ");
        OutputDebugStringA(message);
        OutputDebugStringA("\n");
    }
#else
    (void)message;
#endif
}

std::uint32_t apusrQueueKindForHost(const std::uint32_t captureKind) noexcept
{
    if (captureKind == APUSR_VK_HOST_QUEUE_GRAPHICS) {
        return kHostQueueKindDirect;
    }
    if (captureKind == APUSR_VK_HOST_QUEUE_COMPUTE) {
        return kHostQueueKindCompute;
    }
    return kHostQueueKindUnknown;
}

std::uint32_t captureQueueKindForHost(const std::uint32_t preferredKind) noexcept
{
    if (preferredKind == kHostQueueKindDirect) {
        return APUSR_VK_HOST_QUEUE_GRAPHICS;
    }
    if (preferredKind == kHostQueueKindCompute) {
        return APUSR_VK_HOST_QUEUE_COMPUTE;
    }
    return APUSR_VK_HOST_QUEUE_UNKNOWN;
}

void captureVulkanInfo(const void* rawInfo)
{
    if (rawInfo == nullptr) {
        return;
    }
    const auto& info = *static_cast<const SlVulkanInfo*>(rawInfo);
    if (info.device == nullptr) {
        return;
    }

    apusrVulkanHostAssociateInstance(info.device, info.instance, info.physicalDevice);
    const std::uint32_t requirementFlags = apusrVulkanHostDeviceNegotiationFlags(info.device);
    {
        std::lock_guard<std::mutex> lock(g_vulkanCaptureMutex);
        g_vulkanQueueHintsByDevice[info.device] = VulkanQueueHints{
            .instance = info.instance,
            .physicalDevice = info.physicalDevice,
            .graphicsFamily = info.graphicsQueueFamily,
            .graphicsIndex = info.graphicsQueueIndex,
            .computeFamily = info.computeQueueFamily,
            .computeIndex = info.computeQueueIndex,
            .deviceRequirementFlags = requirementFlags,
            .valid = true,
        };
        g_currentVulkanDevice.store(info.device, std::memory_order_release);
    }
    apusr::host_discovery::eventf(
        "vulkan", "streamline.device_info",
        "device=%p physical_device=%p instance=%p graphics_family=%u graphics_index=%u compute_family=%u compute_index=%u native_device_contract=0x%x",
        info.device,
        info.physicalDevice,
        info.instance,
        info.graphicsQueueFamily,
        info.graphicsQueueIndex,
        info.computeQueueFamily,
        info.computeQueueIndex,
        requirementFlags);
}

void clearVulkanStreamlineState()
{
    std::lock_guard<std::mutex> lock(g_vulkanCaptureMutex);
    g_vulkanQueueHintsByDevice.clear();
    g_currentVulkanDevice.store(nullptr, std::memory_order_release);
}

[[nodiscard]] bool isVulkanStreamlinePath() noexcept
{
    return g_slRenderApi.load(std::memory_order_relaxed) == kSlRenderApiVulkan;
}

[[nodiscard]] bool isD3d12StreamlinePath() noexcept
{
    return g_slRenderApi.load(std::memory_order_relaxed) == kSlRenderApiD3d12;
}

[[nodiscard]] bool isReflexStreamlinePath() noexcept
{
    return isVulkanStreamlinePath() || isD3d12StreamlinePath();
}

[[nodiscard]] HRESULT setD3d12AntiLagStateLocked(const bool enabled, const unsigned maxFps) noexcept
{
    if (g_d3d12AntiLagContext.m_pAntiLagAPI == nullptr) return E_NOINTERFACE;
    if (g_d3d12AntiLagContext.m_enabled == enabled && g_d3d12AntiLagContext.m_maxFPS == maxFps) return S_OK;

    AMD::AntiLag2DX12::APIData_v1 data{};
    data.uiSize = sizeof(data);
    data.uiVersion = 1u;
    data.eMode = enabled ? 1u : 2u;
    data.sControlStr = nullptr;
    data.uiControlStrLength = 0u;
    data.maxFPS = maxFps;
    const auto profileStream = reflexProfileStream();
    apusr::profile::CpuScope profileScope(profileStream, 0u, 0u, "reflex.d3d12.update_state");
    const HRESULT hr = g_d3d12AntiLagContext.m_pAntiLagAPI->UpdateAntiLagState(&data);
    if (SUCCEEDED(hr)) {
        g_d3d12AntiLagContext.m_enabled = enabled;
        g_d3d12AntiLagContext.m_maxFPS = maxFps;
    }
    return hr;
}

void deinitializeD3d12AntiLagLocked() noexcept
{
    if (g_d3d12AntiLagContext.m_pAntiLagAPI != nullptr) {
        (void)setD3d12AntiLagStateLocked(false, 0u);
        (void)AMD::AntiLag2DX12::DeInitialize(&g_d3d12AntiLagContext);
    }
    if (g_d3d12AntiLagDevice != nullptr) {
        g_d3d12AntiLagDevice->Release();
        g_d3d12AntiLagDevice = nullptr;
    }
    g_d3d12AntiLagLastInputFrame = nullptr;
    g_d3d12AntiLagLastRenderEndFrame = nullptr;
    g_d3d12AntiLagAvailable.store(false, std::memory_order_release);
}

[[nodiscard]] bool initializeD3d12AntiLagLocked(
    ID3D12Device* nativeDevice,
    void* publishedDevice,
    const char* source)
{
    // The caller transfers one native-device reference on entry.
    const HRESULT hr = AMD::AntiLag2DX12::Initialize(&g_d3d12AntiLagContext, nativeDevice);
    if (FAILED(hr)) {
        nativeDevice->Release();
        apusr::host_discovery::eventf(
            "streamline", "reflex.d3d12.antilag_unavailable",
            "reason=amd_antilag2_initialize_failed hr=0x%08lx source=%s",
            static_cast<unsigned long>(hr), source);
        return false;
    }
    g_d3d12AntiLagDevice = nativeDevice;
    g_d3d12AntiLagAvailable.store(true, std::memory_order_release);
    apusr::host_discovery::eventf(
        "streamline", "reflex.d3d12.antilag_ready",
        "published_device=%p native_device=%p source=%s",
        publishedDevice, nativeDevice, source);
    return true;
}

void bindD3d12AntiLagDevice(void* streamLineDevice)
{
    std::lock_guard<std::mutex> lock(g_d3d12AntiLagMutex);
    deinitializeD3d12AntiLagLocked();
    g_d3d12AntiLagProbed.store(true, std::memory_order_release);

    if (streamLineDevice == nullptr) {
        apusr::host_discovery::event("streamline", "reflex.d3d12.antilag_unavailable", "reason=null_device");
        return;
    }

    void* nativeInterface = nullptr;
    int nativeResult = kResultFeatureMissing;
    if (auto nativeFn = originalFn<SlGetNativeInterfaceFn>("slGetNativeInterface")) {
        nativeResult = nativeFn(streamLineDevice, &nativeInterface);
    }
    if (nativeResult != kResultOk || nativeInterface == nullptr) {
        apusr::host_discovery::eventf(
            "streamline", "reflex.d3d12.antilag_unavailable",
            "reason=native_interface_unwrap_failed result=%d proxy=%p", nativeResult, streamLineDevice);
        return;
    }

    auto* unknown = static_cast<IUnknown*>(nativeInterface);
    ID3D12Device* nativeDevice = nullptr;
    const HRESULT qi = unknown->QueryInterface(IID_PPV_ARGS(&nativeDevice));
    unknown->Release();
    if (FAILED(qi) || nativeDevice == nullptr) {
        apusr::host_discovery::eventf(
            "streamline", "reflex.d3d12.antilag_unavailable",
            "reason=native_not_d3d12 qi=0x%08lx", static_cast<unsigned long>(qi));
        return;
    }

    (void)initializeD3d12AntiLagLocked(nativeDevice, streamLineDevice, "slSetD3DDevice_proxy_unwrap");
}

[[nodiscard]] bool bindD3d12AntiLagNativeDevice(void* nativeDeviceInterface)
{
    if (nativeDeviceInterface == nullptr) {
        apusr::host_discovery::event(
            "streamline", "reflex.d3d12.antilag_unavailable",
            "reason=null_native_device source=plugin_startup_renderer_restart");
        return false;
    }

    ID3D12Device* nativeDevice = nullptr;
    const HRESULT qi = static_cast<IUnknown*>(nativeDeviceInterface)->QueryInterface(IID_PPV_ARGS(&nativeDevice));
    if (FAILED(qi) || nativeDevice == nullptr) {
        apusr::host_discovery::eventf(
            "streamline", "reflex.d3d12.antilag_unavailable",
            "reason=startup_device_not_d3d12 qi=0x%08lx source=plugin_startup_renderer_restart",
            static_cast<unsigned long>(qi));
        return false;
    }

    std::lock_guard<std::mutex> lock(g_d3d12AntiLagMutex);
    g_d3d12AntiLagProbed.store(true, std::memory_order_release);
    if (g_d3d12AntiLagAvailable.load(std::memory_order_acquire) &&
        g_d3d12AntiLagDevice == nativeDevice) {
        nativeDevice->Release();
        return true;
    }

    deinitializeD3d12AntiLagLocked();
    return initializeD3d12AntiLagLocked(
        nativeDevice, nativeDeviceInterface, "plugin_startup_renderer_restart");
}

[[nodiscard]] HRESULT updateD3d12AntiLagForFrame(const void* frame)
{
    std::lock_guard<std::mutex> lock(g_d3d12AntiLagMutex);
    if (!g_d3d12AntiLagAvailable.load(std::memory_order_acquire)) return E_NOINTERFACE;
    if (frame != nullptr && g_d3d12AntiLagLastInputFrame == frame) return S_OK;
    const bool enabled = g_reflexMode.load(std::memory_order_relaxed) != APUSR_VK_HOST_ANTI_LAG_OFF;
    const unsigned maxFps = g_reflexMaxFps.load(std::memory_order_relaxed);
    const auto profileStream = reflexProfileStream();
    const std::uint64_t profileFrame = reflexProfileFrameId(frame);
    apusr::profile::CpuScope profileScope(profileStream, profileFrame, 0u, "reflex.d3d12.update");
    const HRESULT hr = AMD::AntiLag2DX12::Update(&g_d3d12AntiLagContext, enabled, maxFps);
    if (SUCCEEDED(hr)) g_d3d12AntiLagLastInputFrame = frame;
    return hr;
}

void markD3d12AntiLagRenderEnd(const void* frame)
{
    std::lock_guard<std::mutex> lock(g_d3d12AntiLagMutex);
    if (!g_d3d12AntiLagAvailable.load(std::memory_order_acquire)) return;
    if (frame != nullptr && g_d3d12AntiLagLastRenderEndFrame == frame) return;
    const auto profileStream = reflexProfileStream();
    const std::uint64_t profileFrame = reflexProfileFrameId(frame);
    apusr::profile::CpuScope profileScope(profileStream, profileFrame, 0u, "reflex.d3d12.mark_render_end");
    if (SUCCEEDED(AMD::AntiLag2DX12::MarkEndOfFrameRendering(&g_d3d12AntiLagContext))) {
        g_d3d12AntiLagLastRenderEndFrame = frame;
    }
}

[[nodiscard]] void* currentVulkanDevice() noexcept
{
    return g_currentVulkanDevice.load(std::memory_order_acquire);
}

[[nodiscard]] bool resolveFrameToken(const void* token, std::uint64_t& frameIndex) noexcept
{
    return apusr::streamline::core_abi::frameIndex(token, frameIndex);
}

[[nodiscard]] bool featureRequested(const SlPreferences& prefs, const std::uint32_t feature) noexcept
{
    if (prefs.featuresToLoad == nullptr) {
        return false;
    }
    for (std::uint32_t i = 0; i < prefs.numFeaturesToLoad; ++i) {
        if (prefs.featuresToLoad[i] == feature) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::uint32_t reflexFrameLimitToFps(const std::uint32_t frameLimitUs) noexcept
{
    if (frameLimitUs == 0u) {
        return 0u;
    }
    const std::uint64_t rounded = (1'000'000ull + frameLimitUs / 2u) / frameLimitUs;
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, rounded));
}

void installIdentityHooksForCurrentApi()
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    // Attach mode is deliberately not an identity-spoof owner. D3D games keep
    // the already-proven dxgi/nvapi compatibility surfaces, while Vulkan uses
    // APUSR NGX requirements plus host capture. Do not mutate stock Streamline's
    // module graph from these hooks.
    return;
#else
    if (!isVulkanStreamlinePath()) {
        apusrInstallIdentitySpoofHooks();
    }
#endif
}

bool resolveCapturedVulkanQueue(
    void* device,
    const std::uint32_t preferredKind,
    void** outQueue,
    std::uint32_t* outFamilyIndex,
    std::uint32_t* outQueueIndex,
    std::uint32_t* outKind)
{
    if (device == nullptr || outQueue == nullptr) {
        return false;
    }

    ApusrVulkanHostQueueInfo info{};
    if (!apusrVulkanHostResolveQueue(device, captureQueueKindForHost(preferredKind), &info)) {
        *outQueue = nullptr;
        return false;
    }

    *outQueue = info.queue;
    if (outFamilyIndex != nullptr) {
        *outFamilyIndex = info.familyIndex;
    }
    if (outQueueIndex != nullptr) {
        *outQueueIndex = info.queueIndex;
    }
    if (outKind != nullptr) {
        *outKind = apusrQueueKindForHost(info.kind);
    }
    return true;
}

bool resolveVulkanStreamlineContext(
    void* device,
    std::uint32_t* outGraphicsFamilyIndex,
    std::uint32_t* outComputeFamilyIndex,
    std::uint32_t* outDeviceRequirementFlags)
{
    if (device == nullptr || outGraphicsFamilyIndex == nullptr || outComputeFamilyIndex == nullptr) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_vulkanCaptureMutex);
        const auto hints = g_vulkanQueueHintsByDevice.find(device);
        if (hints != g_vulkanQueueHintsByDevice.end() && hints->second.valid) {
            *outGraphicsFamilyIndex = hints->second.graphicsFamily;
            *outComputeFamilyIndex = hints->second.computeFamily;
            if (outDeviceRequirementFlags != nullptr) {
                *outDeviceRequirementFlags = hints->second.deviceRequirementFlags;
            }
            return true;
        }
    }

    ApusrVulkanHostQueueInfo graphics{};
    ApusrVulkanHostQueueInfo compute{};
    if (!apusrVulkanHostResolveQueue(device, APUSR_VK_HOST_QUEUE_GRAPHICS, &graphics) ||
        !apusrVulkanHostResolveQueue(device, APUSR_VK_HOST_QUEUE_COMPUTE, &compute)) {
        return false;
    }

    *outGraphicsFamilyIndex = graphics.familyIndex;
    *outComputeFamilyIndex = compute.familyIndex;
    if (outDeviceRequirementFlags != nullptr) {
        *outDeviceRequirementFlags = apusrVulkanHostDeviceNegotiationFlags(device);
    }
    return true;
}

HMODULE loadApusrNgx()
{
    if (g_ngx != nullptr) {
        return g_ngx;
    }

    const wchar_t* primaryNgx = isVulkanStreamlinePath() ? L"nvngx_dlss.dll" : L"nvngx.dll";
    const wchar_t* secondaryNgx = isVulkanStreamlinePath() ? L"nvngx.dll" : L"nvngx_dlss.dll";

    wchar_t selfPath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), selfPath, MAX_PATH);
    if (len != 0 && len < MAX_PATH) {
        if (wchar_t* slash = wcsrchr(selfPath, L'\\')) {
            *(slash + 1) = L'\0';
            wchar_t candidate[MAX_PATH]{};
            wcscpy_s(candidate, selfPath);
            wcscat_s(candidate, primaryNgx);
            g_ngx = LoadLibraryW(candidate);
            if (g_ngx == nullptr) {
                wcscpy_s(candidate, selfPath);
                wcscat_s(candidate, secondaryNgx);
                g_ngx = LoadLibraryW(candidate);
            }
        }
    }
    if (g_ngx == nullptr) {
        g_ngx = LoadLibraryW(primaryNgx);
    }
    if (g_ngx == nullptr) {
        g_ngx = LoadLibraryW(secondaryNgx);
    }
    return g_ngx;
}

template <typename Fn>
Fn ngxFn(const char* name)
{
    HMODULE mod = loadApusrNgx();
    return mod != nullptr ? reinterpret_cast<Fn>(GetProcAddress(mod, name)) : nullptr;
}

template <typename Fn>
Fn ngxFnCached(Fn& slot, const char* name)
{
    if (slot != nullptr) {
        return slot;
    }
    slot = ngxFn<Fn>(name);
    return slot;
}

CapturedResource capturedResourceFromTag(const SlResourceTag& tag)
{
    CapturedResource captured;
    captured.tagged = true;
    captured.lifecycle = tag.lifecycle;
    if (tag.extent.width != 0u && tag.extent.height != 0u) {
        captured.extentLeft = tag.extent.left;
        captured.extentTop = tag.extent.top;
        captured.extentWidth = tag.extent.width;
        captured.extentHeight = tag.extent.height;
    }
    if (tag.resource == nullptr) {
        return captured;
    }
    captured.native = tag.resource->native;
    captured.memory = tag.resource->memory;
    captured.view = tag.resource->view;
    captured.state = tag.resource->state;
    captured.width = tag.resource->width;
    captured.height = tag.resource->height;
    captured.nativeFormat = tag.resource->nativeFormat;
    captured.mipLevels = tag.resource->mipLevels;
    captured.arrayLayers = tag.resource->arrayLayers;
    captured.flags = tag.resource->flags;
    captured.usage = tag.resource->usage;

    // ResourceTag::extent is the active render region. The backing VkImage can
    // remain display-sized under DRS, so using Resource::width/height here
    // changes DLSS mode semantics and is incorrect.
    if (captured.extentWidth == 0u || captured.extentHeight == 0u) {
        captured.extentWidth = captured.width;
        captured.extentHeight = captured.height;
    }

    // Streamline chains Vulkan SubresourceRange from Resource::next. Preserve
    // it instead of manufacturing a whole-image range; callers can otherwise
    // transition or bind the wrong mip/array slice.
    for (const BaseStructure* ext = tag.resource->next; ext != nullptr; ext = ext->next) {
        if (apusr::streamline::core_abi::structTypeEquals(ext->structType, kSlSubresourceRangeStructType)) {
            const auto* range = reinterpret_cast<const SlSubresourceRange*>(ext);
            captured.subresourceAspectMask = range->aspectMask;
            captured.subresourceBaseMipLevel = range->baseMipLevel;
            captured.subresourceLevelCount = range->levelCount;
            captured.subresourceBaseArrayLayer = range->baseArrayLayer;
            captured.subresourceLayerCount = range->layerCount;
            captured.hasSubresourceRange = true;
            break;
        }
    }
    return captured;
}

void captureTag(CapturedTags& dst, const SlResourceTag& tag)
{
    const CapturedResource resource = capturedResourceFromTag(tag);
    switch (tag.type) {
    case kBufferTypeDepth:
        dst.depth = resource;
        break;
    case kBufferTypeMotionVectors:
        dst.motionVectors = resource;
        break;
    case kBufferTypeHUDLessColor:
        dst.hudlessColor = resource;
        break;
    case kBufferTypeScalingInputColor:
        dst.color = resource;
        break;
    case kBufferTypeScalingOutputColor:
        dst.output = resource;
        break;
    case kBufferTypeExposure:
        dst.exposure = resource;
        break;
    case kBufferTypeUIColorAndAlpha:
        dst.uiColorAndAlpha = resource;
        break;
    case kBufferTypeBidirectionalDistortionField:
        dst.distortionField = resource;
        break;
    case kBufferTypeBackbuffer:
        dst.backbuffer = resource;
        break;
    case kBufferTypeUIAlpha:
        dst.uiAlpha = resource;
        break;
    case kBufferTypeBiasCurrentColorHint:
    case kBufferTypeReactiveMaskHint:
        dst.reactive = resource;
        break;
    default:
        break;
    }
}

void mergeCapturedTags(CapturedTags& dst, const CapturedTags& src)
{
    auto merge = [](CapturedResource& target, const CapturedResource& source) {
        if (source.tagged)
            target = source;
    };
    merge(dst.color, src.color);
    merge(dst.depth, src.depth);
    merge(dst.motionVectors, src.motionVectors);
    merge(dst.output, src.output);
    merge(dst.exposure, src.exposure);
    merge(dst.reactive, src.reactive);
    merge(dst.hudlessColor, src.hudlessColor);
    merge(dst.uiColorAndAlpha, src.uiColorAndAlpha);
    merge(dst.uiAlpha, src.uiAlpha);
    merge(dst.distortionField, src.distortionField);
    merge(dst.backbuffer, src.backbuffer);
}

void pruneFrameScopedTags(const void* currentFrame)
{
    if (currentFrame == nullptr || g_tagsByFrameViewport.size() <= 64u) {
        return;
    }
    for (auto it = g_tagsByFrameViewport.begin(); it != g_tagsByFrameViewport.end();) {
        if (it->first.frame != currentFrame) {
            it = g_tagsByFrameViewport.erase(it);
        } else {
            ++it;
        }
    }
}

void captureTagsForKey(const StreamlineViewportKey key, const void* rawTags, const std::uint32_t numTags)
{
    if (rawTags == nullptr || numTags == 0) {
        return;
    }
    const auto* tags = static_cast<const SlResourceTag*>(rawTags);
    std::lock_guard<std::mutex> lock(g_streamlineStateMutex);

    auto captureInto = [&](CapturedTags& dst) {
        for (std::uint32_t i = 0; i < numTags; ++i) {
            captureTag(dst, tags[i]);
        }
    };

    CapturedTags& dst = key.frame != nullptr ? g_tagsByFrameViewport[key] : g_legacyTagsByViewport[key.viewport];
    captureInto(dst);

    // Some hosts use a null/default viewport for options and a real viewport for tags,
    // while others pass the viewport only as a local evaluate input. Keep a viewport-0
    // shadow so evaluation can resolve the frame contract without a separate menu-only path.
    if (key.viewport != 0u) {
        CapturedTags& fallback = key.frame != nullptr ? g_tagsByFrameViewport[StreamlineViewportKey{.frame = key.frame, .viewport = 0u}]
                                                     : g_legacyTagsByViewport[0u];
        captureInto(fallback);
    }
}

void captureTagsFromInputs(CapturedTags& dst, const BaseStructure* const* inputs, const std::uint32_t numInputs)
{
    if (inputs == nullptr) {
        return;
    }
    for (std::uint32_t i = 0; i < numInputs; ++i) {
        const BaseStructure* input = inputs[i];
        if (input == nullptr) {
            continue;
        }
        // Streamline ResourceTag's StructType is {4C6A5AAD-B445-496C-87FF-1AF3845BE653}.
        if (input->structType.data1 == 0x4c6a5aadU) {
            captureTag(dst, *reinterpret_cast<const SlResourceTag*>(input));
        }
    }
}

CapturedConstants capturedConstantsFromSl(const SlConstants& constants)
{
    CapturedConstants out;
    out.valid = true;
    out.jitterX = constants.jitterOffset.x;
    out.jitterY = constants.jitterOffset.y;
    out.mvScaleX = constants.mvecScale.x == 0.0f ? 1.0f : constants.mvecScale.x;
    out.mvScaleY = constants.mvecScale.y == 0.0f ? 1.0f : constants.mvecScale.y;
    // sl::Boolean is { false=0, true=1, invalid=2 }. Feature flags
    // consumed by APUSR only become true for the explicit eTrue value.
    out.reset = constants.reset == 1;
    out.depthInverted = constants.depthInverted == 1;
    out.motionVectorsJittered = constants.motionVectorsJittered == 1;
    out.cameraMotionIncluded = constants.cameraMotionIncluded != 0;
    out.motionVectorsDilated = constants.motionVectorsDilated == 1;
    out.orthographicProjection = constants.orthographicProjection == 1;
    out.cameraNear = constants.cameraNear;
    out.cameraFar = constants.cameraFar;
    out.cameraFOV = constants.cameraFOV;
    out.cameraAspectRatio = constants.cameraAspectRatio;
    out.cameraPosition[0] = constants.cameraPos.x;
    out.cameraPosition[1] = constants.cameraPos.y;
    out.cameraPosition[2] = constants.cameraPos.z;
    out.cameraUp[0] = constants.cameraUp.x;
    out.cameraUp[1] = constants.cameraUp.y;
    out.cameraUp[2] = constants.cameraUp.z;
    out.cameraRight[0] = constants.cameraRight.x;
    out.cameraRight[1] = constants.cameraRight.y;
    out.cameraRight[2] = constants.cameraRight.z;
    out.cameraForward[0] = constants.cameraFwd.x;
    out.cameraForward[1] = constants.cameraFwd.y;
    out.cameraForward[2] = constants.cameraFwd.z;
    return out;
}

void captureConstantsForKey(const StreamlineViewportKey key, const SlConstants& constants)
{
    const CapturedConstants captured = capturedConstantsFromSl(constants);
    std::lock_guard<std::mutex> lock(g_streamlineStateMutex);
    if (key.frame != nullptr) {
        g_constantsByFrameViewport[key] = captured;
        if (key.viewport != 0u) {
            g_constantsByFrameViewport[StreamlineViewportKey{.frame = key.frame, .viewport = 0u}] = captured;
        }
    } else {
        g_constantsByViewport[key.viewport] = captured;
        if (key.viewport != 0u) {
            g_constantsByViewport[0u] = captured;
        }
    }
}

void mergeCapturedConstants(CapturedConstants& dst, const CapturedConstants& src)
{
    if (src.valid) {
        dst = src;
    }
}

void captureConstantsFromInputs(CapturedConstants& dst, const BaseStructure* const* inputs, const std::uint32_t numInputs)
{
    if (inputs == nullptr) {
        return;
    }
    for (std::uint32_t i = 0; i < numInputs; ++i) {
        const BaseStructure* input = inputs[i];
        // Streamline Constants' StructType is {DCD35AD7-4E4A-4BAD-A90C-E0C49EB23AFE}.
        if (input != nullptr && input->structType.data1 == 0xdcd35ad7U) {
            mergeCapturedConstants(dst, capturedConstantsFromSl(*reinterpret_cast<const SlConstants*>(input)));
        }
    }
}

bool findViewportInInputs(const BaseStructure* const* inputs, const std::uint32_t numInputs, std::uint32_t& viewport)
{
    if (inputs == nullptr) {
        return false;
    }
    for (std::uint32_t i = 0; i < numInputs; ++i) {
        const BaseStructure* input = inputs[i];
        if (input == nullptr) {
            continue;
        }
        // Streamline ViewportHandle's StructType is {171B6435-9B3C-4FC8-9994-FBE52569AAA4}.
        // Do not infer viewport from arbitrary structures; constants/options can also appear in the input array.
        if (input->structType.data1 == 0x171b6435U) {
            viewport = reinterpret_cast<const ViewportHandle*>(input)->value;
            return true;
        }
    }
    return false;
}

std::uint32_t ngxQualityFromDlssMode(const DLSSMode mode) noexcept
{
    switch (mode) {
    case DLSSMode::eUltraPerformance:
        return 3u;
    case DLSSMode::eMaxPerformance:
        return 0u;
    case DLSSMode::eMaxQuality:
        return 2u;
    case DLSSMode::eUltraQuality:
        return 4u;
    case DLSSMode::eDLAA:
        return 5u;
    case DLSSMode::eBalanced:
    case DLSSMode::eOff:
    default:
        return 1u;
    }
}

void setNgxPointer(NVSDK_NGX_Parameter* params, const char* name, void* value)
{
    if (params != nullptr && name != nullptr) {
        params->Set(name, value);
    }
}

void setNgxUInt(NVSDK_NGX_Parameter* params, const char* name, const std::uint32_t value)
{
    if (params != nullptr && name != nullptr) {
        params->Set(name, value);
    }
}

void setNgxInt(NVSDK_NGX_Parameter* params, const char* name, const int value)
{
    if (params != nullptr && name != nullptr) {
        params->Set(name, value);
    }
}

void setNgxFloat(NVSDK_NGX_Parameter* params, const char* name, const float value)
{
    if (params != nullptr && name != nullptr) {
        params->Set(name, value);
    }
}

using NgxVkSubresourceRangeAbi = VkImageSubresourceRangeAbi;

struct NgxVkImageViewInfoAbi {
    std::uintptr_t imageView;
    std::uintptr_t image;
    NgxVkSubresourceRangeAbi subresourceRange;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
};

union NgxVkResourceUnionAbi {
    NgxVkImageViewInfoAbi imageViewInfo;
};

struct NgxVkResourceAbi {
    NgxVkResourceUnionAbi resource;
    std::uint32_t type;
    bool readWrite;
};

static_assert(sizeof(NgxVkImageViewInfoAbi) == 48, "NGX Vulkan image ABI drift");
static_assert(sizeof(NgxVkResourceAbi) == 56, "NGX Vulkan resource ABI drift");

constexpr std::uint32_t kNgxDlssFeatureCreateFlagIsHdr = 1u << 0u;
constexpr std::uint32_t kNgxDlssFeatureCreateFlagMvLowRes = 1u << 1u;
constexpr std::uint32_t kNgxDlssFeatureCreateFlagMvJittered = 1u << 2u;
constexpr std::uint32_t kNgxDlssFeatureCreateFlagDepthInverted = 1u << 3u;
constexpr std::uint32_t kNgxDlssFeatureCreateFlagAutoExposure = 1u << 6u;

std::uint32_t vulkanAspectMask(const std::uint32_t format) noexcept
{
    switch (format) {
    case 124u: // VK_FORMAT_D16_UNORM
    case 125u: // VK_FORMAT_X8_D24_UNORM_PACK32
    case 126u: // VK_FORMAT_D32_SFLOAT
    case 128u: // VK_FORMAT_D16_UNORM_S8_UINT
    case 129u: // VK_FORMAT_D24_UNORM_S8_UINT
    case 130u: // VK_FORMAT_D32_SFLOAT_S8_UINT
        return 0x2u; // VK_IMAGE_ASPECT_DEPTH_BIT
    case 127u: // VK_FORMAT_S8_UINT
        return 0x4u; // VK_IMAGE_ASPECT_STENCIL_BIT
    default:
        return 0x1u; // VK_IMAGE_ASPECT_COLOR_BIT
    }
}

constexpr std::uint32_t kVkStructureTypeImageMemoryBarrier = 45u;
constexpr std::uint32_t kVkQueueFamilyIgnored = 0xffffffffu;
constexpr std::uint32_t kVkImageLayoutUndefined = 0u;
constexpr std::uint32_t kVkImageLayoutGeneral = 1u;
constexpr std::uint32_t kVkImageLayoutShaderReadOnlyOptimal = 5u;
constexpr std::uint32_t kVkImageLayoutPresentSrcKhr = 1000001002u;
constexpr std::uint32_t kVkPipelineStageComputeShaderBit = 0x00000800u;
constexpr std::uint32_t kVkPipelineStageAllCommandsBit = 0x00010000u;
constexpr std::uint32_t kVkAccessShaderReadBit = 0x00000020u;
constexpr std::uint32_t kVkAccessShaderWriteBit = 0x00000040u;
constexpr std::uint32_t kVkAccessMemoryReadBit = 0x00008000u;
constexpr std::uint32_t kVkAccessMemoryWriteBit = 0x00010000u;

struct VulkanDlssBoundaryResource {
    const CapturedResource* resource;
    std::uint32_t evaluateLayout;
    std::uint32_t evaluateAccess;
    bool allowUndefinedHostLayout;
};

bool transitionVulkanDlssBoundary(
    void* device,
    void* commandBuffer,
    const CapturedTags& tags,
    const bool intoEvaluation)
{
    if (device == nullptr || commandBuffer == nullptr) {
        return false;
    }

    static const VkGetDeviceProcAddrFn getDeviceProcAddr =
        vulkanSystemFn<VkGetDeviceProcAddrFn>("vkGetDeviceProcAddr");
    if (getDeviceProcAddr == nullptr) {
        apusr::host_discovery::event(
            "vulkan", "dlss.transition_failed", "vkGetDeviceProcAddr unavailable");
        return false;
    }
    const auto cmdPipelineBarrier = reinterpret_cast<VkCmdPipelineBarrierFn>(
        getDeviceProcAddr(device, "vkCmdPipelineBarrier"));
    if (cmdPipelineBarrier == nullptr) {
        apusr::host_discovery::event(
            "vulkan", "dlss.transition_failed", "vkCmdPipelineBarrier unavailable");
        return false;
    }

    const std::array<VulkanDlssBoundaryResource, 6> resources{{
        {&tags.color, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit, false},
        {&tags.depth, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit, false},
        {&tags.motionVectors, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit, false},
        {&tags.output, kVkImageLayoutGeneral, kVkAccessShaderReadBit | kVkAccessShaderWriteBit, true},
        {&tags.exposure, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit, false},
        {&tags.reactive, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit, false},
    }};

    std::array<VkImageMemoryBarrierAbi, 6> barriers{};
    std::uint32_t barrierCount = 0;
    for (const VulkanDlssBoundaryResource& entry : resources) {
        const CapturedResource& resource = *entry.resource;
        if (resource.native == nullptr) {
            continue;
        }
        // Vulkan Streamline resources must carry the current VkImageLayout.
        // UINT_MAX is Streamline's unknown/default state marker. Required input
        // content cannot be UNDEFINED, but a newly-created output image can be;
        // like Streamline's Vulkan backend, restore that special case to GENERAL.
        if (resource.state == 0xffffffffu ||
            (resource.state == kVkImageLayoutUndefined && !entry.allowUndefinedHostLayout)) {
            apusr::host_discovery::eventf(
                "vulkan", "dlss.transition_failed",
                "image=%p invalid_tagged_layout=%u", resource.native, resource.state);
            return false;
        }

        const std::uint32_t restoreLayout = resource.state == kVkImageLayoutUndefined
            ? kVkImageLayoutGeneral
            : resource.state;
        const std::uint32_t oldLayout = intoEvaluation ? resource.state : entry.evaluateLayout;
        const std::uint32_t newLayout = intoEvaluation ? entry.evaluateLayout : restoreLayout;
        const std::uint32_t srcAccess = intoEvaluation
            ? ((resource.state == kVkImageLayoutPresentSrcKhr || resource.state == kVkImageLayoutUndefined)
                ? 0u : (kVkAccessMemoryReadBit | kVkAccessMemoryWriteBit))
            : entry.evaluateAccess;
        const std::uint32_t dstAccess = intoEvaluation
            ? entry.evaluateAccess
            : (restoreLayout == kVkImageLayoutPresentSrcKhr ? 0u : (kVkAccessMemoryReadBit | kVkAccessMemoryWriteBit));

        barriers[barrierCount++] = VkImageMemoryBarrierAbi{
            .sType = kVkStructureTypeImageMemoryBarrier,
            .pNext = nullptr,
            .srcAccessMask = srcAccess,
            .dstAccessMask = dstAccess,
            .oldLayout = oldLayout,
            .newLayout = newLayout,
            .srcQueueFamilyIndex = kVkQueueFamilyIgnored,
            .dstQueueFamilyIndex = kVkQueueFamilyIgnored,
            .image = resource.native,
            .subresourceRange = vulkanSubresourceRange(resource),
        };
    }

    if (barrierCount == 0u) {
        return true;
    }
    // This direct evaluator is not the stock-attach product path. Keep its
    // unknown host endpoint conservative, but scope the APUSR side to compute:
    // ingress waits only at compute, and egress waits only on prior compute.
    // The stock-attach path is narrowed below Streamline in host capture, where
    // stock sl.dlss remains the owner of resource transitions/restoration.
    cmdPipelineBarrier(
        commandBuffer,
        intoEvaluation ? kVkPipelineStageAllCommandsBit : kVkPipelineStageComputeShaderBit,
        intoEvaluation ? kVkPipelineStageComputeShaderBit : kVkPipelineStageAllCommandsBit,
        0u,
        0u, nullptr,
        0u, nullptr,
        barrierCount, barriers.data());
    return true;
}

VkImageSubresourceRangeAbi vulkanSubresourceRange(const CapturedResource& resource) noexcept
{
    if (resource.hasSubresourceRange) {
        return VkImageSubresourceRangeAbi{
            .aspectMask = resource.subresourceAspectMask,
            .baseMipLevel = resource.subresourceBaseMipLevel,
            .levelCount = resource.subresourceLevelCount,
            .baseArrayLayer = resource.subresourceBaseArrayLayer,
            .layerCount = resource.subresourceLayerCount,
        };
    }

    // Match Streamline's Vulkan NGX bridge: absent an explicit chained range,
    // expose the whole image rather than inventing a one-mip/one-layer slice.
    return VkImageSubresourceRangeAbi{
        .aspectMask = vulkanAspectMask(resource.nativeFormat),
        .baseMipLevel = 0u,
        .levelCount = 0xffffffffu, // VK_REMAINING_MIP_LEVELS
        .baseArrayLayer = 0u,
        .layerCount = 0xffffffffu, // VK_REMAINING_ARRAY_LAYERS
    };
}

bool validVulkanSubresourceRange(const CapturedResource& resource) noexcept
{
    const VkImageSubresourceRangeAbi range = vulkanSubresourceRange(resource);
    return range.aspectMask != 0u && range.levelCount != 0u && range.layerCount != 0u;
}

NgxVkResourceAbi makeNgxVkImageResource(const CapturedResource& resource)
{
    NgxVkResourceAbi out{};
    out.resource.imageViewInfo.imageView = reinterpret_cast<std::uintptr_t>(resource.view);
    out.resource.imageViewInfo.image = reinterpret_cast<std::uintptr_t>(resource.native);
    out.resource.imageViewInfo.subresourceRange = vulkanSubresourceRange(resource);
    out.resource.imageViewInfo.format = resource.nativeFormat;
    // NGX's resource descriptor describes the backing image. Active render
    // dimensions/origins travel separately through the DLSS subrect contract.
    out.resource.imageViewInfo.width = resource.width;
    out.resource.imageViewInfo.height = resource.height;
    out.type = 0u; // NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW
    out.readWrite = vulkanStorageCapable(resource);
    return out;
}

std::uint32_t createFlagsFromStreamlineContract(
    const CapturedTags& tags,
    const CapturedOptions& options,
    const CapturedConstants& constants,
    const std::uint32_t renderWidth,
    const std::uint32_t renderHeight)
{
    std::uint32_t flags = 0u;
    if (options.highDynamicRange) {
        flags |= kNgxDlssFeatureCreateFlagIsHdr;
    }

    bool motionVectorsAreDisplayRes = false;
    const std::uint32_t motionVectorWidth = activeWidth(tags.motionVectors);
    const std::uint32_t motionVectorHeight = activeHeight(tags.motionVectors);
    if (motionVectorWidth != 0u && motionVectorHeight != 0u && renderWidth != 0u && renderHeight != 0u) {
        motionVectorsAreDisplayRes = motionVectorWidth > renderWidth || motionVectorHeight > renderHeight;
    }
    if (!motionVectorsAreDisplayRes) {
        flags |= kNgxDlssFeatureCreateFlagMvLowRes;
    }
    if (constants.motionVectorsJittered) {
        flags |= kNgxDlssFeatureCreateFlagMvJittered;
    }
    if (constants.depthInverted) {
        flags |= kNgxDlssFeatureCreateFlagDepthInverted;
    }
    if (options.useAutoExposure || tags.exposure.native == nullptr) {
        flags |= kNgxDlssFeatureCreateFlagAutoExposure;
    }
    return flags;
}

float ngxMotionVectorScale(const float streamlineScale, const std::uint32_t renderExtent) noexcept
{
    return streamlineScale > 0.0f && renderExtent != 0u
        ? streamlineScale * static_cast<float>(renderExtent)
        : 1.0f;
}

bool ensureStreamlineNgxD3D12Feature(const CapturedTags& tags, const CapturedOptions& options, void* cmdBuffer)
{
    if (g_d3dDevice == nullptr || cmdBuffer == nullptr) {
        traceDebug("DLSS evaluate missing D3D12 device or command list");
        return false;
    }

    std::uint32_t renderWidth = tags.color.width;
    std::uint32_t renderHeight = tags.color.height;
    if (renderWidth == 0 || renderHeight == 0) {
        const float scale = dlssScale(options.mode);
        renderWidth = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(options.outputWidth) * scale + 0.5f));
        renderHeight = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(options.outputHeight) * scale + 0.5f));
    }
    const std::uint32_t outputWidth = options.outputWidth != 0 ? options.outputWidth : tags.output.width;
    const std::uint32_t outputHeight = options.outputHeight != 0 ? options.outputHeight : tags.output.height;
    if (renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0) {
        traceDebug("DLSS evaluate missing render/output dimensions");
        return false;
    }

    if (g_streamlineNgxFeature != nullptr &&
        g_streamlineNgxRenderWidth == renderWidth &&
        g_streamlineNgxRenderHeight == renderHeight &&
        g_streamlineNgxOutputWidth == outputWidth &&
        g_streamlineNgxOutputHeight == outputHeight &&
        g_streamlineNgxMode == options.mode) {
        return true;
    }

    auto init = ngxFnCached(g_streamlineNgxInitFn, "NVSDK_NGX_D3D12_Init");
    auto alloc = ngxFnCached(g_streamlineNgxAllocateParametersFn, "NVSDK_NGX_D3D12_AllocateParameters");
    auto destroy = ngxFnCached(g_streamlineNgxDestroyParametersFn, "NVSDK_NGX_D3D12_DestroyParameters");
    auto create = ngxFnCached(g_streamlineNgxCreateFeatureFn, "NVSDK_NGX_D3D12_CreateFeature");
    auto release = ngxFnCached(g_streamlineNgxReleaseFeatureFn, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (init == nullptr || alloc == nullptr || destroy == nullptr || create == nullptr) {
        traceDebug("DLSS evaluate could not load APUSR NGX DX12 bridge exports");
        return false;
    }

    if (g_streamlineNgxFeature != nullptr && release != nullptr) {
        release(g_streamlineNgxFeature);
        g_streamlineNgxFeature = nullptr;
    }

    if (init(0ull, nullptr, g_d3dDevice, nullptr, 0u) != kNgxSuccess) {
        traceDebug("APUSR NGX init failed");
        return false;
    }

    NVSDK_NGX_Parameter* params = nullptr;
    if (alloc(&params) != kNgxSuccess || params == nullptr) {
        traceDebug("APUSR NGX parameter allocation failed");
        return false;
    }

    setNgxUInt(params, "Width", renderWidth);
    setNgxUInt(params, "Height", renderHeight);
    setNgxUInt(params, "OutWidth", outputWidth);
    setNgxUInt(params, "OutHeight", outputHeight);
    setNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Width", renderWidth);
    setNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Height", renderHeight);
    setNgxUInt(params, "TargetWidth", outputWidth);
    setNgxUInt(params, "TargetHeight", outputHeight);
    setNgxInt(params, "PerfQualityValue", static_cast<int>(ngxQualityFromDlssMode(options.mode)));
    setNgxUInt(params, "DLSS.Feature.Create.Flags", 0u);

    NVSDK_NGX_Handle* feature = nullptr;
    const std::uint32_t result = create(reinterpret_cast<ID3D12GraphicsCommandList*>(cmdBuffer), kNgxFeatureSuperSampling, params, &feature);
    destroy(params);
    if (result != kNgxSuccess || feature == nullptr) {
        traceDebug("APUSR NGX feature creation failed");
        return false;
    }

    g_streamlineNgxFeature = feature;
    g_streamlineNgxRenderWidth = renderWidth;
    g_streamlineNgxRenderHeight = renderHeight;
    g_streamlineNgxOutputWidth = outputWidth;
    g_streamlineNgxOutputHeight = outputHeight;
    g_streamlineNgxMode = options.mode;
    return true;
}

bool ensureStreamlineNgxVulkanFeature(
    const CapturedTags& tags,
    const CapturedOptions& options,
    const CapturedConstants& constants,
    void* cmdBuffer)
{
    if (cmdBuffer == nullptr) {
        traceDebug("Vulkan DLSS evaluate missing command buffer");
        return false;
    }

    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_vulkanCaptureMutex);
        device = g_currentVulkanDevice.load(std::memory_order_acquire);
        const auto found = g_vulkanQueueHintsByDevice.find(device);
        if (device != nullptr && found != g_vulkanQueueHintsByDevice.end() && found->second.valid) {
            instance = found->second.instance;
            physicalDevice = found->second.physicalDevice;
        }
    }
    if (instance == nullptr || physicalDevice == nullptr || device == nullptr) {
        traceDebug("Vulkan DLSS evaluate has no slSetVulkanInfo device contract");
        return false;
    }

    std::uint32_t renderWidth = activeWidth(tags.color);
    std::uint32_t renderHeight = activeHeight(tags.color);
    const std::uint32_t taggedOutputWidth = activeWidth(tags.output);
    const std::uint32_t taggedOutputHeight = activeHeight(tags.output);
    const bool optionsHaveOutput = options.outputWidth != 0u && options.outputHeight != 0u &&
        options.outputWidth != 0xffffffffu && options.outputHeight != 0xffffffffu;
    const std::uint32_t outputWidth = optionsHaveOutput ? options.outputWidth : taggedOutputWidth;
    const std::uint32_t outputHeight = optionsHaveOutput ? options.outputHeight : taggedOutputHeight;
    if (renderWidth == 0 || renderHeight == 0) {
        const float scale = dlssScale(options.mode);
        renderWidth = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(outputWidth) * scale + 0.5f));
        renderHeight = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(outputHeight) * scale + 0.5f));
    }
    if (renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0) {
        traceDebug("Vulkan DLSS evaluate missing render/output dimensions");
        return false;
    }

    if (g_streamlineNgxFeature != nullptr &&
        g_streamlineNgxRenderWidth == renderWidth &&
        g_streamlineNgxRenderHeight == renderHeight &&
        g_streamlineNgxOutputWidth == outputWidth &&
        g_streamlineNgxOutputHeight == outputHeight &&
        g_streamlineNgxMode == options.mode) {
        return true;
    }

    auto init = ngxFnCached(g_streamlineNgxVulkanInitFn, "NVSDK_NGX_VULKAN_Init");
    auto alloc = ngxFnCached(g_streamlineNgxVulkanAllocateParametersFn, "NVSDK_NGX_VULKAN_AllocateParameters");
    auto destroy = ngxFnCached(g_streamlineNgxVulkanDestroyParametersFn, "NVSDK_NGX_VULKAN_DestroyParameters");
    auto create = ngxFnCached(g_streamlineNgxVulkanCreateFeatureFn, "NVSDK_NGX_VULKAN_CreateFeature");
    auto release = ngxFnCached(g_streamlineNgxVulkanReleaseFeatureFn, "NVSDK_NGX_VULKAN_ReleaseFeature");
    if (init == nullptr || alloc == nullptr || destroy == nullptr || create == nullptr) {
        traceDebug("Vulkan DLSS evaluate could not load APUSR NGX Vulkan bridge exports");
        return false;
    }

    if (g_streamlineNgxFeature != nullptr && release != nullptr) {
        release(g_streamlineNgxFeature);
        g_streamlineNgxFeature = nullptr;
    }

    if (init(0ull, nullptr, instance, physicalDevice, device) != kNgxSuccess) {
        traceDebug("APUSR NGX Vulkan init failed");
        return false;
    }

    NVSDK_NGX_Parameter* params = nullptr;
    if (alloc(&params) != kNgxSuccess || params == nullptr) {
        traceDebug("APUSR NGX Vulkan parameter allocation failed");
        return false;
    }

    setNgxUInt(params, "Width", renderWidth);
    setNgxUInt(params, "Height", renderHeight);
    setNgxUInt(params, "OutWidth", outputWidth);
    setNgxUInt(params, "OutHeight", outputHeight);
    setNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Width", renderWidth);
    setNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Height", renderHeight);
    setNgxUInt(params, "TargetWidth", outputWidth);
    setNgxUInt(params, "TargetHeight", outputHeight);
    setNgxInt(params, "PerfQualityValue", static_cast<int>(ngxQualityFromDlssMode(options.mode)));
    setNgxUInt(params, "DLSS.Feature.Create.Flags", createFlagsFromStreamlineContract(tags, options, constants, renderWidth, renderHeight));

    NVSDK_NGX_Handle* feature = nullptr;
    const std::uint32_t result = create(cmdBuffer, kNgxFeatureSuperSampling, params, &feature);
    destroy(params);
    if (result != kNgxSuccess || feature == nullptr) {
        traceDebug("APUSR NGX Vulkan feature creation failed");
        return false;
    }

    g_streamlineNgxFeature = feature;
    g_streamlineNgxRenderWidth = renderWidth;
    g_streamlineNgxRenderHeight = renderHeight;
    g_streamlineNgxOutputWidth = outputWidth;
    g_streamlineNgxOutputHeight = outputHeight;
    g_streamlineNgxMode = options.mode;
    apusr::host_discovery::eventf(
        "streamline", "vulkan.dlss.feature_created",
        "render=%ux%u output=%ux%u mode=%u",
        renderWidth, renderHeight, outputWidth, outputHeight, static_cast<unsigned>(options.mode));
    apusr::host_discovery::eventf(
        "streamline", "vulkan.dlss.resource_contract",
        "color=fmt:%u backing:%ux%u active:%u,%u+%ux%u usage:0x%08x depth=fmt:%u backing:%ux%u active:%u,%u+%ux%u usage:0x%08x mv=fmt:%u backing:%ux%u active:%u,%u+%ux%u usage:0x%08x output=fmt:%u backing:%ux%u active:%u,%u+%ux%u usage:0x%08x",
        tags.color.nativeFormat, tags.color.width, tags.color.height, tags.color.extentLeft, tags.color.extentTop, activeWidth(tags.color), activeHeight(tags.color), tags.color.usage,
        tags.depth.nativeFormat, tags.depth.width, tags.depth.height, tags.depth.extentLeft, tags.depth.extentTop, activeWidth(tags.depth), activeHeight(tags.depth), tags.depth.usage,
        tags.motionVectors.nativeFormat, tags.motionVectors.width, tags.motionVectors.height, tags.motionVectors.extentLeft, tags.motionVectors.extentTop, activeWidth(tags.motionVectors), activeHeight(tags.motionVectors), tags.motionVectors.usage,
        tags.output.nativeFormat, tags.output.width, tags.output.height, tags.output.extentLeft, tags.output.extentTop, activeWidth(tags.output), activeHeight(tags.output), tags.output.usage);
    return true;
}

int evaluateDlssThroughApusrNgx(const void* frame, const BaseStructure* const* inputs, std::uint32_t numInputs, void* cmdBuffer)
{
    std::uint32_t viewport = 0;
    (void)findViewportInInputs(inputs, numInputs, viewport);

    CapturedOptions options;
    CapturedTags tags;
    CapturedConstants constants;
    {
        std::lock_guard<std::mutex> lock(g_streamlineStateMutex);
        if (auto it = g_optionsByViewport.find(0u); it != g_optionsByViewport.end()) {
            options = it->second;
        }
        if (auto it = g_optionsByViewport.find(viewport); it != g_optionsByViewport.end()) {
            options = it->second;
        }
        if (auto it = g_legacyTagsByViewport.find(0u); it != g_legacyTagsByViewport.end()) {
            mergeCapturedTags(tags, it->second);
        }
        if (auto it = g_tagsByFrameViewport.find(StreamlineViewportKey{.frame = frame, .viewport = 0u}); it != g_tagsByFrameViewport.end()) {
            mergeCapturedTags(tags, it->second);
        }
        if (auto it = g_legacyTagsByViewport.find(viewport); it != g_legacyTagsByViewport.end()) {
            mergeCapturedTags(tags, it->second);
        }
        if (auto it = g_tagsByFrameViewport.find(StreamlineViewportKey{.frame = frame, .viewport = viewport}); it != g_tagsByFrameViewport.end()) {
            mergeCapturedTags(tags, it->second);
        }
        if (auto it = g_constantsByViewport.find(0u); it != g_constantsByViewport.end()) {
            mergeCapturedConstants(constants, it->second);
        }
        if (auto it = g_constantsByFrameViewport.find(StreamlineViewportKey{.frame = frame, .viewport = 0u}); it != g_constantsByFrameViewport.end()) {
            mergeCapturedConstants(constants, it->second);
        }
        if (auto it = g_constantsByViewport.find(viewport); it != g_constantsByViewport.end()) {
            mergeCapturedConstants(constants, it->second);
        }
        if (auto it = g_constantsByFrameViewport.find(StreamlineViewportKey{.frame = frame, .viewport = viewport}); it != g_constantsByFrameViewport.end()) {
            mergeCapturedConstants(constants, it->second);
        }
        pruneFrameScopedTags(frame);
    }
    captureTagsFromInputs(tags, inputs, numInputs);
    captureConstantsFromInputs(constants, inputs, numInputs);

    if (tags.color.native == nullptr || tags.depth.native == nullptr || tags.motionVectors.native == nullptr || tags.output.native == nullptr) {
        traceDebug("DLSS evaluate missing one or more required resource tags");
        return kResultMissingInputParameter;
    }

    std::lock_guard<std::mutex> lock(g_streamlineNgxMutex);
    const bool vulkan = isVulkanStreamlinePath();
    if (vulkan) {
        if (tags.color.view == nullptr || tags.depth.view == nullptr || tags.motionVectors.view == nullptr || tags.output.view == nullptr ||
            tags.color.nativeFormat == 0u || tags.depth.nativeFormat == 0u || tags.motionVectors.nativeFormat == 0u || tags.output.nativeFormat == 0u) {
            apusr::host_discovery::event(
                "streamline", "vulkan.dlss.resources_invalid",
                "required Vulkan tags must include image, image view, format and extent");
            return kResultMissingInputParameter;
        }
        for (const CapturedResource* required : {&tags.color, &tags.depth, &tags.motionVectors, &tags.output}) {
            if (!validVulkanSubresourceRange(*required)) {
                apusr::host_discovery::event(
                    "streamline", "vulkan.dlss.resources_invalid",
                    "required Vulkan tag contains an invalid subresource range");
                return kResultMissingInputParameter;
            }
        }
        if (!vulkanStorageCapable(tags.output)) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlss.resources_invalid",
                "output image is not storage-capable usage=0x%08x", tags.output.usage);
            return kResultMissingInputParameter;
        }
        if (!ensureStreamlineNgxVulkanFeature(tags, options, constants, cmdBuffer)) {
            return kResultFeatureFailedToLoad;
        }
    } else if (!ensureStreamlineNgxD3D12Feature(tags, options, cmdBuffer)) {
        return kResultFeatureFailedToLoad;
    }

    NgxAllocateParametersFn alloc = nullptr;
    if (vulkan) {
        alloc = ngxFnCached(g_streamlineNgxVulkanAllocateParametersFn, "NVSDK_NGX_VULKAN_AllocateParameters");
    } else {
        alloc = ngxFnCached(g_streamlineNgxAllocateParametersFn, "NVSDK_NGX_D3D12_AllocateParameters");
    }
    if (alloc == nullptr) {
        traceDebug("DLSS evaluate could not load APUSR NGX parameter export");
        return kResultFeatureFailedToLoad;
    }

    NVSDK_NGX_Parameter* params = g_streamlineNgxEvalParams;
    if (params == nullptr) {
        if (alloc(&params) != kNgxSuccess || params == nullptr) {
            traceDebug("DLSS evaluate could not allocate APUSR NGX parameters");
            return kResultFeatureFailedToLoad;
        }
        g_streamlineNgxEvalParams = params;
    } else {
        params->Reset();
    }

    const std::uint32_t taggedRenderWidth = activeWidth(tags.color);
    const std::uint32_t taggedRenderHeight = activeHeight(tags.color);
    const std::uint32_t taggedOutputWidth = activeWidth(tags.output);
    const std::uint32_t taggedOutputHeight = activeHeight(tags.output);
    const bool optionsHaveOutput = options.outputWidth != 0u && options.outputHeight != 0u &&
        options.outputWidth != 0xffffffffu && options.outputHeight != 0xffffffffu;
    const std::uint32_t renderWidth = taggedRenderWidth != 0u ? taggedRenderWidth : g_streamlineNgxRenderWidth;
    const std::uint32_t renderHeight = taggedRenderHeight != 0u ? taggedRenderHeight : g_streamlineNgxRenderHeight;
    const std::uint32_t outputWidth = optionsHaveOutput ? options.outputWidth :
        (taggedOutputWidth != 0u ? taggedOutputWidth : g_streamlineNgxOutputWidth);
    const std::uint32_t outputHeight = optionsHaveOutput ? options.outputHeight :
        (taggedOutputHeight != 0u ? taggedOutputHeight : g_streamlineNgxOutputHeight);

    NgxVkResourceAbi vkColor{};
    NgxVkResourceAbi vkDepth{};
    NgxVkResourceAbi vkMotionVectors{};
    NgxVkResourceAbi vkOutput{};
    NgxVkResourceAbi vkExposure{};
    NgxVkResourceAbi vkReactive{};
    if (vulkan) {
        vkColor = makeNgxVkImageResource(tags.color);
        vkDepth = makeNgxVkImageResource(tags.depth);
        vkMotionVectors = makeNgxVkImageResource(tags.motionVectors);
        vkOutput = makeNgxVkImageResource(tags.output);
        setNgxPointer(params, "Color", &vkColor);
        setNgxPointer(params, "Depth", &vkDepth);
        setNgxPointer(params, "MotionVectors", &vkMotionVectors);
        setNgxPointer(params, "Output", &vkOutput);
        if (tags.exposure.native != nullptr && tags.exposure.view != nullptr) {
            vkExposure = makeNgxVkImageResource(tags.exposure);
            setNgxPointer(params, "ExposureTexture", &vkExposure);
        }
        if (tags.reactive.native != nullptr && tags.reactive.view != nullptr) {
            vkReactive = makeNgxVkImageResource(tags.reactive);
            setNgxPointer(params, "DLSS.Input.Bias.Current.Color.Mask", &vkReactive);
        }
    } else {
        setNgxPointer(params, "Color", tags.color.native);
        setNgxPointer(params, "Depth", tags.depth.native);
        setNgxPointer(params, "MotionVectors", tags.motionVectors.native);
        setNgxPointer(params, "Output", tags.output.native);
        if (tags.exposure.native != nullptr) {
            setNgxPointer(params, "ExposureTexture", tags.exposure.native);
        }
        if (tags.reactive.native != nullptr) {
            setNgxPointer(params, "DLSS.Input.Bias.Current.Color.Mask", tags.reactive.native);
        }
    }

    setNgxUInt(params, "Width", renderWidth);
    setNgxUInt(params, "Height", renderHeight);
    setNgxUInt(params, "OutWidth", outputWidth);
    setNgxUInt(params, "OutHeight", outputHeight);
    setNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Width", renderWidth);
    setNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Height", renderHeight);
    if (vulkan) {
        setNgxUInt(params, "DLSS.Input.Color.Subrect.Base.X", tags.color.extentLeft);
        setNgxUInt(params, "DLSS.Input.Color.Subrect.Base.Y", tags.color.extentTop);
        setNgxUInt(params, "DLSS.Input.Depth.Subrect.Base.X", tags.depth.extentLeft);
        setNgxUInt(params, "DLSS.Input.Depth.Subrect.Base.Y", tags.depth.extentTop);
        setNgxUInt(params, "DLSS.Input.MV.Subrect.Base.X", tags.motionVectors.extentLeft);
        setNgxUInt(params, "DLSS.Input.MV.Subrect.Base.Y", tags.motionVectors.extentTop);
        setNgxUInt(params, "DLSS.Output.Subrect.Base.X", tags.output.extentLeft);
        setNgxUInt(params, "DLSS.Output.Subrect.Base.Y", tags.output.extentTop);
        if (tags.reactive.native != nullptr) {
            setNgxUInt(params, "DLSS.Input.Bias.Current.Color.Subrect.Base.X", tags.reactive.extentLeft);
            setNgxUInt(params, "DLSS.Input.Bias.Current.Color.Subrect.Base.Y", tags.reactive.extentTop);
        }
    }
    setNgxUInt(params, "TargetWidth", outputWidth);
    setNgxUInt(params, "TargetHeight", outputHeight);
    setNgxInt(params, "PerfQualityValue", static_cast<int>(ngxQualityFromDlssMode(options.mode)));
    setNgxFloat(params, "Sharpness", 0.0f);
    setNgxFloat(params, "DLSS.Pre.Exposure", options.preExposure);
    setNgxFloat(params, "DLSS.Exposure.Scale", options.exposureScale);
    setNgxFloat(params, "Jitter.Offset.X", constants.jitterX);
    setNgxFloat(params, "Jitter.Offset.Y", constants.jitterY);
    setNgxFloat(params, "MV.Scale.X", ngxMotionVectorScale(constants.mvScaleX, renderWidth));
    setNgxFloat(params, "MV.Scale.Y", ngxMotionVectorScale(constants.mvScaleY, renderHeight));
    setNgxInt(params, "Reset", constants.reset ? 1 : 0);

    std::uint32_t result = 0u;
    if (vulkan) {
        auto eval = ngxFnCached(g_streamlineNgxVulkanEvaluateFeatureFn, "NVSDK_NGX_VULKAN_EvaluateFeature");
        if (eval == nullptr) {
            traceDebug("Vulkan DLSS evaluate could not load APUSR NGX evaluate export");
            return kResultFeatureFailedToLoad;
        }

        void* device = g_currentVulkanDevice.load(std::memory_order_acquire);
        if (!transitionVulkanDlssBoundary(device, cmdBuffer, tags, true)) {
            return kResultMissingInputParameter;
        }

        ++g_vulkanDlssEvaluateDepth;
        result = eval(cmdBuffer, g_streamlineNgxFeature, params, nullptr);
        --g_vulkanDlssEvaluateDepth;

        // The host owns these images. Restore the exact layouts from its
        // Streamline tags even when APUSR evaluation reports a failure.
        if (!transitionVulkanDlssBoundary(device, cmdBuffer, tags, false)) {
            return kResultFeatureFailedToLoad;
        }
    } else {
        auto eval = ngxFnCached(g_streamlineNgxEvaluateFeatureFn, "NVSDK_NGX_D3D12_EvaluateFeature");
        if (eval == nullptr) {
            traceDebug("DLSS evaluate could not load APUSR NGX evaluate export");
            return kResultFeatureFailedToLoad;
        }
        result = eval(reinterpret_cast<ID3D12GraphicsCommandList*>(cmdBuffer), g_streamlineNgxFeature, params, nullptr);
    }

    if (result != kNgxSuccess) {
        const std::uint32_t previous = g_lastStreamlineNgxFailure.exchange(result, std::memory_order_relaxed);
        if (previous != result) {
            apusr::host_discovery::eventf(
                "streamline", vulkan ? "vulkan.dlss.evaluate_failed" : "dlss.evaluate_failed",
                "ngx_result=0x%08x", result);
        }
        return kResultFeatureFailedToLoad;
    }
    const std::uint32_t previousFailure = g_lastStreamlineNgxFailure.exchange(0u, std::memory_order_relaxed);
    if (previousFailure != 0u) {
        apusr::host_discovery::eventf(
            "streamline", vulkan ? "vulkan.dlss.evaluate_recovered" : "dlss.evaluate_recovered",
            "previous_ngx_result=0x%08x", previousFailure);
    }
    return kResultOk;
}

struct StreamlineBackingVersion {
    std::uint32_t major = 0u;
    std::uint32_t minor = 0u;
    std::uint32_t patch = 0u;
    std::uint32_t build = 0u;
    bool valid = false;
};

StreamlineBackingVersion queryModuleVersion(HMODULE module)
{
    if (module == nullptr) {
        return {};
    }

    std::array<wchar_t, 32768> path{};
    const DWORD pathLength = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (pathLength == 0u || pathLength >= path.size()) {
        return {};
    }

    DWORD ignored = 0u;
    const DWORD versionSize = GetFileVersionInfoSizeW(path.data(), &ignored);
    if (versionSize == 0u) {
        return {};
    }

    std::vector<std::byte> versionData(versionSize);
    if (!GetFileVersionInfoW(path.data(), 0u, versionSize, versionData.data())) {
        return {};
    }

    VS_FIXEDFILEINFO* fixedInfo = nullptr;
    UINT fixedInfoSize = 0u;
    if (!VerQueryValueW(
            versionData.data(), L"\\", reinterpret_cast<void**>(&fixedInfo), &fixedInfoSize) ||
        fixedInfo == nullptr || fixedInfoSize < sizeof(VS_FIXEDFILEINFO) ||
        fixedInfo->dwSignature != 0xFEEF04BDu) {
        return {};
    }

    return {
        .major = HIWORD(fixedInfo->dwFileVersionMS),
        .minor = LOWORD(fixedInfo->dwFileVersionMS),
        .patch = HIWORD(fixedInfo->dwFileVersionLS),
        .build = LOWORD(fixedInfo->dwFileVersionLS),
        .valid = true,
    };
}

bool ordinalMatchesNamedExport(HMODULE module, WORD ordinal, const char* name)
{
    if (module == nullptr || name == nullptr) {
        return false;
    }
    const FARPROC byName = GetProcAddress(module, name);
    const FARPROC byOrdinal = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
    return byName != nullptr && byName == byOrdinal;
}

bool backingMatchesCompiledAbi(HMODULE module)
{
    if (module == nullptr || !ordinalMatchesNamedExport(module, 29u, "slSetTag")) {
        return false;
    }

#if APUSR_SL_EXPECTS_TAG_FOR_FRAME
    return ordinalMatchesNamedExport(module, 30u, "slSetTagForFrame") &&
           ordinalMatchesNamedExport(module, 31u, "slSetVulkanInfo") &&
           ordinalMatchesNamedExport(module, 32u, "slShutdown") &&
           ordinalMatchesNamedExport(module, 33u, "slUpgradeInterface") &&
           ordinalMatchesNamedExport(module, 34u, "vkAcquireNextImageKHR") &&
           ordinalMatchesNamedExport(module, APUSR_SL_STOCK_MAX_ORDINAL, "vkWaitSemaphores");
#else
    return GetProcAddress(module, "slSetTagForFrame") == nullptr &&
           ordinalMatchesNamedExport(module, 30u, "slSetVulkanInfo") &&
           ordinalMatchesNamedExport(module, 31u, "slShutdown") &&
           ordinalMatchesNamedExport(module, 32u, "slUpgradeInterface") &&
           ordinalMatchesNamedExport(module, 33u, "vkAcquireNextImageKHR") &&
           ordinalMatchesNamedExport(module, APUSR_SL_STOCK_MAX_ORDINAL, "vkWaitSemaphores");
#endif
}

HMODULE loadOriginalInterposer()
{
    if (g_original != nullptr) {
        return g_original;
    }

#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    // Attach mode never substitutes or loads a renamed Streamline PE. The game
    // owns the real module, including its exact version resources and ordinals.
    g_original = GetModuleHandleW(L"sl.interposer.dll");
    return g_original;
#else
    wchar_t selfPath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), selfPath, MAX_PATH);
    if (len != 0 && len < MAX_PATH) {
        if (wchar_t* slash = wcsrchr(selfPath, L'\\')) {
            *(slash + 1) = L'\0';
            wcscat_s(selfPath, kOriginalInterposerName);
            g_original = LoadLibraryW(selfPath);
        }
    }
    if (g_original == nullptr) {
        g_original = LoadLibraryW(kOriginalInterposerName);
    }
    if (g_original != nullptr && !isVulkanStreamlinePath()) {
        // The DX compatibility path still relies on loader/plugin interception.
        // Vulkan owns DLSS in this proxy and leaves the backing interposer alone.
        apusrRefreshIdentitySpoofHooks();
    }
    return g_original;
#endif
}

HMODULE loadSystemModule(const wchar_t* moduleName, HMODULE& cache)
{
    if (cache != nullptr) {
        return cache;
    }
    wchar_t systemDir[MAX_PATH]{};
    const UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (len != 0 && len < MAX_PATH) {
        wchar_t path[MAX_PATH]{};
        wcscpy_s(path, systemDir);
        wcscat_s(path, L"\\");
        wcscat_s(path, moduleName);
        cache = LoadLibraryW(path);
    }
    if (cache == nullptr) {
        cache = LoadLibraryW(moduleName);
    }
    return cache;
}

bool isDxgiExport(const char* name) noexcept
{
    return name != nullptr &&
           (std::strcmp(name, "CreateDXGIFactory") == 0 ||
            std::strcmp(name, "CreateDXGIFactory1") == 0 ||
            std::strcmp(name, "CreateDXGIFactory2") == 0 ||
            std::strcmp(name, "DXGIGetDebugInterface1") == 0 ||
            std::strcmp(name, "DXGIDeclareAdapterRemovalSupport") == 0);
}

bool isD3d11Export(const char* name) noexcept
{
    return name != nullptr &&
           (std::strcmp(name, "D3D11CreateDevice") == 0 ||
            std::strcmp(name, "D3D11CreateDeviceAndSwapChain") == 0);
}

bool isD3d12Export(const char* name) noexcept
{
    return name != nullptr &&
           (std::strcmp(name, "D3D12CreateDevice") == 0 ||
            std::strcmp(name, "D3D12GetDebugInterface") == 0 ||
            std::strcmp(name, "D3D12GetInterface") == 0 ||
            std::strcmp(name, "D3D12EnableExperimentalFeatures") == 0 ||
            std::strcmp(name, "D3D12CreateRootSignatureDeserializer") == 0 ||
            std::strcmp(name, "D3D12CreateVersionedRootSignatureDeserializer") == 0 ||
            std::strcmp(name, "D3D12SerializeRootSignature") == 0 ||
            std::strcmp(name, "D3D12SerializeVersionedRootSignature") == 0);
}

FARPROC platformProc(const char* name)
{
    HMODULE mod = nullptr;
    if (isDxgiExport(name)) {
        mod = loadSystemModule(L"dxgi.dll", g_systemDxgi);
    } else if (isD3d11Export(name)) {
        mod = loadSystemModule(L"d3d11.dll", g_systemD3d11);
    } else if (isD3d12Export(name)) {
        mod = loadSystemModule(L"d3d12.dll", g_systemD3d12);
    }
    return mod != nullptr ? GetProcAddress(mod, name) : nullptr;
}

template <typename Fn>
Fn vulkanSystemFn(const char* name)
{
    HMODULE mod = loadSystemModule(L"vulkan-1.dll", g_systemVulkan);
    return mod != nullptr ? reinterpret_cast<Fn>(GetProcAddress(mod, name)) : nullptr;
}

FARPROC originalProc(const char* name)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    // GetProcAddress on a detoured export returns the detour entry point. Calls
    // from APUSR wrappers must use Detours' trampoline or they recurse.
    if (PVOID trampoline = attachedStreamlineTrampoline(name)) {
        return reinterpret_cast<FARPROC>(trampoline);
    }
#endif
    HMODULE mod = loadOriginalInterposer();
    if (mod != nullptr) {
        if (FARPROC proc = GetProcAddress(mod, name)) {
            return proc;
        }
    }
    return platformProc(name);
}

template <typename Fn>
Fn originalFn(const char* name)
{
    return reinterpret_cast<Fn>(originalProc(name));
}

std::int32_t WINAPI apusrBackingVkQueuePresentKHR(void* queue, const void* presentInfo)
{
    if (auto fn = originalFn<VkQueuePresentKHRFn>("vkQueuePresentKHR")) {
        return fn(queue, presentInfo);
    }
    return -3; // VK_ERROR_INITIALIZATION_FAILED
}

namespace {

// kVkSuccess and kVkErrorInitializationFailed are shared with the Vulkan
// device-creation bridge declared in the enclosing frontend namespace. Keep
// one definition of each so MSVC's unqualified lookup cannot see competing
// anonymous-namespace constants.
constexpr std::int32_t kVkNotReady = 1;
constexpr std::int32_t kVkTimeout = 2;
constexpr std::int32_t kVkSuboptimalKhr = 1000001003;
constexpr std::uint32_t kVkStructureTypePresentInfoKhr = 1000001001u;
constexpr std::uint32_t kVkImageUsageTransferSrcBit = 0x00000001u;
constexpr std::uint32_t kVkImageUsageTransferDstBit = 0x00000002u;
constexpr std::uint32_t kVkImageUsageStorageBit = 0x00000008u;
constexpr std::uint32_t kVkColorSpaceSrgbNonlinearKhr = 0u;

bool acceptedVkPresent(const std::int32_t result) noexcept
{
    return result == kVkSuccess || result == kVkSuboptimalKhr;
}

std::int32_t framegenPresentImageCallback(
    void* userContext,
    void* queue,
    void* swapchain,
    const std::uint32_t imageIndex,
    void* const* waitSemaphores,
    const std::uint32_t waitSemaphoreCount,
    const std::uint64_t frameId,
    const bool profileCapture,
    const bool generatedFrame) noexcept
{
    void* swapchains[1]{swapchain};
    std::uint32_t imageIndices[1]{imageIndex};
    VkPresentInfoAbi info{};
    info.sType = static_cast<std::int32_t>(kVkStructureTypePresentInfoKhr);
    info.pNext = nullptr;
    info.waitSemaphoreCount = waitSemaphoreCount;
    info.pWaitSemaphores = waitSemaphores;
    info.swapchainCount = 1u;
    info.pSwapchains = swapchains;
    info.pImageIndices = imageIndices;
    info.pResults = nullptr;

    const auto presentStart = std::chrono::steady_clock::now();
    const auto issuePresent = [&]() noexcept {
        // Both physical presents are APUSR-owned in proxy mode. Generated
        // frames intentionally skip Anti-Lag's real-present marker; source
        // frames use the host-capture source helper so exactly one real-frame
        // marker is emitted without re-entering the backing Streamline layer.
        if (profileCapture) {
            const std::uint32_t flags = APUSR_VK_HOST_PRESENT_PROFILE_CAPTURE |
                (generatedFrame ? APUSR_VK_HOST_PRESENT_GENERATED : 0u);
            return apusrVulkanHostQueuePresentFrameGeneration(queue, &info, frameId, flags);
        }
        return generatedFrame
            ? apusrVulkanHostQueuePresentDirect(queue, &info)
            : apusrVulkanHostQueuePresentSource(queue, &info);
    };

    // The off-screen proxy uses distinct generation and presentation queues.
    // Keep the shared-queue guard only for legacy callers that can still route
    // through this callback outside the current proxy path.
    const auto* runtime = static_cast<const VulkanFramegenRuntime*>(userContext);
    const bool sharedComputeAndPresentQueue = runtime && runtime->queue == queue;
    std::int32_t result = kVkSuccess;
    if (sharedComputeAndPresentQueue) {
        std::lock_guard<std::mutex> queueLock(g_vulkanFramegenSharedQueueMutex);
        result = issuePresent();
    } else {
        result = issuePresent();
    }

    const auto presentUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - presentStart).count();
    if (presentUs >= 20000 &&
        g_vulkanFramegenSlowPresentLogs.fetch_add(1u, std::memory_order_relaxed) < 8u) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.present_api_slow",
            "kind=%s queue=%p swapchain=%p image_index=%u duration_us=%lld result=%d shared_compute_queue=%u",
            generatedFrame ? "generated" : "source", queue, swapchain, imageIndex,
            static_cast<long long>(presentUs), result, sharedComputeAndPresentQueue ? 1u : 0u);
    }

    if (acceptedVkPresent(result)) {
        g_dlssgPresentedSinceQuery.fetch_add(1u, std::memory_order_relaxed);
        if (generatedFrame && !g_dlssgFirstGeneratedLogged.exchange(true, std::memory_order_relaxed)) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.first_generated_present",
                "swapchain=%p image_index=%u", swapchain, imageIndex);
        }
    } else {
        apusr::host_discovery::eventf(
            "streamline", generatedFrame ? "vulkan.dlssg.generated_present_failed" : "vulkan.dlssg.source_present_failed",
            "swapchain=%p image_index=%u result=%d", swapchain, imageIndex, result);
    }
    return result;
}

std::int32_t framegenAcquireImageCallback(
    void*,
    void* device,
    void* swapchain,
    const std::uint64_t timeout,
    void* semaphore,
    std::uint32_t* outImageIndex) noexcept
{
    return apusrVulkanHostAcquireNextImageForFrameGeneration(
        device, swapchain, timeout, semaphore, outImageIndex);
}

bool framegenReleaseImageCallback(
    void*,
    void* device,
    void* swapchain,
    const std::uint32_t imageIndex) noexcept
{
    return apusrVulkanHostReleaseRealSwapchainImage(device, swapchain, imageIndex);
}

bool framegenTimelineReadyCallback(
    void* userContext,
    void* timelineSemaphore,
    const std::uint64_t value) noexcept
{
    const auto* runtime = static_cast<const VulkanFramegenRuntime*>(userContext);
    if (!runtime || !runtime->presentSubmit || !timelineSemaphore || value == 0u)
        return false;
    if (timelineSemaphore == apusrFramegenVulkanGenerationCompletionSemaphore(runtime->presentSubmit))
        return apusrFramegenVulkanCompletedGenerationValue(runtime->presentSubmit) >= value;
    if (timelineSemaphore == apusrFramegenVulkanProducerReadySemaphore(runtime->presentSubmit))
        return apusrFramegenVulkanCompletedProducerReadyValue(runtime->presentSubmit) >= value;
    return false;
}

bool framegenTimelineWaitCallback(
    void* userContext,
    void* timelineSemaphore,
    const std::uint64_t value,
    const std::uint64_t timeoutNanoseconds) noexcept
{
    const auto* runtime = static_cast<const VulkanFramegenRuntime*>(userContext);
    if (!runtime || !runtime->presentSubmit || !timelineSemaphore || value == 0u)
        return false;
    if (timelineSemaphore == apusrFramegenVulkanGenerationCompletionSemaphore(runtime->presentSubmit))
        return apusrFramegenWaitVulkanGenerationValue(runtime->presentSubmit, value, timeoutNanoseconds);
    if (timelineSemaphore == apusrFramegenVulkanProducerReadySemaphore(runtime->presentSubmit))
        return apusrFramegenWaitVulkanProducerReadyValue(runtime->presentSubmit, value, timeoutNanoseconds);
    return false;
}

void framegenGenerationDecisionCallback(
    void*,
    const std::uint64_t frameId,
    const std::uint32_t generationSlotIndex,
    const std::uint64_t generationCompletionValue,
    const bool generatedFramePresented) noexcept
{
    if (generationCompletionValue == 0u ||
        generationSlotIndex >= APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS)
        return;
    g_vulkanFramegenGeneratedSlotDecisionValue[generationSlotIndex].store(
        generationCompletionValue, std::memory_order_release);
    if (!generatedFramePresented) {
        const std::uint64_t skipped = g_vulkanFramegenSkippedDeadlineCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (skipped <= 4u || (skipped & (skipped - 1u)) == 0u) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.generated_deadline_missed",
                "frame=%llu generation_value=%llu skipped_total=%llu",
                static_cast<unsigned long long>(frameId),
                static_cast<unsigned long long>(generationCompletionValue),
                static_cast<unsigned long long>(skipped));
        }
    }
}

ApusrFramegenResult framegenPrepareDirectPresentCallback(
    void* userContext,
    const std::uint32_t imageIndex,
    void* generationTimelineSemaphore,
    const std::uint64_t generationTimelineValue,
    void** outPresentWaitSemaphore) noexcept
{
    auto* runtime = static_cast<VulkanFramegenRuntime*>(userContext);
    if (!runtime || !runtime->directWsi)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    return apusrFramegenPrepareVulkanDirectWsiPresent(
        runtime->directWsi,
        imageIndex,
        generationTimelineSemaphore,
        generationTimelineValue,
        outPresentWaitSemaphore);
}

void framegenDirectGenerationDecisionCallback(
    void* userContext,
    const std::uint64_t frameId,
    const std::uint32_t imageIndex,
    void* generationTimelineSemaphore,
    const std::uint64_t generationTimelineValue,
    const bool generatedFramePresented,
    const bool presentBridgePrepared) noexcept
{
    auto* runtime = static_cast<VulkanFramegenRuntime*>(userContext);
    if (!runtime || !runtime->directWsi || generationTimelineValue == 0u)
        return;

    if (generatedFramePresented) {
        apusrFramegenMarkVulkanDirectWsiPresented(runtime->directWsi, imageIndex);
        return;
    }

    const std::uint64_t skipped =
        g_vulkanFramegenSkippedDeadlineCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (skipped <= 4u || (skipped & (skipped - 1u)) == 0u) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.generated_direct_dropped",
            "frame=%llu image=%u generation_value=%llu bridge_prepared=%u skipped_total=%llu",
            static_cast<unsigned long long>(frameId), imageIndex,
            static_cast<unsigned long long>(generationTimelineValue),
            presentBridgePrepared ? 1u : 0u,
            static_cast<unsigned long long>(skipped));
    }

    if (!presentBridgePrepared) {
        // Normal deadline miss: FI may still be writing the acquired WSI image.
        // Keep it private and release it only after the generation timeline is
        // complete. No late present and no WSI queue stall.
        apusrFramegenDeferVulkanDirectWsiRelease(
            runtime->directWsi, imageIndex,
            generationTimelineSemaphore, generationTimelineValue);
    }
    // If the bridge was already submitted and vkQueuePresentKHR itself failed,
    // binary-semaphore consumption is WSI-error dependent. Keep the image
    // quarantined; the deferred presentation error tears down the runtime and
    // its cold drain/release path rather than guessing at reusable state.
}

std::int32_t framegenPresentSourceRelativeCallback(
    void*,
    void* queue,
    void* swapchain,
    const std::uint32_t imageIndex,
    void* const* waitSemaphores,
    const std::uint32_t waitSemaphoreCount,
    const std::uint64_t frameId,
    const bool profileCapture,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled) noexcept
{
    if (outScheduled == nullptr)
        return kVkErrorInitializationFailed;
    *outScheduled = false;

    void* swapchains[1]{swapchain};
    std::uint32_t imageIndices[1]{imageIndex};
    VkPresentInfoAbi info{};
    info.sType = static_cast<std::int32_t>(kVkStructureTypePresentInfoKhr);
    info.pNext = nullptr;
    info.waitSemaphoreCount = waitSemaphoreCount;
    info.pWaitSemaphores = waitSemaphores;
    info.swapchainCount = 1u;
    info.pSwapchains = swapchains;
    info.pImageIndices = imageIndices;
    info.pResults = nullptr;

    if (profileCapture) {
        return apusrVulkanHostQueuePresentSourceRelativeFrameGeneration(
            queue, &info, frameId, APUSR_VK_HOST_PRESENT_PROFILE_CAPTURE,
            minimumPreviousImageVisibleNs, outScheduled);
    }
    return apusrVulkanHostQueuePresentSourceRelative(
        queue, &info, minimumPreviousImageVisibleNs, outScheduled);
}

std::int32_t framegenPresentGeneratedRelativeCallback(
    void*,
    void* queue,
    void* swapchain,
    const std::uint32_t imageIndex,
    void* const* waitSemaphores,
    const std::uint32_t waitSemaphoreCount,
    const std::uint64_t frameId,
    const bool profileCapture,
    const std::uint64_t minimumPreviousImageVisibleNs,
    bool* outScheduled) noexcept
{
    if (outScheduled == nullptr)
        return kVkErrorInitializationFailed;
    *outScheduled = false;

    void* swapchains[1]{swapchain};
    std::uint32_t imageIndices[1]{imageIndex};
    VkPresentInfoAbi info{};
    info.sType = static_cast<std::int32_t>(kVkStructureTypePresentInfoKhr);
    info.pNext = nullptr;
    info.waitSemaphoreCount = waitSemaphoreCount;
    info.pWaitSemaphores = waitSemaphores;
    info.swapchainCount = 1u;
    info.pSwapchains = swapchains;
    info.pImageIndices = imageIndices;
    info.pResults = nullptr;

    if (profileCapture) {
        return apusrVulkanHostQueuePresentGeneratedRelativeFrameGeneration(
            queue, &info, frameId,
            APUSR_VK_HOST_PRESENT_GENERATED | APUSR_VK_HOST_PRESENT_PROFILE_CAPTURE,
            minimumPreviousImageVisibleNs, outScheduled);
    }
    return apusrVulkanHostQueuePresentGeneratedRelative(
        queue, &info, minimumPreviousImageVisibleNs, outScheduled);
}

ApusrFramegenResult framegenPrepareGeneratedReplayCallback(
    void* userContext,
    const std::uint32_t sourceSlotIndex,
    const std::uint64_t completionValue,
    void* const* producerWaitSemaphores,
    const std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore,
    const std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex,
    void** outWaitSemaphore,
    bool* outCopied) noexcept
{
    auto* runtime = static_cast<VulkanFramegenRuntime*>(userContext);
    if (!runtime || sourceSlotIndex >= APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS ||
        !runtime->generatedReplay[sourceSlotIndex] ||
        producerWaitSemaphoreCount != 0u || producerWaitSemaphores != nullptr ||
        !upstreamWaitSemaphore || upstreamWaitValue == 0u)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    auto* const generatedReplay = runtime->generatedReplay[sourceSlotIndex];

    // Real-time scheduling reserves the generated-output consumed value only
    // when the generation timeline is actually ready before the midpoint
    // deadline. A skipped generated frame therefore leaves no unsignaled replay
    // dependency that could poison all later generations.
    const std::uint64_t reservedCompletionValue = completionValue != 0u
        ? completionValue
        : apusrFramegenReserveVulkanReplayCompletion(generatedReplay);
    if (reservedCompletionValue == 0u)
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    ApusrFramegenVulkanSourceReplayMetrics metrics{};
    const ApusrFramegenResult result = apusrFramegenPrepareVulkanReplay(
        generatedReplay, sourceSlotIndex, reservedCompletionValue,
        nullptr, 0u, upstreamWaitSemaphore, upstreamWaitValue,
        outImageIndex, outWaitSemaphore, outCopied, &metrics);
    if (result == APUSR_FRAMEGEN_OK && outCopied && *outCopied) {
        g_vulkanFramegenGeneratedSlotReplayValue[sourceSlotIndex].store(
            reservedCompletionValue, std::memory_order_release);
        if (!g_vulkanFramegenGeneratedReplayLogged.exchange(true, std::memory_order_relaxed)) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.first_generated_replay",
                "source_slot=%u target_image=%u generation_value=%llu consumed_value=%llu copied=1 wait_previous_us=%llu acquire_us=%llu record_us=%llu submit_us=%llu",
                sourceSlotIndex, outImageIndex ? *outImageIndex : 0u,
                static_cast<unsigned long long>(upstreamWaitValue),
                static_cast<unsigned long long>(reservedCompletionValue),
                static_cast<unsigned long long>(metrics.waitPreviousUs),
                static_cast<unsigned long long>(metrics.acquireUs),
                static_cast<unsigned long long>(metrics.recordUs),
                static_cast<unsigned long long>(metrics.submitUs));
        }
    }
    if (result != APUSR_FRAMEGEN_OK || (outCopied && !*outCopied)) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.generated_replay_failed",
            "source_slot=%u generation_value=%llu consumed_value=%llu result=%d copied=%u wait_previous_us=%llu acquire_us=%llu record_us=%llu submit_us=%llu",
            sourceSlotIndex, static_cast<unsigned long long>(upstreamWaitValue),
            static_cast<unsigned long long>(reservedCompletionValue), static_cast<int>(result),
            outCopied && *outCopied ? 1u : 0u,
            static_cast<unsigned long long>(metrics.waitPreviousUs),
            static_cast<unsigned long long>(metrics.acquireUs),
            static_cast<unsigned long long>(metrics.recordUs),
            static_cast<unsigned long long>(metrics.submitUs));
    }
    return result;
}

ApusrFramegenResult framegenPrepareSourceReplayCallback(
    void* userContext,
    const std::uint32_t sourceSlotIndex,
    const std::uint64_t completionValue,
    void* const* producerWaitSemaphores,
    const std::uint32_t producerWaitSemaphoreCount,
    void* upstreamWaitSemaphore,
    const std::uint64_t upstreamWaitValue,
    std::uint32_t* outImageIndex,
    void** outWaitSemaphore,
    bool* outSourceCopied) noexcept
{
    auto* runtime = static_cast<VulkanFramegenRuntime*>(userContext);
    if (!runtime || !runtime->sourceReplay ||
        ((upstreamWaitSemaphore == nullptr) != (upstreamWaitValue == 0u)))
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    ApusrFramegenVulkanSourceReplayMetrics metrics{};
    const ApusrFramegenResult result = apusrFramegenPrepareVulkanReplay(
        runtime->sourceReplay, sourceSlotIndex, completionValue,
        producerWaitSemaphores, producerWaitSemaphoreCount, upstreamWaitSemaphore, upstreamWaitValue,
        outImageIndex, outWaitSemaphore, outSourceCopied, &metrics);
    const std::uint64_t completedValue = apusrFramegenVulkanCompletedReplayValue(runtime->sourceReplay);
    const std::uint64_t prepareUs = metrics.waitPreviousUs + metrics.acquireUs + metrics.recordUs + metrics.submitUs;
    if (result == APUSR_FRAMEGEN_OK) {
        if (!g_vulkanFramegenSourceReplayLogged.exchange(true, std::memory_order_relaxed)) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.first_source_replay",
                "source_slot=%u target_image=%u completion_value=%llu completed_value=%llu copied=%u wait_previous_us=%llu acquire_us=%llu record_us=%llu submit_us=%llu",
                sourceSlotIndex, outImageIndex ? *outImageIndex : 0u,
                static_cast<unsigned long long>(completionValue),
                static_cast<unsigned long long>(completedValue),
                outSourceCopied && *outSourceCopied ? 1u : 0u,
                static_cast<unsigned long long>(metrics.waitPreviousUs),
                static_cast<unsigned long long>(metrics.acquireUs),
                static_cast<unsigned long long>(metrics.recordUs),
                static_cast<unsigned long long>(metrics.submitUs));
        }
        if (completionValue <= 4u || (completionValue & (completionValue - 1u)) == 0u) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.source_replay_progress",
                "reserved_value=%llu completed_value=%llu source_slot=%u target_image=%u copied=%u wait_previous_us=%llu acquire_us=%llu record_us=%llu submit_us=%llu",
                static_cast<unsigned long long>(completionValue),
                static_cast<unsigned long long>(completedValue),
                sourceSlotIndex, outImageIndex ? *outImageIndex : 0u,
                outSourceCopied && *outSourceCopied ? 1u : 0u,
                static_cast<unsigned long long>(metrics.waitPreviousUs),
                static_cast<unsigned long long>(metrics.acquireUs),
                static_cast<unsigned long long>(metrics.recordUs),
                static_cast<unsigned long long>(metrics.submitUs));
        }
        if (prepareUs >= 5000u &&
            g_vulkanFramegenSlowReplayLogs.fetch_add(1u, std::memory_order_relaxed) < 12u) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.source_replay_prepare_slow",
                "completion_value=%llu completed_value=%llu total_us=%llu wait_previous_us=%llu acquire_us=%llu record_us=%llu submit_us=%llu",
                static_cast<unsigned long long>(completionValue),
                static_cast<unsigned long long>(completedValue),
                static_cast<unsigned long long>(prepareUs),
                static_cast<unsigned long long>(metrics.waitPreviousUs),
                static_cast<unsigned long long>(metrics.acquireUs),
                static_cast<unsigned long long>(metrics.recordUs),
                static_cast<unsigned long long>(metrics.submitUs));
        }
    } else {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.source_replay_failed",
            "source_slot=%u completion_value=%llu completed_value=%llu result=%d wait_previous_us=%llu acquire_us=%llu record_us=%llu submit_us=%llu",
            sourceSlotIndex, static_cast<unsigned long long>(completionValue),
            static_cast<unsigned long long>(completedValue), static_cast<int>(result),
            static_cast<unsigned long long>(metrics.waitPreviousUs),
            static_cast<unsigned long long>(metrics.acquireUs),
            static_cast<unsigned long long>(metrics.recordUs),
            static_cast<unsigned long long>(metrics.submitUs));
    }
    return result;
}

void clearPendingFramegenPresentsLocked() noexcept
{
    g_pendingFramegenPresents = {};
    g_pendingFramegenPresentHead = 0u;
    g_pendingFramegenPresentSize = 0u;
}

void queuePendingFramegenPresentLocked(const void* frame, const std::uint64_t frameIndex) noexcept
{
    if (g_pendingFramegenPresentSize == kPendingFramegenPresentCount) {
        g_pendingFramegenPresentHead = (g_pendingFramegenPresentHead + 1u) % kPendingFramegenPresentCount;
        --g_pendingFramegenPresentSize;
    }
    const std::size_t tail = (g_pendingFramegenPresentHead + g_pendingFramegenPresentSize) % kPendingFramegenPresentCount;
    LARGE_INTEGER presentStart{};
    (void)QueryPerformanceCounter(&presentStart);
    g_pendingFramegenPresents[tail] = {
        .frame = frame,
        .frameIndex = frameIndex,
        .presentStartQpc = presentStart.QuadPart,
    };
    ++g_pendingFramegenPresentSize;
}

bool popPendingFramegenPresentLocked(PendingFramegenPresent& out) noexcept
{
    if (g_pendingFramegenPresentSize == 0u)
        return false;
    out = g_pendingFramegenPresents[g_pendingFramegenPresentHead];
    g_pendingFramegenPresents[g_pendingFramegenPresentHead] = {};
    g_pendingFramegenPresentHead = (g_pendingFramegenPresentHead + 1u) % kPendingFramegenPresentCount;
    --g_pendingFramegenPresentSize;
    return true;
}

void destroyVulkanFramegenRuntimeLocked() noexcept
{
    VulkanFramegenRuntime& runtime = g_vulkanFramegenRuntime;
    if (runtime.presentation)
        apusrFramegenDrainVulkanPresentation(runtime.presentation);

    // Virtual slots retain external timeline dependencies. Clear those while
    // the replay/generation timelines still exist, before their owners die.
    if (runtime.swapchain && runtime.presentSubmit && runtime.sourceReplay)
        (void)apusrVulkanHostWaitVirtualSwapchainReleaseDependencies(runtime.swapchain);

    if (runtime.presentation) {
        apusrFramegenDestroyVulkanPresentation(runtime.presentation, {});
        runtime.presentation = nullptr;
    }
    // The presentation worker owns replay submissions. Joining it first keeps
    // replay teardown from racing new work on the presentation queue.
    for (auto*& generatedReplay : runtime.generatedReplay) {
        if (generatedReplay) {
            apusrFramegenDestroyVulkanSourceReplayContext(generatedReplay, {});
            generatedReplay = nullptr;
        }
    }
    if (runtime.sourceReplay) {
        apusrFramegenDestroyVulkanSourceReplayContext(runtime.sourceReplay, {});
        runtime.sourceReplay = nullptr;
    }
    // Queue-idle in submit-context destruction proves the FI/OF command
    // resources and any directly-written WSI image are no longer referenced
    // before direct-WSI/session/present-image teardown.
    if (runtime.presentSubmit) {
        apusrFramegenDestroyVulkanPresentSubmitContext(runtime.presentSubmit, {});
        runtime.presentSubmit = nullptr;
    }
    if (runtime.directWsi) {
        apusrFramegenDestroyVulkanDirectWsiContext(runtime.directWsi, {});
        runtime.directWsi = nullptr;
    }
    if (runtime.session) {
        apusrFramegenDestroyVulkanSession(runtime.session, {});
        runtime.session = nullptr;
    }
    if (runtime.presentImages) {
        apusrFramegenDestroyVulkanPresentImageContext(runtime.presentImages, {});
        runtime.presentImages = nullptr;
    }
    runtime = {};
    for (std::uint32_t slot = 0u; slot < APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS; ++slot) {
        g_vulkanFramegenGeneratedSlotGenerationValue[slot].store(0u, std::memory_order_release);
        g_vulkanFramegenGeneratedSlotDecisionValue[slot].store(0u, std::memory_order_release);
        g_vulkanFramegenGeneratedSlotReplayValue[slot].store(0u, std::memory_order_release);
    }
}

void WINAPI apusrOwnedVkDestroySwapchainKHR(void*, void* swapchain) noexcept
{
    if (!swapchain)
        return;

    // Host capture invokes this before the real vkDestroySwapchainKHR. APUSR
    // owns views/semaphores tied to the WSI images, so their lifetime must end
    // while those images are still valid. The DLSS-G control state survives so
    // a recreated swapchain can build a fresh runtime on its first present.
    std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
    if (g_vulkanFramegenRuntime.swapchain == swapchain) {
        destroyVulkanFramegenRuntimeLocked();
        clearPendingFramegenPresentsLocked();
    }
    void* observed = g_lastObservedPresentSwapchain.load(std::memory_order_relaxed);
    if (observed == swapchain)
        g_lastObservedPresentSwapchain.compare_exchange_strong(
            observed, nullptr, std::memory_order_relaxed, std::memory_order_relaxed);
}

bool resolvePresentQueueInfo(void* device, void* queue, ApusrVulkanHostQueueInfo& out) noexcept
{
    ApusrVulkanHostDeviceInfo deviceInfo{};
    if (!apusrVulkanHostGetDeviceInfo(device, &deviceInfo))
        return false;
    for (std::uint32_t i = 0; i < deviceInfo.queueCount; ++i) {
        ApusrVulkanHostQueueInfo candidate{};
        if (apusrVulkanHostGetQueueInfo(device, i, &candidate) && candidate.queue == queue) {
            out = candidate;
            return true;
        }
    }
    return false;
}

bool selectFramegenQueueInfos(
    void* device,
    const ApusrVulkanHostQueueInfo& applicationQueueInfo,
    ApusrVulkanHostQueueInfo& inputOut,
    ApusrVulkanHostQueueInfo& generationOut,
    ApusrVulkanHostQueueInfo& presentationOut,
    ApusrVulkanHostQueueInfo& acquireOut,
    ApusrVulkanHostQueueInfo& replayOut) noexcept
{
    ApusrVulkanHostDeviceInfo deviceInfo{};
    if (!apusrVulkanHostGetDeviceInfo(device, &deviceInfo))
        return false;

    inputOut = applicationQueueInfo;
    bool generationFound = false;
    bool presentationFound = false;
    bool acquireFound = false;
    bool replayFound = false;
    for (std::uint32_t i = 0; i < deviceInfo.queueCount; ++i) {
        ApusrVulkanHostQueueInfo candidate{};
        if (!apusrVulkanHostGetQueueInfo(device, i, &candidate) || !candidate.queue ||
            (candidate.ownershipFlags & APUSR_VK_HOST_QUEUE_OWNED_BY_APUSR) == 0u)
            continue;
        if ((candidate.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_INPUT) != 0u)
            inputOut = candidate;
        if ((candidate.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_GENERATION) != 0u) {
            generationOut = candidate;
            generationFound = true;
        }
        if ((candidate.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_PRESENTATION) != 0u) {
            presentationOut = candidate;
            presentationFound = true;
        }
        if ((candidate.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_ACQUIRE) != 0u) {
            acquireOut = candidate;
            acquireFound = true;
        }
        if ((candidate.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_REPLAY_TRANSFER) != 0u) {
            replayOut = candidate;
            replayFound = true;
        }
    }
    if (!generationFound || !presentationFound || !acquireFound)
        return false;
    if (!replayFound)
        replayOut = presentationOut;
    // If no dedicated graphics-side INPUT queue was reserved, Phase A runs on
    // the same APUSR-private generation queue immediately before Phase B. That
    // is strictly preferable to injecting reconstruction/HUD-less retention
    // work onto the game's graphics queue merely to manufacture a split.
    if ((inputOut.ownershipFlags & APUSR_VK_HOST_QUEUE_ROLE_INPUT) == 0u)
        inputOut = generationOut;
    // The application queue is game-owned. Treat accidental reuse of it as an
    // unsupported queue contract rather than silently injecting APUSR Phase A
    // or Phase B work into the game's submission stream.
    return generationOut.queue != applicationQueueInfo.queue && inputOut.queue != applicationQueueInfo.queue &&
        generationOut.queue != presentationOut.queue && generationOut.queue != acquireOut.queue &&
        presentationOut.queue != acquireOut.queue && inputOut.queue != presentationOut.queue &&
        inputOut.queue != acquireOut.queue;
}

bool createVulkanFramegenRuntimeLocked(
    void* queue,
    void* swapchain,
    const ApusrVulkanHostSwapchainInfo& swapchainInfo) noexcept
{
    destroyVulkanFramegenRuntimeLocked();
    if (!queue || !swapchain || !swapchainInfo.device || swapchainInfo.imageCount < 2u ||
        swapchainInfo.realImageCount < 2u || swapchainInfo.width == 0u || swapchainInfo.height == 0u)
        return false;

    // The production Vulkan FG path is an actual swapchain proxy. Never fall
    // back to presenting application-visible indices as real WSI images: once
    // vkGetSwapchainImagesKHR returned APUSR-owned images, only the private WSI
    // presenter may touch the physical swapchain.
    if ((swapchainInfo.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_VIRTUALIZED) == 0u) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.swapchain_unsupported",
            "swapchain=%p flags=0x%x reason=offscreen_swapchain_proxy_not_active",
            swapchain, swapchainInfo.framegenFlags);
        return false;
    }
    if ((swapchainInfo.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_TRANSFER_DST) == 0u) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.swapchain_unsupported",
            "swapchain=%p flags=0x%x reason=real_wsi_transfer_dst_required",
            swapchain, swapchainInfo.framegenFlags);
        return false;
    }

    ApusrVulkanHostDeviceInfo deviceInfo{};
    ApusrVulkanHostQueueInfo applicationQueueInfo{};
    ApusrVulkanHostQueueInfo inputQueueInfo{};
    ApusrVulkanHostQueueInfo generationQueueInfo{};
    ApusrVulkanHostQueueInfo presentationQueueInfo{};
    ApusrVulkanHostQueueInfo acquireQueueInfo{};
    ApusrVulkanHostQueueInfo replayQueueInfo{};
    if (!apusrVulkanHostGetDeviceInfo(swapchainInfo.device, &deviceInfo) ||
        !resolvePresentQueueInfo(swapchainInfo.device, queue, applicationQueueInfo))
        return false;
    if ((deviceInfo.negotiationFlags & APUSR_VK_HOST_FRAMEGEN_TIMELINE) == 0u) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.timeline_unsupported",
            "device=%p reason=timeline_semaphore_feature_not_enabled_at_device_creation",
            swapchainInfo.device);
        return false;
    }
    if (!selectFramegenQueueInfos(
            swapchainInfo.device, applicationQueueInfo,
            inputQueueInfo, generationQueueInfo, presentationQueueInfo, acquireQueueInfo, replayQueueInfo)) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.queue_unsupported",
            "application_queue=%p family=%u reason=required_apusr_queue_roles_unavailable",
            queue, applicationQueueInfo.familyIndex);
        return false;
    }

    auto getInstanceProcAddr = vulkanSystemFn<VkGetInstanceProcAddrFn>("vkGetInstanceProcAddr");
    auto getDeviceProcAddr = vulkanSystemFn<VkGetDeviceProcAddrFn>("vkGetDeviceProcAddr");
    if (!getInstanceProcAddr || !getDeviceProcAddr)
        return false;

    VulkanFramegenRuntime runtime{};
    runtime.swapchain = swapchain;
    runtime.device = swapchainInfo.device;
    runtime.applicationQueue = queue;
    runtime.inputQueue = inputQueueInfo.queue;
    runtime.queue = generationQueueInfo.queue;
    runtime.presentationQueue = presentationQueueInfo.queue;
    runtime.replayQueue = replayQueueInfo.queue;
    runtime.imageAcquireQueue = acquireQueueInfo.queue;
    runtime.inputQueueFamilyIndex = inputQueueInfo.familyIndex;
    runtime.generationQueueFamilyIndex = generationQueueInfo.familyIndex;
    runtime.replayQueueFamilyIndex = replayQueueInfo.familyIndex;
    runtime.swapchainInfo = swapchainInfo;
    runtime.deviceContext = {
        .instance = deviceInfo.instance,
        .physicalDevice = deviceInfo.physicalDevice,
        .device = deviceInfo.device,
        .getInstanceProcAddr = reinterpret_cast<void*>(getInstanceProcAddr),
        .getDeviceProcAddr = reinterpret_cast<void*>(getDeviceProcAddr),
        .minUniformBufferOffsetAlignment = 256u,
        .subgroupSize = 32u,
        .subgroupSizeMin = 32u,
        .subgroupSizeMax =
            (deviceInfo.negotiationFlags & APUSR_VK_HOST_FRAMEGEN_SUBGROUP64) != 0u ? 64u : 32u,
        .subgroupSizeControlSupported =
            (deviceInfo.negotiationFlags & APUSR_VK_HOST_FRAMEGEN_SUBGROUP64) != 0u,
        .fp16Supported = (deviceInfo.negotiationFlags & APUSR_VK_HOST_SHADER_FLOAT16) != 0u,
    };
    runtime.deviceContext.internalQueueFamilyIndices[0] = inputQueueInfo.familyIndex;
    runtime.deviceContext.internalQueueFamilyIndexCount = 1u;
    if (generationQueueInfo.familyIndex != inputQueueInfo.familyIndex) {
        runtime.deviceContext.internalQueueFamilyIndices[1] = generationQueueInfo.familyIndex;
        runtime.deviceContext.internalQueueFamilyIndexCount = 2u;
    }

    const auto cleanup = [&]() noexcept {
        if (runtime.presentation)
            apusrFramegenDestroyVulkanPresentation(runtime.presentation, {});
        for (auto* generatedReplay : runtime.generatedReplay) {
            if (generatedReplay)
                apusrFramegenDestroyVulkanSourceReplayContext(generatedReplay, {});
        }
        if (runtime.sourceReplay)
            apusrFramegenDestroyVulkanSourceReplayContext(runtime.sourceReplay, {});
        if (runtime.presentSubmit)
            apusrFramegenDestroyVulkanPresentSubmitContext(runtime.presentSubmit, {});
        if (runtime.directWsi)
            apusrFramegenDestroyVulkanDirectWsiContext(runtime.directWsi, {});
        if (runtime.session)
            apusrFramegenDestroyVulkanSession(runtime.session, {});
        if (runtime.presentImages)
            apusrFramegenDestroyVulkanPresentImageContext(runtime.presentImages, {});
        runtime = {};
    };

    runtime.swapchainImages.resize(swapchainInfo.imageCount);
    for (std::uint32_t i = 0; i < swapchainInfo.imageCount; ++i) {
        if (!apusrVulkanHostGetSwapchainImage(swapchain, i, &runtime.swapchainImages[i]) ||
            !runtime.swapchainImages[i]) {
            cleanup();
            return false;
        }
    }
    runtime.realSwapchainImages.resize(swapchainInfo.realImageCount);
    for (std::uint32_t i = 0; i < swapchainInfo.realImageCount; ++i) {
        if (!apusrVulkanHostGetRealSwapchainImage(swapchain, i, &runtime.realSwapchainImages[i]) ||
            !runtime.realSwapchainImages[i]) {
            cleanup();
            return false;
        }
    }

    ApusrFramegenVulkanPresentImageDescription imageDescription{
        .deviceContext = runtime.deviceContext,
        .width = swapchainInfo.width,
        .height = swapchainInfo.height,
        .nativeFormat = swapchainInfo.imageFormat,
        .imageUsage = swapchainInfo.imageUsage,
        .imageCount = swapchainInfo.imageCount,
        .images = runtime.swapchainImages.data(),
        .presentationImageCount = swapchainInfo.realImageCount,
        .presentationImages = runtime.realSwapchainImages.data(),
        .presentationImageUsage = swapchainInfo.realImageUsage,
        .offscreenProxy = true,
    };
    const auto addInternalQueueFamily = [&](const std::uint32_t family) {
        for (std::uint32_t i = 0u; i < imageDescription.internalQueueFamilyIndexCount; ++i) {
            if (imageDescription.internalQueueFamilyIndices[i] == family)
                return;
        }
        if (imageDescription.internalQueueFamilyIndexCount < imageDescription.internalQueueFamilyIndices.size())
            imageDescription.internalQueueFamilyIndices[imageDescription.internalQueueFamilyIndexCount++] = family;
    };
    addInternalQueueFamily(inputQueueInfo.familyIndex);
    addInternalQueueFamily(generationQueueInfo.familyIndex);
    addInternalQueueFamily(replayQueueInfo.familyIndex);
    if (apusrFramegenCreateVulkanPresentImageContext(
            imageDescription, {}, &runtime.presentImages) != APUSR_FRAMEGEN_OK) {
        cleanup();
        return false;
    }

    const ApusrFramegenVulkanPresentSubmitDescription submitDescription{
        .deviceContext = runtime.deviceContext,
        .inputQueue = runtime.inputQueue,
        .inputQueueFamilyIndex = inputQueueInfo.familyIndex,
        .generationQueue = runtime.queue,
        .generationQueueFamilyIndex = generationQueueInfo.familyIndex,
        .slotCount = swapchainInfo.imageCount,
    };
    if (apusrFramegenCreateVulkanPresentSubmitContext(
            submitDescription, {}, &runtime.presentSubmit) != APUSR_FRAMEGEN_OK) {
        cleanup();
        return false;
    }
    runtime.inputsProcessingCompletionFence =
        apusrFramegenVulkanInputCompletionSemaphore(runtime.presentSubmit);
    if (!runtime.inputsProcessingCompletionFence ||
        !apusrFramegenVulkanProducerReadySemaphore(runtime.presentSubmit) ||
        !apusrFramegenVulkanGenerationCompletionSemaphore(runtime.presentSubmit)) {
        cleanup();
        return false;
    }

    runtime.replayQueueSubmitMutex = std::make_unique<std::mutex>();
    ApusrFramegenVulkanSourceReplayDescription replayDescription{
        .deviceContext = runtime.deviceContext,
        .queue = runtime.replayQueue,
        .queueFamilyIndex = replayQueueInfo.familyIndex,
        .queueSubmitMutex = runtime.replayQueueSubmitMutex.get(),
        .device = runtime.device,
        .swapchain = runtime.swapchain,
        .presentImageCount = swapchainInfo.realImageCount,
        .sourceSlotCount = swapchainInfo.imageCount,
        .presentImages = runtime.presentImages,
        .content = APUSR_FRAMEGEN_VULKAN_REPLAY_SOURCE,
        .userContext = nullptr,
        .acquireImage = &framegenAcquireImageCallback,
        .releaseImage = (swapchainInfo.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_MAINTENANCE1) != 0u
            ? &framegenReleaseImageCallback : nullptr,
    };
    if (apusrFramegenCreateVulkanSourceReplayContext(
            replayDescription, {}, &runtime.sourceReplay) != APUSR_FRAMEGEN_OK || !runtime.sourceReplay) {
        cleanup();
        return false;
    }
    replayDescription.content = APUSR_FRAMEGEN_VULKAN_REPLAY_GENERATED;
    replayDescription.sourceSlotCount = APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS;
    for (auto*& generatedReplay : runtime.generatedReplay) {
        if (apusrFramegenCreateVulkanSourceReplayContext(
                replayDescription, {}, &generatedReplay) != APUSR_FRAMEGEN_OK || !generatedReplay) {
            cleanup();
            return false;
        }
    }

    const bool directWsiEligible =
        (swapchainInfo.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_STORAGE) != 0u &&
        (swapchainInfo.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_MAINTENANCE1) != 0u &&
        apusrFramegenVulkanDirectWsiStorageSupported(runtime.presentImages);
    if (directWsiEligible) {
        const ApusrFramegenVulkanDirectWsiDescription directDescription{
            .deviceContext = runtime.deviceContext,
            .bridgeQueue = runtime.presentationQueue,
            .abortQueue = runtime.queue,
            .device = runtime.device,
            .swapchain = runtime.swapchain,
            .presentImageCount = swapchainInfo.realImageCount,
            .userContext = nullptr,
            .acquireImage = &framegenAcquireImageCallback,
            .releaseImage = &framegenReleaseImageCallback,
        };
        if (apusrFramegenCreateVulkanDirectWsiContext(
                directDescription, {}, &runtime.directWsi) != APUSR_FRAMEGEN_OK || !runtime.directWsi) {
            // Direct-to-WSI is the preferred zero-copy generated path, not a
            // requirement for correctness. Keep the provider-output/replay path
            // alive if its optional setup fails on an otherwise supported WSI.
            runtime.directWsi = nullptr;
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.direct_wsi_unavailable",
                "swapchain=%p flags=0x%x real_usage=0x%x reason=context_creation_failed",
                swapchain, swapchainInfo.framegenFlags, swapchainInfo.realImageUsage);
        }
    }

    if (swapchainInfo.imageCount > ~std::uint32_t{0} - swapchainInfo.realImageCount) {
        cleanup();
        return false;
    }
    // Every queued source task necessarily retains either a virtual source slot
    // (worker replay) or a privately acquired real-WSI image (prearmed replay).
    // Their sum is therefore a hard upper bound on pending presenter work. Size
    // the allocation to that ownership bound instead of relying on an arbitrary
    // queue depth plus application-thread drain/retry behavior.
    const std::uint32_t maxPendingPresentCount =
        swapchainInfo.imageCount + swapchainInfo.realImageCount;
    const ApusrFramegenVulkanPresentationDescription presentationDescription{
        .userContext = &g_vulkanFramegenRuntime,
        .presentImage = &framegenPresentImageCallback,
        .presentGeneratedRelative = &framegenPresentGeneratedRelativeCallback,
        .presentSourceRelative = &framegenPresentSourceRelativeCallback,
        .prepareGeneratedReplay = &framegenPrepareGeneratedReplayCallback,
        .prepareSourceReplay = &framegenPrepareSourceReplayCallback,
        .timelineReady = &framegenTimelineReadyCallback,
        .waitTimeline = &framegenTimelineWaitCallback,
        .generationDecision = &framegenGenerationDecisionCallback,
        .prepareDirectPresent = &framegenPrepareDirectPresentCallback,
        .directGenerationDecision = &framegenDirectGenerationDecisionCallback,
        .maxPendingPresentCount = maxPendingPresentCount,
        // The virtual swapchain lets the application outrun physical FIFO WSI.
        // Profiling showed that allowing the proxy backlog to grow to 3-4
        // source tasks makes VK_AMD_anti_lag eventually perform 120-220 ms
        // catch-up waits at the next INPUT marker. Preserve worker overlap, but
        // restore a normal bounded present lead so Anti-Lag never has to absorb
        // that accumulated presentation debt in one burst.
        .maxApplicationPresentQueueDepth = std::min<std::uint32_t>(2u, maxPendingPresentCount),
        .initialSourceFrameTimeMs = 16.666667f,
    };
    if (apusrFramegenCreateVulkanPresentation(
            presentationDescription, {}, &runtime.presentation) != APUSR_FRAMEGEN_OK) {
        cleanup();
        return false;
    }

    LARGE_INTEGER frequency{};
    if (QueryPerformanceFrequency(&frequency))
        runtime.qpcFrequency = frequency.QuadPart;

    g_vulkanFramegenRuntime = std::move(runtime);
    apusr::host_discovery::eventf(
        "streamline", "vulkan.dlssg.runtime_created",
        "swapchain=%p extent=%ux%u format=%u public_usage=0x%x real_usage=0x%x virtual_images=%u real_images=%u app_queue=%p input_queue=%p generation_queue=%p presentation_queue=%p replay_queue=%p acquire_queue=%p input_family=%u generation_family=%u replay_family=%u async_compute=%u input_private=%u input_completion_timeline=%p generation_completion_timeline=%p source_consumed_timeline=%p fp16=%u subgroup64=%u subgroup_range=%u-%u direct_wsi=%u wsi_mode=retained_virtual_source",
        swapchain, swapchainInfo.width, swapchainInfo.height, swapchainInfo.imageFormat,
        swapchainInfo.imageUsage, swapchainInfo.realImageUsage,
        swapchainInfo.imageCount, swapchainInfo.realImageCount,
        queue, g_vulkanFramegenRuntime.inputQueue, g_vulkanFramegenRuntime.queue,
        g_vulkanFramegenRuntime.presentationQueue, g_vulkanFramegenRuntime.replayQueue,
        g_vulkanFramegenRuntime.imageAcquireQueue,
        inputQueueInfo.familyIndex, generationQueueInfo.familyIndex, replayQueueInfo.familyIndex,
        generationQueueInfo.familyIndex != applicationQueueInfo.familyIndex ? 1u : 0u,
        inputQueueInfo.queue != applicationQueueInfo.queue ? 1u : 0u,
        g_vulkanFramegenRuntime.inputsProcessingCompletionFence,
        apusrFramegenVulkanGenerationCompletionSemaphore(g_vulkanFramegenRuntime.presentSubmit),
        apusrFramegenVulkanReplayConsumedSemaphore(g_vulkanFramegenRuntime.sourceReplay),
        g_vulkanFramegenRuntime.deviceContext.fp16Supported ? 1u : 0u,
        g_vulkanFramegenRuntime.deviceContext.subgroupSizeControlSupported &&
            g_vulkanFramegenRuntime.deviceContext.subgroupSizeMax >= 64u ? 1u : 0u,
        g_vulkanFramegenRuntime.deviceContext.subgroupSizeMin,
        g_vulkanFramegenRuntime.deviceContext.subgroupSizeMax,
        g_vulkanFramegenRuntime.directWsi ? 1u : 0u);
    apusr::host_discovery::eventf(
        "streamline", "vulkan.dlssg.wsi_proxy",
        "swapchain=%p virtual_images=%u real_images=%u retained_virtual_source=1 source_staging=0 direct_source_replay=1 acquire_gpu_waits=0 hudless_staging=0 direct_generated_wsi=%u input_private=%u generation_compute_only=%u",
        swapchain, swapchainInfo.imageCount, swapchainInfo.realImageCount,
        g_vulkanFramegenRuntime.directWsi ? 1u : 0u,
        inputQueueInfo.queue != applicationQueueInfo.queue ? 1u : 0u,
        (generationQueueInfo.familyFlags & 0x1u) == 0u ? 1u : 0u);
    return true;
}

bool ensureVulkanFramegenRuntimeLocked(void* queue, void* swapchain) noexcept
{
    ApusrVulkanHostSwapchainInfo info{};
    if (!apusrVulkanHostGetSwapchainInfo(swapchain, &info))
        return false;
    VulkanFramegenRuntime& runtime = g_vulkanFramegenRuntime;
    constexpr std::uint32_t kSwapchainContractFlags =
        APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_VIRTUALIZED |
        APUSR_VK_HOST_SWAPCHAIN_STORAGE |
        APUSR_VK_HOST_SWAPCHAIN_MAINTENANCE1;
    const bool matching = runtime.swapchain == swapchain && runtime.device == info.device &&
        runtime.applicationQueue == queue && runtime.swapchainInfo.imageCount == info.imageCount &&
        runtime.swapchainInfo.realImageCount == info.realImageCount &&
        runtime.swapchainInfo.width == info.width && runtime.swapchainInfo.height == info.height &&
        runtime.swapchainInfo.imageFormat == info.imageFormat && runtime.swapchainInfo.imageUsage == info.imageUsage &&
        runtime.swapchainInfo.realImageUsage == info.realImageUsage &&
        (runtime.swapchainInfo.framegenFlags & kSwapchainContractFlags) ==
            (info.framegenFlags & kSwapchainContractFlags);
    return matching || createVulkanFramegenRuntimeLocked(queue, swapchain, info);
}

ApusrFramegenStreamlineVulkanResource framegenResource(const CapturedResource& source) noexcept
{
    ApusrFramegenStreamlineVulkanResource resource{};
    resource.tagged = source.tagged && source.native != nullptr;
    resource.image = source.native;
    resource.imageView = source.view;
    resource.nativeFormat = source.nativeFormat;
    resource.nativeLayout = source.state;
    resource.width = source.width;
    resource.height = source.height;
    resource.mipLevels = std::max(source.mipLevels, 1u);
    resource.arrayLayers = std::max(source.arrayLayers, 1u);
    resource.nativeUsage = source.usage;
    resource.extentLeft = source.extentLeft;
    resource.extentTop = source.extentTop;
    resource.extentWidth = source.extentWidth;
    resource.extentHeight = source.extentHeight;
    if (source.hasSubresourceRange) {
        resource.aspectMask = source.subresourceAspectMask;
        resource.baseMipLevel = source.subresourceBaseMipLevel;
        resource.levelCount = source.subresourceLevelCount;
        resource.baseArrayLayer = source.subresourceBaseArrayLayer;
        resource.layerCount = source.subresourceLayerCount;
    }
    return resource;
}

void gatherFramegenInputs(
    const void* frame,
    const std::uint32_t viewport,
    CapturedTags& tags,
    CapturedConstants& constants) noexcept
{
    std::lock_guard<std::mutex> lock(g_streamlineStateMutex);
    if (const auto it = g_legacyTagsByViewport.find(0u); it != g_legacyTagsByViewport.end())
        mergeCapturedTags(tags, it->second);
    if (const auto it = g_tagsByFrameViewport.find({.frame = frame, .viewport = 0u}); it != g_tagsByFrameViewport.end())
        mergeCapturedTags(tags, it->second);
    if (const auto it = g_legacyTagsByViewport.find(viewport); it != g_legacyTagsByViewport.end())
        mergeCapturedTags(tags, it->second);
    if (const auto it = g_tagsByFrameViewport.find({.frame = frame, .viewport = viewport}); it != g_tagsByFrameViewport.end())
        mergeCapturedTags(tags, it->second);
    if (const auto it = g_constantsByViewport.find(0u); it != g_constantsByViewport.end())
        mergeCapturedConstants(constants, it->second);
    if (const auto it = g_constantsByFrameViewport.find({.frame = frame, .viewport = 0u}); it != g_constantsByFrameViewport.end())
        mergeCapturedConstants(constants, it->second);
    if (const auto it = g_constantsByViewport.find(viewport); it != g_constantsByViewport.end())
        mergeCapturedConstants(constants, it->second);
    if (const auto it = g_constantsByFrameViewport.find({.frame = frame, .viewport = viewport}); it != g_constantsByFrameViewport.end())
        mergeCapturedConstants(constants, it->second);
}

float currentFrameTimeDeltaMs(VulkanFramegenRuntime& runtime) noexcept
{
    LARGE_INTEGER now{};
    if (!QueryPerformanceCounter(&now) || runtime.qpcFrequency <= 0)
        return 16.666667f;
    float delta = 16.666667f;
    if (runtime.previousApplicationPresentQpc != 0) {
        delta = static_cast<float>(
            (static_cast<double>(now.QuadPart - runtime.previousApplicationPresentQpc) * 1000.0) /
            static_cast<double>(runtime.qpcFrequency));
        delta = std::clamp(delta, 2.0f, 100.0f);
    }
    runtime.previousApplicationPresentQpc = now.QuadPart;
    return delta;
}

float pclFrameTimeDeltaMs(
    VulkanFramegenRuntime& runtime,
    const std::int64_t presentStartQpc) noexcept
{
    if (presentStartQpc <= 0 || runtime.qpcFrequency <= 0)
        return currentFrameTimeDeltaMs(runtime);

    float delta = 16.666667f;
    if (runtime.previousPclPresentStartQpc != 0) {
        delta = static_cast<float>(
            (static_cast<double>(presentStartQpc - runtime.previousPclPresentStartQpc) * 1000.0) /
            static_cast<double>(runtime.qpcFrequency));
        delta = std::clamp(delta, 2.0f, 100.0f);
    }
    runtime.previousPclPresentStartQpc = presentStartQpc;
    return delta;
}

bool sameSessionDescription(
    const ApusrFramegenVulkanSessionDescription& a,
    const ApusrFramegenVulkanSessionDescription& b) noexcept
{
    return a.deviceContext.device == b.deviceContext.device &&
        a.maxRenderWidth == b.maxRenderWidth && a.maxRenderHeight == b.maxRenderHeight &&
        a.displayWidth == b.displayWidth && a.displayHeight == b.displayHeight &&
        a.backBufferFormat == b.backBufferFormat && a.hudlessFormat == b.hudlessFormat &&
        a.distortionFormat == b.distortionFormat &&
        a.distortionWidth == b.distortionWidth && a.distortionHeight == b.distortionHeight &&
        a.depthInverted == b.depthInverted && a.depthInfinite == b.depthInfinite &&
        a.motionVectorsAtDisplayResolution == b.motionVectorsAtDisplayResolution &&
        a.motionVectorsJittered == b.motionVectorsJittered && a.hdr == b.hdr;
}

bool scheduleSourceOnlyLocked(
    VulkanFramegenRuntime& runtime,
    const VkPresentInfoAbi& info,
    const std::uint64_t frameId,
    const float sourceFrameTimeMs = 0.0f) noexcept
{
    if (!runtime.presentation || !runtime.presentImages || !runtime.sourceReplay ||
        info.swapchainCount != 1u || !info.pSwapchains || !info.pImageIndices ||
        info.waitSemaphoreCount > APUSR_FRAMEGEN_VULKAN_MAX_PRESENT_WAITS ||
        (info.waitSemaphoreCount != 0u && !info.pWaitSemaphores))
        return false;

    const std::uint32_t sourceImageIndex = info.pImageIndices[0];
    if (sourceImageIndex >= runtime.swapchainInfo.imageCount)
        return false;

    const std::uint64_t replayCompletionValue =
        apusrFramegenReserveVulkanReplayCompletion(runtime.sourceReplay);
    if (replayCompletionValue == 0u)
        return false;

    // Source-only frames deserve the same low-latency prearm as generated
    // frames. When replay owns an independent queue, opportunistically acquire
    // a real WSI image and submit the retained-source copy now with the game's
    // render-finished binaries as its waits. This is strictly zero-timeout:
    // queue/WSI contention falls back to worker replay and never stalls the
    // application's present call.
    std::uint32_t prearmedSourceImageIndex = APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE;
    void* prearmedSourceReadySemaphore = nullptr;
    bool sourcePrearmed = false;
    if (runtime.replayQueue != runtime.presentationQueue) {
        ApusrFramegenVulkanSourceReplayMetrics prearmMetrics{};
        const ApusrFramegenResult prearmResult = apusrFramegenTryPrepareVulkanSourceReplay(
            runtime.sourceReplay,
            sourceImageIndex,
            replayCompletionValue,
            info.pWaitSemaphores,
            info.waitSemaphoreCount,
            nullptr,
            0u,
            &prearmedSourceImageIndex,
            &prearmedSourceReadySemaphore,
            &sourcePrearmed,
            &prearmMetrics);
        if (prearmResult != APUSR_FRAMEGEN_OK) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.source_only_prearm_failed",
                "frame=%llu result=%u wait_previous_us=%llu acquire_us=%llu record_us=%llu submit_us=%llu",
                static_cast<unsigned long long>(frameId), static_cast<unsigned>(prearmResult),
                static_cast<unsigned long long>(prearmMetrics.waitPreviousUs),
                static_cast<unsigned long long>(prearmMetrics.acquireUs),
                static_cast<unsigned long long>(prearmMetrics.recordUs),
                static_cast<unsigned long long>(prearmMetrics.submitUs));
            destroyVulkanFramegenRuntimeLocked();
            return false;
        }
    }

    void* prearmedWaits[1]{prearmedSourceReadySemaphore};

    const ApusrFramegenVulkanPresentationSchedule schedule{
        .queue = runtime.presentationQueue,
        .swapchain = info.pSwapchains[0],
        .frameId = frameId,
        .sourceImageIndex = sourcePrearmed ? prearmedSourceImageIndex : sourceImageIndex,
        .sourceWaitSemaphores = sourcePrearmed ? prearmedWaits : info.pWaitSemaphores,
        .sourceWaitSemaphoreCount = sourcePrearmed ? 1u : info.waitSemaphoreCount,
        .hasGeneratedFrame = false,
        .paceSourceFrame = frameId != 0u,
        .replaySourceFrame = !sourcePrearmed,
        .sourceReplaySlotIndex = sourceImageIndex,
        .sourceReplayCompletionValue = replayCompletionValue,
        .sourceReplayUpstreamSemaphore = nullptr,
        .sourceReplayUpstreamValue = 0u,
        .sourceFrameTimeMs = sourceFrameTimeMs,
    };

    const ApusrFramegenResult scheduleResult =
        apusrFramegenScheduleVulkanPresentation(runtime.presentation, schedule);
    if (scheduleResult != APUSR_FRAMEGEN_OK) {
        // Presenter capacity is pre-sized from the ownership bound
        // (virtual-source slots + privately acquirable real WSI images). A full
        // queue therefore indicates a broken lifetime/admission invariant, not
        // ordinary load. Never "recover" by draining WSI work on the game's
        // present thread.
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.presenter_admission_invariant_failed",
            "frame=%llu mode=source_only result=%u virtual_images=%u real_images=%u",
            static_cast<unsigned long long>(frameId), static_cast<unsigned>(scheduleResult),
            runtime.swapchainInfo.imageCount, runtime.swapchainInfo.realImageCount);
        destroyVulkanFramegenRuntimeLocked();
        return false;
    }

    // A virtual backbuffer is retained until the physical source copy has
    // actually consumed it. The application never receives a slot whose
    // acquire semaphore is queued behind unfinished APUSR work.
    if (!apusrVulkanHostReleaseVirtualSwapchainImage(
            runtime.swapchain, sourceImageIndex,
            apusrFramegenVulkanReplayConsumedSemaphore(runtime.sourceReplay), replayCompletionValue,
            nullptr, 0u))
        return false;

    if (replayCompletionValue <= 4u || (replayCompletionValue & (replayCompletionValue - 1u)) == 0u) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.source_only_release_progress",
            "frame=%llu source_slot=%u replay_value=%llu completed_replay=%llu direct_app_waits=%u",
            static_cast<unsigned long long>(frameId), sourceImageIndex,
            static_cast<unsigned long long>(replayCompletionValue),
            static_cast<unsigned long long>(apusrFramegenVulkanCompletedReplayValue(runtime.sourceReplay)),
            info.waitSemaphoreCount);
    }
    return true;
}


} // namespace

std::int32_t WINAPI apusrOwnedVkQueuePresentKHR(void* queue, const void* presentInfo)
{
    // Source-frame presentation is issued by APUSR's worker through the
    // backing Streamline wrapper exactly once. Its device-dispatch call then
    // re-enters this frontend on the worker thread; bypass the feature logic
    // and let host capture perform the real loader present + Anti-Lag update.
    if (g_vulkanFramegenBackingPresent)
        return apusrVulkanHostQueuePresentSource(queue, presentInfo);

    if (presentInfo != nullptr) {
        const auto& info = *static_cast<const VkPresentInfoAbi*>(presentInfo);
        if (info.swapchainCount != 0u && info.pSwapchains != nullptr && info.pImageIndices != nullptr) {
            void* const swapchain = info.pSwapchains[0];
            void* observed = g_lastObservedPresentSwapchain.load(std::memory_order_relaxed);
            if (swapchain != nullptr && observed != swapchain) {
                ApusrVulkanHostSwapchainInfo swapchainInfo{};
                void* image = nullptr;
                const std::uint32_t imageIndex = info.pImageIndices[0];
                if (apusrVulkanHostGetSwapchainInfo(swapchain, &swapchainInfo) &&
                    apusrVulkanHostGetSwapchainImage(swapchain, imageIndex, &image) &&
                    g_lastObservedPresentSwapchain.compare_exchange_strong(
                        observed, swapchain, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    apusr::host_discovery::eventf(
                        "streamline", "vulkan.present_target",
                        "swapchain=%p image=%p image_index=%u extent=%ux%u format=%u color_space=%u usage=0x%x images=%u acquire_serial=%llu present_serial=%llu",
                        swapchain, image, imageIndex, swapchainInfo.width, swapchainInfo.height,
                        swapchainInfo.imageFormat, swapchainInfo.imageColorSpace, swapchainInfo.imageUsage, swapchainInfo.imageCount,
                        static_cast<unsigned long long>(swapchainInfo.acquireSerial),
                        static_cast<unsigned long long>(swapchainInfo.presentSerial));
                }
            }
        }
    }

    if (!presentInfo)
        return apusrBackingVkQueuePresentKHR(queue, presentInfo);

    const auto& info = *static_cast<const VkPresentInfoAbi*>(presentInfo);

    // Once a swapchain is virtualized, an application-visible image index no
    // longer names a real WSI image. Even with DLSS-G disabled we must retain
    // that virtual source until it is replayed to private WSI; forwarding to
    // the backing implementation would be semantically invalid.
    if (!g_dlssgEnabled.load(std::memory_order_acquire)) {
        if (info.pNext == nullptr && info.swapchainCount == 1u && info.pSwapchains && info.pImageIndices &&
            info.waitSemaphoreCount <= APUSR_FRAMEGEN_VULKAN_MAX_PRESENT_WAITS &&
            (info.waitSemaphoreCount == 0u || info.pWaitSemaphores)) {
            ApusrVulkanHostSwapchainInfo disabledInfo{};
            if (apusrVulkanHostGetSwapchainInfo(info.pSwapchains[0], &disabledInfo) &&
                (disabledInfo.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_VIRTUALIZED) != 0u) {
                std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
                if (ensureVulkanFramegenRuntimeLocked(queue, info.pSwapchains[0]) &&
                    scheduleSourceOnlyLocked(g_vulkanFramegenRuntime, info, 0u)) {
                    if (info.pResults)
                        info.pResults[0] = kVkSuccess;
                    return kVkSuccess;
                }
                return kVkErrorInitializationFailed;
            }
        }
        return apusrBackingVkQueuePresentKHR(queue, presentInfo);
    }
    // The first production path deliberately owns one ordinary swapchain
    // present at a time. pNext chains and batched multi-swapchain presents stay
    // on the untouched Streamline path until their semantics are implemented.
    if (info.pNext != nullptr || info.swapchainCount != 1u || !info.pSwapchains || !info.pImageIndices ||
        info.waitSemaphoreCount == 0u || info.waitSemaphoreCount > APUSR_FRAMEGEN_VULKAN_MAX_PRESENT_WAITS ||
        !info.pWaitSemaphores) {
        std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
        if (g_vulkanFramegenRuntime.presentation)
            apusrFramegenDrainVulkanPresentation(g_vulkanFramegenRuntime.presentation);
        if (info.swapchainCount == 1u && info.pSwapchains) {
            ApusrVulkanHostSwapchainInfo unsupportedInfo{};
            if (apusrVulkanHostGetSwapchainInfo(info.pSwapchains[0], &unsupportedInfo) &&
                (unsupportedInfo.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_VIRTUALIZED) != 0u) {
                apusr::host_discovery::event(
                    "streamline", "vulkan.dlssg.virtual_present_contract_unsupported",
                    "virtualized swapchain received pNext/batched/missing-wait present; refusing to forward a virtual image index to real WSI");
                return kVkErrorInitializationFailed;
            }
        }
        return apusrBackingVkQueuePresentKHR(queue, presentInfo);
    }

    std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
    VulkanFramegenRuntime& preRuntime = g_vulkanFramegenRuntime;
    if (preRuntime.presentation) {
        const std::int32_t deferredError = apusrFramegenTakeVulkanPresentationError(preRuntime.presentation);
        if (deferredError < 0) {
            destroyVulkanFramegenRuntimeLocked();
            return deferredError;
        }
    }

    const auto asyncSuccess = [&info]() noexcept -> std::int32_t {
        if (info.pResults)
            info.pResults[0] = kVkSuccess;
        return kVkSuccess;
    };
    float sourceFrameTimeForFallbackMs = 0.0f;
    const auto sourceOnlyOrBacking = [&](
        const std::uint64_t frameId,
        const char* const reason,
        const std::uint32_t decisionFlags) noexcept -> std::int32_t {
        VulkanFramegenRuntime& runtime = g_vulkanFramegenRuntime;
        recordDlssgDecision(
            frameId, apusr::profile::DecisionOutcome::SourceOnly, reason,
            runtime.sourceFrameSequence, runtime.lastSubmittedGenerationCompletionValue,
            0u, 0u, decisionFlags);
        ApusrVulkanHostSwapchainInfo sourceInfo{};
        const bool virtualized = apusrVulkanHostGetSwapchainInfo(info.pSwapchains[0], &sourceInfo) &&
            (sourceInfo.framegenFlags & APUSR_VK_HOST_SWAPCHAIN_FRAMEGEN_VIRTUALIZED) != 0u;
        if (virtualized) {
            if (ensureVulkanFramegenRuntimeLocked(queue, info.pSwapchains[0]) &&
                scheduleSourceOnlyLocked(
                    g_vulkanFramegenRuntime, info, frameId, sourceFrameTimeForFallbackMs))
                return asyncSuccess();
            // Never pass an application-visible virtual image index to the real
            // swapchain. A proxy failure is surfaced explicitly instead.
            return kVkErrorInitializationFailed;
        }
        if (runtime.presentation)
            apusrFramegenDrainVulkanPresentation(runtime.presentation);
        return apusrBackingVkQueuePresentKHR(queue, presentInfo);
    };

    if (!g_dlssgControl.configured || g_dlssgControl.options.mode == DLSSGMode::eOff) {
        // TODO(Vulkan FG recovery): a swapchain that was created in APUSR's
        // virtualized FG mode cannot become the title's native WSI path merely
        // because interpolation is disabled. The source-only replay below still
        // pays the proxy copy/synchronization tax. NVIDIA's DLSS-G integration
        // requires swapchain recreation on FG on/off for the same reason. A
        // drop-in interposer needs a proven, host-safe way to induce/coordinate
        // that lifecycle before we can claim zero-overhead native FG-off recovery.
        return sourceOnlyOrBacking(0u, "framegen_disabled", 0u);
    }

    // Establish the virtual source-frame sequence before any reason to fall
    // back to a source-only present. If PCL correlation, Reflex inputs, or
    // heavy generation are unavailable for this source, the next FI dispatch
    // must see a frame-ID gap rather than pretending the missing source never
    // existed.
    void* const swapchain = info.pSwapchains[0];
    const std::uint32_t sourceImageIndex = info.pImageIndices[0];
    if (!ensureVulkanFramegenRuntimeLocked(queue, swapchain)) {
        g_dlssgLastStatus.store(kDlssgStatusConstantsInvalid, std::memory_order_relaxed);
        return sourceOnlyOrBacking(0u, "runtime_unavailable", apusr::profile::DecisionTriggerBurst);
    }
    VulkanFramegenRuntime& runtime = g_vulkanFramegenRuntime;
    if (runtime.directWsi)
        apusrFramegenReclaimCompletedVulkanDirectWsiTarget(runtime.directWsi);
    if (sourceImageIndex >= runtime.swapchainInfo.imageCount) {
        g_dlssgLastStatus.store(kDlssgStatusConstantsInvalid, std::memory_order_relaxed);
        return sourceOnlyOrBacking(0u, "invalid_source_image", apusr::profile::DecisionTriggerBurst);
    }
    const std::uint64_t sourceFrameSequence = ++runtime.sourceFrameSequence;

    PendingFramegenPresent pending{};
    if (!popPendingFramegenPresentLocked(pending)) {
        if (!g_dlssgWaitingForPclLogged.exchange(true, std::memory_order_relaxed)) {
            apusr::host_discovery::event(
                "streamline", "vulkan.dlssg.waiting_for_pcl_present_marker",
                "enabled=1; no_PCL_PresentStart_or_OutOfBandPresentStart_has_been_correlated_yet");
        }
        return sourceOnlyOrBacking(sourceFrameSequence, "pcl_present_marker_missing", apusr::profile::DecisionTriggerBurst);
    }
    // PCL PresentStart is upstream of our virtualized vkQueuePresentKHR path
    // and therefore cannot inherit jitter from the proxy presenter itself.
    // Use it as the source-production clock whenever frame correlation exists.
    const float sourceFrameTimeDeltaMs = pclFrameTimeDeltaMs(runtime, pending.presentStartQpc);
    sourceFrameTimeForFallbackMs = sourceFrameTimeDeltaMs;

    if (!g_reflexLoaded.load(std::memory_order_relaxed) ||
        g_reflexMode.load(std::memory_order_relaxed) == APUSR_VK_HOST_ANTI_LAG_OFF) {
        g_dlssgLastStatus.store(kDlssgStatusReflexNotDetected, std::memory_order_relaxed);
        return sourceOnlyOrBacking(pending.frameIndex, "reflex_unavailable", apusr::profile::DecisionTriggerBurst);
    }

    CapturedTags tags{};
    CapturedConstants constants{};
    gatherFramegenInputs(pending.frame, g_dlssgControl.viewport, tags, constants);
    const bool mustSignalInputCompletion =
        g_dlssgControl.options.queueParallelismMode == DLSSGQueueParallelismMode::eBlockNoClientQueues;
    // eBlockNoClientQueues gives APUSR a precise client-input lifetime boundary:
    // Phase A consumes/retains every transient game resource, signals input
    // completion, and Phase B touches provider-owned state only.
    const bool splitInputRelease = mustSignalInputCompletion;
    if (!splitInputRelease) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.frame_contract_unsupported",
            "frame=%u queue_parallelism=%u reason=framegen_requires_block_no_client_queues",
            pending.frameIndex, static_cast<unsigned>(g_dlssgControl.options.queueParallelismMode));
        g_dlssgLastStatus.store(kDlssgStatusConstantsInvalid, std::memory_order_relaxed);
        return sourceOnlyOrBacking(pending.frameIndex, "queue_parallelism_unsupported", apusr::profile::DecisionTriggerBurst);
    }

    // DLSS SR's colorBuffersHDR describes the upscaler's input color, which can
    // be pre-tonemap HDR even when the WSI backbuffer presented by DLSS-G is
    // ordinary SDR. Frame Generation's transfer contract belongs to the
    // swapchain/presented image, not to the SR input. True HDR WSI color spaces
    // remain explicit follow-up work.
    if (runtime.swapchainInfo.imageColorSpace != kVkColorSpaceSrgbNonlinearKhr) {
        g_dlssgLastStatus.store(kDlssgStatusHdrUnsupported, std::memory_order_relaxed);
        return sourceOnlyOrBacking(pending.frameIndex, "hdr_wsi_unsupported", apusr::profile::DecisionTriggerBurst);
    }
    if (!constants.valid || !tags.depth.tagged || !tags.motionVectors.tagged) {
        g_dlssgLastStatus.store(kDlssgStatusConstantsInvalid, std::memory_order_relaxed);
        return sourceOnlyOrBacking(pending.frameIndex, "inputs_invalid", apusr::profile::DecisionTriggerBurst);
    }

    const std::uint64_t previousGenerationSourceSequence = runtime.lastGenerationSourceSequence;
    const bool generationHistoryGap = previousGenerationSourceSequence == 0u ||
        sourceFrameSequence != previousGenerationSourceSequence + 1u;
    const bool generationWarmupOnly = constants.reset || generationHistoryGap;

    // FI/OF history is serial: never overlap two heavy generations. Generated
    // *presentation* is independent, however. Two provider-owned output slots
    // let the next completed FI/OF dispatch advance even if the previous output
    // is still waiting for its midpoint decision or physical WSI replay.
    const std::uint64_t completedGeneration =
        apusrFramegenVulkanCompletedGenerationValue(runtime.presentSubmit);
    if (runtime.lastSubmittedGenerationCompletionValue != 0u) {
        if (completedGeneration >= runtime.lastSubmittedGenerationCompletionValue &&
            runtime.generationLatencyLoggedValue < runtime.lastSubmittedGenerationCompletionValue &&
            runtime.generationSubmitQpc != 0 && runtime.qpcFrequency > 0) {
            LARGE_INTEGER generationCompleteObserved{};
            if (QueryPerformanceCounter(&generationCompleteObserved)) {
                const double generationMs =
                    static_cast<double>(generationCompleteObserved.QuadPart - runtime.generationSubmitQpc) * 1000.0 /
                    static_cast<double>(runtime.qpcFrequency);
                runtime.generationLatencyLoggedValue = runtime.lastSubmittedGenerationCompletionValue;
                apusr::host_discovery::eventf(
                    "streamline", "vulkan.dlssg.generation_completed",
                    "source_sequence=%llu generation_value=%llu wall_ms=%.3f",
                    static_cast<unsigned long long>(runtime.lastGenerationSourceSequence),
                    static_cast<unsigned long long>(runtime.lastSubmittedGenerationCompletionValue), generationMs);
            }
        }
        // Do not discard N+1 merely because Phase B(N) is still finishing.
        // The two-frame FI history does require ordering, but Vulkan can express
        // that dependency directly: source replay remains free to consume
        // producerReady, while Phase A(N+1) waits on the previous generation
        // timeline before mutating history. The old source-only fallback also
        // forced N+2 into reset/warmup, so one small overlap killed two
        // generated frames in a row.
    }

    const std::uint64_t priorGenerationCompletionValue =
        runtime.lastSubmittedGenerationCompletionValue != 0u &&
        completedGeneration < runtime.lastSubmittedGenerationCompletionValue
            ? runtime.lastSubmittedGenerationCompletionValue
            : 0u;
    if (priorGenerationCompletionValue != 0u) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.generation_chained_in_flight",
            "frame=%llu previous_generation=%llu completed_generation=%llu",
            static_cast<unsigned long long>(pending.frameIndex),
            static_cast<unsigned long long>(priorGenerationCompletionValue),
            static_cast<unsigned long long>(completedGeneration));
    }

    const auto chooseGeneratedOutputSlot = [&]() noexcept -> std::uint32_t {
        for (std::uint32_t offset = 0u; offset < APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS; ++offset) {
            const std::uint32_t slot =
                (runtime.nextGeneratedOutputSlot + offset) % APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS;
            const std::uint64_t generatedReplayCompleted = runtime.generatedReplay[slot]
                ? apusrFramegenVulkanCompletedReplayValue(runtime.generatedReplay[slot])
                : 0u;
            const std::uint64_t slotGeneration =
                g_vulkanFramegenGeneratedSlotGenerationValue[slot].load(std::memory_order_acquire);
            const std::uint64_t slotDecision =
                g_vulkanFramegenGeneratedSlotDecisionValue[slot].load(std::memory_order_acquire);
            const std::uint64_t slotReplay =
                g_vulkanFramegenGeneratedSlotReplayValue[slot].load(std::memory_order_acquire);
            const bool generationDone = slotGeneration == 0u || completedGeneration >= slotGeneration;
            const bool decisionDone = slotGeneration == 0u || slotDecision >= slotGeneration;
            const bool replayDone = slotReplay == 0u || generatedReplayCompleted >= slotReplay;
            if (generationDone && decisionDone && replayDone) {
                runtime.nextGeneratedOutputSlot =
                    (slot + 1u) % APUSR_FRAMEGEN_VULKAN_GENERATED_OUTPUT_SLOTS;
                return slot;
            }
        }
        return APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE;
    };

    void* inputCommandBuffer = nullptr;
    void* generationCommandBuffer = nullptr;
    const ApusrFramegenResult beginSubmitResult = apusrFramegenBeginVulkanSplitPresentSubmit(
        runtime.presentSubmit, sourceImageIndex, completedGeneration,
        &inputCommandBuffer, &generationCommandBuffer);
    if (beginSubmitResult == APUSR_FRAMEGEN_ERROR_BUSY) {
        // Optional generation must never turn a source frame into a CPU wait.
        // The scheduler normally proves this globally before reaching here; the
        // per-slot check is the final nonblocking lifetime guard.
        return sourceOnlyOrBacking(pending.frameIndex, "present_submit_busy", apusr::profile::DecisionTriggerBurst);
    }
    if (beginSubmitResult != APUSR_FRAMEGEN_OK) {
        g_dlssgLastStatus.store(kDlssgStatusConstantsInvalid, std::memory_order_relaxed);
        return sourceOnlyOrBacking(pending.frameIndex, "present_submit_begin_failed", apusr::profile::DecisionTriggerBurst);
    }

    const std::uint64_t sourceReplayCompletionValue =
        apusrFramegenReserveVulkanReplayCompletion(runtime.sourceReplay);
    const std::uint64_t plannedProducerReadyValue =
        apusrFramegenVulkanNextProducerReadyValue(runtime.presentSubmit);
    if (sourceReplayCompletionValue == 0u || plannedProducerReadyValue == 0u) {
        apusrFramegenAbortVulkanPresentSubmit(runtime.presentSubmit, sourceImageIndex);
        return sourceOnlyOrBacking(pending.frameIndex, "timeline_reservation_failed", apusr::profile::DecisionTriggerBurst);
    }

    // Secure the mandatory source target first. This submission waits a future
    // producerReady timeline value and therefore consumes none of the game's
    // binary render-finished semaphores. Once the fan-out submit lands, source
    // replay and Phase A can run concurrently on independent queues.
    std::uint32_t prearmedSourceImageIndex = APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE;
    void* prearmedSourceReadySemaphore = nullptr;
    bool sourcePrearmed = false;
    ApusrFramegenVulkanSourceReplayMetrics sourcePrearmMetrics{};
    // Prearm submits from the application-present thread. It is only legal on
    // a queue that the presentation worker does not also own; VkQueue access is
    // externally synchronized. When no independent replay lane could be
    // reserved, let the worker perform source replay serially on its own
    // presentation queue instead of introducing a cross-thread queue race.
    const bool sourcePrearmQueueIndependent = runtime.replayQueue != runtime.presentationQueue;
    const ApusrFramegenResult sourcePrearmResult = sourcePrearmQueueIndependent
        ? apusrFramegenTryPrepareVulkanSourceReplay(
            runtime.sourceReplay,
            sourceImageIndex,
            sourceReplayCompletionValue,
            nullptr,
            0u,
            apusrFramegenVulkanProducerReadySemaphore(runtime.presentSubmit),
            plannedProducerReadyValue,
            &prearmedSourceImageIndex,
            &prearmedSourceReadySemaphore,
            &sourcePrearmed,
            &sourcePrearmMetrics)
        : APUSR_FRAMEGEN_OK;
    if (sourcePrearmResult != APUSR_FRAMEGEN_OK) {
        apusrFramegenAbortVulkanPresentSubmit(runtime.presentSubmit, sourceImageIndex);
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.source_prearm_failed",
            "frame=%u result=%u wait_previous_us=%llu acquire_us=%llu record_us=%llu submit_us=%llu",
            pending.frameIndex, static_cast<unsigned>(sourcePrearmResult),
            static_cast<unsigned long long>(sourcePrearmMetrics.waitPreviousUs),
            static_cast<unsigned long long>(sourcePrearmMetrics.acquireUs),
            static_cast<unsigned long long>(sourcePrearmMetrics.recordUs),
            static_cast<unsigned long long>(sourcePrearmMetrics.submitUs));
        return sourceOnlyOrBacking(pending.frameIndex, "source_prearm_failed", apusr::profile::DecisionTriggerBurst);
    }

    const auto finishPrearmedSourceOnly = [&](
        const char* const reason,
        const std::uint32_t decisionFlags,
        const bool producerReadyAlreadySignaled = false) noexcept -> std::int32_t {
        if (!sourcePrearmed || !prearmedSourceReadySemaphore ||
            prearmedSourceImageIndex == APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE)
            return sourceOnlyOrBacking(pending.frameIndex, reason, decisionFlags);
        recordDlssgDecision(
            pending.frameIndex, apusr::profile::DecisionOutcome::SourceOnly, reason,
            sourceFrameSequence, runtime.lastSubmittedGenerationCompletionValue,
            1u, 0u, decisionFlags);

        if (!producerReadyAlreadySignaled) {
            std::uint64_t producerReadyValue = 0u;
            if (apusrFramegenSignalVulkanProducerReadyOnly(
                    runtime.presentSubmit,
                    info.pWaitSemaphores,
                    info.waitSemaphoreCount,
                    &producerReadyValue) != APUSR_FRAMEGEN_OK ||
                producerReadyValue < plannedProducerReadyValue)
                return kVkErrorInitializationFailed;
        }

        void* sourceWaits[1]{prearmedSourceReadySemaphore};
        const ApusrFramegenVulkanPresentationSchedule sourceSchedule{
            .queue = runtime.presentationQueue,
            .swapchain = swapchain,
            .frameId = pending.frameIndex,
            .sourceImageIndex = prearmedSourceImageIndex,
            .sourceWaitSemaphores = sourceWaits,
            .sourceWaitSemaphoreCount = 1u,
            .hasGeneratedFrame = false,
            .replaySourceFrame = false,
            .sourceReplayCompletionValue = sourceReplayCompletionValue,
            .sourceFrameTimeMs = sourceFrameTimeDeltaMs,
        };
        const ApusrFramegenResult scheduleResult =
            apusrFramegenScheduleVulkanPresentation(runtime.presentation, sourceSchedule);
        if (scheduleResult != APUSR_FRAMEGEN_OK) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.presenter_admission_invariant_failed",
                "frame=%u mode=prearmed_source result=%u virtual_images=%u real_images=%u",
                pending.frameIndex, static_cast<unsigned>(scheduleResult),
                runtime.swapchainInfo.imageCount, runtime.swapchainInfo.realImageCount);
            destroyVulkanFramegenRuntimeLocked();
            return kVkErrorInitializationFailed;
        }

        if (!apusrVulkanHostReleaseVirtualSwapchainImage(
                swapchain, sourceImageIndex,
                apusrFramegenVulkanReplayConsumedSemaphore(runtime.sourceReplay), sourceReplayCompletionValue,
                nullptr, 0u))
            return kVkErrorInitializationFailed;
        return asyncSuccess();
    };

    const auto finishReplayedSourceOnlyAfterProducerReady = [&](
        const std::uint64_t producerReadyValue,
        const char* const reason,
        const std::uint32_t decisionFlags) noexcept -> std::int32_t {
        if (producerReadyValue == 0u)
            return kVkErrorInitializationFailed;
        recordDlssgDecision(
            pending.frameIndex, apusr::profile::DecisionOutcome::SourceOnly, reason,
            sourceFrameSequence, runtime.lastSubmittedGenerationCompletionValue,
            producerReadyValue, 0u, decisionFlags);
        const ApusrFramegenVulkanPresentationSchedule sourceSchedule{
            .queue = runtime.presentationQueue,
            .swapchain = swapchain,
            .frameId = pending.frameIndex,
            .sourceImageIndex = sourceImageIndex,
            .hasGeneratedFrame = false,
            .replaySourceFrame = true,
            .sourceReplaySlotIndex = sourceImageIndex,
            .sourceReplayCompletionValue = sourceReplayCompletionValue,
            .sourceReplayUpstreamSemaphore = apusrFramegenVulkanProducerReadySemaphore(runtime.presentSubmit),
            .sourceReplayUpstreamValue = producerReadyValue,
            .sourceFrameTimeMs = sourceFrameTimeDeltaMs,
        };
        const ApusrFramegenResult scheduleResult =
            apusrFramegenScheduleVulkanPresentation(runtime.presentation, sourceSchedule);
        if (scheduleResult != APUSR_FRAMEGEN_OK) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.presenter_admission_invariant_failed",
                "frame=%u mode=producer_ready_source result=%u virtual_images=%u real_images=%u",
                pending.frameIndex, static_cast<unsigned>(scheduleResult),
                runtime.swapchainInfo.imageCount, runtime.swapchainInfo.realImageCount);
            destroyVulkanFramegenRuntimeLocked();
            return kVkErrorInitializationFailed;
        }
        if (!apusrVulkanHostReleaseVirtualSwapchainImage(
                swapchain, sourceImageIndex,
                apusrFramegenVulkanReplayConsumedSemaphore(runtime.sourceReplay), sourceReplayCompletionValue,
                nullptr, 0u))
            return kVkErrorInitializationFailed;
        return asyncSuccess();
    };

    // Optional generation may take a second real WSI image only after source
    // replay has secured its mandatory target. Warm-up dispatches are never
    // presented, so they deliberately use the provider output ring instead of
    // occupying a WSI image.
    std::uint32_t directPresentationImageIndex = APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE;
    void* directTargetAcquireSemaphore = nullptr;
    bool directWsiAcquired = false;
    if (sourcePrearmed && runtime.directWsi && !generationWarmupOnly) {
        const ApusrFramegenResult acquireResult = apusrFramegenTryAcquireVulkanDirectWsiTarget(
            runtime.directWsi,
            &directPresentationImageIndex,
            &directTargetAcquireSemaphore,
            &directWsiAcquired);
        if (acquireResult != APUSR_FRAMEGEN_OK) {
            apusrFramegenAbortVulkanPresentSubmit(runtime.presentSubmit, sourceImageIndex);
            return finishPrearmedSourceOnly("direct_wsi_acquire_failed", apusr::profile::DecisionTriggerBurst);
        }
    }

    std::uint32_t generationSlotIndex = APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE;
    if (!directWsiAcquired) {
        generationSlotIndex = chooseGeneratedOutputSlot();
        if (generationSlotIndex == APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE) {
            apusrFramegenAbortVulkanPresentSubmit(runtime.presentSubmit, sourceImageIndex);
            std::uint64_t generatedReplayCompleted = ~std::uint64_t{0};
            for (auto* generatedReplay : runtime.generatedReplay) {
                generatedReplayCompleted = std::min(
                    generatedReplayCompleted,
                    generatedReplay ? apusrFramegenVulkanCompletedReplayValue(generatedReplay) : 0u);
            }
            if (generatedReplayCompleted == ~std::uint64_t{0})
                generatedReplayCompleted = 0u;
            const std::uint64_t skipped =
                g_vulkanFramegenSkippedBusyCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (skipped <= 4u || (skipped & (skipped - 1u)) == 0u) {
                apusr::host_discovery::eventf(
                    "streamline", "vulkan.dlssg.generation_skipped_busy",
                    "frame=%llu reason=output_ring_full direct_wsi=%u source_prearmed=%u completed_generation=%llu replay_completed=%llu skipped_total=%llu",
                    static_cast<unsigned long long>(pending.frameIndex), runtime.directWsi ? 1u : 0u,
                    sourcePrearmed ? 1u : 0u,
                    static_cast<unsigned long long>(completedGeneration),
                    static_cast<unsigned long long>(generatedReplayCompleted),
                    static_cast<unsigned long long>(skipped));
            }
            return sourcePrearmed
                ? finishPrearmedSourceOnly("output_ring_full", apusr::profile::DecisionTriggerBurst)
                : sourceOnlyOrBacking(pending.frameIndex, "output_ring_full", apusr::profile::DecisionTriggerBurst);
        }
    }

    const auto abortUnsubmittedGeneration = [&]() noexcept {
        apusrFramegenAbortVulkanPresentSubmit(runtime.presentSubmit, sourceImageIndex);
        apusrFramegenRollbackVulkanPresentImageInitialization(runtime.presentImages);
        if (directWsiAcquired) {
            apusrFramegenAbortVulkanDirectWsiAcquire(runtime.directWsi, directPresentationImageIndex);
            directWsiAcquired = false;
            directPresentationImageIndex = APUSR_FRAMEGEN_VULKAN_NO_PRESENT_IMAGE;
            directTargetAcquireSemaphore = nullptr;
        }
    };

    ApusrFramegenVulkanPresentFrameResources presentResources{};
    if (apusrFramegenPrepareVulkanOffscreenGenerationImages(
            runtime.presentImages,
            inputCommandBuffer,
            generationCommandBuffer,
            sourceImageIndex,
            generationSlotIndex,
            directPresentationImageIndex,
            &presentResources) != APUSR_FRAMEGEN_OK) {
        abortUnsubmittedGeneration();
        g_dlssgLastStatus.store(kDlssgStatusConstantsInvalid, std::memory_order_relaxed);
        return sourcePrearmed
            ? finishPrearmedSourceOnly("generation_images_prepare_failed", apusr::profile::DecisionTriggerBurst)
            : sourceOnlyOrBacking(pending.frameIndex, "generation_images_prepare_failed", apusr::profile::DecisionTriggerBurst);
    }

    ApusrFramegenStreamlineVulkanResource hudless{};
    bool hudlessRetained = false;
    if (tags.hudlessColor.tagged) {
        hudless = framegenResource(tags.hudlessColor);
        // Final-form Vulkan FG ingests the transient Streamline HUD-less image
        // directly into FI's provider-owned ping-pong history during Phase A.
        // No independent full-resolution staging image exists anymore.
        if ((hudless.nativeUsage & kVkImageUsageTransferSrcBit) == 0u) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.hudless_contract_unsupported",
                "frame=%u source_image=%u image=%p format=%u layout=%u usage=0x%x extent=%ux%u reason=missing_transfer_src",
                pending.frameIndex, sourceImageIndex, hudless.image, hudless.nativeFormat,
                hudless.nativeLayout, hudless.nativeUsage, hudless.width, hudless.height);
            abortUnsubmittedGeneration();
            return sourcePrearmed
                ? finishPrearmedSourceOnly("hudless_contract_unsupported", apusr::profile::DecisionTriggerBurst)
                : sourceOnlyOrBacking(pending.frameIndex, "hudless_contract_unsupported", apusr::profile::DecisionTriggerBurst);
        }
        hudlessRetained = true;
        if (!g_vulkanFramegenHudlessRetainedLogged.exchange(true, std::memory_order_relaxed)) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.first_hudless_direct_retention",
                "frame=%u source_image=%u source=%p format=%u layout=%u usage=0x%x extent=%ux%u",
                pending.frameIndex, sourceImageIndex, hudless.image, hudless.nativeFormat,
                hudless.nativeLayout, hudless.nativeUsage, hudless.width, hudless.height);
        }
    }
    ApusrFramegenStreamlineVulkanResource distortion{};
    bool distortionRetained = false;
    if (tags.distortionField.tagged) {
        distortion = framegenResource(tags.distortionField);
        if ((distortion.nativeUsage & kVkImageUsageTransferSrcBit) == 0u) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.distortion_contract_unsupported",
                "frame=%u source_image=%u image=%p format=%u layout=%u usage=0x%x extent=%ux%u reason=missing_transfer_src",
                pending.frameIndex, sourceImageIndex, distortion.image, distortion.nativeFormat,
                distortion.nativeLayout, distortion.nativeUsage, distortion.width, distortion.height);
            abortUnsubmittedGeneration();
            return sourcePrearmed
                ? finishPrearmedSourceOnly("distortion_contract_unsupported", apusr::profile::DecisionTriggerBurst)
                : sourceOnlyOrBacking(pending.frameIndex, "distortion_contract_unsupported", apusr::profile::DecisionTriggerBurst);
        }
        distortionRetained = true;
    }
    if (!g_vulkanFramegenInputContractLogged.exchange(true, std::memory_order_relaxed)) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.input_contract",
            "frame=%u source_sequence=%llu depth=%u motion_vectors=%u mv_scale=(%.9g,%.9g) jitter=(%.9g,%.9g) mv_jittered=%u mv_dilated=%u hudless=%u hudless_retained=%u hudless_format=%u hudless_layout=%u hudless_usage=0x%x hudless_extent=%ux%u distortion=%u distortion_retained=%u distortion_format=%u distortion_layout=%u distortion_usage=0x%x distortion_extent=%ux%u backbuffer=%u ui_color_alpha=%u ui_format=%u ui_layout=%u ui_usage=0x%x ui_extent=%ux%u ui_alpha=%u input_completion=1 early_release=1 retained_source=1 source_staging=0",
            pending.frameIndex, static_cast<unsigned long long>(sourceFrameSequence),
            tags.depth.tagged ? 1u : 0u, tags.motionVectors.tagged ? 1u : 0u,
            constants.mvScaleX, constants.mvScaleY, constants.jitterX, constants.jitterY,
            constants.motionVectorsJittered ? 1u : 0u, constants.motionVectorsDilated ? 1u : 0u,
            tags.hudlessColor.tagged ? 1u : 0u, hudlessRetained ? 1u : 0u,
            tags.hudlessColor.nativeFormat, tags.hudlessColor.state, tags.hudlessColor.usage,
            activeWidth(tags.hudlessColor), activeHeight(tags.hudlessColor),
            tags.distortionField.tagged ? 1u : 0u, distortionRetained ? 1u : 0u,
            tags.distortionField.nativeFormat, tags.distortionField.state, tags.distortionField.usage,
            activeWidth(tags.distortionField), activeHeight(tags.distortionField),
            tags.backbuffer.tagged ? 1u : 0u,
            tags.uiColorAndAlpha.tagged ? 1u : 0u, tags.uiColorAndAlpha.nativeFormat,
            tags.uiColorAndAlpha.state, tags.uiColorAndAlpha.usage,
            activeWidth(tags.uiColorAndAlpha), activeHeight(tags.uiColorAndAlpha),
            tags.uiAlpha.tagged ? 1u : 0u);
    }

    ApusrFramegenStreamlineVulkanInputs streamlineInputs{};
    streamlineInputs.deviceContext = runtime.deviceContext;
    streamlineInputs.commandBuffer = inputCommandBuffer;
    streamlineInputs.frameId = sourceFrameSequence;
    streamlineInputs.frameTimeDelta = sourceFrameTimeDeltaMs;
    streamlineInputs.hdr = false;
    streamlineInputs.backbufferTransferFunction = APUSR_FRAMEGEN_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    streamlineInputs.backbufferContract = tags.backbuffer.tagged
        ? framegenResource(tags.backbuffer) : presentResources.presentColor;
    streamlineInputs.depth = framegenResource(tags.depth);
    streamlineInputs.motionVectors = framegenResource(tags.motionVectors);
    streamlineInputs.presentColor = presentResources.presentColor;
    streamlineInputs.hudlessColor = hudlessRetained ? hudless : ApusrFramegenStreamlineVulkanResource{};
    streamlineInputs.distortionField = distortionRetained ? distortion : ApusrFramegenStreamlineVulkanResource{};
    streamlineInputs.generatedOutput = presentResources.generatedOutput;
    streamlineInputs.constants.valid = constants.valid;
    streamlineInputs.constants.jitterOffset = {constants.jitterX, constants.jitterY};
    streamlineInputs.constants.motionVectorScale = {constants.mvScaleX, constants.mvScaleY};
    // FidelityFX can only use game MVs for adjacent source frames. Any source
    // frame skipped by the real-time scheduler invalidates that history, so
    // explicitly reset and warm the new source without presenting its output.
    streamlineInputs.constants.reset = generationWarmupOnly;
    streamlineInputs.constants.depthInverted = constants.depthInverted;
    streamlineInputs.constants.motionVectorsJittered = constants.motionVectorsJittered;
    streamlineInputs.constants.motionVectorsDilated = constants.motionVectorsDilated;
    streamlineInputs.constants.orthographicProjection = constants.orthographicProjection;
    streamlineInputs.constants.cameraNear = constants.cameraNear;
    streamlineInputs.constants.cameraFar = constants.cameraFar;
    streamlineInputs.constants.cameraFov = constants.cameraFOV;
    streamlineInputs.constants.cameraAspectRatio = constants.cameraAspectRatio;
    std::memcpy(streamlineInputs.constants.cameraPosition, constants.cameraPosition, sizeof(constants.cameraPosition));
    std::memcpy(streamlineInputs.constants.cameraUp, constants.cameraUp, sizeof(constants.cameraUp));
    std::memcpy(streamlineInputs.constants.cameraRight, constants.cameraRight, sizeof(constants.cameraRight));
    std::memcpy(streamlineInputs.constants.cameraForward, constants.cameraForward, sizeof(constants.cameraForward));

    ApusrFramegenAdaptedStreamlineVulkanFrame adapted{};
    if (apusrFramegenAdaptStreamlineVulkanFrame(streamlineInputs, &adapted) != APUSR_FRAMEGEN_OK) {
        abortUnsubmittedGeneration();
        g_dlssgLastStatus.store(kDlssgStatusConstantsInvalid, std::memory_order_relaxed);
        return sourcePrearmed
            ? finishPrearmedSourceOnly("streamline_adapt_failed", apusr::profile::DecisionTriggerBurst)
            : sourceOnlyOrBacking(pending.frameIndex, "streamline_adapt_failed", apusr::profile::DecisionTriggerBurst);
    }

    if (runtime.session && !sameSessionDescription(runtime.sessionDescription, adapted.sessionDescription)) {
        // Do not hot-swap session history while dedicated-queue work may still
        // reference it. The recorded staging work has not been submitted, so
        // the application's source image remains untouched on the GPU.
        abortUnsubmittedGeneration();
        if (sourcePrearmed) {
            const std::int32_t sourceResult = finishPrearmedSourceOnly("session_description_changed", apusr::profile::DecisionTriggerBurst);
            destroyVulkanFramegenRuntimeLocked();
            return sourceResult;
        }
        destroyVulkanFramegenRuntimeLocked();
        return sourceOnlyOrBacking(pending.frameIndex, "session_description_changed", apusr::profile::DecisionTriggerBurst);
    }
    if (!runtime.session) {
        const auto sessionCreateStart = std::chrono::steady_clock::now();
        const ApusrFramegenResult sessionCreateResult =
            apusrFramegenCreateVulkanSession(adapted.sessionDescription, {}, &runtime.session);
        const auto sessionCreateUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - sessionCreateStart).count();
        if (sessionCreateUs >= 20000) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.session_create_slow",
                "frame=%u duration_us=%lld result=%u", pending.frameIndex,
                static_cast<long long>(sessionCreateUs), static_cast<unsigned>(sessionCreateResult));
        }
        if (sessionCreateResult != APUSR_FRAMEGEN_OK || !runtime.session) {
            abortUnsubmittedGeneration();
            g_dlssgLastStatus.store(kDlssgStatusConstantsInvalid, std::memory_order_relaxed);
            return sourcePrearmed
                ? finishPrearmedSourceOnly("session_create_failed", apusr::profile::DecisionTriggerBurst)
                : sourceOnlyOrBacking(pending.frameIndex, "session_create_failed", apusr::profile::DecisionTriggerBurst);
        }
        runtime.sessionDescription = adapted.sessionDescription;
    }

    ApusrFramegenResult dispatchResult = apusrFramegenPrepareVulkanSession(
        runtime.session, adapted.frameDescription);
    if (dispatchResult == APUSR_FRAMEGEN_OK) {
        ApusrFramegenVulkanFrameDescription generationFrame = adapted.frameDescription;
        generationFrame.commandBuffer = generationCommandBuffer;
        dispatchResult = apusrFramegenDispatchPreparedVulkanSession(runtime.session, generationFrame);
    }
    if (dispatchResult != APUSR_FRAMEGEN_OK) {
        abortUnsubmittedGeneration();
        // FI history may have advanced in CPU-side session state. Tear down the
        // session/runtime rather than trying to continue with mismatched history.
        if (sourcePrearmed) {
            const std::int32_t sourceResult = finishPrearmedSourceOnly("generation_dispatch_failed", apusr::profile::DecisionTriggerBurst);
            destroyVulkanFramegenRuntimeLocked();
            return sourceResult;
        }
        destroyVulkanFramegenRuntimeLocked();
        return sourceOnlyOrBacking(pending.frameIndex, "generation_dispatch_failed", apusr::profile::DecisionTriggerBurst);
    }

    if (apusrFramegenFinishVulkanOffscreenGenerationImages(
            runtime.presentImages,
            generationCommandBuffer,
            directPresentationImageIndex) != APUSR_FRAMEGEN_OK) {
        abortUnsubmittedGeneration();
        if (sourcePrearmed) {
            const std::int32_t sourceResult = finishPrearmedSourceOnly("generation_images_finish_failed", apusr::profile::DecisionTriggerBurst);
            destroyVulkanFramegenRuntimeLocked();
            return sourceResult;
        }
        destroyVulkanFramegenRuntimeLocked();
        return sourceOnlyOrBacking(pending.frameIndex, "generation_images_finish_failed", apusr::profile::DecisionTriggerBurst);
    }

    std::uint64_t producerReadyValue = 0u;
    std::uint64_t inputCompletionValue = 0u;
    std::uint64_t generationCompletionValue = 0u;
    // Virtual-slot acquisition already proves every consumer of the previous
    // frame in this slot has completed. No replay timeline belongs on Phase A.
    const ApusrFramegenResult submitResult = apusrFramegenSubmitVulkanSplitOffscreenPresentWork(
        runtime.presentSubmit,
        sourceImageIndex,
        info.pWaitSemaphores,
        info.waitSemaphoreCount,
        nullptr,
        0u,
        priorGenerationCompletionValue,
        directTargetAcquireSemaphore,
        &producerReadyValue,
        &inputCompletionValue,
        &generationCompletionValue);
    if (submitResult != APUSR_FRAMEGEN_OK) {
        apusrFramegenRollbackVulkanPresentImageInitialization(runtime.presentImages);
        if (directWsiAcquired)
            apusrFramegenAbortVulkanDirectWsiAcquire(runtime.directWsi, directPresentationImageIndex);
        std::int32_t sourceResult = kVkErrorInitializationFailed;
        if (sourcePrearmed) {
            sourceResult = finishPrearmedSourceOnly(
                "generation_submit_failed", apusr::profile::DecisionTriggerBurst,
                producerReadyValue >= plannedProducerReadyValue);
        } else if (producerReadyValue != 0u) {
            sourceResult = finishReplayedSourceOnlyAfterProducerReady(
                producerReadyValue, "generation_submit_failed", apusr::profile::DecisionTriggerBurst);
        } else {
            // No queue submission consumed the application's binary waits.
            destroyVulkanFramegenRuntimeLocked();
            return sourceOnlyOrBacking(pending.frameIndex, "generation_submit_failed", apusr::profile::DecisionTriggerBurst);
        }
        // Phase A may already have been submitted on the cross-queue path, so
        // FI history is no longer trustworthy when Phase B submission failed.
        destroyVulkanFramegenRuntimeLocked();
        return sourceResult;
    }
    if (producerReadyValue < plannedProducerReadyValue) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.producer_ready_invalid",
            "frame=%u planned=%llu submitted=%llu",
            pending.frameIndex,
            static_cast<unsigned long long>(plannedProducerReadyValue),
            static_cast<unsigned long long>(producerReadyValue));
        destroyVulkanFramegenRuntimeLocked();
        return kVkErrorInitializationFailed;
    }
    if (producerReadyValue != plannedProducerReadyValue) {
        // A timeline signal to a value greater than the prearmed wait also
        // satisfies that wait. Keep this diagnostic, but never turn harmless
        // forward progress into a source-frame failure.
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.producer_ready_advanced",
            "frame=%u planned=%llu submitted=%llu",
            pending.frameIndex,
            static_cast<unsigned long long>(plannedProducerReadyValue),
            static_cast<unsigned long long>(producerReadyValue));
    }
    if (!directWsiAcquired) {
        g_vulkanFramegenGeneratedSlotGenerationValue[generationSlotIndex].store(
            generationCompletionValue, std::memory_order_release);
        g_vulkanFramegenGeneratedSlotReplayValue[generationSlotIndex].store(
            0u, std::memory_order_release);
        g_vulkanFramegenGeneratedSlotDecisionValue[generationSlotIndex].store(
            generationWarmupOnly ? generationCompletionValue : 0u, std::memory_order_release);
    }
    runtime.lastSubmittedGenerationCompletionValue = generationCompletionValue;
    runtime.lastGenerationSourceSequence = sourceFrameSequence;
    if (generationWarmupOnly) {
        const char* const warmupReason = constants.reset
            ? (generationHistoryGap ? "explicit_reset_history_gap" : "explicit_reset")
            : "history_gap";
        recordDlssgDecision(
            pending.frameIndex, apusr::profile::DecisionOutcome::Warmup, warmupReason,
            sourceFrameSequence, previousGenerationSourceSequence, generationCompletionValue,
            priorGenerationCompletionValue, apusr::profile::DecisionTriggerBurst);
    } else {
        recordDlssgDecision(
            pending.frameIndex, apusr::profile::DecisionOutcome::Accepted, "generation_scheduled",
            sourceFrameSequence, generationCompletionValue, priorGenerationCompletionValue,
            directWsiAcquired ? 1u : 0u, 0u);
    }
    LARGE_INTEGER generationSubmittedNow{};
    if (QueryPerformanceCounter(&generationSubmittedNow))
        runtime.generationSubmitQpc = generationSubmittedNow.QuadPart;
    if (generationWarmupOnly) {
        // This generation exists only to establish a valid adjacent source
        // history. Decision is already known (do not replay), while the normal
        // completion gate still prevents another heavy dispatch from reusing
        // its resources before the GPU is done.

        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.generation_history_warmup",
            "frame=%u source_sequence=%llu generation_value=%llu explicit_reset=%u history_gap=%u",
            pending.frameIndex, static_cast<unsigned long long>(sourceFrameSequence),
            static_cast<unsigned long long>(generationCompletionValue), constants.reset ? 1u : 0u,
            generationHistoryGap ? 1u : 0u);
    }

    if (inputCompletionValue != 0u) {
        const bool firstInputCompletionSignal = runtime.lastPresentInputsProcessingCompletionFenceValue == 0u;
        runtime.lastPresentInputsProcessingCompletionFenceValue = inputCompletionValue;
        if (firstInputCompletionSignal) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.first_input_completion_signal",
                "timeline=%p value=%llu frame=%u", runtime.inputsProcessingCompletionFence,
                static_cast<unsigned long long>(inputCompletionValue), pending.frameIndex);
        }
        if (inputCompletionValue <= 4u || (inputCompletionValue & (inputCompletionValue - 1u)) == 0u) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlssg.input_release_progress",
                "frame=%u submitted_value=%llu completed_value=%llu generation_value=%llu",
                pending.frameIndex, static_cast<unsigned long long>(inputCompletionValue),
                static_cast<unsigned long long>(apusrFramegenVulkanCompletedInputValue(runtime.presentSubmit)),
                static_cast<unsigned long long>(generationCompletionValue));
        }
    }
    apusrFramegenCommitVulkanPresentImageInitialization(runtime.presentImages);

    void* prearmedSourceWaits[1]{prearmedSourceReadySemaphore};
    const bool hasPresentableGeneratedFrame = !generationWarmupOnly;
    const bool directGeneratedFrame = hasPresentableGeneratedFrame && directWsiAcquired;
    const bool replayGeneratedFrame = hasPresentableGeneratedFrame && !directWsiAcquired;
    const ApusrFramegenVulkanPresentationSchedule schedule{
        .queue = runtime.presentationQueue,
        .swapchain = swapchain,
        .frameId = pending.frameIndex,
        .sourceImageIndex = sourcePrearmed ? prearmedSourceImageIndex : sourceImageIndex,
        .sourceWaitSemaphores = sourcePrearmed ? prearmedSourceWaits : nullptr,
        .sourceWaitSemaphoreCount = sourcePrearmed ? 1u : 0u,
        .hasGeneratedFrame = hasPresentableGeneratedFrame,
        .paceGeneratedFrame = hasPresentableGeneratedFrame,
        .generatedImageIndex = directGeneratedFrame ? directPresentationImageIndex : generationSlotIndex,
        .generatedWaitSemaphore = nullptr,
        .replayGeneratedFrame = replayGeneratedFrame,
        .directGeneratedFrame = directGeneratedFrame,
        .generationReplaySlotIndex = generationSlotIndex,
        .generationStartSemaphore = apusrFramegenVulkanProducerReadySemaphore(runtime.presentSubmit),
        .generationStartValue = producerReadyValue,
        .generationCompletionSemaphore = apusrFramegenVulkanGenerationCompletionSemaphore(runtime.presentSubmit),
        .generationCompletionValue = generationCompletionValue,
        .generatedReplayCompletionValue = 0u, // reserve only if fallback generation meets the deadline
        .replaySourceFrame = !sourcePrearmed,
        .sourceReplaySlotIndex = sourceImageIndex,
        .sourceReplayCompletionValue = sourceReplayCompletionValue,
        .sourceReplayUpstreamSemaphore = sourcePrearmed
            ? nullptr : apusrFramegenVulkanProducerReadySemaphore(runtime.presentSubmit),
        .sourceReplayUpstreamValue = sourcePrearmed ? 0u : producerReadyValue,
        .sourceFrameTimeMs = sourceFrameTimeDeltaMs,
    };
    const ApusrFramegenResult presentationScheduleResult =
        apusrFramegenScheduleVulkanPresentation(runtime.presentation, schedule);
    if (presentationScheduleResult != APUSR_FRAMEGEN_OK) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.presenter_admission_invariant_failed",
            "frame=%u mode=generated_pair result=%u virtual_images=%u real_images=%u source_prearmed=%u direct_wsi=%u",
            pending.frameIndex, static_cast<unsigned>(presentationScheduleResult),
            runtime.swapchainInfo.imageCount, runtime.swapchainInfo.realImageCount,
            sourcePrearmed ? 1u : 0u, directWsiAcquired ? 1u : 0u);
        // Generation/source copies may already be submitted. Cold teardown owns
        // the required waits and WSI retirement; do not serialize the game's hot
        // present thread behind the existing presenter backlog as a fallback.
        destroyVulkanFramegenRuntimeLocked();
        return kVkErrorInitializationFailed;
    }

    // eBlockNoClientQueues gives Phase A a precise lifetime boundary for every
    // transient application resource. Once Phase A and the independent source
    // replay copy are complete, the virtual backbuffer can be reused; retaining
    // it through the provider-owned Phase B interpolation work only creates
    // unnecessary swapchain pressure. Preserve the old conservative dependency
    // when the split input-release contract is unavailable.
    void* const virtualSourceConsumedSemaphore = inputCompletionValue != 0u
        ? runtime.inputsProcessingCompletionFence
        : apusrFramegenVulkanGenerationCompletionSemaphore(runtime.presentSubmit);
    const std::uint64_t virtualSourceConsumedValue = inputCompletionValue != 0u
        ? inputCompletionValue
        : generationCompletionValue;
    if (!apusrVulkanHostReleaseVirtualSwapchainImage(
            swapchain, sourceImageIndex,
            apusrFramegenVulkanReplayConsumedSemaphore(runtime.sourceReplay), sourceReplayCompletionValue,
            virtualSourceConsumedSemaphore, virtualSourceConsumedValue)) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.virtual_slot_release_failed",
            "frame=%u source_slot=%u source_replay_value=%llu consumed_value=%llu input_release=%u",
            pending.frameIndex, sourceImageIndex,
            static_cast<unsigned long long>(sourceReplayCompletionValue),
            static_cast<unsigned long long>(virtualSourceConsumedValue),
            inputCompletionValue != 0u ? 1u : 0u);
        destroyVulkanFramegenRuntimeLocked();
        return kVkErrorInitializationFailed;
    }

    g_dlssgLastStatus.store(kDlssgStatusOk, std::memory_order_relaxed);
    return asyncSuccess();
}

int __cdecl apusrSlReflexGetState(ReflexStatePrefix* state)
{
    if (state == nullptr) {
        return kResultMissingInputParameter;
    }
    state->lowLatencyAvailable = false;
    state->latencyReportAvailable = false;
    state->statsWindowMessage = 0u;

    if (isVulkanStreamlinePath()) {
        if (void* device = currentVulkanDevice()) {
            state->lowLatencyAvailable = apusrVulkanHostAntiLagAvailable(device);
        }
        return kResultOk;
    }
    if (isD3d12StreamlinePath()) {
        state->lowLatencyAvailable = g_d3d12AntiLagAvailable.load(std::memory_order_acquire);
        return kResultOk;
    }
    return kResultFeatureNotSupported;
}

int __cdecl apusrSlReflexSleep(const void* frame)
{
    const auto profileStream = reflexProfileStream();
    const std::uint64_t profileFrame = reflexProfileFrameId(frame);
    apusr::profile::CpuScope profileScope(profileStream, profileFrame, 0u, "reflex.sleep");
    if (frame == nullptr) {
        return kResultMissingInputParameter;
    }
    if (isVulkanStreamlinePath()) {
        // VK_AMD_anti_lag has no Reflex-style explicit sleep call. Its INPUT
        // update belongs at PCL eSimulationStart, so slReflexSleep stays a
        // callable no-op on Vulkan.
        return kResultOk;
    }
    if (isD3d12StreamlinePath()) {
        const HRESULT hr = updateD3d12AntiLagForFrame(frame);
        if (FAILED(hr)) {
            apusr::host_discovery::eventf(
                "streamline", "reflex.d3d12.sleep_failed",
                "frame=%p hr=0x%08lx", frame, static_cast<unsigned long>(hr));
            return kResultFeatureNotSupported;
        }
        return kResultOk;
    }
    return kResultFeatureNotSupported;
}

int __cdecl apusrSlReflexSetOptions(const ReflexOptions* options)
{
    if (options == nullptr) {
        return kResultMissingInputParameter;
    }
    if (options->mode != ReflexMode::eOff &&
        options->mode != ReflexMode::eLowLatency &&
        options->mode != ReflexMode::eLowLatencyWithBoost) {
        return kResultInvalidParameter;
    }
    if (!isReflexStreamlinePath()) {
        return kResultFeatureNotSupported;
    }

    // Both AMD native low-latency interfaces are binary on/off mechanisms for
    // this contract. Reflex LowLatencyWithBoost therefore maps to ordinary
    // Anti-Lag ON; APUSR does not invent a separate boost semantic.
    const std::uint32_t antiLagMode = options->mode == ReflexMode::eOff
        ? APUSR_VK_HOST_ANTI_LAG_OFF
        : APUSR_VK_HOST_ANTI_LAG_ON;
    const std::uint32_t maxFps = reflexFrameLimitToFps(options->frameLimitUs);
    const std::uint32_t previousAntiLagMode = g_reflexMode.exchange(antiLagMode, std::memory_order_relaxed);
    g_reflexMaxFps.store(maxFps, std::memory_order_relaxed);

    if (const auto stream = reflexProfileStream()) {
        if (previousAntiLagMode != antiLagMode) {
            apusr::profile::recordStateTransition({
                .stream = stream,
                .name = "streamline.reflex.mode",
                .reason = "slReflexSetOptions",
                .previousValue = previousAntiLagMode,
                .nextValue = antiLagMode,
            });
        }
        apusr::profile::recordCounter({
            .stream = stream,
            .name = "streamline.reflex.max_fps",
            .value = maxFps,
            .value2 = options->frameLimitUs,
        });
    }

    if (isVulkanStreamlinePath()) {
        // TODO(Vulkan Reflex teardown): DO NOT issue any native Anti-Lag OFF
        // transition here. Affected hosts reproduced severe post-FG WSI blocking for both
        // standalone and paired OFF transitions. Keep only the logical state
        // until a genuinely different driver/device-lifecycle solution exists.
        (void)previousAntiLagMode;
    } else {
        // Update the driver state immediately without inserting the latency
        // delay in the settings thread. slReflexSleep/SimulationStart performs
        // the actual once-per-frame delay.
        std::lock_guard<std::mutex> lock(g_d3d12AntiLagMutex);
        g_d3d12AntiLagLastInputFrame = nullptr;
        const HRESULT stateHr = setD3d12AntiLagStateLocked(antiLagMode != APUSR_VK_HOST_ANTI_LAG_OFF, maxFps);
        if (FAILED(stateHr)) {
            apusr::host_discovery::eventf(
                "streamline", "reflex.d3d12.state_failed",
                "hr=0x%08lx enabled=%u max_fps=%u", static_cast<unsigned long>(stateHr),
                antiLagMode != APUSR_VK_HOST_ANTI_LAG_OFF ? 1u : 0u, maxFps);
        }
    }

    apusr::host_discovery::eventf(
        "streamline", "reflex.options",
        "backend=%s mode=%u anti_lag_mode=%u frame_limit_us=%u max_fps=%u boost_collapsed_to_antilag_on=%u",
        isVulkanStreamlinePath() ? "vulkan" : "d3d12",
        static_cast<std::uint32_t>(options->mode),
        antiLagMode,
        options->frameLimitUs,
        maxFps,
        options->mode == ReflexMode::eLowLatencyWithBoost ? 1u : 0u);
    if (isVulkanStreamlinePath() && options->mode == ReflexMode::eOff && options->frameLimitUs != 0u) {
        apusr::host_discovery::event(
            "streamline", "reflex.frame_limit_limitation",
            "reflex_mode_off_preserved; VK_AMD_anti_lag_does_not_provide_an_independent_off_mode_frame_cap");
    }
    return kResultOk;
}

int __cdecl apusrSlReflexSetCameraData(const void*, const void*, const void*)
{
    // Camera prediction/latency-warp is a separate Reflex capability. Keeping
    // the setter harmless lets integrations that submit optional camera data
    // continue to use the low-latency path without advertising prediction.
    return kResultOk;
}

int __cdecl apusrSlReflexGetPredictedCameraData(const void*, const void*, void*)
{
    return kResultFeatureNotSupported;
}

int __cdecl apusrSlPCLSetMarker(const std::uint32_t marker, const void* frame)
{
    const auto profileStream = reflexProfileStream();
    const std::uint64_t profileFrame = reflexProfileFrameId(frame);
    const char* profileName = nullptr;
    if (marker == 0u)
        profileName = "reflex.pcl.simulation_start";
    else if (marker == 3u || marker == 10u)
        profileName = "reflex.pcl.render_submit_end";
    else if (marker == 4u || marker == 11u)
        profileName = "reflex.pcl.present_start";
    apusr::profile::CpuScope profileScope(
        profileName != nullptr ? profileStream : apusr::profile::StreamHandle{},
        profileFrame, 0u, profileName);

    if (isVulkanStreamlinePath() && frame != nullptr) {
        std::uint64_t frameIndex = 0;
        if (resolveFrameToken(frame, frameIndex)) {
            if (void* device = currentVulkanDevice()) {
                if (marker == 0u) { // eSimulationStart: immediately before input/simulation work
                    const std::uint32_t mode = g_reflexMode.load(std::memory_order_relaxed);
                    const std::uint32_t maxFps = g_reflexMaxFps.load(std::memory_order_relaxed);
                    (void)apusrVulkanHostAntiLagInput(device, frameIndex, mode, maxFps);
                } else if (marker == 4u || marker == 11u) {
                    // ePresentStart / eOutOfBandPresentStart. The actual PRESENT
                    // Anti-Lag update is delayed to the innermost real present.
                    apusrVulkanHostAntiLagSetPresentFrame(device, frameIndex);
                    if (g_dlssgEnabled.load(std::memory_order_acquire)) {
                        std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
                        queuePendingFramegenPresentLocked(frame, frameIndex);
                        if (!g_dlssgFirstPclPresentLogged.exchange(true, std::memory_order_relaxed)) {
                            apusr::host_discovery::eventf(
                                "streamline", "vulkan.dlssg.first_pcl_present_marker",
                                "marker=%u frame=%llu", marker,
                                static_cast<unsigned long long>(frameIndex));
                        }
                    }
                }
            }
        } else if (!g_pclUnresolvedFrameLogged.exchange(true, std::memory_order_relaxed)) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.pcl.unresolved_frame_token",
                "marker=%u frame=%p", marker, frame);
        }
    } else if (isD3d12StreamlinePath() && frame != nullptr) {
        if (marker == 0u) { // eSimulationStart
            const HRESULT hr = updateD3d12AntiLagForFrame(frame);
            if (FAILED(hr) && hr != E_NOINTERFACE) {
                apusr::host_discovery::eventf(
                    "streamline", "reflex.d3d12.input_marker_failed",
                    "frame=%p hr=0x%08lx", frame, static_cast<unsigned long>(hr));
            }
        } else if (marker == 3u || marker == 10u) {
            // eRenderSubmitEnd / eOutOfBandRenderSubmitEnd.
            markD3d12AntiLagRenderEnd(frame);
        }
    }

    if (auto fn = g_backingPclSetMarker.load(std::memory_order_acquire)) {
        return fn(marker, frame);
    }
    return kResultOk;
}

template <std::size_t N>
void setSpoofName(WCHAR (&description)[N])
{
    wcsncpy_s(description, N, L"NVIDIA GeForce RTX 3090", _TRUNCATE);
}

bool shouldSpoofVendor(UINT vendorId)
{
    return vendorId != 0u && vendorId != kVendorMicrosoft && vendorId != kSpoofVendorNvidia;
}

void spoofDesc(DXGI_ADAPTER_DESC* desc)
{
    if (desc == nullptr || !shouldSpoofVendor(desc->VendorId)) {
        return;
    }
    setSpoofName(desc->Description);
    desc->VendorId = kSpoofVendorNvidia;
    // Streamline's Vulkan capability discovery maps the DXGI device id back to
    // a real VkPhysicalDevice. Preserve that identity while making only the
    // NVIDIA support gate see an NVIDIA vendor. D3D paths retain the existing
    // RTX-shaped device id used by host discovery.
    if (g_slRenderApi.load(std::memory_order_relaxed) != kSlRenderApiVulkan) {
        desc->DeviceId = kSpoofDeviceRTX3090;
    }
    desc->DedicatedVideoMemory = std::max<UINT64>(desc->DedicatedVideoMemory, kSpoofDedicatedVideoMemory);
}

void spoofDesc(DXGI_ADAPTER_DESC1* desc)
{
    if (desc == nullptr || !shouldSpoofVendor(desc->VendorId)) {
        return;
    }
    setSpoofName(desc->Description);
    desc->VendorId = kSpoofVendorNvidia;
    if (g_slRenderApi.load(std::memory_order_relaxed) != kSlRenderApiVulkan) {
        desc->DeviceId = kSpoofDeviceRTX3090;
    }
    desc->DedicatedVideoMemory = std::max<UINT64>(desc->DedicatedVideoMemory, kSpoofDedicatedVideoMemory);
    desc->Flags &= ~static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE);
}

void spoofDesc(DXGI_ADAPTER_DESC2* desc)
{
    if (desc == nullptr || !shouldSpoofVendor(desc->VendorId)) {
        return;
    }
    setSpoofName(desc->Description);
    desc->VendorId = kSpoofVendorNvidia;
    if (g_slRenderApi.load(std::memory_order_relaxed) != kSlRenderApiVulkan) {
        desc->DeviceId = kSpoofDeviceRTX3090;
    }
    desc->DedicatedVideoMemory = std::max<UINT64>(desc->DedicatedVideoMemory, kSpoofDedicatedVideoMemory);
    desc->Flags &= ~static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE);
}

void spoofDesc(DXGI_ADAPTER_DESC3* desc)
{
    if (desc == nullptr || !shouldSpoofVendor(desc->VendorId)) {
        return;
    }
    setSpoofName(desc->Description);
    desc->VendorId = kSpoofVendorNvidia;
    if (g_slRenderApi.load(std::memory_order_relaxed) != kSlRenderApiVulkan) {
        desc->DeviceId = kSpoofDeviceRTX3090;
    }
    desc->DedicatedVideoMemory = std::max<UINT64>(desc->DedicatedVideoMemory, kSpoofDedicatedVideoMemory);
}


void fillNeutralOptionalFeatureRequirements(FeatureRequirements* requirements)
{
    if (requirements == nullptr) {
        return;
    }

    // Some hosts batch requirement discovery for optional Streamline features
    // before deciding which ones are actually supported.  Returning
    // FeatureMissing here aborts those hosts before they can reach APUSR-owned
    // DLSS SR.  Keep discovery neutral; actual support remains authoritative in
    // slIsFeatureSupported/slIsFeatureLoaded/slGetFeatureFunction/evaluation.
    requirements->flags = isVulkanStreamlinePath() ? (1u << 2) : 0u;
    requirements->maxNumCPUThreads = 0;
    requirements->maxNumViewports = 1;
    requirements->numRequiredTags = 0;
    requirements->requiredTags = nullptr;
    requirements->osVersionDetected = {0u, 0u, 0u};
    requirements->osVersionRequired = {0u, 0u, 0u};
    requirements->driverVersionDetected = {0u, 0u, 0u};
    requirements->driverVersionRequired = {0u, 0u, 0u};
    requirements->vkNumComputeQueuesRequired = 0;
    requirements->vkNumGraphicsQueuesRequired = 0;
    requirements->vkNumDeviceExtensions = 0;
    requirements->vkDeviceExtensions = nullptr;
    requirements->vkNumInstanceExtensions = 0;
    requirements->vkInstanceExtensions = nullptr;
    requirements->vkNumFeatures12 = 0;
    requirements->vkFeatures12 = nullptr;
    requirements->vkNumFeatures13 = 0;
    requirements->vkFeatures13 = nullptr;
    if (requirements->structVersion >= 2u) {
        requirements->vkNumOpticalFlowQueuesRequired = 0;
    }
}

void fillDlssRequirements(FeatureRequirements* requirements)
{
    if (requirements == nullptr) {
        return;
    }

    // Streamline's const char** ABI requires mutable pointer slots.
    static const char* kApusrFeatures12[] = {
        "shaderFloat16",
        "shaderInt8",
    };
    requirements->flags = isVulkanStreamlinePath() ? (1u << 2) : (1u << 1);
    requirements->maxNumCPUThreads = 0;
    // The current APUSR NGX facade owns one temporal SR context at a time.
    // Do not over-advertise multi-viewport history isolation.
    requirements->maxNumViewports = 1;
    requirements->numRequiredTags = static_cast<std::uint32_t>(sizeof(kRequiredDlssTags) / sizeof(kRequiredDlssTags[0]));
    requirements->requiredTags = kRequiredDlssTags;
    requirements->osVersionDetected = {0u, 0u, 0u};
    requirements->osVersionRequired = {10u, 0u, 0u};
    requirements->driverVersionDetected = {0u, 0u, 0u};
    requirements->driverVersionRequired = {0u, 0u, 0u};
    requirements->vkNumComputeQueuesRequired = 0;
    requirements->vkNumGraphicsQueuesRequired = 0;
    requirements->vkNumDeviceExtensions = 0;
    requirements->vkDeviceExtensions = nullptr;
    requirements->vkNumInstanceExtensions = 0;
    requirements->vkInstanceExtensions = nullptr;
    requirements->vkNumFeatures12 = 0;
    requirements->vkFeatures12 = nullptr;
    requirements->vkNumFeatures13 = 0;
    requirements->vkFeatures13 = nullptr;
    if (requirements->structVersion >= 2u) {
        requirements->vkNumOpticalFlowQueuesRequired = 0;
    }

    if (isVulkanStreamlinePath()) {
        requirements->vkNumFeatures12 = static_cast<std::uint32_t>(sizeof(kApusrFeatures12) / sizeof(kApusrFeatures12[0]));
        requirements->vkFeatures12 = kApusrFeatures12;
        apusr::host_discovery::eventf(
            "vulkan", "dlss.requirements",
            "owner=apusr features12=%u features13=0 integer_dot=vulkan_host_capture core_features=vulkan_host_capture",
            requirements->vkNumFeatures12);
    }
}

void fillDlssgRequirements(FeatureRequirements* requirements)
{
    fillNeutralOptionalFeatureRequirements(requirements);
    if (requirements == nullptr || !isVulkanStreamlinePath()) {
        return;
    }

    // Some Streamline 2.7.x integrations only query DLSS-G state after they have
    // assigned graphics, compute and optical-flow queue roles. APUSR's optical
    // flow is ordinary Vulkan compute, so the optical role is a compatibility
    // handle rather than a request for VK_NV_optical_flow. The host-capture
    // layer exposes that role on one graphics+compute family without exposing
    // the NV extension or any NV optical-flow commands.
    apusr::streamline::dlssg_abi::writeLegacyVulkanQueueRoles(*requirements);
    apusrVulkanHostRequestLegacyOpticalFlowQueueRole(requirements->structVersion >= 2u);

    apusr::host_discovery::eventf(
        "streamline", "dlssg.requirements",
        "struct_version=%zu graphics_queues=%u compute_queues=%u optical_flow_role_queues=%u native_nv_optical_flow=0",
        requirements->structVersion,
        requirements->vkNumGraphicsQueuesRequired,
        requirements->vkNumComputeQueuesRequired,
        requirements->structVersion >= 2u ? requirements->vkNumOpticalFlowQueuesRequired : 0u);
}

float dlssScale(DLSSMode mode)
{
    switch (mode) {
    case DLSSMode::eDLAA:
        return 1.0f;
    case DLSSMode::eUltraQuality:
        return 0.77f;
    case DLSSMode::eMaxQuality:
        return 0.6666667f;
    case DLSSMode::eBalanced:
        return 0.58f;
    case DLSSMode::eMaxPerformance:
        return 0.5f;
    case DLSSMode::eUltraPerformance:
        return 0.3333333f;
    case DLSSMode::eOff:
    default:
        return 1.0f;
    }
}

class AdapterProxy final : public IDXGIAdapter4 {
public:
    explicit AdapterProxy(IUnknown* inner) : m_inner(inner) {}
    ~AdapterProxy()
    {
        {
            std::lock_guard<std::mutex> lock(g_adapterProxyMutex);
            g_adapterProxyToInner.erase(static_cast<IDXGIAdapter4*>(this));
        }
        if (m_inner != nullptr) {
            m_inner->Release();
            m_inner = nullptr;
        }
    }

    static AdapterProxy* create(IUnknown* inner)
    {
        if (inner == nullptr) {
            return nullptr;
        }
        AdapterProxy* proxy = new AdapterProxy(inner);
        {
            std::lock_guard<std::mutex> lock(g_adapterProxyMutex);
            g_adapterProxyToInner[static_cast<IDXGIAdapter4*>(proxy)] = inner;
        }
        return proxy;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIAdapter) ||
            riid == __uuidof(IDXGIAdapter1) ||
            riid == __uuidof(IDXGIAdapter2) ||
            riid == __uuidof(IDXGIAdapter3) ||
            riid == __uuidof(IDXGIAdapter4)) {
            IUnknown* supported = nullptr;
            const HRESULT hr = m_inner->QueryInterface(riid, reinterpret_cast<void**>(&supported));
            if (FAILED(hr)) {
                return hr;
            }
            supported->Release();
            *ppvObject = static_cast<IDXGIAdapter4*>(this);
            AddRef();
            return S_OK;
        }
        return m_inner->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override
    {
        if (auto* obj = query<IDXGIObject>()) {
            const HRESULT hr = obj->SetPrivateData(Name, DataSize, pData);
            obj->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override
    {
        if (auto* obj = query<IDXGIObject>()) {
            const HRESULT hr = obj->SetPrivateDataInterface(Name, pUnknown);
            obj->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override
    {
        if (auto* obj = query<IDXGIObject>()) {
            const HRESULT hr = obj->GetPrivateData(Name, pDataSize, pData);
            obj->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override
    {
        if (auto* obj = query<IDXGIObject>()) {
            const HRESULT hr = obj->GetParent(riid, ppParent);
            obj->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override
    {
        if (auto* a = query<IDXGIAdapter>()) {
            const HRESULT hr = a->EnumOutputs(Output, ppOutput);
            a->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override
    {
        if (auto* a = query<IDXGIAdapter>()) {
            const HRESULT hr = a->GetDesc(pDesc);
            a->Release();
            if (SUCCEEDED(hr)) {
                spoofDesc(pDesc);
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override
    {
        if (auto* a = query<IDXGIAdapter>()) {
            const HRESULT hr = a->CheckInterfaceSupport(InterfaceName, pUMDVersion);
            a->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override
    {
        if (auto* a = query<IDXGIAdapter1>()) {
            const HRESULT hr = a->GetDesc1(pDesc);
            a->Release();
            if (SUCCEEDED(hr)) {
                spoofDesc(pDesc);
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2* pDesc) override
    {
        if (auto* a = query<IDXGIAdapter2>()) {
            const HRESULT hr = a->GetDesc2(pDesc);
            a->Release();
            if (SUCCEEDED(hr)) {
                spoofDesc(pDesc);
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override
    {
        if (auto* a = query<IDXGIAdapter3>()) {
            const HRESULT hr = a->RegisterHardwareContentProtectionTeardownStatusEvent(hEvent, pdwCookie);
            a->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie) override
    {
        if (auto* a = query<IDXGIAdapter3>()) {
            a->UnregisterHardwareContentProtectionTeardownStatus(dwCookie);
            a->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(UINT NodeIndex,
                                                   DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                   DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) override
    {
        if (auto* a = query<IDXGIAdapter3>()) {
            const HRESULT hr = a->QueryVideoMemoryInfo(NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);
            a->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(UINT NodeIndex,
                                                        DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                        UINT64 Reservation) override
    {
        if (auto* a = query<IDXGIAdapter3>()) {
            const HRESULT hr = a->SetVideoMemoryReservation(NodeIndex, MemorySegmentGroup, Reservation);
            a->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE hEvent, DWORD* pdwCookie) override
    {
        if (auto* a = query<IDXGIAdapter3>()) {
            const HRESULT hr = a->RegisterVideoMemoryBudgetChangeNotificationEvent(hEvent, pdwCookie);
            a->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie) override
    {
        if (auto* a = query<IDXGIAdapter3>()) {
            a->UnregisterVideoMemoryBudgetChangeNotification(dwCookie);
            a->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3* pDesc) override
    {
        if (auto* a = query<IDXGIAdapter4>()) {
            const HRESULT hr = a->GetDesc3(pDesc);
            a->Release();
            if (SUCCEEDED(hr)) {
                spoofDesc(pDesc);
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    IUnknown* innerNoAddRef() const { return m_inner; }

private:
    template <typename T>
    T* query() const
    {
        T* out = nullptr;
        if (m_inner != nullptr && SUCCEEDED(m_inner->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&out)))) {
            return out;
        }
        return nullptr;
    }

    std::atomic<ULONG> m_refs{1};
    IUnknown* m_inner = nullptr;
};

struct UnwrappedAdapter {
    void* ptr = nullptr;
    bool release = false;
};

UnwrappedAdapter unwrapAdapter(void* adapter)
{
    if (adapter == nullptr) {
        return {};
    }
    std::lock_guard<std::mutex> lock(g_adapterProxyMutex);
    const auto it = g_adapterProxyToInner.find(adapter);
    if (it == g_adapterProxyToInner.end()) {
        return {adapter, false};
    }
    if (it->second != nullptr) {
        it->second->AddRef();
    }
    return {it->second, true};
}

HRESULT wrapAdapterResult(REFIID riid, void** ppAdapter)
{
    if (ppAdapter == nullptr || *ppAdapter == nullptr) {
        return S_OK;
    }
    IUnknown* inner = reinterpret_cast<IUnknown*>(*ppAdapter);
    *ppAdapter = nullptr;
    AdapterProxy* proxy = AdapterProxy::create(inner);
    if (proxy == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = proxy->QueryInterface(riid, ppAdapter);
    proxy->Release();
    return hr;
}

class FactoryProxy final : public IDXGIFactory6 {
public:
    explicit FactoryProxy(IUnknown* inner) : m_inner(inner) {}
    ~FactoryProxy()
    {
        if (m_inner != nullptr) {
            m_inner->Release();
            m_inner = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (ppvObject == nullptr) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) ||
            riid == __uuidof(IDXGIFactory2) ||
            riid == __uuidof(IDXGIFactory3) ||
            riid == __uuidof(IDXGIFactory4) ||
            riid == __uuidof(IDXGIFactory5) ||
            riid == __uuidof(IDXGIFactory6)) {
            IUnknown* supported = nullptr;
            const HRESULT hr = m_inner->QueryInterface(riid, reinterpret_cast<void**>(&supported));
            if (FAILED(hr)) {
                return hr;
            }
            supported->Release();
            *ppvObject = static_cast<IDXGIFactory6*>(this);
            AddRef();
            return S_OK;
        }
        return m_inner->QueryInterface(riid, ppvObject);
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs = --m_refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override
    {
        if (auto* obj = query<IDXGIObject>()) {
            const HRESULT hr = obj->SetPrivateData(Name, DataSize, pData);
            obj->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override
    {
        if (auto* obj = query<IDXGIObject>()) {
            const HRESULT hr = obj->SetPrivateDataInterface(Name, pUnknown);
            obj->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override
    {
        if (auto* obj = query<IDXGIObject>()) {
            const HRESULT hr = obj->GetPrivateData(Name, pDataSize, pData);
            obj->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override
    {
        if (auto* obj = query<IDXGIObject>()) {
            const HRESULT hr = obj->GetParent(riid, ppParent);
            obj->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override
    {
        if (auto* f = query<IDXGIFactory>()) {
            const HRESULT hr = f->EnumAdapters(Adapter, ppAdapter);
            f->Release();
            if (SUCCEEDED(hr)) {
                return wrapAdapterResult(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(ppAdapter));
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override
    {
        if (auto* f = query<IDXGIFactory>()) {
            const HRESULT hr = f->MakeWindowAssociation(WindowHandle, Flags);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override
    {
        if (auto* f = query<IDXGIFactory>()) {
            const HRESULT hr = f->GetWindowAssociation(pWindowHandle);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) override
    {
        if (auto* f = query<IDXGIFactory>()) {
            const HRESULT hr = f->CreateSwapChain(pDevice, pDesc, ppSwapChain);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override
    {
        if (auto* f = query<IDXGIFactory>()) {
            const HRESULT hr = f->CreateSoftwareAdapter(Module, ppAdapter);
            f->Release();
            if (SUCCEEDED(hr)) {
                return wrapAdapterResult(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(ppAdapter));
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override
    {
        if (auto* f = query<IDXGIFactory1>()) {
            const HRESULT hr = f->EnumAdapters1(Adapter, ppAdapter);
            f->Release();
            if (SUCCEEDED(hr)) {
                return wrapAdapterResult(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(ppAdapter));
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    BOOL STDMETHODCALLTYPE IsCurrent() override
    {
        if (auto* f = query<IDXGIFactory1>()) {
            const BOOL result = f->IsCurrent();
            f->Release();
            return result;
        }
        return TRUE;
    }

    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const BOOL result = f->IsWindowedStereoEnabled();
            f->Release();
            return result;
        }
        return FALSE;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice,
                                                     HWND hWnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                                     IDXGIOutput* pRestrictToOutput,
                                                     IDXGISwapChain1** ppSwapChain) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const HRESULT hr = f->CreateSwapChainForHwnd(pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice,
                                                           IUnknown* pWindow,
                                                           const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                           IDXGIOutput* pRestrictToOutput,
                                                           IDXGISwapChain1** ppSwapChain) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const HRESULT hr = f->CreateSwapChainForCoreWindow(pDevice, pWindow, pDesc, pRestrictToOutput, ppSwapChain);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const HRESULT hr = f->GetSharedResourceAdapterLuid(hResource, pLuid);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const HRESULT hr = f->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const HRESULT hr = f->RegisterStereoStatusEvent(hEvent, pdwCookie);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            f->UnregisterStereoStatus(dwCookie);
            f->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const HRESULT hr = f->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const HRESULT hr = f->RegisterOcclusionStatusEvent(hEvent, pdwCookie);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            f->UnregisterOcclusionStatus(dwCookie);
            f->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice,
                                                            const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                            IDXGIOutput* pRestrictToOutput,
                                                            IDXGISwapChain1** ppSwapChain) override
    {
        if (auto* f = query<IDXGIFactory2>()) {
            const HRESULT hr = f->CreateSwapChainForComposition(pDevice, pDesc, pRestrictToOutput, ppSwapChain);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    UINT STDMETHODCALLTYPE GetCreationFlags() override
    {
        if (auto* f = query<IDXGIFactory3>()) {
            const UINT result = f->GetCreationFlags();
            f->Release();
            return result;
        }
        return 0;
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) override
    {
        if (auto* f = query<IDXGIFactory4>()) {
            const HRESULT hr = f->EnumAdapterByLuid(AdapterLuid, riid, ppvAdapter);
            f->Release();
            if (SUCCEEDED(hr)) {
                return wrapAdapterResult(riid, ppvAdapter);
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID riid, void** ppvAdapter) override
    {
        if (auto* f = query<IDXGIFactory4>()) {
            const HRESULT hr = f->EnumWarpAdapter(riid, ppvAdapter);
            f->Release();
            if (SUCCEEDED(hr)) {
                return wrapAdapterResult(riid, ppvAdapter);
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override
    {
        if (auto* f = query<IDXGIFactory5>()) {
            const HRESULT hr = f->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT Adapter,
                                                         DXGI_GPU_PREFERENCE GpuPreference,
                                                         REFIID riid,
                                                         void** ppvAdapter) override
    {
        if (auto* f = query<IDXGIFactory6>()) {
            const HRESULT hr = f->EnumAdapterByGpuPreference(Adapter, GpuPreference, riid, ppvAdapter);
            f->Release();
            if (SUCCEEDED(hr)) {
                return wrapAdapterResult(riid, ppvAdapter);
            }
            return hr;
        }
        return E_NOINTERFACE;
    }

private:
    template <typename T>
    T* query() const
    {
        T* out = nullptr;
        if (m_inner != nullptr && SUCCEEDED(m_inner->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&out)))) {
            return out;
        }
        return nullptr;
    }

    std::atomic<ULONG> m_refs{1};
    IUnknown* m_inner = nullptr;
};

HRESULT wrapFactoryResult(REFIID riid, void** ppFactory)
{
    if (ppFactory == nullptr || *ppFactory == nullptr) {
        return S_OK;
    }
    IUnknown* inner = reinterpret_cast<IUnknown*>(*ppFactory);
    *ppFactory = nullptr;
    FactoryProxy* proxy = new FactoryProxy(inner);
    const HRESULT hr = proxy->QueryInterface(riid, ppFactory);
    proxy->Release();
    return hr;
}

int __cdecl apusrSlDLSSGetOptimalSettings(const DLSSOptions* options, DLSSOptimalSettings* settings)
{
    if (options == nullptr || settings == nullptr) {
        return kResultInvalidParameter;
    }
    if (options->outputWidth == 0 || options->outputHeight == 0 ||
        options->outputWidth == 0xFFFFFFFFu || options->outputHeight == 0xFFFFFFFFu) {
        return kResultMissingInputParameter;
    }

    const float scale = dlssScale(options->mode);
    const std::uint32_t optW = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(options->outputWidth) * scale + 0.5f));
    const std::uint32_t optH = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(options->outputHeight) * scale + 0.5f));

    settings->optimalRenderWidth = optW;
    settings->optimalRenderHeight = optH;
    settings->optimalSharpness = 0.0f;
    settings->renderWidthMin = std::max(1u, options->outputWidth / 3u);
    settings->renderHeightMin = std::max(1u, options->outputHeight / 3u);
    settings->renderWidthMax = options->outputWidth;
    settings->renderHeightMax = options->outputHeight;
    return kResultOk;
}

int __cdecl apusrSlDLSSGetState(const ViewportHandle*, DLSSState* state)
{
    if (state == nullptr) {
        return kResultInvalidParameter;
    }
    state->estimatedVRAMUsageInBytes = 0;
    return kResultOk;
}

int __cdecl apusrSlDLSSSetOptions(const ViewportHandle* viewport, const DLSSOptions* options)
{
    g_dlssLoaded.store(true, std::memory_order_relaxed);
    if (viewport != nullptr && options != nullptr) {
        CapturedOptions captured;
        captured.mode = options->mode;
        captured.outputWidth = options->outputWidth;
        captured.outputHeight = options->outputHeight;
        captured.sharpness = 0.0f;
        captured.preExposure = options->preExposure == 0.0f ? 1.0f : options->preExposure;
        captured.exposureScale = options->exposureScale == 0.0f ? 1.0f : options->exposureScale;
        captured.highDynamicRange = options->colorBuffersHDR != 0;
        captured.useAutoExposure = options->structVersion >= 3u && options->useAutoExposure != 0;
        if (isVulkanStreamlinePath()) {
            apusr::host_discovery::eventf(
                "streamline", "vulkan.dlss.options",
                "viewport=%u mode=%u output=%ux%u hdr=%u auto_exposure=%u pre_exposure=%.6g exposure_scale=%.6g",
                viewport->value, static_cast<unsigned>(captured.mode), captured.outputWidth, captured.outputHeight,
                captured.highDynamicRange ? 1u : 0u, captured.useAutoExposure ? 1u : 0u,
                captured.preExposure, captured.exposureScale);
        }
        std::lock_guard<std::mutex> lock(g_streamlineStateMutex);
        g_optionsByViewport[viewport->value] = captured;
        if (viewport->value != 0u) {
            g_optionsByViewport[0u] = captured;
        }
    }
    return kResultOk;
}


int __cdecl apusrSlDLSSGSetOptions(const ViewportHandle* viewport, const DLSSGOptions* options)
{
    if (!isVulkanStreamlinePath())
        return kResultFeatureNotSupported;
    if (!viewport || !options)
        return kResultInvalidParameter;
    if (options->structVersion < apusr::streamline::dlssg_abi::kStructVersion1)
        return kResultInvalidParameter;

    // Streamline extends DLSSGOptions by appending fields and bumping
    // structVersion. Capture only the tail that the caller actually owns.
    // Older Streamline callers may predate the MFG-era v3 queue field.
    const DLSSGOptions capturedOptions = apusr::streamline::dlssg_abi::captureOptions(*options);
    if (capturedOptions.mode != DLSSGMode::eOff && capturedOptions.mode != DLSSGMode::eOn &&
        capturedOptions.mode != DLSSGMode::eAuto && capturedOptions.mode != DLSSGMode::eDynamic)
        return kResultInvalidParameter;
    if (capturedOptions.mode == DLSSGMode::eDynamic ||
        (capturedOptions.mode != DLSSGMode::eOff && capturedOptions.numFramesToGenerate != 1u) ||
        (capturedOptions.queueParallelismMode != DLSSGQueueParallelismMode::eBlockPresentingClientQueue &&
         capturedOptions.queueParallelismMode != DLSSGQueueParallelismMode::eBlockNoClientQueues)) {
        return kResultFeatureNotSupported;
    }

    const bool previousEnabled = g_dlssgEnabled.load(std::memory_order_relaxed);
    const bool enabled =
        g_dlssgLoaded.load(std::memory_order_relaxed) && capturedOptions.mode != DLSSGMode::eOff;
    std::uint32_t previousMode = 0u;
    bool previouslyConfigured = false;
    {
        std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
        previousMode = static_cast<std::uint32_t>(g_dlssgControl.options.mode);
        previouslyConfigured = g_dlssgControl.configured;
        g_dlssgControl.configured = true;
        g_dlssgControl.viewport = viewport->value;
        g_dlssgControl.options = capturedOptions;
        if (!enabled) {
            destroyVulkanFramegenRuntimeLocked();
            clearPendingFramegenPresentsLocked();
        }
    }
    g_dlssgEnabled.store(enabled, std::memory_order_release);
    apusrVulkanHostSetFrameGenerationActive(enabled);
    if (const auto stream = dlssgProfileStream()) {
        const std::uint32_t nextMode = static_cast<std::uint32_t>(capturedOptions.mode);
        if (!previouslyConfigured || previousMode != nextMode) {
            apusr::profile::recordStateTransition({
                .stream = stream,
                .name = "streamline.dlssg.mode",
                .reason = previouslyConfigured ? "slDLSSGSetOptions" : "initial_options",
                .previousValue = previousMode,
                .nextValue = nextMode,
                .subject = viewport->value,
            });
        }
        if (previousEnabled != enabled) {
            apusr::profile::recordStateTransition({
                .stream = stream,
                .name = "streamline.dlssg.enabled",
                .reason = "options_applied",
                .previousValue = previousEnabled ? 1u : 0u,
                .nextValue = enabled ? 1u : 0u,
                .subject = viewport->value,
            });
        }
        apusr::profile::recordCounter({
            .stream = stream,
            .name = "streamline.dlssg.queue_parallelism",
            .value = static_cast<std::uint32_t>(capturedOptions.queueParallelismMode),
            .value2 = capturedOptions.numFramesToGenerate,
        });
    }
    if (enabled) {
        g_dlssgWaitingForPclLogged.store(false, std::memory_order_relaxed);
        g_dlssgFirstPclPresentLogged.store(false, std::memory_order_relaxed);
    }
    g_dlssgLastStatus.store(kDlssgStatusOk, std::memory_order_relaxed);
    g_dlssgStateLogSignature.store(~std::uint64_t{0}, std::memory_order_relaxed);

    // Keep WSI capability negotiation tied to feature load rather than current
    // mode. A game is allowed to enable FG after it creates its swapchain.
    apusrVulkanHostRequestFrameGeneration(g_dlssgLoaded.load(std::memory_order_relaxed));
    apusr::host_discovery::eventf(
        "streamline", "vulkan.dlssg.options",
        "viewport=%u caller_struct_version=%zu mode=%u enabled=%u frames=%u queue_parallelism=%u ui_recomposition=%u",
        viewport->value, options->structVersion, static_cast<unsigned>(capturedOptions.mode), enabled ? 1u : 0u,
        capturedOptions.numFramesToGenerate, static_cast<unsigned>(capturedOptions.queueParallelismMode),
        capturedOptions.enableUserInterfaceRecomposition != 0 ? 1u : 0u);
    if (capturedOptions.enableUserInterfaceRecomposition != 0) {
        apusr::host_discovery::event(
            "streamline", "vulkan.dlssg.ui_recomposition_pending",
            "generated_frame_currently_uses_full_present_color; separate_UI_recomposition_is_not_yet_enabled");
    }
    return kResultOk;
}

int __cdecl apusrSlDLSSGGetState(
    const ViewportHandle* viewport,
    DLSSGState* state,
    const DLSSGOptions*)
{
    if (!isVulkanStreamlinePath())
        return kResultFeatureNotSupported;
    if (!viewport || !state)
        return kResultInvalidParameter;
    if (state->structVersion < apusr::streamline::dlssg_abi::kStructVersion1)
        return kResultInvalidParameter;

    const bool enabled = g_dlssgEnabled.load(std::memory_order_acquire);
    const bool reflexLoaded = g_reflexLoaded.load(std::memory_order_relaxed);
    const std::uint32_t reflexMode = g_reflexMode.load(std::memory_order_relaxed);
    const bool reflexActive = reflexLoaded && reflexMode != APUSR_VK_HOST_ANTI_LAG_OFF;

    std::uint32_t status = g_dlssgLastStatus.load(std::memory_order_relaxed);
    // eFailReflexNotDetectedAtRuntime is explicitly a running-FG failure. Do
    // not poison pre-enable capability discovery just because Reflex is still
    // off; older games use GetState(status == eOk) to enable their FG menu.
    if (enabled && !reflexActive)
        status |= kDlssgStatusReflexNotDetected;

    const std::uint32_t presented =
        g_dlssgPresentedSinceQuery.exchange(0u, std::memory_order_acq_rel);
    apusr::streamline::dlssg_abi::StateValues values{};
    values.status = status;
    values.numFramesActuallyPresented = presented;
    values.numFramesToGenerateMax = 1u;
    std::uint32_t queueParallelismMode = static_cast<std::uint32_t>(DLSSGQueueParallelismMode::eBlockPresentingClientQueue);
    std::uint64_t completedInputFenceValue = 0u;
    {
        std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
        if (g_dlssgControl.configured) {
            queueParallelismMode = static_cast<std::uint32_t>(g_dlssgControl.options.queueParallelismMode);
            if (g_dlssgControl.options.queueParallelismMode == DLSSGQueueParallelismMode::eBlockNoClientQueues) {
                values.inputsProcessingCompletionFence = g_vulkanFramegenRuntime.inputsProcessingCompletionFence;
                values.lastPresentInputsProcessingCompletionFenceValue =
                    g_vulkanFramegenRuntime.lastPresentInputsProcessingCompletionFenceValue;
                completedInputFenceValue =
                    apusrFramegenVulkanCompletedInputValue(g_vulkanFramegenRuntime.presentSubmit);
            }
        }
    }
    apusr::streamline::dlssg_abi::writeState(*state, values);

    const bool inputFenceAvailable = values.inputsProcessingCompletionFence != nullptr;
    const bool inputFenceSignaled = values.lastPresentInputsProcessingCompletionFenceValue != 0u;
    const bool inputFenceCaughtUp = inputFenceSignaled &&
        completedInputFenceValue >= values.lastPresentInputsProcessingCompletionFenceValue;
    const std::uint64_t signature =
        (static_cast<std::uint64_t>(state->structVersion & 0xffffu) << 48u) |
        (static_cast<std::uint64_t>(status) << 16u) |
        (static_cast<std::uint64_t>(queueParallelismMode & 0xffu) << 8u) |
        (static_cast<std::uint64_t>(inputFenceCaughtUp ? 1u : 0u) << 5u) |
        (static_cast<std::uint64_t>(inputFenceAvailable ? 1u : 0u) << 4u) |
        (static_cast<std::uint64_t>(inputFenceSignaled ? 1u : 0u) << 3u) |
        (static_cast<std::uint64_t>(enabled ? 1u : 0u) << 2u) |
        (static_cast<std::uint64_t>(reflexLoaded ? 1u : 0u) << 1u) |
        static_cast<std::uint64_t>(reflexActive ? 1u : 0u);
    if (g_dlssgStateLogSignature.exchange(signature, std::memory_order_relaxed) != signature) {
        apusr::host_discovery::eventf(
            "streamline", "vulkan.dlssg.state",
            "viewport=%u caller_struct_version=%zu status=0x%x enabled=%u loaded=%u reflex_loaded=%u reflex_mode=%u queue_parallelism=%u input_fence=%p input_fence_value=%llu input_fence_completed_value=%llu input_fence_caught_up=%u mfg_tail_written=%u presented_since_query=%u",
            viewport->value, state->structVersion, status, enabled ? 1u : 0u,
            g_dlssgLoaded.load(std::memory_order_relaxed) ? 1u : 0u,
            reflexLoaded ? 1u : 0u, reflexMode, queueParallelismMode,
            values.inputsProcessingCompletionFence,
            static_cast<unsigned long long>(values.lastPresentInputsProcessingCompletionFenceValue),
            static_cast<unsigned long long>(completedInputFenceValue), inputFenceCaughtUp ? 1u : 0u,
            state->structVersion >= apusr::streamline::dlssg_abi::kStructVersion2 ? 1u : 0u,
            presented);
    }
    return kResultOk;
}

} // namespace

#if defined(APUSR_STREAMLINE_ATTACH_MODE)
void apusrDetachFromLoadedStreamlineAfterShutdown() noexcept;
#endif

extern "C" HRESULT WINAPI apusr_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    installIdentityHooksForCurrentApi();
    if (auto fn = originalFn<CreateDXGIFactoryFn>("CreateDXGIFactory")) {
        const HRESULT hr = fn(riid, ppFactory);
        if (SUCCEEDED(hr)) {
            const HRESULT wrapHr = wrapFactoryResult(riid, ppFactory);
            return FAILED(wrapHr) ? wrapHr : hr;
        }
        return hr;
    }
    return E_FAIL;
}

extern "C" HRESULT WINAPI apusr_CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    installIdentityHooksForCurrentApi();
    if (auto fn = originalFn<CreateDXGIFactory1Fn>("CreateDXGIFactory1")) {
        const HRESULT hr = fn(riid, ppFactory);
        if (SUCCEEDED(hr)) {
            const HRESULT wrapHr = wrapFactoryResult(riid, ppFactory);
            return FAILED(wrapHr) ? wrapHr : hr;
        }
        return hr;
    }
    return E_FAIL;
}

extern "C" HRESULT WINAPI apusr_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    installIdentityHooksForCurrentApi();
    if (auto fn = originalFn<CreateDXGIFactory2Fn>("CreateDXGIFactory2")) {
        const HRESULT hr = fn(flags, riid, ppFactory);
        if (SUCCEEDED(hr)) {
            const HRESULT wrapHr = wrapFactoryResult(riid, ppFactory);
            return FAILED(wrapHr) ? wrapHr : hr;
        }
        return hr;
    }
    return E_FAIL;
}

#if !defined(APUSR_STREAMLINE_ATTACH_MODE)
APUSR_SL_EXPORT HRESULT WINAPI D3D11CreateDevice(void* adapter,
                                                                  int driverType,
                                                                  HMODULE software,
                                                                  UINT flags,
                                                                  const void* featureLevels,
                                                                  UINT featureLevelsCount,
                                                                  UINT sdkVersion,
                                                                  void** device,
                                                                  void* featureLevel,
                                                                  void** immediateContext)
{
    if (auto fn = originalFn<D3D11CreateDeviceFn>("D3D11CreateDevice")) {
        const UnwrappedAdapter unwrapped = unwrapAdapter(adapter);
        const HRESULT hr = fn(unwrapped.ptr, driverType, software, flags, featureLevels, featureLevelsCount, sdkVersion, device, featureLevel, immediateContext);
        if (unwrapped.release && unwrapped.ptr != nullptr) {
            reinterpret_cast<IUnknown*>(unwrapped.ptr)->Release();
        }
        return hr;
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D11CreateDeviceAndSwapChain(void* adapter,
                                                                              int driverType,
                                                                              HMODULE software,
                                                                              UINT flags,
                                                                              const void* featureLevels,
                                                                              UINT featureLevelsCount,
                                                                              UINT sdkVersion,
                                                                              const void* swapChainDesc,
                                                                              void** swapChain,
                                                                              void** device,
                                                                              void* featureLevel,
                                                                              void** immediateContext)
{
    if (auto fn = originalFn<D3D11CreateDeviceAndSwapChainFn>("D3D11CreateDeviceAndSwapChain")) {
        const UnwrappedAdapter unwrapped = unwrapAdapter(adapter);
        const HRESULT hr = fn(unwrapped.ptr, driverType, software, flags, featureLevels, featureLevelsCount, sdkVersion, swapChainDesc, swapChain, device, featureLevel, immediateContext);
        if (unwrapped.release && unwrapped.ptr != nullptr) {
            reinterpret_cast<IUnknown*>(unwrapped.ptr)->Release();
        }
        return hr;
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D12CreateDevice(void* adapter, int minimumFeatureLevel, REFIID riid, void** device)
{
    if (auto fn = originalFn<D3D12CreateDeviceFn>("D3D12CreateDevice")) {
        const UnwrappedAdapter unwrapped = unwrapAdapter(adapter);
        const HRESULT hr = fn(unwrapped.ptr, minimumFeatureLevel, riid, device);
        if (unwrapped.release && unwrapped.ptr != nullptr) {
            reinterpret_cast<IUnknown*>(unwrapped.ptr)->Release();
        }
        return hr;
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void** debugInterface)
{
    if (auto fn = originalFn<D3D12GetDebugInterfaceFn>("D3D12GetDebugInterface")) {
        return fn(riid, debugInterface);
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void** outInterface)
{
    if (auto fn = originalFn<D3D12GetInterfaceFn>("D3D12GetInterface")) {
        return fn(clsid, riid, outInterface);
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D12EnableExperimentalFeatures(UINT numFeatures,
                                                                                const IID* featureIIDs,
                                                                                void* configurationStructs,
                                                                                UINT* configurationStructSizes)
{
    if (auto fn = originalFn<D3D12EnableExperimentalFeaturesFn>("D3D12EnableExperimentalFeatures")) {
        return fn(numFeatures, featureIIDs, configurationStructs, configurationStructSizes);
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D12CreateRootSignatureDeserializer(const void* srcData,
                                                                                     SIZE_T srcDataSizeInBytes,
                                                                                     REFIID riid,
                                                                                     void** deserializer)
{
    if (auto fn = originalFn<D3D12CreateRootSignatureDeserializerFn>("D3D12CreateRootSignatureDeserializer")) {
        return fn(srcData, srcDataSizeInBytes, riid, deserializer);
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(const void* srcData,
                                                                                              SIZE_T srcDataSizeInBytes,
                                                                                              REFIID riid,
                                                                                              void** deserializer)
{
    if (auto fn = originalFn<D3D12CreateVersionedRootSignatureDeserializerFn>("D3D12CreateVersionedRootSignatureDeserializer")) {
        return fn(srcData, srcDataSizeInBytes, riid, deserializer);
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D12SerializeRootSignature(const void* rootSignature,
                                                                            int version,
                                                                            void** blob,
                                                                            void** errorBlob)
{
    if (auto fn = originalFn<D3D12SerializeRootSignatureFn>("D3D12SerializeRootSignature")) {
        return fn(rootSignature, version, blob, errorBlob);
    }
    return E_FAIL;
}

APUSR_SL_EXPORT HRESULT WINAPI D3D12SerializeVersionedRootSignature(const void* rootSignature,
                                                                                     void** blob,
                                                                                     void** errorBlob)
{
    if (auto fn = originalFn<D3D12SerializeVersionedRootSignatureFn>("D3D12SerializeVersionedRootSignature")) {
        return fn(rootSignature, blob, errorBlob);
    }
    return E_FAIL;
}

#endif

extern "C" HRESULT WINAPI apusr_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debugInterface)
{
    if (auto fn = originalFn<DXGIGetDebugInterface1Fn>("DXGIGetDebugInterface1")) {
        return fn(flags, riid, debugInterface);
    }
    return E_FAIL;
}

APUSR_SL_EXPORT int __cdecl slInit(const void* preferences, std::uint64_t sdkVersion)
{
    g_slInitSeen.store(true, std::memory_order_release);
    // Streamline 1.x exported several core APIs with bool return semantics.
    // Newer Streamline passes kSDKVersion, whose low 16 bits are 0xfedc.
    constexpr std::uint64_t kSlSdkVersionMagic = 0xfedcull;
    const bool legacySlBoolApi = (sdkVersion & 0xffffull) != kSlSdkVersionMagic;
    g_legacySlBoolApi.store(legacySlBoolApi, std::memory_order_relaxed);
    g_slRenderApi.store(kSlRenderApiUnknown, std::memory_order_relaxed);
    g_reflexLoaded.store(false, std::memory_order_relaxed);
    g_reflexMode.store(APUSR_VK_HOST_ANTI_LAG_OFF, std::memory_order_relaxed);
    g_reflexMaxFps.store(0u, std::memory_order_relaxed);
    g_dlssgLoaded.store(false, std::memory_order_relaxed);
    g_dlssgEnabled.store(false, std::memory_order_relaxed);
    g_dlssgPresentedSinceQuery.store(0u, std::memory_order_relaxed);
    g_dlssgLastStatus.store(kDlssgStatusOk, std::memory_order_relaxed);
    g_dlssgFirstGeneratedLogged.store(false, std::memory_order_relaxed);
    g_dlssgWaitingForPclLogged.store(false, std::memory_order_relaxed);
    g_dlssgFirstPclPresentLogged.store(false, std::memory_order_relaxed);
    g_pclMarkerFunctionLogged.store(false, std::memory_order_relaxed);
    g_pclUnresolvedFrameLogged.store(false, std::memory_order_relaxed);
    g_dlssgStateLogSignature.store(~std::uint64_t{0}, std::memory_order_relaxed);
    g_backingPclSetMarker.store(nullptr, std::memory_order_release);
    apusrVulkanHostSetQueuePresentInterceptor(nullptr);
    apusrVulkanHostSetSwapchainDestroyInterceptor(nullptr);
    if (!legacySlBoolApi) {
        g_hostSlMajor.store(static_cast<std::uint32_t>((sdkVersion >> 48u) & 0xffffu), std::memory_order_relaxed);
        g_hostSlMinor.store(static_cast<std::uint32_t>((sdkVersion >> 32u) & 0xffffu), std::memory_order_relaxed);
        g_hostSlBuild.store(static_cast<std::uint32_t>((sdkVersion >> 16u) & 0xffffu), std::memory_order_relaxed);
    }

    std::uint32_t requestedFeatures = 0;
    if (!legacySlBoolApi && preferences != nullptr) {
        const auto* prefs = static_cast<const SlPreferences*>(preferences);
        requestedFeatures = prefs->numFeaturesToLoad;
        if (prefs->renderAPI <= kSlRenderApiVulkan) {
            g_slRenderApi.store(prefs->renderAPI, std::memory_order_relaxed);
        }
    }

    const std::uint32_t renderApi = g_slRenderApi.load(std::memory_order_relaxed);
    bool reflexRequested = false;
    bool vulkanFramegenRequested = false;
    if (!legacySlBoolApi && preferences != nullptr) {
        const auto& prefs = *static_cast<const SlPreferences*>(preferences);
        if (renderApi == kSlRenderApiVulkan || renderApi == kSlRenderApiD3d12) {
            reflexRequested = featureRequested(prefs, kFeatureReflex);
        }
        if (renderApi == kSlRenderApiVulkan) {
            vulkanFramegenRequested = featureRequested(prefs, kFeatureDLSS_G);
        }
    }
    g_reflexLoaded.store(reflexRequested, std::memory_order_relaxed);
    g_dlssgLoaded.store(vulkanFramegenRequested, std::memory_order_relaxed);
    apusrVulkanHostRequestAntiLag(renderApi == kSlRenderApiVulkan && reflexRequested);
    apusrVulkanHostRequestFrameGeneration(vulkanFramegenRequested);
    {
        std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
        g_dlssgControl = {};
        clearPendingFramegenPresentsLocked();
    }

    apusr::host_discovery::loggingReady("streamline");
    apusr::host_discovery::moduleLoaded("streamline", reinterpret_cast<HMODULE>(&__ImageBase));
    apusr::host_discovery::eventf(
        "streamline", "slInit.begin",
        "sdk=0x%llx legacy=%u render_api=%u requested_features=%u",
        static_cast<unsigned long long>(sdkVersion), legacySlBoolApi ? 1u : 0u, renderApi, requestedFeatures);

#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    // Keep stock Streamline as the version-matched DLSS-SR translator, but do
    // not initialize its NVIDIA Reflex/DLSS-G plugins beside APUSR's Vulkan
    // implementations.  A legacy APUSR interposer used to apply this ownership
    // split implicitly; the stock-preserving attach path must make it explicit.
    // Otherwise both implementations can install WSI/presentation state in one
    // process, which is invalid and can crash native Vulkan hosts.
    apusrIdentitySpoofSetStreamlineRenderApi(renderApi);
    if (renderApi == kSlRenderApiVulkan && !apusrInstallVulkanHostCapture()) {
        apusr::host_discovery::event(
            "vulkan", "host_capture.unavailable",
            "attach_mode_preserved_stock_slInit_but_APUSR_Vulkan_capture_is_unavailable");
    }
    if (auto fn = originalFn<SlInitFn>("slInit")) {
        const void* stockPreferences = preferences;
        SlPreferences filteredPreferences{};
        std::vector<std::uint32_t> stockFeatures;
        std::uint32_t excludedApusrOwned = 0u;
        std::uint32_t excludedUnsupported = 0u;
        if (!legacySlBoolApi && preferences != nullptr && renderApi == kSlRenderApiVulkan) {
            const auto& prefs = *static_cast<const SlPreferences*>(preferences);
            stockFeatures.reserve(prefs.numFeaturesToLoad);
            if (prefs.featuresToLoad != nullptr) {
                for (std::uint32_t i = 0; i < prefs.numFeaturesToLoad; ++i) {
                    const std::uint32_t feature = prefs.featuresToLoad[i];
                    if (apusr::streamline::feature_ownership::excludeFromStockVulkanInitialization(feature)) {
                        if (feature == kFeatureDLSS_RR) {
                            ++excludedUnsupported;
                        } else {
                            ++excludedApusrOwned;
                        }
                        continue;
                    }
                    stockFeatures.push_back(feature);
                }
            }
            filteredPreferences = prefs;
            filteredPreferences.featuresToLoad = stockFeatures.empty() ? nullptr : stockFeatures.data();
            filteredPreferences.numFeaturesToLoad = static_cast<std::uint32_t>(stockFeatures.size());
            stockPreferences = &filteredPreferences;
            apusr::host_discovery::eventf(
                "streamline", "vulkan.feature_ownership",
                "requested=%u stock_kept=%u apusr_owned_excluded=%u unsupported_excluded=%u "
                "dlss_sr_owner=stock reflex_owner=apusr dlssg_owner=apusr pcl_owner=stock_optional",
                prefs.numFeaturesToLoad, filteredPreferences.numFeaturesToLoad,
                excludedApusrOwned, excludedUnsupported);
        }
        const int result = fn(stockPreferences, sdkVersion);
        apusr::host_discovery::eventf(
            "streamline", "attach.slInit.result",
            "result=%d render_api=%u stock_dlss_sr_preserved=1 vulkan_feature_ownership_split=%u",
            result, renderApi, renderApi == kSlRenderApiVulkan ? 1u : 0u);
        return result;
    }
#else
    apusrIdentitySpoofSetStreamlineRenderApi(renderApi);
    // Install identity/plugin hooks only after the render API is known. Vulkan
    // owns DLSS in this proxy and does not need NVIDIA adapter/plugin spoofing.
    installIdentityHooksForCurrentApi();
    if (renderApi == kSlRenderApiVulkan && !apusrInstallVulkanHostCapture()) {
        apusr::host_discovery::event(
            "vulkan", "host_capture.unavailable",
            "system_loader_capture_and_native_dot4_device_contract_could_not_be_installed");
        return kResultFeatureFailedToLoad;
    }
    if (auto fn = originalFn<SlInitFn>("slInit")) {
        if (!legacySlBoolApi && preferences != nullptr && renderApi == kSlRenderApiVulkan) {
            const auto* prefs = static_cast<const SlPreferences*>(preferences);
            std::vector<std::uint32_t> filteredFeatures;
            filteredFeatures.reserve(prefs->numFeaturesToLoad);
            if (prefs->featuresToLoad != nullptr) {
                for (std::uint32_t i = 0; i < prefs->numFeaturesToLoad; ++i) {
                    const std::uint32_t feature = prefs->featuresToLoad[i];
                    // APUSR owns Vulkan DLSS SR, Reflex and DLSS-G. PCL marker
                    // timing is also APUSR-owned because older Streamline hosts
                    // some integrations can expose slPCLSetMarker through Reflex rather
                    // than a standalone sl.pcl plugin. A backing PCL function is
                    // still forwarded when a richer package supplies one.
                    const bool apusrOwned = feature == kFeatureDLSS || feature == kFeatureReflex ||
                        feature == kFeatureDLSS_G;
                    if (!apusrOwned && !apusrStreamlineFeatureExplicitlyUnsupported(feature)) {
                        filteredFeatures.push_back(feature);
                    }
                }
            }

            SlPreferences filtered = *prefs;
            filtered.featuresToLoad = filteredFeatures.empty() ? nullptr : filteredFeatures.data();
            filtered.numFeaturesToLoad = static_cast<std::uint32_t>(filteredFeatures.size());
            apusr::host_discovery::eventf(
                "streamline", "vulkan.feature_filter",
                "requested=%u backing_kept=%u removed_or_apusr_owned=%u dlss_owner=apusr reflex_owner=apusr_vk_amd_anti_lag dlssg_owner=apusr_vk_fi_of pcl_marker_owner=apusr_backing_optional",
                prefs->numFeaturesToLoad, filtered.numFeaturesToLoad,
                prefs->numFeaturesToLoad - filtered.numFeaturesToLoad);
            const int result = fn(&filtered, sdkVersion);
            apusr::host_discovery::eventf(
                "streamline", "slInit.result",
                "result=%d render_api=%u kept_features=%u", result, renderApi, filtered.numFeaturesToLoad);
            return result;
        }

        const int result = fn(preferences, sdkVersion);
        apusr::host_discovery::eventf(
            "streamline", "slInit.result", "result=%d render_api=%u", result, renderApi);
        return result;
    }

#endif

    const int result = legacySlBoolApi ? 1 : kResultOk;
    apusr::host_discovery::eventf(
        "streamline", "slInit.result", "result=%d backing_interposer=missing", result);
    return result;
}

APUSR_SL_EXPORT int __cdecl slShutdown()
{
    const bool vulkan = isVulkanStreamlinePath();
    {
        std::lock_guard<std::mutex> lock(g_streamlineNgxMutex);
        if (g_streamlineNgxFeature != nullptr) {
            NgxReleaseFeatureFn release = vulkan
                ? ngxFnCached(g_streamlineNgxVulkanReleaseFeatureFn, "NVSDK_NGX_VULKAN_ReleaseFeature")
                : ngxFnCached(g_streamlineNgxReleaseFeatureFn, "NVSDK_NGX_D3D12_ReleaseFeature");
            if (release != nullptr) {
                release(g_streamlineNgxFeature);
            }
            g_streamlineNgxFeature = nullptr;
        }
        if (g_streamlineNgxEvalParams != nullptr) {
            NgxDestroyParametersFn destroy = vulkan
                ? ngxFnCached(g_streamlineNgxVulkanDestroyParametersFn, "NVSDK_NGX_VULKAN_DestroyParameters")
                : ngxFnCached(g_streamlineNgxDestroyParametersFn, "NVSDK_NGX_D3D12_DestroyParameters");
            if (destroy != nullptr) {
                destroy(g_streamlineNgxEvalParams);
            }
            g_streamlineNgxEvalParams = nullptr;
        }
        g_streamlineNgxRenderWidth = 0;
        g_streamlineNgxRenderHeight = 0;
        g_streamlineNgxOutputWidth = 0;
        g_streamlineNgxOutputHeight = 0;
    }
    {
        std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
        destroyVulkanFramegenRuntimeLocked();
        clearPendingFramegenPresentsLocked();
        g_dlssgControl = {};
    }
    apusrVulkanHostRequestFrameGeneration(false);
    g_dlssgEnabled.store(false, std::memory_order_relaxed);
    g_dlssgLoaded.store(false, std::memory_order_relaxed);

    const int result = [&]() {
        if (auto fn = originalFn<SlShutdownFn>("slShutdown")) {
            return fn();
        }
        return kResultOk;
    }();
    {
        std::lock_guard<std::mutex> lock(g_d3d12AntiLagMutex);
        deinitializeD3d12AntiLagLocked();
        g_d3d12AntiLagProbed.store(false, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(g_streamlineStateMutex);
        g_optionsByViewport.clear();
        g_tagsByFrameViewport.clear();
        g_legacyTagsByViewport.clear();
        g_constantsByFrameViewport.clear();
        g_constantsByViewport.clear();
    }
    apusrVulkanHostSetQueuePresentInterceptor(nullptr);
    apusrVulkanHostSetSwapchainDestroyInterceptor(nullptr);
    apusrVulkanHostRequestAntiLag(false);
    g_reflexLoaded.store(false, std::memory_order_relaxed);
    g_reflexMode.store(APUSR_VK_HOST_ANTI_LAG_OFF, std::memory_order_relaxed);
    g_reflexMaxFps.store(0u, std::memory_order_relaxed);
    g_dlssgPresentedSinceQuery.store(0u, std::memory_order_relaxed);
    g_dlssgLastStatus.store(kDlssgStatusOk, std::memory_order_relaxed);
    g_dlssgFirstGeneratedLogged.store(false, std::memory_order_relaxed);
    g_dlssgWaitingForPclLogged.store(false, std::memory_order_relaxed);
    g_dlssgFirstPclPresentLogged.store(false, std::memory_order_relaxed);
    g_pclMarkerFunctionLogged.store(false, std::memory_order_relaxed);
    g_pclUnresolvedFrameLogged.store(false, std::memory_order_relaxed);
    g_dlssgStateLogSignature.store(~std::uint64_t{0}, std::memory_order_relaxed);
    g_backingPclSetMarker.store(nullptr, std::memory_order_release);
    clearVulkanStreamlineState();
    g_slInitSeen.store(false, std::memory_order_release);
    g_slRenderApi.store(kSlRenderApiUnknown, std::memory_order_relaxed);
    apusrIdentitySpoofSetStreamlineRenderApi(kSlRenderApiUnknown);
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    // Restore the stock interposer entry points before the host is free to
    // unload/reload Streamline after shutdown. This keeps Detours from turning
    // a process-lifetime compatibility module into a stale-module dependency.
    apusrDetachFromLoadedStreamlineAfterShutdown();
#endif
    return result;
}

APUSR_SL_EXPORT int __cdecl slGetNewFrameToken(void** token, const std::uint32_t* frameIndex)
{
    if (auto fn = originalFn<SlGetNewFrameTokenFn>("slGetNewFrameToken"))
        return fn(token, frameIndex);
    if (token != nullptr)
        *token = nullptr;
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slSetConstants(const void* values, const void* frame, const void* viewport)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (!isVulkanStreamlinePath()) {
        if (auto fn = originalFn<SlSetConstantsFn>("slSetConstants")) {
            return fn(values, frame, viewport);
        }
        return kResultFeatureMissing;
    }
#endif
    if (values != nullptr) {
        captureConstantsForKey(
            StreamlineViewportKey{.frame = frame, .viewport = viewportValue(viewport)},
            *static_cast<const SlConstants*>(values));
    }
    if (auto fn = originalFn<SlSetConstantsFn>("slSetConstants")) {
        return fn(values, frame, viewport);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slSetTag(const void* viewport, const void* tags, std::uint32_t numTags, void* cmdBuffer)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (!isVulkanStreamlinePath()) {
        if (auto fn = originalFn<SlSetTagFn>("slSetTag")) {
            return fn(viewport, tags, numTags, cmdBuffer);
        }
        return kResultFeatureMissing;
    }
#endif
    captureTagsForKey(StreamlineViewportKey{.frame = nullptr, .viewport = viewportValue(viewport)}, tags, numTags);
    if (auto fn = originalFn<SlSetTagFn>("slSetTag")) {
        return fn(viewport, tags, numTags, cmdBuffer);
    }
    return kResultOk;
}

extern "C" int __cdecl slSetTagForFrame(const void* frame, const void* viewport, const void* tags, std::uint32_t numTags, void* cmdBuffer)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (!isVulkanStreamlinePath()) {
        if (auto fn = originalFn<SlSetTagForFrameFn>("slSetTagForFrame")) {
            return fn(frame, viewport, tags, numTags, cmdBuffer);
        }
        return kResultFeatureMissing;
    }
#endif
    captureTagsForKey(StreamlineViewportKey{.frame = frame, .viewport = viewportValue(viewport)}, tags, numTags);
    if (auto fn = originalFn<SlSetTagForFrameFn>("slSetTagForFrame")) {
        return fn(frame, viewport, tags, numTags, cmdBuffer);
    }
    return kResultOk;
}

APUSR_SL_EXPORT int __cdecl slSetVulkanInfo(const void* info);

bool buildAutoStreamlineVulkanInfo(void* device, void* physicalDevice, SlVulkanInfo& info)
{
    if (device == nullptr || physicalDevice == nullptr) {
        return false;
    }

    ApusrVulkanHostDeviceInfo deviceInfo{};
    if (!apusrVulkanHostGetDeviceInfo(device, &deviceInfo) || deviceInfo.instance == nullptr) {
        return false;
    }

    ApusrVulkanHostQueueInfo graphics{};
    ApusrVulkanHostQueueInfo compute{};
    bool haveGraphics = apusrVulkanHostResolveQueue(device, APUSR_VK_HOST_QUEUE_GRAPHICS, &graphics);
    bool haveCompute = apusrVulkanHostResolveQueue(device, APUSR_VK_HOST_QUEUE_COMPUTE, &compute);
    if (!haveGraphics && !haveCompute) {
        return false;
    }
    if (!haveGraphics) {
        graphics = compute;
    }
    if (!haveCompute) {
        compute = graphics;
    }

    info = {};
    info.next = nullptr;
    info.structType = kSlVulkanInfoStructType;
    info.structVersion = kSlVulkanInfoStructVersion;
    info.device = device;
    info.instance = deviceInfo.instance;
    info.physicalDevice = deviceInfo.physicalDevice != nullptr ? deviceInfo.physicalDevice : physicalDevice;
    info.computeQueueIndex = compute.queueIndex;
    info.computeQueueFamily = compute.familyIndex;
    info.graphicsQueueIndex = graphics.queueIndex;
    info.graphicsQueueFamily = graphics.familyIndex;
    info.opticalFlowQueueIndex = compute.queueIndex;
    info.opticalFlowQueueFamily = compute.familyIndex;
    info.useNativeOpticalFlowMode = false;
    info.computeQueueCreateFlags = compute.queueCreateFlags;
    info.graphicsQueueCreateFlags = graphics.queueCreateFlags;
    info.opticalFlowQueueCreateFlags = compute.queueCreateFlags;
    return true;
}

std::int32_t WINAPI apusrVkCreateDevice(
    void* physicalDevice,
    const void* createInfo,
    const void* allocator,
    void** device)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (!isVulkanStreamlinePath()) {
        if (auto fn = originalFn<VkCreateDeviceFn>("vkCreateDevice")) {
            return fn(physicalDevice, createInfo, allocator, device);
        }
        return kVkErrorInitializationFailed;
    }

    // Stock Streamline's Vulkan proxy derives VkDevice requirements from the
    // NVIDIA implementations it loaded. sl.dlss 2.12 unconditionally contributes
    // VK_NVX_binary_import/VK_NVX_image_view_handle and sl.reflex contributes
    // VK_NV_low_latency. On AMD the proxy rejects those extensions *before* it
    // reaches the real loader, so APUSR's backend-level vkCreateDevice capture can
    // never negotiate the replacement implementation's actual requirements.
    //
    // APUSR therefore switches only the Vulkan device-creation boundary to the
    // documented manual-hooking model: preserve the application's native
    // VkDeviceCreateInfo, let apusr_vulkan_host_capture negotiate APUSR's real
    // device features/extensions, then publish the captured device/queue topology
    // back to stock Streamline through slSetVulkanInfo. Stock sl.dlss remains the
    // semantic SR translator after plugin initialization.
    auto systemCreateDevice = vulkanSystemFn<VkCreateDeviceFn>("vkCreateDevice");
    if (systemCreateDevice == nullptr) {
        apusr::host_discovery::event(
            "vulkan", "streamline.device_proxy_bypass_failed", "reason=system_vkCreateDevice_missing");
        if (device != nullptr) {
            *device = nullptr;
        }
        return kVkErrorInitializationFailed;
    }

    apusr::host_discovery::eventf(
        "vulkan", "streamline.device_proxy_bypass",
        "physical_device=%p reason=replace_nvidia_plugin_device_contract_with_apusr_host_capture",
        physicalDevice);
    const std::int32_t result = systemCreateDevice(physicalDevice, createInfo, allocator, device);
    if (result != kVkSuccess || device == nullptr || *device == nullptr) {
        apusr::host_discovery::eventf(
            "vulkan", "streamline.device_proxy_bypass_result", "result=%d device=%p", result,
            device != nullptr ? *device : nullptr);
        return result;
    }

    SlVulkanInfo info{};
    if (!buildAutoStreamlineVulkanInfo(*device, physicalDevice, info)) {
        apusr::host_discovery::eventf(
            "vulkan", "streamline.device_publish_failed",
            "device=%p reason=host_capture_device_or_queue_context_missing", *device);
        return result;
    }

    const int streamlineResult = slSetVulkanInfo(&info);
    apusr::host_discovery::eventf(
        "vulkan", "streamline.device_published",
        "device=%p instance=%p physical_device=%p graphics=%u:%u compute=%u:%u sl_result=%d",
        info.device, info.instance, info.physicalDevice,
        info.graphicsQueueFamily, info.graphicsQueueIndex,
        info.computeQueueFamily, info.computeQueueIndex,
        streamlineResult);
    // Match stock vkCreateDevice behavior: plugin initialization failure does not
    // invalidate an otherwise successfully created application VkDevice. The host
    // may continue Vulkan without APUSR and the discovery log retains the failure.
    return result;
#else
    if (auto fn = vulkanSystemFn<VkCreateDeviceFn>("vkCreateDevice")) {
        return fn(physicalDevice, createInfo, allocator, device);
    }
    return kVkErrorInitializationFailed;
#endif
}

APUSR_SL_EXPORT int __cdecl slSetVulkanInfo(const void* info)
{
    g_slRenderApi.store(kSlRenderApiVulkan, std::memory_order_relaxed);
    apusrIdentitySpoofSetStreamlineRenderApi(kSlRenderApiVulkan);
    captureVulkanInfo(info);
    if (info != nullptr) {
        const auto& vulkanInfo = *static_cast<const SlVulkanInfo*>(info);
        if (vulkanInfo.device != nullptr && apusrVulkanHostDeviceNegotiationFlags(vulkanInfo.device) == 0u) {
            apusr::host_discovery::eventf(
                "vulkan", "host_capture.device_missing",
                "device=%p reason=VkDevice_was_not_created_through_APUSR_Vulkan_host_capture",
                vulkanInfo.device);
            apusrVulkanHostSetQueuePresentInterceptor(nullptr);
            apusrVulkanHostSetSwapchainDestroyInterceptor(nullptr);
            return kResultFeatureFailedToLoad;
        }
    }
    if (auto fn = originalFn<SlSetVulkanInfoFn>("slSetVulkanInfo")) {
        const int result = fn(info);
        if (result == kResultOk) {
            // Register only after the backing Streamline has built its Vulkan
            // dispatch table. The host-capture present trampoline then nests
            // the backing hooks exactly once and falls through to the real
            // loader on re-entry.
            g_lastObservedPresentSwapchain.store(nullptr, std::memory_order_relaxed);
            apusrVulkanHostSetQueuePresentInterceptor(
                reinterpret_cast<void*>(&apusrOwnedVkQueuePresentKHR));
            apusrVulkanHostSetSwapchainDestroyInterceptor(
                reinterpret_cast<void*>(&apusrOwnedVkDestroySwapchainKHR));
        } else {
            // Never retain a callback from an earlier successful Vulkan setup
            // after the backing interposer rejects a replacement device.
            apusrVulkanHostSetQueuePresentInterceptor(nullptr);
            apusrVulkanHostSetSwapchainDestroyInterceptor(nullptr);
        }
        apusr::host_discovery::eventf("streamline", "slSetVulkanInfo.result", "result=%d", result);
        return result;
    }
    apusrVulkanHostSetQueuePresentInterceptor(nullptr);
    apusrVulkanHostSetSwapchainDestroyInterceptor(nullptr);
    apusr::host_discovery::event("streamline", "slSetVulkanInfo.result", "result=missing_backing_export");
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slUpgradeInterface(void** baseInterface)
{
    if (auto fn = originalFn<SlUpgradeInterfaceFn>("slUpgradeInterface")) {
        return fn(baseInterface);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slGetNativeInterface(void* proxyInterface, void** baseInterface)
{
    if (auto fn = originalFn<SlGetNativeInterfaceFn>("slGetNativeInterface")) {
        return fn(proxyInterface, baseInterface);
    }
    if (baseInterface != nullptr) {
        *baseInterface = proxyInterface;
    }
    return kResultOk;
}

APUSR_SL_EXPORT int __cdecl slSetD3DDevice(void* d3dDevice)
{
    apusr::host_discovery::eventf("streamline", "slSetD3DDevice.called", "device=%p", d3dDevice);

    std::uint32_t detectedApi = kSlRenderApiUnknown;
    if (d3dDevice != nullptr) {
        auto* unknown = static_cast<IUnknown*>(d3dDevice);
        ID3D12Device* d3d12 = nullptr;
        if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&d3d12))) && d3d12 != nullptr) {
            detectedApi = kSlRenderApiD3d12;
            d3d12->Release();
        } else {
            ID3D11Device* d3d11 = nullptr;
            if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&d3d11))) && d3d11 != nullptr) {
                detectedApi = kSlRenderApiD3d11;
                d3d11->Release();
            }
        }
    }
    if (detectedApi != kSlRenderApiUnknown) {
        g_slRenderApi.store(detectedApi, std::memory_order_relaxed);
        apusrIdentitySpoofSetStreamlineRenderApi(detectedApi);
    }
    g_d3dDevice = d3dDevice;

#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    // Stock Streamline still owns D3D device/proxy semantics. APUSR only unwraps
    // the native D3D12 device through stock slGetNativeInterface so its Reflex
    // translation can initialize AMD Anti-Lag 2 on the real driver device.
    if (detectedApi == kSlRenderApiD3d12) {
        bindD3d12AntiLagDevice(d3dDevice);
    } else {
        std::lock_guard<std::mutex> lock(g_d3d12AntiLagMutex);
        deinitializeD3d12AntiLagLocked();
        g_d3d12AntiLagProbed.store(false, std::memory_order_release);
    }
    if (auto fn = originalFn<SlSetD3DDeviceFn>("slSetD3DDevice")) {
        return fn(d3dDevice);
    }
    return kResultFeatureMissing;
#else
    installIdentityHooksForCurrentApi();
    HMODULE backing = loadOriginalInterposer();
    if (!g_proxyAbiLogged.exchange(true, std::memory_order_acq_rel)) {
        const StreamlineBackingVersion backingVersion = queryModuleVersion(backing);
        const bool backingAbiMatch = backingMatchesCompiledAbi(backing);
        const bool backingVersionExact = backingVersion.valid &&
            backingVersion.major == APUSR_SL_EXPECTED_VERSION_MAJOR &&
            backingVersion.minor == APUSR_SL_EXPECTED_VERSION_MINOR &&
            backingVersion.patch == APUSR_SL_EXPECTED_VERSION_PATCH &&
            backingVersion.build == APUSR_SL_EXPECTED_VERSION_BUILD;
        const bool backingHasTagForFrame =
            backing != nullptr && GetProcAddress(backing, "slSetTagForFrame") != nullptr;
        apusr::host_discovery::eventf(
            "streamline", "proxy.abi",
            "profile=%s stock_x64_ordinals=1 stock_range=1-%u private_range=%u-%u "
            "sl_init_seen=%u backing_interposer=%s backing_version=%u.%u.%u.%u "
            "backing_tag_for_frame=%u backing_abi_match=%u backing_version_exact=%u",
            APUSR_SL_ABI_PROFILE_NAME,
            static_cast<unsigned>(APUSR_SL_STOCK_MAX_ORDINAL),
            static_cast<unsigned>(APUSR_SL_PRIVATE_FIRST_ORDINAL),
            static_cast<unsigned>(APUSR_SL_PRIVATE_FIRST_ORDINAL + 2u),
            g_slInitSeen.load(std::memory_order_acquire) ? 1u : 0u,
            backing != nullptr ? "loaded" : "missing",
            static_cast<unsigned>(backingVersion.major),
            static_cast<unsigned>(backingVersion.minor),
            static_cast<unsigned>(backingVersion.patch),
            static_cast<unsigned>(backingVersion.build),
            backingHasTagForFrame ? 1u : 0u,
            backingAbiMatch ? 1u : 0u,
            backingVersionExact ? 1u : 0u);
    }
    if (backing != nullptr) {
        if (auto fn = reinterpret_cast<SlSetD3DDeviceFn>(GetProcAddress(backing, "slSetD3DDevice"))) {
            return fn(d3dDevice);
        }
    }
    return kResultOk;
#endif
}

APUSR_SL_EXPORT int __cdecl slIsFeatureSupported(std::uint32_t feature, const void* adapterInfo)
{
    apusr::host_discovery::eventf("streamline", "slIsFeatureSupported.called", "feature=%u adapter_info=%p", feature, adapterInfo);
    installIdentityHooksForCurrentApi();
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        if (auto fn = originalFn<SlIsFeatureSupportedFn>("slIsFeatureSupported")) {
            return fn(feature, adapterInfo);
        }
        return kResultFeatureMissing;
    }
#endif
    if (feature == kFeatureDLSS) {
        apusr::host_discovery::event("streamline", "dlss.supported", "result=ok");
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 1 : kResultOk;
    }
    if (feature == kFeatureReflex && isReflexStreamlinePath()) {
        if (isD3d12StreamlinePath() &&
            g_d3d12AntiLagProbed.load(std::memory_order_acquire) &&
            !g_d3d12AntiLagAvailable.load(std::memory_order_acquire)) {
            return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 0 : kResultAdapterNotSupported;
        }
        // An empty adapter asks only whether the feature is generally exposed
        // by this integration. With a Vulkan physical device, do not fabricate
        // support: require the real VK_AMD_anti_lag extension + feature bit.
        if (isVulkanStreamlinePath() && adapterInfo != nullptr) {
            const auto* adapter = static_cast<const SlAdapterInfo*>(adapterInfo);
            if (adapter->vkPhysicalDevice != nullptr &&
                !apusrVulkanHostAntiLagSupported(adapter->vkPhysicalDevice)) {
                return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 0 : kResultAdapterNotSupported;
            }
            // Streamline's documented "general support" query passes an empty
            // AdapterInfo object by reference, not an actual null pointer. A
            // LUID-only adapter cannot be mapped to a VkPhysicalDevice at this
            // layer yet; leave that case at general compatibility and let
            // ReflexState report native availability after device capture.
        }
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 1 : kResultOk;
    }
    if (feature == kFeatureDLSS_G && isVulkanStreamlinePath()) {
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 1 : kResultOk;
    }
    if (apusrStreamlineFeatureExplicitlyUnsupported(feature)) {
        apusr::host_discovery::eventf("streamline", "feature.unsupported_apusr", "feature=%u", feature);
        return kResultFeatureMissing;
    }
    if (auto fn = originalFn<SlIsFeatureSupportedFn>("slIsFeatureSupported")) {
        return fn(feature, adapterInfo);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slIsFeatureLoaded(std::uint32_t feature, bool* loaded)
{
    installIdentityHooksForCurrentApi();
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        if (auto fn = originalFn<SlIsFeatureLoadedFn>("slIsFeatureLoaded")) {
            return fn(feature, loaded);
        }
        return kResultFeatureMissing;
    }
#endif
    if (feature == kFeatureDLSS) {
        if (loaded != nullptr) {
            *loaded = g_dlssLoaded.load(std::memory_order_relaxed);
            return g_legacySlBoolApi.load(std::memory_order_relaxed) ? (*loaded ? 1 : 0) : kResultOk;
        }
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 1 : kResultOk;
    }
    if (feature == kFeatureReflex && isReflexStreamlinePath()) {
        const bool reflexLoaded = g_reflexLoaded.load(std::memory_order_relaxed);
        if (loaded != nullptr) {
            *loaded = reflexLoaded;
        }
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? (reflexLoaded ? 1 : 0) : kResultOk;
    }
    if (feature == kFeatureDLSS_G && isVulkanStreamlinePath()) {
        const bool framegenLoaded = g_dlssgLoaded.load(std::memory_order_relaxed);
        if (loaded != nullptr)
            *loaded = framegenLoaded;
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? (framegenLoaded ? 1 : 0) : kResultOk;
    }
    if (apusrStreamlineFeatureExplicitlyUnsupported(feature)) {
        if (loaded != nullptr) {
            *loaded = false;
        }
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 0 : kResultOk;
    }
    if (auto fn = originalFn<SlIsFeatureLoadedFn>("slIsFeatureLoaded")) {
        return fn(feature, loaded);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slSetFeatureLoaded(std::uint32_t feature, bool loaded)
{
    installIdentityHooksForCurrentApi();
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        if (auto fn = originalFn<SlSetFeatureLoadedFn>("slSetFeatureLoaded")) {
            return fn(feature, loaded);
        }
        return kResultFeatureMissing;
    }
#endif
    if (feature == kFeatureDLSS) {
        g_dlssLoaded.store(loaded, std::memory_order_relaxed);
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 1 : kResultOk;
    }
    if (feature == kFeatureReflex && isReflexStreamlinePath()) {
        const bool previousLoaded = g_reflexLoaded.load(std::memory_order_relaxed);
        g_reflexLoaded.store(loaded, std::memory_order_relaxed);
        if (isVulkanStreamlinePath()) {
            apusrVulkanHostRequestAntiLag(loaded);
        }
        if (!loaded) {
            g_reflexMode.store(APUSR_VK_HOST_ANTI_LAG_OFF, std::memory_order_relaxed);
            g_reflexMaxFps.store(0u, std::memory_order_relaxed);
            if (isD3d12StreamlinePath()) {
                std::lock_guard<std::mutex> lock(g_d3d12AntiLagMutex);
                (void)setD3d12AntiLagStateLocked(false, 0u);
                g_d3d12AntiLagLastInputFrame = nullptr;
            }
            // Vulkan intentionally does not issue a native OFF transition on
            // the existing logical device; see apusrSlReflexSetOptions.
        }
        if (previousLoaded != loaded) {
            if (const auto stream = reflexProfileStream()) {
                apusr::profile::recordStateTransition({
                    .stream = stream,
                    .name = "streamline.reflex.loaded",
                    .reason = "slSetFeatureLoaded",
                    .previousValue = previousLoaded ? 1u : 0u,
                    .nextValue = loaded ? 1u : 0u,
                });
            }
        }
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 1 : kResultOk;
    }
    if (feature == kFeatureDLSS_G && isVulkanStreamlinePath()) {
        const bool previousLoaded = g_dlssgLoaded.load(std::memory_order_relaxed);
        const bool previousEnabled = g_dlssgEnabled.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_vulkanFramegenMutex);
            g_dlssgLoaded.store(loaded, std::memory_order_relaxed);
            if (!loaded) {
                g_dlssgEnabled.store(false, std::memory_order_release);
                g_dlssgLastStatus.store(kDlssgStatusOk, std::memory_order_relaxed);
                g_dlssgStateLogSignature.store(~std::uint64_t{0}, std::memory_order_relaxed);
                destroyVulkanFramegenRuntimeLocked();
                clearPendingFramegenPresentsLocked();
            } else if (g_dlssgControl.configured && g_dlssgControl.options.mode != DLSSGMode::eOff) {
                g_dlssgEnabled.store(true, std::memory_order_release);
            }
        }
        if (!loaded) {
            apusrVulkanHostRequestLegacyOpticalFlowQueueRole(false);
        }
        apusrVulkanHostRequestFrameGeneration(loaded);
        if (const auto stream = dlssgProfileStream()) {
            if (previousLoaded != loaded) {
                apusr::profile::recordStateTransition({
                    .stream = stream,
                    .name = "streamline.dlssg.loaded",
                    .reason = "slSetFeatureLoaded",
                    .previousValue = previousLoaded ? 1u : 0u,
                    .nextValue = loaded ? 1u : 0u,
                });
            }
            const bool nextEnabled = g_dlssgEnabled.load(std::memory_order_relaxed);
            if (previousEnabled != nextEnabled) {
                apusr::profile::recordStateTransition({
                    .stream = stream,
                    .name = "streamline.dlssg.enabled",
                    .reason = "feature_load_change",
                    .previousValue = previousEnabled ? 1u : 0u,
                    .nextValue = nextEnabled ? 1u : 0u,
                });
            }
        }
        return g_legacySlBoolApi.load(std::memory_order_relaxed) ? 1 : kResultOk;
    }
    if (apusrStreamlineFeatureShouldNoopIfSelected(feature)) {
        return kResultOk;
    }
    if (auto fn = originalFn<SlSetFeatureLoadedFn>("slSetFeatureLoaded")) {
        return fn(feature, loaded);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slGetFeatureFunction(std::uint32_t feature, const char* functionName, void** function)
{
    installIdentityHooksForCurrentApi();
    if (function == nullptr) {
        return kResultInvalidParameter;
    }
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        if (auto fn = originalFn<SlGetFeatureFunctionFn>("slGetFeatureFunction")) {
            return fn(feature, functionName, function);
        }
        *function = nullptr;
        return kResultFeatureMissing;
    }
#endif
    if (feature == kFeatureReflex && isReflexStreamlinePath() && functionName != nullptr) {
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
        // Attachment can occur during stock slInit (for example when its NVAPI
        // probe first enters APUSR). In that case we did not observe the
        // preferences passed to the in-flight slInit call. Obtaining a Reflex
        // function is an unambiguous late signal that the host selected it.
        g_reflexLoaded.store(true, std::memory_order_relaxed);
        if (isVulkanStreamlinePath()) {
            apusrVulkanHostRequestAntiLag(true);
        }
#endif
        if (std::strcmp(functionName, "slReflexGetState") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlReflexGetState);
            return kResultOk;
        }
        if (std::strcmp(functionName, "slReflexSleep") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlReflexSleep);
            return kResultOk;
        }
        if (std::strcmp(functionName, "slReflexSetOptions") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlReflexSetOptions);
            return kResultOk;
        }
        if (std::strcmp(functionName, "slReflexSetCameraData") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlReflexSetCameraData);
            return kResultOk;
        }
        if (std::strcmp(functionName, "slReflexGetPredictedCameraData") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlReflexGetPredictedCameraData);
            return kResultOk;
        }
        if (std::strcmp(functionName, "slPCLSetMarker") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlPCLSetMarker);
            return kResultOk;
        }
        *function = nullptr;
        return kResultFeatureMissing;
    }
    if (feature == kFeaturePCL && isReflexStreamlinePath() && functionName != nullptr &&
        std::strcmp(functionName, "slPCLSetMarker") == 0) {
        void* backing = nullptr;
        if (auto fn = originalFn<SlGetFeatureFunctionFn>("slGetFeatureFunction")) {
            const int result = fn(feature, functionName, &backing);
            if (result == kResultOk && backing != nullptr) {
                g_backingPclSetMarker.store(
                    reinterpret_cast<SlPclSetMarkerFn>(backing),
                    std::memory_order_release);
            }
        }
        // The marker itself is part of APUSR's Vulkan Reflex/FG timing
        // contract. Backing PCL adds its statistics path when available, but
        // absence of that optional plugin must not remove APUSR's marker sink.
        *function = reinterpret_cast<void*>(&apusrSlPCLSetMarker);
        if (!g_pclMarkerFunctionLogged.exchange(true, std::memory_order_relaxed)) {
            apusr::host_discovery::eventf(
                "streamline", "pcl.marker_function",
                "backend=%s local=1 backing=%u", isVulkanStreamlinePath() ? "vulkan" : "d3d12", g_backingPclSetMarker.load(std::memory_order_acquire) ? 1u : 0u);
        }
        return kResultOk;
    }
    if (feature == kFeatureDLSS_G && isVulkanStreamlinePath() && functionName != nullptr) {
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
        // Same late-attachment case as Reflex above: requesting the feature
        // function is sufficient evidence to arm APUSR's present-driven FG
        // path even when the original slInit call began before Detours attached.
        g_dlssgLoaded.store(true, std::memory_order_relaxed);
        apusrVulkanHostRequestFrameGeneration(true);
#endif
        if (std::strcmp(functionName, "slDLSSGGetState") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlDLSSGGetState);
            return kResultOk;
        }
        if (std::strcmp(functionName, "slDLSSGSetOptions") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlDLSSGSetOptions);
            return kResultOk;
        }
        *function = nullptr;
        return kResultFeatureMissing;
    }
    if (apusrStreamlineFeatureExplicitlyUnsupported(feature)) {
        *function = nullptr;
        return kResultFeatureMissing;
    }
    if (feature == kFeatureDLSS && functionName != nullptr) {
        if (std::strcmp(functionName, "slDLSSGetOptimalSettings") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlDLSSGetOptimalSettings);
            return kResultOk;
        }
        if (std::strcmp(functionName, "slDLSSGetState") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlDLSSGetState);
            return kResultOk;
        }
        if (std::strcmp(functionName, "slDLSSSetOptions") == 0) {
            *function = reinterpret_cast<void*>(&apusrSlDLSSSetOptions);
            return kResultOk;
        }
    }
    if (auto fn = originalFn<SlGetFeatureFunctionFn>("slGetFeatureFunction")) {
        return fn(feature, functionName, function);
    }
    *function = nullptr;
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slGetFeatureRequirements(std::uint32_t feature, FeatureRequirements* requirements)
{
    installIdentityHooksForCurrentApi();
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        // Let the host's own sl.dlss populate its version-matched semantic
        // requirements first. On Vulkan it unconditionally appends NVIDIA-only
        // extensions after querying NGX; those must not escape to an AMD VkDevice.
        // Preserve its required-tag contract while replacing only the Vulkan
        // device contract with APUSR's actual requirements.
        int stockResult = kResultFeatureMissing;
        if (auto fn = originalFn<SlGetFeatureRequirementsFn>("slGetFeatureRequirements")) {
            stockResult = fn(feature, requirements);
        }
        // The canonical path intentionally relies on stock sl.dlss for the
        // Streamline semantic translation. If that plugin is absent or rejects
        // the feature, do not manufacture a requirements success that cannot
        // later evaluate through the same owner.
        if (stockResult != kResultOk) {
            return stockResult;
        }
        // FeatureRequirements::flags is a capability mask, not the active
        // renderer. Stock sl.dlss advertises Vulkan support even while the host
        // is running D3D12. A previous implementation incorrectly treated that bit as a renderer
        // transition, which flipped the whole attachment to Vulkan in the
        // middle of an active D3D12 startup and can remove DLSS from host discovery.
        // Sanitize Vulkan extensions only when the host-selected renderer is
        // actually Vulkan.
        if (!isVulkanStreamlinePath()) {
            return stockResult;
        }

        const std::uint32_t stockRequiredTagCount = requirements != nullptr ? requirements->numRequiredTags : 0u;
        const std::uint32_t* stockRequiredTags = requirements != nullptr ? requirements->requiredTags : nullptr;
        const std::uint32_t stockDeviceExtensionCount = requirements != nullptr ? requirements->vkNumDeviceExtensions : 0u;
        const std::uint32_t stockInstanceExtensionCount = requirements != nullptr ? requirements->vkNumInstanceExtensions : 0u;
        fillDlssRequirements(requirements);
        if (requirements != nullptr && stockRequiredTags != nullptr) {
            requirements->numRequiredTags = stockRequiredTagCount;
            requirements->requiredTags = stockRequiredTags;
        }
        apusr::host_discovery::eventf(
            "streamline", "attach.dlss.requirements_sanitized",
            "stock_result=%d stock_device_extensions=%u stock_instance_extensions=%u apusr_device_extensions=%u apusr_instance_extensions=%u required_tags=%u",
            stockResult, stockDeviceExtensionCount, stockInstanceExtensionCount,
            requirements != nullptr ? requirements->vkNumDeviceExtensions : 0u,
            requirements != nullptr ? requirements->vkNumInstanceExtensions : 0u,
            requirements != nullptr ? requirements->numRequiredTags : 0u);
        return kResultOk;
    }
#else
    if (feature == kFeatureDLSS) {
        fillDlssRequirements(requirements);
        return kResultOk;
    }
#endif
    if (feature == kFeatureDLSS_G && isVulkanStreamlinePath()) {
        fillDlssgRequirements(requirements);
        return kResultOk;
    }
    if (feature == kFeatureReflex && isReflexStreamlinePath()) {
        fillNeutralOptionalFeatureRequirements(requirements);
        return kResultOk;
    }
    if (apusrStreamlineFeatureExplicitlyUnsupported(feature)) {
        if (isVulkanStreamlinePath()) {
            fillNeutralOptionalFeatureRequirements(requirements);
            apusr::host_discovery::eventf(
                "streamline", "optional_feature.requirements_neutral",
                "feature=%u supported=0 reason=allow_batched_pre_device_discovery", feature);
            return kResultOk;
        }
        return kResultFeatureMissing;
    }
    if (auto fn = originalFn<SlGetFeatureRequirementsFn>("slGetFeatureRequirements")) {
        return fn(feature, requirements);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slGetFeatureVersion(std::uint32_t feature, FeatureVersion* version)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        if (auto fn = originalFn<SlGetFeatureVersionFn>("slGetFeatureVersion")) {
            return fn(feature, version);
        }
        return kResultFeatureMissing;
    }
#endif
    if (feature == kFeatureDLSS) {
        if (version != nullptr) {
            version->versionSL = {
                g_hostSlMajor.load(std::memory_order_relaxed),
                g_hostSlMinor.load(std::memory_order_relaxed),
                g_hostSlBuild.load(std::memory_order_relaxed)};
            version->versionNGX = {3u, 7u, 0u};
        }
        return kResultOk;
    }
    if ((feature == kFeatureReflex && isReflexStreamlinePath()) ||
        (feature == kFeatureDLSS_G && isVulkanStreamlinePath())) {
        if (version != nullptr) {
            version->versionSL = {
                g_hostSlMajor.load(std::memory_order_relaxed),
                g_hostSlMinor.load(std::memory_order_relaxed),
                g_hostSlBuild.load(std::memory_order_relaxed)};
            version->versionNGX = {0u, 0u, 0u};
        }
        return kResultOk;
    }
    if (apusrStreamlineFeatureExplicitlyUnsupported(feature)) {
        return kResultFeatureMissing;
    }
    if (auto fn = originalFn<SlGetFeatureVersionFn>("slGetFeatureVersion")) {
        return fn(feature, version);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slAllocateResources(void* cmdBuffer, std::uint32_t feature, const ViewportHandle* viewport)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        if (auto fn = originalFn<SlAllocateResourcesFn>("slAllocateResources")) {
            return fn(cmdBuffer, feature, viewport);
        }
        return kResultFeatureMissing;
    }
#endif
    if (feature == kFeatureDLSS ||
        ((feature == kFeatureReflex && isReflexStreamlinePath()) || (feature == kFeatureDLSS_G && isVulkanStreamlinePath()))) {
        return kResultOk;
    }
    if (apusrStreamlineFeatureShouldNoopIfSelected(feature)) {
        return kResultOk;
    }
    if (auto fn = originalFn<SlAllocateResourcesFn>("slAllocateResources")) {
        return fn(cmdBuffer, feature, viewport);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slFreeResources(std::uint32_t feature, const ViewportHandle* viewport)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        if (auto fn = originalFn<SlFreeResourcesFn>("slFreeResources")) {
            return fn(feature, viewport);
        }
        return kResultFeatureMissing;
    }
#endif
    if (feature == kFeatureDLSS ||
        ((feature == kFeatureReflex && isReflexStreamlinePath()) || (feature == kFeatureDLSS_G && isVulkanStreamlinePath()))) {
        return kResultOk;
    }
    if (apusrStreamlineFeatureShouldNoopIfSelected(feature)) {
        return kResultOk;
    }
    if (auto fn = originalFn<SlFreeResourcesFn>("slFreeResources")) {
        return fn(feature, viewport);
    }
    return kResultFeatureMissing;
}

APUSR_SL_EXPORT int __cdecl slEvaluateFeature(std::uint32_t feature,
                                                               const void* frame,
                                                               const BaseStructure* const* inputs,
                                                               std::uint32_t numInputs,
                                                               void* cmdBuffer)
{
#if defined(APUSR_STREAMLINE_ATTACH_MODE)
    if (feature == kFeatureDLSS) {
        // Preserve stock sl.dlss evaluation. It provides the exact Streamline
        // revision's tags/options/state transitions and enters APUSR only at
        // the stable NGX boundary. The thread-local depth is a provenance bit
        // for the NGX Vulkan frontend: only this stock evaluation path may
        // claim that Streamline prepared/restores the external image states.
        if (auto fn = originalFn<SlEvaluateFeatureFn>("slEvaluateFeature")) {
            const bool vulkan = isVulkanStreamlinePath();
            if (vulkan) {
                ++g_vulkanDlssEvaluateDepth;
            }
            const int result = fn(feature, frame, inputs, numInputs, cmdBuffer);
            if (vulkan) {
                --g_vulkanDlssEvaluateDepth;
            }
            return result;
        }
        return kResultFeatureMissing;
    }
#endif
    // slInit establishes the render API and installs any required identity hooks.
    // Do not repeat cold-path hook installation on the per-frame evaluation path.
    if (feature == kFeatureDLSS) {
        const std::uint64_t call = g_streamlineDlssEvaluateCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (call == 1u) {
            apusr::host_discovery::eventf(
                "streamline", "slEvaluateFeature.called",
                "feature=%u frame=%p inputs=%u cmd=%p", feature, frame, numInputs, cmdBuffer);
        }
        return evaluateDlssThroughApusrNgx(frame, inputs, numInputs, cmdBuffer);
    }
    if (feature == kFeatureReflex && isReflexStreamlinePath()) {
        return kResultOk;
    }
    if (feature == kFeatureDLSS_G && isVulkanStreamlinePath()) {
        // Most integrations provide persistent/frame tags through slSetTag*;
        // still absorb any local evaluate inputs so the present-driven path
        // sees the same resource/constants contract.
        std::uint32_t viewport = 0u;
        {
            std::lock_guard<std::mutex> framegenLock(g_vulkanFramegenMutex);
            viewport = g_dlssgControl.viewport;
        }
        (void)findViewportInInputs(inputs, numInputs, viewport);
        CapturedTags localTags{};
        CapturedConstants localConstants{};
        captureTagsFromInputs(localTags, inputs, numInputs);
        captureConstantsFromInputs(localConstants, inputs, numInputs);
        if (frame != nullptr) {
            std::lock_guard<std::mutex> stateLock(g_streamlineStateMutex);
            mergeCapturedTags(g_tagsByFrameViewport[{.frame = frame, .viewport = viewport}], localTags);
            if (localConstants.valid)
                g_constantsByFrameViewport[{.frame = frame, .viewport = viewport}] = localConstants;
        }
        return kResultOk;
    }
    if (apusrStreamlineFeatureShouldNoopIfSelected(feature)) {
        return kResultOk;
    }
    if (auto fn = originalFn<SlEvaluateFeatureFn>("slEvaluateFeature")) {
        return fn(feature, frame, inputs, numInputs, cmdBuffer);
    }
    return kResultFeatureMissing;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    // Do not perform logging, LoadLibrary/FreeLibrary, NGX calls, or other
    // loader-sensitive work here. Streamline hosts are free to load and unload
    // this proxy during capability discovery, and DllMain runs under the loader
    // lock. Runtime diagnostics start at slInit instead.
    return TRUE;
}


// Cross-DLL bridge for the Vulkan frontend. The pinned Vulkan host-capture
// module owns device/queue state; Streamline exposes the resolved context to
// independently loaded APUSR frontend DLLs.
APUSR_SL_EXPORT bool __cdecl apusrSlResolveVulkanQueue(
    void* device,
    std::uint32_t preferredKind,
    void** outQueue,
    std::uint32_t* outFamilyIndex,
    std::uint32_t* outQueueIndex,
    std::uint32_t* outKind)
{
    return resolveCapturedVulkanQueue(
        device,
        preferredKind,
        outQueue,
        outFamilyIndex,
        outQueueIndex,
        outKind);
}

APUSR_SL_EXPORT bool __cdecl apusrSlResolveVulkanStreamlineContext(
    void* device,
    std::uint32_t* outGraphicsFamilyIndex,
    std::uint32_t* outComputeFamilyIndex,
    std::uint32_t* outDeviceRequirementFlags)
{
    return resolveVulkanStreamlineContext(
        device,
        outGraphicsFamilyIndex,
        outComputeFamilyIndex,
        outDeviceRequirementFlags);
}

APUSR_SL_EXPORT bool __cdecl apusrSlVulkanDlssEvaluationActive()
{
    return g_vulkanDlssEvaluateDepth != 0u;
}

#if defined(APUSR_STREAMLINE_ATTACH_MODE)
namespace {

PVOID streamlineDetourForName(const char* name) noexcept
{
    if (name == nullptr) {
        return nullptr;
    }
#define APUSR_SL_DETOUR(nameLiteral, functionName) \
    if (std::strcmp(name, nameLiteral) == 0) return reinterpret_cast<PVOID>(&functionName)
    APUSR_SL_DETOUR("slInit", slInit);
    APUSR_SL_DETOUR("slShutdown", slShutdown);
    APUSR_SL_DETOUR("slSetConstants", slSetConstants);
    APUSR_SL_DETOUR("slSetTag", slSetTag);
    APUSR_SL_DETOUR("slSetTagForFrame", slSetTagForFrame);
    APUSR_SL_DETOUR("slSetVulkanInfo", slSetVulkanInfo);
    APUSR_SL_DETOUR("slSetD3DDevice", slSetD3DDevice);
    APUSR_SL_DETOUR("slGetNativeInterface", slGetNativeInterface);
    APUSR_SL_DETOUR("slIsFeatureSupported", slIsFeatureSupported);
    APUSR_SL_DETOUR("slIsFeatureLoaded", slIsFeatureLoaded);
    APUSR_SL_DETOUR("slSetFeatureLoaded", slSetFeatureLoaded);
    APUSR_SL_DETOUR("slGetFeatureFunction", slGetFeatureFunction);
    APUSR_SL_DETOUR("slGetFeatureRequirements", slGetFeatureRequirements);
    APUSR_SL_DETOUR("slGetFeatureVersion", slGetFeatureVersion);
    APUSR_SL_DETOUR("slAllocateResources", slAllocateResources);
    APUSR_SL_DETOUR("slFreeResources", slFreeResources);
    APUSR_SL_DETOUR("slEvaluateFeature", slEvaluateFeature);
    APUSR_SL_DETOUR("vkCreateDevice", apusrVkCreateDevice);
#undef APUSR_SL_DETOUR
    return nullptr;
}

[[nodiscard]] bool isLegacyApusrStreamlineProxy(HMODULE module) noexcept
{
    if (module == nullptr) {
        return false;
    }
    // Older replacement interposers exported these APUSR-private bridges.
    // Attaching the current module to that older proxy creates two independent
    // Reflex/DLSS-G state machines in one process.  Reject the mixed-generation
    // layout instead of silently letting one module configure the feature while
    // the other owns vkQueuePresentKHR.
    for (const char* marker : {
             "apusrSlResolveVulkanQueue",
             "apusrSlResolveVulkanStreamlineContext",
             "apusrSlVulkanDlssEvaluationActive"}) {
        if (GetProcAddress(module, marker) != nullptr) {
            return true;
        }
    }
    return false;
}

bool attachToLoadedStreamlineLocked()
{
    // The normal path attaches to the host-loaded canonical interposer. A tiny
    // loader-boundary bootstrap shim may instead provide the exact renamed
    // stock module explicitly when no APUSR DLL can become visible before the
    // host enters slInit. The attachment semantics are identical either way:
    // the backing PE remains NVIDIA's stock interposer and APUSR detours only
    // named entry points in that exact module.
    HMODULE module = g_streamlineAttachRequestedModule != nullptr
        ? g_streamlineAttachRequestedModule
        : GetModuleHandleW(L"sl.interposer.dll");
    if (module == nullptr) {
        return false;
    }
    if (isLegacyApusrStreamlineProxy(module)) {
        apusr::host_discovery::event(
            "streamline", "attach.unsupported",
            "reason=legacy_apusr_sl_interposer_detected action=restore_game_stock_sl_interposer");
        return false;
    }
    if (g_streamlineAttachInstalled.load(std::memory_order_acquire) &&
        g_attachedStreamlineModule == module) {
        return true;
    }

    // Hold one ordinary loader reference for exactly as long as Detours are
    // installed. The host may balance its own LoadLibrary/FreeLibrary calls,
    // but it must not be able to physically unload the patched PE underneath
    // APUSR. slShutdown detaches first and then releases this reference.
    HMODULE ownedModuleRef = nullptr;
    std::array<wchar_t, 32768> modulePath{};
    const DWORD modulePathLength = GetModuleFileNameW(
        module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (modulePathLength != 0u && modulePathLength < modulePath.size()) {
        ownedModuleRef = LoadLibraryW(modulePath.data());
    }
    if (ownedModuleRef != module) {
        if (ownedModuleRef != nullptr) {
            FreeLibrary(ownedModuleRef);
        }
        apusr::host_discovery::event(
            "streamline", "attach.failed", "stage=retain_stock_module");
        return false;
    }

    // A host may unload a capability-probe copy and later load its persistent
    // interposer. Never cache name/ordinal assumptions across module identity.
    if (g_attachedStreamlineModule != module) {
        for (auto& entry : g_attachedStreamlineExports) {
            entry.trampoline = nullptr;
        }
        g_streamlineAttachInstalled.store(false, std::memory_order_release);
        g_streamlineAttachHookCount.store(0u, std::memory_order_relaxed);
        g_attachedStreamlineModule = module;
        g_original = module;
    }

    std::uint32_t available = 0u;
    for (auto& entry : g_attachedStreamlineExports) {
        entry.trampoline = reinterpret_cast<PVOID>(GetProcAddress(module, entry.name));
        if (entry.trampoline != nullptr) {
            ++available;
        }
    }

    // These four named exports are the minimum v2 attachment contract. Other
    // hooks are revision-optional and are attached only when present.
    for (const char* required : {
             "slEvaluateFeature", "slGetFeatureRequirements",
             "slIsFeatureSupported", "slGetFeatureFunction"}) {
        if (attachedStreamlineTrampoline(required) == nullptr) {
            apusr::host_discovery::eventf(
                "streamline", "attach.unsupported",
                "module=%p missing_named_export=%s available=%u", module, required, available);
            FreeLibrary(ownedModuleRef);
            return false;
        }
    }

    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR) {
        apusr::host_discovery::eventf(
            "streamline", "attach.failed", "stage=transaction_begin error=%ld", error);
        FreeLibrary(ownedModuleRef);
        return false;
    }
    error = DetourUpdateThread(GetCurrentThread());
    if (error != NO_ERROR) {
        DetourTransactionAbort();
        apusr::host_discovery::eventf(
            "streamline", "attach.failed", "stage=update_thread error=%ld", error);
        FreeLibrary(ownedModuleRef);
        return false;
    }

    std::uint32_t attached = 0u;
    for (auto& entry : g_attachedStreamlineExports) {
        if (entry.trampoline == nullptr) {
            continue;
        }
        PVOID detour = streamlineDetourForName(entry.name);
        if (detour == nullptr) {
            continue;
        }
        error = DetourAttach(&entry.trampoline, detour);
        if (error != NO_ERROR) {
            DetourTransactionAbort();
            for (auto& reset : g_attachedStreamlineExports) {
                reset.trampoline = nullptr;
            }
            apusr::host_discovery::eventf(
                "streamline", "attach.failed",
                "stage=attach export=%s error=%ld", entry.name, error);
            FreeLibrary(ownedModuleRef);
            return false;
        }
        ++attached;
    }

    error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        for (auto& entry : g_attachedStreamlineExports) {
            entry.trampoline = nullptr;
        }
        apusr::host_discovery::eventf(
            "streamline", "attach.failed", "stage=commit error=%ld", error);
        FreeLibrary(ownedModuleRef);
        return false;
    }

    g_attachedStreamlineOwnedModuleRef = ownedModuleRef;
    g_streamlineAttachHookCount.store(attached, std::memory_order_relaxed);
    g_streamlineAttachInstalled.store(true, std::memory_order_release);
    const StreamlineBackingVersion version = queryModuleVersion(module);
    if (version.valid) {
        // slInit may already be executing when NGX Vulkan discovery bootstraps
        // attachment. Use the authoritative stock PE version so APUSR-owned Vulkan feature
        // version queries do not depend on having intercepted that one call.
        g_hostSlMajor.store(version.major, std::memory_order_relaxed);
        g_hostSlMinor.store(version.minor, std::memory_order_relaxed);
        g_hostSlBuild.store(version.patch, std::memory_order_relaxed);
        g_legacySlBoolApi.store(version.major < 2u, std::memory_order_relaxed);
    }
    apusr::host_discovery::eventf(
        "streamline", "attach.installed",
        "module=%p version=%u.%u.%u.%u named_exports=%u hooks=%u stock_pe_preserved=1 "
        "loader_bootstrap_alias=%u tag_for_frame=%u",
        module, static_cast<unsigned>(version.major), static_cast<unsigned>(version.minor),
        static_cast<unsigned>(version.patch), static_cast<unsigned>(version.build),
        available, attached, g_streamlineAttachRequestedModule != nullptr ? 1u : 0u,
        GetProcAddress(module, "slSetTagForFrame") != nullptr ? 1u : 0u);
    return true;
}

bool detachFromLoadedStreamlineLocked() noexcept
{
    if (!g_streamlineAttachInstalled.load(std::memory_order_acquire)) {
        return true;
    }

    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR) {
        apusr::host_discovery::eventf(
            "streamline", "attach.detach_failed", "stage=transaction_begin error=%ld", error);
        return false;
    }
    error = DetourUpdateThread(GetCurrentThread());
    if (error != NO_ERROR) {
        DetourTransactionAbort();
        apusr::host_discovery::eventf(
            "streamline", "attach.detach_failed", "stage=update_thread error=%ld", error);
        return false;
    }

    std::uint32_t detached = 0u;
    for (auto& entry : g_attachedStreamlineExports) {
        if (entry.trampoline == nullptr) {
            continue;
        }
        PVOID detour = streamlineDetourForName(entry.name);
        if (detour == nullptr) {
            continue;
        }
        error = DetourDetach(&entry.trampoline, detour);
        if (error != NO_ERROR) {
            DetourTransactionAbort();
            apusr::host_discovery::eventf(
                "streamline", "attach.detach_failed",
                "stage=detach export=%s error=%ld", entry.name, error);
            return false;
        }
        ++detached;
    }

    error = DetourTransactionCommit();
    if (error != NO_ERROR) {
        apusr::host_discovery::eventf(
            "streamline", "attach.detach_failed", "stage=commit error=%ld", error);
        return false;
    }

    for (auto& entry : g_attachedStreamlineExports) {
        entry.trampoline = nullptr;
    }
    HMODULE ownedModuleRef = g_attachedStreamlineOwnedModuleRef;
    g_attachedStreamlineOwnedModuleRef = nullptr;
    g_streamlineAttachHookCount.store(0u, std::memory_order_relaxed);
    g_streamlineAttachInstalled.store(false, std::memory_order_release);
    g_attachedStreamlineModule = nullptr;
    g_original = nullptr;
    if (ownedModuleRef != nullptr) {
        FreeLibrary(ownedModuleRef);
    }
    apusr::host_discovery::eventf(
        "streamline", "attach.detached", "hooks=%u stock_pe_restored=1 module_ref_released=1", detached);
    return true;
}

} // namespace

void apusrDetachFromLoadedStreamlineAfterShutdown() noexcept
{
    std::lock_guard<std::mutex> lock(g_streamlineAttachMutex);
    (void)detachFromLoadedStreamlineLocked();
}

extern "C" __declspec(dllexport) bool __cdecl apusrAttachToLoadedStreamline()
{
    std::lock_guard<std::mutex> lock(g_streamlineAttachMutex);
    return attachToLoadedStreamlineLocked();
}

extern "C" __declspec(dllexport) bool __cdecl apusrAttachToStreamlineModule(void* module)
{
    if (module == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_streamlineAttachMutex);
    g_streamlineAttachRequestedModule = static_cast<HMODULE>(module);
    return attachToLoadedStreamlineLocked();
}

extern "C" __declspec(dllexport) void __cdecl apusrStreamlineAttachHintRenderApi(std::uint32_t renderApi)
{
    if (renderApi > kSlRenderApiVulkan) {
        return;
    }
    g_slRenderApi.store(renderApi, std::memory_order_relaxed);
    apusrIdentitySpoofSetStreamlineRenderApi(renderApi);
    if (renderApi == kSlRenderApiVulkan) {
        const bool captureInstalled = apusrInstallVulkanHostCapture();
        apusr::host_discovery::eventf(
            "streamline", "attach.render_api_hint",
            "render_api=vulkan host_capture=%u hooks=%u",
            captureInstalled ? 1u : 0u,
            g_streamlineAttachHookCount.load(std::memory_order_relaxed));
    }
}

extern "C" __declspec(dllexport) std::uint32_t __cdecl apusrStreamlineAttachGetRenderApi()
{
    return g_slRenderApi.load(std::memory_order_acquire);
}

extern "C" __declspec(dllexport) bool __cdecl apusrStreamlineAttachBindD3D12NativeDevice(void* nativeDevice)
{
    g_slRenderApi.store(kSlRenderApiD3d12, std::memory_order_relaxed);
    apusrIdentitySpoofSetStreamlineRenderApi(kSlRenderApiD3d12);
    g_reflexLoaded.store(true, std::memory_order_release);
    return bindD3d12AntiLagNativeDevice(nativeDevice);
}

extern "C" __declspec(dllexport) bool __cdecl apusrStreamlineVulkanDlssEvaluationActive()
{
    return g_vulkanDlssEvaluateDepth != 0u;
}

extern "C" __declspec(dllexport) bool __cdecl apusrStreamlineAttachInstalled()
{
    return g_streamlineAttachInstalled.load(std::memory_order_acquire);
}
#endif

#endif // _WIN32
