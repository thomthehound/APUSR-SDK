// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.
//
// Minimal NVAPI compatibility shim for DLSS-on-non-NVIDIA availability.
//
// This is not a full Fakenvapi replacement.  It exists to answer the early NVIDIA-driver/GPU
// capability probes that some games perform before they decide whether to load nvngx_dlss.dll.
// The D3D12 NGX path records APUSR's imported FSR4 INT8 reference backend; this shim stays limited to
// compatibility answers and native-DXR forwarding.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include "../common/apusr_host_discovery_log.h"

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace {

using NvU32 = std::uint32_t;
using NvAPI_Status = std::int32_t;
using NvPhysicalGpuHandle = void*;
using NvLogicalGpuHandle = void*;
using NvDisplayHandle = void*;
using NvAPI_ShortString = char[64];

constexpr NvAPI_Status NVAPI_OK = 0;
constexpr NvAPI_Status NVAPI_ERROR = -1;
constexpr NvAPI_Status NVAPI_INVALID_ARGUMENT = -5;
constexpr NvAPI_Status NVAPI_END_ENUMERATION = -7;
constexpr NvAPI_Status NVAPI_INVALID_POINTER = -14;
constexpr NvAPI_Status NVAPI_NO_IMPLEMENTATION = -104;
constexpr NvAPI_Status NVAPI_NVIDIA_DEVICE_NOT_FOUND = -6;

constexpr NvU32 kNgxFeatureSuperSampling = 1u;

constexpr NvU32 kSpoofedVendorId = 0x10DE;
constexpr NvU32 kSpoofedDeviceId = 0x2204;
constexpr NvU32 kSpoofedSubsystemId = 0x147A10DE;
constexpr NvU32 kSpoofedRevisionId = 0xA1;
constexpr NvU32 kSpoofedDriverVersion = 56081;
constexpr char kSpoofedGpuName[] = "NVIDIA GeForce RTX 3090";
constexpr char kSpoofedDriverBranch[] = "r560_00";
constexpr char kInterfaceVersion[] = "NVAPI Open Source Interface / APUSR compatibility shim";

struct FakeGpuTag {
    std::uint32_t magic;
};

FakeGpuTag g_fakePhysicalGpu { 0x4E505531u };
FakeGpuTag g_fakeLogicalGpu { 0x4E505532u };
FakeGpuTag g_fakeDisplay { 0x4E505533u };

wchar_t g_moduleDir[MAX_PATH] = {};
std::atomic<bool> g_loggedUnknownId { false };
std::atomic<unsigned long> g_nvapiQueryCount { 0 };
std::atomic<unsigned long> g_nvapiUnknownQueryCount { 0 };

struct PrimaryAdapterInfo {
    bool initialized = false;
    LUID luid {};
    NvU32 vendorId = kSpoofedVendorId;
    NvU32 deviceId = kSpoofedDeviceId;
    NvU32 subSystemId = kSpoofedSubsystemId;
    NvU32 revisionId = kSpoofedRevisionId;
};

PrimaryAdapterInfo g_primaryAdapter {};

void rememberModuleDir(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    DWORD count = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (count == 0 || count >= std::size(path)) {
        return;
    }
    wchar_t* slash = std::wcsrchr(path, L'\\');
    if (slash != nullptr) {
        *slash = L'\0';
        std::wcsncpy(g_moduleDir, path, std::size(g_moduleDir) - 1);
    }
}

void ensureCentralLogDir(wchar_t (&dir)[MAX_PATH])
{
    const DWORD explicitSize = GetEnvironmentVariableW(L"APUSR_LOG_DIR", dir, static_cast<DWORD>(MAX_PATH));
    if (explicitSize > 0 && explicitSize < MAX_PATH) {
        CreateDirectoryW(dir, nullptr);
        return;
    }
    std::wcsncpy(dir, L"C:\\NGXLogs\\APUSR", MAX_PATH - 1);
    CreateDirectoryW(L"C:\\NGXLogs", nullptr);
    CreateDirectoryW(dir, nullptr);
}

bool pathAlreadyReset(const wchar_t* path)
{
    static std::mutex mutex;
    static std::vector<std::wstring> resetPaths;
    std::lock_guard<std::mutex> lock(mutex);
    for (const std::wstring& resetPath : resetPaths) {
        if (_wcsicmp(resetPath.c_str(), path) == 0) {
            return true;
        }
    }
    resetPaths.emplace_back(path);
    return false;
}

void appendFile(const wchar_t* path, const char* line)
{
    if (path == nullptr || line == nullptr) {
        return;
    }
    if (!pathAlreadyReset(path)) {
        HANDLE clear = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (clear != INVALID_HANDLE_VALUE) {
            CloseHandle(clear);
        }
    }
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    CloseHandle(file);
}

void appendLogLine(const wchar_t* fileName, const char* line)
{
    wchar_t dir[MAX_PATH] = {};
    ensureCentralLogDir(dir);
    wchar_t path[MAX_PATH] = {};
    std::swprintf(path, std::size(path), L"%ls\\%ls", dir, fileName);
    appendFile(path, line);
}
bool envTruth(const wchar_t* name)
{
    wchar_t value[16] = {};
    DWORD count = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    return count > 0 && !(value[0] == L'0' || value[0] == L'n' || value[0] == L'N' ||
                          value[0] == L'f' || value[0] == L'F');
}

bool nvapiVerboseTraceEnabled()
{
    static int cached = -1;
    if (cached < 0) {
        cached = envTruth(L"APUSR_NVAPI_TRACE_VERBOSE") ? 1 : 0;
    }
    return cached != 0;
}

bool nvapiRtCompatibilityEnabled()
{
    static int cached = -1;
    if (cached < 0) {
        // Default on.  Some DLSS hosts probe NVAPI D3D12 RT bridge entry points
        // during renderer startup once nvapi64.dll exists.  Returning null can make those hosts
        // fail RT initialization before APUSR ever reaches the upscaler path.  The bridge itself
        // must therefore be a near-zero-overhead native-DXR forwarder, not a verbose/debug path.
        // Disable only for diagnostics/titles that prove they do not need these entry points.
        cached = envTruth(L"APUSR_NVAPI_DISABLE_RT_COMPAT") ? 0 : 1;
    }
    return cached != 0;
}

bool nvapiSafePointerChecksEnabled()
{
    static int cached = -1;
    if (cached < 0) {
        // VirtualQuery is useful while reverse-engineering loose NVAPI structs, but it is far too
        // expensive to sit on a hot acceleration-structure build path.  Keep it opt-in now that the
        // the supported host structure layouts are known and validated.
        cached = envTruth(L"APUSR_NVAPI_SAFE_POINTER_CHECKS") ? 1 : 0;
    }
    return cached != 0;
}

bool isNvapiRtCompatibilityInterface(NvU32 id)
{
    switch (id) {
    case 0x43D867C0u: // NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread
    case 0xAC2DFEB5u: // NvAPI_D3D12_SetNvShaderExtnSlotSpace
    case 0x85A6C2A0u: // NvAPI_D3D12_GetRaytracingCaps
    case 0x8D025B77u: // NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx
    case 0xE24EAD45u: // NvAPI_D3D12_BuildRaytracingAccelerationStructureEx
        return true;
    default:
        return false;
    }
}

void tracef(const char* fmt, ...)
{
    if (!nvapiVerboseTraceEnabled()) {
        return;
    }
    char line[512] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    appendLogLine(L"apusr_nvapi_host_proxy_trace.log", line);
}

void copyShortString(char* dst, const char* src)
{
    if (dst == nullptr) {
        return;
    }
    std::memset(dst, 0, 64);
    std::strncpy(dst, src, 63);
}

bool isReadablePointer(const void* ptr, std::size_t bytes)
{
    if (ptr == nullptr || bytes == 0) {
        return false;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    // Reject obvious integer/version values before VirtualQuery. A host can expose this
    // failure mode when an NVAPI version word such as 0x00010018 is accidentally interpreted
    // as a pointer.
    if (address < 0x100000u) {
        return false;
    }

    if (!nvapiSafePointerChecksEnabled()) {
        return true;
    }

    MEMORY_BASIC_INFORMATION mbi {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    const DWORD readableMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readableMask) == 0) {
        return false;
    }

    const auto regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto regionSize = static_cast<std::uintptr_t>(mbi.RegionSize);
    return address >= regionStart && bytes <= (regionStart + regionSize - address);
}

bool isWritablePointer(void* ptr, std::size_t bytes)
{
    if (ptr == nullptr || bytes == 0) {
        return false;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(ptr);
    if (address < 0x100000u) {
        return false;
    }

    if (!nvapiSafePointerChecksEnabled()) {
        return true;
    }

    MEMORY_BASIC_INFORMATION mbi {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    const DWORD writableMask = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & writableMask) == 0) {
        return false;
    }

    const auto regionStart = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto regionSize = static_cast<std::uintptr_t>(mbi.RegionSize);
    return address >= regionStart && bytes <= (regionStart + regionSize - address);
}

void initPrimaryAdapterInfo()
{
    if (g_primaryAdapter.initialized) {
        return;
    }
    g_primaryAdapter.initialized = true;

    wchar_t systemDir[MAX_PATH] = {};
    UINT count = GetSystemDirectoryW(systemDir, static_cast<UINT>(std::size(systemDir)));
    if (count == 0 || count >= std::size(systemDir)) {
        return;
    }

    wchar_t dxgiPath[MAX_PATH] = {};
    std::swprintf(dxgiPath, std::size(dxgiPath), L"%ls\\dxgi.dll", systemDir);
    HMODULE dxgi = LoadLibraryW(dxgiPath);
    if (dxgi == nullptr) {
        return;
    }

    using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (createFactory == nullptr) {
        return;
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr) {
        return;
    }

    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT hr = factory->EnumAdapters1(index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || adapter == nullptr) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId != 0x1414) {
            g_primaryAdapter.luid = desc.AdapterLuid;
            g_primaryAdapter.vendorId = desc.VendorId;
            g_primaryAdapter.deviceId = desc.DeviceId;
            g_primaryAdapter.subSystemId = desc.SubSysId;
            g_primaryAdapter.revisionId = desc.Revision;
            adapter->Release();
            break;
        }
        adapter->Release();
    }

    factory->Release();
}

