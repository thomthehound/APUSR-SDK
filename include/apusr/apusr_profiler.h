// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>

#include <array>
#include <cstdint>
#include <string_view>

#ifndef APUSR_ENABLE_PROFILER
#define APUSR_ENABLE_PROFILER 0
#endif

namespace apusr::profile {

enum class Mode : std::uint32_t {
    Off = 0,
    Sampled = 1,
    Full = 2,
    Errors = 3,
};

enum class BackendApi : std::uint32_t {
    None = 0,
    Dx12 = 1,
    Vulkan = 2,
    Xrt = 3,
};

enum class Component : std::uint32_t {
    Core = 0,
    Upscaler = 1,
    FrameGeneration = 2,
    GraphicsInterop = 3,
    Npu = 4,
    Frontend = 5,
};

enum class Severity : std::uint32_t {
    Info = 0,
    Warning = 1,
    Error = 2,
};

enum FrameFlags : std::uint32_t {
    FrameConfigurationChanged = 1u << 0u,
    FrameFailed = 1u << 1u,
    FrameBackendSwitched = 1u << 2u,
};

enum class QueueOperation : std::uint32_t {
    Submit = 0,
    Wait = 1,
    Signal = 2,
    Present = 3,
    Acquire = 4,
};

// Declares which semantic record classes a stream knows how to emit. This is
// a capability manifest, not an assertion that every capture must contain an
// observation of every class. Offline analysis uses it to distinguish
// "supported but not observed" from "instrumentation coverage unknown".
enum StreamCapability : std::uint64_t {
    StreamCapabilityNone = 0ull,
    StreamCapabilityCpuIntervals = 1ull << 0u,
    StreamCapabilityFrames = 1ull << 1u,
    StreamCapabilityGpuIntervals = 1ull << 2u,
    StreamCapabilityCalibration = 1ull << 3u,
    StreamCapabilityCounters = 1ull << 4u,
    StreamCapabilityInstants = 1ull << 5u,
    StreamCapabilityErrors = 1ull << 6u,
    StreamCapabilityQueueEvents = 1ull << 7u,
    StreamCapabilityDecisions = 1ull << 8u,
    StreamCapabilityStateTransitions = 1ull << 9u,
    StreamCapabilityResourceEvents = 1ull << 10u,
    StreamCapabilityFlows = 1ull << 11u,
    StreamCapabilityThreadInfo = 1ull << 12u,
    StreamCapabilityWaitIntervals = 1ull << 13u,
    StreamCapabilityMetadata = 1ull << 14u,
    StreamCapabilityGpuWorkloadInfo = 1ull << 15u,
};

// Structured branch outcomes are deliberately backend-neutral.  The caller
// supplies a stable subject name and reason string while these coarse outcomes
// let the offline reporter aggregate "why did this frame/path not proceed?"
// without hard-coding every frontend/backend policy into the report script.
enum class DecisionOutcome : std::uint32_t {
    Accepted = 0,
    Presented = 1,
    SourceOnly = 2,
    Deferred = 3,
    Warmup = 4,
    Skipped = 5,
    Dropped = 6,
    Fallback = 7,
    Retried = 8,
    Failed = 9,
};

enum DecisionFlags : std::uint32_t {
    // Start a fresh sampled burst on the next SampleGate decision.  This is
    // intended for rare policy/anomaly outcomes so the detailed GPU/CPU
    // capture naturally surrounds the next frames without making every frame
    // a full-profile frame.
    DecisionTriggerBurst = 1u << 0u,
};

enum class ResourceOperation : std::uint32_t {
    Create = 0,
    Destroy = 1,
    Allocate = 2,
    Free = 3,
    Import = 4,
    Export = 5,
    Acquire = 6,
    Release = 7,
    Bind = 8,
    Unbind = 9,
};

enum class FlowPhase : std::uint32_t {
    Begin = 0,
    Step = 1,
    End = 2,
    Cancel = 3,
};

enum class WaitKind : std::uint32_t {
    Unknown = 0,
    CpuPrimitive = 1,
    GpuFence = 2,
    TimelineSemaphore = 3,
    QueueIdle = 4,
    DeviceIdle = 5,
    Present = 6,
    Acquire = 7,
    Timer = 8,
    WorkerWake = 9,
    External = 10,
};

enum class WaitResult : std::uint32_t {
    Unknown = 0,
    Satisfied = 1,
    Timeout = 2,
    Interrupted = 3,
    Cancelled = 4,
    Failed = 5,
};

struct Settings {
    Mode mode = Mode::Off;
    bool cpu = false;
    bool gpu = false;
    bool errors = false;
    std::uint32_t sampleIntervalMs = 1000;
    std::uint32_t burstFrames = 3;
    std::uint32_t ringMiB = 32;
};

struct StreamHandle {
    std::uint64_t id = 0;
    BackendApi backend = BackendApi::None;
    Component component = Component::Core;

