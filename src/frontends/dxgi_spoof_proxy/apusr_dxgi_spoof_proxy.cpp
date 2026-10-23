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
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#include <dxgi1_6.h>

#include "../identity_spoof/apusr_identity_spoof_hooks.h"
#include "../common/apusr_host_discovery_log.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

constexpr wchar_t kOriginalDxgiUnusedName[] = L"dxgi.dll";
constexpr std::uint32_t kFeatureDLSS = 0u;
constexpr std::uint32_t kFeatureReflex = 3u;
constexpr std::uint32_t kFeaturePCL = 4u;
constexpr std::uint32_t kFeatureDLSS_G = 1000u;
constexpr std::uint32_t kFeatureDLSS_RR = 1001u;

constexpr bool apusrStreamlineFeatureSupported(std::uint32_t feature) noexcept
{
    return feature == kFeatureDLSS;
}

constexpr bool apusrStreamlineFeatureExplicitlyUnsupported(std::uint32_t feature) noexcept
{
    return feature == kFeatureDLSS_G || feature == kFeatureDLSS_RR || feature == kFeatureReflex || feature == kFeaturePCL;
}

constexpr bool apusrStreamlineFeatureShouldNoopIfSelected(std::uint32_t feature) noexcept
{
    return apusrStreamlineFeatureExplicitlyUnsupported(feature);
}

constexpr int kResultOk = 0;
constexpr int kResultFeatureMissing = 31;       // sl::Result::eErrorFeatureMissing
constexpr int kResultFeatureFailedToLoad = 34;  // sl::Result::eErrorFeatureFailedToLoad
constexpr int kResultInvalidParameter = 25;     // sl::Result::eErrorInvalidParameter
constexpr int kResultMissingInputParameter = 20; // sl::Result::eErrorMissingInputParameter
constexpr UINT kSpoofVendorNvidia = 0x10DEu;
constexpr UINT kSpoofDeviceRTX3090 = 0x2204u;
constexpr UINT kVendorMicrosoft = 0x1414u;
constexpr UINT64 kSpoofDedicatedVideoMemory = 16ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t kSlRenderApiD3d12 = 1u;
constexpr std::uint32_t kSlRenderApiVulkan = 2u;
constexpr std::uint32_t kSlRenderApiUnknown = 0xffffffffu;

struct StructType {
    std::uint32_t data1;
    std::uint16_t data2;
    std::uint16_t data3;
    std::uint8_t data4[8];
};

struct BaseStructure {
    BaseStructure* next;
    StructType structType;
    std::size_t structVersion;
};

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

struct FeatureVersion : BaseStructure {
    Version versionSL;
    Version versionNGX;
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
    // compiler padding follows before the preset uint32 fields.
};

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
using SlSetVulkanInfoFn = int(__cdecl*)(const void* info);
using SlUpgradeInterfaceFn = int(__cdecl*)(void** baseInterface);
using SlGetNativeInterfaceFn = int(__cdecl*)(void* proxyInterface, void** baseInterface);

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

HMODULE g_original = nullptr;
std::atomic<bool> g_dlssLoaded{true};
void* g_d3dDevice = nullptr;
std::mutex g_adapterProxyMutex;
std::unordered_map<void*, IUnknown*> g_adapterProxyToInner;
std::atomic<bool> g_loggedDxgiSpoof{false};
std::atomic<bool> g_loggedVulkanDxgiIdentity{false};
std::atomic<bool> g_loggedUnknownIdentityPassthrough{false};
std::atomic<bool> g_loggedVulkanHostIdentityPassthrough{false};
std::atomic<bool> g_loggedVulkanStreamlineIdentitySpoof{false};

constexpr std::uint32_t kRequiredDlssTags[] = {0u, 1u, 3u, 4u};

bool isSelfModule(HMODULE module)
{
    return module == reinterpret_cast<HMODULE>(&__ImageBase);
}

bool samePathCaseInsensitive(const wchar_t* a, const wchar_t* b)
{
    if (a == nullptr || b == nullptr || *a == L'\0' || *b == L'\0') {
        return false;
    }
    return _wcsicmp(a, b) == 0;
}

bool isSelfPath(HMODULE module)
{
    if (module == nullptr) {
        return false;
    }
    if (isSelfModule(module)) {
        return true;
    }
    wchar_t modulePath[MAX_PATH] = {};
    wchar_t selfPath[MAX_PATH] = {};
    const DWORD moduleLen = GetModuleFileNameW(module, modulePath, MAX_PATH);
    const DWORD selfLen = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), selfPath, MAX_PATH);
    return moduleLen != 0 && moduleLen < MAX_PATH && selfLen != 0 && selfLen < MAX_PATH &&
           samePathCaseInsensitive(modulePath, selfPath);
}

