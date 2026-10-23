// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_diagnostics.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace apusr {
namespace {

#if defined(_WIN32) && defined(APUSR_ENABLE_DX12_TRACE) && APUSR_ENABLE_DX12_TRACE
[[nodiscard]] std::string envString(const char* name)
{
    DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::string value(needed, '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return value;
}

[[nodiscard]] std::string defaultTraceDirectory()
{
    const std::string explicitDir = envString("APUSR_LOG_DIR");
    return explicitDir.empty() ? "C:/NGXLogs/APUSR" : explicitDir;
}

void ensureParentDirectory(const std::string& path) noexcept
{
    try {
        const std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(fsPath.parent_path(), ec);
        }
    } catch (...) {
    }
}
#endif

} // namespace

void traceDiagnosticLine(const std::string& line)
{
#if defined(_WIN32) && defined(APUSR_ENABLE_DX12_TRACE) && APUSR_ENABLE_DX12_TRACE
    static std::mutex mutex;
    static std::ofstream file = []() {
        std::string path = envString("APUSR_DX12_TRACE_FILE");
        if (path.empty()) {
            path = defaultTraceDirectory() + "/apusr_dx12_trace.log";
        }
        ensureParentDirectory(path);
        return std::ofstream(path, std::ios::out | std::ios::trunc);
    }();

    std::lock_guard<std::mutex> lock(mutex);
    if (file) {
        file << line << '\n';
        file.flush();
    }
#else
    (void)line;
#endif
}

} // namespace apusr