struct NvDisplayDriverVersionLoose {
    NvU32 version;
    NvU32 drvVersion;
    NvU32 bldChangeListNum;
    char szBuildBranchString[64];
    char szAdapterString[64];
};

struct NvGpuArchInfoLoose {
    NvU32 version;
    NvU32 architecture;
    NvU32 implementation;
    NvU32 revision;
    NvU32 architecture_id;
    NvU32 implementation_id;
    NvU32 revision_id;
};

constexpr std::size_t kNvapiMaxPhysicalGpus = 64;

// NV_LOGICAL_GPU_DATA_V1 is an externally consumed NVAPI ABI.  Keep the exact x64 field
// order/layout here: Streamline supplies storage for the OS adapter ID and expects NVAPI to
// write the LUID through pOSAdapterId.  An earlier APUSR layout embedded a LUID after the
// handle array, which overwrote the caller's pointer/count fields and prevented sl.common
// from correlating the NVAPI GPU with its DXGI adapter.
struct NvLogicalGpuDataLoose {
    NvU32 version;
    void* pOSAdapterId;
    NvU32 physicalGpuCount;
    NvPhysicalGpuHandle physicalGpuHandles[kNvapiMaxPhysicalGpus];
    NvU32 reserved[8];
};

static_assert(sizeof(void*) == 8, "APUSR NVAPI host compatibility targets Windows x64");
static_assert(offsetof(NvLogicalGpuDataLoose, pOSAdapterId) == 8);
static_assert(offsetof(NvLogicalGpuDataLoose, physicalGpuCount) == 16);
static_assert(offsetof(NvLogicalGpuDataLoose, physicalGpuHandles) == 24);
static_assert(sizeof(NvLogicalGpuDataLoose) == 568);