    [[nodiscard]] explicit operator bool() const noexcept { return id != 0; }
};

struct StreamDesc {
    BackendApi backend = BackendApi::None;
    Component component = Component::Core;
    const char* name = nullptr;
    std::uint64_t capabilities = StreamCapabilityNone;
    std::uint64_t nativeDevice = 0;
    std::uint64_t nativeQueue = 0;
    std::uint32_t apiVersion = 0;
    std::uint32_t driverVersion = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t queueFamily = 0xffffffffu;
    std::uint32_t flags = 0;
};

struct SampleDecision {
    bool capture = false;
    bool configurationChanged = false;
    std::uint64_t sampleSequence = 0;
    std::uint64_t burstSequence = 0;
    std::uint32_t burstFrame = 0;
};

class SampleGate final {
public:
    [[nodiscard]] SampleDecision decide(
        std::uint64_t configurationHash,
        std::uint64_t frameId,
        bool force = false) noexcept;

    // Use when configuration identity is owned by a logical sub-context rather
    // than by the gate itself. This keeps one periodic sampling cadence while
    // allowing callers with stable alternating contexts to report only genuine
    // per-context changes instead of retriggering a burst on every alternation.
    [[nodiscard]] SampleDecision decideStable(
        std::uint64_t frameId,
        bool configurationChanged,
        bool force = false) noexcept;

