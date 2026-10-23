// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_profiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace apusr::profile {
namespace {

constexpr std::uint64_t kTraceMagic = 0x3152465041535552ull; // "RUSAPFR1" little-endian marker.
constexpr std::uint32_t kTraceVersion = 1u;
constexpr std::size_t kHeaderBytes = 4096u;
constexpr std::size_t kRecordBytes = 256u;
constexpr std::uint32_t kMinRingMiB = 4u;
constexpr std::uint32_t kMaxRingMiB = 2048u;

#if APUSR_ENABLE_PROFILER

std::atomic<std::uint64_t> gBurstTriggerSequence{0u};

enum class RecordType : std::uint32_t {
    Stream = 1,
    CpuInterval = 2,
    Frame = 3,
    GpuInterval = 4,
    Calibration = 5,
    Counter = 6,
    Instant = 7,
    Error = 8,
    TextContinuation = 9,
    QueueEvent = 10,
    Decision = 11,
    StateTransition = 12,
    ResourceEvent = 13,
    FlowEvent = 14,
    ThreadInfo = 15,
    WaitInterval = 16,
    Metadata = 17,
    GpuWorkloadInfo = 18,
};

struct alignas(64) TraceRecord {
    std::uint64_t commitSequence;
    std::uint64_t cpuTick;
    std::uint64_t streamId;
    std::uint64_t frameId;
    std::uint64_t correlationId;
    std::array<std::uint64_t, 8> values;
    std::array<std::uint64_t, 4> extraValues;
    std::uint32_t type;
    std::uint32_t backend;
    std::uint32_t component;
    std::uint32_t threadId;
    std::uint32_t flags;
    std::uint32_t aux0;
    std::uint32_t aux1;
    std::uint32_t aux2;
    std::array<char, 48> name;
    std::array<char, 40> detail;
};
static_assert(sizeof(TraceRecord) == kRecordBytes);

struct alignas(64) TraceHeader {
    std::uint64_t magic = kTraceMagic;
    std::uint32_t version = kTraceVersion;
    std::uint32_t headerBytes = static_cast<std::uint32_t>(kHeaderBytes);
    std::uint32_t recordBytes = static_cast<std::uint32_t>(kRecordBytes);
    std::uint32_t mode = 0;
    std::uint64_t capacity = 0;
    std::atomic<std::uint64_t> writeSequence{0};
    std::atomic<std::uint64_t> streamSequence{1};
    std::uint64_t cpuFrequency = 0;
    std::uint64_t startCpuTick = 0;
    std::uint64_t moduleNonce = 0;
    std::uint32_t processId = 0;
    std::uint32_t ringMiB = 0;
    std::uint32_t sampleIntervalMs = 0;
    std::uint32_t burstFrames = 0;
    std::array<char, 128> moduleName{};
    std::array<char, 128> sessionTag{};
    std::array<std::byte, kHeaderBytes - 352> reserved{};
};
static_assert(sizeof(TraceHeader) == kHeaderBytes);

[[nodiscard]] bool asciiIequals(const std::string_view lhs, const std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0u; i < lhs.size(); ++i) {
        const auto fold = [](const char c) noexcept -> char {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (fold(lhs[i]) != fold(rhs[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parseTruthValue(const std::string_view value, const bool fallback) noexcept
{
    if (value.empty()) {
        return fallback;
    }
    if (value == "0" || asciiIequals(value, "false") || asciiIequals(value, "no") || asciiIequals(value, "off")) {
        return false;
    }
    if (value == "1" || asciiIequals(value, "true") || asciiIequals(value, "yes") || asciiIequals(value, "on")) {
        return true;
    }
    return fallback;
}

[[nodiscard]] bool envTruth(const char* name, const bool fallback = false) noexcept
{
    if (name == nullptr) {
        return fallback;
    }
#if defined(_WIN32)
    char buffer[32]{};
    const DWORD size = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (size == 0u || size >= sizeof(buffer)) {
        return fallback;
    }
    return parseTruthValue(std::string_view{buffer, size}, fallback);
#else
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return parseTruthValue(value, fallback);
#endif
}

[[nodiscard]] std::string envString(const char* name)
{
#if defined(_WIN32)
    DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
    if (needed == 0u) {
        return {};
    }
    std::string value(needed, '\0');
    const DWORD written = GetEnvironmentVariableA(name, value.data(), needed);
    if (written == 0u || written >= needed) {
        return {};
    }
    value.resize(written);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

[[nodiscard]] std::uint32_t envU32(const char* name, const std::uint32_t fallback, const std::uint32_t minValue, const std::uint32_t maxValue) noexcept
{
    const std::string text = envString(name);
    if (text.empty()) {
        return fallback;
    }
    std::uint32_t value = fallback;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return fallback;
    }
    return std::clamp(value, minValue, maxValue);
}

[[nodiscard]] Mode parseMode() noexcept
{
    const std::string value = envString("APUSR_PROFILE");
    if (value.empty() || value == "0" || asciiIequals(value, "off") || asciiIequals(value, "false") || asciiIequals(value, "no")) {
        return Mode::Off;
    }
    if (value == "2" || asciiIequals(value, "full")) {
        return Mode::Full;
    }
    if (value == "3" || asciiIequals(value, "errors") || asciiIequals(value, "error")) {
        return Mode::Errors;
    }
    if (value == "1" || asciiIequals(value, "sampled") || asciiIequals(value, "on") ||
        asciiIequals(value, "true") || asciiIequals(value, "yes")) {
        return Mode::Sampled;
    }
    // Keep historical behavior for unknown non-empty values: profiling turns
    // on in sampled mode rather than silently failing to capture anything.
    return Mode::Sampled;
}

[[nodiscard]] Settings loadSettings() noexcept
{
    Settings result{};
    result.mode = parseMode();
    if (result.mode == Mode::Off) {
        return result;
    }
    const bool errorsOnly = result.mode == Mode::Errors;
    result.cpu = errorsOnly ? false : envTruth("APUSR_PROFILE_CPU", true);
    result.gpu = errorsOnly ? false : envTruth("APUSR_PROFILE_GPU", true);
    result.errors = envTruth("APUSR_PROFILE_ERRORS", true);
    result.sampleIntervalMs = envU32("APUSR_PROFILE_INTERVAL_MS", 1000u, 50u, 600000u);
    result.burstFrames = envU32("APUSR_PROFILE_BURST", 3u, 1u, 120u);
    result.ringMiB = envU32("APUSR_PROFILE_RING_MIB", errorsOnly ? 4u : 32u, kMinRingMiB, kMaxRingMiB);
    return result;
}

[[nodiscard]] std::uint32_t currentProcessId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

[[nodiscard]] std::uint32_t currentThreadId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<std::uint32_t>(syscall(SYS_gettid));
#else
    return 0u;
#endif
}

[[nodiscard]] std::uint64_t nativeCpuFrequency() noexcept
{
#if defined(_WIN32)
    LARGE_INTEGER value{};
    return QueryPerformanceFrequency(&value) ? static_cast<std::uint64_t>(value.QuadPart) : 0u;
#else
    return 1000000000ull;
#endif
}

[[nodiscard]] std::uint64_t nativeCpuTick() noexcept
{
#if defined(_WIN32)
    LARGE_INTEGER value{};
    return QueryPerformanceCounter(&value) ? static_cast<std::uint64_t>(value.QuadPart) : 0u;
#else
    timespec value{};
#if defined(CLOCK_MONOTONIC_RAW)
    constexpr clockid_t clockId = CLOCK_MONOTONIC_RAW;
#else
    constexpr clockid_t clockId = CLOCK_MONOTONIC;
#endif
    if (clock_gettime(clockId, &value) != 0) {
        return 0u;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(value.tv_nsec);
#endif
}

void copyText(char* destination, const std::size_t capacity, const char* source) noexcept
{
    if (destination == nullptr || capacity == 0u) {
        return;
    }
    destination[0] = '\0';
    if (source == nullptr) {
        return;
    }
    std::size_t index = 0u;
    while (index + 1u < capacity && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

void copyText(char* destination, const std::size_t capacity, const std::string_view source) noexcept
{
    if (destination == nullptr || capacity == 0u) {
        return;
    }
    const std::size_t count = (std::min)(capacity - 1u, source.size());
    if (count != 0u) {
        std::memcpy(destination, source.data(), count);
    }
    destination[count] = '\0';
}

[[nodiscard]] std::string sanitizeFileName(std::string value)
{
    if (value.empty()) {
        return "module";
    }
    for (char& c : value) {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!valid) {
            c = '_';
        }
    }
    return value;
}

[[nodiscard]] std::string moduleName()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<LPCSTR>(&moduleName);
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            address,
            &module) || module == nullptr) {
        return "apusr";
    }
    std::array<char, 32768> path{};
    const DWORD size = GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0u || size >= path.size()) {
        return "apusr";
    }
    return std::filesystem::path(std::string(path.data(), size)).filename().string();
#else
    return "apusr";
#endif
}

[[nodiscard]] std::filesystem::path defaultDirectory()
{
    const std::string explicitProfileDir = envString("APUSR_PROFILE_DIR");
    if (!explicitProfileDir.empty()) {
        return explicitProfileDir;
    }
    const std::string logDir = envString("APUSR_LOG_DIR");
    if (!logDir.empty()) {
        return std::filesystem::path(logDir) / "profile";
    }
#if defined(_WIN32)
    return "C:/NGXLogs/APUSR/profile";
#else
    return "/tmp/APUSR/profile";
#endif
}

[[nodiscard]] std::uint64_t fnv1a(const std::string_view value, std::uint64_t seed = 1469598103934665603ull) noexcept
{
    std::uint64_t hash = seed;
    for (const unsigned char c : value) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

class MappedTrace final {
public:
    MappedTrace() = default;
    ~MappedTrace() noexcept { close(); }

    MappedTrace(const MappedTrace&) = delete;
    MappedTrace& operator=(const MappedTrace&) = delete;

    [[nodiscard]] bool open(const Settings& cfg) noexcept
    {
        close();
        try {
            const std::filesystem::path dir = defaultDirectory();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);

            const std::string module = sanitizeFileName(moduleName());
            const std::uint32_t pid = currentProcessId();
            const std::uint64_t tick = nativeCpuTick();
            const std::string fileName = "apusr_profile_" + std::to_string(pid) + "_" + module + "_" + std::to_string(tick) + ".aprof";
            path_ = dir / fileName;

            const std::uint64_t bytes = static_cast<std::uint64_t>(cfg.ringMiB) * 1024ull * 1024ull;
            if (bytes <= kHeaderBytes + kRecordBytes) {
                return false;
            }
            size_ = static_cast<std::size_t>(bytes);

#if defined(_WIN32)
            file_ = CreateFileW(
                path_.wstring().c_str(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (file_ == INVALID_HANDLE_VALUE) {
                file_ = nullptr;
                return false;
            }
            LARGE_INTEGER end{};
            end.QuadPart = static_cast<LONGLONG>(size_);
            if (!SetFilePointerEx(file_, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) {
                close();
                return false;
            }
            mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0u, 0u, nullptr);
            if (mapping_ == nullptr) {
                close();
                return false;
            }
            view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0u, 0u, size_);
            if (view_ == nullptr) {
                close();
                return false;
            }
#else
            fd_ = ::open(path_.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
            if (fd_ < 0) {
                return false;
            }
            if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
                close();
                return false;
            }
            view_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (view_ == MAP_FAILED) {
                view_ = nullptr;
                close();
                return false;
            }
#endif

            std::memset(view_, 0, kHeaderBytes);
            header_ = new (view_) TraceHeader{};
            header_->mode = static_cast<std::uint32_t>(cfg.mode);
            header_->capacity = (size_ - kHeaderBytes) / kRecordBytes;
            header_->cpuFrequency = nativeCpuFrequency();
            header_->startCpuTick = tick;
            header_->processId = pid;
            header_->ringMiB = cfg.ringMiB;
            header_->sampleIntervalMs = cfg.sampleIntervalMs;
            header_->burstFrames = cfg.burstFrames;
            const std::uint64_t nonceSeed = fnv1a(module);
            header_->moduleNonce = hashCombine(nonceSeed, hashCombine(pid, tick));
            copyText(header_->moduleName.data(), header_->moduleName.size(), module.c_str());
            const std::string session = envString("APUSR_PROFILE_SESSION");
            copyText(header_->sessionTag.data(), header_->sessionTag.size(), session.c_str());
            records_ = reinterpret_cast<TraceRecord*>(static_cast<std::byte*>(view_) + kHeaderBytes);
            return header_->capacity != 0u;
        } catch (...) {
            close();
            return false;
        }
    }

    void close() noexcept
    {
        header_ = nullptr;
        records_ = nullptr;
#if defined(_WIN32)
        if (view_ != nullptr) {
            FlushViewOfFile(view_, 0u);
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
        if (file_ != nullptr) {
            CloseHandle(file_);
            file_ = nullptr;
        }
#else
        if (view_ != nullptr) {
            msync(view_, size_, MS_ASYNC);
            munmap(view_, size_);
            view_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
#endif
        size_ = 0u;
    }

    [[nodiscard]] bool valid() const noexcept { return header_ != nullptr && records_ != nullptr; }
    [[nodiscard]] TraceHeader* header() noexcept { return header_; }

    void flushErrorPath() noexcept
    {
        if (!valid() || view_ == nullptr) {
            return;
        }
        // Error records are rare and are the records we most need after an
        // abnormal process exit.  Ask the OS to start writing dirty mapped
        // pages without turning normal profiling into synchronous file I/O.
        std::atomic_thread_fence(std::memory_order_seq_cst);
#if defined(_WIN32)
        (void)FlushViewOfFile(view_, 0u);
#else
        (void)msync(view_, size_, MS_ASYNC);
#endif
    }

    void write(TraceRecord& record) noexcept
    {
        if (!valid()) {
            return;
        }
        const std::uint64_t sequence = header_->writeSequence.fetch_add(1u, std::memory_order_relaxed);
        TraceRecord* slotStorage = &records_[sequence % header_->capacity];
        TraceRecord& slot = *::new (static_cast<void*>(slotStorage)) TraceRecord;
        std::atomic_ref<std::uint64_t>(slot.commitSequence).store(0u, std::memory_order_relaxed);
        record.commitSequence = 0u;
        std::memcpy(
            reinterpret_cast<std::byte*>(&slot) + sizeof(slot.commitSequence),
            reinterpret_cast<const std::byte*>(&record) + sizeof(record.commitSequence),
            sizeof(TraceRecord) - sizeof(slot.commitSequence));
        std::atomic_thread_fence(std::memory_order_release);
        std::atomic_ref<std::uint64_t>(slot.commitSequence).store(sequence + 1u, std::memory_order_release);
    }

private:
    std::filesystem::path path_;
    void* view_ = nullptr;
    std::size_t size_ = 0u;
    TraceHeader* header_ = nullptr;
    TraceRecord* records_ = nullptr;
#if defined(_WIN32)
    HANDLE file_ = nullptr;
    HANDLE mapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

class Runtime final {
public:
    explicit Runtime(const bool initialize = true) noexcept
    {
        if (!initialize) {
            return;
        }
        const Settings& cfg = settings();
        if (cfg.mode != Mode::Off) {
            active_ = trace_.open(cfg);
        }
    }

    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] StreamHandle addStream(const StreamDesc& desc) noexcept
    {
        if (!active_) {
            return {};
        }
        TraceHeader* header = trace_.header();
        const std::uint64_t localId = header->streamSequence.fetch_add(1u, std::memory_order_relaxed);
        const StreamHandle stream{
            .id = hashCombine(header->moduleNonce, localId),
            .backend = desc.backend,
            .component = desc.component,
        };
        TraceRecord record = baseRecord(RecordType::Stream, stream, 0u, 0u, desc.name);
        record.values[0] = desc.nativeDevice;
        record.values[1] = desc.nativeQueue;
        record.values[2] = (static_cast<std::uint64_t>(desc.apiVersion) << 32u) | desc.driverVersion;
        record.values[3] = (static_cast<std::uint64_t>(desc.vendorId) << 32u) | desc.deviceId;
        record.values[4] = desc.capabilities;
        record.aux0 = desc.queueFamily;
        record.flags = desc.flags;
        trace_.write(record);
        return stream;
    }

    void write(TraceRecord& record) noexcept { trace_.write(record); }

    void flushAfterError() noexcept
    {
        // Repeated failures can happen once per frame.  Preserve the first
        // failure immediately, then rate-limit flush requests so a broken game
        // cannot turn the profiler itself into a new performance problem.
        const std::uint64_t now = nativeCpuTick();
        const std::uint64_t frequency = nativeCpuFrequency();
        const std::uint64_t interval = frequency != 0u ? frequency : 1u;
        std::uint64_t expected = nextErrorFlushTick_.load(std::memory_order_relaxed);
        while (expected == 0u || now >= expected) {
            const std::uint64_t next = now + interval;
            if (nextErrorFlushTick_.compare_exchange_weak(
                    expected, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
                trace_.flushErrorPath();
                return;
            }
        }
    }

    [[nodiscard]] TraceRecord baseRecord(
        const RecordType type,
        const StreamHandle stream,
        const std::uint64_t frameId,
        const std::uint64_t correlationId,
        const char* name) noexcept
    {
        TraceRecord record{};
        record.cpuTick = nativeCpuTick();
        record.streamId = stream.id;
        record.frameId = frameId;
        record.correlationId = correlationId;
        record.type = static_cast<std::uint32_t>(type);
        record.backend = static_cast<std::uint32_t>(stream.backend);
        record.component = static_cast<std::uint32_t>(stream.component);
        record.threadId = currentThreadId();
        copyText(record.name.data(), record.name.size(), name);
        return record;
    }

private:
    MappedTrace trace_;
    std::atomic<std::uint64_t> nextErrorFlushTick_{0u};
    bool active_ = false;
};

[[nodiscard]] Runtime& runtime() noexcept
{
    // Intentional process-lifetime allocation. APUSR frontends are DLLs, and
    // avoiding destructor-time synchronization/file teardown under the Windows
    // loader lock is more important than reclaiming a few dozen bytes at exit.
    static Runtime* instance = new (std::nothrow) Runtime{};
    static Runtime fallback{false};
    return instance != nullptr ? *instance : fallback;
}

void writeContinuation(
    Runtime& rt,
    const StreamHandle stream,
    const std::uint64_t frameId,
    const std::uint64_t correlationId,
    std::string_view text) noexcept
{
    constexpr std::size_t chunkBytes = 86u;
    std::size_t offset = 0u;
    std::uint32_t chunk = 0u;
    while (offset < text.size()) {
        TraceRecord record = rt.baseRecord(RecordType::TextContinuation, stream, frameId, correlationId, "text");
        const std::size_t count = (std::min)(chunkBytes, text.size() - offset);
        const std::size_t first = (std::min)(record.name.size() - 1u, count);
        copyText(record.name.data(), record.name.size(), text.substr(offset, first));
        if (count > first) {
            copyText(record.detail.data(), record.detail.size(), text.substr(offset + first, count - first));
        }
        record.aux0 = chunk++;
        rt.write(record);
        offset += count;
    }
}

#endif // APUSR_ENABLE_PROFILER

} // namespace

const Settings& settings() noexcept
{
#if APUSR_ENABLE_PROFILER
    static const Settings value = loadSettings();
    return value;
#else
    static const Settings value{};
    return value;
#endif
}

bool enabled() noexcept
{
#if APUSR_ENABLE_PROFILER
    return settings().mode != Mode::Off;
#else
    return false;
#endif
}

bool cpuEnabled() noexcept
{
    return enabled() && settings().cpu;
}

bool gpuEnabled() noexcept
{
    return enabled() && settings().gpu;
}

std::uint64_t cpuTick() noexcept
{
#if APUSR_ENABLE_PROFILER
    return nativeCpuTick();
#else
    return 0u;
#endif
}

std::uint64_t cpuFrequency() noexcept
{
#if APUSR_ENABLE_PROFILER
    static const std::uint64_t value = nativeCpuFrequency();
    return value;
#else
    return 0u;
#endif
}

std::uint64_t cpuTicksToNs(const std::uint64_t ticks) noexcept
{
    const std::uint64_t frequency = cpuFrequency();
    if (frequency == 0u || ticks == 0u) {
        return 0u;
    }
    const long double ns = static_cast<long double>(ticks) * 1000000000.0L / static_cast<long double>(frequency);
    if (ns >= static_cast<long double>((std::numeric_limits<std::uint64_t>::max)())) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return static_cast<std::uint64_t>(ns);
}

std::uint64_t durationNs(const std::uint64_t beginTick, const std::uint64_t endTick) noexcept
{
    return endTick >= beginTick ? cpuTicksToNs(endTick - beginTick) : 0u;
}

SampleDecision SampleGate::decide(
    const std::uint64_t configurationHash,
    const std::uint64_t frameId,
    const bool force) noexcept
{
    const bool changed = !haveConfiguration_ || configurationHash != lastConfigurationHash_;
    if (changed) {
        haveConfiguration_ = true;
        lastConfigurationHash_ = configurationHash;
    }
    return decideStable(frameId, changed, force);
}

SampleDecision SampleGate::decideStable(
    const std::uint64_t frameId,
    const bool configurationChanged,
    const bool force) noexcept
{
    (void)frameId;
    SampleDecision decision{};
    if (!enabled()) {
        return decision;
    }

    const Settings& cfg = settings();
    if (cfg.mode == Mode::Errors) {
        return decision;
    }
    const std::uint64_t now = cpuTick();
    bool externallyTriggered = false;
#if APUSR_ENABLE_PROFILER
    const std::uint64_t triggerSequence = gBurstTriggerSequence.load(std::memory_order_relaxed);
    externallyTriggered = triggerSequence != lastTriggerSequence_;
    lastTriggerSequence_ = triggerSequence;
#endif

    if (cfg.mode == Mode::Full) {
        decision.capture = true;
        decision.configurationChanged = configurationChanged;
        decision.sampleSequence = ++sampleSequence_;
        decision.burstSequence = ++burstSequence_;
        decision.burstFrame = 0u;
        return decision;
    }

    // A genuine configuration/backend/anomaly transition starts a fresh complete
    // burst. Callers with multiple stable alternating logical contexts should
    // use decideStable() and perform configuration-change detection per context.
    if (force || configurationChanged || externallyTriggered) {
        burstRemaining_ = cfg.burstFrames;
        ++burstSequence_;
    } else if (burstRemaining_ == 0u && now >= nextSampleTick_) {
        burstRemaining_ = cfg.burstFrames;
        ++burstSequence_;
    }
    if (burstRemaining_ == 0u) {
        return decision;
    }

    decision.capture = true;
    decision.configurationChanged = configurationChanged;
    decision.sampleSequence = ++sampleSequence_;
    decision.burstSequence = burstSequence_;
    decision.burstFrame = cfg.burstFrames - burstRemaining_;
    --burstRemaining_;
    if (burstRemaining_ == 0u) {
        const std::uint64_t intervalTicks = cpuFrequency() != 0u
            ? (cpuFrequency() * static_cast<std::uint64_t>(cfg.sampleIntervalMs)) / 1000ull
            : 0u;
        nextSampleTick_ = now + intervalTicks;
    }
    return decision;
}

void SampleGate::reset() noexcept
{
    *this = SampleGate{};
}

StreamHandle registerStream(const StreamDesc& desc) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled()) {
        return {};
    }
    return runtime().addStream(desc);
#else
    (void)desc;
    return {};
#endif
}

void recordCpuInterval(const CpuInterval& interval) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!cpuEnabled() || !interval.stream || interval.endCpuTick < interval.startCpuTick) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(RecordType::CpuInterval, interval.stream, interval.frameId, interval.sampleSequence, interval.name);
    record.values[0] = interval.startCpuTick;
    record.values[1] = interval.endCpuTick;
    record.values[2] = durationNs(interval.startCpuTick, interval.endCpuTick);
    record.flags = interval.flags;
    rt.write(record);
#else
    (void)interval;
#endif
}

void recordFrameSample(const FrameSample& sample) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || (!settings().cpu && !settings().gpu) || !sample.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(RecordType::Frame, sample.stream, sample.frameId, sample.sampleSequence, "frame");
    record.values[0] = sample.configurationHash;
    record.values[1] = sample.burstSequence;
    record.values[2] = (static_cast<std::uint64_t>(sample.renderWidth) << 32u) | sample.renderHeight;
    record.values[3] = (static_cast<std::uint64_t>(sample.displayWidth) << 32u) | sample.displayHeight;
    record.values[4] = sample.gpuDurationNs;
    record.values[5] = sample.cpuTotalNs;
    record.values[6] = sample.cpuRecordNs;
    record.values[7] = sample.totalWorkgroups;
    record.extraValues[0] = sample.gpuBeginTick;
    record.extraValues[1] = sample.gpuEndTick;
    record.extraValues[2] = sample.gpuFrequency;
    record.aux0 = sample.burstFrame;
    record.aux1 = static_cast<std::uint32_t>(sample.nativeQueue);
    record.aux2 = static_cast<std::uint32_t>(sample.nativeQueue >> 32u);
    record.flags = sample.flags;
    rt.write(record);
#else
    (void)sample;
#endif
}

void recordGpuInterval(const GpuInterval& interval) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!gpuEnabled() || !interval.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(RecordType::GpuInterval, interval.stream, interval.frameId, interval.sampleSequence, interval.name);
    record.values[0] = interval.durationNs;
    record.values[1] = interval.setupNs;
    record.values[2] = interval.executionNs;
    record.values[3] = interval.readBytes;
    record.values[4] = interval.writeBytes;
    record.values[5] = (static_cast<std::uint64_t>(interval.groupsX) << 32u) | interval.groupsY;
    record.values[6] = (static_cast<std::uint64_t>(interval.groupsZ) << 32u) | interval.profileLevel;
    record.values[7] = (static_cast<std::uint64_t>(interval.barrierCount) << 32u) | interval.descriptorCount;
    record.extraValues[0] = interval.gpuBeginTick;
    record.extraValues[1] = interval.gpuExecutionBeginTick;
    record.extraValues[2] = interval.gpuEndTick;
    record.extraValues[3] = interval.gpuFrequency;
    record.aux1 = static_cast<std::uint32_t>(interval.nativeQueue);
    record.aux2 = static_cast<std::uint32_t>(interval.nativeQueue >> 32u);
    record.flags = interval.flags;
    rt.write(record);
#else
    (void)interval;
#endif
}