struct NvNgxDriverFeatureSupportInfoLoose {
    NvU32 featureId;
    NvU32 bSupported;
};

struct NvNgxGetDriverFeatureSupportParamsLoose {
    NvU32 version;
    NvU32 featureCount;
    NvNgxDriverFeatureSupportInfoLoose* featureSupportInfo;
};

NvAPI_Status __cdecl NvAPI_Initialize()
{
    tracef("NvAPI_Initialize -> OK\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_Unload()
{
    tracef("NvAPI_Unload -> OK\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetInterfaceVersionString(NvAPI_ShortString desc)
{
    copyShortString(desc, kInterfaceVersion);
    tracef("NvAPI_GetInterfaceVersionString -> OK\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetErrorMessage(NvAPI_Status status, NvAPI_ShortString msg)
{
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "APUSR NVAPI status %d", static_cast<int>(status));
    copyShortString(msg, buf);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_EnumPhysicalGPUs(NvPhysicalGpuHandle handles[64], NvU32* count)
{
    if (handles == nullptr || count == nullptr) {
        return NVAPI_INVALID_ARGUMENT;
    }
    handles[0] = &g_fakePhysicalGpu;
    *count = 1;
    tracef("NvAPI_EnumPhysicalGPUs -> 1\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_EnumLogicalGPUs(NvLogicalGpuHandle handles[64], NvU32* count)
{
    if (handles == nullptr || count == nullptr) {
        return NVAPI_INVALID_ARGUMENT;
    }
    handles[0] = &g_fakeLogicalGpu;
    *count = 1;
    tracef("NvAPI_EnumLogicalGPUs -> 1\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_EnumNvidiaDisplayHandle(NvU32 index, NvDisplayHandle* handle)
{
    if (handle == nullptr) {
        return NVAPI_INVALID_ARGUMENT;
    }
    if (index > 0) {
        return NVAPI_END_ENUMERATION;
    }
    *handle = &g_fakeDisplay;
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetLogicalGPUFromPhysicalGPU(NvPhysicalGpuHandle, NvLogicalGpuHandle* logical)
{
    if (logical == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    *logical = &g_fakeLogicalGpu;
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetPhysicalGPUsFromLogicalGPU(NvLogicalGpuHandle, NvPhysicalGpuHandle handles[64], NvU32* count)
{
    if (handles == nullptr || count == nullptr) {
        return NVAPI_INVALID_ARGUMENT;
    }
    handles[0] = &g_fakePhysicalGpu;
    *count = 1;
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetPhysicalGPUsFromDisplay(NvDisplayHandle, NvPhysicalGpuHandle handles[64], NvU32* count)
{
    if (handles == nullptr || count == nullptr) {
        return NVAPI_INVALID_ARGUMENT;
    }
    handles[0] = &g_fakePhysicalGpu;
    *count = 1;
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetGPUIDfromPhysicalGPU(NvPhysicalGpuHandle, NvU32* gpuId)
{
    if (gpuId == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    *gpuId = 1;
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetPhysicalGPUFromGPUID(NvU32, NvPhysicalGpuHandle* gpu)
{
    if (gpu == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    *gpu = &g_fakePhysicalGpu;
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GPU_GetFullName(NvPhysicalGpuHandle, NvAPI_ShortString name)
{
    copyShortString(name, kSpoofedGpuName);
    tracef("NvAPI_GPU_GetFullName -> %s\\r\\n", kSpoofedGpuName);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GPU_GetPCIIdentifiers(NvPhysicalGpuHandle,
                                                  NvU32* deviceId,
                                                  NvU32* subSystemId,
                                                  NvU32* revisionId,
                                                  NvU32* extDeviceId)
{
    if (deviceId != nullptr) {
        *deviceId = (kSpoofedDeviceId << 16) | kSpoofedVendorId;
    }
    if (subSystemId != nullptr) {
        *subSystemId = kSpoofedSubsystemId;
    }
    if (revisionId != nullptr) {
        *revisionId = kSpoofedRevisionId;
    }
    if (extDeviceId != nullptr) {
        *extDeviceId = kSpoofedDeviceId;
    }
    tracef("NvAPI_GPU_GetPCIIdentifiers -> vendor=0x%04X device=0x%04X\\r\\n", kSpoofedVendorId, kSpoofedDeviceId);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_SYS_GetDriverAndBranchVersion(NvU32* driverVersion, NvAPI_ShortString branch)
{
    if (driverVersion != nullptr) {
        *driverVersion = kSpoofedDriverVersion;
    }
    copyShortString(branch, kSpoofedDriverBranch);
    tracef("NvAPI_SYS_GetDriverAndBranchVersion -> %u %s\\r\\n", kSpoofedDriverVersion, kSpoofedDriverBranch);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GetDisplayDriverVersion(NvDisplayHandle, NvDisplayDriverVersionLoose* version)
{
    if (version == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    version->drvVersion = kSpoofedDriverVersion;
    version->bldChangeListNum = 0;
    copyShortString(version->szBuildBranchString, kSpoofedDriverBranch);
    copyShortString(version->szAdapterString, kSpoofedGpuName);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GPU_GetArchInfo(NvPhysicalGpuHandle, NvGpuArchInfoLoose* arch)
{
    if (arch == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    // Use stable nonzero Ada-like placeholders.  Games should not execute vendor-specific shader paths on the AMD device;
    // this is only intended for menu availability checks.
    arch->architecture = 0x190;
    arch->architecture_id = 0x190;
    arch->implementation = 0x192;
    arch->implementation_id = 0x192;
    arch->revision = 0;
    arch->revision_id = 0;
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GPU_GetLogicalGpuInfo(NvLogicalGpuHandle, NvLogicalGpuDataLoose* data)
{
    if (data == nullptr) {
        return NVAPI_INVALID_POINTER;
    }

    initPrimaryAdapterInfo();
    data->physicalGpuCount = 1;
    data->physicalGpuHandles[0] = &g_fakePhysicalGpu;

    // The caller owns the LUID storage.  Streamline points this at a stack LUID and then uses
    // the value to match the NVAPI physical GPU back to its enumerated DXGI adapter before it
    // asks NvAPI_GPU_GetArchInfo for the architecture gate.
    if (data->pOSAdapterId != nullptr) {
        if (!isWritablePointer(data->pOSAdapterId, sizeof(g_primaryAdapter.luid))) {
            tracef("NvAPI_GPU_GetLogicalGpuInfo -> invalid pOSAdapterId=%p\r\n", data->pOSAdapterId);
            return NVAPI_INVALID_POINTER;
        }
        std::memcpy(data->pOSAdapterId, &g_primaryAdapter.luid, sizeof(g_primaryAdapter.luid));
    }

    apusr::host_discovery::eventf(
        "nvapi", "logical_gpu.info",
        "count=1 luid=%ld.%lu os_adapter_id=%p",
        static_cast<long>(g_primaryAdapter.luid.HighPart),
        static_cast<unsigned long>(g_primaryAdapter.luid.LowPart), data->pOSAdapterId);
    tracef("NvAPI_GPU_GetLogicalGpuInfo -> count=1 luid=%ld.%lu osAdapterId=%p\r\n",
           static_cast<long>(g_primaryAdapter.luid.HighPart),
           static_cast<unsigned long>(g_primaryAdapter.luid.LowPart), data->pOSAdapterId);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GPU_GetAdapterIdFromPhysicalGpu(NvPhysicalGpuHandle, void* osAdapterId)
{
    if (osAdapterId == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    initPrimaryAdapterInfo();
    std::memcpy(osAdapterId, &g_primaryAdapter.luid, sizeof(g_primaryAdapter.luid));
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GPU_GetGpuCoreCount(NvPhysicalGpuHandle, NvU32* count)
{
    if (count != nullptr) {
        *count = 128;
    }
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_GPU_CudaEnumComputeCapableGpus(void* topo)
{
    if (topo == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    // Loose layout: version, gpuCount, then entries.  Only gpuCount is commonly used for availability checks.
    auto words = static_cast<NvU32*>(topo);
    words[1] = 1;
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(void*, NvU32, bool* supported)
{
    if (supported != nullptr) {
        *supported = false;
    }
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D11_IsNvShaderExtnOpCodeSupported(void*, NvU32, bool* supported)
{
    if (supported != nullptr) {
        *supported = false;
    }
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_NGX_GetDriverFeatureSupport(NvNgxGetDriverFeatureSupportParamsLoose* params)
{
    if (params == nullptr || params->featureSupportInfo == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    NvU32 supportedCount = 0;
    for (NvU32 i = 0; i < params->featureCount; ++i) {
        const NvU32 featureId = params->featureSupportInfo[i].featureId;
        const bool supported = (featureId == kNgxFeatureSuperSampling);
        params->featureSupportInfo[i].bSupported = supported ? 1u : 0u;
        if (supported) {
            ++supportedCount;
        }
    }
    tracef("NvAPI_NGX_GetDriverFeatureSupport count=%u supported=%u policy=sr-only\r\n", params->featureCount, supportedCount);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_PlaceholderOk()
{
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_PlaceholderNoImplementation()
{
    tracef("NvAPI_PlaceholderNoImplementation -> NO_IMPLEMENTATION\\r\\n");
    return NVAPI_NO_IMPLEMENTATION;
}

NvAPI_Status __cdecl NvAPI_Disp_GetDisplayIdByDisplayName(const char*, NvU32* displayId)
{
    if (displayId == nullptr) {
        return NVAPI_INVALID_POINTER;
    }
    *displayId = 1;
    tracef("NvAPI_Disp_GetDisplayIdByDisplayName -> displayId=1\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_Disp_GetOutputMode(NvU32, NvU32* outputMode)
{
    if (outputMode != nullptr) {
        *outputMode = 0;
    }
    tracef("NvAPI_Disp_GetOutputMode -> default\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_Disp_SetOutputMode(NvU32, NvU32)
{
    tracef("NvAPI_Disp_SetOutputMode -> OK/no-op\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(void*, NvU32, NvU32)
{
    // Return OK but keep NvAPI_D3D12_IsNvShaderExtnOpCodeSupported=false so titles should not emit NVIDIA intrinsics.
    tracef("NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread -> OK/no-op\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D12_SetNvShaderExtnSlotSpace(void*, NvU32, NvU32)
{
    tracef("NvAPI_D3D12_SetNvShaderExtnSlotSpace -> OK/no-op\\r\\n");
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D12_GetRaytracingCaps(void*, NvU32 type, void* data, std::size_t dataSize)
{
    if (data == nullptr || dataSize == 0) {
        return NVAPI_INVALID_POINTER;
    }

    // Some hosts probe NVIDIA-specific RT extensions during startup once nvapi64.dll is present.
    // Report that optional NVIDIA extensions such as SER / OMM / DMM are unavailable, so the game can
    // stay on the native D3D12 DXR path instead of failing ray-tracing initialization.
    std::memset(data, 0, dataSize);
    tracef("NvAPI_D3D12_GetRaytracingCaps type=%u bytes=%zu -> none\\r\\n", type, dataSize);
    return NVAPI_OK;
}

enum : NvU32 {
    NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES_EX = 0,
    NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS_EX = 1,
};

struct NvapiD3D12RaytracingGeometryDescExLoose {
    NvU32 type;
    D3D12_RAYTRACING_GEOMETRY_FLAGS flags;
    union {
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles;
        D3D12_RAYTRACING_GEOMETRY_AABBS_DESC aabbs;
    };
};

struct NvapiD3D12BuildRtAsInputsExLoose {
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE type;
    NvU32 flags;
    UINT numDescs;
    D3D12_ELEMENTS_LAYOUT descsLayout;
    UINT geometryDescStrideInBytes;
    UINT reserved;
    union {
        D3D12_GPU_VIRTUAL_ADDRESS instanceDescs;
        const NvapiD3D12RaytracingGeometryDescExLoose* pGeometryDescs;
        const NvapiD3D12RaytracingGeometryDescExLoose* const* ppGeometryDescs;
    };
};

struct NvapiGetRtPrebuildInfoParamsExLoose {
    NvU32 version;
    NvU32 reserved;
    const NvapiD3D12BuildRtAsInputsExLoose* pDesc;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO* pInfo;
};

struct NvapiD3D12BuildRtAsDescExLoose {
    D3D12_GPU_VIRTUAL_ADDRESS destAccelerationStructureData;
    NvapiD3D12BuildRtAsInputsExLoose inputs;
    D3D12_GPU_VIRTUAL_ADDRESS sourceAccelerationStructureData;
    D3D12_GPU_VIRTUAL_ADDRESS scratchAccelerationStructureData;
};

struct NvapiBuildRtAsParamsExLoose {
    NvU32 version;
    NvU32 reserved;
    const NvapiD3D12BuildRtAsDescExLoose* pDesc;
    UINT numPostbuildInfoDescs;
    UINT reserved2;
    const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* pPostbuildInfoDescs;
};

bool convertRtGeometryDesc(const NvapiD3D12RaytracingGeometryDescExLoose* nvGeometry,
                           D3D12_RAYTRACING_GEOMETRY_DESC* nativeGeometry)
{
    if (nvGeometry == nullptr || nativeGeometry == nullptr) {
        return false;
    }

    nativeGeometry->Flags = nvGeometry->flags;

    switch (nvGeometry->type) {
    case NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES_EX:
        nativeGeometry->Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        nativeGeometry->Triangles = nvGeometry->triangles;
        return true;
    case NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS_EX:
        nativeGeometry->Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        nativeGeometry->AABBs = nvGeometry->aabbs;
        return true;
    default:
        tracef("NvAPI_D3D12 RT geometry type %u unsupported by compatibility shim\r\n", nvGeometry->type);
        return false;
    }
}

bool convertRtPrebuildInputs(const NvapiD3D12BuildRtAsInputsExLoose* nvInputs,
                             D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS* nativeInputs,
                             std::vector<D3D12_RAYTRACING_GEOMETRY_DESC>* copiedGeometry)
{
    if (nvInputs == nullptr || nativeInputs == nullptr) {
        return false;
    }

    nativeInputs->Type = nvInputs->type;
    // Keep standard D3D12 build flags and drop NVIDIA-only extension bits such as OMM/DMM.
    nativeInputs->Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(nvInputs->flags & 0x3fu);
    nativeInputs->NumDescs = nvInputs->numDescs;
    nativeInputs->DescsLayout = nvInputs->descsLayout;

    if (nvInputs->type == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL) {
        nativeInputs->InstanceDescs = nvInputs->instanceDescs;
        return true;
    }

    if (nvInputs->type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL) {
        return false;
    }

    if (copiedGeometry == nullptr) {
        return false;
    }

    copiedGeometry->resize(nvInputs->numDescs);

    if (nvInputs->descsLayout == D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS) {
        if (nvInputs->ppGeometryDescs == nullptr && nvInputs->numDescs != 0) {
            tracef("NvAPI_D3D12 RT array-of-pointers geometry list is null\r\n");
            return false;
        }
        if (nvInputs->numDescs != 0 &&
            !isReadablePointer(nvInputs->ppGeometryDescs,
                               sizeof(*nvInputs->ppGeometryDescs) *
                                   static_cast<std::size_t>(nvInputs->numDescs))) {
            tracef("NvAPI_D3D12 RT array-of-pointers geometry list is unreadable ptr=%p count=%u\r\n",
                   static_cast<const void*>(nvInputs->ppGeometryDescs), nvInputs->numDescs);
            return false;
        }

        // Match the dxvk-nvapi/Fakenvapi bridge behavior here.  In the ARRAY_OF_POINTERS
        // layout, supported hosts pass standard D3D12 geometry descriptors behind the pointer
        // array; trying to reinterpret the pointer-array itself as APUSR's loose extended
        // geometry shape is both unnecessary and unsafe.
        nativeInputs->ppGeometryDescs =
            reinterpret_cast<const D3D12_RAYTRACING_GEOMETRY_DESC* const*>(nvInputs->ppGeometryDescs);
        return true;
    }

    if (nvInputs->descsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY) {
        return false;
    }

    if (nvInputs->pGeometryDescs == nullptr && nvInputs->numDescs != 0) {
        return false;
    }

    const UINT stride = nvInputs->geometryDescStrideInBytes == 0
        ? static_cast<UINT>(sizeof(NvapiD3D12RaytracingGeometryDescExLoose))
        : nvInputs->geometryDescStrideInBytes;
    if (stride < sizeof(NvapiD3D12RaytracingGeometryDescExLoose)) {
        tracef("NvAPI_D3D12 RT geometry stride %u too small\r\n", stride);
        return false;
    }

    if (nvInputs->numDescs != 0 &&
        !isReadablePointer(nvInputs->pGeometryDescs,
                           static_cast<std::size_t>(stride) * static_cast<std::size_t>(nvInputs->numDescs))) {
        tracef("NvAPI_D3D12 RT geometry array is unreadable ptr=%p count=%u stride=%u\r\n",
               static_cast<const void*>(nvInputs->pGeometryDescs), nvInputs->numDescs, stride);
        return false;
    }

    const auto* bytes = reinterpret_cast<const std::byte*>(nvInputs->pGeometryDescs);
    for (UINT i = 0; i < nvInputs->numDescs; ++i) {
        const auto* nvGeometry = reinterpret_cast<const NvapiD3D12RaytracingGeometryDescExLoose*>(
            bytes + static_cast<std::size_t>(i) * stride);
        if (!convertRtGeometryDesc(nvGeometry, &(*copiedGeometry)[i])) {
            return false;
        }
    }

    nativeInputs->DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    nativeInputs->pGeometryDescs = copiedGeometry->data();
    return true;
}

NvAPI_Status __cdecl NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx(
    ID3D12Device5* device,
    const NvapiGetRtPrebuildInfoParamsExLoose* params)
{
    if (device == nullptr || !isReadablePointer(params, sizeof(*params))) {
        tracef("NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx -> invalid params pointer\r\n");
        return NVAPI_INVALID_POINTER;
    }
    if (!isReadablePointer(params->pDesc, sizeof(*params->pDesc)) ||
        !isReadablePointer(params->pInfo, sizeof(*params->pInfo))) {
        tracef("NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx version=0x%08X -> invalid desc/info pointer desc=%p info=%p\r\n",
               params->version, static_cast<const void*>(params->pDesc), static_cast<void*>(params->pInfo));
        return NVAPI_INVALID_POINTER;
    }

    tracef("NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx version=0x%08X desc type=%u flags=0x%X count=%u layout=%u stride=%u ptr=%p\r\n",
           params->version, static_cast<unsigned>(params->pDesc->type), params->pDesc->flags,
           params->pDesc->numDescs, static_cast<unsigned>(params->pDesc->descsLayout),
           params->pDesc->geometryDescStrideInBytes,
           reinterpret_cast<const void*>(params->pDesc->pGeometryDescs));

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS nativeInputs {};
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> copiedGeometry;
    if (!convertRtPrebuildInputs(params->pDesc, &nativeInputs, &copiedGeometry)) {
        tracef("NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx version=0x%08X -> invalid inputs\r\n",
               params->version);
        return NVAPI_INVALID_ARGUMENT;
    }

    device->GetRaytracingAccelerationStructurePrebuildInfo(&nativeInputs, params->pInfo);
    tracef("NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx version=0x%08X -> native D3D12\r\n",
           params->version);
    return NVAPI_OK;
}

NvAPI_Status __cdecl NvAPI_D3D12_BuildRaytracingAccelerationStructureEx(
    ID3D12GraphicsCommandList4* commandList,
    const NvapiBuildRtAsParamsExLoose* params)
{
    if (commandList == nullptr || !isReadablePointer(params, sizeof(*params))) {
        tracef("NvAPI_D3D12_BuildRaytracingAccelerationStructureEx -> invalid params pointer\r\n");
        return NVAPI_INVALID_POINTER;
    }
    if (!isReadablePointer(params->pDesc, sizeof(*params->pDesc))) {
        tracef("NvAPI_D3D12_BuildRaytracingAccelerationStructureEx version=0x%08X -> invalid desc pointer desc=%p\r\n",
               params->version, static_cast<const void*>(params->pDesc));
        return NVAPI_INVALID_POINTER;
    }
    if (params->numPostbuildInfoDescs != 0 &&
        !isReadablePointer(params->pPostbuildInfoDescs,
                           sizeof(*params->pPostbuildInfoDescs) *
                               static_cast<std::size_t>(params->numPostbuildInfoDescs))) {
        tracef("NvAPI_D3D12_BuildRaytracingAccelerationStructureEx version=0x%08X -> invalid postbuild info\r\n",
               params->version);
        return NVAPI_INVALID_ARGUMENT;
    }

    tracef("NvAPI_D3D12_BuildRaytracingAccelerationStructureEx version=0x%08X desc type=%u flags=0x%X count=%u layout=%u stride=%u ptr=%p\r\n",
           params->version, static_cast<unsigned>(params->pDesc->inputs.type), params->pDesc->inputs.flags,
           params->pDesc->inputs.numDescs, static_cast<unsigned>(params->pDesc->inputs.descsLayout),
           params->pDesc->inputs.geometryDescStrideInBytes,
           reinterpret_cast<const void*>(params->pDesc->inputs.pGeometryDescs));

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC nativeDesc {};
    nativeDesc.DestAccelerationStructureData = params->pDesc->destAccelerationStructureData;
    nativeDesc.SourceAccelerationStructureData = params->pDesc->sourceAccelerationStructureData;
    nativeDesc.ScratchAccelerationStructureData = params->pDesc->scratchAccelerationStructureData;

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> copiedGeometry;
    if (!convertRtPrebuildInputs(&params->pDesc->inputs, &nativeDesc.Inputs, &copiedGeometry)) {
        tracef("NvAPI_D3D12_BuildRaytracingAccelerationStructureEx version=0x%08X -> invalid inputs\r\n",
               params->version);
        return NVAPI_INVALID_ARGUMENT;
    }

    commandList->BuildRaytracingAccelerationStructure(&nativeDesc,
                                                      params->numPostbuildInfoDescs,
                                                      params->pPostbuildInfoDescs);
    tracef("NvAPI_D3D12_BuildRaytracingAccelerationStructureEx version=0x%08X -> native D3D12\r\n",
           params->version);
    return NVAPI_OK;
}

struct InterfaceEntry {
    NvU32 id;
    void* fn;
    const char* name;
};

#define ENTRY(id, fn) InterfaceEntry { id, reinterpret_cast<void*>(&fn), #fn }

InterfaceEntry kInterfaces[] = {
    ENTRY(0x0150E828u, NvAPI_Initialize),
    ENTRY(0xD22BDD7Eu, NvAPI_Unload),
    ENTRY(0x6C2D048Cu, NvAPI_GetErrorMessage),
    ENTRY(0x01053FA5u, NvAPI_GetInterfaceVersionString),
    ENTRY(0xE5AC921Fu, NvAPI_EnumPhysicalGPUs),
    ENTRY(0x48B3EA59u, NvAPI_EnumLogicalGPUs),
    ENTRY(0x9ABDD40Du, NvAPI_EnumNvidiaDisplayHandle),
    ENTRY(0xADD604D1u, NvAPI_GetLogicalGPUFromPhysicalGPU),
    ENTRY(0x34EF9506u, NvAPI_GetPhysicalGPUsFromDisplay),
    ENTRY(0xAEA3FA32u, NvAPI_GetPhysicalGPUsFromLogicalGPU),
    ENTRY(0xB2F454A3u, NvAPI_GetGPUIDfromPhysicalGPU),
    ENTRY(0x5380AD1Au, NvAPI_GetPhysicalGPUFromGPUID),
    ENTRY(0xF951A4D1u, NvAPI_GetDisplayDriverVersion),
    ENTRY(0xCEEE8E9Fu, NvAPI_GPU_GetFullName),
    ENTRY(0x2DDFB66Eu, NvAPI_GPU_GetPCIIdentifiers),
    ENTRY(0x2926AAADu, NvAPI_SYS_GetDriverAndBranchVersion),
    ENTRY(0xD8265D24u, NvAPI_GPU_GetArchInfo),
    ENTRY(0x8E875CF9u, NvAPI_GPU_GetLogicalGpuInfo),
    ENTRY(0x373BF4A0u, NvAPI_GPU_GetAdapterIdFromPhysicalGpu),
    ENTRY(0xC7026A87u, NvAPI_GPU_GetGpuCoreCount),
    ENTRY(0x5786CC6Eu, NvAPI_GPU_CudaEnumComputeCapableGpus),
    ENTRY(0x5F68DA40u, NvAPI_D3D11_IsNvShaderExtnOpCodeSupported),
    ENTRY(0x3DFACEC8u, NvAPI_D3D12_IsNvShaderExtnOpCodeSupported),
    ENTRY(0x0BB81A4Fu, NvAPI_NGX_GetDriverFeatureSupport),
    // Public NVAPI IDs used by recent D3D12 titles during pre-NGX / ray-tracing startup.
    ENTRY(0x842B066Eu, NvAPI_GPU_GetLogicalGpuInfo),
    ENTRY(0x0FF07FDEu, NvAPI_GPU_GetAdapterIdFromPhysicalGpu),
    ENTRY(0x6533EA3Eu, NvAPI_GetGPUIDfromPhysicalGPU),
    ENTRY(0xAE457190u, NvAPI_Disp_GetDisplayIdByDisplayName),
    ENTRY(0x81FED88Du, NvAPI_Disp_GetOutputMode),
    ENTRY(0x98E7661Au, NvAPI_Disp_SetOutputMode),
    ENTRY(0x43D867C0u, NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread),
    ENTRY(0xAC2DFEB5u, NvAPI_D3D12_SetNvShaderExtnSlotSpace),
    ENTRY(0x85A6C2A0u, NvAPI_D3D12_GetRaytracingCaps),
    ENTRY(0x8D025B77u, NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx),
    ENTRY(0xE24EAD45u, NvAPI_D3D12_BuildRaytracingAccelerationStructureEx),
    // Common benign no-op probes seen in wrappers/tools.
    ENTRY(0x33C7358Cu, NvAPI_PlaceholderOk),
    ENTRY(0x593E8644u, NvAPI_PlaceholderOk),
    ENTRY(0xE9B009B9u, NvAPI_PlaceholderOk),
};

#undef ENTRY

} // namespace

extern "C" void* __cdecl nvapi_QueryInterface(NvU32 id)
{
    const unsigned long queryIndex = ++g_nvapiQueryCount;
    for (const InterfaceEntry& entry : kInterfaces) {
        if (entry.id == id) {
            if (isNvapiRtCompatibilityInterface(id) && !nvapiRtCompatibilityEnabled()) {
                tracef("nvapi_QueryInterface id=0x%08X -> %s suppressed (rtCompat disabled)\r\n", id, entry.name);
                apusr::host_discovery::eventf("nvapi", "query.suppressed",
                                             "index=%lu id=0x%08X name=%s reason=rt_compat_disabled",
                                             queryIndex, id, entry.name);
                return nullptr;
            }
            tracef("nvapi_QueryInterface id=0x%08X -> %s\r\n", id, entry.name);
            apusr::host_discovery::eventf("nvapi", "query.known", "index=%lu id=0x%08X name=%s",
                                         queryIndex, id, entry.name);
            return entry.fn;
        }
    }

    // Match the behavior used by mature NVAPI compatibility layers: truly unknown IDs return null.
    // Returning a generic callable stub for every unknown probe is risky because callers may treat
    // non-null as "this feature exists" and enter NVIDIA-only paths on the real AMD device.
    const unsigned long unknownIndex = ++g_nvapiUnknownQueryCount;
    bool expected = false;
    if (g_loggedUnknownId.compare_exchange_strong(expected, true)) {
        tracef("nvapi_QueryInterface first unknown id=0x%08X -> null\r\n", id);
        apusr::host_discovery::eventf("nvapi", "query.unknown.first",
                                     "index=%lu unknown_index=%lu id=0x%08X result=null",
                                     queryIndex, unknownIndex, id);
    } else {
        // Unknown probes can occur thousands of times per launch. Preserve the
        // first event plus logarithmic progress samples without allowing one
        // benign polling ID to drown the renderer/Streamline diagnostics.
        const bool powerOfTwoSample = (unknownIndex & (unknownIndex - 1ul)) == 0ul;
        if (powerOfTwoSample) {
            tracef("nvapi_QueryInterface unknown id=0x%08X -> null (sample %lu)\r\n", id, unknownIndex);
            apusr::host_discovery::eventf("nvapi", "query.unknown.sample",
                                         "index=%lu unknown_index=%lu id=0x%08X result=null sampling=power_of_two",
                                         queryIndex, unknownIndex, id);
        }
    }
    return nullptr;
}

extern "C" void* __cdecl nvapi_Direct_GetMethod(NvU32 id)
{
    // Several NVAPI loaders export/use nvapi_Direct_GetMethod as the same dispatch table
    // resolver as nvapi_QueryInterface. Keep this as a true aliasing wrapper so the .def
    // export surface and implementation stay in lockstep.
    apusr::host_discovery::eventf("nvapi", "direct_get_method", "id=0x%08X", id);
    return nvapi_QueryInterface(id);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        rememberModuleDir(instance);
        apusr::host_discovery::moduleLoaded("nvapi", instance);
        apusr::host_discovery::event("host", "verdict", "loaded=nvapi next=wait_for_nvapi_queryinterface_or_ngx_init");
    }
    return TRUE;
}

#endif // _WIN32