    void reset() noexcept;

private:
    std::uint64_t lastConfigurationHash_ = 0;
    std::uint64_t nextSampleTick_ = 0;
    std::uint64_t sampleSequence_ = 0;
    std::uint64_t burstSequence_ = 0;
    std::uint64_t lastTriggerSequence_ = 0;
    std::uint32_t burstRemaining_ = 0;
    bool haveConfiguration_ = false;
};

struct CpuInterval {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    std::uint64_t sampleSequence = 0;
    std::uint64_t startCpuTick = 0;
    std::uint64_t endCpuTick = 0;
    const char* name = nullptr;
    std::uint32_t flags = 0;
};

struct FrameSample {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    std::uint64_t sampleSequence = 0;
    std::uint64_t burstSequence = 0;
    std::uint64_t configurationHash = 0;
    std::uint32_t burstFrame = 0;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    std::uint64_t gpuDurationNs = 0;
    std::uint64_t cpuTotalNs = 0;
    std::uint64_t cpuRecordNs = 0;
    std::uint64_t totalWorkgroups = 0;
    std::uint64_t gpuBeginTick = 0;
    std::uint64_t gpuEndTick = 0;
    std::uint64_t gpuFrequency = 0;
    // Queue that produced the aggregate GPU timestamps when known. Zero means
    // the stream queue/unknown.
    std::uint64_t nativeQueue = 0;
    std::uint32_t flags = 0;
};

struct GpuInterval {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    std::uint64_t sampleSequence = 0;
    const char* name = nullptr;
    std::uint64_t durationNs = 0;
    std::uint64_t setupNs = 0;
    std::uint64_t executionNs = 0;
    std::uint64_t readBytes = 0;
    std::uint64_t writeBytes = 0;
    std::uint32_t groupsX = 0;
    std::uint32_t groupsY = 0;
    std::uint32_t groupsZ = 0;
    std::uint32_t profileLevel = 0xffffffffu;
    std::uint32_t barrierCount = 0;
    std::uint32_t descriptorCount = 0;
    std::uint64_t gpuBeginTick = 0;
    std::uint64_t gpuExecutionBeginTick = 0;
    std::uint64_t gpuEndTick = 0;
    std::uint64_t gpuFrequency = 0;
    // Optional native queue identity for intervals whose execution queue differs
    // from the stream's primary queue (for example FrameGen presentation vs.
    // interpolation). Zero means the stream queue/unknown.
    std::uint64_t nativeQueue = 0;
    std::uint32_t flags = 0;
};

struct ClockCalibration {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    std::uint64_t sampleSequence = 0;
    std::uint64_t cpuTick = 0;
    std::uint64_t gpuTick = 0;
    std::uint64_t gpuFrequency = 0;
    std::uint64_t maxDeviationNs = 0;
    // Queue that owns the calibrated GPU timestamp domain when known.
    std::uint64_t nativeQueue = 0;
    std::uint32_t gpuValidBits = 64;
    std::uint32_t flags = 0;
};

struct Counter {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    const char* name = nullptr;
    std::uint64_t value = 0;
    std::uint64_t value2 = 0;
    std::uint32_t flags = 0;
};

// Describes queue-side ordering work rather than a measured duration.  The
// profiler records the CPU enqueue point plus the native queue/synchronization
// identity so offline analysis can reconstruct dependencies without pretending
// that CPU enqueue spacing is GPU stall time.
struct QueueEvent {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    std::uint64_t sampleSequence = 0;
    const char* name = nullptr;
    std::uint64_t nativeQueue = 0;
    std::uint64_t syncObject = 0;
    std::uint64_t syncValue = 0;
    std::uint64_t relatedQueue = 0;
    std::uint64_t submitSequence = 0;
    QueueOperation operation = QueueOperation::Submit;
    std::uint32_t stageMask = 0;
    std::uint32_t objectCount = 0;
    std::uint32_t flags = 0;
};

// A structured policy/eligibility decision.  `reason` should be a stable,
// short machine-readable token (for example "explicit_reset" or
// "history_gap"), not prose.  value0..value3 are optional numeric context
// whose meaning is owned by `name` and documented at the instrumentation site.
struct Decision {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    std::uint64_t correlationId = 0;
    const char* name = nullptr;
    const char* reason = nullptr;
    DecisionOutcome outcome = DecisionOutcome::Accepted;
    std::uint64_t value0 = 0;
    std::uint64_t value1 = 0;
    std::uint64_t value2 = 0;
    std::uint64_t value3 = 0;
    std::uint32_t flags = 0;
};

// Records durable logical/device state changes.  This is intentionally not a
// generic counter: previous/next values and the optional subject identity let
// the report reconstruct residency and spot state that was entered but never
// left (backend mode, FG enablement, model epoch, queue mode, etc.).
struct StateTransition {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    const char* name = nullptr;
    const char* reason = nullptr;
    std::uint64_t previousValue = 0;
    std::uint64_t nextValue = 0;
    std::uint64_t subject = 0;
    std::uint64_t epoch = 0;
    std::uint32_t flags = 0;
};

// Resource lifetime events are distinct from memory-budget counters.  They
// describe APUSR-owned logical/native objects so the report can detect
// unmatched creates/frees and compute peak/outstanding bytes without assuming
// that an OS budget query is a resource ledger.
struct ResourceEvent {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    const char* name = nullptr;
    const char* detail = nullptr;
    ResourceOperation operation = ResourceOperation::Create;
    std::uint64_t resourceId = 0;
    std::uint64_t nativeHandle = 0;
    std::uint64_t bytes = 0;
    std::uint64_t alignment = 0;
    std::uint32_t heapOrMemoryType = 0xffffffffu;
    std::uint32_t flags = 0;
};

// Cross-thread/module logical flow markers.  GPU queue causality remains in
// QueueEvent; FlowEvent is for higher-level work such as a runtime frame,
// feature evaluation, or generated-frame lifecycle that can cross worker
// threads before it reaches a queue.
struct FlowEvent {
    StreamHandle stream;
    // Semantic frame identity supplied by the host/application when available.
    // Repeated occurrences of the same host frame must retain that identity.
    std::uint64_t frameId = 0;
    // Distinguishes repeated occurrences/operations without pretending that the
    // host supplied a new frame identity (for example repeated Presents of a
    // static menu frame). Zero means no separate occurrence identity is known.
    std::uint64_t occurrenceId = 0;
    std::uint64_t flowId = 0;
    std::uint64_t parentFlowId = 0;
    const char* name = nullptr;
    const char* detail = nullptr;
    FlowPhase phase = FlowPhase::Step;
    std::uint64_t value0 = 0;
    std::uint64_t value1 = 0;
    std::uint32_t flags = 0;
};

// Names and characterizes a worker/current thread once. Thread metadata is
// intentionally a separate record rather than an instant so Perfetto and the
// CSV reports can label numeric OS thread IDs and reason about worker roles.
struct ThreadInfo {
    StreamHandle stream;
    const char* name = nullptr;
    const char* role = nullptr;
    std::uint64_t nativeThreadId = 0; // zero means the current thread.
    std::int64_t priority = 0;
    std::uint64_t affinityMask = 0;
    std::uint32_t flags = 0;
};

// A measured host wait with the native object/value that caused it. This is
// deliberately distinct from CpuInterval: callers can state whether a wait was
// a fence/timeline/timer/worker wake and whether it satisfied, timed out, or
// was interrupted. Offline analysis can then join host waits to QueueEvent
// signals without guessing from scope names.
struct WaitInterval {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    std::uint64_t correlationId = 0;
    const char* name = nullptr;
    const char* detail = nullptr;
    WaitKind kind = WaitKind::Unknown;
    WaitResult result = WaitResult::Unknown;
    std::uint64_t startCpuTick = 0;
    std::uint64_t endCpuTick = 0;
    std::uint64_t object = 0;
    std::uint64_t value = 0;
    std::uint64_t timeoutNs = 0;
    std::uint64_t observedValue = 0;
    std::uint32_t flags = 0;
};

// Sparse run/build/configuration metadata. `name` is a stable key, `detail` is
// its human-readable/string value, and value0/value1 hold optional numeric
// forms. This is intentionally cheap and event-driven rather than a generic
// per-frame key/value logger.
struct Metadata {
    StreamHandle stream;
    std::uint64_t frameId = 0;
    const char* name = nullptr;
    const char* detail = nullptr;
    std::uint64_t value0 = 0;
    std::uint64_t value1 = 0;
    std::uint32_t flags = 0;
};

// Static identity/shape for a GPU workload. Timed GpuInterval records answer
// how long a dispatch took; this record answers what code/pipeline that name
// actually referred to. Emit it when a compute pipeline is created or selected,
// not per dispatch. Zero-valued fields mean the backend cannot expose that
// property without perturbing the workload.
struct GpuWorkloadInfo {
    StreamHandle stream;
    const char* name = nullptr;
    const char* detail = nullptr;
    std::uint64_t workloadId = 0;
    std::uint64_t nativePipeline = 0;
    std::uint64_t codeHash = 0;
    std::uint64_t codeBytes = 0;
    std::uint64_t sharedMemoryBytes = 0;
    std::uint32_t threadsX = 0;
    std::uint32_t threadsY = 0;
    std::uint32_t threadsZ = 0;
    std::uint32_t subgroupSize = 0;
    std::uint32_t flags = 0;
};

[[nodiscard]] const Settings& settings() noexcept;
[[nodiscard]] bool enabled() noexcept;
[[nodiscard]] bool cpuEnabled() noexcept;
[[nodiscard]] bool gpuEnabled() noexcept;
[[nodiscard]] std::uint64_t cpuTick() noexcept;
[[nodiscard]] std::uint64_t cpuFrequency() noexcept;
[[nodiscard]] std::uint64_t cpuTicksToNs(std::uint64_t ticks) noexcept;
[[nodiscard]] std::uint64_t durationNs(std::uint64_t beginTick, std::uint64_t endTick) noexcept;

[[nodiscard]] StreamHandle registerStream(const StreamDesc& desc) noexcept;
void recordCpuInterval(const CpuInterval& interval) noexcept;
void recordFrameSample(const FrameSample& sample) noexcept;
void recordGpuInterval(const GpuInterval& interval) noexcept;
void recordCalibration(const ClockCalibration& calibration) noexcept;
void recordCounter(const Counter& counter) noexcept;
void recordQueueEvent(const QueueEvent& event) noexcept;
void recordDecision(const Decision& decision) noexcept;
void recordStateTransition(const StateTransition& transition) noexcept;
void recordResourceEvent(const ResourceEvent& event) noexcept;
void recordFlowEvent(const FlowEvent& event) noexcept;
void recordThreadInfo(const ThreadInfo& info) noexcept;
void recordWaitInterval(const WaitInterval& interval) noexcept;
void recordMetadata(const Metadata& metadata) noexcept;
void recordGpuWorkloadInfo(const GpuWorkloadInfo& info) noexcept;
void requestBurst() noexcept;
void recordInstant(StreamHandle stream, std::uint64_t frameId, const char* name, const char* detail = nullptr, std::uint32_t flags = 0) noexcept;
void recordError(
    StreamHandle stream,
    std::uint64_t frameId,
    const char* stage,
    StatusCode code,
    std::string_view message,
    std::uint64_t nativeCode = 0,
    std::uint64_t contextValue = 0) noexcept;
void recordStatus(StreamHandle stream, std::uint64_t frameId, const char* stage, const Status& status) noexcept;

[[nodiscard]] std::uint64_t hashCombine(std::uint64_t seed, std::uint64_t value) noexcept;
[[nodiscard]] std::uint64_t hashString(std::string_view value) noexcept;

class CpuScope final {
public:
    CpuScope(StreamHandle stream, std::uint64_t frameId, std::uint64_t sampleSequence, const char* name, std::uint32_t flags = 0) noexcept;
    ~CpuScope() noexcept;

    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;

private:
    StreamHandle stream_{};
    std::uint64_t frameId_ = 0;
    std::uint64_t sampleSequence_ = 0;
    std::uint64_t startTick_ = 0;
    const char* name_ = nullptr;
    std::uint32_t flags_ = 0;
};

class WaitScope final {
public:
    WaitScope(
        StreamHandle stream,
        std::uint64_t frameId,
        std::uint64_t correlationId,
        const char* name,
        WaitKind kind,
        std::uint64_t object = 0,
        std::uint64_t value = 0,
        std::uint64_t timeoutNs = 0,
        const char* detail = nullptr,
        std::uint32_t flags = 0) noexcept;
    ~WaitScope() noexcept;

    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

    void setResult(WaitResult result, std::uint64_t observedValue = 0) noexcept;

private:
    StreamHandle stream_{};
    std::uint64_t frameId_ = 0;
    std::uint64_t correlationId_ = 0;
    std::uint64_t startTick_ = 0;
    std::uint64_t object_ = 0;
    std::uint64_t value_ = 0;
    std::uint64_t timeoutNs_ = 0;
    std::uint64_t observedValue_ = 0;
    const char* name_ = nullptr;
    const char* detail_ = nullptr;
    WaitKind kind_ = WaitKind::Unknown;
    WaitResult result_ = WaitResult::Unknown;
    std::uint32_t flags_ = 0;
};

} // namespace apusr::profile