void recordCalibration(const ClockCalibration& calibration) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!gpuEnabled() || !calibration.stream || calibration.cpuTick == 0u || calibration.gpuFrequency == 0u) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(
        RecordType::Calibration, calibration.stream, calibration.frameId, calibration.sampleSequence, "clock_calibration");
    record.values[0] = calibration.cpuTick;
    record.values[1] = calibration.gpuTick;
    record.values[2] = calibration.gpuFrequency;
    record.values[3] = cpuFrequency();
    record.values[4] = calibration.maxDeviationNs;
    record.aux0 = calibration.gpuValidBits;
    record.aux1 = static_cast<std::uint32_t>(calibration.nativeQueue);
    record.aux2 = static_cast<std::uint32_t>(calibration.nativeQueue >> 32u);
    record.flags = calibration.flags;
    rt.write(record);
#else
    (void)calibration;
#endif
}

void recordCounter(const Counter& counter) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || (!settings().cpu && !settings().gpu) || !counter.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(RecordType::Counter, counter.stream, counter.frameId, 0u, counter.name);
    record.values[0] = counter.value;
    record.values[1] = counter.value2;
    record.flags = counter.flags;
    rt.write(record);
#else
    (void)counter;
#endif
}

void recordQueueEvent(const QueueEvent& event) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || settings().mode == Mode::Errors || (!settings().cpu && !settings().gpu) || !event.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(
        RecordType::QueueEvent, event.stream, event.frameId, event.sampleSequence, event.name);
    record.values[0] = event.nativeQueue;
    record.values[1] = event.syncObject;
    record.values[2] = event.syncValue;
    record.values[3] = event.relatedQueue;
    record.values[4] = event.submitSequence;
    record.aux0 = static_cast<std::uint32_t>(event.operation);
    record.aux1 = event.stageMask;
    record.aux2 = event.objectCount;
    record.flags = event.flags;
    rt.write(record);
