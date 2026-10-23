// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>
#include <apusr/apusr_manifest.h>

#include <cstdint>
#include <memory>

namespace apusr {

enum class NpuVendor : std::uint32_t {
    AmdXdna,
    Intel,
    Qualcomm,
    Unknown,
};

enum class NpuRuntimeKind : std::uint32_t {
    AmdXrt,
    VendorNative,
};

enum class NpuSubmissionGranularity : std::uint32_t {
    TokenBand,
    FrameWorklist,
    PersistentDoorbell,
};

enum class NpuPersistentExecutionMode : std::uint32_t {
    // Current bridge: descriptors are staged through the persistent ABI, then
    // lowered to the existing frame-worklist backend. This is not the product
    // endpoint; it keeps the control-plane ABI honest during bring-up.
    FrameWorklistLowering = 0,

    // Future bridge: a persistent NPU-side boundary controller consumes the
    // control ring directly. Worker tiles still follow their local stream,
    // lock, and DMA contracts; they do not poll host-visible ring state.
    PersistentControlRing = 1,

    // Product target: boundary epochs feed a free-running tiled pipeline. The
    // control plane marks valid input and completed output epochs; the compute
    // data plane stays payload/stream driven.
    StreamingPipeline = 2,
};

inline constexpr std::uint32_t kNpuPersistentDoorbellAbiMagic = 0x5244504eu; // NPDR, little-endian.
inline constexpr std::uint32_t kNpuPersistentDoorbellAbiVersion = 5;
inline constexpr std::uint32_t kNpuPersistentDoorbellHeaderBytes = 128;
inline constexpr std::uint32_t kNpuPersistentDoorbellDescriptorStride = 256;
inline constexpr std::uint32_t kNpuPersistentDoorbellDescriptorOffset = kNpuPersistentDoorbellHeaderBytes;

enum class NpuDoorbellDescriptorKind : std::uint32_t {
    Invalid = 0,
    TokenBand = 1,
    FrameEnd = 2,
    Shutdown = 3,
};

enum class NpuDoorbellPayloadOwner : std::uint32_t {
    GpuQueue = 0,
    CpuDiagnostic = 1,
};

enum class NpuDoorbellCompletionConsumer : std::uint32_t {
    GpuQueue = 0,
    CpuDiagnostic = 1,
    None = 2,
};

enum NpuDoorbellDescriptorFlags : std::uint32_t {
    NpuDoorbellDescriptorFlagNone = 0,
    NpuDoorbellDescriptorFlagFirstBand = 1u << 0u,
    NpuDoorbellDescriptorFlagLastBand = 1u << 1u,
};

struct alignas(kNpuPersistentDoorbellHeaderBytes) NpuDoorbellRingHeader {
    std::uint32_t magic = kNpuPersistentDoorbellAbiMagic;
    std::uint32_t version = kNpuPersistentDoorbellAbiVersion;
    std::uint32_t headerByteSize = kNpuPersistentDoorbellHeaderBytes;
    std::uint32_t descriptorStrideBytes = kNpuPersistentDoorbellDescriptorStride;
    std::uint32_t descriptorOffsetBytes = kNpuPersistentDoorbellDescriptorOffset;
    std::uint32_t descriptorCapacity = 0;
    std::uint32_t flags = 0;
    std::uint32_t executionMode = static_cast<std::uint32_t>(NpuPersistentExecutionMode::FrameWorklistLowering);

    // Boundary ownership state. These sequences are for the host/XRT/GPU-facing
    // control plane, not for worker-tile scheduling. The final tiled pipeline
    // should consume payload through its local stream, lock, and DMA contracts.
    std::uint64_t publishedSequence = 0;
    std::uint64_t retiredSequence = 0;
    std::uint64_t completedSequence = 0;

    // Frame epochs are the visible validity contract for the outside world.
    // They let the GPU import only completed output and let APUSR drain or
    // reconfigure the persistent pipeline without exposing tile-local control.
    std::uint64_t publishedFrameEpoch = 0;
    std::uint64_t completedFrameEpoch = 0;
    std::uint64_t reconfigureEpoch = 0;
    std::uint64_t shutdownEpoch = 0;

