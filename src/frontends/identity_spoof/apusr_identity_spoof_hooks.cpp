// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
//
// Minimal process compatibility hooks for DLSS host discovery.
//
// This is intentionally not a general hook framework.  It patches import-table slots in already-loaded
// modules for the few identity/capability surfaces used while the host decides whether DLSS is available:
// USER32 display-device strings, ADVAPI display-driver registry strings, D3DKMT adapter-private data,
// relevant loader calls and sl.common's plugin-function lookup for the DX compatibility path. Vulkan
// manual-host device creation is deliberately left to the host and Vulkan loader. The hooks are
// installed by the early DLSS compatibility package or the explicit Streamline Vulkan package.
// Frame evaluation remains separate.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <winnt.h>

#include "../common/apusr_host_discovery_log.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <string_view>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

extern "C" HRESULT WINAPI apusr_CreateDXGIFactory(REFIID riid, void** factory);
extern "C" HRESULT WINAPI apusr_CreateDXGIFactory1(REFIID riid, void** factory);
extern "C" HRESULT WINAPI apusr_CreateDXGIFactory2(UINT flags, REFIID riid, void** factory);
namespace {

constexpr wchar_t kSpoofGpuNameW[] = L"NVIDIA GeForce RTX 3090";
constexpr char kSpoofGpuNameA[] = "NVIDIA GeForce RTX 3090";
constexpr wchar_t kSpoofProviderW[] = L"NVIDIA";
constexpr char kSpoofProviderA[] = "NVIDIA";
constexpr wchar_t kSpoofDriverVersionW[] = L"32.0.15.6081";
constexpr char kSpoofDriverVersionA[] = "32.0.15.6081";
constexpr wchar_t kSpoofVendorTokenW[] = L"VEN_10DE";
constexpr char kSpoofVendorTokenA[] = "VEN_10DE";
constexpr wchar_t kSpoofDeviceTokenW[] = L"DEV_2204";
constexpr char kSpoofDeviceTokenA[] = "DEV_2204";

using EnumDisplayDevicesAFn = BOOL(WINAPI*)(LPCSTR, DWORD, PDISPLAY_DEVICEA, DWORD);
using EnumDisplayDevicesWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, PDISPLAY_DEVICEW, DWORD);
using RegOpenKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using RegEnumValueWFn = LSTATUS(WINAPI*)(HKEY, DWORD, LPWSTR, LPDWORD, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);
using RegQueryValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegQueryValueExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegGetValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using SlGetPluginFunctionFn = void*(__cdecl*)(const char*);
using SlOnPluginLoadFn = bool(__cdecl*)(void*, const char*, const char**);
using SlOnPluginStartupFn = bool(__cdecl*)(const char*, void*);
using SlOnPluginShutdownFn = void(__cdecl*)();
using SlOnPluginsInitializedFn = bool(__cdecl*)();
using SlIsSupportedFn = int(__cdecl*)(const void*);
using SlInitFn = int(__cdecl*)(const void*, std::uint64_t);
using ApusrAttachToLoadedStreamlineFn = bool(__cdecl*)();
using ApusrStreamlineAttachHintRenderApiFn = void(__cdecl*)(std::uint32_t);
using ApusrStreamlineAttachGetRenderApiFn = std::uint32_t(__cdecl*)();
using ApusrStreamlineAttachBindD3D12NativeDeviceFn = bool(__cdecl*)(void*);
using SlCoreSetD3DDeviceFn = int(__cdecl*)(void*);
using SlCoreGetFeatureFunctionFn = int(__cdecl*)(std::uint32_t, const char*, void**);
using SlCoreSetTagFn = int(__cdecl*)(const void*, const void*, std::uint32_t, void*);
using SlCoreSetTagForFrameFn = int(__cdecl*)(const void*, const void*, const void*, std::uint32_t, void*);
using SlCoreSetConstantsFn = int(__cdecl*)(const void*, const void*, const void*);
using SlCoreEvaluateFeatureFn = int(__cdecl*)(std::uint32_t, const void*, const void* const*, std::uint32_t, void*);
using SlCoreAllocateResourcesFn = int(__cdecl*)(void*, std::uint32_t, const void*);
using SlCoreFreeResourcesFn = int(__cdecl*)(std::uint32_t, const void*);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using D3DKMTQueryAdapterInfoFn = LONG(WINAPI*)(void*);
using D3DKMTEnumAdaptersFn = LONG(WINAPI*)(void*);
using D3DKMTEnumAdapters2Fn = LONG(WINAPI*)(void*);
using D3DKMTEnumAdapters3Fn = LONG(WINAPI*)(void*);
using D3DKMTCloseAdapterFn = LONG(WINAPI*)(void*);

struct KmtQueryAdapterInfoLoose {
    std::uint32_t hAdapter;
    std::uint32_t type;
    void* privateDriverData;
    std::uint32_t privateDriverDataSize;
};


constexpr std::uint32_t kFeatureDLSS = 0u;
constexpr std::uint32_t kFeatureReflex = 3u;
constexpr std::uint32_t kFeatureDLSS_G = 1000u;
constexpr std::uint32_t kFeatureDLSS_RR = 1001u;
constexpr std::uint32_t kSlRenderApiD3d11 = 0u;
constexpr std::uint32_t kSlRenderApiD3d12 = 1u;
constexpr std::uint32_t kSlRenderApiVulkan = 2u;
constexpr std::uint32_t kSlRenderApiUnknown = 0xffffffffu;

constexpr bool apusrStreamlineFeatureExplicitlyUnsupported(std::uint32_t feature) noexcept
{
    // PCL is cross-vendor and remains owned by the backing Streamline plugin.
    // Keep only vendor-specific features that APUSR cannot yet translate here.
    return feature == kFeatureDLSS_G || feature == kFeatureDLSS_RR || feature == kFeatureReflex;
}

constexpr bool apusrStreamlineFeatureShouldNoopIfSelected(std::uint32_t feature) noexcept
{
    return apusrStreamlineFeatureExplicitlyUnsupported(feature);
}

constexpr int kSlResultOk = 0;
constexpr int kSlResultInvalidParameter = 25;
constexpr int kSlResultMissingInputParameter = 20;
constexpr int kSlResultFeatureMissing = 31;

// Streamline Preferences v1 prefix through renderAPI. This is the same stable
// x64 layout consumed by the private attachment DLL. The identity layer reads
// it only from modern SDK calls (kSDKVersion low word 0xfedc) and only to arm
// renderer-specific APUSR infrastructure before stock slInit creates devices.
struct SlPreferencesBootstrapLoose {
    void* next;
    std::uint8_t structType[16];
    std::size_t structVersion;
    bool showConsole;
    std::uint8_t _padShowConsole[3];
    std::uint32_t logLevel;
    const wchar_t** pathsToPlugins;
    std::uint32_t numPathsToPlugins;
    std::uint32_t _padPaths;
    const wchar_t* pathToLogsAndData;
    void* allocateCallback;
    void* releaseCallback;
    void* logMessageCallback;
    std::uint64_t flags;
    const std::uint32_t* featuresToLoad;
    std::uint32_t numFeaturesToLoad;
    std::uint32_t applicationId;
    std::uint32_t engine;
    std::uint32_t _padEngine;
    const char* engineVersion;
    const char* projectId;
    std::uint32_t renderAPI;
    std::uint32_t _padRenderApi;
};
static_assert(sizeof(SlPreferencesBootstrapLoose) == 144, "Streamline x64 Preferences bootstrap ABI drift");
static_assert(offsetof(SlPreferencesBootstrapLoose, featuresToLoad) == 96, "Streamline Preferences features ABI drift");
static_assert(offsetof(SlPreferencesBootstrapLoose, renderAPI) == 136, "Streamline Preferences renderAPI ABI drift");

struct StructType {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data4[8];
};