#else
    (void)event;
#endif
}

void requestBurst() noexcept
{
#if APUSR_ENABLE_PROFILER
    if (enabled() && settings().mode != Mode::Errors) {
        gBurstTriggerSequence.fetch_add(1u, std::memory_order_relaxed);
    }
#endif
}

void recordDecision(const Decision& decision) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || settings().mode == Mode::Errors || !decision.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(
        RecordType::Decision, decision.stream, decision.frameId, decision.correlationId, decision.name);
    record.values[0] = decision.value0;
    record.values[1] = decision.value1;
    record.values[2] = decision.value2;
    record.values[3] = decision.value3;
    record.aux0 = static_cast<std::uint32_t>(decision.outcome);
    record.flags = decision.flags;
    copyText(record.detail.data(), record.detail.size(), decision.reason);
    rt.write(record);
    if ((decision.flags & DecisionTriggerBurst) != 0u) {
        gBurstTriggerSequence.fetch_add(1u, std::memory_order_relaxed);
    }
#else
    (void)decision;
#endif
}

void recordStateTransition(const StateTransition& transition) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || settings().mode == Mode::Errors || !transition.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(
        RecordType::StateTransition, transition.stream, transition.frameId, 0u, transition.name);
    record.values[0] = transition.previousValue;
    record.values[1] = transition.nextValue;
    record.values[2] = transition.subject;
    record.values[3] = transition.epoch;
    record.flags = transition.flags;
    copyText(record.detail.data(), record.detail.size(), transition.reason);
    rt.write(record);