    std::uint64_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
    std::uint64_t reserved2 = 0;
    std::uint64_t reserved3 = 0;
    std::uint64_t reserved4 = 0;
};

struct alignas(kNpuPersistentDoorbellDescriptorStride) NpuDoorbellTokenBandDesc {
    std::uint32_t kind = static_cast<std::uint32_t>(NpuDoorbellDescriptorKind::TokenBand);
    std::uint32_t abiVersion = kNpuPersistentDoorbellAbiVersion;
    std::uint32_t byteSize = kNpuPersistentDoorbellDescriptorStride;
    std::uint32_t flags = NpuDoorbellDescriptorFlagNone;

    std::uint64_t sequence = 0;
    std::uint64_t frameIndex = 0;
    std::uint32_t segmentIndex = 0;
    std::uint32_t tokenSlotIndex = 0;

    std::uint32_t bandIndex = 0;
    std::uint32_t bandCount = 0;
    std::uint32_t inputFirstRow = 0;
    std::uint32_t inputRowCount = 0;
    std::uint32_t outputFirstRow = 0;
    std::uint32_t outputRowCount = 0;
    std::uint32_t egressFirstRow = 0;
    std::uint32_t egressRowCount = 0;

    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint64_t inputByteOffset = 0;
    std::uint64_t inputByteSize = 0;
    std::uint64_t outputByteOffset = 0;
    std::uint64_t outputByteSize = 0;

    std::uint32_t ingressOwner = static_cast<std::uint32_t>(NpuDoorbellPayloadOwner::GpuQueue);
    std::uint32_t egressConsumer = static_cast<std::uint32_t>(NpuDoorbellCompletionConsumer::GpuQueue);

    std::uint32_t tokenLogicalWidth = 0;
    std::uint32_t tokenPaddedWidth = 0;
    std::uint32_t tokenRowPitchBytes = 0;
    std::uint32_t tokenValidRowBytes = 0;