struct SlBaseStructure {
    SlBaseStructure* next;
    StructType structType;
    std::size_t structVersion;
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

struct SlViewportHandle : SlBaseStructure {
    std::uint32_t value;
};

enum class SlDLSSMode : std::uint32_t {
    eOff = 0,
    eMaxPerformance = 1,
    eBalanced = 2,
    eMaxQuality = 3,
    eUltraPerformance = 4,
    eUltraQuality = 5,
    eDLAA = 6,
};

enum class SlDLSSPreset : std::uint32_t {
    eDefault = 0,
    ePresetE = 5,
    ePresetF = 6,
    ePresetG = 7,
    ePresetH = 8,
    ePresetI = 9,
    ePresetJ = 10,
    ePresetK = 11,
    ePresetL = 12,
    ePresetM = 13,
    ePresetN = 14,
    ePresetO = 15,
};

struct SlDLSSOptions : SlBaseStructure {
    SlDLSSMode mode;
    std::uint32_t outputWidth;
    std::uint32_t outputHeight;
    float sharpness;
    float preExposure;
    float exposureScale;
    char colorBuffersHDR;
    char indicatorInvertAxisX;
    char indicatorInvertAxisY;
    SlDLSSPreset dlaaPreset;
    SlDLSSPreset qualityPreset;
    SlDLSSPreset balancedPreset;
    SlDLSSPreset performancePreset;
    SlDLSSPreset ultraPerformancePreset;
    SlDLSSPreset ultraQualityPreset;
    char useAutoExposure;
    char alphaUpscalingEnabled;
};

struct SlConstants : SlBaseStructure {
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

struct SlDLSSOptimalSettings : SlBaseStructure {
    std::uint32_t optimalRenderWidth;
    std::uint32_t optimalRenderHeight;
    float optimalSharpness;
    std::uint32_t renderWidthMin;
    std::uint32_t renderHeightMin;
    std::uint32_t renderWidthMax;
    std::uint32_t renderHeightMax;
};

struct SlDLSSState : SlBaseStructure {
    std::uint64_t estimatedVRAMUsageInBytes;
};


struct SlExtent {
    std::uint32_t left;
    std::uint32_t top;
    std::uint32_t width;
    std::uint32_t height;
};

struct SlResource : SlBaseStructure {
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

struct SlResourceTag : SlBaseStructure {
    SlResource* resource;
    std::uint32_t type;
    std::uint32_t lifecycle;
    SlExtent extent;
};

struct ID3D11Resource;
struct ID3D12Resource;
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

constexpr std::uint32_t kStructDLSSOptions = 0x6ac826e4U;
constexpr std::uint32_t kStructDLSSOptimalSettings = 0xef1d0957U;
constexpr std::uint32_t kStructDLSSState = 0x9366b056U;
constexpr std::uint32_t kStructViewportHandle = 0x171b6435U;
constexpr std::uint32_t kStructConstants = 0xdcd35ad7U;

constexpr std::uint32_t kStructResourceTag = 0x4c6a5aadU;
constexpr std::uint32_t kBufferTypeDepth = 0u;
constexpr std::uint32_t kBufferTypeMotionVectors = 1u;
constexpr std::uint32_t kBufferTypeScalingInputColor = 3u;
constexpr std::uint32_t kBufferTypeScalingOutputColor = 4u;
constexpr std::uint32_t kBufferTypeExposure = 13u;
constexpr std::uint32_t kBufferTypeBiasCurrentColorHint = 29u;
constexpr std::uint32_t kBufferTypeReactiveMaskHint = 36u;
constexpr std::uint32_t kNgxFeatureSuperSampling = 1u;
constexpr std::uint32_t kNgxSuccess = 0x00000001u;
constexpr char kApusrStreamlineD3D12PreparedStatesParameter[] =
    "APUSR.Streamline.D3D12.PreparedStates";

using NgxD3D12InitFn = std::uint32_t(__cdecl*)(unsigned long long, const wchar_t*, void*, const void*, unsigned int);
using NgxVulkanInitFn = std::uint32_t(__cdecl*)(unsigned long long, const wchar_t*, void*, void*, void*, ...);
using NgxAllocateParametersFn = std::uint32_t(__cdecl*)(NVSDK_NGX_Parameter**);
using NgxGetCapabilityParametersFn = std::uint32_t(__cdecl*)(NVSDK_NGX_Parameter**);
using NgxDestroyParametersFn = std::uint32_t(__cdecl*)(NVSDK_NGX_Parameter*);
using NgxCreateFeatureFn = std::uint32_t(__cdecl*)(void*, unsigned int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using NgxEvaluateFeatureFn = std::uint32_t(__cdecl*)(void*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*);
using NgxReleaseFeatureFn = std::uint32_t(__cdecl*)(NVSDK_NGX_Handle*);
using NgxUpdateFeatureFn = std::uint32_t(__cdecl*)(const void*, unsigned int);

// Streamline 2.x passes this stack-local structure as slOnPluginStartup's
// `device` argument when the active renderer is Vulkan. Keep the private ABI
// isolated to this cold-path bridge; stock sl.dlss remains the semantic owner.
struct StreamlineVkDevicesBridge {
    void* instance = nullptr;
    void* device = nullptr;
    void* physical = nullptr;
};
static_assert(sizeof(StreamlineVkDevicesBridge) == 24, "Streamline VkDevices x64 ABI drift");
static_assert(offsetof(StreamlineVkDevicesBridge, device) == 8, "Streamline VkDevices x64 ABI drift");
static_assert(offsetof(StreamlineVkDevicesBridge, physical) == 16, "Streamline VkDevices x64 ABI drift");

using StreamlineNgxCreateFeatureFn = bool(__cdecl*)(void*, unsigned int, NVSDK_NGX_Handle**, const char*);
using StreamlineNgxEvaluateFeatureFn = bool(__cdecl*)(void*, NVSDK_NGX_Handle*, const char*);
using StreamlineNgxReleaseFeatureFn = bool(__cdecl*)(NVSDK_NGX_Handle*, const char*);
using StreamlineNgxUpdateFeatureFn = void(__cdecl*)(unsigned int);

struct StreamlineNgxContextBridge {
    NVSDK_NGX_Parameter* params = nullptr;
    StreamlineNgxCreateFeatureFn createFeature = nullptr;
    StreamlineNgxReleaseFeatureFn releaseFeature = nullptr;
    StreamlineNgxEvaluateFeatureFn evaluateFeature = nullptr;
    StreamlineNgxUpdateFeatureFn updateFeature = nullptr;
};

// slOnPluginLoad receives the current Streamline parameter bus. Hosts may
// destroy and recreate this bus without restarting the process, so its pointer
// is an epoch-scoped handle rather than process-lifetime state. The pointer ABI
// below is the 2.x plugin contract used by stock sl.common/sl.dlss.
// APUSR touches only set(const char*, void*) and keeps this cold-path private ABI
// isolated here rather than replacing Streamline's per-frame SR translator.
struct StreamlineParameterInterface {
    virtual void set(const char*, bool) = 0;
    virtual void set(const char*, unsigned long long) = 0;
    virtual void set(const char*, float) = 0;
    virtual void set(const char*, double) = 0;
    virtual void set(const char*, unsigned int) = 0;
    virtual void set(const char*, int) = 0;
    virtual void set(const char*, void*) = 0;

    virtual bool get(const char*, bool*) const = 0;
    virtual bool get(const char*, unsigned long long*) const = 0;
    virtual bool get(const char*, float*) const = 0;
    virtual bool get(const char*, double*) const = 0;
    virtual bool get(const char*, unsigned int*) const = 0;
    virtual bool get(const char*, int*) const = 0;
    virtual bool get(const char*, void**) const = 0;

    virtual std::vector<std::string> enumerate() const = 0;
};

constexpr char kStreamlineNgxContextParameter[] = "sl.param.global.ngxContext";

std::uint32_t apusrStreamlineRenderApiFromStartupJson(const char* json) noexcept
{
    // slOnPluginStartup receives the plugin-manager JSON used by stock
    // plugin::onStartup.  Read only the stable integer deviceType field; do not
    // pull a JSON parser into this loader/cold path.  This also keeps renderer
    // identity local to the DLL instance that owns the hook instead of relying
    // on state written by the separate Vulkan attachment DLL.
    if (json == nullptr) {
        return kSlRenderApiUnknown;
    }
    constexpr char key[] = "\"deviceType\"";
    const char* cursor = std::strstr(json, key);
    if (cursor == nullptr) {
        return kSlRenderApiUnknown;
    }
    cursor += sizeof(key) - 1u;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        ++cursor;
    }
    if (*cursor != ':') {
        return kSlRenderApiUnknown;
    }
    ++cursor;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
        ++cursor;
    }
    if (cursor[0] >= '0' && cursor[0] <= '2' &&
        !(cursor[1] >= '0' && cursor[1] <= '9')) {
        return static_cast<std::uint32_t>(cursor[0] - '0');
    }
    return kSlRenderApiUnknown;
}

struct ApusrStreamlineKey {
    const void* frame = nullptr;
    std::uint32_t viewport = 0;
    [[nodiscard]] bool operator==(const ApusrStreamlineKey& other) const noexcept
    {
        return frame == other.frame && viewport == other.viewport;
    }
};

struct ApusrStreamlineKeyHash {
    [[nodiscard]] std::size_t operator()(const ApusrStreamlineKey& key) const noexcept
    {
        return (reinterpret_cast<std::size_t>(key.frame) >> 4u) ^
               (static_cast<std::size_t>(key.viewport) * 0x9e3779b97f4a7c15ull);
    }
};

struct ApusrCapturedResource {
    void* native = nullptr;
    std::uint32_t state = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct ApusrCapturedTags {
    ApusrCapturedResource color;
    ApusrCapturedResource depth;
    ApusrCapturedResource motionVectors;
    ApusrCapturedResource output;
    ApusrCapturedResource exposure;
    ApusrCapturedResource reactive;
};

struct ApusrCapturedOptions {
    SlDLSSMode mode = SlDLSSMode::eBalanced;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    float sharpness = 0.0f;
    float preExposure = 1.0f;
    float exposureScale = 1.0f;
    bool useAutoExposure = false;
};

struct ApusrCapturedConstants {
    bool valid = false;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    bool reset = false;
    bool depthInverted = false;
    bool motionVectorsJittered = false;
    bool cameraMotionIncluded = true;
};

int __cdecl apusrCoreSlSetD3DDevice(void* d3dDevice);
int __cdecl apusrCoreSlGetFeatureFunction(std::uint32_t feature, const char* functionName, void** function);
int __cdecl apusrCoreSlSetTag(const void* viewport, const void* tags, std::uint32_t numTags, void* cmdBuffer);
int __cdecl apusrCoreSlSetTagForFrame(const void* frame, const void* viewport, const void* tags, std::uint32_t numTags, void* cmdBuffer);
int __cdecl apusrCoreSlSetConstants(const void* values, const void* frame, const void* viewport);
int __cdecl apusrCoreSlEvaluateFeature(std::uint32_t feature, const void* frame, const void* const* inputs, std::uint32_t numInputs, void* cmdBuffer);
int __cdecl apusrCoreSlAllocateResources(void* cmdBuffer, std::uint32_t feature, const void* viewport);
int __cdecl apusrCoreSlFreeResources(std::uint32_t feature, const void* viewport);

EnumDisplayDevicesAFn g_enumDisplayDevicesA = nullptr;
EnumDisplayDevicesWFn g_enumDisplayDevicesW = nullptr;
RegOpenKeyExWFn g_regOpenKeyExW = nullptr;
RegEnumValueWFn g_regEnumValueW = nullptr;
RegCloseKeyFn g_regCloseKey = nullptr;
RegQueryValueExWFn g_regQueryValueExW = nullptr;
RegQueryValueExAFn g_regQueryValueExA = nullptr;
RegGetValueWFn g_regGetValueW = nullptr;
SlGetPluginFunctionFn g_slGetPluginFunction = nullptr;
SlOnPluginLoadFn g_slOnPluginLoad = nullptr;
SlOnPluginStartupFn g_slOnPluginStartup = nullptr;
SlOnPluginShutdownFn g_slOnPluginShutdown = nullptr;
SlOnPluginsInitializedFn g_slOnPluginsInitialized = nullptr;
SlIsSupportedFn g_slIsSupported = nullptr;
std::atomic<SlInitFn> g_stockSlInit{nullptr};
std::mutex g_streamlineBootstrapMutex;
HMODULE g_streamlineAttachBootstrapModule = nullptr;
std::atomic<bool> g_streamlineReflexRequested{false};

[[nodiscard]] bool rearmD3d12ReflexAfterRendererRestart(void* nativeDevice);

// Vulkan-capable hosts can resolve vkCreateDevice through vkGetInstanceProcAddr
// before Streamline calls slInit.  If capture is installed only from slInit,
// those already-cached loader trampolines permanently bypass our Detours hook.
// Arm the passive loader capture as soon as the host actually loads/resolves
// vulkan-1.dll; this does not select Vulkan or alter D3D identity.
std::mutex g_vulkanEarlyCaptureMutex;
HMODULE g_vulkanEarlyCaptureModule = nullptr;
std::atomic<bool> g_vulkanEarlyCaptureReady{false};

// Loader interception is intentionally process-wide, but capture installation
// itself calls Vulkan/Win32 loader APIs. Never let those calls recursively enter
// the early-capture installer: std::mutex detects same-thread relock on MSVC and
// reports "resource deadlock would occur", and even a recursive mutex would only
// turn the bug into nested Detour transactions.
thread_local bool g_vulkanEarlyCaptureInstallingOnThisThread = false;
std::atomic<bool> g_loggedVulkanEarlyCaptureReentrant{false};
SlCoreSetD3DDeviceFn g_coreSlSetD3DDevice = nullptr;
SlCoreGetFeatureFunctionFn g_coreSlGetFeatureFunction = nullptr;
SlCoreSetTagFn g_coreSlSetTag = nullptr;
SlCoreSetTagForFrameFn g_coreSlSetTagForFrame = nullptr;
SlCoreSetConstantsFn g_coreSlSetConstants = nullptr;
SlCoreEvaluateFeatureFn g_coreSlEvaluateFeature = nullptr;
SlCoreAllocateResourcesFn g_coreSlAllocateResources = nullptr;
SlCoreFreeResourcesFn g_coreSlFreeResources = nullptr;
GetProcAddressFn g_getProcAddress = nullptr;
LoadLibraryAFn g_loadLibraryA = nullptr;
LoadLibraryWFn g_loadLibraryW = nullptr;
LoadLibraryExAFn g_loadLibraryExA = nullptr;
LoadLibraryExWFn g_loadLibraryExW = nullptr;
D3DKMTQueryAdapterInfoFn g_d3dkmtQueryAdapterInfo = nullptr;
D3DKMTEnumAdaptersFn g_d3dkmtEnumAdapters = nullptr;
D3DKMTEnumAdapters2Fn g_d3dkmtEnumAdapters2 = nullptr;
D3DKMTEnumAdapters3Fn g_d3dkmtEnumAdapters3 = nullptr;
D3DKMTCloseAdapterFn g_d3dkmtCloseAdapter = nullptr;

std::mutex g_hookMutex;
std::unordered_set<HMODULE> g_patchedModules;
std::atomic<bool> g_installing{false};
std::atomic<bool> g_installedOnce{false};
std::mutex g_spoofRegistryMutex;
std::unordered_set<HKEY> g_spoofRegistryHandles;
std::atomic<bool> g_loggedDisplaySpoof{false};
std::atomic<bool> g_loggedRegistrySpoof{false};
std::atomic<bool> g_loggedKmtQuery{false};

HMODULE g_apusrHostNgx = nullptr;
void* g_apusrHostD3DDevice = nullptr;
void* g_apusrNgxFeatureDevice = nullptr;
std::mutex g_apusrStreamlineMutex;
std::unordered_map<std::uint32_t, ApusrCapturedOptions> g_apusrOptionsByViewport;
std::unordered_map<std::uint32_t, ApusrCapturedConstants> g_apusrConstantsByViewport;
std::unordered_map<ApusrStreamlineKey, ApusrCapturedConstants, ApusrStreamlineKeyHash> g_apusrConstantsByFrameViewport;
std::unordered_map<std::uint32_t, ApusrCapturedTags> g_apusrLegacyTagsByViewport;
std::unordered_map<ApusrStreamlineKey, ApusrCapturedTags, ApusrStreamlineKeyHash> g_apusrTagsByFrameViewport;
std::atomic<std::uint64_t> g_apusrTagLogCounter{0};
std::atomic<std::uint64_t> g_apusrConstantsLogCounter{0};
std::atomic<std::uint64_t> g_apusrSetOptionsLogCounter{0};
std::atomic<std::uint64_t> g_apusrEvaluateResultLogCounter{0};
std::mutex g_apusrPluginFunctionLogMutex;
std::unordered_set<std::string> g_apusrLoggedPluginFunctions;
std::atomic<bool> g_apusrOwnsStreamlineDlss{false};
std::atomic<std::uint32_t> g_apusrStreamlineRenderApi{kSlRenderApiUnknown};
std::atomic<ApusrStreamlineAttachGetRenderApiFn> g_apusrAttachedRenderApiGetter{nullptr};
std::atomic<std::uint32_t> g_apusrLastBridgedRenderApi{kSlRenderApiUnknown};
std::atomic<bool> g_apusrLoggedSkippedOwnedTagsForward{false};
std::atomic<bool> g_apusrLoggedSkippedOwnedConstantsForward{false};
std::mutex g_apusrNgxMutex;
NVSDK_NGX_Handle* g_apusrNgxFeature = nullptr;
std::uint32_t g_apusrNgxRenderWidth = 0;
std::uint32_t g_apusrNgxRenderHeight = 0;
std::uint32_t g_apusrNgxOutputWidth = 0;
std::uint32_t g_apusrNgxOutputHeight = 0;
SlDLSSMode g_apusrNgxMode = SlDLSSMode::eBalanced;
std::uint32_t g_apusrNgxFeatureFlags = 0;
NgxD3D12InitFn g_apusrNgxInitFn = nullptr;
NgxVulkanInitFn g_apusrNgxVulkanInitFn = nullptr;
NgxAllocateParametersFn g_apusrNgxAllocateParametersFn = nullptr;
NgxGetCapabilityParametersFn g_apusrNgxGetCapabilityParametersFn = nullptr;
NgxDestroyParametersFn g_apusrNgxDestroyParametersFn = nullptr;
NgxCreateFeatureFn g_apusrNgxCreateFeatureFn = nullptr;
NgxEvaluateFeatureFn g_apusrNgxEvaluateFeatureFn = nullptr;
NgxReleaseFeatureFn g_apusrNgxReleaseFeatureFn = nullptr;
NgxGetCapabilityParametersFn g_apusrNgxVulkanGetCapabilityParametersFn = nullptr;
NgxDestroyParametersFn g_apusrNgxVulkanDestroyParametersFn = nullptr;
NgxCreateFeatureFn g_apusrNgxVulkanCreateFeatureFn = nullptr;
NgxEvaluateFeatureFn g_apusrNgxVulkanEvaluateFeatureFn = nullptr;
NgxReleaseFeatureFn g_apusrNgxVulkanReleaseFeatureFn = nullptr;
NgxUpdateFeatureFn g_apusrNgxUpdateFeatureFn = nullptr;
NVSDK_NGX_Parameter* g_apusrNgxEvalParams = nullptr;

std::mutex g_streamlineNgxBridgeMutex;
StreamlineParameterInterface* g_streamlineParameters = nullptr;
StreamlineParameterInterface* g_streamlineNgxContextInstalledBus = nullptr;
std::uint64_t g_streamlineParameterBusEpoch = 0;
StreamlineNgxContextBridge g_streamlineNgxContext{};
std::uint32_t g_streamlineNgxContextRenderApi = kSlRenderApiUnknown;
Microsoft::WRL::ComPtr<IUnknown> g_streamlineNgxDeviceIdentity;
void* g_streamlineNgxVulkanInstance = nullptr;
void* g_streamlineNgxVulkanPhysicalDevice = nullptr;
void* g_streamlineNgxVulkanDevice = nullptr;
std::atomic<bool> g_streamlineNgxDeviceLossCircuitOpen{false};

void patchLoadedModules();
bool apusrInstallStreamlineNgxContextBridge(std::uint32_t renderApi, void* device);

[[nodiscard]] std::uint32_t currentStreamlineRenderApi() noexcept
{
    auto getter = g_apusrAttachedRenderApiGetter.load(std::memory_order_acquire);
    std::uint32_t local = g_apusrStreamlineRenderApi.load(std::memory_order_acquire);

    // The loader bootstrap can enter apusr_streamline_attach before dxgi.dll is
    // loaded. identity_spoof is compiled into both DLLs, so their atomics are
    // intentionally separate. When this copy has no renderer yet, discover the
    // already-loaded attach module and use its renderer state as the canonical
    // process value. This preserves the existing lazy DXGI load order instead
    // of preloading dxgi.dll from the bootstrap.
    if (getter == nullptr && local == kSlRenderApiUnknown) {
        if (HMODULE attach = GetModuleHandleW(L"apusr_streamline_attach.dll")) {
            getter = reinterpret_cast<ApusrStreamlineAttachGetRenderApiFn>(
                GetProcAddress(attach, "apusrStreamlineAttachGetRenderApi"));
            if (getter != nullptr) {
                g_apusrAttachedRenderApiGetter.store(getter, std::memory_order_release);
            }
        }
    }

    if (getter != nullptr) {
        const std::uint32_t attached = getter();
        if (attached <= kSlRenderApiVulkan || attached == kSlRenderApiUnknown) {
            g_apusrStreamlineRenderApi.store(attached, std::memory_order_release);
            if (attached != kSlRenderApiUnknown) {
                const std::uint32_t previous = g_apusrLastBridgedRenderApi.exchange(
                    attached, std::memory_order_acq_rel);
                if (previous != attached) {
                    apusr::host_discovery::eventf(
                        "streamline", "renderer_state.bridged",
                        "render_api=%u source=apusr_streamline_attach",
                        attached);
                }
            }
            return attached;
        }
    }

    return local;
}

[[nodiscard]] const char* streamlineRenderApiName(const std::uint32_t renderApi) noexcept
{
    switch (renderApi) {
    case kSlRenderApiD3d11: return "d3d11";
    case kSlRenderApiD3d12: return "d3d12";
    case kSlRenderApiVulkan: return "vulkan";
    default: return "unknown";
    }
}

bool equalsIgnoreCaseAscii(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

bool isOurModule(HMODULE module)
{
    if (module == reinterpret_cast<HMODULE>(&__ImageBase)) {
        return true;
    }

    // Do not IAT-hook APUSR support DLLs with the loader interception that they
    // themselves use to install Vulkan capture. A previous implementation patched
    // apusr_vulkan_host_capture.dll after D3D12/Reflex loaded it as a dependency,
    // so its later LoadLibraryW("vulkan-1.dll") re-entered the DXGI hook while
    // the early-capture mutex was already held.
    wchar_t path[MAX_PATH] = {};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return false;
    }
    const wchar_t* base = std::wcsrchr(path, L'\\');
    base = base != nullptr ? base + 1 : path;
    return _wcsnicmp(base, L"apusr_", 6u) == 0;
}

const wchar_t* moduleBaseName(HMODULE module)
{
    static thread_local wchar_t path[MAX_PATH] = {};
    path[0] = L'\0';
    if (module == nullptr) return nullptr;
    const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return nullptr;
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    return slash != nullptr ? slash + 1 : path;
}

bool moduleBaseNameEquals(HMODULE module, const wchar_t* expected)
{
    const wchar_t* name = moduleBaseName(module);
    return name != nullptr && expected != nullptr && _wcsicmp(name, expected) == 0;
}

bool requestLooksRelevantA(LPCSTR name)
{
    if (name == nullptr) return false;
    const char* slash = std::strrchr(name, '\\');
    const char* base = slash != nullptr ? slash + 1 : name;
    return _stricmp(base, "nvapi64.dll") == 0 ||
           _stricmp(base, "nvngx.dll") == 0 ||
           _stricmp(base, "nvngx_dlss.dll") == 0 ||
           _stricmp(base, "nvngx_dlssd.dll") == 0 ||
           _stricmp(base, "sl.interposer.dll") == 0 ||
           _stricmp(base, "sl.common.dll") == 0 ||
           _stricmp(base, "sl.dlss.dll") == 0 ||
           _stricmp(base, "sl.dlss_d.dll") == 0 ||
           _stricmp(base, "vulkan-1.dll") == 0;
}

bool requestLooksRelevantW(LPCWSTR name)
{
    if (name == nullptr) return false;
    const wchar_t* slash = std::wcsrchr(name, L'\\');
    const wchar_t* base = slash != nullptr ? slash + 1 : name;
    return _wcsicmp(base, L"nvapi64.dll") == 0 ||
           _wcsicmp(base, L"nvngx.dll") == 0 ||
           _wcsicmp(base, L"nvngx_dlss.dll") == 0 ||
           _wcsicmp(base, L"nvngx_dlssd.dll") == 0 ||
           _wcsicmp(base, L"sl.interposer.dll") == 0 ||
           _wcsicmp(base, L"sl.common.dll") == 0 ||
           _wcsicmp(base, L"sl.dlss.dll") == 0 ||
           _wcsicmp(base, L"sl.dlss_d.dll") == 0 ||
           _wcsicmp(base, L"vulkan-1.dll") == 0;
}


const wchar_t* requestBaseNameW(LPCWSTR name)
{
    if (name == nullptr) return nullptr;
    const wchar_t* slash = std::wcsrchr(name, L'\\');
    return slash != nullptr ? slash + 1 : name;
}

const char* requestBaseNameA(LPCSTR name)
{
    if (name == nullptr) return nullptr;
    const char* slash = std::strrchr(name, '\\');
    return slash != nullptr ? slash + 1 : name;
}

bool requestHasExplicitPathW(LPCWSTR name)
{
    if (name == nullptr) return false;
    return std::wcschr(name, L'\\') != nullptr || std::wcschr(name, L'/') != nullptr || std::wcschr(name, L':') != nullptr;
}

bool requestHasExplicitPathA(LPCSTR name)
{
    if (name == nullptr) return false;
    return std::strchr(name, '\\') != nullptr || std::strchr(name, '/') != nullptr || std::strchr(name, ':') != nullptr;
}

std::wstring localSiblingPath(const wchar_t* baseName)
{
    if (baseName == nullptr || *baseName == L'\0') return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (slash == nullptr) return {};
    *(slash + 1) = L'\0';
    std::wstring out(path);
    out += baseName;
    return out;
}


const wchar_t* localRedirectTargetForBaseW(const wchar_t* base)
{
    if (base == nullptr) return nullptr;
    if (_wcsicmp(base, L"nvapi64.dll") == 0) return L"nvapi64.dll";
    if (_wcsicmp(base, L"nvngx.dll") == 0) return L"nvngx.dll";
    if (_wcsicmp(base, L"nvngx_dlss.dll") == 0) return L"nvngx_dlss.dll";
    if (_wcsicmp(base, L"nvngx_dlssd.dll") == 0) return L"nvngx_dlss.dll";
    return nullptr;
}

const char* redirectComponentForBaseW(const wchar_t* base)
{
    if (base == nullptr) return "loader";
    if (_wcsicmp(base, L"nvapi64.dll") == 0) return "nvapi";
    if (_wcsicmp(base, L"nvngx.dll") == 0 || _wcsicmp(base, L"nvngx_dlss.dll") == 0 ||
        _wcsicmp(base, L"nvngx_dlssd.dll") == 0) return "ngx";
    return "loader";
}

bool explicitPathShouldStillRedirectW(LPCWSTR requestedName)
{
    if (!requestHasExplicitPathW(requestedName)) return true;
    const wchar_t* base = requestBaseNameW(requestedName);
    if (localRedirectTargetForBaseW(base) == nullptr) return false;

    wchar_t sys[MAX_PATH] = {};
    const UINT sysLen = GetSystemDirectoryW(sys, MAX_PATH);
    if (sysLen > 0 && sysLen < MAX_PATH) {
        std::wstring sysPrefix(sys);
        if (!sysPrefix.empty() && sysPrefix.back() != L'\\') sysPrefix += L'\\';
        const std::wstring req(requestedName);
        if (req.size() >= sysPrefix.size() && _wcsnicmp(req.c_str(), sysPrefix.c_str(), sysPrefix.size()) == 0) {
            return true;
        }
    }
    return false;
}

HMODULE loadLocalFrontendFallbackW(LPCWSTR requestedName, DWORD originalFlags, const char* source)
{
    if (requestedName == nullptr) return nullptr;
    const wchar_t* base = requestBaseNameW(requestedName);
    const wchar_t* target = localRedirectTargetForBaseW(base);
    if (target == nullptr) return nullptr;
    if (!explicitPathShouldStillRedirectW(requestedName)) return nullptr;

    const std::wstring localPath = localSiblingPath(target);
    if (localPath.empty()) return nullptr;

    HMODULE module = nullptr;
    if (g_loadLibraryExW != nullptr) {
        module = g_loadLibraryExW(localPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    } else if (g_loadLibraryW != nullptr) {
        module = g_loadLibraryW(localPath.c_str());
    }

    const std::string path = apusr::host_discovery::narrowBestEffort(localPath.c_str());
    const std::string req = apusr::host_discovery::narrowBestEffort(requestedName);
    const char* component = redirectComponentForBaseW(base);
    apusr::host_discovery::eventf(component, "local_redirect",
                                  "source=%s request=%s original_flags=0x%08lX path=%s result=%p",
                                  source != nullptr ? source : "<unknown>", req.c_str(),
                                  static_cast<unsigned long>(originalFlags), path.c_str(), module);
    return module;
}

HMODULE loadLocalFrontendFallbackA(LPCSTR requestedName, DWORD originalFlags, const char* source)
{
    if (requestedName == nullptr) return nullptr;
    wchar_t wide[MAX_PATH] = {};
    const int n = MultiByteToWideChar(CP_ACP, 0, requestedName, -1, wide, MAX_PATH);
    if (n <= 0) return nullptr;
    return loadLocalFrontendFallbackW(wide, originalFlags, source);
}

void logRelevantModuleSeen(HMODULE module)
{
    if (module == nullptr) return;
    const wchar_t* name = moduleBaseName(module);
    if (name == nullptr) return;
    const char* component = nullptr;
    if (_wcsicmp(name, L"nvapi64.dll") == 0) component = "nvapi";
    else if (_wcsicmp(name, L"nvngx.dll") == 0 || _wcsicmp(name, L"nvngx_dlss.dll") == 0 || _wcsicmp(name, L"nvngx_dlssd.dll") == 0) component = "ngx";
    else if (_wcsicmp(name, L"sl.interposer.dll") == 0 || _wcsicmp(name, L"sl.common.dll") == 0 ||
             _wcsicmp(name, L"sl.dlss.dll") == 0 || _wcsicmp(name, L"sl.dlss_d.dll") == 0) component = "streamline";
    if (component != nullptr) {
        apusr::host_discovery::moduleLoaded(component, module);
    }
}

std::wstring replaceAll(std::wstring input, const wchar_t* from, const wchar_t* to)
{
    const std::wstring needle(from);
    const std::wstring repl(to);
    std::size_t pos = 0;
    while ((pos = input.find(needle, pos)) != std::wstring::npos) {
        input.replace(pos, needle.size(), repl);
        pos += repl.size();
    }
    return input;
}

std::string replaceAll(std::string input, const char* from, const char* to)
{
    const std::string needle(from);
    const std::string repl(to);
    std::size_t pos = 0;
    while ((pos = input.find(needle, pos)) != std::string::npos) {
        input.replace(pos, needle.size(), repl);
        pos += repl.size();
    }
    return input;
}

std::wstring replaceVendorDeviceTokens(std::wstring value)
{
    value = replaceAll(std::move(value), L"VEN_1002", kSpoofVendorTokenW);
    value = replaceAll(std::move(value), L"VEN_8086", kSpoofVendorTokenW);
    std::size_t pos = 0;
    while ((pos = value.find(L"DEV_", pos)) != std::wstring::npos) {
        if (pos + 8 <= value.size()) {
            value.replace(pos, 8, kSpoofDeviceTokenW);
            pos += 8;
        } else {
            break;
        }
    }
    return value;
}

std::string replaceVendorDeviceTokens(std::string value)
{
    value = replaceAll(std::move(value), "VEN_1002", kSpoofVendorTokenA);
    value = replaceAll(std::move(value), "VEN_8086", kSpoofVendorTokenA);
    std::size_t pos = 0;
    while ((pos = value.find("DEV_", pos)) != std::string::npos) {
        if (pos + 8 <= value.size()) {
            value.replace(pos, 8, kSpoofDeviceTokenA);
            pos += 8;
        } else {
            break;
        }
    }
    return value;
}

LSTATUS writeRegSzW(LPBYTE data, LPDWORD dataBytes, LPDWORD type, const wchar_t* value)
{
    if (dataBytes == nullptr) return ERROR_INVALID_PARAMETER;
    const DWORD required = static_cast<DWORD>((std::wcslen(value) + 1) * sizeof(wchar_t));
    if (data == nullptr) {
        *dataBytes = required;
        if (type) *type = REG_SZ;
        return ERROR_SUCCESS;
    }
    if (*dataBytes < required) {
        *dataBytes = required;
        if (type) *type = REG_SZ;
        return ERROR_MORE_DATA;
    }
    std::memcpy(data, value, required);
    *dataBytes = required;
    if (type) *type = REG_SZ;
    return ERROR_SUCCESS;
}

LSTATUS writeRegSzA(LPBYTE data, LPDWORD dataBytes, LPDWORD type, const char* value)
{
    if (dataBytes == nullptr) return ERROR_INVALID_PARAMETER;
    const DWORD required = static_cast<DWORD>(std::strlen(value) + 1);
    if (data == nullptr) {
        *dataBytes = required;
        if (type) *type = REG_SZ;
        return ERROR_SUCCESS;
    }
    if (*dataBytes < required) {
        *dataBytes = required;
        if (type) *type = REG_SZ;
        return ERROR_MORE_DATA;
    }
    std::memcpy(data, value, required);
    *dataBytes = required;
    if (type) *type = REG_SZ;
    return ERROR_SUCCESS;
}

bool isRegValueNameW(LPCWSTR name, const wchar_t* expected)
{
    return name != nullptr && _wcsicmp(name, expected) == 0;
}

bool isRegValueNameA(LPCSTR name, const char* expected)
{
    return name != nullptr && _stricmp(name, expected) == 0;
}

bool containsIgnoreCaseW(LPCWSTR haystack, LPCWSTR needle)
{
    if (haystack == nullptr || needle == nullptr || *needle == L'\0') return false;
    const std::size_t needleLen = std::wcslen(needle);
    for (const wchar_t* p = haystack; *p != L'\0'; ++p) {
        if (_wcsnicmp(p, needle, needleLen) == 0) return true;
    }
    return false;
}

bool isSpoofedNvidiaRegistrySubkey(LPCWSTR subKey)
{
    return containsIgnoreCaseW(subKey, L"NVIDIA Corporation") ||
           containsIgnoreCaseW(subKey, L"nvlddmkm") ||
           containsIgnoreCaseW(subKey, L"VEN_10DE");
}

bool isSpoofRegistryHandle(HKEY hKey)
{
    if (hKey == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_spoofRegistryMutex);
    return g_spoofRegistryHandles.find(hKey) != g_spoofRegistryHandles.end();
}

void rememberSpoofRegistryHandle(HKEY hKey)
{
    if (hKey == nullptr) return;
    std::lock_guard<std::mutex> lock(g_spoofRegistryMutex);
    g_spoofRegistryHandles.insert(hKey);
}

bool forgetSpoofRegistryHandle(HKEY hKey)
{
    if (hKey == nullptr) return false;
    std::lock_guard<std::mutex> lock(g_spoofRegistryMutex);
    return g_spoofRegistryHandles.erase(hKey) != 0;
}

LSTATUS openSpoofRegistryBackingKey(PHKEY outKey)
{
    if (outKey == nullptr) return ERROR_INVALID_PARAMETER;
    *outKey = nullptr;
    if (g_regOpenKeyExW == nullptr) return ERROR_CALL_NOT_IMPLEMENTED;

    // Never hand the host a fabricated HKEY.  Some registry paths call ADVAPI/RPC
    // APIs that we do not and should not hook.  A real harmless backing key keeps
    // those unhooked paths safe while our hooked query/enumeration calls still
    // synthesize NVIDIA-like values.
    static constexpr LPCWSTR kBackingKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    return g_regOpenKeyExW(HKEY_LOCAL_MACHINE, kBackingKey, 0, KEY_READ, outKey);
}

LSTATUS writeRegBinaryOne(LPBYTE data, LPDWORD dataBytes, LPDWORD type)
{
    if (dataBytes == nullptr) return ERROR_INVALID_PARAMETER;
    if (data == nullptr) {
        *dataBytes = 1;
        if (type) *type = REG_BINARY;
        return ERROR_SUCCESS;
    }
    if (*dataBytes < 1) {
        *dataBytes = 1;
        if (type) *type = REG_BINARY;
        return ERROR_MORE_DATA;
    }
    data[0] = 1;
    *dataBytes = 1;
    if (type) *type = REG_BINARY;
    return ERROR_SUCCESS;
}

void spoofDisplayDeviceA(PDISPLAY_DEVICEA device)
{
    if (device == nullptr) return;
    std::strncpy(device->DeviceString, kSpoofGpuNameA, sizeof(device->DeviceString) - 1);
    device->DeviceString[sizeof(device->DeviceString) - 1] = '\0';
    std::string id(device->DeviceID);
    const std::string patched = replaceVendorDeviceTokens(id);
    if (patched != id) {
        std::strncpy(device->DeviceID, patched.c_str(), sizeof(device->DeviceID) - 1);
        device->DeviceID[sizeof(device->DeviceID) - 1] = '\0';
    }
}

void spoofDisplayDeviceW(PDISPLAY_DEVICEW device)
{
    if (device == nullptr) return;
    wcsncpy_s(device->DeviceString, kSpoofGpuNameW, _TRUNCATE);
    std::wstring id(device->DeviceID);
    const std::wstring patched = replaceVendorDeviceTokens(id);
    if (patched != id) {
        wcsncpy_s(device->DeviceID, patched.c_str(), _TRUNCATE);
    }
}

bool shouldSpoofGlobalD3dIdentityNow() noexcept
{
    // The process may load DXGI and enumerate adapters before Streamline has
    // selected a renderer. Spoofing OS-wide adapter identity at that point
    // poisons Vulkan engines that later correlate cached DXGI/KMT identity with
    // the real VkPhysicalDevice. Keep these broad Win32 identity hooks dormant
    // until the authoritative Streamline renderer is explicitly D3D12. Vulkan
    // uses a narrow Streamline-only DXGI adapter view instead.
    return currentStreamlineRenderApi() == kSlRenderApiD3d12;
}

BOOL WINAPI hkEnumDisplayDevicesA(LPCSTR lpDevice, DWORD iDevNum, PDISPLAY_DEVICEA lpDisplayDevice, DWORD dwFlags)
{
    const BOOL result = g_enumDisplayDevicesA ? g_enumDisplayDevicesA(lpDevice, iDevNum, lpDisplayDevice, dwFlags) : FALSE;
    if (result && shouldSpoofGlobalD3dIdentityNow()) {
        bool expected = false;
        if (g_loggedDisplaySpoof.compare_exchange_strong(expected, true)) {
            apusr::host_discovery::event("identity", "EnumDisplayDevices.spoofed", "scope=d3d12");
        }
        spoofDisplayDeviceA(lpDisplayDevice);
    }
    return result;
}

BOOL WINAPI hkEnumDisplayDevicesW(LPCWSTR lpDevice, DWORD iDevNum, PDISPLAY_DEVICEW lpDisplayDevice, DWORD dwFlags)
{
    const BOOL result = g_enumDisplayDevicesW ? g_enumDisplayDevicesW(lpDevice, iDevNum, lpDisplayDevice, dwFlags) : FALSE;
    if (result && shouldSpoofGlobalD3dIdentityNow()) {
        bool expected = false;
        if (g_loggedDisplaySpoof.compare_exchange_strong(expected, true)) {
            apusr::host_discovery::event("identity", "EnumDisplayDevices.spoofed", "scope=d3d12");
        }
        spoofDisplayDeviceW(lpDisplayDevice);
    }
    return result;
}

LSTATUS WINAPI hkRegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
{
    if (lpSubKey != nullptr && phkResult != nullptr && isSpoofedNvidiaRegistrySubkey(lpSubKey)) {
        HKEY backing = nullptr;
        const LSTATUS status = openSpoofRegistryBackingKey(&backing);
        if (status != ERROR_SUCCESS || backing == nullptr) {
            return status;
        }
        rememberSpoofRegistryHandle(backing);
        bool expected = false;
        if (g_loggedRegistrySpoof.compare_exchange_strong(expected, true)) {
            const std::string subkey = apusr::host_discovery::narrowBestEffort(lpSubKey);
            apusr::host_discovery::eventf("identity", "registry.nvidia_key.spoofed", "subkey=%s", subkey.c_str());
        }
        *phkResult = backing;
        return ERROR_SUCCESS;
    }
    return g_regOpenKeyExW ? g_regOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult) : ERROR_CALL_NOT_IMPLEMENTED;
}

LSTATUS WINAPI hkRegCloseKey(HKEY hKey)
{
    const bool spoofHandle = forgetSpoofRegistryHandle(hKey);
    (void)spoofHandle;
    return g_regCloseKey ? g_regCloseKey(hKey) : ERROR_SUCCESS;
}

LSTATUS WINAPI hkRegEnumValueW(HKEY hKey, DWORD index, LPWSTR valueName, LPDWORD valueNameChars, LPDWORD reserved,
                               LPDWORD type, LPBYTE data, LPDWORD dataBytes)
{
    (void)reserved;
    if (isSpoofRegistryHandle(hKey)) {
        if (index != 0) return ERROR_NO_MORE_ITEMS;
        constexpr wchar_t kSigName[] = L"{41FCC608-8496-4DEF-B43E-7D9BD675A6FF}";
        const DWORD nameLen = static_cast<DWORD>(std::wcslen(kSigName));
        if (valueNameChars == nullptr) return ERROR_INVALID_PARAMETER;
        if (valueName == nullptr || *valueNameChars <= nameLen) {
            *valueNameChars = nameLen + 1;
            return ERROR_MORE_DATA;
        }
        std::wmemcpy(valueName, kSigName, nameLen + 1);
        *valueNameChars = nameLen;
        if (type) *type = REG_BINARY;
        if (dataBytes != nullptr) {
            if (data == nullptr || *dataBytes < 1) {
                *dataBytes = 1;
                return data == nullptr ? ERROR_SUCCESS : ERROR_MORE_DATA;
            }
            data[0] = 1;
            *dataBytes = 1;
        }
        return ERROR_SUCCESS;
    }
    return g_regEnumValueW ? g_regEnumValueW(hKey, index, valueName, valueNameChars, reserved, type, data, dataBytes)
                           : ERROR_CALL_NOT_IMPLEMENTED;
}

LSTATUS spoofKnownRegValueW(LPCWSTR valueName, LPBYTE data, LPDWORD dataBytes, LPDWORD type)
{
    if (isRegValueNameW(valueName, L"DriverVersion")) {
        return writeRegSzW(data, dataBytes, type, kSpoofDriverVersionW);
    }
    if (isRegValueNameW(valueName, L"DriverDesc") ||
        isRegValueNameW(valueName, L"HardwareInformation.AdapterString")) {
        return writeRegSzW(data, dataBytes, type, kSpoofGpuNameW);
    }
    if (isRegValueNameW(valueName, L"ProviderName")) {
        return writeRegSzW(data, dataBytes, type, kSpoofProviderW);
    }
    if (isRegValueNameW(valueName, L"HwSchMode")) {
        if (dataBytes == nullptr) return ERROR_INVALID_PARAMETER;
        if (data == nullptr) {
            *dataBytes = sizeof(DWORD);
            if (type) *type = REG_DWORD;
            return ERROR_SUCCESS;
        }
        if (*dataBytes < sizeof(DWORD)) {
            *dataBytes = sizeof(DWORD);
            if (type) *type = REG_DWORD;
            return ERROR_MORE_DATA;
        }
        *reinterpret_cast<DWORD*>(data) = 2u;
        *dataBytes = sizeof(DWORD);
        if (type) *type = REG_DWORD;
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}

LSTATUS spoofKnownRegValueA(LPCSTR valueName, LPBYTE data, LPDWORD dataBytes, LPDWORD type)
{
    if (isRegValueNameA(valueName, "DriverVersion")) {
        return writeRegSzA(data, dataBytes, type, kSpoofDriverVersionA);
    }
    if (isRegValueNameA(valueName, "DriverDesc") ||
        isRegValueNameA(valueName, "HardwareInformation.AdapterString")) {
        return writeRegSzA(data, dataBytes, type, kSpoofGpuNameA);
    }
    if (isRegValueNameA(valueName, "ProviderName")) {
        return writeRegSzA(data, dataBytes, type, kSpoofProviderA);
    }
    return ERROR_FILE_NOT_FOUND;
}

void replaceRegBufferTokensW(LPCWSTR valueName, LPBYTE data, LPDWORD dataBytes, LPDWORD type)
{
    if (valueName == nullptr || data == nullptr || dataBytes == nullptr || *dataBytes < sizeof(wchar_t)) return;
    const DWORD regType = type != nullptr ? *type : REG_NONE;
    const bool interesting = isRegValueNameW(valueName, L"HardwareID") ||
                             isRegValueNameW(valueName, L"MatchingDeviceId") ||
                             (std::wcslen(valueName) >= 13 && _wcsnicmp(valueName, L"\\Device\\Video", 13) == 0);
    if (!interesting) return;

    if (regType == REG_MULTI_SZ) {
        wchar_t* text = reinterpret_cast<wchar_t*>(data);
        const DWORD chars = *dataBytes / sizeof(wchar_t);
        DWORD off = 0;
        while (off < chars && text[off] != L'\0') {
            std::wstring segment(text + off);
            std::wstring patched = replaceVendorDeviceTokens(segment);
            if (patched.size() <= segment.size()) {
                std::wmemcpy(text + off, patched.c_str(), patched.size() + 1);
            }
            off += static_cast<DWORD>(segment.size()) + 1;
        }
    } else if (regType == REG_SZ || regType == REG_EXPAND_SZ || regType == REG_NONE) {
        std::wstring value(reinterpret_cast<wchar_t*>(data), *dataBytes / sizeof(wchar_t));
        std::wstring patched = replaceVendorDeviceTokens(value);
        const DWORD bytes = static_cast<DWORD>((patched.size() + 1) * sizeof(wchar_t));
        if (bytes <= *dataBytes) {
            std::memcpy(data, patched.c_str(), bytes);
            *dataBytes = bytes;
        }
    }
}

void replaceRegBufferTokensA(LPCSTR valueName, LPBYTE data, LPDWORD dataBytes, LPDWORD type)
{
    if (valueName == nullptr || data == nullptr || dataBytes == nullptr || *dataBytes == 0) return;
    const DWORD regType = type != nullptr ? *type : REG_NONE;
    const bool interesting = isRegValueNameA(valueName, "HardwareID") ||
                             isRegValueNameA(valueName, "MatchingDeviceId") ||
                             (std::strlen(valueName) >= 13 && _strnicmp(valueName, "\\Device\\Video", 13) == 0);
    if (!interesting) return;

    if (regType == REG_MULTI_SZ) {
        char* text = reinterpret_cast<char*>(data);
        DWORD off = 0;
        while (off < *dataBytes && text[off] != '\0') {
            std::string segment(text + off);
            std::string patched = replaceVendorDeviceTokens(segment);
            if (patched.size() <= segment.size()) {
                std::memcpy(text + off, patched.c_str(), patched.size() + 1);
            }
            off += static_cast<DWORD>(segment.size()) + 1;
        }
    } else if (regType == REG_SZ || regType == REG_EXPAND_SZ || regType == REG_NONE) {
        std::string value(reinterpret_cast<char*>(data), *dataBytes);
        std::string patched = replaceVendorDeviceTokens(value);
        const DWORD bytes = static_cast<DWORD>(patched.size() + 1);
        if (bytes <= *dataBytes) {
            std::memcpy(data, patched.c_str(), bytes);
            *dataBytes = bytes;
        }
    }
}

LSTATUS WINAPI hkRegQueryValueExW(HKEY hKey, LPCWSTR valueName, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD dataBytes)
{
    if (isSpoofRegistryHandle(hKey)) {
        const LSTATUS spoof = spoofKnownRegValueW(valueName, data, dataBytes, type);
        return spoof == ERROR_FILE_NOT_FOUND ? writeRegBinaryOne(data, dataBytes, type) : spoof;
    }

    const DWORD oldSize = dataBytes != nullptr ? *dataBytes : 0;
    LSTATUS result = g_regQueryValueExW ? g_regQueryValueExW(hKey, valueName, reserved, type, data, dataBytes)
                                        : ERROR_CALL_NOT_IMPLEMENTED;

    const LSTATUS directSpoof = spoofKnownRegValueW(valueName, data, dataBytes, type);
    if (directSpoof != ERROR_FILE_NOT_FOUND) {
        return directSpoof;
    }

    if (result == ERROR_SUCCESS) {
        if (dataBytes != nullptr && oldSize != 0 && *dataBytes > oldSize) *dataBytes = oldSize;
        replaceRegBufferTokensW(valueName, data, dataBytes, type);
    }
    return result;
}

LSTATUS WINAPI hkRegQueryValueExA(HKEY hKey, LPCSTR valueName, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD dataBytes)
{
    if (isSpoofRegistryHandle(hKey)) {
        const LSTATUS spoof = spoofKnownRegValueA(valueName, data, dataBytes, type);
        return spoof == ERROR_FILE_NOT_FOUND ? writeRegBinaryOne(data, dataBytes, type) : spoof;
    }

    const DWORD oldSize = dataBytes != nullptr ? *dataBytes : 0;
    LSTATUS result = g_regQueryValueExA ? g_regQueryValueExA(hKey, valueName, reserved, type, data, dataBytes)
                                        : ERROR_CALL_NOT_IMPLEMENTED;

    const LSTATUS directSpoof = spoofKnownRegValueA(valueName, data, dataBytes, type);
    if (directSpoof != ERROR_FILE_NOT_FOUND) {
        return directSpoof;
    }

    if (result == ERROR_SUCCESS) {
        if (dataBytes != nullptr && oldSize != 0 && *dataBytes > oldSize) *dataBytes = oldSize;
        replaceRegBufferTokensA(valueName, data, dataBytes, type);
    }
    return result;
}

LSTATUS WINAPI hkRegGetValueW(HKEY hKey,
                              LPCWSTR subKey,
                              LPCWSTR valueName,
                              DWORD flags,
                              LPDWORD type,
                              PVOID data,
                              LPDWORD dataBytes)
{
    if (isSpoofedNvidiaRegistrySubkey(subKey)) {
        const LSTATUS directSpoof = spoofKnownRegValueW(valueName,
                                                        static_cast<LPBYTE>(data),
                                                        dataBytes,
                                                        type);
        if (directSpoof != ERROR_FILE_NOT_FOUND) return directSpoof;
        return writeRegBinaryOne(static_cast<LPBYTE>(data), dataBytes, type);
    }

    const DWORD oldSize = dataBytes != nullptr ? *dataBytes : 0;
    LSTATUS result = g_regGetValueW ? g_regGetValueW(hKey, subKey, valueName, flags, type, data, dataBytes)
                                    : ERROR_CALL_NOT_IMPLEMENTED;

    const LSTATUS directSpoof = spoofKnownRegValueW(valueName,
                                                    static_cast<LPBYTE>(data),
                                                    dataBytes,
                                                    type);
    if (directSpoof != ERROR_FILE_NOT_FOUND) {
        return directSpoof;
    }

    if (result == ERROR_SUCCESS) {
        if (dataBytes != nullptr && oldSize != 0 && *dataBytes > oldSize) *dataBytes = oldSize;
        replaceRegBufferTokensW(valueName, static_cast<LPBYTE>(data), dataBytes, type);
    }
    return result;
}

bool moduleLooksLikeGdi32(HMODULE module)
{
    if (module == nullptr) return false;
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;
    return _wcsicmp(name, L"gdi32.dll") == 0;
}

void patchAsciiIdentityBuffer(char* data, std::size_t bytes)
{
    if (data == nullptr || bytes == 0) return;
    for (std::size_t i = 0; i + 8 <= bytes; ++i) {
        if (std::memcmp(data + i, "VEN_1002", 8) == 0 || std::memcmp(data + i, "VEN_8086", 8) == 0) {
            std::memcpy(data + i, kSpoofVendorTokenA, 8);
        }
        if (std::memcmp(data + i, "DEV_", 4) == 0) {
            std::memcpy(data + i, kSpoofDeviceTokenA, 8);
        }
    }

    const std::size_t spoofLen = std::strlen(kSpoofGpuNameA);
    for (std::size_t i = 0; i < bytes; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c < 0x20 || c > 0x7e) continue;
        std::size_t end = i;
        while (end < bytes) {
            const unsigned char ch = static_cast<unsigned char>(data[end]);
            if (ch == 0 || ch < 0x20 || ch > 0x7e) break;
            ++end;
        }
        const std::size_t len = end - i;
        if (len >= 3) {
            std::string_view segment(data + i, len);
            const bool gpuLike = segment.find("AMD") != std::string_view::npos ||
                                 segment.find("Radeon") != std::string_view::npos ||
                                 segment.find("Advanced Micro Devices") != std::string_view::npos ||
                                 segment.find("ATI") != std::string_view::npos;
            if (gpuLike && len >= spoofLen) {
                std::memset(data + i, 0, len);
                std::memcpy(data + i, kSpoofGpuNameA, spoofLen);
            }
        }
        i = end;
    }
}

void patchWideIdentityBuffer(wchar_t* data, std::size_t chars)
{
    if (data == nullptr || chars == 0) return;
    const std::size_t vendorLen = std::wcslen(kSpoofVendorTokenW);
    const std::size_t deviceLen = std::wcslen(kSpoofDeviceTokenW);
    for (std::size_t i = 0; i + 8 <= chars; ++i) {
        if (std::wmemcmp(data + i, L"VEN_1002", 8) == 0 || std::wmemcmp(data + i, L"VEN_8086", 8) == 0) {
            std::wmemcpy(data + i, kSpoofVendorTokenW, vendorLen);
        }
        if (std::wmemcmp(data + i, L"DEV_", 4) == 0) {
            std::wmemcpy(data + i, kSpoofDeviceTokenW, deviceLen);
        }
    }

    const std::size_t spoofLen = std::wcslen(kSpoofGpuNameW);
    for (std::size_t i = 0; i < chars; ++i) {
        const wchar_t c = data[i];
        if (c < 0x20 || c > 0x7e) continue;
        std::size_t end = i;
        while (end < chars) {
            const wchar_t ch = data[end];
            if (ch == 0 || ch < 0x20 || ch > 0x7e) break;
            ++end;
        }
        const std::size_t len = end - i;
        if (len >= 3) {
            std::wstring_view segment(data + i, len);
            const bool gpuLike = segment.find(L"AMD") != std::wstring_view::npos ||
                                 segment.find(L"Radeon") != std::wstring_view::npos ||
                                 segment.find(L"Advanced Micro Devices") != std::wstring_view::npos ||
                                 segment.find(L"ATI") != std::wstring_view::npos;
            if (gpuLike && len >= spoofLen) {
                std::wmemset(data + i, 0, len);
                std::wmemcpy(data + i, kSpoofGpuNameW, spoofLen);
            }
        }
        i = end;
    }
}

void spoofKmtPrivateData(void* data, std::uint32_t bytes)
{
    if (data == nullptr || bytes == 0 || bytes > (1u << 20)) return;
    auto* raw = static_cast<unsigned char*>(data);
    patchAsciiIdentityBuffer(reinterpret_cast<char*>(raw), bytes);
    patchWideIdentityBuffer(reinterpret_cast<wchar_t*>(raw), bytes / sizeof(wchar_t));

    // KMT adapter/private data often carries raw vendor IDs.  Rewrite only the two
    // known non-NVIDIA PCI vendor IDs we support, and leave all other fields intact.
    for (std::uint32_t off = 0; off + sizeof(std::uint32_t) <= bytes; off += sizeof(std::uint32_t)) {
        auto* word = reinterpret_cast<std::uint32_t*>(raw + off);
        if (*word == 0x1002u || *word == 0x8086u) {
            *word = 0x10DEu;
        }
    }
}

LONG WINAPI hkD3DKMTQueryAdapterInfo(void* data)
{
    LONG result = g_d3dkmtQueryAdapterInfo ? g_d3dkmtQueryAdapterInfo(data) : static_cast<LONG>(0xC0000002u);
    auto* query = reinterpret_cast<KmtQueryAdapterInfoLoose*>(data);
    if (result >= 0 && shouldSpoofGlobalD3dIdentityNow() &&
        query != nullptr && query->privateDriverData != nullptr && query->privateDriverDataSize != 0) {
        bool expected = false;
        if (g_loggedKmtQuery.compare_exchange_strong(expected, true)) {
            apusr::host_discovery::eventf("identity", "D3DKMTQueryAdapterInfo.spoofed",
                                         "scope=d3d12 type=%u bytes=%u", query->type, query->privateDriverDataSize);
        }
        spoofKmtPrivateData(query->privateDriverData, query->privateDriverDataSize);
    }
    return result;
}

LONG WINAPI hkD3DKMTEnumAdapters(void* data)
{
    return g_d3dkmtEnumAdapters ? g_d3dkmtEnumAdapters(data) : static_cast<LONG>(0xC0000002u);
}

LONG WINAPI hkD3DKMTEnumAdapters2(void* data)
{
    return g_d3dkmtEnumAdapters2 ? g_d3dkmtEnumAdapters2(data) : static_cast<LONG>(0xC0000002u);
}

LONG WINAPI hkD3DKMTEnumAdapters3(void* data)
{
    return g_d3dkmtEnumAdapters3 ? g_d3dkmtEnumAdapters3(data) : static_cast<LONG>(0xC0000002u);
}

LONG WINAPI hkD3DKMTCloseAdapter(void* data)
{
    return g_d3dkmtCloseAdapter ? g_d3dkmtCloseAdapter(data) : 0;
}

bool apusrShouldLogPeriodic(std::atomic<std::uint64_t>& counter, const std::uint64_t early = 16, const std::uint64_t period = 300)
{
    if (!apusr::host_discovery::enabled()) {
        return false;
    }
    const std::uint64_t index = counter.fetch_add(1, std::memory_order_relaxed) + 1u;
    return index <= early || (period != 0 && (index % period) == 0);
}

ApusrCapturedConstants apusrCapturedConstantsFromSl(const SlConstants& constants)
{
    ApusrCapturedConstants out;
    out.valid = true;
    out.jitterX = constants.jitterOffset.x;
    out.jitterY = constants.jitterOffset.y;
    out.mvScaleX = constants.mvecScale.x;
    out.mvScaleY = constants.mvecScale.y;
    if (out.mvScaleX == 0.0f) out.mvScaleX = 1.0f;
    if (out.mvScaleY == 0.0f) out.mvScaleY = 1.0f;
    out.reset = constants.reset != 0;
    out.depthInverted = constants.depthInverted != 0;
    out.motionVectorsJittered = constants.motionVectorsJittered != 0;
    out.cameraMotionIncluded = constants.cameraMotionIncluded != 0;
    return out;
}

void apusrMergeConstants(ApusrCapturedConstants& dst, const ApusrCapturedConstants& src)
{
    if (src.valid) dst = src;
}

void apusrCaptureConstantsForKey(ApusrStreamlineKey key, const SlConstants& constants)
{
    const ApusrCapturedConstants captured = apusrCapturedConstantsFromSl(constants);
    {
        std::lock_guard<std::mutex> lock(g_apusrStreamlineMutex);
        if (key.frame != nullptr) {
            g_apusrConstantsByFrameViewport[key] = captured;
            if (key.viewport != 0u) g_apusrConstantsByFrameViewport[ApusrStreamlineKey{key.frame, 0u}] = captured;
        } else {
            g_apusrConstantsByViewport[key.viewport] = captured;
            if (key.viewport != 0u) g_apusrConstantsByViewport[0u] = captured;
        }
    }
    if (apusrShouldLogPeriodic(g_apusrConstantsLogCounter, 4, 1200)) {
        apusr::host_discovery::eventf("streamline", "core.constants.captured",
                                      "frame=%p viewport=%u jitter=(%.6f,%.6f) mv_scale=(%.6f,%.6f) reset=%d depth_inverted=%d mv_jittered=%d camera_motion=%d",
                                      key.frame, key.viewport, captured.jitterX, captured.jitterY, captured.mvScaleX, captured.mvScaleY,
                                      captured.reset ? 1 : 0, captured.depthInverted ? 1 : 0,
                                      captured.motionVectorsJittered ? 1 : 0, captured.cameraMotionIncluded ? 1 : 0);
    }
}

void apusrCaptureConstantsFromInputs(ApusrCapturedConstants& dst, const void* const* inputs, std::uint32_t numInputs)
{
    if (inputs == nullptr) return;
    for (std::uint32_t i = 0; i < numInputs; ++i) {
        const auto* base = static_cast<const SlBaseStructure*>(inputs[i]);
        if (base != nullptr && base->structType.data1 == kStructConstants) {
            apusrMergeConstants(dst, apusrCapturedConstantsFromSl(*reinterpret_cast<const SlConstants*>(base)));
        }
    }
}

constexpr std::uint32_t kNgxDlssFeatureCreateFlagMvLowRes = 1u << 1u;
constexpr std::uint32_t kNgxDlssFeatureCreateFlagMvJittered = 1u << 2u;
constexpr std::uint32_t kNgxDlssFeatureCreateFlagDepthInverted = 1u << 3u;
constexpr std::uint32_t kNgxDlssFeatureCreateFlagAutoExposure = 1u << 6u;

std::uint32_t apusrCreateFlagsFromStreamlineContract(const ApusrCapturedTags& tags,
                                                     const ApusrCapturedOptions& options,
                                                     const ApusrCapturedConstants& constants,
                                                     std::uint32_t renderWidth,
                                                     std::uint32_t renderHeight)
{
    std::uint32_t flags = 0u;

    // Match the stock Streamline DLSS contract: motion vectors are assumed low-res unless the
    // tagged MV buffer is larger than the color input extent. Omitting this flag makes
    // the NGX/APUSR facade treat low-resolution host motion vectors as display-resolution vectors.
    bool motionVectorsAreDisplayRes = false;
    if (tags.motionVectors.width != 0u && tags.motionVectors.height != 0u &&
        renderWidth != 0u && renderHeight != 0u) {
        motionVectorsAreDisplayRes = tags.motionVectors.width > renderWidth || tags.motionVectors.height > renderHeight;
    }
    if (!motionVectorsAreDisplayRes) flags |= kNgxDlssFeatureCreateFlagMvLowRes;

    if (constants.motionVectorsJittered) flags |= kNgxDlssFeatureCreateFlagMvJittered;
    if (constants.depthInverted) flags |= kNgxDlssFeatureCreateFlagDepthInverted;

    // If no exposure resource is tagged, use the frontend's auto-exposure path even if the host did
    // not explicitly set the newer Streamline auto-exposure option.
    if (options.useAutoExposure || tags.exposure.native == nullptr) flags |= kNgxDlssFeatureCreateFlagAutoExposure;
    return flags;
}

float apusrNgxMotionVectorScale(float streamlineScale, std::uint32_t renderExtent) noexcept
{
    // Streamline's normalized scale can legitimately be negative. Some integrations use
    // approximately -1/renderExtent on both axes; preserving that sign yields
    // the NGX pixel-space scale of -1 rather than silently reversing motion.
    if (!std::isfinite(streamlineScale) || streamlineScale == 0.0f || renderExtent == 0u) return 1.0f;
    return streamlineScale * static_cast<float>(renderExtent);
}

float apusrHostDlssScale(SlDLSSMode mode)
{
    switch (mode) {
    case SlDLSSMode::eUltraPerformance:
        return 1.0f / 3.0f;
    case SlDLSSMode::eMaxPerformance:
        return 0.5f;
    case SlDLSSMode::eBalanced:
        return 0.58f;
    case SlDLSSMode::eMaxQuality:
        return 2.0f / 3.0f;
    case SlDLSSMode::eUltraQuality:
        return 0.77f;
    case SlDLSSMode::eDLAA:
        return 1.0f;
    case SlDLSSMode::eOff:
    default:
        return 1.0f;
    }
}

void apusrFillDlssOptimalSettings(const SlDLSSOptions& options, SlDLSSOptimalSettings& settings)
{
    const std::uint32_t outW = (options.outputWidth == 0 || options.outputWidth == 0xFFFFFFFFu) ? 1u : options.outputWidth;
    const std::uint32_t outH = (options.outputHeight == 0 || options.outputHeight == 0xFFFFFFFFu) ? 1u : options.outputHeight;
    const float scale = apusrHostDlssScale(options.mode);
    settings.optimalRenderWidth = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(outW) * scale + 0.5f));
    settings.optimalRenderHeight = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(outH) * scale + 0.5f));
    settings.optimalSharpness = 0.0f;
    settings.renderWidthMin = std::max(1u, outW / 3u);
    settings.renderHeightMin = std::max(1u, outH / 3u);
    settings.renderWidthMax = outW;
    settings.renderHeightMax = outH;
}