#else
    (void)transition;
#endif
}

void recordResourceEvent(const ResourceEvent& event) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || settings().mode == Mode::Errors || !event.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(
        RecordType::ResourceEvent, event.stream, event.frameId, event.resourceId, event.name);
    record.values[0] = event.resourceId;
    record.values[1] = event.nativeHandle;
    record.values[2] = event.bytes;
    record.values[3] = event.alignment;
    record.aux0 = static_cast<std::uint32_t>(event.operation);
    record.aux1 = event.heapOrMemoryType;
    record.flags = event.flags;
    copyText(record.detail.data(), record.detail.size(), event.detail);
    rt.write(record);
#else
    (void)event;
#endif
}

void recordFlowEvent(const FlowEvent& event) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || settings().mode == Mode::Errors || !event.stream || event.flowId == 0u) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(
        RecordType::FlowEvent, event.stream, event.frameId, event.flowId, event.name);
    record.values[0] = event.parentFlowId;
    record.values[1] = event.value0;
    record.values[2] = event.value1;
    record.values[3] = event.occurrenceId;
    record.aux0 = static_cast<std::uint32_t>(event.phase);
    record.flags = event.flags;
    copyText(record.detail.data(), record.detail.size(), event.detail);
    rt.write(record);
#else
    (void)event;