HMODULE loadSystemDxgi()
{
    if (g_original != nullptr) {
        return g_original;
    }

    wchar_t systemPath[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(systemPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return nullptr;
    }
    if (wcscat_s(systemPath, L"\\dxgi.dll") != 0) {
        return nullptr;
    }

    HMODULE candidate = LoadLibraryW(systemPath);
    if (candidate == nullptr) {
        candidate = LoadLibraryExW(systemPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    // A local dxgi.dll proxy must never resolve its original entrypoints from itself.
    // If the loader hands us this module, calling CreateDXGIFactory would recurse until
    // stack overflow before the game reaches its renderer.  Fail closed instead.
    if (isSelfPath(candidate)) {
        candidate = nullptr;
    }

    g_original = candidate;
    return g_original;
}

FARPROC originalProc(const char* name)
{
    HMODULE mod = loadSystemDxgi();
    return mod != nullptr ? GetProcAddress(mod, name) : nullptr;
}

template <typename Fn>
Fn originalFn(const char* name)
{
    return reinterpret_cast<Fn>(originalProc(name));
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

#if defined(_MSC_VER)
#define APUSR_CALLER_ADDRESS() _ReturnAddress()
#elif defined(__clang__) || defined(__GNUC__)
#define APUSR_CALLER_ADDRESS() __builtin_return_address(0)
#else
#define APUSR_CALLER_ADDRESS() nullptr
#endif

bool moduleAddressHasBaseName(void* address, const wchar_t* expected)
{
    if (address == nullptr || expected == nullptr) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(address), &module) || module == nullptr) {
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base != nullptr ? base + 1 : path;
    return _wcsicmp(base, expected) == 0;
}

bool isStreamlineIdentityConsumer(void* address)
{
    return moduleAddressHasBaseName(address, L"sl.common.dll") ||
           moduleAddressHasBaseName(address, L"sl.interposer.dll") ||
           moduleAddressHasBaseName(address, L"sl.dlss.dll");
}

bool shouldSpoofDxgiIdentityForCaller(void* address)
{
    const std::uint32_t renderApi = apusrIdentitySpoofGetStreamlineRenderApi();
    if (renderApi == kSlRenderApiD3d12) {
        return true;
    }
    if (renderApi == kSlRenderApiVulkan) {
        const bool streamlineConsumer = isStreamlineIdentityConsumer(address);
        if (streamlineConsumer) {
            bool expected = false;
            if (g_loggedVulkanStreamlineIdentitySpoof.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                apusr::host_discovery::event(
                    "dxgi", "adapter.desc.vulkan_streamline_scoped_spoof",
                    "scope=streamline vendor=nvidia device=real reason=stock_sl_common_vendor_gate");
            }
        } else {
            bool expected = false;
            if (g_loggedVulkanHostIdentityPassthrough.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
                apusr::host_discovery::event(
                    "dxgi", "adapter.desc.vulkan_host_passthrough",
                    "scope=host identity=real reason=avoid_cross_api_adapter_mismatch");
            }
        }
        return streamlineConsumer;
    }

    bool expected = false;
    if (renderApi == kSlRenderApiUnknown &&
        g_loggedUnknownIdentityPassthrough.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        apusr::host_discovery::event(
            "dxgi", "adapter.desc.pre_renderer_passthrough",
            "identity=real reason=renderer_not_selected");
    }
    return false;
}

[[nodiscard]] UINT spoofDeviceIdForCurrentRenderer(const UINT realDeviceId) noexcept
{
    // Streamline's Vulkan capability discovery correlates its DXGI adapter
    // record with VkPhysicalDeviceProperties::deviceID. Preserve the real AMD
    // device id once the host has explicitly selected Vulkan; only the vendor
    // gate needs to remain NVIDIA-shaped. D3D12 keeps the established
    // RTX-shaped device id used by the compatibility discovery path.
    if (apusrIdentitySpoofGetStreamlineRenderApi() == kSlRenderApiVulkan) {
        bool expected = false;
        if (g_loggedVulkanDxgiIdentity.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            apusr::host_discovery::eventf(
                "dxgi", "adapter.desc.vulkan_identity_bridge",
                "spoof_vendor=0x%04X preserve_device=0x%04X reason=streamline_vk_physical_device_correlation",
                kSpoofVendorNvidia, realDeviceId);
        }
        return realDeviceId;
    }
    return kSpoofDeviceRTX3090;
}

void logFirstDxgiSpoof(UINT realVendorId, UINT realDeviceId, const wchar_t* realDescription)
{
    bool expected = false;
    if (!g_loggedDxgiSpoof.compare_exchange_strong(expected, true)) {
        return;
    }
    char detail[512] = {};
    const std::string desc = apusr::host_discovery::narrowBestEffort(realDescription);
    const UINT spoofDeviceId = spoofDeviceIdForCurrentRenderer(realDeviceId);
    std::snprintf(detail, sizeof(detail),
                  "real_vendor=0x%04X real_device=0x%04X spoof_vendor=0x%04X spoof_device=0x%04X description=%s",
                  realVendorId, realDeviceId, kSpoofVendorNvidia, spoofDeviceId, desc.c_str());
    apusr::host_discovery::event("dxgi", "adapter.desc.spoofed", detail);
}

void spoofDesc(DXGI_ADAPTER_DESC* desc, const bool enabled)
{
    if (!enabled || desc == nullptr || !shouldSpoofVendor(desc->VendorId)) {
        return;
    }
    logFirstDxgiSpoof(desc->VendorId, desc->DeviceId, desc->Description);
    setSpoofName(desc->Description);
    desc->VendorId = kSpoofVendorNvidia;
    desc->DeviceId = spoofDeviceIdForCurrentRenderer(desc->DeviceId);
    desc->DedicatedVideoMemory = std::max<UINT64>(desc->DedicatedVideoMemory, kSpoofDedicatedVideoMemory);
}

void spoofDesc(DXGI_ADAPTER_DESC1* desc, const bool enabled)
{
    if (!enabled || desc == nullptr || !shouldSpoofVendor(desc->VendorId)) {
        return;
    }
    logFirstDxgiSpoof(desc->VendorId, desc->DeviceId, desc->Description);
    setSpoofName(desc->Description);
    desc->VendorId = kSpoofVendorNvidia;
    desc->DeviceId = spoofDeviceIdForCurrentRenderer(desc->DeviceId);
    desc->DedicatedVideoMemory = std::max<UINT64>(desc->DedicatedVideoMemory, kSpoofDedicatedVideoMemory);
    desc->Flags &= ~static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE);
}

void spoofDesc(DXGI_ADAPTER_DESC2* desc, const bool enabled)
{
    if (!enabled || desc == nullptr || !shouldSpoofVendor(desc->VendorId)) {
        return;
    }
    logFirstDxgiSpoof(desc->VendorId, desc->DeviceId, desc->Description);
    setSpoofName(desc->Description);
    desc->VendorId = kSpoofVendorNvidia;
    desc->DeviceId = spoofDeviceIdForCurrentRenderer(desc->DeviceId);
    desc->DedicatedVideoMemory = std::max<UINT64>(desc->DedicatedVideoMemory, kSpoofDedicatedVideoMemory);
    desc->Flags &= ~static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE);
}

void spoofDesc(DXGI_ADAPTER_DESC3* desc, const bool enabled)
{
    if (!enabled || desc == nullptr || !shouldSpoofVendor(desc->VendorId)) {
        return;
    }
    logFirstDxgiSpoof(desc->VendorId, desc->DeviceId, desc->Description);
    setSpoofName(desc->Description);
    desc->VendorId = kSpoofVendorNvidia;
    desc->DeviceId = spoofDeviceIdForCurrentRenderer(desc->DeviceId);
    desc->DedicatedVideoMemory = std::max<UINT64>(desc->DedicatedVideoMemory, kSpoofDedicatedVideoMemory);
}

void fillDlssRequirements(FeatureRequirements* requirements)
{
    if (requirements == nullptr) {
        return;
    }
    requirements->flags = (1u << 1); // D3D12 supported.
    requirements->maxNumCPUThreads = 0;
    requirements->maxNumViewports = 16;
    requirements->numRequiredTags = static_cast<std::uint32_t>(sizeof(kRequiredDlssTags) / sizeof(kRequiredDlssTags[0]));
    requirements->requiredTags = kRequiredDlssTags;
    requirements->osVersionRequired = {10u, 0u, 0u};
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
    requirements->vkNumOpticalFlowQueuesRequired = 0;
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
                spoofDesc(pDesc, shouldSpoofDxgiIdentityForCaller(APUSR_CALLER_ADDRESS()));
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
                spoofDesc(pDesc, shouldSpoofDxgiIdentityForCaller(APUSR_CALLER_ADDRESS()));
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
                spoofDesc(pDesc, shouldSpoofDxgiIdentityForCaller(APUSR_CALLER_ADDRESS()));
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
                spoofDesc(pDesc, shouldSpoofDxgiIdentityForCaller(APUSR_CALLER_ADDRESS()));
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

using RegisterD3D12QueueForSwapChainFn = bool(__cdecl*)(void* swapChain, void* commandQueue);

std::wstring localModuleSiblingPath(const wchar_t* fileName)
{
    wchar_t selfPath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), selfPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return fileName != nullptr ? std::wstring(fileName) : std::wstring();
    }
    wchar_t* slash = std::wcsrchr(selfPath, L'\\');
    if (slash != nullptr) {
        slash[1] = L'\0';
        return std::wstring(selfPath) + (fileName != nullptr ? fileName : L"");
    }
    return fileName != nullptr ? std::wstring(fileName) : std::wstring();
}

