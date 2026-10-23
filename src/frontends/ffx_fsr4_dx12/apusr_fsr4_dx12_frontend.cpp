// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_fsr4_dx12_frontend.h>
#include <apusr/frontends/apusr_fsr4_dx12_runtime.h>
#include <apusr/frontends/apusr_fsr4_dx12_shaders.h>
#include <apusr/frontends/apusr_dx12_queue_bridge.h>

#include <apusr/adapters/apusr_ffx_fsr4_adapter.h>
#include <apusr/adapters/apusr_fsr4_boundary.h>
#include <apusr/backends/apusr_fsr4_reference_dx12.h>
#include <apusr/backends/apusr_gfx_dx12.h>
#include <apusr/backends/apusr_npu_xrt.h>
#include <apusr/apusr_diagnostics.h>
#include <apusr/apusr_frontend.h>
#include <apusr/apusr_pipeline.h>
#include <apusr/apusr_profiler.h>
#include <apusr/apusr_upscaler_common.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <cwctype>
#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <thread>
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
#endif

#ifndef APUSR_ENABLE_XRT_BACKEND
#define APUSR_ENABLE_XRT_BACKEND 0
#endif

#ifndef APUSR_ENABLE_DX12_BACKEND
#define APUSR_ENABLE_DX12_BACKEND 0
#endif

#ifndef APUSR_REQUIRE_PACKAGED_NATIVE
#define APUSR_REQUIRE_PACKAGED_NATIVE 0
#endif

#ifndef APUSR_REPO_ROOT
#define APUSR_REPO_ROOT "."
#endif

namespace {

constexpr std::uint32_t kContextMagic = 0x41505553u; // APUS


[[nodiscard]] bool envTruthLocal(const char* name)
{
    if (name == nullptr) {
        return false;
    }
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const char c = value[0];
    return c != '0' && c != 'n' && c != 'N' && c != 'f' && c != 'F' && c != 'o' && c != 'O';
}

[[nodiscard]] std::uint64_t envU64Local(const char* name, const std::uint64_t fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end != value ? static_cast<std::uint64_t>(parsed) : fallback;
}

[[nodiscard]] std::string envStringLocal(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::string{};
}


[[nodiscard]] std::filesystem::path defaultFfxRouteTracePath()
{
    const std::string explicitPath = envStringLocal("APUSR_FFX_ROUTE_TRACE_FILE");
    if (!explicitPath.empty()) {
        return explicitPath;
    }
    const std::string explicitDir = envStringLocal("APUSR_LOG_DIR");
    if (!explicitDir.empty()) {
        return std::filesystem::path(explicitDir) / "apusr_ffx_dx12_route_trace.log";
    }
#if defined(_WIN32)
    return std::filesystem::path("C:/NGXLogs/APUSR/apusr_ffx_dx12_route_trace.log");
#else
    return std::filesystem::path("/tmp/APUSR/apusr_ffx_dx12_route_trace.log");
#endif
}

[[nodiscard]] bool ffxRouteTraceEnabled()
{
    static const bool enabled = envTruthLocal("APUSR_FFX_ROUTE_TRACE");
    return enabled;
}

void traceFfxRouteLine(const std::string& line)
{
    if (!ffxRouteTraceEnabled()) {
        return;
    }

    static const std::uint64_t limit = envU64Local("APUSR_FFX_ROUTE_TRACE_LIMIT", 0);
    static std::atomic<std::uint64_t> sequence{0};
    const std::uint64_t id = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (limit != 0 && id > limit) {
        return;
    }

    static std::mutex mutex;
    static std::ofstream file = []() {
        const std::filesystem::path path = defaultFfxRouteTracePath();
        std::error_code ec;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), ec);
        }
        std::ofstream opened(path, std::ios::out | std::ios::trunc);
        if (opened) {
            opened << "APUSR FFX DX12 route trace attached path=" << path.string() << '\n';
            opened.flush();
        }
        return opened;
    }();

    std::lock_guard<std::mutex> lock(mutex);
    if (!file) {
        return;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    file << "seq=" << id
         << " time_ms=" << static_cast<long long>(millis)
         << " tid=" << std::hash<std::thread::id>{}(std::this_thread::get_id())
         << ' ' << line << '\n';
    file.flush();
}

#define APUSR_FFX_ROUTE_TRACE(line) do { if (ffxRouteTraceEnabled()) traceFfxRouteLine((line)); } while (false)


[[nodiscard]] bool dx12GpuTimestampsRequestedForReferenceRuntimeRoute() noexcept
{
    static const bool legacyRequested = envTruthLocal("APUSR_DX12_GPU_TIMESTAMPS");
    return legacyRequested || apusr::profile::gpuEnabled();
}

[[nodiscard]] apusr::models::fsr4_int8::PipelineVariant frontendPipelineVariant() noexcept
{
    return apusr::models::fsr4_int8::PipelineVariant::Dp4aOptimized;
}

[[nodiscard]] const char* frontendPipelineVariantName() noexcept
{
    return apusr::models::fsr4_int8::pipelineVariantName(frontendPipelineVariant());
}


constexpr std::uint64_t makeFfxSdkVersion(const std::uint32_t major, const std::uint32_t minor, const std::uint32_t patch)
{
    return (static_cast<std::uint64_t>(major) << 22) |
           (static_cast<std::uint64_t>(minor) << 12) |
           static_cast<std::uint64_t>(patch);
}

constexpr std::uint64_t kFsr4CompatibleProviderId = (0xF5A5CA1Eull << 32) | makeFfxSdkVersion(4, 0, 2);
constexpr const char* kFsr4CompatibleProviderName = "4.0.2";
constexpr std::uint32_t kFsr4LegacyApiVersion = FFX_UPSCALER_MAKE_VERSION(4u, 0u, 2u);
constexpr std::uint32_t kFsr4LatestAcceptedApiVersion = FFX_UPSCALER_VERSION;

struct FfxGlobalDebugState {
    std::atomic<ffxApiMessage> callback{nullptr};
    std::atomic<std::uint32_t> level{FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_SILENCE};
};

FfxGlobalDebugState& globalDebugState() noexcept
{
    static FfxGlobalDebugState state{};
    return state;
}

void configureGlobalDebug(const ffxApiMessage callback, const std::uint32_t level) noexcept
{
    FfxGlobalDebugState& state = globalDebugState();
    state.callback.store(callback, std::memory_order_relaxed);
    state.level.store(level, std::memory_order_relaxed);
}

void emitFfxDebugMessage(const std::uint32_t type, const wchar_t* message) noexcept
{
    const FfxGlobalDebugState& state = globalDebugState();
    const std::uint32_t level = state.level.load(std::memory_order_relaxed);
    const bool enabled = (type == FFX_API_MESSAGE_TYPE_ERROR && level >= FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS) ||
                         (type == FFX_API_MESSAGE_TYPE_WARNING && level >= FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS);
    if (!enabled) {
        return;
    }
    if (const ffxApiMessage callback = state.callback.load(std::memory_order_relaxed); callback != nullptr) {
        callback(type, message);
    }
}


// Some FidelityFX integrations link against the loader DLL, which then loads this provider DLL.
// After CreateContext returns, the loader dispatches Configure/Query/Dispatch/Destroy
// through the provider pointer stored at the first word of the context object. Keep
// this loader-compatible header first in FfxFsr4Dx12Context; direct callers can still
// use the exported ffx* functions below.
class FfxLoaderProviderShim;

ffxReturnCode_t ffxCreateContextImpl(ffxContext* context, ffxCreateContextDescHeader* header, const ffxAllocationCallbacks* allocationCallbacks);
ffxReturnCode_t ffxDestroyContextImpl(ffxContext* context, const ffxAllocationCallbacks* allocationCallbacks);
ffxReturnCode_t ffxConfigureImpl(ffxContext* context, const ffxConfigureDescHeader* header);
ffxReturnCode_t ffxQueryImpl(ffxContext* context, ffxQueryDescHeader* header);
ffxReturnCode_t ffxDispatchImpl(ffxContext* context, const ffxDispatchDescHeader* header);

class FfxLoaderProviderShim {
public:
    virtual ~FfxLoaderProviderShim() = default;

    virtual bool CanProvide(std::uint64_t type) const
    {
        return type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE ||
               type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE ||
               (type & FFX_API_EFFECT_ID_UPSCALE) == FFX_API_EFFECT_ID_UPSCALE;
    }

    virtual bool IsSupported(void*) const
    {
        return true;
    }

    virtual std::uint64_t GetId() const
    {
        return kFsr4CompatibleProviderId;
    }

    virtual const char* GetVersionName() const
    {
        return kFsr4CompatibleProviderName;
    }

    virtual ffxReturnCode_t CreateContext(ffxContext* context, ffxCreateContextDescHeader* header, void*) const
    {
        return ffxCreateContextImpl(context, header, nullptr);
    }

    virtual ffxReturnCode_t DestroyContext(ffxContext* context, void*) const
    {
        return ffxDestroyContextImpl(context, nullptr);
    }

    virtual ffxReturnCode_t Configure(ffxContext* context, const ffxConfigureDescHeader* header) const
    {
        return ffxConfigureImpl(context, header);
    }

    virtual ffxReturnCode_t Query(ffxContext* context, ffxQueryDescHeader* header) const
    {
        return ffxQueryImpl(context, header);
    }