#endif
}

void recordThreadInfo(const ThreadInfo& info) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || !info.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(RecordType::ThreadInfo, info.stream, 0u, 0u, info.name);
    record.values[0] = info.nativeThreadId != 0u ? info.nativeThreadId : currentThreadId();
    record.values[1] = info.affinityMask;
    record.values[2] = static_cast<std::uint64_t>(info.priority);
    record.flags = info.flags;
    copyText(record.detail.data(), record.detail.size(), info.role);
    rt.write(record);
#else
    (void)info;
#endif
}

void recordWaitInterval(const WaitInterval& interval) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!cpuEnabled() || !interval.stream || interval.startCpuTick == 0u ||
        interval.endCpuTick < interval.startCpuTick) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(
        RecordType::WaitInterval, interval.stream, interval.frameId, interval.correlationId, interval.name);
    record.values[0] = interval.startCpuTick;
    record.values[1] = interval.endCpuTick;
    record.values[2] = cpuTicksToNs(interval.endCpuTick - interval.startCpuTick);
    record.values[3] = interval.object;
    record.values[4] = interval.value;
    record.values[5] = interval.timeoutNs;
    record.values[6] = interval.observedValue;
    record.aux0 = static_cast<std::uint32_t>(interval.kind);
    record.aux1 = static_cast<std::uint32_t>(interval.result);
    record.flags = interval.flags;
    copyText(record.detail.data(), record.detail.size(), interval.detail);
    rt.write(record);
