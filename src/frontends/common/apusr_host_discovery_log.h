// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

// Single cold-path host-discovery log for integration bring-up.
// Header-only so loader-facing frontends can emit diagnostics without a shared
// APUSR runtime dependency. No work is performed from DllMain.

#if defined(_WIN32)
#ifndef APUSR_HOST_DISCOVERY_LOG_COMPILED
#define APUSR_HOST_DISCOVERY_LOG_COMPILED 1
#endif
static_assert(APUSR_HOST_DISCOVERY_LOG_COMPILED == 1,
              "APUSR host-discovery logging must not be compiled out of Windows integration builds");
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>

namespace apusr::host_discovery {

inline bool stringEqualsNoCase(const wchar_t* a, const wchar_t* b)
{
    return a != nullptr && b != nullptr && _wcsicmp(a, b) == 0;
}

inline bool isDisabledValue(const wchar_t* value)
{
    return value != nullptr && (stringEqualsNoCase(value, L"0") ||
                                stringEqualsNoCase(value, L"false") ||
                                stringEqualsNoCase(value, L"off") ||
                                stringEqualsNoCase(value, L"no"));
}

inline bool isEnabledValue(const wchar_t* value)
{
    return value != nullptr && (stringEqualsNoCase(value, L"1") ||
                                stringEqualsNoCase(value, L"true") ||
                                stringEqualsNoCase(value, L"on") ||
                                stringEqualsNoCase(value, L"yes"));
}

inline std::string narrowBestEffort(const wchar_t* value)
{
    if (value == nullptr) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    // WideCharToMultiByte includes the terminating NUL in `required`. Allocate
    // room for it, then remove it from the C++ string after conversion.
    std::string out(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), required, nullptr, nullptr) <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(required - 1));
    return out;
}

inline bool directoryExists(const std::wstring& path)
{
    if (path.empty()) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

inline std::wstring parentDirectory(std::wstring path)
{
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    if (slash == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3); // Drive root.
    }
    if (slash == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, slash);
}

inline bool ensureDirectoryTree(const std::wstring& directory)
{
    if (directory.empty()) {
        return false;
    }
    if (directoryExists(directory)) {
        return true;
    }

    const std::wstring parent = parentDirectory(directory);
    if (!parent.empty() && parent != directory && !directoryExists(parent)) {
        if (!ensureDirectoryTree(parent)) {
            return false;
        }
    }

    if (CreateDirectoryW(directory.c_str(), nullptr) != FALSE) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS && directoryExists(directory);
}

inline std::wstring pathInDirectory(const wchar_t* directory)
{
    if (directory == nullptr || directory[0] == L'\0') {
        return {};
    }
    std::wstring dir(directory);
    (void)ensureDirectoryTree(dir);
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir.push_back(L'\\');
    }
    dir += L"apusr_host_discovery.jsonl";
    return dir;
}

inline std::wstring tempFallbackPath()
{
    wchar_t temp[MAX_PATH]{};
    const DWORD count = GetTempPathW(MAX_PATH, temp);
    if (count == 0 || count >= MAX_PATH) {
        return {};
    }
    std::wstring dir(temp);
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') {
        dir.push_back(L'\\');
    }
    dir += L"APUSR";
    return pathInDirectory(dir.c_str());
}

inline bool canOpenForAppend(const std::wstring& path, DWORD* error)
{
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    if (path.empty()) {
        if (error != nullptr) {
            *error = ERROR_PATH_NOT_FOUND;
        }
        return false;
    }

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = GetLastError();
        }
        return false;
    }
    CloseHandle(file);
    return true;
}

struct LogTarget {
    std::wstring requestedPath;
    std::wstring activePath;
    DWORD requestedOpenError = ERROR_SUCCESS;
    bool fallback = false;
};

inline LogTarget computeLogTarget()
{
    wchar_t explicitDir[32768]{};
    const DWORD dirCount = GetEnvironmentVariableW(
        L"APUSR_LOG_DIR", explicitDir, static_cast<DWORD>(std::size(explicitDir)));
    const bool hasExplicitDir = dirCount > 0 && dirCount < std::size(explicitDir);

    wchar_t explicitSetting[32768]{};
    const DWORD settingCount = GetEnvironmentVariableW(
        L"APUSR_HOST_DISCOVERY_LOG", explicitSetting, static_cast<DWORD>(std::size(explicitSetting)));

    LogTarget target{};
    if (settingCount > 0 && settingCount < std::size(explicitSetting)) {
        if (isDisabledValue(explicitSetting)) {
            return target;
        }
        if (isEnabledValue(explicitSetting)) {
            if (hasExplicitDir) {
                target.requestedPath = pathInDirectory(explicitDir);
            } else {
                target.requestedPath = pathInDirectory(L"C:\\NGXLogs\\APUSR");
            }
        } else {
            target.requestedPath = explicitSetting;
            const std::wstring parent = parentDirectory(target.requestedPath);
            if (!parent.empty()) {
                (void)ensureDirectoryTree(parent);
            }
        }
    } else {
        // APUSR_LOG_DIR selects a location only; it does not enable logging.
        // Product builds stay silent unless APUSR_HOST_DISCOVERY_LOG explicitly
        // opts in. This keeps integration diagnostics out of normal play.
        return target;
    }

    if (canOpenForAppend(target.requestedPath, &target.requestedOpenError)) {
        target.activePath = target.requestedPath;
        return target;
    }

    const std::wstring fallback = tempFallbackPath();
    DWORD fallbackError = ERROR_SUCCESS;
    if (canOpenForAppend(fallback, &fallbackError)) {
        target.activePath = fallback;
        target.fallback = true;
    }
    return target;
}