const SlBaseStructure* apusrFindStruct(const SlBaseStructure* head, std::uint32_t data1)
{
    for (const SlBaseStructure* it = head; it != nullptr; it = it->next) {
        if (it->structType.data1 == data1) return it;
    }
    return nullptr;
}

SlBaseStructure* apusrFindStruct(SlBaseStructure* head, std::uint32_t data1)
{
    for (SlBaseStructure* it = head; it != nullptr; it = it->next) {
        if (it->structType.data1 == data1) return it;
    }
    return nullptr;
}

int __cdecl apusrHostSlDLSSGetOptimalSettings(const SlDLSSOptions& options, SlDLSSOptimalSettings& settings)
{
    if (options.outputWidth == 0 || options.outputHeight == 0 ||
        options.outputWidth == 0xFFFFFFFFu || options.outputHeight == 0xFFFFFFFFu) {
        apusr::host_discovery::eventf("streamline", "sl.dlss.optimal_settings.failed",
                                      "reason=missing_output mode=%u out=%ux%u",
                                      static_cast<std::uint32_t>(options.mode), options.outputWidth, options.outputHeight);
        return kSlResultMissingInputParameter;
    }
    apusrFillDlssOptimalSettings(options, settings);
    apusr::host_discovery::eventf("streamline", "sl.dlss.optimal_settings.apusr",
                                  "mode=%u render=%ux%u output=%ux%u",
                                  static_cast<std::uint32_t>(options.mode),
                                  settings.optimalRenderWidth, settings.optimalRenderHeight,
                                  options.outputWidth, options.outputHeight);
    return kSlResultOk;
}