#else
    (void)interval;
#endif
}

void recordMetadata(const Metadata& metadata) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || settings().mode == Mode::Errors || !metadata.stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(RecordType::Metadata, metadata.stream, metadata.frameId, 0u, metadata.name);
    record.values[0] = metadata.value0;
    record.values[1] = metadata.value1;
    record.flags = metadata.flags;
    copyText(record.detail.data(), record.detail.size(), metadata.detail);
    rt.write(record);
#else
    (void)metadata;
#endif
}

void recordGpuWorkloadInfo(const GpuWorkloadInfo& info) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!gpuEnabled() || !info.stream || info.name == nullptr) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(
        RecordType::GpuWorkloadInfo, info.stream, 0u, info.workloadId, info.name);
    record.values[0] = info.nativePipeline;
    record.values[1] = info.codeHash;
    record.values[2] = info.codeBytes;
    record.values[3] = info.sharedMemoryBytes;
    record.values[4] = (static_cast<std::uint64_t>(info.threadsX) << 32u) | info.threadsY;
    record.values[5] = (static_cast<std::uint64_t>(info.threadsZ) << 32u) | info.subgroupSize;
    record.flags = info.flags;
    copyText(record.detail.data(), record.detail.size(), info.detail);
    rt.write(record);
#else
    (void)info;