void tryRegisterSwapChainQueue(IUnknown* deviceOrQueue, IUnknown* swapChain, const char* source)
{
    if (deviceOrQueue == nullptr || swapChain == nullptr) {
        return;
    }

    // Do not force-load nvngx.dll here.  The host-discovery log is supposed to tell us
    // whether the game reached NGX on its own.  Loading our NGX facade from the DXGI
    // swapchain path makes the verdict ambiguous and can perturb the game's feature
    // discovery order.  Queue registration is opportunistic until the host has loaded NGX.
    HMODULE ngx = GetModuleHandleW(L"nvngx.dll");
    if (ngx == nullptr) {
        apusr::host_discovery::eventf("dxgi", "queue.register.skipped",
                                     "source=%s reason=ngx_not_loaded_by_host", source);
        return;
    }

    auto fn = reinterpret_cast<RegisterD3D12QueueForSwapChainFn>(
        GetProcAddress(ngx, "apusrNgxDlssRegisterD3D12CommandQueueForSwapChain"));
    if (fn == nullptr) {
        apusr::host_discovery::eventf("dxgi", "queue.register.skipped", "source=%s reason=export_missing", source);
        return;
    }

    const bool ok = fn(static_cast<void*>(swapChain), static_cast<void*>(deviceOrQueue));
    apusr::host_discovery::eventf("dxgi", ok ? "queue.register.ok" : "queue.register.failed",
                                 "source=%s swapchain=%p device_or_queue=%p",
                                 source, static_cast<void*>(swapChain), static_cast<void*>(deviceOrQueue));
}