int __cdecl apusrHostSlDLSSGetState(const SlViewportHandle& viewport, SlDLSSState& state)
{
    (void)viewport;
    state.estimatedVRAMUsageInBytes = 0;
    apusr::host_discovery::event("streamline", "sl.dlss.state.apusr", "vram=0");
    return kSlResultOk;
}

int __cdecl apusrHostSlDLSSSetOptions(const SlViewportHandle& viewport, const SlDLSSOptions& options)
{
    ApusrCapturedOptions captured;
    captured.mode = options.mode;
    captured.outputWidth = options.outputWidth;
    captured.outputHeight = options.outputHeight;
    captured.sharpness = 0.0f;
    captured.preExposure = options.preExposure == 0.0f ? 1.0f : options.preExposure;
    captured.exposureScale = options.exposureScale == 0.0f ? 1.0f : options.exposureScale;
    captured.useAutoExposure = options.structVersion >= 3u && options.useAutoExposure != 0;
    {
        std::lock_guard<std::mutex> lock(g_apusrStreamlineMutex);
        g_apusrOptionsByViewport[viewport.value] = captured;
        if (viewport.value != 0u) {
            g_apusrOptionsByViewport[0u] = captured;
        }
    }
    if (apusrShouldLogPeriodic(g_apusrSetOptionsLogCounter, 4, 1200)) {
        apusr::host_discovery::eventf("streamline", "sl.dlss.set_options.apusr",
                                      "viewport=%u mode=%u output=%ux%u sharpness=%.4f auto_exposure=%d",
                                      viewport.value, static_cast<std::uint32_t>(options.mode),
                                      options.outputWidth, options.outputHeight, options.sharpness,
                                      captured.useAutoExposure ? 1 : 0);
    }
    return kSlResultOk;
}

int __cdecl apusrHostSlSetData(const SlBaseStructure* inputs, void* cmdBuffer)
{
    (void)cmdBuffer;
    const auto* viewport = reinterpret_cast<const SlViewportHandle*>(apusrFindStruct(inputs, kStructViewportHandle));
    const auto* options = reinterpret_cast<const SlDLSSOptions*>(apusrFindStruct(inputs, kStructDLSSOptions));
    if (viewport != nullptr && options != nullptr) {
        return apusrHostSlDLSSSetOptions(*viewport, *options);
    }
    apusr::host_discovery::event("streamline", "sl.dlss.set_data.apusr", "result=ok_missing_known_structs");
    return kSlResultOk;
}

int __cdecl apusrHostSlGetData(const SlBaseStructure* inputs, SlBaseStructure* output, void* cmdBuffer)
{
    (void)cmdBuffer;
    if (output == nullptr) {
        return kSlResultInvalidParameter;
    }
    if (output->structType.data1 == kStructDLSSOptimalSettings) {
        const auto* options = reinterpret_cast<const SlDLSSOptions*>(apusrFindStruct(inputs, kStructDLSSOptions));
        if (options == nullptr) {
            return kSlResultMissingInputParameter;
        }
        return apusrHostSlDLSSGetOptimalSettings(*options, *reinterpret_cast<SlDLSSOptimalSettings*>(output));
    }
    if (output->structType.data1 == kStructDLSSState) {
        SlViewportHandle viewport{};
        if (const auto* found = reinterpret_cast<const SlViewportHandle*>(apusrFindStruct(inputs, kStructViewportHandle))) {
            viewport = *found;
        }
        return apusrHostSlDLSSGetState(viewport, *reinterpret_cast<SlDLSSState*>(output));
    }
    apusr::host_discovery::eventf("streamline", "sl.dlss.get_data.apusr",
                                  "result=unsupported_output struct=0x%08X", output->structType.data1);
    return kSlResultFeatureMissing;
}

int __cdecl apusrHostSlAllocateResources(void* cmdBuffer, std::uint32_t feature, const SlViewportHandle& viewport)
{
    (void)cmdBuffer;
    apusr::host_discovery::eventf("streamline", "sl.dlss.allocate_resources.apusr",
                                  "feature=%u viewport=%u", feature, viewport.value);
    return kSlResultOk;
}

int __cdecl apusrHostSlFreeResources(std::uint32_t feature, const SlViewportHandle& viewport)
{
    apusr::host_discovery::eventf("streamline", "sl.dlss.free_resources.apusr",
                                  "feature=%u viewport=%u", feature, viewport.value);
    return kSlResultOk;
}

bool __cdecl hkSlOnPluginLoad(void* params, const char* loaderJSON, const char** pluginJSON)
{
    // Preserve the stock plugin's JSON and support decision. Retain the current
    // parameter bus only for this Streamline epoch; a quality-mode change can
    // rebuild sl.common/sl.dlss and deliver a different bus in the same process.
    // The D3D12 startup seam republishes APUSR NGX into each such bus.
    {
        std::lock_guard<std::mutex> lock(g_streamlineNgxBridgeMutex);
        auto* const nextBus = static_cast<StreamlineParameterInterface*>(params);
        if (g_streamlineParameters != nextBus) {
            g_streamlineParameters = nextBus;
            g_streamlineNgxContextInstalledBus = nullptr;
            ++g_streamlineParameterBusEpoch;
            apusr::host_discovery::eventf(
                "streamline", "ngx_context.parameter_bus.changed",
                "parameter_bus=%p epoch=%llu", params,
                static_cast<unsigned long long>(g_streamlineParameterBusEpoch));
        }
    }

    const bool ok = g_slOnPluginLoad != nullptr && g_slOnPluginLoad(params, loaderJSON, pluginJSON);
    const auto renderApi = currentStreamlineRenderApi();
    apusr::host_discovery::eventf(
        "streamline", "sl.dlss.on_plugin_load",
        "delegated_original=1 render_api=%s ok=%d plugin_json=%d patched=0 forced_ok=0 parameter_bus=%p",
        streamlineRenderApiName(renderApi), ok ? 1 : 0,
        pluginJSON != nullptr && *pluginJSON != nullptr ? 1 : 0, params);
    return ok;
}


int __cdecl hkSlIsSupported(const void* adapterInfo)
{
    // With a coherent logical-GPU LUID and architecture in sl.common's SystemCaps, the stock
    // adapter mask is authoritative.  Do not manufacture support after plugin discovery.
    const int result = g_slIsSupported != nullptr ? g_slIsSupported(adapterInfo) : kSlResultFeatureMissing;
    const auto renderApi = currentStreamlineRenderApi();
    apusr::host_discovery::eventf(
        "streamline", "sl.dlss.is_supported",
        "delegated_original=1 render_api=%s result=%d",
        streamlineRenderApiName(renderApi), result);
    return result;
}


bool __cdecl hkSlOnPluginStartup(const char* jsonConfig, void* device)
{
    // Stock sl.dlss remains the semantic owner. On D3D12, however, stock
    // sl.common's statically linked NGX client refuses an unsigned APUSR nvngx
    // core before LoadLibrary is reached. Supply the equivalent common::NGXContext
    // through Streamline's own parameter bus immediately before sl.dlss startup.
    // Feature create/evaluate callbacks still receive the native command list
    // produced by commonInterface's proxy-unwrapping path.
    g_apusrOwnsStreamlineDlss.store(false, std::memory_order_release);
    const std::uint32_t configuredRenderApi = apusrStreamlineRenderApiFromStartupJson(jsonConfig);
    if (configuredRenderApi != kSlRenderApiUnknown) {
        g_apusrStreamlineRenderApi.store(configuredRenderApi, std::memory_order_release);
    }
    const auto renderApi = currentStreamlineRenderApi();

    // An in-process renderer restart can tear down Streamline, then rebuild
    // sl.common/sl.dlss for D3D12 without another slInit. The startup device is
    // Streamline's native ID3D12Device*, so this cold seam can safely restore
    // the APUSR attachment and Anti-Lag binding that slShutdown released.
    if (renderApi == kSlRenderApiD3d12 &&
        g_streamlineReflexRequested.load(std::memory_order_acquire)) {
        (void)rearmD3d12ReflexAfterRendererRestart(device);
    }

    bool startupReady = true;
    bool bridgeInstalled = false;
    if (renderApi == kSlRenderApiD3d12 || renderApi == kSlRenderApiVulkan) {
        bridgeInstalled = apusrInstallStreamlineNgxContextBridge(renderApi, device);
        startupReady = bridgeInstalled;
    }

    const bool ok = startupReady && g_slOnPluginStartup != nullptr &&
        g_slOnPluginStartup(jsonConfig, device);
    const char* renderApiName = streamlineRenderApiName(renderApi);
    apusr::host_discovery::eventf(
        "streamline", "sl.dlss.on_plugin_startup",
        "delegated_original=1 render_api=%s ok=%d owner=stock_plugin ngx_context_bridge=%d",
        renderApiName, ok ? 1 : 0, bridgeInstalled ? 1 : 0);
    return ok;
}