inline const LogTarget& logTarget()
{
    static const LogTarget cached = computeLogTarget();
    return cached;
}

inline std::wstring logPath()
{
    return logTarget().activePath;
}

inline bool enabled()
{
    return !logTarget().activePath.empty();
}

inline bool usingFallback()
{
    return logTarget().fallback;
}

inline DWORD requestedOpenError()
{
    return logTarget().requestedOpenError;
}

inline std::string activeLogPathUtf8()
{
    return narrowBestEffort(logTarget().activePath.c_str());
}

inline std::string requestedLogPathUtf8()
{
    return narrowBestEffort(logTarget().requestedPath.c_str());
}

inline std::string exePathUtf8()
{
    static const std::string cached = [] {
        wchar_t path[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        return (count > 0 && count < std::size(path)) ? narrowBestEffort(path) : std::string{};
    }();
    return cached;
}

inline std::string modulePathUtf8(HMODULE module)
{
    wchar_t path[32768]{};
    const DWORD count = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    return (count > 0 && count < std::size(path)) ? narrowBestEffort(path) : std::string{};
}

inline std::string jsonEscape(const char* value)
{
    if (value == nullptr) {
        return {};
    }
    std::string out;
    for (const unsigned char ch : std::string(value)) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[8]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                out += escaped;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

inline void appendLine(const std::string& line)
{
    const std::wstring path = logTarget().activePath;
    if (path.empty()) {
        return;
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    (void)WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

inline void event(const char* component, const char* name, const char* detail = nullptr)
{
    if (!enabled()) {
        return;
    }

    SYSTEMTIME st{};
    GetSystemTime(&st);

    char timeBuffer[64]{};
    std::snprintf(timeBuffer, sizeof(timeBuffer), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    char pidBuffer[32]{};
    std::snprintf(pidBuffer, sizeof(pidBuffer), "%lu", static_cast<unsigned long>(GetCurrentProcessId()));

    const std::string exe = exePathUtf8();

    std::string line;
    line.reserve(512);
    line += "{\"ts\":\"";
    line += timeBuffer;
    line += "\",\"pid\":";
    line += pidBuffer;
    line += ",\"exe\":\"";
    line += jsonEscape(exe.c_str());
    line += "\",\"component\":\"";
    line += jsonEscape(component != nullptr ? component : "unknown");
    line += "\",\"event\":\"";
    line += jsonEscape(name != nullptr ? name : "unknown");
    line += "\"";
    if (detail != nullptr && detail[0] != '\0') {
        line += ",\"detail\":\"";
        line += jsonEscape(detail);
        line += "\"";
    }
    line += "}\r\n";
    appendLine(line);
}

inline void eventf(const char* component, const char* name, const char* fmt, ...)
{
    if (!enabled()) {
        return;
    }

    char detail[768]{};
    if (fmt != nullptr) {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(detail, sizeof(detail), fmt, args);
        va_end(args);
    }
    event(component, name, detail);
}

inline void loggingReady(const char* component)
{
    if (!enabled()) {
        return;
    }
    const std::string active = activeLogPathUtf8();
    const std::string requested = requestedLogPathUtf8();
    eventf(component, "logging.ready",
           "compiled=1 active=%s requested=%s fallback=%u requested_open_error=%lu",
           active.c_str(), requested.c_str(), usingFallback() ? 1u : 0u,
           static_cast<unsigned long>(requestedOpenError()));
}

inline void moduleLoaded(const char* component, HMODULE module)
{
    const std::string modulePath = modulePathUtf8(module);
    eventf(component, "module.loaded", "module=%s", modulePath.c_str());
}

} // namespace apusr::host_discovery

#else
namespace apusr::host_discovery {
inline bool enabled() { return false; }
inline bool usingFallback() { return false; }
inline unsigned long requestedOpenError() { return 0; }
inline void loggingReady(const char*) {}
inline void event(const char*, const char*, const char* = nullptr) {}
inline void eventf(const char*, const char*, const char*, ...) {}
inline void moduleLoaded(const char*, void*) {}
} // namespace apusr::host_discovery
#endif