#if defined(__IDXGIFactory7_INTERFACE_DEFINED__)
using FactoryProxyBase = IDXGIFactory7;
#else
using FactoryProxyBase = IDXGIFactory6;
#endif

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

class FactoryProxy final : public FactoryProxyBase {
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
            riid == __uuidof(IDXGIFactory6)
#if defined(__IDXGIFactory7_INTERFACE_DEFINED__)
            || riid == __uuidof(IDXGIFactory7)
#endif
            ) {
            IUnknown* supported = nullptr;
            const HRESULT hr = m_inner->QueryInterface(riid, reinterpret_cast<void**>(&supported));
            if (FAILED(hr)) {
                return hr;
            }
            supported->Release();
            *ppvObject = static_cast<FactoryProxyBase*>(this);
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
            if (SUCCEEDED(hr) && ppSwapChain != nullptr && *ppSwapChain != nullptr) {
                tryRegisterSwapChainQueue(pDevice, *ppSwapChain, "CreateSwapChain");
            }
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
            if (SUCCEEDED(hr) && ppSwapChain != nullptr && *ppSwapChain != nullptr) {
                tryRegisterSwapChainQueue(pDevice, *ppSwapChain, "CreateSwapChainForHwnd");
            }
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
            if (SUCCEEDED(hr) && ppSwapChain != nullptr && *ppSwapChain != nullptr) {
                tryRegisterSwapChainQueue(pDevice, *ppSwapChain, "CreateSwapChainForCoreWindow");
            }
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
            if (SUCCEEDED(hr) && ppSwapChain != nullptr && *ppSwapChain != nullptr) {
                tryRegisterSwapChainQueue(pDevice, *ppSwapChain, "CreateSwapChainForComposition");
            }
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

#if defined(__IDXGIFactory7_INTERFACE_DEFINED__)
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) override
    {
        if (auto* f = query<IDXGIFactory7>()) {
            const HRESULT hr = f->RegisterAdaptersChangedEvent(hEvent, pdwCookie);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD dwCookie) override
    {
        if (auto* f = query<IDXGIFactory7>()) {
            const HRESULT hr = f->UnregisterAdaptersChangedEvent(dwCookie);
            f->Release();
            return hr;
        }
        return E_NOINTERFACE;
    }
#endif

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

} // namespace