void __cdecl hkSlOnPluginShutdown()
{
    g_apusrOwnsStreamlineDlss.store(false, std::memory_order_release);
    if (g_slOnPluginShutdown != nullptr) {
        g_slOnPluginShutdown();
    }
    {
        // Hosts may rebuild sl.common/sl.dlss when an upscaler option changes.
        // Force the next startup to republish the APUSR NGX context even when
        // the parameter-bus object itself is reused at the same address.
        std::lock_guard<std::mutex> lock(g_streamlineNgxBridgeMutex);
        g_streamlineNgxContextInstalledBus = nullptr;
    }
    apusr::host_discovery::event("streamline", "sl.dlss.on_plugin_shutdown",
                                 "delegated_original=1 owner=stock_plugin ngx_context_republish_required=1");
}

bool __cdecl hkSlOnPluginsInitialized()
{
    const bool ok = g_slOnPluginsInitialized != nullptr && g_slOnPluginsInitialized();
    apusr::host_discovery::eventf("streamline", "sl.dlss.on_plugins_initialized",
                                 "delegated_original=1 ok=%d owner=stock_plugin",
                                 ok ? 1 : 0);
    return ok;
}

void* __cdecl hkSlGetPluginFunction(const char* functionName)
{
    if (g_slGetPluginFunction == nullptr) return nullptr;
    void* result = g_slGetPluginFunction(functionName);
    if (functionName != nullptr && apusr::host_discovery::enabled()) {
        bool firstLookup = false;
        {
            std::lock_guard<std::mutex> lock(g_apusrPluginFunctionLogMutex);
            firstLookup = g_apusrLoggedPluginFunctions.emplace(functionName).second;
        }
        if (firstLookup) {
            apusr::host_discovery::eventf("streamline", "sl.dlss.get_plugin_function",
                                         "name=%s result=%p", functionName, result);
        }
    }
    if (functionName != nullptr && result != nullptr) {
        if (std::strcmp(functionName, "slOnPluginLoad") == 0) {
            g_slOnPluginLoad = reinterpret_cast<SlOnPluginLoadFn>(result);
            return reinterpret_cast<void*>(&hkSlOnPluginLoad);
        }
        if (std::strcmp(functionName, "slIsSupported") == 0) {
            g_slIsSupported = reinterpret_cast<SlIsSupportedFn>(result);
            return reinterpret_cast<void*>(&hkSlIsSupported);
        }
        if (std::strcmp(functionName, "slOnPluginStartup") == 0) {
            g_slOnPluginStartup = reinterpret_cast<SlOnPluginStartupFn>(result);
            return reinterpret_cast<void*>(&hkSlOnPluginStartup);
        }
        if (std::strcmp(functionName, "slOnPluginShutdown") == 0) {
            g_slOnPluginShutdown = reinterpret_cast<SlOnPluginShutdownFn>(result);
            return reinterpret_cast<void*>(&hkSlOnPluginShutdown);
        }
        if (std::strcmp(functionName, "slOnPluginsInitialized") == 0) {
            g_slOnPluginsInitialized = reinterpret_cast<SlOnPluginsInitializedFn>(result);
            return reinterpret_cast<void*>(&hkSlOnPluginsInitialized);
        }
    }
    // Do not replace DLSS options/state/resource entry points.  The stock plugin
    // owns those semantics; APUSR owns only the NGX implementation it calls into.
    return result;
}


bool moduleLooksLikeDlssPlugin(HMODULE module)
{
    return moduleBaseNameEquals(module, L"sl.dlss.dll") || moduleBaseNameEquals(module, L"sl.dlss_d.dll");
}



std::uint32_t apusrViewportValue(const void* viewport) noexcept
{
    if (viewport == nullptr) return 0u;
    return static_cast<const SlViewportHandle*>(viewport)->value;
}

HMODULE apusrLoadHostNgx()
{
    if (g_apusrHostNgx != nullptr) return g_apusrHostNgx;

    wchar_t selfPath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), selfPath, MAX_PATH);
    if (len != 0 && len < MAX_PATH) {
        if (wchar_t* slash = wcsrchr(selfPath, L'\\')) {
            *(slash + 1) = L'\0';
            const wchar_t* names[] = {L"nvngx_dlss.dll", L"nvngx.dll"};
            for (const wchar_t* name : names) {
                wchar_t candidate[MAX_PATH]{};
                wcscpy_s(candidate, selfPath);
                wcscat_s(candidate, name);
                g_apusrHostNgx = LoadLibraryW(candidate);
                if (g_apusrHostNgx != nullptr) {
                    apusr::host_discovery::eventf("ngx", "local.loaded_for_streamline", "path=%ls", candidate);
                    return g_apusrHostNgx;
                }
            }
        }
    }
    g_apusrHostNgx = LoadLibraryW(L"nvngx_dlss.dll");
    if (g_apusrHostNgx == nullptr) g_apusrHostNgx = LoadLibraryW(L"nvngx.dll");
    apusr::host_discovery::eventf("ngx", g_apusrHostNgx ? "loaded_for_streamline" : "load_for_streamline.failed",
                                  "module=%p", g_apusrHostNgx);
    return g_apusrHostNgx;
}

template <typename Fn>
Fn apusrNgxFn(const char* name)
{
    HMODULE mod = apusrLoadHostNgx();
    return mod != nullptr ? reinterpret_cast<Fn>(GetProcAddress(mod, name)) : nullptr;
}

template <typename Fn>
Fn apusrNgxFnCached(Fn& cached, const char* name)
{
    if (cached == nullptr) cached = apusrNgxFn<Fn>(name);
    return cached;
}

