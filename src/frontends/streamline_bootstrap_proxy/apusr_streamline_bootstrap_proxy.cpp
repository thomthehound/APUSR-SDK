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

#include "../common/apusr_host_discovery_log.h"
#include "../streamline_attach/apusr_streamline_attach_abi.h"

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <string>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

constexpr wchar_t kStockInterposerName[] = L"sl_interposer_apusr_stock.dll";
constexpr wchar_t kAttachModuleName[] = L"apusr_streamline_attach.dll";
constexpr std::uint64_t kSlSdkVersionMagic = 0xfedcull;
constexpr std::uint32_t kSlRenderApiVulkan = 2u;
constexpr int kSlResultFeatureMissing = 31;

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
static_assert(offsetof(SlPreferencesBootstrapLoose, renderAPI) == 136, "Streamline Preferences renderAPI ABI drift");

using SlInitFn = int(__cdecl*)(const void* preferences, std::uint64_t sdkVersion);

[[nodiscard]] std::wstring selfDirectory()
{
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(
        reinterpret_cast<HMODULE>(&__ImageBase), path, static_cast<DWORD>(std::size(path)));
    if (length == 0u || length >= std::size(path)) {
        return {};
    }
    std::wstring value(path, path + length);
    const std::size_t slash = value.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    value.resize(slash + 1u);
    return value;
}

[[nodiscard]] HMODULE loadSibling(const wchar_t* name)
{
    if (name == nullptr || *name == L'\0') {
        return nullptr;
    }
    if (HMODULE existing = GetModuleHandleW(name)) {
        return existing;
    }
    const std::wstring directory = selfDirectory();
    if (directory.empty()) {
        return nullptr;
    }
    const std::wstring path = directory + name;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

[[nodiscard]] std::uint32_t bootstrapRenderApi(const void* preferences, const std::uint64_t sdkVersion) noexcept
{
    if ((sdkVersion & 0xffffull) != kSlSdkVersionMagic || preferences == nullptr) {
        return 0xffffffffu;
    }
    const auto& prefs = *static_cast<const SlPreferencesBootstrapLoose*>(preferences);
    return prefs.renderAPI <= kSlRenderApiVulkan ? prefs.renderAPI : 0xffffffffu;
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl slInit(const void* preferences, const std::uint64_t sdkVersion)
{
    apusr::host_discovery::loggingReady("streamline_bootstrap");

    HMODULE stock = loadSibling(kStockInterposerName);
    if (stock == nullptr) {
        apusr::host_discovery::event(
            "streamline", "bootstrap.stock_missing",
            "expected=sl_interposer_apusr_stock.dll action=fail_without_semantic_replacement");
        return kSlResultFeatureMissing;
    }

    const auto stockInit = reinterpret_cast<SlInitFn>(GetProcAddress(stock, "slInit"));
    if (stockInit == nullptr) {
        apusr::host_discovery::event(
            "streamline", "bootstrap.stock_invalid",
            "missing_export=slInit action=fail_without_semantic_replacement");
        return kSlResultFeatureMissing;
    }

    const std::uint32_t renderApi = bootstrapRenderApi(preferences, sdkVersion);
    HMODULE attach = loadSibling(kAttachModuleName);
    bool attached = false;
    if (attach != nullptr) {
        const auto hint = reinterpret_cast<ApusrStreamlineAttachHintRenderApiFn>(
            GetProcAddress(attach, "apusrStreamlineAttachHintRenderApi"));
        const auto attachModule = reinterpret_cast<ApusrAttachToStreamlineModuleFn>(
            GetProcAddress(attach, "apusrAttachToStreamlineModule"));
        if (hint != nullptr && renderApi != 0xffffffffu) {
            hint(renderApi);
        }
        if (attachModule != nullptr) {
            attached = attachModule(stock);
        }
    }

    apusr::host_discovery::eventf(
        "streamline", "bootstrap.slInit",
        "stock=%p attach=%p attached=%u render_api=%u stock_semantics=1 loader_shim_only=1",
        stock, attach, attached ? 1u : 0u, renderApi);

    // Detours patches the stock slInit entry point when attachment succeeds.
    // Re-resolve after the transaction so the call enters the APUSR-owned
    // filtering wrapper, whose trampoline still reaches this exact stock PE.
    const auto entry = reinterpret_cast<SlInitFn>(GetProcAddress(stock, "slInit"));
    return entry != nullptr ? entry(preferences, sdkVersion) : stockInit(preferences, sdkVersion);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

#endif // _WIN32