#endif
}

void recordInstant(
    const StreamHandle stream,
    const std::uint64_t frameId,
    const char* name,
    const char* detail,
    const std::uint32_t flags) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || !stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    TraceRecord record = rt.baseRecord(RecordType::Instant, stream, frameId, 0u, name);
    copyText(record.detail.data(), record.detail.size(), detail);
    record.flags = flags;
    rt.write(record);
#else
    (void)stream; (void)frameId; (void)name; (void)detail; (void)flags;
#endif
}

void recordError(
    const StreamHandle stream,
    const std::uint64_t frameId,
    const char* stage,
    const StatusCode code,
    const std::string_view message,
    const std::uint64_t nativeCode,
    const std::uint64_t contextValue) noexcept
{
#if APUSR_ENABLE_PROFILER
    if (!enabled() || !settings().errors || !stream) {
        return;
    }
    Runtime& rt = runtime();
    if (!rt.active()) {
        return;
    }
    static std::atomic<std::uint64_t> correlation{1u};
    const std::uint64_t id = correlation.fetch_add(1u, std::memory_order_relaxed);
    TraceRecord record = rt.baseRecord(RecordType::Error, stream, frameId, id, stage);
    record.values[0] = static_cast<std::uint64_t>(code);
    record.values[1] = nativeCode;
    record.values[2] = contextValue;
    copyText(record.detail.data(), record.detail.size(), message);
    record.flags = static_cast<std::uint32_t>(Severity::Error);
    rt.write(record);
    if (message.size() >= record.detail.size()) {
        writeContinuation(rt, stream, frameId, id, message.substr(record.detail.size() - 1u));
    }
    // If the process survives the fault, every active sampler starts a fresh
    // burst on its next decision. This gives us post-failure timing context
    // without paying full-mode profiling cost all the time.
    gBurstTriggerSequence.fetch_add(1u, std::memory_order_relaxed);
    rt.flushAfterError();
#else
    (void)stream; (void)frameId; (void)stage; (void)code; (void)message; (void)nativeCode; (void)contextValue;
#endif
}