bool apusrEnsureStreamlineNgxD3D12Device(void* commandList)
{
    if (commandList == nullptr) {
        apusr::host_discovery::event("streamline", "ngx_context.device.failed", "reason=missing_command_list");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const HRESULT deviceHr = static_cast<ID3D12GraphicsCommandList*>(commandList)->GetDevice(
        IID_PPV_ARGS(&device));
    if (FAILED(deviceHr) || device == nullptr) {
        apusr::host_discovery::eventf(
            "streamline", "ngx_context.device.failed",
            "reason=get_device hr=0x%08X command_list=%p",
            static_cast<unsigned int>(deviceHr), commandList);
        return false;
    }

    Microsoft::WRL::ComPtr<IUnknown> identity;
    if (FAILED(device.As(&identity)) || identity == nullptr) {
        apusr::host_discovery::event(
            "streamline", "ngx_context.device.failed", "reason=query_iunknown");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_streamlineNgxBridgeMutex);
    if (g_streamlineNgxDeviceIdentity.Get() == identity.Get()) {
        return true;
    }

    auto init = apusrNgxFnCached(g_apusrNgxInitFn, "NVSDK_NGX_D3D12_Init");
    if (init == nullptr) {
        apusr::host_discovery::event(
            "streamline", "ngx_context.device.failed", "reason=missing_apusr_ngx_init");
        return false;
    }

    const std::uint32_t result = init(0ull, nullptr, device.Get(), nullptr, 0u);
    if (result != kNgxSuccess) {
        apusr::host_discovery::eventf(
            "streamline", "ngx_context.device.failed",
            "reason=apusr_ngx_init result=0x%08X device=%p",
            result, device.Get());
        return false;
    }

    g_streamlineNgxDeviceIdentity = identity;
    apusr::host_discovery::eventf(
        "streamline", "ngx_context.device.ready",
        "device=%p identity=%p source=native_command_list",
        device.Get(), identity.Get());
    return true;
}

bool __cdecl apusrStreamlineNgxCreateFeature(
    void* commandList,
    const unsigned int feature,
    NVSDK_NGX_Handle** handle,
    const char* id)
{
    if (feature != kNgxFeatureSuperSampling || handle == nullptr ||
        g_streamlineNgxContext.params == nullptr ||
        !apusrEnsureStreamlineNgxD3D12Device(commandList)) {
        return false;
    }

    auto create = apusrNgxFnCached(g_apusrNgxCreateFeatureFn, "NVSDK_NGX_D3D12_CreateFeature");
    if (create == nullptr) {
        apusr::host_discovery::event("streamline", "ngx_context.create.failed", "reason=missing_export");
        return false;
    }

    const std::uint32_t result = create(
        commandList, feature, g_streamlineNgxContext.params, handle);
    apusr::host_discovery::eventf(
        "streamline", result == kNgxSuccess ? "ngx_context.create.ok" : "ngx_context.create.failed",
        "feature=%u id=%s result=0x%08X handle=%p",
        feature, id != nullptr ? id : "", result,
        handle != nullptr ? static_cast<void*>(*handle) : nullptr);
    return result == kNgxSuccess;
}

bool __cdecl apusrStreamlineNgxEvaluateFeature(
    void* commandList,
    NVSDK_NGX_Handle* handle,
    const char* id)
{
    if (commandList == nullptr || handle == nullptr || g_streamlineNgxContext.params == nullptr ||
        g_streamlineNgxDeviceLossCircuitOpen.load(std::memory_order_acquire)) {
        return false;
    }
    auto evaluate = apusrNgxFnCached(
        g_apusrNgxEvaluateFeatureFn, "NVSDK_NGX_D3D12_EvaluateFeature");
    if (evaluate == nullptr) {
        return false;
    }

    // Stock sl.dlss has already transitioned its inputs to eTextureRead and
    // output to eStorageRW before it enters the NGX context. Mark that state
    // provenance for the APUSR frontend so it does not invent COMMON as the
    // StateBefore for game-owned resources. Clear the marker synchronously
    // after evaluation so direct/non-Streamline NGX callers remain unaffected.
    g_streamlineNgxContext.params->Set(kApusrStreamlineD3D12PreparedStatesParameter, 1u);
    const std::uint32_t result = evaluate(
        commandList, handle, g_streamlineNgxContext.params, nullptr);
    g_streamlineNgxContext.params->Set(kApusrStreamlineD3D12PreparedStatesParameter, 0u);

    if (result != kNgxSuccess) {
        apusr::host_discovery::eventf(
            "streamline", "ngx_context.evaluate.failed",
            "id=%s result=0x%08X handle=%p",
            id != nullptr ? id : "", result, handle);
    }

    // A failed APUSR recording can force the host to rebuild the entire D3D12
    // device. Never turn that into an infinite Streamline reload/reinject loop:
    // if this evaluation removed the native command-list device, keep the NGX
    // bridge disabled for the rest of this process. A restart is the recovery
    // boundary while the underlying correctness bug is investigated.
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    const HRESULT getDeviceHr = static_cast<ID3D12GraphicsCommandList*>(commandList)->GetDevice(
        IID_PPV_ARGS(&device));
    if (SUCCEEDED(getDeviceHr) && device != nullptr) {
        const HRESULT removedReason = device->GetDeviceRemovedReason();
        if (FAILED(removedReason)) {
            g_streamlineNgxDeviceLossCircuitOpen.store(true, std::memory_order_release);
            apusr::host_discovery::eventf(
                "streamline", "ngx_context.circuit_open",
                "reason=device_removed id=%s result=0x%08X device=%p removed_reason=0x%08X",
                id != nullptr ? id : "", result, device.Get(),
                static_cast<unsigned int>(removedReason));
        }
    }

    return result == kNgxSuccess &&
        !g_streamlineNgxDeviceLossCircuitOpen.load(std::memory_order_acquire);
}

bool __cdecl apusrStreamlineNgxReleaseFeature(NVSDK_NGX_Handle* handle, const char* id)
{
    if (handle == nullptr) {
        return false;
    }
    auto release = apusrNgxFnCached(
        g_apusrNgxReleaseFeatureFn, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (release == nullptr) {
        return false;
    }
    const std::uint32_t result = release(handle);
    apusr::host_discovery::eventf(
        "streamline", result == kNgxSuccess ? "ngx_context.release.ok" : "ngx_context.release.failed",
        "id=%s result=0x%08X handle=%p",
        id != nullptr ? id : "", result, handle);
    return result == kNgxSuccess;
}

void __cdecl apusrStreamlineNgxUpdateFeature(const unsigned int feature)
{
    auto update = apusrNgxFnCached(g_apusrNgxUpdateFeatureFn, "NVSDK_NGX_UpdateFeature");
    if (update != nullptr) {
        (void)update(nullptr, feature);
    }
}

bool __cdecl apusrStreamlineNgxVulkanCreateFeature(
    void* commandBuffer,
    const unsigned int feature,
    NVSDK_NGX_Handle** handle,
    const char* id)
{
    if (feature != kNgxFeatureSuperSampling || commandBuffer == nullptr || handle == nullptr ||
        g_streamlineNgxContext.params == nullptr || g_streamlineNgxVulkanDevice == nullptr) {
        return false;
    }
    auto create = apusrNgxFnCached(
        g_apusrNgxVulkanCreateFeatureFn, "NVSDK_NGX_VULKAN_CreateFeature");
    if (create == nullptr) {
        apusr::host_discovery::event(
            "streamline", "ngx_context.create.failed", "backend=vulkan reason=missing_export");
        return false;
    }
    const std::uint32_t result = create(
        commandBuffer, feature, g_streamlineNgxContext.params, handle);
    apusr::host_discovery::eventf(
        "streamline", result == kNgxSuccess ? "ngx_context.create.ok" : "ngx_context.create.failed",
        "backend=vulkan feature=%u id=%s result=0x%08X handle=%p",
        feature, id != nullptr ? id : "", result,
        handle != nullptr ? static_cast<void*>(*handle) : nullptr);
    return result == kNgxSuccess;
}

bool __cdecl apusrStreamlineNgxVulkanEvaluateFeature(
    void* commandBuffer,
    NVSDK_NGX_Handle* handle,
    const char* id)
{
    if (commandBuffer == nullptr || handle == nullptr || g_streamlineNgxContext.params == nullptr) {
        return false;
    }
    auto evaluate = apusrNgxFnCached(
        g_apusrNgxVulkanEvaluateFeatureFn, "NVSDK_NGX_VULKAN_EvaluateFeature");
    if (evaluate == nullptr) {
        return false;
    }
    const std::uint32_t result = evaluate(
        commandBuffer, handle, g_streamlineNgxContext.params, nullptr);
    if (result != kNgxSuccess) {
        apusr::host_discovery::eventf(
            "streamline", "ngx_context.evaluate.failed",
            "backend=vulkan id=%s result=0x%08X handle=%p",
            id != nullptr ? id : "", result, handle);
    }
    return result == kNgxSuccess;
}

bool __cdecl apusrStreamlineNgxVulkanReleaseFeature(NVSDK_NGX_Handle* handle, const char* id)
{
    if (handle == nullptr) {
        return false;
    }
    auto release = apusrNgxFnCached(
        g_apusrNgxVulkanReleaseFeatureFn, "NVSDK_NGX_VULKAN_ReleaseFeature");
    if (release == nullptr) {
        return false;
    }
    const std::uint32_t result = release(handle);
    apusr::host_discovery::eventf(
        "streamline", result == kNgxSuccess ? "ngx_context.release.ok" : "ngx_context.release.failed",
        "backend=vulkan id=%s result=0x%08X handle=%p",
        id != nullptr ? id : "", result, handle);
    return result == kNgxSuccess;
}

bool apusrInstallStreamlineNgxContextBridge(const std::uint32_t renderApi, void* startupDevice)
{
    if (renderApi == kSlRenderApiD3d12 &&
        g_streamlineNgxDeviceLossCircuitOpen.load(std::memory_order_acquire)) {
        apusr::host_discovery::event(
            "streamline", "ngx_context.inject.blocked",
            "backend=d3d12 reason=prior_device_loss recovery=process_restart");
        return false;
    }
    if (renderApi != kSlRenderApiD3d12 && renderApi != kSlRenderApiVulkan) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_streamlineNgxBridgeMutex);
    StreamlineParameterInterface* const parameterBus = g_streamlineParameters;
    if (parameterBus == nullptr) {
        apusr::host_discovery::event(
            "streamline", "ngx_context.inject.failed", "reason=missing_parameter_bus");
        return false;
    }

    NgxGetCapabilityParametersFn getCapabilities = nullptr;
    NgxDestroyParametersFn destroyParameters = nullptr;
    StreamlineNgxCreateFeatureFn createCallback = nullptr;
    StreamlineNgxEvaluateFeatureFn evaluateCallback = nullptr;
    StreamlineNgxReleaseFeatureFn releaseCallback = nullptr;
    const char* backendName = nullptr;

    if (renderApi == kSlRenderApiVulkan) {
        backendName = "vulkan";
        const auto* vk = static_cast<const StreamlineVkDevicesBridge*>(startupDevice);
        if (vk == nullptr || vk->instance == nullptr || vk->physical == nullptr || vk->device == nullptr) {
            apusr::host_discovery::eventf(
                "streamline", "ngx_context.inject.failed",
                "backend=vulkan reason=missing_vk_devices startup_device=%p", startupDevice);
            return false;
        }

        auto init = apusrNgxFnCached(g_apusrNgxVulkanInitFn, "NVSDK_NGX_VULKAN_Init");
        getCapabilities = apusrNgxFnCached(
            g_apusrNgxVulkanGetCapabilityParametersFn,
            "NVSDK_NGX_VULKAN_GetCapabilityParameters");
        destroyParameters = apusrNgxFnCached(
            g_apusrNgxVulkanDestroyParametersFn,
            "NVSDK_NGX_VULKAN_DestroyParameters");
        auto create = apusrNgxFnCached(
            g_apusrNgxVulkanCreateFeatureFn, "NVSDK_NGX_VULKAN_CreateFeature");
        auto evaluate = apusrNgxFnCached(
            g_apusrNgxVulkanEvaluateFeatureFn, "NVSDK_NGX_VULKAN_EvaluateFeature");
        auto release = apusrNgxFnCached(
            g_apusrNgxVulkanReleaseFeatureFn, "NVSDK_NGX_VULKAN_ReleaseFeature");
        if (init == nullptr || getCapabilities == nullptr || destroyParameters == nullptr ||
            create == nullptr || evaluate == nullptr || release == nullptr) {
            apusr::host_discovery::event(
                "streamline", "ngx_context.inject.failed",
                "backend=vulkan reason=missing_apusr_ngx_exports");
            return false;
        }

        // slSetVulkanInfo has already established the real host VkInstance,
        // VkPhysicalDevice and VkDevice. Initialize APUSR's Vulkan NGX state
        // from that exact tuple before stock sl.dlss asks the context for its
        // availability parameter. This replaces only NVIDIA's signed NGX-core
        // bootstrap; stock Streamline still owns tags, layouts and evaluation.
        const std::uint32_t initResult = init(
            0ull, nullptr, vk->instance, vk->physical, vk->device);
        if (initResult != kNgxSuccess) {
            apusr::host_discovery::eventf(
                "streamline", "ngx_context.inject.failed",
                "backend=vulkan reason=apusr_ngx_init result=0x%08X instance=%p physical=%p device=%p",
                initResult, vk->instance, vk->physical, vk->device);
            return false;
        }
        g_streamlineNgxVulkanInstance = vk->instance;
        g_streamlineNgxVulkanPhysicalDevice = vk->physical;
        g_streamlineNgxVulkanDevice = vk->device;
        apusr::host_discovery::eventf(
            "streamline", "ngx_context.device.ready",
            "backend=vulkan instance=%p physical=%p device=%p source=slOnPluginStartup_vk_devices",
            vk->instance, vk->physical, vk->device);

        createCallback = &apusrStreamlineNgxVulkanCreateFeature;
        evaluateCallback = &apusrStreamlineNgxVulkanEvaluateFeature;
        releaseCallback = &apusrStreamlineNgxVulkanReleaseFeature;
    } else {
        backendName = "d3d12";
        getCapabilities = apusrNgxFnCached(
            g_apusrNgxGetCapabilityParametersFn,
            "NVSDK_NGX_D3D12_GetCapabilityParameters");
        destroyParameters = apusrNgxFnCached(
            g_apusrNgxDestroyParametersFn,
            "NVSDK_NGX_D3D12_DestroyParameters");
        auto create = apusrNgxFnCached(
            g_apusrNgxCreateFeatureFn, "NVSDK_NGX_D3D12_CreateFeature");
        auto evaluate = apusrNgxFnCached(
            g_apusrNgxEvaluateFeatureFn, "NVSDK_NGX_D3D12_EvaluateFeature");
        auto release = apusrNgxFnCached(
            g_apusrNgxReleaseFeatureFn, "NVSDK_NGX_D3D12_ReleaseFeature");
        if (getCapabilities == nullptr || destroyParameters == nullptr ||
            create == nullptr || evaluate == nullptr || release == nullptr) {
            apusr::host_discovery::event(
                "streamline", "ngx_context.inject.failed",
                "backend=d3d12 reason=missing_apusr_ngx_exports");
            return false;
        }
        createCallback = &apusrStreamlineNgxCreateFeature;
        evaluateCallback = &apusrStreamlineNgxEvaluateFeature;
        releaseCallback = &apusrStreamlineNgxReleaseFeature;
    }

    // D3D12 and Vulkan capability stores are separate NGX allocations. When a
    // host changes renderer in-process, discard the old backend's store before
    // publishing the same stable Streamline NGXContext object into the new bus.
    if (g_streamlineNgxContext.params != nullptr &&
        g_streamlineNgxContextRenderApi != renderApi) {
        NgxDestroyParametersFn destroyOld = nullptr;
        if (g_streamlineNgxContextRenderApi == kSlRenderApiVulkan) {
            destroyOld = apusrNgxFnCached(
                g_apusrNgxVulkanDestroyParametersFn,
                "NVSDK_NGX_VULKAN_DestroyParameters");
        } else if (g_streamlineNgxContextRenderApi == kSlRenderApiD3d12) {
            destroyOld = apusrNgxFnCached(
                g_apusrNgxDestroyParametersFn,
                "NVSDK_NGX_D3D12_DestroyParameters");
        }
        if (destroyOld != nullptr) {
            (void)destroyOld(g_streamlineNgxContext.params);
        }
        g_streamlineNgxContext.params = nullptr;
        g_streamlineNgxContextInstalledBus = nullptr;
        g_streamlineNgxDeviceIdentity.Reset();
    }

    if (g_streamlineNgxContext.params == nullptr) {
        NVSDK_NGX_Parameter* params = nullptr;
        const std::uint32_t capabilityResult = getCapabilities(&params);
        if (capabilityResult != kNgxSuccess || params == nullptr) {
            apusr::host_discovery::eventf(
                "streamline", "ngx_context.inject.failed",
                "backend=%s reason=capability_parameters result=0x%08X params=%p",
                backendName, capabilityResult, params);
            return false;
        }
        g_streamlineNgxContext.params = params;
    }

    g_streamlineNgxContextRenderApi = renderApi;
    g_streamlineNgxContext.createFeature = createCallback;
    g_streamlineNgxContext.releaseFeature = releaseCallback;
    g_streamlineNgxContext.evaluateFeature = evaluateCallback;
    g_streamlineNgxContext.updateFeature = &apusrStreamlineNgxUpdateFeature;

    parameterBus->set(
        kStreamlineNgxContextParameter,
        static_cast<void*>(&g_streamlineNgxContext));

    void* observed = nullptr;
    if (!parameterBus->get(kStreamlineNgxContextParameter, &observed) ||
        observed != static_cast<void*>(&g_streamlineNgxContext)) {
        g_streamlineNgxContextInstalledBus = nullptr;
        apusr::host_discovery::eventf(
            "streamline", "ngx_context.inject.failed",
            "backend=%s reason=parameter_bus_roundtrip parameter_bus=%p expected=%p observed=%p epoch=%llu",
            backendName,
            static_cast<void*>(parameterBus),
            static_cast<void*>(&g_streamlineNgxContext), observed,
            static_cast<unsigned long long>(g_streamlineParameterBusEpoch));
        return false;
    }

    const bool republished = g_streamlineNgxContextInstalledBus == parameterBus;
    g_streamlineNgxContextInstalledBus = parameterBus;
    apusr::host_discovery::eventf(
        "streamline", republished ? "ngx_context.republished" : "ngx_context.injected",
        "backend=%s parameter_bus=%p context=%p params=%p epoch=%llu owner=stock_sl_dlss implementation=apusr_ngx signed_core_loader_bypassed=1",
        backendName,
        static_cast<void*>(parameterBus),
        static_cast<void*>(&g_streamlineNgxContext),
        static_cast<void*>(g_streamlineNgxContext.params),
        static_cast<unsigned long long>(g_streamlineParameterBusEpoch));
    return true;
}

void apusrSetNgxPointer(NVSDK_NGX_Parameter* params, const char* name, void* value)
{
    if (params != nullptr && name != nullptr) params->Set(name, value);
}

void apusrSetNgxUInt(NVSDK_NGX_Parameter* params, const char* name, std::uint32_t value)
{
    if (params != nullptr && name != nullptr) params->Set(name, value);
}

void apusrSetNgxInt(NVSDK_NGX_Parameter* params, const char* name, int value)
{
    if (params != nullptr && name != nullptr) params->Set(name, value);
}

void apusrSetNgxFloat(NVSDK_NGX_Parameter* params, const char* name, float value)
{
    if (params != nullptr && name != nullptr) params->Set(name, value);
}

std::uint32_t apusrNgxQualityFromMode(SlDLSSMode mode) noexcept
{
    switch (mode) {
    case SlDLSSMode::eUltraPerformance: return 3u;
    case SlDLSSMode::eMaxPerformance: return 0u;
    case SlDLSSMode::eMaxQuality: return 2u;
    case SlDLSSMode::eUltraQuality: return 4u;
    case SlDLSSMode::eDLAA: return 5u;
    case SlDLSSMode::eBalanced:
    case SlDLSSMode::eOff:
    default: return 1u;
    }
}

ApusrCapturedResource apusrCapturedResourceFromTag(const SlResourceTag& tag)
{
    ApusrCapturedResource out;
    if (tag.resource == nullptr) return out;
    out.native = tag.resource->native;
    out.state = tag.resource->state;
    out.width = tag.extent.width != 0 ? tag.extent.width : tag.resource->width;
    out.height = tag.extent.height != 0 ? tag.extent.height : tag.resource->height;
    return out;
}

void apusrCaptureTag(ApusrCapturedTags& dst, const SlResourceTag& tag)
{
    const ApusrCapturedResource r = apusrCapturedResourceFromTag(tag);
    switch (tag.type) {
    case kBufferTypeDepth: dst.depth = r; break;
    case kBufferTypeMotionVectors: dst.motionVectors = r; break;
    case kBufferTypeScalingInputColor: dst.color = r; break;
    case kBufferTypeScalingOutputColor: dst.output = r; break;
    case kBufferTypeExposure: dst.exposure = r; break;
    case kBufferTypeBiasCurrentColorHint:
    case kBufferTypeReactiveMaskHint: dst.reactive = r; break;
    default: break;
    }
}

bool apusrTagsAreOwnedDlssSrContract(const void* rawTags, std::uint32_t numTags)
{
    if (rawTags == nullptr || numTags == 0) return false;
    const auto* tags = static_cast<const SlResourceTag*>(rawTags);
    for (std::uint32_t i = 0; i < numTags; ++i) {
        switch (tags[i].type) {
        case kBufferTypeDepth:
        case kBufferTypeMotionVectors:
        case kBufferTypeScalingInputColor:
        case kBufferTypeScalingOutputColor:
        case kBufferTypeExposure:
        case kBufferTypeBiasCurrentColorHint:
        case kBufferTypeReactiveMaskHint:
            return true;
        default:
            break;
        }
    }
    return false;
}

void apusrMergeTags(ApusrCapturedTags& dst, const ApusrCapturedTags& src)
{
    if (src.color.native != nullptr) dst.color = src.color;
    if (src.depth.native != nullptr) dst.depth = src.depth;
    if (src.motionVectors.native != nullptr) dst.motionVectors = src.motionVectors;
    if (src.output.native != nullptr) dst.output = src.output;
    if (src.exposure.native != nullptr) dst.exposure = src.exposure;
    if (src.reactive.native != nullptr) dst.reactive = src.reactive;
}

void apusrCaptureTagsForKey(ApusrStreamlineKey key, const void* rawTags, std::uint32_t numTags)
{
    if (rawTags == nullptr || numTags == 0) return;
    const auto* tags = static_cast<const SlResourceTag*>(rawTags);
    std::lock_guard<std::mutex> lock(g_apusrStreamlineMutex);
    auto captureInto = [&](ApusrCapturedTags& dst) {
        for (std::uint32_t i = 0; i < numTags; ++i) apusrCaptureTag(dst, tags[i]);
    };
    ApusrCapturedTags& dst = key.frame != nullptr ? g_apusrTagsByFrameViewport[key] : g_apusrLegacyTagsByViewport[key.viewport];
    captureInto(dst);
    if (key.viewport != 0u) {
        ApusrCapturedTags& fallback = key.frame != nullptr ? g_apusrTagsByFrameViewport[ApusrStreamlineKey{key.frame, 0u}]
                                                           : g_apusrLegacyTagsByViewport[0u];
        captureInto(fallback);
    }
    if (apusrShouldLogPeriodic(g_apusrTagLogCounter, 4, 1200)) {
        apusr::host_discovery::eventf("streamline", "core.tags.captured",
                                      "frame=%p viewport=%u count=%u color=%p depth=%p mv=%p output=%p color_size=%ux%u depth_size=%ux%u mv_size=%ux%u output_size=%ux%u states=(0x%X,0x%X,0x%X,0x%X)",
                                      key.frame, key.viewport, numTags, dst.color.native, dst.depth.native, dst.motionVectors.native, dst.output.native,
                                      dst.color.width, dst.color.height, dst.depth.width, dst.depth.height,
                                      dst.motionVectors.width, dst.motionVectors.height, dst.output.width, dst.output.height,
                                      dst.color.state, dst.depth.state, dst.motionVectors.state, dst.output.state);
    }
}

void apusrCaptureTagsFromInputs(ApusrCapturedTags& dst, const void* const* inputs, std::uint32_t numInputs)
{
    if (inputs == nullptr) return;
    for (std::uint32_t i = 0; i < numInputs; ++i) {
        const auto* base = static_cast<const SlBaseStructure*>(inputs[i]);
        if (base != nullptr && base->structType.data1 == kStructResourceTag) {
            apusrCaptureTag(dst, *reinterpret_cast<const SlResourceTag*>(base));
        }
    }
}


void apusrPruneFrameScopedStreamlineState(const void* currentFrame)
{
    if (currentFrame == nullptr || (g_apusrTagsByFrameViewport.size() <= 64u && g_apusrConstantsByFrameViewport.size() <= 64u)) {
        return;
    }
    for (auto it = g_apusrTagsByFrameViewport.begin(); it != g_apusrTagsByFrameViewport.end();) {
        if (it->first.frame != currentFrame) {
            it = g_apusrTagsByFrameViewport.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_apusrConstantsByFrameViewport.begin(); it != g_apusrConstantsByFrameViewport.end();) {
        if (it->first.frame != currentFrame) {
            it = g_apusrConstantsByFrameViewport.erase(it);
        } else {
            ++it;
        }
    }
}

bool apusrFindViewportInInputs(const void* const* inputs, std::uint32_t numInputs, std::uint32_t& viewport)
{
    if (inputs == nullptr) return false;
    for (std::uint32_t i = 0; i < numInputs; ++i) {
        const auto* base = static_cast<const SlBaseStructure*>(inputs[i]);
        if (base != nullptr && base->structType.data1 == kStructViewportHandle) {
            viewport = reinterpret_cast<const SlViewportHandle*>(base)->value;
            return true;
        }
    }
    return false;
}

bool apusrEnsureNgxFeature(const ApusrCapturedTags& tags, const ApusrCapturedOptions& options, const ApusrCapturedConstants& constants, void* cmdBuffer)
{
    if (cmdBuffer == nullptr) {
        apusr::host_discovery::eventf("streamline", "core.evaluate.failed", "reason=missing_cmd host_device=%p cmd=%p", g_apusrHostD3DDevice, cmdBuffer);
        return false;
    }

    // slSetD3DDevice is host state, not a lifetime guarantee. A renderer can
    // replace its D3D12 device while Streamline still has the previous pointer.
    // The command list passed to evaluation is authoritative for the work we are
    // about to record, so derive the NGX feature device from that command list.
    Microsoft::WRL::ComPtr<ID3D12Device> commandDevice;
    const HRESULT commandDeviceHr = static_cast<ID3D12GraphicsCommandList*>(cmdBuffer)->GetDevice(
        IID_PPV_ARGS(&commandDevice));
    if (FAILED(commandDeviceHr) || commandDevice == nullptr) {
        apusr::host_discovery::eventf(
            "streamline", "core.evaluate.failed",
            "reason=command_list_get_device_failed hr=0x%08X host_device=%p cmd=%p",
            static_cast<unsigned int>(commandDeviceHr), g_apusrHostD3DDevice, cmdBuffer);
        return false;
    }
    std::uint32_t renderWidth = tags.color.width;
    std::uint32_t renderHeight = tags.color.height;
    if (renderWidth == 0 || renderHeight == 0) {
        const float scale = apusrHostDlssScale(options.mode);
        renderWidth = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(options.outputWidth) * scale + 0.5f));
        renderHeight = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(options.outputHeight) * scale + 0.5f));
    }
    const std::uint32_t outputWidth = options.outputWidth != 0 && options.outputWidth != 0xFFFFFFFFu ? options.outputWidth : tags.output.width;
    const std::uint32_t outputHeight = options.outputHeight != 0 && options.outputHeight != 0xFFFFFFFFu ? options.outputHeight : tags.output.height;
    if (renderWidth == 0 || renderHeight == 0 || outputWidth == 0 || outputHeight == 0) {
        apusr::host_discovery::eventf("streamline", "core.evaluate.failed", "reason=missing_dimensions render=%ux%u output=%ux%u", renderWidth, renderHeight, outputWidth, outputHeight);
        return false;
    }
    const std::uint32_t createFlags = apusrCreateFlagsFromStreamlineContract(tags, options, constants, renderWidth, renderHeight);
    if (g_apusrNgxFeature != nullptr && g_apusrNgxRenderWidth == renderWidth && g_apusrNgxRenderHeight == renderHeight &&
        g_apusrNgxOutputWidth == outputWidth && g_apusrNgxOutputHeight == outputHeight &&
        g_apusrNgxMode == options.mode && g_apusrNgxFeatureFlags == createFlags &&
        g_apusrNgxFeatureDevice == commandDevice.Get()) {
        return true;
    }
    if (g_apusrNgxFeature != nullptr && g_apusrNgxFeatureDevice != commandDevice.Get()) {
        apusr::host_discovery::eventf(
            "streamline", "core.ngx_feature.device_changed",
            "host_device=%p feature_device=%p command_device=%p cmd=%p",
            g_apusrHostD3DDevice, g_apusrNgxFeatureDevice, commandDevice.Get(), cmdBuffer);
    }
    auto init = apusrNgxFnCached(g_apusrNgxInitFn, "NVSDK_NGX_D3D12_Init");
    auto alloc = apusrNgxFnCached(g_apusrNgxAllocateParametersFn, "NVSDK_NGX_D3D12_AllocateParameters");
    auto destroy = apusrNgxFnCached(g_apusrNgxDestroyParametersFn, "NVSDK_NGX_D3D12_DestroyParameters");
    auto create = apusrNgxFnCached(g_apusrNgxCreateFeatureFn, "NVSDK_NGX_D3D12_CreateFeature");
    auto release = apusrNgxFnCached(g_apusrNgxReleaseFeatureFn, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (init == nullptr || alloc == nullptr || destroy == nullptr || create == nullptr) {
        apusr::host_discovery::event("streamline", "core.evaluate.failed", "reason=missing_ngx_exports");
        return false;
    }
    if (g_apusrNgxFeature != nullptr && release != nullptr) {
        release(g_apusrNgxFeature);
        g_apusrNgxFeature = nullptr;
        g_apusrNgxFeatureDevice = nullptr;
    }
    if (init(0ull, nullptr, commandDevice.Get(), nullptr, 0u) != kNgxSuccess) {
        apusr::host_discovery::event("streamline", "core.evaluate.failed", "reason=ngx_init_failed");
        return false;
    }
    NVSDK_NGX_Parameter* params = nullptr;
    if (alloc(&params) != kNgxSuccess || params == nullptr) {
        apusr::host_discovery::event("streamline", "core.evaluate.failed", "reason=ngx_alloc_failed");
        return false;
    }
    apusrSetNgxUInt(params, "Width", renderWidth);
    apusrSetNgxUInt(params, "Height", renderHeight);
    apusrSetNgxUInt(params, "OutWidth", outputWidth);
    apusrSetNgxUInt(params, "OutHeight", outputHeight);
    apusrSetNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Width", renderWidth);
    apusrSetNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Height", renderHeight);
    apusrSetNgxUInt(params, "TargetWidth", outputWidth);
    apusrSetNgxUInt(params, "TargetHeight", outputHeight);
    apusrSetNgxInt(params, "PerfQualityValue", static_cast<int>(apusrNgxQualityFromMode(options.mode)));
    apusrSetNgxUInt(params, "DLSS.Feature.Create.Flags", createFlags);
    NVSDK_NGX_Handle* feature = nullptr;
    const std::uint32_t result = create(cmdBuffer, kNgxFeatureSuperSampling, params, &feature);
    destroy(params);
    if (result != kNgxSuccess || feature == nullptr) {
        apusr::host_discovery::eventf("streamline", "core.evaluate.failed", "reason=ngx_create_failed result=0x%08X", result);
        return false;
    }
    g_apusrNgxFeature = feature;
    g_apusrNgxFeatureDevice = commandDevice.Get();
    g_apusrNgxRenderWidth = renderWidth;
    g_apusrNgxRenderHeight = renderHeight;
    g_apusrNgxOutputWidth = outputWidth;
    g_apusrNgxOutputHeight = outputHeight;
    g_apusrNgxMode = options.mode;
    g_apusrNgxFeatureFlags = createFlags;
    apusr::host_discovery::eventf("streamline", "core.ngx_feature.created",
                                  "device=%p host_device=%p render=%ux%u output=%ux%u mode=%u flags=0x%08X mv_size=%ux%u depth_size=%ux%u",
                                  commandDevice.Get(), g_apusrHostD3DDevice,
                                  renderWidth, renderHeight, outputWidth, outputHeight,
                                  static_cast<std::uint32_t>(options.mode), createFlags,
                                  tags.motionVectors.width, tags.motionVectors.height, tags.depth.width, tags.depth.height);
    return true;
}