    virtual ffxReturnCode_t Dispatch(ffxContext* context, const ffxDispatchDescHeader* header) const
    {
        return ffxDispatchImpl(context, header);
    }
};

const FfxLoaderProviderShim& loaderProviderShim()
{
    static const FfxLoaderProviderShim provider{};
    return provider;
}

using FfxApiCreateContextFn = ffxReturnCode_t (*)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using FfxApiDestroyContextFn = ffxReturnCode_t (*)(ffxContext*, const ffxAllocationCallbacks*);
using FfxApiConfigureFn = ffxReturnCode_t (*)(ffxContext*, const ffxConfigureDescHeader*);
using FfxApiQueryFn = ffxReturnCode_t (*)(ffxContext*, ffxQueryDescHeader*);
using FfxApiDispatchFn = ffxReturnCode_t (*)(ffxContext*, const ffxDispatchDescHeader*);

struct FfxDx12PassthroughProvider {
    void* module = nullptr;
    FfxApiCreateContextFn createContext = nullptr;
    FfxApiDestroyContextFn destroyContext = nullptr;
    FfxApiConfigureFn configure = nullptr;
    FfxApiQueryFn query = nullptr;
    FfxApiDispatchFn dispatch = nullptr;
    std::filesystem::path loadedPath;

    [[nodiscard]] bool available() const noexcept
    {
        return module != nullptr && createContext != nullptr && destroyContext != nullptr &&
               configure != nullptr && query != nullptr && dispatch != nullptr;
    }
};

#if defined(_WIN32)

std::wstring widenLocalUtf8(const std::string& value)
{
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed - 1), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), needed);
    return wide;
}

std::filesystem::path currentFfxFrontendModulePath()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&currentFfxFrontendModulePath),
            &module) || module == nullptr) {
        return {};
    }

    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::wstring normalizedPathKey(std::filesystem::path path)
{
    std::error_code ec;
    path = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        path = std::filesystem::absolute(path, ec);
    }
    std::wstring key = path.wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return key;
}

void* loadFfxPassthroughProc(void* module, const char* name)
{
    return module != nullptr ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name)) : nullptr;
}

[[nodiscard]] bool tryLoadFfxPassthroughCandidate(
    FfxDx12PassthroughProvider& provider,
    const std::filesystem::path& candidate,
    const std::wstring& selfKey)
{
    if (candidate.empty()) {
        return false;
    }
    const std::wstring candidateKey = normalizedPathKey(candidate);
    if (!selfKey.empty() && candidateKey == selfKey) {
        return false;
    }

    HMODULE module = LoadLibraryW(candidate.wstring().c_str());
    if (module == nullptr) {
        return false;
    }

    FfxDx12PassthroughProvider loaded{};
    loaded.module = module;
    loaded.createContext = reinterpret_cast<FfxApiCreateContextFn>(loadFfxPassthroughProc(module, "ffxCreateContext"));
    loaded.destroyContext = reinterpret_cast<FfxApiDestroyContextFn>(loadFfxPassthroughProc(module, "ffxDestroyContext"));
    loaded.configure = reinterpret_cast<FfxApiConfigureFn>(loadFfxPassthroughProc(module, "ffxConfigure"));
    loaded.query = reinterpret_cast<FfxApiQueryFn>(loadFfxPassthroughProc(module, "ffxQuery"));
    loaded.dispatch = reinterpret_cast<FfxApiDispatchFn>(loadFfxPassthroughProc(module, "ffxDispatch"));
    loaded.loadedPath = candidate;
    if (!loaded.available()) {
        FreeLibrary(module);
        return false;
    }

    provider = loaded;
    APUSR_FFX_ROUTE_TRACE(std::string("ffx.passthrough.loaded path=") + provider.loadedPath.string());
    return true;
}

#else

void* loadFfxPassthroughProc(void*, const char*)
{
    return nullptr;
}

#endif

const FfxDx12PassthroughProvider& ffxDx12PassthroughProvider()
{
    static FfxDx12PassthroughProvider provider = [] {
        FfxDx12PassthroughProvider loaded{};
#if defined(_WIN32)
        const std::filesystem::path self = currentFfxFrontendModulePath();
        const std::filesystem::path dir = self.empty() ? std::filesystem::current_path() : self.parent_path();
        const std::wstring selfKey = self.empty() ? std::wstring{} : normalizedPathKey(self);

        std::vector<std::filesystem::path> candidates;
        const std::string explicitPath = envStringLocal("APUSR_FFX_DX12_PASSTHROUGH_DLL");
        if (!explicitPath.empty()) {
            candidates.emplace_back(widenLocalUtf8(explicitPath));
        }

        // SDK 2.x split DLLs: the loader is the safest delegation point when present.
        candidates.emplace_back(dir / L"amd_fidelityfx_loader_dx12.dll");
        candidates.emplace_back(dir / L"amd_fidelityfx_framegeneration_dx12.dll");

        // Older FSR3-era integrations can ship the
        // effect stack under ffx_*_x64.dll names rather than SDK-2.x provider names.
        // These candidates are harmless when they do not export the five-function FFX
        // API, and allow native Frame Generation passthrough when a title's FSR3 DLL
        // is also an FFX provider.
        candidates.emplace_back(dir / L"ffx_fsr3_x64.dll");
        candidates.emplace_back(dir / L"ffx_frameinterpolation_x64.dll");

        // Sidecar names for preserving an original monolithic provider beside APUSR's drop-in DLL.
        candidates.emplace_back(dir / L"amd_fidelityfx_dx12_host.dll");
        candidates.emplace_back(dir / L"amd_fidelityfx_dx12.real.dll");
        candidates.emplace_back(dir / L"amd_fidelityfx_dx12.original.dll");

        for (const auto& candidate : candidates) {
            if (tryLoadFfxPassthroughCandidate(loaded, candidate, selfKey)) {
                break;
            }
        }
        if (!loaded.available()) {
            APUSR_FFX_ROUTE_TRACE("ffx.passthrough.unavailable");
        }
#endif
        return loaded;
    }();
    return provider;
}

ffxReturnCode_t ffxPassthroughCreateContext(
    ffxContext* context,
    ffxCreateContextDescHeader* header,
    const ffxAllocationCallbacks* allocationCallbacks)
{
    const FfxDx12PassthroughProvider& provider = ffxDx12PassthroughProvider();
    if (!provider.available()) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    return provider.createContext(context, header, allocationCallbacks);
}

ffxReturnCode_t ffxPassthroughDestroyContext(ffxContext* context, const ffxAllocationCallbacks* allocationCallbacks)
{
    const FfxDx12PassthroughProvider& provider = ffxDx12PassthroughProvider();
    if (!provider.available()) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    return provider.destroyContext(context, allocationCallbacks);
}

ffxReturnCode_t ffxPassthroughConfigure(ffxContext* context, const ffxConfigureDescHeader* header)
{
    const FfxDx12PassthroughProvider& provider = ffxDx12PassthroughProvider();
    if (!provider.available()) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    return provider.configure(context, header);
}

ffxReturnCode_t ffxPassthroughQuery(ffxContext* context, ffxQueryDescHeader* header)
{
    const FfxDx12PassthroughProvider& provider = ffxDx12PassthroughProvider();
    if (!provider.available()) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    return provider.query(context, header);
}

ffxReturnCode_t ffxPassthroughDispatch(ffxContext* context, const ffxDispatchDescHeader* header)
{
    const FfxDx12PassthroughProvider& provider = ffxDx12PassthroughProvider();
    if (!provider.available()) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    return provider.dispatch(context, header);
}

float upscaleRatioForQualityMode(const std::uint32_t qualityMode)
{
    return apusr::adapters::fsr4UpscaleRatioForQualityMode(qualityMode);
}

std::uint32_t renderExtentFromDisplayExtent(const std::uint32_t displayExtent, const float upscaleRatio)
{
    return apusr::adapters::fsr4RenderExtentFromDisplayExtent(displayExtent, upscaleRatio);
}