void recordStatus(
    const StreamHandle stream,
    const std::uint64_t frameId,
    const char* stage,
    const Status& status) noexcept
{
    if (status.code != StatusCode::Ok) {
        recordError(stream, frameId, stage, status.code, status.message);
    }
}

std::uint64_t hashCombine(std::uint64_t seed, const std::uint64_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
    return seed;
}

std::uint64_t hashString(const std::string_view value) noexcept
{
#if APUSR_ENABLE_PROFILER
    return fnv1a(value);
#else
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char c : value) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
#endif
}

CpuScope::CpuScope(
    const StreamHandle stream,
    const std::uint64_t frameId,
    const std::uint64_t sampleSequence,
    const char* name,
    const std::uint32_t flags) noexcept
    : stream_(stream), frameId_(frameId), sampleSequence_(sampleSequence), name_(name), flags_(flags)
{
    if (cpuEnabled() && stream_) {
        startTick_ = cpuTick();
    }
}

CpuScope::~CpuScope() noexcept
{
    if (startTick_ == 0u) {
        return;
    }
    recordCpuInterval(CpuInterval{
        .stream = stream_,
        .frameId = frameId_,
        .sampleSequence = sampleSequence_,
        .startCpuTick = startTick_,
        .endCpuTick = cpuTick(),
        .name = name_,
        .flags = flags_,
    });
}

WaitScope::WaitScope(
    const StreamHandle stream,
    const std::uint64_t frameId,
    const std::uint64_t correlationId,
    const char* name,
    const WaitKind kind,
    const std::uint64_t object,
    const std::uint64_t value,
    const std::uint64_t timeoutNs,
    const char* detail,
    const std::uint32_t flags) noexcept
    : stream_(stream),
      frameId_(frameId),
      correlationId_(correlationId),
      object_(object),
      value_(value),
      timeoutNs_(timeoutNs),
      name_(name),
      detail_(detail),
      kind_(kind),
      flags_(flags)
{
    if (cpuEnabled() && stream_) {
        startTick_ = cpuTick();
    }
}

WaitScope::~WaitScope() noexcept
{
    if (startTick_ == 0u) {
        return;
    }
    recordWaitInterval(WaitInterval{
        .stream = stream_,
        .frameId = frameId_,
        .correlationId = correlationId_,
        .name = name_,
        .detail = detail_,
        .kind = kind_,
        .result = result_,
        .startCpuTick = startTick_,
        .endCpuTick = cpuTick(),
        .object = object_,
        .value = value_,
        .timeoutNs = timeoutNs_,
        .observedValue = observedValue_,
        .flags = flags_,
    });
}

void WaitScope::setResult(const WaitResult result, const std::uint64_t observedValue) noexcept
{
    result_ = result;
    observedValue_ = observedValue;
}

} // namespace apusr::profile