int apusrEvaluateDlss(const void* frame, const void* const* inputs, std::uint32_t numInputs, void* cmdBuffer)
{
    std::uint32_t viewport = 0;
    (void)apusrFindViewportInInputs(inputs, numInputs, viewport);
    ApusrCapturedOptions options;
    ApusrCapturedConstants constants;
    ApusrCapturedTags tags;
    {
        std::lock_guard<std::mutex> lock(g_apusrStreamlineMutex);
        if (auto it = g_apusrOptionsByViewport.find(0u); it != g_apusrOptionsByViewport.end()) options = it->second;
        if (auto it = g_apusrOptionsByViewport.find(viewport); it != g_apusrOptionsByViewport.end()) options = it->second;
        if (auto it = g_apusrConstantsByViewport.find(0u); it != g_apusrConstantsByViewport.end()) apusrMergeConstants(constants, it->second);
        if (auto it = g_apusrConstantsByViewport.find(viewport); it != g_apusrConstantsByViewport.end()) apusrMergeConstants(constants, it->second);
        if (auto it = g_apusrConstantsByFrameViewport.find(ApusrStreamlineKey{frame, 0u}); it != g_apusrConstantsByFrameViewport.end()) apusrMergeConstants(constants, it->second);
        if (auto it = g_apusrConstantsByFrameViewport.find(ApusrStreamlineKey{frame, viewport}); it != g_apusrConstantsByFrameViewport.end()) apusrMergeConstants(constants, it->second);
        if (auto it = g_apusrLegacyTagsByViewport.find(0u); it != g_apusrLegacyTagsByViewport.end()) apusrMergeTags(tags, it->second);
        if (auto it = g_apusrLegacyTagsByViewport.find(viewport); it != g_apusrLegacyTagsByViewport.end()) apusrMergeTags(tags, it->second);
        if (auto it = g_apusrTagsByFrameViewport.find(ApusrStreamlineKey{frame, 0u}); it != g_apusrTagsByFrameViewport.end()) apusrMergeTags(tags, it->second);
        if (auto it = g_apusrTagsByFrameViewport.find(ApusrStreamlineKey{frame, viewport}); it != g_apusrTagsByFrameViewport.end()) apusrMergeTags(tags, it->second);
        apusrPruneFrameScopedStreamlineState(frame);
    }
    apusrCaptureTagsFromInputs(tags, inputs, numInputs);
    apusrCaptureConstantsFromInputs(constants, inputs, numInputs);
    if (tags.color.native == nullptr || tags.depth.native == nullptr || tags.motionVectors.native == nullptr || tags.output.native == nullptr) {
        apusr::host_discovery::eventf("streamline", "core.evaluate.failed", "reason=missing_tags color=%p depth=%p mv=%p output=%p", tags.color.native, tags.depth.native, tags.motionVectors.native, tags.output.native);
        return kSlResultMissingInputParameter;
    }
    std::lock_guard<std::mutex> lock(g_apusrNgxMutex);
    if (!apusrEnsureNgxFeature(tags, options, constants, cmdBuffer)) return 34;
    auto alloc = apusrNgxFnCached(g_apusrNgxAllocateParametersFn, "NVSDK_NGX_D3D12_AllocateParameters");
    auto eval = apusrNgxFnCached(g_apusrNgxEvaluateFeatureFn, "NVSDK_NGX_D3D12_EvaluateFeature");
    if (alloc == nullptr || eval == nullptr) {
        apusr::host_discovery::event("streamline", "core.evaluate.failed", "reason=missing_ngx_eval_exports");
        return 34;
    }
    NVSDK_NGX_Parameter* params = g_apusrNgxEvalParams;
    if (params == nullptr) {
        if (alloc(&params) != kNgxSuccess || params == nullptr) return 34;
        g_apusrNgxEvalParams = params;
    } else {
        params->Reset();
    }
    const std::uint32_t renderWidth = tags.color.width != 0 ? tags.color.width : g_apusrNgxRenderWidth;
    const std::uint32_t renderHeight = tags.color.height != 0 ? tags.color.height : g_apusrNgxRenderHeight;
    const std::uint32_t outputWidth = options.outputWidth != 0 && options.outputWidth != 0xFFFFFFFFu ? options.outputWidth : (tags.output.width != 0 ? tags.output.width : g_apusrNgxOutputWidth);
    const std::uint32_t outputHeight = options.outputHeight != 0 && options.outputHeight != 0xFFFFFFFFu ? options.outputHeight : (tags.output.height != 0 ? tags.output.height : g_apusrNgxOutputHeight);
    apusrSetNgxPointer(params, "Color", tags.color.native);
    apusrSetNgxPointer(params, "Depth", tags.depth.native);
    apusrSetNgxPointer(params, "MotionVectors", tags.motionVectors.native);
    apusrSetNgxPointer(params, "Output", tags.output.native);
    if (tags.exposure.native != nullptr) apusrSetNgxPointer(params, "ExposureTexture", tags.exposure.native);
    if (tags.reactive.native != nullptr) apusrSetNgxPointer(params, "DLSS.Input.Bias.Current.Color.Mask", tags.reactive.native);
    apusrSetNgxUInt(params, "Width", renderWidth);
    apusrSetNgxUInt(params, "Height", renderHeight);
    apusrSetNgxUInt(params, "OutWidth", outputWidth);
    apusrSetNgxUInt(params, "OutHeight", outputHeight);
    apusrSetNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Width", renderWidth);
    apusrSetNgxUInt(params, "DLSS.Render.Subrect.Dimensions.Height", renderHeight);
    apusrSetNgxUInt(params, "TargetWidth", outputWidth);
    apusrSetNgxUInt(params, "TargetHeight", outputHeight);
    apusrSetNgxInt(params, "PerfQualityValue", static_cast<int>(apusrNgxQualityFromMode(options.mode)));
    apusrSetNgxFloat(params, "Sharpness", 0.0f);
    apusrSetNgxFloat(params, "DLSS.Pre.Exposure", options.preExposure);
    apusrSetNgxFloat(params, "DLSS.Exposure.Scale", options.exposureScale);
    const std::uint32_t createFlags = apusrCreateFlagsFromStreamlineContract(tags, options, constants, renderWidth, renderHeight);
    const float ngxMvScaleX = apusrNgxMotionVectorScale(constants.mvScaleX, renderWidth);
    const float ngxMvScaleY = apusrNgxMotionVectorScale(constants.mvScaleY, renderHeight);
    apusrSetNgxFloat(params, "Jitter.Offset.X", constants.jitterX);
    apusrSetNgxFloat(params, "Jitter.Offset.Y", constants.jitterY);
    apusrSetNgxFloat(params, "MV.Scale.X", ngxMvScaleX);
    apusrSetNgxFloat(params, "MV.Scale.Y", ngxMvScaleY);
    apusrSetNgxInt(params, "Reset", constants.reset ? 1 : 0);
    apusrSetNgxUInt(params, "DLSS.Feature.Create.Flags", createFlags);
    const std::uint32_t result = eval(cmdBuffer, g_apusrNgxFeature, params, nullptr);
    const bool shouldLogEval = result != kNgxSuccess || apusrShouldLogPeriodic(g_apusrEvaluateResultLogCounter, 8, 1200);
    if (shouldLogEval) {
        apusr::host_discovery::eventf("streamline", result == kNgxSuccess ? "core.evaluate.apusr" : "core.evaluate.failed",
                                      "result=0x%08X render=%ux%u output=%ux%u color=%p depth=%p mv=%p output_res=%p jitter=(%.6f,%.6f) sl_mv_scale=(%.6f,%.6f) ngx_mv_scale=(%.3f,%.3f) reset=%d flags=0x%08X mv_size=%ux%u depth_size=%ux%u",
                                      result, renderWidth, renderHeight, outputWidth, outputHeight,
                                      tags.color.native, tags.depth.native, tags.motionVectors.native, tags.output.native,
                                      constants.jitterX, constants.jitterY, constants.mvScaleX, constants.mvScaleY,
                                      ngxMvScaleX, ngxMvScaleY, constants.reset ? 1 : 0, createFlags,
                                      tags.motionVectors.width, tags.motionVectors.height, tags.depth.width, tags.depth.height);
    }
    return result == kNgxSuccess ? kSlResultOk : 34;
}

int __cdecl apusrCoreSlSetD3DDevice(void* d3dDevice)
{
    g_apusrHostD3DDevice = d3dDevice;
    apusr::host_discovery::eventf("streamline", "core.set_d3d_device.apusr", "device=%p", d3dDevice);
    return g_coreSlSetD3DDevice ? g_coreSlSetD3DDevice(d3dDevice) : kSlResultOk;
}

int __cdecl apusrCoreSlGetFeatureFunction(std::uint32_t feature, const char* functionName, void** function)
{
    if (function == nullptr) return kSlResultInvalidParameter;
    if (apusrStreamlineFeatureExplicitlyUnsupported(feature)) {
        *function = nullptr;
        return kSlResultFeatureMissing;
    }
    if (feature == kFeatureDLSS &&
        currentStreamlineRenderApi() == kSlRenderApiVulkan) {
        return g_coreSlGetFeatureFunction ? g_coreSlGetFeatureFunction(feature, functionName, function) : kSlResultFeatureMissing;
    }
    if (feature == kFeatureDLSS && functionName != nullptr) {
        void* replacement = nullptr;
        if (std::strcmp(functionName, "slDLSSGetOptimalSettings") == 0) replacement = reinterpret_cast<void*>(&apusrHostSlDLSSGetOptimalSettings);
        else if (std::strcmp(functionName, "slDLSSGetState") == 0) replacement = reinterpret_cast<void*>(&apusrHostSlDLSSGetState);
        else if (std::strcmp(functionName, "slDLSSSetOptions") == 0) replacement = reinterpret_cast<void*>(&apusrHostSlDLSSSetOptions);
        if (replacement != nullptr) {
            g_apusrOwnsStreamlineDlss.store(true, std::memory_order_release);
            *function = replacement;
            apusr::host_discovery::eventf("streamline", "core.get_feature_function.apusr", "feature=%u name=%s function=%p", feature, functionName, replacement);
            return kSlResultOk;
        }
    }
    return g_coreSlGetFeatureFunction ? g_coreSlGetFeatureFunction(feature, functionName, function) : kSlResultFeatureMissing;
}

int __cdecl apusrCoreSlSetTag(const void* viewport, const void* tags, std::uint32_t numTags, void* cmdBuffer)
{
    apusrCaptureTagsForKey(ApusrStreamlineKey{nullptr, apusrViewportValue(viewport)}, tags, numTags);
    const bool ownedDlssTags = g_apusrOwnsStreamlineDlss.load(std::memory_order_acquire) &&
                               apusrTagsAreOwnedDlssSrContract(tags, numTags);
    if (ownedDlssTags) {
        bool expected = false;
        if (g_apusrLoggedSkippedOwnedTagsForward.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            apusr::host_discovery::event("streamline", "core.tags.forward.skipped",
                                         "reason=apusr_owns_dlss_sr_contract");
        }
        (void)cmdBuffer;
        return kSlResultOk;
    }
    return g_coreSlSetTag ? g_coreSlSetTag(viewport, tags, numTags, cmdBuffer) : kSlResultOk;
}

int __cdecl apusrCoreSlSetTagForFrame(const void* frame, const void* viewport, const void* tags, std::uint32_t numTags, void* cmdBuffer)
{
    apusrCaptureTagsForKey(ApusrStreamlineKey{frame, apusrViewportValue(viewport)}, tags, numTags);
    const bool ownedDlssTags = g_apusrOwnsStreamlineDlss.load(std::memory_order_acquire) &&
                               apusrTagsAreOwnedDlssSrContract(tags, numTags);
    if (ownedDlssTags) {
        bool expected = false;
        if (g_apusrLoggedSkippedOwnedTagsForward.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            apusr::host_discovery::event("streamline", "core.tags.forward.skipped",
                                         "reason=apusr_owns_dlss_sr_contract");
        }
        (void)cmdBuffer;
        return kSlResultOk;
    }
    return g_coreSlSetTagForFrame ? g_coreSlSetTagForFrame(frame, viewport, tags, numTags, cmdBuffer) : kSlResultOk;
}

int __cdecl apusrCoreSlSetConstants(const void* values, const void* frame, const void* viewport)
{
    if (values != nullptr) {
        const auto* base = static_cast<const SlBaseStructure*>(values);
        if (base->structType.data1 == kStructConstants) {
            apusrCaptureConstantsForKey(ApusrStreamlineKey{frame, apusrViewportValue(viewport)}, *reinterpret_cast<const SlConstants*>(values));
            if (g_apusrOwnsStreamlineDlss.load(std::memory_order_acquire)) {
                bool expected = false;
                if (g_apusrLoggedSkippedOwnedConstantsForward.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                    apusr::host_discovery::event("streamline", "core.constants.forward.skipped",
                                                 "reason=apusr_owns_dlss_sr_contract");
                }
                return kSlResultOk;
            }
        }
    }
    return g_coreSlSetConstants ? g_coreSlSetConstants(values, frame, viewport) : kSlResultOk;
}

int __cdecl apusrCoreSlEvaluateFeature(std::uint32_t feature, const void* frame, const void* const* inputs, std::uint32_t numInputs, void* cmdBuffer)
{
    // DLSS success/failure is already sampled with dimensions by apusrEvaluateDlss().
    // Duplicating a second per-evaluation ingress event only makes the flight recorder noisy.
    if (feature != kFeatureDLSS) {
        apusr::host_discovery::eventf("streamline", "core.evaluate.called", "feature=%u frame=%p inputs=%u cmd=%p", feature, frame, numInputs, cmdBuffer);
    }
    if (feature == kFeatureDLSS) {
        if (currentStreamlineRenderApi() == kSlRenderApiVulkan) {
            return g_coreSlEvaluateFeature ? g_coreSlEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer) : kSlResultFeatureMissing;
        }
        return apusrEvaluateDlss(frame, inputs, numInputs, cmdBuffer);
    }
    if (apusrStreamlineFeatureShouldNoopIfSelected(feature)) {
        return kSlResultOk;
    }
    return g_coreSlEvaluateFeature ? g_coreSlEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer) : kSlResultFeatureMissing;
}

int __cdecl apusrCoreSlAllocateResources(void* cmdBuffer, std::uint32_t feature, const void* viewport)
{
    if (feature == kFeatureDLSS) {
        if (currentStreamlineRenderApi() == kSlRenderApiVulkan) {
            return g_coreSlAllocateResources ? g_coreSlAllocateResources(cmdBuffer, feature, viewport) : kSlResultFeatureMissing;
        }
        apusr::host_discovery::eventf("streamline", "core.allocate_resources.apusr", "viewport=%u cmd=%p", apusrViewportValue(viewport), cmdBuffer);
        return kSlResultOk;
    }
    if (apusrStreamlineFeatureShouldNoopIfSelected(feature)) {
        return kSlResultOk;
    }
    return g_coreSlAllocateResources ? g_coreSlAllocateResources(cmdBuffer, feature, viewport) : kSlResultFeatureMissing;
}

int __cdecl apusrCoreSlFreeResources(std::uint32_t feature, const void* viewport)
{
    if (feature == kFeatureDLSS) {
        if (currentStreamlineRenderApi() == kSlRenderApiVulkan) {
            return g_coreSlFreeResources ? g_coreSlFreeResources(feature, viewport) : kSlResultFeatureMissing;
        }
        apusr::host_discovery::eventf("streamline", "core.free_resources.apusr", "viewport=%u", apusrViewportValue(viewport));
        return kSlResultOk;
    }
    if (apusrStreamlineFeatureShouldNoopIfSelected(feature)) {
        return kSlResultOk;
    }
    return g_coreSlFreeResources ? g_coreSlFreeResources(feature, viewport) : kSlResultFeatureMissing;
}

FARPROC apusrCoreStreamlineProc(LPCSTR procName)
{
    (void)procName;
    // D3D12 now follows the same stock-semantic ownership rule as Vulkan:
    // sl.interposer/sl.common/sl.dlss own Streamline execution and APUSR begins
    // at the NGX boundary.  Keeping this resolver inert prevents accidental
    // reintroduction of the proxy-command-list bypass that caused device loss.
    return nullptr;
}

[[nodiscard]] bool bootstrapFeatureRequested(const SlPreferencesBootstrapLoose& prefs, const std::uint32_t feature) noexcept
{
    if (prefs.featuresToLoad == nullptr) return false;
    for (std::uint32_t i = 0; i < prefs.numFeaturesToLoad; ++i) {
        if (prefs.featuresToLoad[i] == feature) return true;
    }
    return false;
}

[[nodiscard]] std::wstring moduleDirectory(HMODULE module)
{
    if (module == nullptr) return {};
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring value(path, path + length);
    const std::size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : value.substr(0, slash + 1u);
}


[[nodiscard]] bool requestLooksLikeVulkanLoaderA(LPCSTR name) noexcept
{
    const char* base = requestBaseNameA(name);
    return base != nullptr && _stricmp(base, "vulkan-1.dll") == 0;
}

[[nodiscard]] bool requestLooksLikeVulkanLoaderW(LPCWSTR name) noexcept
{
    const wchar_t* base = requestBaseNameW(name);
    return base != nullptr && _wcsicmp(base, L"vulkan-1.dll") == 0;
}

[[nodiscard]] bool ensureEarlyVulkanHostCapture(const char* reason)
{
    if (g_vulkanEarlyCaptureReady.load(std::memory_order_acquire)) {
        return true;
    }

    if (g_vulkanEarlyCaptureInstallingOnThisThread) {
        if (!g_loggedVulkanEarlyCaptureReentrant.exchange(true, std::memory_order_acq_rel)) {
            apusr::host_discovery::eventf(
                "vulkan", "capture.early_reentrant_suppressed",
                "reason=%s action=skip_nested_install",
                reason != nullptr ? reason : "unknown");
        }
        return false;
    }

    struct ScopedEarlyCaptureInstall final {
        ScopedEarlyCaptureInstall() noexcept
        {
            g_vulkanEarlyCaptureInstallingOnThisThread = true;
        }
        ~ScopedEarlyCaptureInstall()
        {
            g_vulkanEarlyCaptureInstallingOnThisThread = false;
        }
    } installScope;

    std::lock_guard<std::mutex> lock(g_vulkanEarlyCaptureMutex);
    if (g_vulkanEarlyCaptureReady.load(std::memory_order_relaxed)) {
        return true;
    }

    HMODULE module = g_vulkanEarlyCaptureModule;
    if (module == nullptr) {
        if (HMODULE loaded = GetModuleHandleW(L"apusr_vulkan_host_capture.dll")) {
            module = loaded;
        } else {
            const std::wstring path =
                moduleDirectory(reinterpret_cast<HMODULE>(&__ImageBase)) +
                L"apusr_vulkan_host_capture.dll";
            if (!path.empty()) {
                module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (module != nullptr) {
                    apusr::host_discovery::eventf(
                        "vulkan", "capture.early_module_loaded",
                        "reason=%s path=%s module=%p",
                        reason != nullptr ? reason : "unknown",
                        apusr::host_discovery::narrowBestEffort(path.c_str()).c_str(),
                        module);
                }
            }
        }
        if (module == nullptr) {
            apusr::host_discovery::eventf(
                "vulkan", "capture.early_failed",
                "reason=%s stage=load_module",
                reason != nullptr ? reason : "unknown");
            return false;
        }
        g_vulkanEarlyCaptureModule = module;
    }

    using InstallFn = bool(__cdecl*)();
    const auto install = reinterpret_cast<InstallFn>(
        GetProcAddress(module, "apusrInstallVulkanHostCapture"));
    if (install == nullptr) {
        apusr::host_discovery::eventf(
            "vulkan", "capture.early_failed",
            "reason=%s stage=resolve_install module=%p",
            reason != nullptr ? reason : "unknown", module);
        return false;
    }

    const bool installed = install();
    if (installed) {
        g_vulkanEarlyCaptureReady.store(true, std::memory_order_release);
    }
    apusr::host_discovery::eventf(
        "vulkan", installed ? "capture.early_ready" : "capture.early_failed",
        "reason=%s stage=install module=%p",
        reason != nullptr ? reason : "unknown", module);
    return installed;
}

[[nodiscard]] HMODULE loadStreamlineAttachForBootstrap()
{
    if (HMODULE loaded = GetModuleHandleW(L"apusr_streamline_attach.dll")) return loaded;

    std::array<std::wstring, 2> candidates{};
    candidates[0] = moduleDirectory(reinterpret_cast<HMODULE>(&__ImageBase)) + L"apusr_streamline_attach.dll";
    candidates[1] = moduleDirectory(GetModuleHandleW(L"sl.interposer.dll")) + L"apusr_streamline_attach.dll";
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        HMODULE module = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module != nullptr) {
            apusr::host_discovery::eventf(
                "streamline", "attach.bootstrap.module_loaded",
                "path=%s module=%p", apusr::host_discovery::narrowBestEffort(candidate.c_str()).c_str(), module);
            return module;
        }
    }
    return nullptr;
}

[[nodiscard]] bool bootstrapStreamlineAttach(const std::uint32_t renderApi, const char* reason)
{
    if (renderApi != kSlRenderApiVulkan && renderApi != kSlRenderApiD3d12) return false;
    std::lock_guard<std::mutex> lock(g_streamlineBootstrapMutex);
    HMODULE module = g_streamlineAttachBootstrapModule;
    if (module == nullptr) {
        module = loadStreamlineAttachForBootstrap();
        if (module == nullptr) {
            apusr::host_discovery::eventf(
                "streamline", "attach.bootstrap.failed", "render_api=%u reason=%s stage=load_module",
                renderApi, reason != nullptr ? reason : "unknown");
            return false;
        }
        g_streamlineAttachBootstrapModule = module;
    }
    const auto hint = reinterpret_cast<ApusrStreamlineAttachHintRenderApiFn>(
        GetProcAddress(module, "apusrStreamlineAttachHintRenderApi"));
    const auto attach = reinterpret_cast<ApusrAttachToLoadedStreamlineFn>(
        GetProcAddress(module, "apusrAttachToLoadedStreamline"));
    if (hint == nullptr || attach == nullptr) {
        apusr::host_discovery::eventf(
            "streamline", "attach.bootstrap.failed", "render_api=%u reason=%s stage=resolve_private_exports",
            renderApi, reason != nullptr ? reason : "unknown");
        return false;
    }
    // Vulkan must arm the process-lifetime loader capture before stock slInit
    // creates VkInstance/VkDevice. D3D12 uses the same attachment only when the
    // host actually requested Reflex, leaving stock DLSS semantics untouched.
    hint(renderApi);
    const bool attached = attach();
    apusr::host_discovery::eventf(
        "streamline", "attach.bootstrap", "render_api=%u reason=%s attached=%u",
        renderApi, reason != nullptr ? reason : "unknown", attached ? 1u : 0u);
    return attached;
}

[[nodiscard]] bool rearmD3d12ReflexAfterRendererRestart(void* nativeDevice)
{
    if (nativeDevice == nullptr ||
        !g_streamlineReflexRequested.load(std::memory_order_acquire)) {
        return false;
    }
    if (!bootstrapStreamlineAttach(kSlRenderApiD3d12, "plugin_startup_d3d12_reflex_rearm")) {
        return false;
    }

    HMODULE module = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_streamlineBootstrapMutex);
        module = g_streamlineAttachBootstrapModule;
    }
    const auto bind = module != nullptr ?
        reinterpret_cast<ApusrStreamlineAttachBindD3D12NativeDeviceFn>(
            GetProcAddress(module, "apusrStreamlineAttachBindD3D12NativeDevice")) : nullptr;
    const bool ready = bind != nullptr && bind(nativeDevice);
    apusr::host_discovery::eventf(
        "streamline", "attach.d3d12_reflex_rearmed",
        "ready=%u source=sl.dlss_plugin_startup native_device=%p",
        ready ? 1u : 0u, nativeDevice);
    return ready;
}