    std::uint64_t reserved[15]{};
};

static_assert(kNpuPersistentDoorbellHeaderBytes == 128, "NPU doorbell ring header ABI size changed");
static_assert(kNpuPersistentDoorbellDescriptorStride == 256, "NPU doorbell descriptor stride changed");
static_assert(kNpuPersistentDoorbellDescriptorOffset == kNpuPersistentDoorbellHeaderBytes, "NPU doorbell descriptor array must begin after the fixed header");
static_assert(sizeof(NpuDoorbellRingHeader) == kNpuPersistentDoorbellHeaderBytes, "NPU doorbell ring header ABI size changed");
static_assert(alignof(NpuDoorbellRingHeader) == kNpuPersistentDoorbellHeaderBytes, "NPU doorbell ring header alignment changed");
static_assert(sizeof(NpuDoorbellTokenBandDesc) == kNpuPersistentDoorbellDescriptorStride, "NPU doorbell descriptor ABI size changed");
static_assert(alignof(NpuDoorbellTokenBandDesc) == kNpuPersistentDoorbellDescriptorStride, "NPU doorbell descriptor alignment changed");

struct NpuBackendCapabilities {
    NpuVendor vendor = NpuVendor::Unknown;
    NpuRuntimeKind runtimeKind = NpuRuntimeKind::VendorNative;
    SharedBufferHandleKind importedBufferHandleKind = SharedBufferHandleKind::None;
    TimelineFenceHandleKind importedFenceHandleKind = TimelineFenceHandleKind::None;
    NpuSubmissionGranularity preferredSubmissionGranularity = NpuSubmissionGranularity::TokenBand;
    NpuPersistentExecutionMode persistentExecutionMode = NpuPersistentExecutionMode::FrameWorklistLowering;
    std::uint32_t persistentDoorbellAbiVersion = 0;
    bool supportsSharedBufferImport = false;
    bool supportsExternalTimelineFenceWait = false;
    bool supportsExternalTimelineFenceSignal = false;
    bool supportsPersistentDispatchObjects = false;
    bool supportsQuiescentDrain = false;
    bool supportsFrameWorklists = false;
    bool supportsPersistentDoorbells = false;
    // XRT doorbell rings live in host-visible shared memory imported through USERPTR.
    // The NPU does not advertise direct visibility into dedicated GPU memory.
    bool supportsHostVisibleDoorbellControlRing = false;
    bool supportsGpuPublishedDoorbells = false;
};

enum class NpuTokenBufferRole : std::uint32_t {
    Ingress,
    Egress,
};

enum class NpuControlBufferRole : std::uint32_t {
    DoorbellRing,
    ResidentParameterArena,
    SkipSpillArena,
};

enum class NpuPayloadProducer : std::uint32_t {
    GpuQueue,
    CpuDiagnostic,
};

enum class NpuPayloadConsumer : std::uint32_t {
    GpuQueue,
    CpuDiagnostic,
    None,
};

struct NpuPayloadContract {
    // Product payloads are queue-ordered GPU -> NPU -> GPU frame-slot slices.
    // CPU cache syncs are reserved for explicit diagnostic CPU payloads.
    NpuPayloadProducer ingressProducer = NpuPayloadProducer::GpuQueue;
    NpuPayloadConsumer egressConsumer = NpuPayloadConsumer::GpuQueue;
    std::uint64_t ingressByteSize = 0;
    std::uint64_t egressByteSize = 0;
};

struct NpuPayloadSyncStats {
    std::uint64_t toDeviceCount = 0;
    std::uint64_t toDeviceBytes = 0;
    std::uint64_t fromDeviceCount = 0;
    std::uint64_t fromDeviceBytes = 0;
};

struct NpuDoorbellRingStats {
    std::uint64_t reserveCount = 0;
    std::uint64_t commitCount = 0;
    std::uint64_t stagedDescriptorCount = 0;
    std::uint64_t occupancyHighWatermark = 0;
    std::uint64_t controlRingWriteCount = 0;
    std::uint64_t controlRingWriteBytes = 0;
    std::uint64_t publishedSequence = 0;
    std::uint64_t retiredSequence = 0;
    std::uint64_t completedSequence = 0;
};

struct NpuSubmissionStats {
    std::uint64_t frameWorklistCount = 0;
    std::uint64_t doorbellDescriptorCount = 0;
    std::uint64_t persistentDoorbellFrameCount = 0;
    std::uint64_t persistentControllerStartCount = 0;
    NpuDoorbellRingStats doorbellRing;
    std::uint64_t modelEpochParameterLoadCount = 0;
    std::uint64_t modelEpochDrainCount = 0;
    std::uint64_t modelEpochParameterDmaBytes = 0;
    std::uint64_t residentParameterArenaBytes = 0;
    std::uint64_t skipSpillArenaBytes = 0;
    std::uint64_t hotPathParameterDmaBytesPerRun = 0;
    std::uint64_t residentModelEpoch = 0;
    std::uint64_t frameCommandRunCount = 0;
    std::uint64_t tokenBandRunCount = 0;
    std::uint64_t submitWaitCount = 0;
    std::uint64_t startCount = 0;
    std::uint64_t submitSignalCount = 0;
    std::uint64_t enqueueFrameCpuNanoseconds = 0;
    std::uint64_t enqueueTokenBandCpuNanoseconds = 0;
    NpuPayloadSyncStats payloadSync;
};

struct NpuBufferView {
    SharedBuffer sharedBuffer;
    NativeHandle nativeBuffer;
    std::uint64_t byteOffset = 0;
    std::uint64_t byteSize = 0;
};

struct NpuDispatchDesc {
    SegmentManifest segment;
    TokenBandDesc band;
    std::uint32_t tokenSlotIndex = 0;
    NpuBufferView input;
    NpuBufferView output;
    NpuBufferView residentParameterArena;
    NpuBufferView skipSpillArena;
    std::uint64_t modelEpoch = 0;
    TimelineFenceWait waitBeforeRun;
    TimelineFenceSignal signalAfterRun;
    RenderSize renderSize;
    NpuPayloadContract payloadContract;
};

struct NpuFrameDispatchDesc {
    // dispatches and doorbellDescriptors are valid only for the duration of
    // enqueueFrame(). The descriptors are staged in the runtime-owned
    // persistent-doorbell ring before the backend lowers the worklist.
    std::uint64_t frameIndex = 0;
    const SegmentManifest* segment = nullptr;
    RenderSize renderSize;
    const NpuDispatchDesc* dispatches = nullptr;
    const NpuDoorbellRingHeader* doorbellRingHeader = nullptr;
    const NpuDoorbellTokenBandDesc* doorbellDescriptors = nullptr;
    NpuBufferView doorbellControlRing;
    NpuBufferView residentParameterArena;
    NpuBufferView skipSpillArena;
    std::uint64_t modelEpoch = 0;
    NpuBufferView ingressPayloadArena;
    NpuBufferView egressPayloadArena;
    std::uint64_t doorbellDescriptorByteOffset = 0;
    std::uint64_t doorbellStartSequence = 0;
    std::uint32_t doorbellStartIndex = 0;
    std::uint32_t doorbellRingCapacity = 0;
    std::uint32_t dispatchCount = 0;
};

struct NpuModelEpochParameterLoadDesc {
    NpuBufferView residentParameterArena;
    NpuBufferView skipSpillArena;
    const SegmentManifest* segment = nullptr;
    std::uint64_t residentParameterArenaBytes = 0;
    std::uint64_t skipSpillArenaBytes = 0;
    std::uint64_t modelEpochParameterDmaBytes = 0;
    std::uint64_t hotPathParameterDmaBytesPerRun = 0;
    std::uint64_t modelEpoch = 0;
};

class NpuBackend {
public:
    virtual ~NpuBackend() = default;