ffxReturnCode_t queryProviderVersions(ffxQueryDescGetVersions& query)
{
    if (query.createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE || query.outputCount == nullptr) {
        return FFX_API_RETURN_NO_PROVIDER;
    }

    const std::uint64_t capacity = *query.outputCount;
    if (capacity == 0 || (query.versionIds == nullptr && query.versionNames == nullptr)) {
        *query.outputCount = 1;
        return FFX_API_RETURN_OK;
    }

    *query.outputCount = 1;
    if (query.versionIds != nullptr) {
        query.versionIds[0] = kFsr4CompatibleProviderId;
    }
    if (query.versionNames != nullptr) {
        query.versionNames[0] = kFsr4CompatibleProviderName;
    }
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t queryUpscaleRatio(ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode& query)
{
    if (query.pOutUpscaleRatio == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const float ratio = upscaleRatioForQualityMode(query.qualityMode);
    if (ratio <= 0.0f) {
        *query.pOutUpscaleRatio = 0.0f;
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    *query.pOutUpscaleRatio = ratio;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t queryRenderResolution(ffxQueryDescUpscaleGetRenderResolutionFromQualityMode& query)
{
    if (query.pOutRenderWidth == nullptr || query.pOutRenderHeight == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    const float ratio = upscaleRatioForQualityMode(query.qualityMode);
    if (ratio <= 0.0f || query.displayWidth == 0 || query.displayHeight == 0) {
        *query.pOutRenderWidth = 0;
        *query.pOutRenderHeight = 0;
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    *query.pOutRenderWidth = renderExtentFromDisplayExtent(query.displayWidth, ratio);
    *query.pOutRenderHeight = renderExtentFromDisplayExtent(query.displayHeight, ratio);
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t queryGpuMemoryUsage(FfxApiEffectMemoryUsage* usage)
{
    if (usage == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    usage->totalUsageInBytes = 0;
    usage->aliasableUsageInBytes = 0;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t queryResourceRequirements(ffxQueryDescUpscaleGetResourceRequirements& query)
{
    query.required_resources = FFX_API_QUERY_RESOURCE_INPUT_COLOR |
                               FFX_API_QUERY_RESOURCE_INPUT_DEPTH |
                               FFX_API_QUERY_RESOURCE_INPUT_MV |
                               FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE;
    query.optional_resources = 0;
    return FFX_API_RETURN_OK;
}

enum class FfxFsr4Dx12DispatchRoute : std::uint32_t {
    None,
    NativeNpu,
    ReferenceDx12,
};


[[nodiscard]] const char* routeName(const FfxFsr4Dx12DispatchRoute route) noexcept
{
    switch (route) {
    case FfxFsr4Dx12DispatchRoute::None:
        return "none";
    case FfxFsr4Dx12DispatchRoute::NativeNpu:
        return "native-npu";
    case FfxFsr4Dx12DispatchRoute::ReferenceDx12:
        return "reference-dx12";
    }
    return "unknown";
}

struct FfxFsr4Dx12Context {
    // Must be first. The FFX loader treats ffxContext as InternalContextHeader.
    const FfxLoaderProviderShim* provider = &loaderProviderShim();
    std::uint32_t magic = kContextMagic;
    apusr::RenderSize maxRenderSize;
    apusr::DisplaySize maxActiveOutputSize;
    std::uint32_t flags = 0;
    std::uint32_t requestedApiVersion = kFsr4LegacyApiVersion;
    void* device = nullptr;
    void* queue = nullptr;
    apusr::frontends::Dx12QueueSourceKind queueSource = apusr::frontends::Dx12QueueSourceKind::ExplicitNativeConfig;
    std::uint64_t nextFrameIndex = 0;
    apusr::Jitter previousJitter;
    bool hasPreviousJitter = false;
    apusr::PipelineSession pipeline;
    std::unique_ptr<apusr::frontends::Fsr4Dx12TokenShaderRecorder> tokenShaders;
    std::unique_ptr<apusr::backends::Dx12Fsr4ReferenceRuntime> referenceRuntime;
    apusr::Status referenceRuntimeConfigureStatus = apusr::Status::ok();
    bool referenceRuntimeConfigureAttempted = false;
    bool referenceRuntimeConfigured = false;
    bool referenceRuntimeHasHistory = false;
    bool referenceRuntimeRestoreExternalStates = true;
    float referenceRuntimePreviousPreExposure = 1.0f;
    FfxFsr4Dx12DispatchRoute lastSuccessfulDispatchRoute = FfxFsr4Dx12DispatchRoute::None;
    ApusrFsr4Dx12FrontendTelemetry telemetry;
    apusr::NpuSubmissionStats lastSubmissionStats;
    apusr::frontends::Fsr4Dx12PackagedRuntimeConfig packagedRuntimeConfig;
    apusr::Status packagedRuntimeConfigStatus = apusr::Status::ok();
    apusr::Status packagedRuntimeConfigureStatus = apusr::Status::ok();
    bool packagedRuntimeConfigureAttempted = false;
    bool pipelineConfigured = false;
};

const apusr::FrontendCompatibilityDesc& profile()
{
    static const apusr::FrontendCompatibilityDesc desc = apusr::makeFfxFsr4Dx12DropInFrontend();
    return desc;
}

FfxFsr4Dx12Context* checkedContext(ffxContext* context)
{
    if (context == nullptr || *context == nullptr) {
        return nullptr;
    }
    auto* typedContext = static_cast<FfxFsr4Dx12Context*>(*context);
    if (typedContext->provider != &loaderProviderShim() || typedContext->magic != kContextMagic) {
        return nullptr;
    }
    return typedContext;
}

ffxReturnCode_t returnCode(const apusr::Status& status)
{
    switch (status.code) {
    case apusr::StatusCode::Ok:
        return FFX_API_RETURN_OK;
    case apusr::StatusCode::InvalidArgument:
        return FFX_API_RETURN_ERROR_PARAMETER;
    case apusr::StatusCode::UnsupportedBackend:
    case apusr::StatusCode::MissingBackend:
    case apusr::StatusCode::NotImplemented:
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    case apusr::StatusCode::RuntimeFailure:
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    return FFX_API_RETURN_ERROR;
}

ffxReturnCode_t parseUpscaleApiVersion(ffxCreateContextDescHeader* header, std::uint32_t& version)
{
    version = kFsr4LegacyApiVersion;
    for (ffxApiHeader* cursor = header == nullptr ? nullptr : header->pNext; cursor != nullptr; cursor = cursor->pNext) {
        if (cursor->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION) {
            continue;
        }
        const auto* versionDesc = static_cast<const ffxCreateContextDescUpscaleVersion*>(static_cast<const void*>(cursor));
        if (versionDesc->version < kFsr4LegacyApiVersion || versionDesc->version > kFsr4LatestAcceptedApiVersion) {
            emitFfxDebugMessage(FFX_API_MESSAGE_TYPE_ERROR, L"ffxCreateContextDescUpscaleVersion version is outside APUSR's supported FSR4 API range.");
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        version = versionDesc->version;
        break;
    }
    return FFX_API_RETURN_OK;
}

void* findDx12Device(ffxCreateContextDescHeader* header)
{
    for (ffxApiHeader* cursor = header == nullptr ? nullptr : header->pNext; cursor != nullptr; cursor = cursor->pNext) {
        if (cursor->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12) {
            return static_cast<ffxCreateBackendDX12Desc*>(static_cast<void*>(cursor))->device;
        }
    }
    return nullptr;
}


const char* nativeExecutionModeName(const apusr::NativeBringupStage stage)
{
    return apusr::productNativeStage(stage) ? "FrameConveyorNative" : "Invalid";
}



apusr::NpuSubmissionStats subtractSubmissionStats(
    const apusr::NpuSubmissionStats& current,
    const apusr::NpuSubmissionStats& previous)
{
    return apusr::NpuSubmissionStats{
        .frameWorklistCount = current.frameWorklistCount - previous.frameWorklistCount,
        .doorbellDescriptorCount = current.doorbellDescriptorCount - previous.doorbellDescriptorCount,
        .persistentDoorbellFrameCount = current.persistentDoorbellFrameCount - previous.persistentDoorbellFrameCount,
        .persistentControllerStartCount = current.persistentControllerStartCount - previous.persistentControllerStartCount,
        .doorbellRing = apusr::NpuDoorbellRingStats{
            .reserveCount = current.doorbellRing.reserveCount - previous.doorbellRing.reserveCount,
            .commitCount = current.doorbellRing.commitCount - previous.doorbellRing.commitCount,
            .stagedDescriptorCount = current.doorbellRing.stagedDescriptorCount - previous.doorbellRing.stagedDescriptorCount,
            .occupancyHighWatermark = current.doorbellRing.occupancyHighWatermark,
            .controlRingWriteCount = current.doorbellRing.controlRingWriteCount - previous.doorbellRing.controlRingWriteCount,
            .controlRingWriteBytes = current.doorbellRing.controlRingWriteBytes - previous.doorbellRing.controlRingWriteBytes,
            .publishedSequence = current.doorbellRing.publishedSequence,
            .retiredSequence = current.doorbellRing.retiredSequence,
            .completedSequence = current.doorbellRing.completedSequence,
        },
        .frameCommandRunCount = current.frameCommandRunCount - previous.frameCommandRunCount,
        .tokenBandRunCount = current.tokenBandRunCount - previous.tokenBandRunCount,
        .submitWaitCount = current.submitWaitCount - previous.submitWaitCount,
        .startCount = current.startCount - previous.startCount,
        .submitSignalCount = current.submitSignalCount - previous.submitSignalCount,
        .enqueueFrameCpuNanoseconds = current.enqueueFrameCpuNanoseconds - previous.enqueueFrameCpuNanoseconds,
        .enqueueTokenBandCpuNanoseconds = current.enqueueTokenBandCpuNanoseconds - previous.enqueueTokenBandCpuNanoseconds,
        .payloadSync = apusr::NpuPayloadSyncStats{
            .toDeviceCount = current.payloadSync.toDeviceCount - previous.payloadSync.toDeviceCount,
            .toDeviceBytes = current.payloadSync.toDeviceBytes - previous.payloadSync.toDeviceBytes,
            .fromDeviceCount = current.payloadSync.fromDeviceCount - previous.payloadSync.fromDeviceCount,
            .fromDeviceBytes = current.payloadSync.fromDeviceBytes - previous.payloadSync.fromDeviceBytes,
        },
    };
}

std::string submissionStatsTraceString(
    const char* prefix,
    const apusr::NpuSubmissionStats& stats)
{
    std::string message = prefix;
    message += " frameWorklists=" + std::to_string(stats.frameWorklistCount);
    message += " doorbellDescriptors=" + std::to_string(stats.doorbellDescriptorCount);
    message += " persistentDoorbellFrames=" + std::to_string(stats.persistentDoorbellFrameCount);
    message += " persistentControllerStarts=" + std::to_string(stats.persistentControllerStartCount);
    message += " doorbellRingReserves=" + std::to_string(stats.doorbellRing.reserveCount);
    message += " doorbellRingCommits=" + std::to_string(stats.doorbellRing.commitCount);
    message += " doorbellRingStagedDescriptors=" + std::to_string(stats.doorbellRing.stagedDescriptorCount);
    message += " doorbellRingHighWatermark=" + std::to_string(stats.doorbellRing.occupancyHighWatermark);
    message += " doorbellRingControlWrites=" + std::to_string(stats.doorbellRing.controlRingWriteCount);
    message += " doorbellRingControlBytes=" + std::to_string(stats.doorbellRing.controlRingWriteBytes);
    message += " doorbellRingPublished=" + std::to_string(stats.doorbellRing.publishedSequence);
    message += " doorbellRingRetired=" + std::to_string(stats.doorbellRing.retiredSequence);
    message += " doorbellRingCompleted=" + std::to_string(stats.doorbellRing.completedSequence);
    message += " frameCommandRuns=" + std::to_string(stats.frameCommandRunCount);
    message += " tokenBandRuns=" + std::to_string(stats.tokenBandRunCount);
    message += " waits=" + std::to_string(stats.submitWaitCount);
    message += " starts=" + std::to_string(stats.startCount);
    message += " signals=" + std::to_string(stats.submitSignalCount);
    message += " enqueueFrameNs=" + std::to_string(stats.enqueueFrameCpuNanoseconds);
    message += " enqueueBandNs=" + std::to_string(stats.enqueueTokenBandCpuNanoseconds);
    message += " payloadSyncToCount=" + std::to_string(stats.payloadSync.toDeviceCount);
    message += " payloadSyncToBytes=" + std::to_string(stats.payloadSync.toDeviceBytes);
    message += " payloadSyncFromCount=" + std::to_string(stats.payloadSync.fromDeviceCount);
    message += " payloadSyncFromBytes=" + std::to_string(stats.payloadSync.fromDeviceBytes);
    return message;
}

void traceSubmissionStatsDelta(FfxFsr4Dx12Context& context, const char* prefix)
{
    const apusr::NpuSubmissionStats current = context.pipeline.npuSubmissionStats();
    const apusr::NpuSubmissionStats delta = subtractSubmissionStats(current, context.lastSubmissionStats);
    context.lastSubmissionStats = current;
    APUSR_TRACE_DIAGNOSTIC(submissionStatsTraceString(prefix, delta));
}

std::string statusTraceString(const char* prefix, const apusr::Status& status)
{
    std::string message = prefix;
    message += " code=";
    message += apusr::statusCodeName(status.code);
    if (!status.message.empty()) {
        message += " message=";
        message += status.message;
    }
    return message;
}


apusr::models::fsr4_int8::Preset referencePresetForFfxBoundaryPreset(
    const apusr::adapters::Fsr4ModelPreset preset) noexcept
{
    using FfxPreset = apusr::adapters::Fsr4ModelPreset;
    using ReferencePreset = apusr::models::fsr4_int8::Preset;
    switch (preset) {
    case FfxPreset::NativeAA:
        return ReferencePreset::NativeAA;
    case FfxPreset::Quality:
        return ReferencePreset::Quality;
    case FfxPreset::Balanced:
        return ReferencePreset::Balanced;
    case FfxPreset::Performance:
        return ReferencePreset::Performance;
    case FfxPreset::DynamicResolution:
        return ReferencePreset::DynamicResolution;
    case FfxPreset::UltraPerformance:
        return ReferencePreset::UltraPerformance;
    }
    return ReferencePreset::Balanced;
}

std::string getenvString(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string{value} : std::string{};
}

bool hasReferenceInitializerArtifacts(const std::filesystem::path& repoRoot)
{
    std::error_code error;
    return std::filesystem::is_regular_file(
        repoRoot / "models" / "fsr4_int8" / "reference" / "fsr4_4_0_2_int8" / "balanced" / "initializers.bin",
        error);
}

std::filesystem::path referenceFrontendModuleDirectory()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&referenceFrontendModuleDirectory),
            &module) != 0 &&
        module != nullptr) {
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length != 0 && length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
    }
#endif
    return {};
}

std::string defaultReferenceRepoRoot()
{
    const std::string explicitRepo = getenvString("APUSR_FSR4_REFERENCE_REPO_ROOT");
    if (!explicitRepo.empty()) {
        return explicitRepo;
    }

    const std::string legacyRepo = getenvString("APUSR_REPO_ROOT");
    if (!legacyRepo.empty()) {
        return legacyRepo;
    }

    const std::filesystem::path compiledRepo = std::filesystem::path(APUSR_REPO_ROOT);
    if (hasReferenceInitializerArtifacts(compiledRepo)) {
        return compiledRepo.string();
    }

    const std::filesystem::path moduleDir = referenceFrontendModuleDirectory();
    if (!moduleDir.empty() && hasReferenceInitializerArtifacts(moduleDir)) {
        return moduleDir.string();
    }

    std::error_code error;
    const std::filesystem::path cwd = std::filesystem::current_path(error);
    if (!error && hasReferenceInitializerArtifacts(cwd)) {
        return cwd.string();
    }

    return {};
}

std::string defaultReferenceShaderCacheRoot()
{
    const std::string env = getenvString("APUSR_FSR4_REFERENCE_DX12_SHADER_CACHE_ROOT");
    if (!env.empty()) {
        return env;
    }
    const std::filesystem::path artifactRoot =
        std::filesystem::path(APUSR_REPO_ROOT) /
        "artifacts" /
        "fsr4_reference_dx12_product" /
        "fsr4_reference_dx12_shader_cache";
    std::error_code error;
    if (std::filesystem::is_directory(artifactRoot, error)) {
        return artifactRoot.string();
    }
    return {};
}

std::string defaultReferenceReflectionManifestPath()
{
    const std::string env = getenvString("APUSR_FSR4_REFERENCE_SHADER_RESOURCE_MANIFEST");
    if (!env.empty()) {
        return env;
    }
    const std::filesystem::path artifactPath =
        std::filesystem::path(APUSR_REPO_ROOT) /
        "artifacts" /
        "fsr4_reference_dx12_product" /
        "fsr4_reference_dx12_shader_resource_manifest.json";
    std::error_code error;
    if (std::filesystem::is_regular_file(artifactPath, error)) {
        return artifactPath.string();
    }
    return {};
}

apusr::Status configureReferenceDx12RuntimeIfEnabled(FfxFsr4Dx12Context& context)
{
#if APUSR_ENABLE_DX12_BACKEND
    if (context.referenceRuntimeConfigured) {
        return apusr::Status::ok();
    }
    if (context.referenceRuntimeConfigureAttempted) {
        return context.referenceRuntimeConfigureStatus;
    }
    context.referenceRuntimeConfigureAttempted = true;
    if (context.device == nullptr) {
        context.referenceRuntimeConfigureStatus = apusr::Status::missingBackend(
            "FSR4 DX12 reference runtime requires a DX12 device from ffxCreateBackendDX12Desc");
        APUSR_FFX_ROUTE_TRACE(statusTraceString("reference.configure.failed", context.referenceRuntimeConfigureStatus));
        return context.referenceRuntimeConfigureStatus;
    }

    apusr::backends::Dx12Fsr4ReferenceCreateDesc create{};
    create.device = apusr::NativeHandle{context.device};
    create.repoRoot = defaultReferenceRepoRoot();
    create.runner.maxRenderSize = context.maxRenderSize;
    create.runner.maxUpscaleSize = context.maxActiveOutputSize;
    create.runner.features = apusr::frontends::decodeFfxUpscaleFeatureFlags(context.flags);
    create.runner.pipelineVariant = frontendPipelineVariant();
    create.runner.allowDebugVisualization = create.runner.features.debugVisualization;
    create.runner.supportsWmma = false;
    create.runner.initialPreset = apusr::models::fsr4_int8::Preset::Balanced;
    create.shaderProvider.enablePackagedCache = true;
    create.shaderProvider.enableEmbeddedCache = true;
    create.shaderProvider.allowRuntimeCompilation = false;
    create.shaderProvider.requireCompletePrecompiledCache = false;
    create.shaderProvider.packagedCacheRoot = defaultReferenceShaderCacheRoot();
    create.reflectionManifestPath = defaultReferenceReflectionManifestPath();
    {
        std::ostringstream detail;
        detail << "reference.configure.begin device=present"
               << " maxRender=" << context.maxRenderSize.width << 'x' << context.maxRenderSize.height
               << " maxUpscale=" << context.maxActiveOutputSize.width << 'x' << context.maxActiveOutputSize.height
               << " flags=0x" << std::hex << context.flags << std::dec
               << " variant=" << frontendPipelineVariantName()
               << " repoRoot=" << (create.repoRoot.empty() ? "<empty>" : create.repoRoot)
               << " cacheRoot=" << (create.shaderProvider.packagedCacheRoot.empty() ? "<empty>" : create.shaderProvider.packagedCacheRoot)
               << " manifest=" << (create.reflectionManifestPath.empty() ? "<empty>" : create.reflectionManifestPath);
        APUSR_FFX_ROUTE_TRACE(detail.str());
    }
    create.enableReflectionManifestValidation = !create.reflectionManifestPath.empty();
    create.requireCompleteReflectionCoverage = false;
    create.requireInitializerBytes = true;

    auto runtime = apusr::backends::createDx12Fsr4ReferenceRuntime();
    if (!runtime) {
        context.referenceRuntimeConfigureStatus = apusr::Status::missingBackend(
            "could not create FSR4 DX12 reference runtime");
        return context.referenceRuntimeConfigureStatus;
    }
    context.referenceRuntimeConfigureStatus = runtime->initialize(create);
    if (!context.referenceRuntimeConfigureStatus.succeeded()) {
        APUSR_FFX_ROUTE_TRACE(statusTraceString("reference.configure.failed", context.referenceRuntimeConfigureStatus));
        return context.referenceRuntimeConfigureStatus;
    }

    // Keep the first borrowed game command list clean: create internal resources,
    // PSOs, initializer/default-exposure upload buffers, and the constant-upload
    // ring during context setup instead of during the first FSR dispatch.  This
    // is still a conservative single-preset warmup; later modes can be warmed
    // explicitly once the frontend exposes a stable product policy.
    apusr::backends::Dx12Fsr4ReferenceFrameDesc warmup{};
    warmup.runner.frame.renderSize = context.maxRenderSize;
    warmup.runner.frame.displaySize = context.maxActiveOutputSize;
    warmup.runner.frame.options.features = create.runner.features;
    warmup.runner.frame.options.resetHistory = true;
    warmup.runner.frame.options.hasDepth = true;
    warmup.runner.frame.options.hasExposure = !create.runner.features.autoExposure;
    warmup.runner.frame.options.sharpeningRequested = true;
    warmup.runner.frame.options.preExposure = 1.0f;
    warmup.runner.frame.camera.nearPlane = 0.1f;
    warmup.runner.frame.camera.farPlane = 1000.0f;
    warmup.runner.frame.camera.verticalFovRadians = 1.04719755f;
    warmup.runner.maxUpscaleSize = context.maxActiveOutputSize;
    warmup.runner.residentModelPreset = create.runner.initialPreset;
    warmup.runner.previousPreExposure = 1.0f;
    warmup.runner.forceReset = true;
    APUSR_FFX_ROUTE_TRACE(std::string("reference.warmup.begin preset=balanced rcas=true variant=") + frontendPipelineVariantName());
    context.referenceRuntimeConfigureStatus = runtime->prepareFrameCache(warmup);
    if (!context.referenceRuntimeConfigureStatus.succeeded()) {
        APUSR_FFX_ROUTE_TRACE(statusTraceString("reference.warmup.failed", context.referenceRuntimeConfigureStatus));
        return context.referenceRuntimeConfigureStatus;
    }
    APUSR_FFX_ROUTE_TRACE("reference.warmup.ok");

    context.referenceRuntime = std::move(runtime);
    context.referenceRuntimeConfigured = true;
    APUSR_FFX_ROUTE_TRACE(std::string("reference.configure.ok restoreExternalStates=") +
                      (context.referenceRuntimeRestoreExternalStates ? "true" : "false"));
    return apusr::Status::ok();
#else
    (void)context;
    return apusr::Status::unsupportedBackend("FSR4 DX12 reference runtime requires the DX12 backend");
#endif
}

apusr::Status submitFsr4Dx12ReferenceRuntime(
    FfxFsr4Dx12Context& context,
    const apusr::frontends::FfxFsr4DispatchNormalization& normalized)
{
#if APUSR_ENABLE_DX12_BACKEND
    apusr::Status status = configureReferenceDx12RuntimeIfEnabled(context);
    if (!status.succeeded()) {
        return status;
    }
    if (!context.referenceRuntime) {
        return apusr::Status::missingBackend("FSR4 DX12 reference runtime was not configured");
    }

    if (context.queue == nullptr && context.device != nullptr && dx12GpuTimestampsRequestedForReferenceRuntimeRoute()) {
        const apusr::frontends::Dx12ResolvedQueue cachedQueue =
            apusr::frontends::resolveDx12Queue(apusr::NativeHandle{context.device});
        if (cachedQueue.available) {
            context.queue = cachedQueue.queue.value;
            context.queueSource = cachedQueue.source;
        }
    }

    apusr::backends::Dx12Fsr4ReferenceFrameDesc frame{};
    frame.commandList = apusr::NativeHandle{normalized.commandList};
    frame.commandQueue = apusr::NativeHandle{context.queue};
    frame.runner.frame = normalized.frame;
    frame.runner.maxUpscaleSize = context.maxActiveOutputSize;
    frame.runner.residentModelPreset = referencePresetForFfxBoundaryPreset(normalized.realBoundary.preset);
    frame.runner.previousPreExposure = context.referenceRuntimePreviousPreExposure;
    frame.runner.previousJitter[0] = normalized.frame.previousJitter.x;
    frame.runner.previousJitter[1] = normalized.frame.previousJitter.y;
    frame.runner.forceReset = normalized.frame.options.resetHistory || !context.referenceRuntimeHasHistory;

    frame.externalResources.inputColor = normalized.frame.surfaces.color.resource;
    frame.externalResources.inputDepth = normalized.frame.surfaces.depth.resource;
    frame.externalResources.inputMotionVectors = normalized.frame.surfaces.motionVectors.resource;
    frame.externalResources.inputExposure = normalized.frame.surfaces.exposure.resource;
    frame.externalResources.output = normalized.frame.surfaces.output.resource;
    frame.externalResources.inputColorFormat = normalized.frame.surfaces.color.format;
    frame.externalResources.inputDepthFormat = normalized.frame.surfaces.depth.format;
    frame.externalResources.inputMotionVectorsFormat = normalized.frame.surfaces.motionVectors.format;
    frame.externalResources.inputExposureFormat = normalized.frame.surfaces.exposure.format;
    frame.externalResources.outputFormat = normalized.frame.surfaces.output.format;
    frame.externalResources.inputColorState = normalized.frame.surfaces.color.nativeState;
    frame.externalResources.inputDepthState = normalized.frame.surfaces.depth.nativeState;
    frame.externalResources.inputMotionVectorsState = normalized.frame.surfaces.motionVectors.nativeState;
    frame.externalResources.inputExposureState = normalized.frame.surfaces.exposure.nativeState;
    frame.externalResources.outputState = normalized.frame.surfaces.output.nativeState;
    frame.externalResources.restoreExternalResourceStatesAfterDispatch = context.referenceRuntimeRestoreExternalStates;

    if (ffxRouteTraceEnabled()) {
        std::ostringstream detail;
        detail << "reference.dispatch.begin"
               << " frame=" << normalized.frame.frameIndex
               << " render=" << normalized.frame.renderSize.width << 'x' << normalized.frame.renderSize.height
               << " display=" << normalized.frame.displaySize.width << 'x' << normalized.frame.displaySize.height
               << " maxUpscale=" << context.maxActiveOutputSize.width << 'x' << context.maxActiveOutputSize.height
               << " preset=" << static_cast<unsigned>(normalized.realBoundary.preset)
               << " cmd=" << (normalized.commandList != nullptr ? "present" : "null")
               << " color=" << (frame.externalResources.inputColor.value != nullptr ? "present" : "null")
               << " depth=" << (frame.externalResources.inputDepth.value != nullptr ? "present" : "null")
               << " mv=" << (frame.externalResources.inputMotionVectors.value != nullptr ? "present" : "null")
               << " exposure=" << (frame.externalResources.inputExposure.value != nullptr ? "present" : "null")
               << " output=" << (frame.externalResources.output.value != nullptr ? "present" : "null")
               << " formats=color:" << static_cast<unsigned>(frame.externalResources.inputColorFormat)
               << ",depth:" << static_cast<unsigned>(frame.externalResources.inputDepthFormat)
               << ",mv:" << static_cast<unsigned>(frame.externalResources.inputMotionVectorsFormat)
               << ",exposure:" << static_cast<unsigned>(frame.externalResources.inputExposureFormat)
               << ",output:" << static_cast<unsigned>(frame.externalResources.outputFormat)
               << " reset=" << (frame.runner.forceReset ? "true" : "false")
               << " restoreExternalStates="
               << (frame.externalResources.restoreExternalResourceStatesAfterDispatch ? "true" : "false");
        APUSR_FFX_ROUTE_TRACE(detail.str());
    }
    status = context.referenceRuntime->recordFrame(frame);
    if (!status.succeeded()) {
        APUSR_FFX_ROUTE_TRACE(statusTraceString("reference.dispatch.failed", status));
        return status;
    }
    APUSR_FFX_ROUTE_TRACE("reference.dispatch.ok");
    context.referenceRuntimeHasHistory = true;
    context.referenceRuntimePreviousPreExposure = normalized.frame.options.preExposure;
    return apusr::Status::ok();
#else
    (void)context;
    (void)normalized;
    return apusr::Status::unsupportedBackend("FSR4 DX12 reference runtime requires the DX12 backend");
#endif
}

apusr::Status submitNativeApusrFrame(
    FfxFsr4Dx12Context& context,
    const apusr::frontends::FfxFsr4DispatchNormalization& normalized)
{
    apusr::PipelineFrameDesc frame{
        .frameIndex = normalized.frame.frameIndex,
        .renderSize = normalized.frame.renderSize,
        .displaySize = normalized.frame.displaySize,
        .jitter = normalized.frame.jitter,
        .previousJitter = normalized.frame.previousJitter,
        .camera = normalized.frame.camera,
        .options = normalized.frame.options,
        .surfaces = normalized.frame.surfaces,
        .borrowedCommandList = apusr::NativeHandle{normalized.commandList},
    };

    return context.pipeline.submitFrame(frame);
}

apusr::Status configurePipeline(
    FfxFsr4Dx12Context& context,
    const apusr::frontends::Fsr4Dx12RuntimeConfigureDesc& desc);

apusr::Status configurePackagedNativePipelineIfAvailable(
    FfxFsr4Dx12Context& context,
    const apusr::frontends::FfxFsr4DispatchNormalization& normalized)
{
    if (context.pipelineConfigured || !context.packagedRuntimeConfig.available) {
        return apusr::Status::ok();
    }
    if (!context.packagedRuntimeConfigStatus.succeeded()) {
        return context.packagedRuntimeConfigStatus;
    }
    if (context.packagedRuntimeConfigureAttempted) {
        return context.packagedRuntimeConfigureStatus;
    }

    context.packagedRuntimeConfigureAttempted = true;
    apusr::frontends::Fsr4Dx12PackagedRuntimeConfig selectedConfig{};
    context.packagedRuntimeConfigureStatus = apusr::frontends::selectFsr4Dx12PackagedRuntimeConfig(
        context.packagedRuntimeConfig,
        normalized.realBoundary,
        selectedConfig);
    if (!context.packagedRuntimeConfigureStatus.succeeded()) {
        return context.packagedRuntimeConfigureStatus;
    }
    apusr::frontends::Fsr4Dx12RuntimeConfigureDesc desc =
        apusr::frontends::makeFsr4Dx12RuntimeConfigureDesc(selectedConfig, nullptr);
    context.packagedRuntimeConfigureStatus = configurePipeline(context, desc);
    return context.packagedRuntimeConfigureStatus;
}


apusr::Status submitDiagnosticTileOverlay(
    FfxFsr4Dx12Context& context,
    const apusr::frontends::FfxFsr4DispatchNormalization& normalized)
{
    (void)context;
    (void)normalized;
    return apusr::Status::ok();
}



apusr::Status submitFsr4Dx12Ingress(
    FfxFsr4Dx12Context& context,
    const ffxDispatchDescUpscale& dispatch)
{
    context.lastSuccessfulDispatchRoute = FfxFsr4Dx12DispatchRoute::None;

    apusr::frontends::FfxFsr4DispatchNormalization normalized{};
    apusr::Status status = apusr::frontends::normalizeFfxFsr4Dispatch(
        dispatch,
        context.nextFrameIndex,
        context.flags,
        normalized,
        context.maxActiveOutputSize);
    if (!status.succeeded()) {
        return status;
    }
    if (ffxRouteTraceEnabled()) {
        std::ostringstream detail;
        detail << "dispatch.normalized frame=" << normalized.frame.frameIndex
               << " render=" << normalized.frame.renderSize.width << 'x' << normalized.frame.renderSize.height
               << " display=" << normalized.frame.displaySize.width << 'x' << normalized.frame.displaySize.height
               << " boundaryPreset=" << static_cast<unsigned>(normalized.realBoundary.preset)
               << " flags=0x" << std::hex << normalized.dispatchFlags << std::dec;
        APUSR_FFX_ROUTE_TRACE(detail.str());
    }
    if (!context.hasPreviousJitter || normalized.frame.options.resetHistory) {
        normalized.frame.previousJitter = normalized.frame.jitter;
    } else {
        normalized.frame.previousJitter = context.previousJitter;
    }
    context.previousJitter = normalized.frame.jitter;
    context.hasPreviousJitter = true;
    context.nextFrameIndex += 1;

    if (!context.pipelineConfigured) {
        status = configurePackagedNativePipelineIfAvailable(context, normalized);
        if (!status.succeeded()) {
#if APUSR_REQUIRE_PACKAGED_NATIVE
            return status;
#else
            APUSR_TRACE_DIAGNOSTIC(statusTraceString("ERROR packaged native configure failed", status));
#endif
        }
    }

    if (context.pipelineConfigured) {
        const apusr::NativeBringupStage stage = apusr::compiledNativeBringupStage();
#if defined(APUSR_ENABLE_DX12_TRACE) && APUSR_ENABLE_DX12_TRACE
        std::string nativeSubmitTrace = std::string("BEGIN native submit stage=") + nativeExecutionModeName(stage);
        nativeSubmitTrace += " completeTransportFrame=";
        nativeSubmitTrace += apusr::packagedNativeKernelProducesCompleteTransportFrame(stage) ? "true" : "false";
        nativeSubmitTrace += " modelEgressFrame=";
        nativeSubmitTrace += apusr::packagedNativeKernelProducesModelEgressFrame(stage) ? "true" : "false";
        APUSR_TRACE_DIAGNOSTIC(nativeSubmitTrace);
#endif
        status = submitNativeApusrFrame(context, normalized);
        if (!status.succeeded()) {
            APUSR_TRACE_DIAGNOSTIC(statusTraceString("ERROR native submit failed", status));
            traceSubmissionStatsDelta(context, "NPU submission stats delta after native failure");
            return status;
        }

        const bool producedOutput = context.pipeline.lastSubmitProducedOutput();
        APUSR_TRACE_DIAGNOSTIC(std::string("END native submit status=ok producedOutput=") + (producedOutput ? "true" : "false"));
        traceSubmissionStatsDelta(context, "NPU submission stats delta after native success");

        // The frame-slot path cannot be used as a drop-in reference route until it
        // produces a same-frame output. Do not hide that with a fake frame copy.
        if (!producedOutput) {
            return apusr::Status::missingBackend("APUSR native route produced no same-frame output; fake fallback is not allowed");
        }
        context.lastSuccessfulDispatchRoute = FfxFsr4Dx12DispatchRoute::NativeNpu;
        APUSR_FFX_ROUTE_TRACE(std::string("dispatch.route=") + routeName(context.lastSuccessfulDispatchRoute));
        return apusr::Status::ok();
    }

#if APUSR_ENABLE_DX12_BACKEND
    status = submitFsr4Dx12ReferenceRuntime(context, normalized);
    if (status.succeeded()) {
        context.lastSuccessfulDispatchRoute = FfxFsr4Dx12DispatchRoute::ReferenceDx12;
        APUSR_FFX_ROUTE_TRACE(std::string("dispatch.route=") + routeName(context.lastSuccessfulDispatchRoute));
        return apusr::Status::ok();
    }
    APUSR_TRACE_DIAGNOSTIC(statusTraceString("ERROR FSR4 DX12 reference runtime dispatch failed", status));
    APUSR_FFX_ROUTE_TRACE(statusTraceString("reference.route.failed", status));
    return status;
#endif

#if APUSR_REQUIRE_PACKAGED_NATIVE
    return apusr::Status::missingBackend(
        "APUSR packaged-native build reached dispatch without a configured native NPU pipeline");
#else
    status = apusr::Status::missingBackend("FFX DX12 reference replacement reached dispatch without a real APUSR route");
    APUSR_FFX_ROUTE_TRACE(statusTraceString("dispatch.noRealRoute", status));
    return status;
#endif
}

apusr::Status configurePipeline(
    FfxFsr4Dx12Context& context,
    const apusr::frontends::Fsr4Dx12RuntimeConfigureDesc& desc)
{
#if APUSR_ENABLE_XRT_BACKEND && APUSR_ENABLE_DX12_BACKEND
    if (context.device == nullptr) {
        return apusr::Status::invalidArgument("FSR4 DX12 context requires a DX12 device from ffxCreateBackendDX12Desc");
    }
    void* resolvedQueue = desc.commandQueue;
    apusr::frontends::Dx12QueueSourceKind queueSource = apusr::frontends::Dx12QueueSourceKind::ExplicitNativeConfig;
    if (resolvedQueue != nullptr) {
        const apusr::Status registrationStatus = apusr::frontends::registerDx12Queue(apusr::frontends::Dx12QueueRegistrationDesc{
            .device = apusr::NativeHandle{context.device},
            .queue = apusr::NativeHandle{resolvedQueue},
            .source = apusr::frontends::Dx12QueueSourceKind::ExplicitNativeConfig,
        });
        if (!registrationStatus.succeeded()) {
            return registrationStatus;
        }
    } else {
        const apusr::frontends::Dx12ResolvedQueue cachedQueue =
            apusr::frontends::resolveDx12Queue(apusr::NativeHandle{context.device});
        if (!cachedQueue.available) {
            return apusr::Status::missingBackend(
                "APUSR FSR4 DX12 native path requires a cached direct D3D12 command queue; "
                "install the DX12 queue bridge or provide Fsr4Dx12RuntimeConfigureDesc::commandQueue");
        }
        resolvedQueue = cachedQueue.queue.value;
        queueSource = cachedQueue.source;
    }
    if ((desc.xclbinPath == nullptr || std::strlen(desc.xclbinPath) == 0) && desc.xclbinData.empty()) {
        return apusr::Status::invalidArgument("APUSR FSR4 DX12 fast path requires a packaged XRT xclbin");
    }
    if (desc.kernelName == nullptr || std::strlen(desc.kernelName) == 0) {
        return apusr::Status::invalidArgument("APUSR FSR4 DX12 fast path requires an XRT kernel name");
    }

    context.queue = resolvedQueue;
    context.queueSource = queueSource;

    auto tokenShaders = std::make_unique<apusr::frontends::Fsr4Dx12TokenShaderRecorder>();
    apusr::Status status = tokenShaders->initialize(apusr::NativeHandle{context.device});
    if (!status.succeeded()) {
        return status;
    }

    apusr::backends::Dx12BackendConfig graphicsConfig{
        .device = apusr::NativeHandle{context.device},
        .queue = apusr::NativeHandle{context.queue},
        .recordExportPass = &apusr::frontends::Fsr4Dx12TokenShaderRecorder::recordExportCallback,
        .recordImportPass = &apusr::frontends::Fsr4Dx12TokenShaderRecorder::recordImportCallback,
        .recordUserData = tokenShaders.get(),
        .requireExternalFenceInterop = true,
    };

    apusr::backends::AmdXrtBackendConfig xrtConfig{
        .deviceIndex = desc.xrtDeviceIndex,
        .xclbinPath = desc.xclbinPath != nullptr ? desc.xclbinPath : "",
        .xclbinData = desc.xclbinData,
        .kernelName = desc.kernelName,
        .instructionData = desc.instructionData,
        .parameterLoadInstructionData = desc.parameterLoadInstructionData,
        .opcode = desc.xrtOpcode,
        .kernelAbi = desc.kernelAbi,
        .requireFenceInterop = true,
        .requireUserptrImport = true,
        .preferExclusiveHwContext = false,
    };

    const std::uint32_t tokenBandRows = desc.tokenBandRows != 0 ? desc.tokenBandRows : apusr::kDefaultTokenBandRows;
    const std::uint32_t tokenRingDepth = desc.tokenRingDepth != 0 ? desc.tokenRingDepth : 6;

    apusr::adapters::Fsr4AdapterConfig adapterConfig{
        .maxRenderSize = context.maxRenderSize,
        .maxActiveOutputSize = context.maxActiveOutputSize,
        .tokenRingDepth = tokenRingDepth,
        .tokenBandRows = tokenBandRows,
        .vendorPolicy = apusr::VendorPolicy::PerformanceFirst,
    };

    apusr::RuntimeConfig runtimeConfig = apusr::adapters::makeFsr4RuntimeConfig(
        adapterConfig,
        apusr::backends::createDx12GraphicsBackend(graphicsConfig),
        apusr::backends::createAmdXrtNpuBackend(xrtConfig));
    runtimeConfig.residentParameterArenaData = desc.residentParameterArenaData.empty() ?
        nullptr : desc.residentParameterArenaData.data();
    runtimeConfig.residentParameterArenaBytes = desc.residentParameterArenaData.size();
    runtimeConfig.modelEpochParameterDmaBytes = desc.modelEpochParameterDmaBytes;
    runtimeConfig.hotPathParameterDmaBytesPerRun = desc.hotPathParameterDmaBytesPerRun;
    runtimeConfig.skipSpillArenaBytes = desc.skipSpillArenaBytes;

    apusr::PipelineSession newPipeline;
    status = newPipeline.configure(std::move(runtimeConfig));
    if (!status.succeeded()) {
        return status;
    }

    context.pipeline = std::move(newPipeline);
    context.tokenShaders = std::move(tokenShaders);
    context.pipelineConfigured = true;
    return apusr::Status::ok();
#else
    (void)context;
    (void)desc;
    return apusr::Status::unsupportedBackend(
        "APUSR FSR4 DX12 native fast path requires APUSR_ENABLE_XRT_BACKEND=ON and APUSR_ENABLE_DX12_BACKEND=ON");
#endif
}

} // namespace

extern "C" {

std::uint32_t apusrFsr4Dx12FrontendAbiVersion()
{
    return 3;
}

const char* apusrFsr4Dx12FrontendName()
{
    return profile().implementationName.c_str();
}

const char* apusrFsr4Dx12FrontendOutputBaseName()
{
    return "amd_fidelityfx_dx12";
}

bool apusrFsr4Dx12FrontendUsesFidelityFxHooks()
{
    return profile().usesThirdPartyApiHooks;
}

bool apusrFsr4Dx12FrontendUsesThirdPartyModelArtifacts()
{
    return profile().usesThirdPartyModelArtifacts;
}


bool apusrFsr4Dx12FrontendGetTelemetry(ffxContext* context, ApusrFsr4Dx12FrontendTelemetry* outTelemetry)
{
    FfxFsr4Dx12Context* typedContext = checkedContext(context);
    if (typedContext == nullptr || outTelemetry == nullptr) {
        return false;
    }
    *outTelemetry = typedContext->telemetry;
    return true;
}

bool apusrDx12QueueBridgeRegister(void* device, void* queue, std::uint32_t source)
{
    const auto sourceKind = static_cast<apusr::frontends::Dx12QueueSourceKind>(source);
    const apusr::Status status = apusr::frontends::registerDx12Queue(apusr::frontends::Dx12QueueRegistrationDesc{
        .device = apusr::NativeHandle{device},
        .queue = apusr::NativeHandle{queue},
        .source = sourceKind,
    });
    return status.succeeded();
}

bool apusrDx12QueueBridgeResolve(void* device, void** outQueue, std::uint32_t* outSource)
{
    if (outQueue == nullptr) {
        return false;
    }
    const apusr::frontends::Dx12ResolvedQueue resolved =
        apusr::frontends::resolveDx12Queue(apusr::NativeHandle{device});
    if (!resolved.available) {
        *outQueue = nullptr;
        if (outSource != nullptr) {
            *outSource = APUSR_DX12_QUEUE_SOURCE_EXPLICIT_NATIVE_CONFIG;
        }
        return false;
    }
    *outQueue = resolved.queue.value;
    if (outSource != nullptr) {
        *outSource = static_cast<std::uint32_t>(resolved.source);
    }
    return true;
}

bool apusrDx12QueueBridgeInstallDeviceQueueCapture(void* device)
{
    const apusr::Status status = apusr::frontends::installDx12DeviceQueueCapture(apusr::NativeHandle{device});
    return status.succeeded();
}

void apusrDx12QueueBridgeUninstallDeviceQueueCapture()
{
    apusr::frontends::uninstallDx12DeviceQueueCapture();
}

bool apusrDx12QueueBridgeInstallDiscovery(void* device)
{
    const apusr::Status status = apusr::frontends::installDx12QueueDiscovery(apusr::NativeHandle{device});
    return status.succeeded();
}

void apusrDx12QueueBridgeUninstallDiscovery()
{
    apusr::frontends::uninstallDx12QueueDiscovery();
}

void apusrDx12QueueBridgeClear()
{
    apusr::frontends::clearDx12QueueBridge();
}

bool apusrDx12QueueBridgeGetStats(ApusrDx12QueueBridgeStats* outStats)
{
    if (outStats == nullptr) {
        return false;
    }
    const apusr::frontends::Dx12QueueBridgeStats stats = apusr::frontends::dx12QueueBridgeStats();
    outStats->registrations = stats.registrations;
    outStats->replacements = stats.replacements;
    outStats->resolves = stats.resolves;
    outStats->misses = stats.misses;
    outStats->discoveryInstallAttempts = stats.discoveryInstallAttempts;
    outStats->discoveryHooksInstalled = stats.discoveryHooksInstalled;
    outStats->discoveryCallbacks = stats.discoveryCallbacks;
    outStats->discoveryUninstalls = stats.discoveryUninstalls;
    outStats->deviceCaptureInstallAttempts = stats.deviceCaptureInstallAttempts;
    outStats->deviceCaptureHooksInstalled = stats.deviceCaptureHooksInstalled;
    outStats->deviceCreateCommandQueueCallbacks = stats.deviceCreateCommandQueueCallbacks;
    outStats->deviceCaptureUninstalls = stats.deviceCaptureUninstalls;
    outStats->lastDevice = stats.lastDevice;
    outStats->lastQueue = stats.lastQueue;
    outStats->lastSource = static_cast<std::uint32_t>(stats.lastSource);
    return true;
}

} // extern "C"

namespace {

ffxReturnCode_t ffxCreateContextImpl(ffxContext* context, ffxCreateContextDescHeader* header, const ffxAllocationCallbacks* allocationCallbacks)
{
    if (context == nullptr || header == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (header->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
        const ffxReturnCode_t delegated = ffxPassthroughCreateContext(context, header, allocationCallbacks);
        APUSR_FFX_ROUTE_TRACE(std::string("ffxCreateContext.passthrough type=") + std::to_string(header->type) +
                          " ret=" + std::to_string(delegated));
        return delegated;
    }

    const auto* upscale = static_cast<const ffxCreateContextDescUpscale*>(static_cast<const void*>(header));
    if (upscale->maxRenderSize.width == 0 || upscale->maxRenderSize.height == 0 ||
        upscale->maxUpscaleSize.width == 0 || upscale->maxUpscaleSize.height == 0) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    std::uint32_t requestedApiVersion = kFsr4LegacyApiVersion;
    if (const ffxReturnCode_t versionStatus = parseUpscaleApiVersion(header, requestedApiVersion); versionStatus != FFX_API_RETURN_OK) {
        return versionStatus;
    }

    auto ownedContext = std::make_unique<FfxFsr4Dx12Context>();
    APUSR_FFX_ROUTE_TRACE("ffxCreateContext.upscale.enter");
    ownedContext->maxRenderSize = apusr::RenderSize{
        .width = upscale->maxRenderSize.width,
        .height = upscale->maxRenderSize.height,
    };
    ownedContext->maxActiveOutputSize = apusr::DisplaySize{
        .width = upscale->maxUpscaleSize.width,
        .height = upscale->maxUpscaleSize.height,
    };
    ownedContext->flags = upscale->flags;
    ownedContext->requestedApiVersion = requestedApiVersion;
    ownedContext->device = findDx12Device(header);
    {
        std::ostringstream detail;
        detail << "ffxCreateContext.upscale.desc"
               << " maxRender=" << ownedContext->maxRenderSize.width << 'x' << ownedContext->maxRenderSize.height
               << " maxUpscale=" << ownedContext->maxActiveOutputSize.width << 'x' << ownedContext->maxActiveOutputSize.height
               << " flags=0x" << std::hex << ownedContext->flags << std::dec
               << " apiVersion=" << ((ownedContext->requestedApiVersion >> 22u) & 0x3ffu)
               << '.' << ((ownedContext->requestedApiVersion >> 12u) & 0x3ffu)
               << '.' << (ownedContext->requestedApiVersion & 0xfffu)
               << " device=" << (ownedContext->device != nullptr ? "present" : "null");
        APUSR_FFX_ROUTE_TRACE(detail.str());
    }
    ownedContext->packagedRuntimeConfigStatus =
        apusr::frontends::loadFsr4Dx12PackagedRuntimeConfig(ownedContext->packagedRuntimeConfig);
    if (!ownedContext->packagedRuntimeConfigStatus.succeeded()) {
        return returnCode(ownedContext->packagedRuntimeConfigStatus);
    }
#if APUSR_REQUIRE_PACKAGED_NATIVE
    if (!ownedContext->packagedRuntimeConfig.available) {
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
#endif
    if (ownedContext->device != nullptr) {
        // Queue discovery is cold-path setup. The native NPU path still requires a
        // resolved direct queue before it can configure; this hook only feeds the
        // cache without adding work to ffxDispatch.
        (void)apusr::frontends::installDx12DeviceQueueCapture(apusr::NativeHandle{ownedContext->device});
        (void)apusr::frontends::installDx12QueueDiscovery(apusr::NativeHandle{ownedContext->device});
    }

#if APUSR_ENABLE_DX12_BACKEND
    if (ownedContext->device != nullptr) {
        const apusr::Status referenceRuntimeStatus = configureReferenceDx12RuntimeIfEnabled(*ownedContext);
        if (!referenceRuntimeStatus.succeeded()) {
            APUSR_FFX_ROUTE_TRACE(statusTraceString("reference.configure.createContext.failed", referenceRuntimeStatus));
            APUSR_TRACE_DIAGNOSTIC(statusTraceString(
                "Reference DX12 setup failed; context remains valid for provider/API queries and dispatch will fail unless a real route configures",
                referenceRuntimeStatus));
        }
    }
#endif

    const bool hasDevice = ownedContext->device != nullptr;
    const bool referenceConfigured = ownedContext->referenceRuntimeConfigured;
    *context = ownedContext.release();
    APUSR_FFX_ROUTE_TRACE(std::string("ffxCreateContext.ok device=") + (hasDevice ? "present" : "null") +
                      " referenceConfigured=" + (referenceConfigured ? "true" : "false"));
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxDestroyContextImpl(ffxContext* context, const ffxAllocationCallbacks* allocationCallbacks)
{
    FfxFsr4Dx12Context* typedContext = checkedContext(context);
    if (typedContext == nullptr) {
        if (context != nullptr && *context != nullptr) {
            return ffxPassthroughDestroyContext(context, allocationCallbacks);
        }
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    if (typedContext->device != nullptr) {
        apusr::frontends::uninstallDx12QueueDiscovery();
        apusr::frontends::uninstallDx12DeviceQueueCapture();
    }
    typedContext->magic = 0;
    delete typedContext;
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t ffxConfigureImpl(ffxContext* context, const ffxConfigureDescHeader* header)
{
    if (header == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    if (header->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG) {
        const auto* debug = static_cast<const ffxConfigureDescGlobalDebug*>(static_cast<const void*>(header));
        configureGlobalDebug(debug->fpMessage, debug->debugLevel);
        return FFX_API_RETURN_OK;
    }
    if (header->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1) {
        const auto* debug = static_cast<const ffxConfigureDescGlobalDebug1*>(static_cast<const void*>(header));
        configureGlobalDebug(debug->fpMessage, debug->debugLevel);
        return FFX_API_RETURN_OK;
    }

    FfxFsr4Dx12Context* typedContext = checkedContext(context);
    if (typedContext == nullptr) {
        return ffxPassthroughConfigure(context, header);
    }

    if (header->type == apusr::frontends::kConfigureDescTypeFsr4Dx12Runtime) {
        const auto* runtimeDesc = static_cast<const apusr::frontends::Fsr4Dx12RuntimeConfigureDesc*>(static_cast<const void*>(header));
        return returnCode(configurePipeline(*typedContext, *runtimeDesc));
    }

    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

ffxReturnCode_t ffxQueryImpl(ffxContext* context, ffxQueryDescHeader* header)
{
    if (header == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    switch (header->type) {
    case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: {
        auto* query = static_cast<ffxQueryDescGetVersions*>(static_cast<void*>(header));
        if (query->createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
            return ffxPassthroughQuery(context, header);
        }
        return queryProviderVersions(*query);
    }
    case FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION: {
        auto* query = static_cast<ffxQueryGetProviderVersion*>(static_cast<void*>(header));
        query->versionId = 0;
        query->versionName = nullptr;
        if (context == nullptr) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        if (checkedContext(context) == nullptr) {
            return ffxPassthroughQuery(context, header);
        }
        query->versionId = kFsr4CompatibleProviderId;
        query->versionName = kFsr4CompatibleProviderName;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE: {
        auto* query = static_cast<ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode*>(static_cast<void*>(header));
        return queryUpscaleRatio(*query);
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE: {
        auto* query = static_cast<ffxQueryDescUpscaleGetRenderResolutionFromQualityMode*>(static_cast<void*>(header));
        return queryRenderResolution(*query);
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT: {
        auto* query = static_cast<ffxQueryDescUpscaleGetJitterPhaseCount*>(static_cast<void*>(header));
        if (query->pOutPhaseCount == nullptr || query->renderWidth == 0 || query->displayWidth == 0) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        *query->pOutPhaseCount = apusr::upscaleJitterPhaseCount(
            apusr::RenderSize{.width = query->renderWidth, .height = 1},
            apusr::DisplaySize{.width = query->displayWidth, .height = 1});
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET: {
        auto* query = static_cast<ffxQueryDescUpscaleGetJitterOffset*>(static_cast<void*>(header));
        if (query->pOutX == nullptr || query->pOutY == nullptr || query->phaseCount <= 0) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        const apusr::Jitter jitter = apusr::upscaleJitterOffset(query->index, query->phaseCount);
        *query->pOutX = jitter.x;
        *query->pOutY = jitter.y;
        return FFX_API_RETURN_OK;
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE: {
        auto* query = static_cast<ffxQueryDescUpscaleGetGPUMemoryUsage*>(static_cast<void*>(header));
        return queryGpuMemoryUsage(query->gpuMemoryUsageUpscaler);
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2: {
        auto* query = static_cast<ffxQueryDescUpscaleGetGPUMemoryUsageV2*>(static_cast<void*>(header));
        return queryGpuMemoryUsage(query->gpuMemoryUsageUpscaler);
    }
    case FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS: {
        auto* query = static_cast<ffxQueryDescUpscaleGetResourceRequirements*>(static_cast<void*>(header));
        return queryResourceRequirements(*query);
    }
    default:
        if (context == nullptr || checkedContext(context) == nullptr) {
            return ffxPassthroughQuery(context, header);
        }
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
}

ffxReturnCode_t ffxDispatchImpl(ffxContext* context, const ffxDispatchDescHeader* header)
{
    if (header == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    FfxFsr4Dx12Context* typedContext = checkedContext(context);
    if (typedContext == nullptr) {
        if (context != nullptr && *context != nullptr) {
            return ffxPassthroughDispatch(context, header);
        }
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (header->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    }
    const auto* dispatch = static_cast<const ffxDispatchDescUpscale*>(static_cast<const void*>(header));
    typedContext->telemetry.totalDispatches += 1;
    const apusr::Status status = submitFsr4Dx12Ingress(*typedContext, *dispatch);
    if (status.succeeded()) {
        switch (typedContext->lastSuccessfulDispatchRoute) {
        case FfxFsr4Dx12DispatchRoute::NativeNpu:
            typedContext->telemetry.nativeApusrDispatches += 1;
            break;
        case FfxFsr4Dx12DispatchRoute::ReferenceDx12:
            typedContext->telemetry.referenceDx12Dispatches += 1;
            break;
        case FfxFsr4Dx12DispatchRoute::None:
            break;
        }
    } else {
        typedContext->telemetry.failedDispatches += 1;
    }
    return returnCode(status);
}


} // namespace

extern "C" {

ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* header, const ffxAllocationCallbacks* allocationCallbacks)
{
    return ffxCreateContextImpl(context, header, allocationCallbacks);
}

ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* allocationCallbacks)
{
    return ffxDestroyContextImpl(context, allocationCallbacks);
}

ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* header)
{
    return ffxConfigureImpl(context, header);
}

ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* header)
{
    return ffxQueryImpl(context, header);
}

ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* header)
{
    return ffxDispatchImpl(context, header);
}

} // extern "C"