extern "C" HRESULT WINAPI apusr_CreateDXGIFactory(REFIID riid, void** ppFactory)
{
    apusr::host_discovery::event("dxgi", "CreateDXGIFactory.called");
    apusrInstallIdentitySpoofHooks();
    if (auto fn = originalFn<CreateDXGIFactoryFn>("CreateDXGIFactory")) {
        const HRESULT hr = fn(riid, ppFactory);
        if (SUCCEEDED(hr)) {
            const HRESULT wrapHr = wrapFactoryResult(riid, ppFactory);
            if (FAILED(wrapHr)) {
                return wrapHr;
            }
        }
        return hr;
    }
    return E_FAIL;
}

extern "C" HRESULT WINAPI apusr_CreateDXGIFactory1(REFIID riid, void** ppFactory)
{
    apusr::host_discovery::event("dxgi", "CreateDXGIFactory1.called");
    apusrInstallIdentitySpoofHooks();
    if (auto fn = originalFn<CreateDXGIFactory1Fn>("CreateDXGIFactory1")) {
        const HRESULT hr = fn(riid, ppFactory);
        if (SUCCEEDED(hr)) {
            const HRESULT wrapHr = wrapFactoryResult(riid, ppFactory);
            if (FAILED(wrapHr)) {
                return wrapHr;
            }
        }
        return hr;
    }
    return E_FAIL;
}

extern "C" HRESULT WINAPI apusr_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory)
{
    apusr::host_discovery::eventf("dxgi", "CreateDXGIFactory2.called", "flags=0x%08X", flags);
    apusrInstallIdentitySpoofHooks();
    if (auto fn = originalFn<CreateDXGIFactory2Fn>("CreateDXGIFactory2")) {
        const HRESULT hr = fn(flags, riid, ppFactory);
        if (SUCCEEDED(hr)) {
            const HRESULT wrapHr = wrapFactoryResult(riid, ppFactory);
            if (FAILED(wrapHr)) {
                return wrapHr;
            }
        }
        return hr;
    }
    return E_FAIL;
}

using DXGIDeclareAdapterRemovalSupportFn = HRESULT(WINAPI*)();
extern "C" HRESULT WINAPI apusr_DXGIDeclareAdapterRemovalSupport()
{
    apusrInstallIdentitySpoofHooks();
    if (auto fn = originalFn<DXGIDeclareAdapterRemovalSupportFn>("DXGIDeclareAdapterRemovalSupport")) {
        return fn();
    }
    return S_OK;
}

extern "C" HRESULT WINAPI apusr_DXGIGetDebugInterface1(UINT flags, REFIID riid, void** debugInterface)
{
    apusrInstallIdentitySpoofHooks();
    if (auto fn = originalFn<DXGIGetDebugInterface1Fn>("DXGIGetDebugInterface1")) {
        return fn(flags, riid, debugInterface);
    }
    return E_FAIL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        apusr::host_discovery::moduleLoaded("dxgi", instance);
        apusr::host_discovery::event("host", "verdict", "loaded=dxgi next=wait_for_dxgi_adapter_or_nvapi_or_ngx_probe");
        // Some hosts enter stock Streamline slInit before that
        // function makes its first DXGI call. Arm only the main executable's
        // renderer-neutral loader/GetProcAddress IAT slots now so slInit can be
        // observed before it starts. The helper intentionally performs no
        // module loads, snapshots, Detours work, or renderer identity spoofing
        // under the loader lock; the normal renderer-aware bootstrap remains
        // authoritative once Preferences::renderAPI is visible.
        (void)apusrInstallIdentitySpoofLoaderHooksEarly();
        // Loading DXGI is not proof that this process selected a D3D renderer.
        // Vulkan hosts can load dxgi.dll for display/adapter utilities before
        // Streamline has established its render API. Installing the D3D identity
        // takeover here would permanently patch already-loaded modules before we
        // know that Vulkan must remain on the real AMD identity. The exported
        // DXGI entry points install the hooks lazily when D3D actually uses DXGI.
    }
    if (reason == DLL_PROCESS_DETACH && g_original != nullptr) {
        FreeLibrary(g_original);
        g_original = nullptr;
    }
    return TRUE;
}

#endif // _WIN32