    [[nodiscard]] virtual NpuVendor vendor() const = 0;
    [[nodiscard]] virtual NpuRuntimeKind runtimeKind() const = 0;
    [[nodiscard]] virtual NpuBackendCapabilities capabilities() const = 0;
    [[nodiscard]] virtual Status createTimelineFence(std::uint64_t initialValue, TimelineFence& outFence) = 0;
    [[nodiscard]] virtual Status importSharedBuffer(
        const SharedBuffer& graphicsBuffer,
        NpuTokenBufferRole role,
        NpuBufferView& outView) = 0;

    [[nodiscard]] virtual Status importControlBuffer(
        const SharedBuffer& graphicsBuffer,
        NpuControlBufferRole role,
        NpuBufferView& outView)
    {
        (void)role;
        outView = NpuBufferView{
            .sharedBuffer = graphicsBuffer,
            .nativeBuffer = NativeHandle{},
            .byteOffset = 0,
            .byteSize = graphicsBuffer.byteSize,
        };
        return Status::ok();
    }

    [[nodiscard]] virtual Status loadModelParameters(const NpuModelEpochParameterLoadDesc& desc)
    {
        if (desc.residentParameterArenaBytes != 0 || desc.modelEpochParameterDmaBytes != 0) {
            return Status::notImplemented("NPU backend does not expose a model-epoch parameter load path");
        }
        return Status::ok();
    }

    [[nodiscard]] virtual Status drainSubmittedWork(std::uint32_t timeoutMilliseconds)
    {
        (void)timeoutMilliseconds;
        return Status::notImplemented("NPU backend does not expose a quiescent drain path");
    }

    [[nodiscard]] virtual Status enqueue(const NpuDispatchDesc& desc) = 0;

    [[nodiscard]] virtual Status enqueueFrame(const NpuFrameDispatchDesc& desc)
    {
        if (desc.dispatchCount != 0 && desc.dispatches == nullptr) {
            return Status::invalidArgument("NPU frame dispatch requires a dispatch array");
        }
        for (std::uint32_t dispatchIndex = 0; dispatchIndex < desc.dispatchCount; ++dispatchIndex) {
            Status status = enqueue(desc.dispatches[dispatchIndex]);
            if (!status.succeeded()) {
                return status;
            }
        }
        return Status::ok();
    }

    [[nodiscard]] virtual NpuPayloadSyncStats payloadSyncStats() const
    {
        return NpuPayloadSyncStats{};
    }

    [[nodiscard]] virtual NpuSubmissionStats submissionStats() const
    {
        return NpuSubmissionStats{};
    }

    // Developer-only probe hook. This waits for a previously enqueued run that
    // uses the same imported input/output views. It is intentionally not part of
    // the product hot path and exists so bring-up can prove one NPU chunk without
    // CPU polling or importing incomplete frame-slot pieces into the visible frame.
    [[nodiscard]] virtual Status waitForLastDispatch(const NpuDispatchDesc& desc, std::uint32_t timeoutMilliseconds)
    {
        (void)desc;
        (void)timeoutMilliseconds;
        return Status::notImplemented("NPU backend does not expose a blocking diagnostic dispatch wait");
    }
};

using NpuBackendPtr = std::shared_ptr<NpuBackend>;

[[nodiscard]] const char* npuVendorName(NpuVendor vendor);
[[nodiscard]] const char* npuRuntimeKindName(NpuRuntimeKind kind);

} // namespace apusr