int __cdecl hkSlInit(const void* preferences, const std::uint64_t sdkVersion)
{
    constexpr std::uint64_t kSlSdkVersionMagic = 0xfedcull;
    std::uint32_t renderApi = kSlRenderApiUnknown;
    bool reflexRequested = false;
    if ((sdkVersion & 0xffffull) == kSlSdkVersionMagic && preferences != nullptr) {
        const auto& prefs = *static_cast<const SlPreferencesBootstrapLoose*>(preferences);
        if (prefs.renderAPI <= kSlRenderApiVulkan) renderApi = prefs.renderAPI;
        reflexRequested = bootstrapFeatureRequested(prefs, kFeatureReflex);
        g_streamlineReflexRequested.store(reflexRequested, std::memory_order_release);
    }

    // Preferences::renderAPI is the earliest authoritative renderer signal in
    // this DLL. Publish it locally before any stock slInit work so DXGI adapter
    // enumeration during Vulkan system-capability discovery preserves the real
    // device id needed for VkPhysicalDevice correlation. The attachment DLL has
    // its own renderer state and receives the same value through its private
    // hint; neither DLL may infer active renderer from feature-support flags.
    if (renderApi != kSlRenderApiUnknown) {
        g_apusrStreamlineRenderApi.store(renderApi, std::memory_order_release);
    }

    if (renderApi == kSlRenderApiVulkan) {
        (void)bootstrapStreamlineAttach(renderApi, "slInit_vulkan_pre_device");
    } else if (renderApi == kSlRenderApiD3d12 && reflexRequested) {
        (void)bootstrapStreamlineAttach(renderApi, "slInit_d3d12_reflex");
    }

    const auto stock = g_stockSlInit.load(std::memory_order_acquire);
    if (stock == nullptr) return kSlResultFeatureMissing;
    return stock(preferences, sdkVersion);
}

FARPROC WINAPI hkGetProcAddress(HMODULE module, LPCSTR procName)
{
    // A host may build a persistent Vulkan dispatch table before Streamline slInit
    // call.  Once vkCreateDevice has been cached from the original loader, a
    // later Detours install cannot recover that call site.  Trigger passive
    // Vulkan capture before returning any proc from vulkan-1.dll.
    if (moduleBaseNameEquals(module, L"vulkan-1.dll") &&
        procName != nullptr && reinterpret_cast<std::uintptr_t>(procName) > 0xFFFFu) {
        (void)ensureEarlyVulkanHostCapture("vulkan_loader_getproc");
    }

    FARPROC result = g_getProcAddress ? g_getProcAddress(module, procName) : nullptr;
    const bool streamlineVulkan = currentStreamlineRenderApi() == kSlRenderApiVulkan;
    if (!streamlineVulkan && procName != nullptr && reinterpret_cast<std::uintptr_t>(procName) > 0xFFFFu) {
        if (moduleBaseNameEquals(module, L"sl.interposer.dll")) {
            if (std::strcmp(procName, "slInit") == 0 && result != nullptr) {
                g_stockSlInit.store(reinterpret_cast<SlInitFn>(result), std::memory_order_release);
                apusr::host_discovery::event("streamline", "slInit.bootstrap_hooked");
                return reinterpret_cast<FARPROC>(&hkSlInit);
            }
            if (FARPROC replacement = apusrCoreStreamlineProc(procName)) {
                apusr::host_discovery::eventf("streamline", "core.getproc.redirect", "name=%s original=%p replacement=%p", procName, result, replacement);
                return replacement;
            }
        }
    }
    if (procName != nullptr && reinterpret_cast<std::uintptr_t>(procName) > 0xFFFFu && result != nullptr) {
        if (!streamlineVulkan && moduleLooksLikeGdi32(module)) {
            if (std::strcmp(procName, "D3DKMTQueryAdapterInfo") == 0) {
                if (g_d3dkmtQueryAdapterInfo == nullptr) g_d3dkmtQueryAdapterInfo = reinterpret_cast<D3DKMTQueryAdapterInfoFn>(result);
                return reinterpret_cast<FARPROC>(&hkD3DKMTQueryAdapterInfo);
            }
            if (std::strcmp(procName, "D3DKMTEnumAdapters") == 0) {
                if (g_d3dkmtEnumAdapters == nullptr) g_d3dkmtEnumAdapters = reinterpret_cast<D3DKMTEnumAdaptersFn>(result);
                return reinterpret_cast<FARPROC>(&hkD3DKMTEnumAdapters);
            }
            if (std::strcmp(procName, "D3DKMTEnumAdapters2") == 0) {
                if (g_d3dkmtEnumAdapters2 == nullptr) g_d3dkmtEnumAdapters2 = reinterpret_cast<D3DKMTEnumAdapters2Fn>(result);
                return reinterpret_cast<FARPROC>(&hkD3DKMTEnumAdapters2);
            }
            if (std::strcmp(procName, "D3DKMTEnumAdapters3") == 0) {
                if (g_d3dkmtEnumAdapters3 == nullptr) g_d3dkmtEnumAdapters3 = reinterpret_cast<D3DKMTEnumAdapters3Fn>(result);
                return reinterpret_cast<FARPROC>(&hkD3DKMTEnumAdapters3);
            }
            if (std::strcmp(procName, "D3DKMTCloseAdapter") == 0) {
                if (g_d3dkmtCloseAdapter == nullptr) g_d3dkmtCloseAdapter = reinterpret_cast<D3DKMTCloseAdapterFn>(result);
                return reinterpret_cast<FARPROC>(&hkD3DKMTCloseAdapter);
            }
        }
        if (std::strcmp(procName, "slGetPluginFunction") == 0 && moduleLooksLikeDlssPlugin(module)) {
            apusr::host_discovery::event("streamline", "sl.dlss.get_plugin_function.hooked");
            g_slGetPluginFunction = reinterpret_cast<SlGetPluginFunctionFn>(result);
            return reinterpret_cast<FARPROC>(&hkSlGetPluginFunction);
        }
    }
    return result;
}

HMODULE WINAPI hkLoadLibraryA(LPCSTR name)
{
    const bool relevant = requestLooksRelevantA(name);
    HMODULE module = g_loadLibraryA ? g_loadLibraryA(name) : nullptr;
    if (module == nullptr) {
        module = loadLocalFrontendFallbackA(name, 0, "LoadLibraryA");
    }
    if (relevant) {
        apusr::host_discovery::eventf("loader", "LoadLibraryA", "request=%s result=%p", name ? name : "<null>", module);
    }
    if (module != nullptr && requestLooksLikeVulkanLoaderA(name) &&
        !g_vulkanEarlyCaptureInstallingOnThisThread) {
        (void)ensureEarlyVulkanHostCapture("LoadLibraryA_vulkan_loader");
    }
    if (!g_vulkanEarlyCaptureInstallingOnThisThread) {
        patchLoadedModules();
    }
    return module;
}

HMODULE WINAPI hkLoadLibraryW(LPCWSTR name)
{
    const bool relevant = requestLooksRelevantW(name);
    HMODULE module = g_loadLibraryW ? g_loadLibraryW(name) : nullptr;
    if (module == nullptr) {
        module = loadLocalFrontendFallbackW(name, 0, "LoadLibraryW");
    }
    if (relevant) {
        const std::string req = apusr::host_discovery::narrowBestEffort(name);
        apusr::host_discovery::eventf("loader", "LoadLibraryW", "request=%s result=%p", req.c_str(), module);
    }
    if (module != nullptr && requestLooksLikeVulkanLoaderW(name) &&
        !g_vulkanEarlyCaptureInstallingOnThisThread) {
        (void)ensureEarlyVulkanHostCapture("LoadLibraryW_vulkan_loader");
    }
    if (!g_vulkanEarlyCaptureInstallingOnThisThread) {
        patchLoadedModules();
    }
    return module;
}

HMODULE WINAPI hkLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    const bool relevant = requestLooksRelevantA(name);
    HMODULE module = g_loadLibraryExA ? g_loadLibraryExA(name, file, flags) : nullptr;
    if (module == nullptr && file == nullptr) {
        module = loadLocalFrontendFallbackA(name, flags, "LoadLibraryExA");
    }
    if (relevant) {
        apusr::host_discovery::eventf("loader", "LoadLibraryExA", "request=%s flags=0x%08lX result=%p", name ? name : "<null>", static_cast<unsigned long>(flags), module);
    }
    if (module != nullptr && requestLooksLikeVulkanLoaderA(name) &&
        !g_vulkanEarlyCaptureInstallingOnThisThread) {
        (void)ensureEarlyVulkanHostCapture("LoadLibraryExA_vulkan_loader");
    }
    if (!g_vulkanEarlyCaptureInstallingOnThisThread) {
        patchLoadedModules();
    }
    return module;
}

HMODULE WINAPI hkLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    const bool relevant = requestLooksRelevantW(name);
    HMODULE module = g_loadLibraryExW ? g_loadLibraryExW(name, file, flags) : nullptr;
    if (module == nullptr && file == nullptr) {
        module = loadLocalFrontendFallbackW(name, flags, "LoadLibraryExW");
    }
    if (relevant) {
        const std::string req = apusr::host_discovery::narrowBestEffort(name);
        apusr::host_discovery::eventf("loader", "LoadLibraryExW", "request=%s flags=0x%08lX result=%p", req.c_str(), static_cast<unsigned long>(flags), module);
    }
    if (module != nullptr && requestLooksLikeVulkanLoaderW(name) &&
        !g_vulkanEarlyCaptureInstallingOnThisThread) {
        (void)ensureEarlyVulkanHostCapture("LoadLibraryExW_vulkan_loader");
    }
    if (!g_vulkanEarlyCaptureInstallingOnThisThread) {
        patchLoadedModules();
    }
    return module;
}

[[nodiscard]] bool patchSlot(void** slot, void* replacement)
{
    if (slot == nullptr) return false;
    if (*slot == replacement) return true;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    *slot = replacement;
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    return true;
}

bool patchImportByName(
    HMODULE module,
    const char* dllName,
    const char* importName,
    void* replacement,
    void** original)
{
    if (module == nullptr || isOurModule(module) || dllName == nullptr || importName == nullptr || replacement == nullptr) return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return false;

    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; imports->Name != 0; ++imports) {
        const char* importedDll = reinterpret_cast<const char*>(base + imports->Name);
        if (!equalsIgnoreCaseAscii(importedDll, dllName)) continue;
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->FirstThunk);
        auto* names = imports->OriginalFirstThunk != 0 ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imports->OriginalFirstThunk) : thunk;
        for (; thunk->u1.Function != 0 && names->u1.AddressOfData != 0; ++thunk, ++names) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), importName) != 0) continue;
            void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            if (original != nullptr && *original == nullptr && *slot != replacement) {
                *original = *slot;
            }
            return patchSlot(slot, replacement);
        }
    }
    return false;
}

void patchModule(HMODULE module)
{
    if (module == nullptr || isOurModule(module)) return;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        if (g_patchedModules.find(module) != g_patchedModules.end()) return;
        g_patchedModules.insert(module);
    }

    logRelevantModuleSeen(module);

    // Loader interception is renderer-neutral and must remain active in Vulkan.
    // Some hosts load renderer helpers after slInit and resolve Vulkan entry
    // points there; returning before these IAT hooks are installed would let
    // those modules cache raw loader trampolines outside process-lifetime
    // capture. The hook bodies themselves keep D3D identity behavior gated on
    // the explicit renderer.
    patchImportByName(module, "KERNEL32.dll", "GetProcAddress", reinterpret_cast<void*>(&hkGetProcAddress), reinterpret_cast<void**>(&g_getProcAddress));
    patchImportByName(module, "KERNEL32.dll", "LoadLibraryA", reinterpret_cast<void*>(&hkLoadLibraryA), reinterpret_cast<void**>(&g_loadLibraryA));
    patchImportByName(module, "KERNEL32.dll", "LoadLibraryW", reinterpret_cast<void*>(&hkLoadLibraryW), reinterpret_cast<void**>(&g_loadLibraryW));
    patchImportByName(module, "KERNEL32.dll", "LoadLibraryExA", reinterpret_cast<void*>(&hkLoadLibraryExA), reinterpret_cast<void**>(&g_loadLibraryExA));
    patchImportByName(module, "KERNEL32.dll", "LoadLibraryExW", reinterpret_cast<void*>(&hkLoadLibraryExW), reinterpret_cast<void**>(&g_loadLibraryExW));

    const bool streamlineVulkan = currentStreamlineRenderApi() == kSlRenderApiVulkan;
    if (streamlineVulkan) {
        // Vulkan uses its own requirement-sanitization/host-capture attachment.
        // Keep only D3D identity compatibility hooks out of that renderer; the
        // renderer-neutral loader hooks above remain active.
        return;
    }

    // DX Streamline discovery still needs sl.common to see the APUSR adapter proxy.
    if (moduleBaseNameEquals(module, L"sl.common.dll")) {
        patchImportByName(module, "dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&apusr_CreateDXGIFactory), nullptr);
        patchImportByName(module, "dxgi.dll", "CreateDXGIFactory1", reinterpret_cast<void*>(&apusr_CreateDXGIFactory1), nullptr);
        patchImportByName(module, "dxgi.dll", "CreateDXGIFactory2", reinterpret_cast<void*>(&apusr_CreateDXGIFactory2), nullptr);
    }

    patchImportByName(module, "USER32.dll", "EnumDisplayDevicesA", reinterpret_cast<void*>(&hkEnumDisplayDevicesA), reinterpret_cast<void**>(&g_enumDisplayDevicesA));
    patchImportByName(module, "USER32.dll", "EnumDisplayDevicesW", reinterpret_cast<void*>(&hkEnumDisplayDevicesW), reinterpret_cast<void**>(&g_enumDisplayDevicesW));
    patchImportByName(module, "ADVAPI32.dll", "RegOpenKeyExW", reinterpret_cast<void*>(&hkRegOpenKeyExW), reinterpret_cast<void**>(&g_regOpenKeyExW));
    patchImportByName(module, "ADVAPI32.dll", "RegEnumValueW", reinterpret_cast<void*>(&hkRegEnumValueW), reinterpret_cast<void**>(&g_regEnumValueW));
    patchImportByName(module, "ADVAPI32.dll", "RegCloseKey", reinterpret_cast<void*>(&hkRegCloseKey), reinterpret_cast<void**>(&g_regCloseKey));
    patchImportByName(module, "ADVAPI32.dll", "RegQueryValueExW", reinterpret_cast<void*>(&hkRegQueryValueExW), reinterpret_cast<void**>(&g_regQueryValueExW));
    patchImportByName(module, "ADVAPI32.dll", "RegQueryValueExA", reinterpret_cast<void*>(&hkRegQueryValueExA), reinterpret_cast<void**>(&g_regQueryValueExA));
    patchImportByName(module, "ADVAPI32.dll", "RegGetValueW", reinterpret_cast<void*>(&hkRegGetValueW), reinterpret_cast<void**>(&g_regGetValueW));
    patchImportByName(module, "GDI32.dll", "D3DKMTQueryAdapterInfo", reinterpret_cast<void*>(&hkD3DKMTQueryAdapterInfo), reinterpret_cast<void**>(&g_d3dkmtQueryAdapterInfo));
    patchImportByName(module, "GDI32.dll", "D3DKMTEnumAdapters", reinterpret_cast<void*>(&hkD3DKMTEnumAdapters), reinterpret_cast<void**>(&g_d3dkmtEnumAdapters));
    patchImportByName(module, "GDI32.dll", "D3DKMTEnumAdapters2", reinterpret_cast<void*>(&hkD3DKMTEnumAdapters2), reinterpret_cast<void**>(&g_d3dkmtEnumAdapters2));
    patchImportByName(module, "GDI32.dll", "D3DKMTEnumAdapters3", reinterpret_cast<void*>(&hkD3DKMTEnumAdapters3), reinterpret_cast<void**>(&g_d3dkmtEnumAdapters3));
    patchImportByName(module, "GDI32.dll", "D3DKMTCloseAdapter", reinterpret_cast<void*>(&hkD3DKMTCloseAdapter), reinterpret_cast<void**>(&g_d3dkmtCloseAdapter));

    // Only discovery/support callbacks are wrapped.  The plugin's actual DLSS
    // options/resource/evaluate functions remain stock so commonInterface can
    // unwrap D3D12 proxies, perform transitions, and restore host pipeline state.
    patchImportByName(module, "sl.common.dll", "slGetPluginFunction", reinterpret_cast<void*>(&hkSlGetPluginFunction), reinterpret_cast<void**>(&g_slGetPluginFunction));
}

void patchLoadedModules()
{
    // Capture installation may itself cross loader boundaries. Defer all module
    // patching until the outer host loader call resumes; mutating IATs in the
    // middle of a Detours installation is both unnecessary and reentrancy-prone.
    if (g_vulkanEarlyCaptureInstallingOnThisThread) {
        return;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        patchModule(GetModuleHandleW(nullptr));
        patchModule(GetModuleHandleW(L"sl.common.dll"));
        patchModule(GetModuleHandleW(L"sl.dlss.dll"));
        return;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            patchModule(entry.hModule);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    // If the host loaded the Vulkan loader before our first DXGI-triggered hook
    // refresh, arm capture now. This cannot retroactively replace a raw function
    // pointer that was already cached, but it closes the common preloaded-loader
    // case and makes the remaining race visible in discovery logs.
    if (GetModuleHandleW(L"vulkan-1.dll") != nullptr &&
        !g_vulkanEarlyCaptureReady.load(std::memory_order_acquire)) {
        (void)ensureEarlyVulkanHostCapture("patch_loaded_modules_preloaded_vulkan");
    }
}

} // namespace

extern "C" bool apusrInstallIdentitySpoofLoaderHooksEarly()
{
    // Some hosts load sl.interposer.dll and call slInit before stock Streamline
    // makes its first CreateDXGIFactory call. The ordinary lazy installation
    // point is therefore already inside stock slInit and cannot replace the
    // function pointer the host just cached. Patch only the main
    // executable's renderer-neutral loader imports while the DXGI proxy is
    // attaching; hkLoadLibrary* can then refresh the full module set after the
    // loader lock has been released, and hkGetProcAddress can return hkSlInit.
    //
    // Do not invoke the full module refresh here.  This function runs from DllMain
    // and deliberately avoids snapshots, LoadLibrary, Detours, renderer
    // identity hooks, and the Streamline attachment itself.
    HMODULE mainModule = GetModuleHandleW(nullptr);
    if (mainModule == nullptr) {
        apusr::host_discovery::event(
            "loader", "early_main_hooks.failed", "reason=main_module_missing");
        return false;
    }

    const bool getProcAddressHooked = patchImportByName(
        mainModule, "KERNEL32.dll", "GetProcAddress",
        reinterpret_cast<void*>(&hkGetProcAddress),
        reinterpret_cast<void**>(&g_getProcAddress));
    const bool loadLibraryAHooked = patchImportByName(
        mainModule, "KERNEL32.dll", "LoadLibraryA",
        reinterpret_cast<void*>(&hkLoadLibraryA),
        reinterpret_cast<void**>(&g_loadLibraryA));
    const bool loadLibraryWHooked = patchImportByName(
        mainModule, "KERNEL32.dll", "LoadLibraryW",
        reinterpret_cast<void*>(&hkLoadLibraryW),
        reinterpret_cast<void**>(&g_loadLibraryW));
    const bool loadLibraryExAHooked = patchImportByName(
        mainModule, "KERNEL32.dll", "LoadLibraryExA",
        reinterpret_cast<void*>(&hkLoadLibraryExA),
        reinterpret_cast<void**>(&g_loadLibraryExA));
    const bool loadLibraryExWHooked = patchImportByName(
        mainModule, "KERNEL32.dll", "LoadLibraryExW",
        reinterpret_cast<void*>(&hkLoadLibraryExW),
        reinterpret_cast<void**>(&g_loadLibraryExW));

    const bool loadHooked = loadLibraryAHooked || loadLibraryWHooked ||
        loadLibraryExAHooked || loadLibraryExWHooked;
    const bool ready = getProcAddressHooked && loadHooked;
    apusr::host_discovery::eventf(
        "loader", ready ? "early_main_hooks.ready" : "early_main_hooks.incomplete",
        "main=%p get_proc=%u load_a=%u load_w=%u load_ex_a=%u load_ex_w=%u renderer_identity=deferred",
        mainModule,
        getProcAddressHooked ? 1u : 0u,
        loadLibraryAHooked ? 1u : 0u,
        loadLibraryWHooked ? 1u : 0u,
        loadLibraryExAHooked ? 1u : 0u,
        loadLibraryExWHooked ? 1u : 0u);
    return ready;
}

extern "C" void apusrIdentitySpoofSetStreamlineRenderApi(const std::uint32_t renderApi)
{
    g_apusrStreamlineRenderApi.store(renderApi, std::memory_order_release);
    if (renderApi == kSlRenderApiVulkan) {
        // Vulkan device/queue capture is owned by the pinned loader boundary,
        // while stock Streamline owns Vulkan DLSS translation. This D3D identity
        // compatibility layer must not replace the real AMD Vulkan adapter identity.
        g_apusrOwnsStreamlineDlss.store(false, std::memory_order_release);
    }
}

extern "C" std::uint32_t apusrIdentitySpoofGetStreamlineRenderApi()
{
    return currentStreamlineRenderApi();
}

extern "C" void apusrRefreshIdentitySpoofHooks()
{
    bool expected = false;
    if (!g_installing.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    patchLoadedModules();
    g_installedOnce.store(true, std::memory_order_release);
    g_installing.store(false, std::memory_order_release);
}

extern "C" void apusrInstallIdentitySpoofHooks()
{
    if (!g_installedOnce.load(std::memory_order_acquire)) {
        apusrRefreshIdentitySpoofHooks();
    }
}

#endif // _WIN32
