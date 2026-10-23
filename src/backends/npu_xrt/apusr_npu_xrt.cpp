// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/backends/apusr_npu_xrt.h>
#include <apusr/apusr_profiler.h>

#ifndef APUSR_ENABLE_HOT_PATH_GUARDS
#define APUSR_ENABLE_HOT_PATH_GUARDS 0
#endif

#ifndef APUSR_ENABLE_PERF_TELEMETRY
#define APUSR_ENABLE_PERF_TELEMETRY 0
#endif

#ifndef APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION
#define APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION 0
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <cstring>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32) && defined(APUSR_HAS_NATIVE_XRT) && APUSR_HAS_NATIVE_XRT
#define APUSR_XRT_NATIVE 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <xrt/experimental/xrt_fence.h>
#if __has_include(<xrt/xrt_xclbin.h>)
#include <xrt/xrt_xclbin.h>
#elif __has_include(<xrt/experimental/xrt_xclbin.h>)
#include <xrt/experimental/xrt_xclbin.h>
#else
#error "XRT xclbin header not found"
#endif
#include <xrt/xrt_bo.h>
#include <xrt/xrt_device.h>
#include <xrt/xrt_hw_context.h>
#include <xrt/xrt_kernel.h>
#else
#define APUSR_XRT_NATIVE 0
#endif

namespace apusr::backends {

namespace {

[[nodiscard]] bool isAlignedPointer(const void* pointer, const std::uint64_t alignment)
{
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    return pointer != nullptr && (value % alignment) == 0;
}

#if APUSR_XRT_NATIVE

[[nodiscard]] Status exceptionStatus(const char* operation, const std::exception& error)
{
    std::string message = operation;
    message += " failed: ";
    message += error.what();
    return Status::runtimeFailure(std::move(message));
}

[[nodiscard]] Status unknownExceptionStatus(const char* operation)
{
    std::string message = operation;
    message += " failed with an unknown exception";
    return Status::runtimeFailure(std::move(message));
}

struct XrtFenceState {
    xrt::fence fence;
    xrt::fence::export_handle exportHandle{};
};

struct XrtImportedBuffer {
    xrt::bo bo;
    void* userPointer = nullptr;
    std::uint64_t byteSize = 0;
};

struct XrtRunState {
    xrt::run run;
    const XrtImportedBuffer* input = nullptr;
    const XrtImportedBuffer* output = nullptr;
    std::uint64_t key = 0;
    bool inFlight = false;
};

struct XrtInstructionBuffer {
    xrt::bo bo;
    std::uint32_t instructionCount = 0;
};

struct XrtPersistentControllerState {
    xrt::run run;
    const XrtImportedBuffer* controlRing = nullptr;
    const XrtImportedBuffer* residentParameterArena = nullptr;
    const XrtImportedBuffer* skipSpillArena = nullptr;
    const XrtImportedBuffer* ingressArena = nullptr;
    const XrtImportedBuffer* egressArena = nullptr;
    bool started = false;
};

[[nodiscard]] xrt::fence::access_mode fenceAccessMode()
{
    return xrt::fence::access_mode::hybrid;
}

[[nodiscard]] xrt::hw_context::access_mode hwContextAccessMode(const AmdXrtBackendConfig& config)
{
    return config.preferExclusiveHwContext ?
        xrt::hw_context::access_mode::exclusive :
        xrt::hw_context::access_mode::shared;
}


[[nodiscard]] const char* ertCommandStateName(const ert_cmd_state state)
{
    switch (state) {
    case ERT_CMD_STATE_NEW:
        return "NEW";
    case ERT_CMD_STATE_QUEUED:
        return "QUEUED";
    case ERT_CMD_STATE_RUNNING:
        return "RUNNING";
    case ERT_CMD_STATE_COMPLETED:
        return "COMPLETED";
    case ERT_CMD_STATE_ERROR:
        return "ERROR";
    case ERT_CMD_STATE_ABORT:
        return "ABORT";
    case ERT_CMD_STATE_SUBMITTED:
        return "SUBMITTED";
    case ERT_CMD_STATE_TIMEOUT:
        return "TIMEOUT";
    case ERT_CMD_STATE_NORESPONSE:
        return "NORESPONSE";
    case ERT_CMD_STATE_SKERROR:
        return "SKERROR";
    default:
        return "UNKNOWN";
    }
}

[[nodiscard]] std::string xrtRunCommandErrorStatus(
    const char* operation,
    const xrt::run::command_error& error,
    const ert_cmd_state observedState)
{
    std::ostringstream message;
    const ert_cmd_state commandState = error.get_command_state();
    message << operation << " failed: " << error.what()
            << " command_state=" << ertCommandStateName(commandState)
            << "(" << static_cast<int>(commandState) << ")"
            << " observed_run_state=" << ertCommandStateName(observedState)
            << "(" << static_cast<int>(observedState) << ")";

    // The packaged Windows XRT import library does not export the header-declared
    // AIE health-data accessor. Keep diagnostics on the stable command-error API.

    return message.str();
}

[[nodiscard]] std::uint64_t runKey(const XrtImportedBuffer* input, const XrtImportedBuffer* output)
{
    const auto left = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(input));
    const auto right = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(output));
    return (left << 1U) ^ (right + 0x9e3779b97f4a7c15ull + (left << 6U) + (left >> 2U));
}


#if APUSR_ENABLE_PERF_TELEMETRY
class ScopedNanosecondCounter {
public:
    explicit ScopedNanosecondCounter(std::uint64_t& outCounter) : outCounter_(outCounter), start_(std::chrono::steady_clock::now()) {}

    ~ScopedNanosecondCounter()
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        outCounter_ += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

private:
    std::uint64_t& outCounter_;
    std::chrono::steady_clock::time_point start_;
};
#endif

[[nodiscard]] bool payloadSyncStatsChanged(const NpuPayloadSyncStats& before, const NpuPayloadSyncStats& after)
{
    return before.toDeviceCount != after.toDeviceCount ||
        before.toDeviceBytes != after.toDeviceBytes ||
        before.fromDeviceCount != after.fromDeviceCount ||
        before.fromDeviceBytes != after.fromDeviceBytes;
}

[[nodiscard]] bool isGpuQueuePayloadContract(const NpuPayloadContract& contract)
{
    return contract.ingressProducer == NpuPayloadProducer::GpuQueue &&
        contract.egressConsumer == NpuPayloadConsumer::GpuQueue;
}


[[nodiscard]] std::uint32_t expectedDoorbellFlags(const TokenBandDesc& band)
{
    std::uint32_t flags = NpuDoorbellDescriptorFlagNone;
    if (band.firstBandInFrame) {
        flags |= NpuDoorbellDescriptorFlagFirstBand;
    }
    if (band.lastBandInFrame) {
        flags |= NpuDoorbellDescriptorFlagLastBand;
    }
    return flags;
}

[[nodiscard]] Status validateDoorbellDescriptor(
    const NpuDispatchDesc& dispatch,
    const NpuDoorbellTokenBandDesc& descriptor,
    const std::uint64_t frameIndex,
    const std::uint64_t startSequence,
    const std::uint32_t dispatchIndex)
{
    if (descriptor.kind != static_cast<std::uint32_t>(NpuDoorbellDescriptorKind::TokenBand) ||
        descriptor.abiVersion != kNpuPersistentDoorbellAbiVersion ||
        descriptor.byteSize != kNpuPersistentDoorbellDescriptorStride) {
        return Status::invalidArgument("NPU doorbell descriptor header does not match the persistent ABI");
    }
    if (descriptor.sequence != startSequence + dispatchIndex || descriptor.frameIndex != frameIndex) {
        return Status::invalidArgument("NPU doorbell descriptor sequence/frame does not match the staged ring worklist");
    }
    if (descriptor.flags != expectedDoorbellFlags(dispatch.band)) {
        return Status::invalidArgument("NPU doorbell descriptor band flags do not match the token-band descriptor");
    }
    if (descriptor.tokenSlotIndex != dispatch.tokenSlotIndex ||
        descriptor.bandIndex != dispatch.band.bandIndex ||
        descriptor.bandCount != dispatch.band.bandCount) {
        return Status::invalidArgument("NPU doorbell descriptor token slot or band index does not match the dispatch");
    }
    if (descriptor.inputFirstRow != dispatch.band.inputFirstRow ||
        descriptor.inputRowCount != dispatch.band.inputRowCount ||
        descriptor.outputFirstRow != dispatch.band.outputFirstRow ||
        descriptor.outputRowCount != dispatch.band.outputRowCount ||
        descriptor.egressFirstRow != dispatch.band.egressFirstRow ||
        descriptor.egressRowCount != dispatch.band.egressRowCount) {
        return Status::invalidArgument("NPU doorbell descriptor row window does not match the dispatch");
    }
    if (descriptor.renderWidth != dispatch.renderSize.width || descriptor.renderHeight != dispatch.renderSize.height) {
        return Status::invalidArgument("NPU doorbell descriptor render size does not match the dispatch");
    }
    if (descriptor.inputByteOffset != dispatch.input.byteOffset ||
        descriptor.inputByteSize != dispatch.payloadContract.ingressByteSize ||
        descriptor.outputByteOffset != dispatch.output.byteOffset ||
        descriptor.outputByteSize != dispatch.payloadContract.egressByteSize) {
        return Status::invalidArgument("NPU doorbell descriptor payload range does not match the dispatch");
    }
    if (descriptor.tokenLogicalWidth != dispatch.band.inputShape.width ||
        descriptor.tokenPaddedWidth != tokenPaddedRowWidth(dispatch.band.inputShape) ||
        descriptor.tokenRowPitchBytes != tokenRowPitchBytes(dispatch.band.inputShape) ||
        descriptor.tokenValidRowBytes != tokenValidRowBytes(dispatch.band.inputShape)) {
        return Status::invalidArgument("NPU doorbell descriptor token row layout does not match the dispatch");
    }
    return Status::ok();
}

[[nodiscard]] Status validateDoorbellControlRingBuffer(const NpuFrameDispatchDesc& desc)
{
    if (desc.doorbellControlRing.sharedBuffer.nativeBuffer.value == nullptr) {
        return Status::invalidArgument("NPU frame worklist must carry the serialized doorbell control ring buffer");
    }
    if (desc.doorbellRingHeader == nullptr) {
        return Status::invalidArgument("NPU frame worklist must carry the doorbell ring header");
    }

    const std::uint64_t requiredBytes =
        static_cast<std::uint64_t>(desc.doorbellRingHeader->descriptorOffsetBytes) +
        (static_cast<std::uint64_t>(desc.doorbellRingHeader->descriptorCapacity) *
            desc.doorbellRingHeader->descriptorStrideBytes);
    if (desc.doorbellControlRing.byteSize < requiredBytes ||
        desc.doorbellControlRing.sharedBuffer.byteSize < requiredBytes) {
        return Status::invalidArgument("NPU doorbell control ring buffer is smaller than the staged ABI layout");
    }

    const auto* base = static_cast<const std::uint8_t*>(desc.doorbellControlRing.sharedBuffer.nativeBuffer.value);
    if (std::memcmp(base, desc.doorbellRingHeader, sizeof(NpuDoorbellRingHeader)) != 0) {
        return Status::invalidArgument("NPU doorbell control ring header bytes do not match the frame worklist header");
    }

    const std::uint64_t expectedDescriptorByteOffset =
        kNpuPersistentDoorbellDescriptorOffset +
        (static_cast<std::uint64_t>(desc.doorbellStartIndex) * kNpuPersistentDoorbellDescriptorStride);
    if (desc.doorbellDescriptorByteOffset != expectedDescriptorByteOffset) {
        return Status::invalidArgument("NPU frame worklist descriptor byte offset does not match the staged ring range");
    }
    const std::uint64_t descriptorBytes =
        static_cast<std::uint64_t>(desc.dispatchCount) * kNpuPersistentDoorbellDescriptorStride;
    if (descriptorBytes != 0 &&
        std::memcmp(base + desc.doorbellDescriptorByteOffset, desc.doorbellDescriptors, static_cast<std::size_t>(descriptorBytes)) != 0) {
        return Status::invalidArgument("NPU doorbell control ring descriptor bytes do not match the frame worklist range");
    }

    return Status::ok();
}

[[nodiscard]] Status validateFrameDoorbellDescriptors(const NpuFrameDispatchDesc& desc)
{
    if (desc.dispatchCount == 0) {
        return Status::ok();
    }
    if (desc.doorbellRingHeader == nullptr || desc.doorbellDescriptors == nullptr) {
        return Status::invalidArgument("NPU frame worklist must carry a staged persistent doorbell ring range");
    }
    const std::uint32_t executionMode = desc.doorbellRingHeader->executionMode;
    const bool supportedExecutionMode =
        executionMode == static_cast<std::uint32_t>(NpuPersistentExecutionMode::FrameWorklistLowering) ||
        executionMode == static_cast<std::uint32_t>(NpuPersistentExecutionMode::PersistentControlRing);
    if (desc.doorbellRingHeader->magic != kNpuPersistentDoorbellAbiMagic ||
        desc.doorbellRingHeader->version != kNpuPersistentDoorbellAbiVersion ||
        !supportedExecutionMode ||
        desc.doorbellRingHeader->descriptorOffsetBytes != kNpuPersistentDoorbellDescriptorOffset ||
        desc.doorbellRingHeader->descriptorStrideBytes != kNpuPersistentDoorbellDescriptorStride) {
        return Status::invalidArgument("NPU frame worklist doorbell ring header does not match the persistent ABI");
    }
    if (desc.doorbellRingCapacity != desc.doorbellRingHeader->descriptorCapacity ||
        desc.doorbellStartIndex >= desc.doorbellRingCapacity ||
        desc.dispatchCount > (desc.doorbellRingCapacity - desc.doorbellStartIndex)) {
        return Status::invalidArgument("NPU frame worklist doorbell ring range is outside the staged ring");
    }
    Status controlRingStatus = validateDoorbellControlRingBuffer(desc);
    if (!controlRingStatus.succeeded()) {
        return controlRingStatus;
    }
    for (std::uint32_t dispatchIndex = 0; dispatchIndex < desc.dispatchCount; ++dispatchIndex) {
        Status status = validateDoorbellDescriptor(
            desc.dispatches[dispatchIndex],
            desc.doorbellDescriptors[dispatchIndex],
            desc.frameIndex,
            desc.doorbellStartSequence,
            dispatchIndex);
        if (!status.succeeded()) {
            return status;
        }
    }
    return Status::ok();
}

[[nodiscard]] std::uint64_t ingressSyncBytes(const NpuDispatchDesc& desc)
{
    return desc.payloadContract.ingressByteSize != 0 ? desc.payloadContract.ingressByteSize : desc.band.inputByteSize;
}

[[nodiscard]] std::uint64_t egressSyncBytes(const NpuDispatchDesc& desc)
{
    return desc.payloadContract.egressByteSize != 0 ? desc.payloadContract.egressByteSize : desc.band.outputByteSize;
}

[[nodiscard]] Status validatePayloadRange(
    const NpuBufferView& view,
    const std::uint64_t byteSize,
    const char* name)
{
    if (byteSize == 0) {
        return Status::ok();
    }
    if (view.byteOffset > view.byteSize || byteSize > (view.byteSize - view.byteOffset)) {
        std::string message = name;
        message += " payload sync range exceeds imported token view";
        return Status::invalidArgument(std::move(message));
    }
    if (byteSize > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max()) ||
        view.byteOffset > static_cast<std::uint64_t>(std::numeric_limits<size_t>::max())) {
        std::string message = name;
        message += " payload sync range does not fit size_t";
        return Status::invalidArgument(std::move(message));
    }
    return Status::ok();
}

[[nodiscard]] xrt::memory_group kernelMemoryGroup(const xrt::kernel& kernel, const int argIndex)
{
    return static_cast<xrt::memory_group>(kernel.group_id(argIndex));
}

void setBoArg(xrt::run& run, const int index, const xrt::bo& value)
{
    if (index >= 0) {
        run.set_arg(index, value);
    }
}

void setU32Arg(xrt::run& run, const int index, const std::uint32_t value)
{
    if (index >= 0) {
        run.set_arg(index, value);
    }
}

[[nodiscard]] XrtImportedBuffer* importedControlRing(const NpuFrameDispatchDesc& desc)
{
    return static_cast<XrtImportedBuffer*>(desc.doorbellControlRing.nativeBuffer.value);
}

#endif

class AmdXrtNpuBackend final : public NpuBackend {
public:
    explicit AmdXrtNpuBackend(AmdXrtBackendConfig config) : config_(std::move(config)) {}

    [[nodiscard]] NpuVendor vendor() const override { return NpuVendor::AmdXdna; }
    [[nodiscard]] NpuRuntimeKind runtimeKind() const override { return NpuRuntimeKind::AmdXrt; }

    [[nodiscard]] NpuBackendCapabilities capabilities() const override
    {
        return NpuBackendCapabilities{
            .vendor = NpuVendor::AmdXdna,
            .runtimeKind = NpuRuntimeKind::AmdXrt,
            .importedBufferHandleKind = SharedBufferHandleKind::HostUserPointer,
            .importedFenceHandleKind = TimelineFenceHandleKind::Dx12Fence,
            .preferredSubmissionGranularity = APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION != 0 ?
                NpuSubmissionGranularity::PersistentDoorbell :
                NpuSubmissionGranularity::FrameWorklist,
            .persistentExecutionMode = APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION != 0 ?
                NpuPersistentExecutionMode::PersistentControlRing :
                NpuPersistentExecutionMode::FrameWorklistLowering,
            .persistentDoorbellAbiVersion = kNpuPersistentDoorbellAbiVersion,
            .supportsSharedBufferImport = true,
            .supportsExternalTimelineFenceWait = true,
            .supportsExternalTimelineFenceSignal = true,
            .supportsPersistentDispatchObjects = true,
            .supportsQuiescentDrain = true,
            .supportsFrameWorklists = true,
            .supportsPersistentDoorbells = APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION != 0,
            .supportsHostVisibleDoorbellControlRing = true,
            .supportsGpuPublishedDoorbells = false,
        };
    }

    [[nodiscard]] NpuPayloadSyncStats payloadSyncStats() const override
    {
        return payloadSyncStats_;
    }

    [[nodiscard]] NpuSubmissionStats submissionStats() const override
    {
        NpuSubmissionStats out = submissionStats_;
        out.payloadSync = payloadSyncStats_;
        return out;
    }

    [[nodiscard]] Status loadModelParameters(const NpuModelEpochParameterLoadDesc& desc) override
    {
        if (desc.hotPathParameterDmaBytesPerRun != 0) {
            return Status::invalidArgument("XRT resident middle-core hot path must not DMA parameters");
        }
        if (desc.residentParameterArenaBytes == 0 || desc.modelEpochParameterDmaBytes == 0) {
            return Status::invalidArgument("XRT model-epoch parameter load requires non-empty resident bytes and DMA bytes");
        }
        if (desc.modelEpochParameterDmaBytes > desc.residentParameterArenaBytes) {
            return Status::invalidArgument("XRT model-epoch parameter load exceeds the resident arena size");
        }
        if (desc.residentParameterArena.nativeBuffer.value == nullptr) {
            return Status::invalidArgument("XRT model-epoch parameter load requires an imported resident parameter arena");
        }
        if (config_.kernelAbi.skipSpillBufferArgIndex >= 0 && desc.skipSpillArena.nativeBuffer.value == nullptr) {
            return Status::invalidArgument("XRT resident middle-core profile requires an imported skip spill arena");
        }
        if (desc.skipSpillArena.nativeBuffer.value != nullptr &&
            desc.skipSpillArena.nativeBuffer.value == desc.residentParameterArena.nativeBuffer.value) {
            return Status::invalidArgument("XRT skip spill arena must not alias the resident parameter arena");
        }
        if (desc.modelEpoch == 0) {
            return Status::invalidArgument("XRT model-epoch parameter load requires a non-zero model epoch");
        }

        ensureProfileStream();
        const apusr::profile::StreamHandle sampledStream = apusr::profile::cpuEnabled() ? profileStream_ : apusr::profile::StreamHandle{};
        apusr::profile::CpuScope profileScope(sampledStream, desc.modelEpoch, 0u, "xrt.model_epoch_load");

#if APUSR_XRT_NATIVE
        auto* residentParameterArena = static_cast<XrtImportedBuffer*>(desc.residentParameterArena.nativeBuffer.value);
        auto* skipSpillArena = static_cast<XrtImportedBuffer*>(desc.skipSpillArena.nativeBuffer.value);
        try {
            Status status = ensureInitialized();
            if (!status.succeeded()) {
                return status;
            }

            if (parameterLoadInstructions_ == nullptr) {
                return Status::invalidArgument("XRT resident middle-core profile requires a model-epoch parameter-load instruction stream");
            }

            {
                apusr::profile::CpuScope scope(sampledStream, desc.modelEpoch, 0u, "xrt.model_epoch_dma");
                residentParameterArena->bo.sync(
                    XCL_BO_SYNC_BO_TO_DEVICE,
                    static_cast<size_t>(desc.modelEpochParameterDmaBytes),
                    0);
            }

            xrt::run run{kernel_};
            bindParameterLoadArguments(run, *residentParameterArena, skipSpillArena);
            run.start();
            std::cv_status waitResult{};
            {
                apusr::profile::WaitScope wait(
                    sampledStream, desc.modelEpoch, 0u, "xrt.model_epoch_wait",
                    apusr::profile::WaitKind::External, 0u, 0u, 10'000'000'000ull,
                    "xrt_run_completion");
                waitResult = run.wait2(std::chrono::milliseconds{10000});
                wait.setResult(waitResult == std::cv_status::timeout
                    ? apusr::profile::WaitResult::Timeout
                    : apusr::profile::WaitResult::Satisfied);
            }
            if (waitResult == std::cv_status::timeout) {
                return Status::runtimeFailure("XRT model-epoch parameter load timed out");
            }
            submissionStats_.startCount += 1;
#else
        (void)desc;
#endif
            modelParametersLoaded_ = true;
            loadedModelEpoch_ = desc.modelEpoch;
            submissionStats_.modelEpochParameterLoadCount += 1;
            submissionStats_.modelEpochParameterDmaBytes += desc.modelEpochParameterDmaBytes;
            submissionStats_.residentParameterArenaBytes = desc.residentParameterArenaBytes;
            submissionStats_.skipSpillArenaBytes = desc.skipSpillArenaBytes;
            submissionStats_.hotPathParameterDmaBytesPerRun = desc.hotPathParameterDmaBytesPerRun;
            submissionStats_.residentModelEpoch = desc.modelEpoch;
            if (sampledStream) {
                apusr::profile::recordCounter(apusr::profile::Counter{
                    .stream = sampledStream, .frameId = desc.modelEpoch, .name = "xrt.model_epoch_bytes",
                    .value = desc.modelEpochParameterDmaBytes, .value2 = desc.residentParameterArenaBytes});
            }
            return Status::ok();
#if APUSR_XRT_NATIVE
        } catch (const xrt::run::command_error& error) {
            return Status::runtimeFailure(xrtRunCommandErrorStatus("XRT model-epoch parameter load", error, ERT_CMD_STATE_NORESPONSE));
        } catch (const std::exception& error) {
            return exceptionStatus("XRT model-epoch parameter load", error);
        } catch (...) {
            return unknownExceptionStatus("XRT model-epoch parameter load");
        }
#endif
    }

    [[nodiscard]] Status createTimelineFence(const std::uint64_t initialValue, TimelineFence& outFence) override
    {
        if (config_.requireFenceInterop && (config_.xclbinPath.empty() && config_.xclbinData.empty())) {
            return Status::invalidArgument("AMD XRT backend requires a packaged xclbin before creating interop fences");
        }
        if (config_.requireFenceInterop && config_.kernelName.empty()) {
            return Status::invalidArgument("AMD XRT backend requires a kernel name before creating interop fences");
        }

#if APUSR_XRT_NATIVE
        Status status = ensureInitialized();
        if (!status.succeeded()) {
            return status;
        }

        if (initialValue != 0) {
            return Status::invalidArgument("AMD XRT timeline fences must start at zero; APUSR advances them through queue/XRT signal operations");
        }

        try {
            auto state = std::make_unique<XrtFenceState>();
            state->fence = xrt::fence{device_, fenceAccessMode()};
            state->exportHandle = state->fence.export_fence();

            outFence.nativeNpuFence = NativeHandle{&state->fence};
            outFence.nativeFence = outFence.nativeNpuFence;
            outFence.nativeSharedHandle = NativeHandle{reinterpret_cast<void*>(static_cast<std::uintptr_t>(state->exportHandle))};
            outFence.handleKind = TimelineFenceHandleKind::Dx12Fence;
            outFence.value = state->fence.get_next_state();

            fences_.push_back(std::move(state));
            return Status::ok();
        } catch (const std::exception& error) {
            return exceptionStatus("xrt::fence(hybrid) creation/export", error);
        } catch (...) {
            return unknownExceptionStatus("xrt::fence(hybrid) creation/export");
        }
#else
        (void)initialValue;
        (void)outFence;
        return Status::notImplemented("native AMD XRT backend is not built; configure on Windows with XRT_ROOT to enable xrt::fence interop");
#endif
    }

    [[nodiscard]] Status importSharedBuffer(
        const SharedBuffer& graphicsBuffer,
        const NpuTokenBufferRole role,
        NpuBufferView& outView) override
    {
        if ((config_.xclbinPath.empty() && config_.xclbinData.empty())) {
            return Status::invalidArgument("AMD XRT backend requires a packaged xclbin");
        }
        if (config_.kernelName.empty()) {
            return Status::invalidArgument("AMD XRT backend requires a kernel name");
        }
        if (config_.instructionData.empty()) {
            return Status::invalidArgument("AMD XRT backend requires packaged MLIR-AIE insts.bin bytes");
        }
        if ((config_.instructionData.size() % sizeof(std::uint32_t)) != 0) {
            return Status::invalidArgument("MLIR-AIE instruction stream must be uint32-aligned");
        }
        if (graphicsBuffer.byteSize == 0) {
            return Status::invalidArgument("cannot import an empty graphics token buffer into XRT");
        }
        if (graphicsBuffer.handleKind != SharedBufferHandleKind::HostUserPointer) {
            return Status::invalidArgument("AMD XRT backend requires host-userptr token buffers for the fast path");
        }
        if (!isAlignedPointer(graphicsBuffer.nativeBuffer.value, kHostUserptrSharedBufferAlignmentBytes)) {
            return Status::invalidArgument("XRT USERPTR token buffer must be 4096-byte aligned");
        }

#if APUSR_XRT_NATIVE
        Status status = ensureInitialized();
        if (!status.succeeded()) {
            return status;
        }

        try {
            const int kernelArgIndex = tokenBufferKernelArgIndex(role);
            auto imported = std::make_unique<XrtImportedBuffer>();
            imported->userPointer = graphicsBuffer.nativeBuffer.value;
            imported->byteSize = graphicsBuffer.byteSize;
            imported->bo = xrt::bo{
                hwContext_,
                imported->userPointer,
                static_cast<size_t>(imported->byteSize),
                xrt::bo::flags::host_only,
                kernelMemoryGroup(kernel_, kernelArgIndex)};

            outView.sharedBuffer = graphicsBuffer;
            outView.nativeBuffer = NativeHandle{imported.get()};
            outView.byteOffset = 0;
            outView.byteSize = graphicsBuffer.byteSize;

            importedBuffers_.push_back(std::move(imported));
            return Status::ok();
        } catch (const std::exception& error) {
            return exceptionStatus("XRT USERPTR BO import", error);
        } catch (...) {
            return unknownExceptionStatus("XRT USERPTR BO import");
        }
#else
        (void)role;
        (void)outView;
        return Status::notImplemented("native AMD XRT backend is not built; configure on Windows with XRT_ROOT to enable USERPTR BO import");
#endif
    }


    [[nodiscard]] Status importControlBuffer(
        const SharedBuffer& graphicsBuffer,
        const NpuControlBufferRole role,
        NpuBufferView& outView) override
    {
        const bool doorbellRole = role == NpuControlBufferRole::DoorbellRing;
        const bool residentParameterRole = role == NpuControlBufferRole::ResidentParameterArena;
        const bool skipSpillRole = role == NpuControlBufferRole::SkipSpillArena;
        if (!doorbellRole && !residentParameterRole && !skipSpillRole) {
            return Status::invalidArgument("AMD XRT backend only supports doorbell, resident-parameter, and skip-spill control buffers");
        }
        if (doorbellRole && graphicsBuffer.byteSize < kNpuPersistentDoorbellHeaderBytes) {
            return Status::invalidArgument("XRT doorbell control-ring buffer is smaller than the persistent ABI header");
        }
        if (residentParameterRole && graphicsBuffer.byteSize == 0) {
            return Status::invalidArgument("XRT resident parameter arena cannot be empty");
        }
        if (skipSpillRole && graphicsBuffer.byteSize == 0) {
            return Status::invalidArgument("XRT skip spill arena cannot be empty");
        }
        if (graphicsBuffer.handleKind != SharedBufferHandleKind::HostUserPointer) {
            return Status::invalidArgument(doorbellRole ?
                "AMD XRT backend requires a host-userptr doorbell control ring" :
                (residentParameterRole ?
                    "AMD XRT backend requires a host-userptr resident parameter arena" :
                    "AMD XRT backend requires a host-userptr skip spill arena"));
        }
        if (graphicsBuffer.nativeBuffer.value == nullptr) {
            return Status::invalidArgument(doorbellRole ?
                "XRT doorbell control ring requires a mapped shared pointer" :
                (residentParameterRole ?
                    "XRT resident parameter arena requires a mapped shared pointer" :
                    "XRT skip spill arena requires a mapped shared pointer"));
        }
        if (!isAlignedPointer(graphicsBuffer.nativeBuffer.value, kHostUserptrSharedBufferAlignmentBytes)) {
            return Status::invalidArgument(doorbellRole ?
                "XRT doorbell control ring USERPTR must be 4096-byte aligned" :
                (residentParameterRole ?
                    "XRT resident parameter arena USERPTR must be 4096-byte aligned" :
                    "XRT skip spill arena USERPTR must be 4096-byte aligned"));
        }

        outView = NpuBufferView{
            .sharedBuffer = graphicsBuffer,
            .nativeBuffer = NativeHandle{},
            .byteOffset = 0,
            .byteSize = graphicsBuffer.byteSize,
        };

#if APUSR_XRT_NATIVE
        const int kernelArgIndex = doorbellRole ?
            config_.kernelAbi.controlRingBufferArgIndex :
            (residentParameterRole ?
                config_.kernelAbi.residentParameterBufferArgIndex :
                config_.kernelAbi.skipSpillBufferArgIndex);
        if (kernelArgIndex < 0) {
            return Status::ok();
        }

        Status status = ensureInitialized();
        if (!status.succeeded()) {
            return status;
        }

        try {
            auto imported = std::make_unique<XrtImportedBuffer>();
            imported->userPointer = graphicsBuffer.nativeBuffer.value;
            imported->byteSize = graphicsBuffer.byteSize;
            imported->bo = xrt::bo{
                hwContext_,
                imported->userPointer,
                static_cast<size_t>(imported->byteSize),
                xrt::bo::flags::host_only,
                kernelMemoryGroup(kernel_, kernelArgIndex)};

            outView.nativeBuffer = NativeHandle{imported.get()};
            importedBuffers_.push_back(std::move(imported));
            return Status::ok();
        } catch (const std::exception& error) {
            return exceptionStatus(doorbellRole ? "XRT doorbell control-ring BO import" : (residentParameterRole ? "XRT resident parameter arena BO import" : "XRT skip spill arena BO import"), error);
        } catch (...) {
            return unknownExceptionStatus(doorbellRole ? "XRT doorbell control-ring BO import" : (residentParameterRole ? "XRT resident parameter arena BO import" : "XRT skip spill arena BO import"));
        }
#else
        return Status::ok();
#endif
    }

    [[nodiscard]] Status drainSubmittedWork(const std::uint32_t timeoutMilliseconds) override
    {
        ensureProfileStream();
        const apusr::profile::StreamHandle sampledStream = apusr::profile::cpuEnabled() ? profileStream_ : apusr::profile::StreamHandle{};
        apusr::profile::CpuScope profileScope(sampledStream, 0u, 0u, "xrt.drain");
#if APUSR_XRT_NATIVE
        try {
            if (persistentController_ != nullptr && persistentController_->started) {
                return Status::unsupportedBackend("XRT persistent controller drain requires an explicit pause/ack protocol");
            }
            for (const auto& state : runs_) {
                if (!state->inFlight) {
                    continue;
                }
                const auto waitResult = state->run.wait2(std::chrono::milliseconds{timeoutMilliseconds});
                if (waitResult == std::cv_status::timeout) {
                    return Status::runtimeFailure("XRT quiescent drain timed out");
                }
                state->inFlight = false;
            }
            submissionStats_.modelEpochDrainCount += 1;
            return Status::ok();
        } catch (const xrt::run::command_error& error) {
            return Status::runtimeFailure(xrtRunCommandErrorStatus("XRT quiescent drain", error, ERT_CMD_STATE_NORESPONSE));
        } catch (const std::exception& error) {
            return exceptionStatus("XRT quiescent drain", error);
        } catch (...) {
            return unknownExceptionStatus("XRT quiescent drain");
        }
#else
        (void)timeoutMilliseconds;
        submissionStats_.modelEpochDrainCount += 1;
        return Status::ok();
#endif
    }

    [[nodiscard]] Status enqueue(const NpuDispatchDesc& desc) override
    {
        ensureProfileStream();
        apusr::profile::SampleDecision profileDecision{};
        apusr::profile::StreamHandle sampledStream{};
        if (apusr::profile::cpuEnabled() && profileStream_) {
            std::uint64_t configurationHash = 0x4150555358525442ull; // "APUSXRTB"
            configurationHash = apusr::profile::hashCombine(configurationHash,
                (static_cast<std::uint64_t>(desc.renderSize.width) << 32u) | desc.renderSize.height);
            configurationHash = apusr::profile::hashCombine(configurationHash, desc.modelEpoch);
            configurationHash = apusr::profile::hashCombine(configurationHash,
                (static_cast<std::uint64_t>(desc.band.bandCount) << 32u) | desc.band.bandIndex);
            profileDecision = profileSampleGate_.decide(configurationHash, 0u);
            if (profileDecision.capture)
                sampledStream = profileStream_;
        }
        apusr::profile::CpuScope profileScope(
            sampledStream, 0u, profileDecision.sampleSequence, "xrt.enqueue_band");
#if APUSR_ENABLE_PERF_TELEMETRY && APUSR_XRT_NATIVE
        ScopedNanosecondCounter telemetryTimer{submissionStats_.enqueueTokenBandCpuNanoseconds};
#endif
#if APUSR_ENABLE_HOT_PATH_GUARDS
        if (desc.input.byteSize == 0 || desc.output.byteSize == 0) {
            return Status::invalidArgument("XRT dispatch requires non-empty input and output token views");
        }
        if (desc.input.nativeBuffer.value == nullptr || desc.output.nativeBuffer.value == nullptr) {
            return Status::invalidArgument("XRT dispatch requires imported token BO views");
        }
        if (desc.waitBeforeRun.fence.nativeNpuFence.value == nullptr) {
            return Status::invalidArgument("XRT dispatch requires an NPU-side wait fence");
        }
        if (desc.signalAfterRun.fence.nativeNpuFence.value == nullptr) {
            return Status::invalidArgument("XRT dispatch requires an NPU-side signal fence");
        }
#endif

#if APUSR_XRT_NATIVE
        return enqueueLoweredDispatch(desc, nullptr, false, sampledStream, 0u, profileDecision.sampleSequence);
#else
        (void)desc;
        return Status::notImplemented("native AMD XRT backend is not built; configure on Windows with XRT_ROOT to enqueue xrt::run objects");
#endif
    }

    [[nodiscard]] Status enqueueFrame(const NpuFrameDispatchDesc& desc) override
    {
        ensureProfileStream();
        apusr::profile::SampleDecision profileDecision{};
        apusr::profile::StreamHandle sampledStream{};
        std::uint64_t profileConfigurationHash = 0u;
        if (apusr::profile::cpuEnabled() && profileStream_) {
            profileConfigurationHash = 0x4150555358525446ull; // "APUSXRTF"
            profileConfigurationHash = apusr::profile::hashCombine(profileConfigurationHash,
                (static_cast<std::uint64_t>(desc.renderSize.width) << 32u) | desc.renderSize.height);
            profileConfigurationHash = apusr::profile::hashCombine(profileConfigurationHash, desc.modelEpoch);
            profileConfigurationHash = apusr::profile::hashCombine(profileConfigurationHash, desc.dispatchCount);
            profileConfigurationHash = apusr::profile::hashCombine(profileConfigurationHash,
                APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION != 0 ? 1u : 0u);
            if (desc.segment != nullptr)
                profileConfigurationHash = apusr::profile::hashCombine(profileConfigurationHash, apusr::profile::hashString(desc.segment->name));
            profileDecision = profileSampleGate_.decide(profileConfigurationHash, desc.frameIndex);
            if (profileDecision.capture)
                sampledStream = profileStream_;
        }
        const std::uint64_t profileFrameBegin = sampledStream ? apusr::profile::cpuTick() : 0u;
        apusr::profile::CpuScope profileScope(
            sampledStream, desc.frameIndex, profileDecision.sampleSequence, "xrt.enqueue_frame");
        const auto recordSuccessfulProfileFrame = [&]() noexcept {
            if (!sampledStream || profileFrameBegin == 0u) {
                return;
            }
            const std::uint64_t endTick = apusr::profile::cpuTick();
            const std::uint64_t duration = apusr::profile::durationNs(profileFrameBegin, endTick);
            apusr::profile::recordFrameSample(apusr::profile::FrameSample{
                .stream = sampledStream,
                .frameId = desc.frameIndex,
                .sampleSequence = profileDecision.sampleSequence,
                .burstSequence = profileDecision.burstSequence,
                .configurationHash = profileConfigurationHash,
                .burstFrame = profileDecision.burstFrame,
                .renderWidth = desc.renderSize.width,
                .renderHeight = desc.renderSize.height,
                .cpuTotalNs = duration,
                .cpuRecordNs = duration,
                .flags = profileDecision.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u,
            });
        };
#if APUSR_ENABLE_PERF_TELEMETRY && APUSR_XRT_NATIVE
        ScopedNanosecondCounter telemetryTimer{submissionStats_.enqueueFrameCpuNanoseconds};
#endif
        if (desc.dispatchCount != 0 && desc.dispatches == nullptr) {
            return Status::invalidArgument("XRT frame worklist requires a dispatch array");
        }
#if APUSR_XRT_NATIVE
        Status doorbellStatus = validateFrameDoorbellDescriptors(desc);
        if (!doorbellStatus.succeeded()) {
            return doorbellStatus;
        }
#else
        if (desc.dispatchCount != 0 && (desc.doorbellRingHeader == nullptr || desc.doorbellDescriptors == nullptr)) {
            return Status::invalidArgument("XRT frame worklist must carry a staged persistent doorbell ring range");
        }
#endif
        submissionStats_.doorbellDescriptorCount += desc.dispatchCount;
#if APUSR_XRT_NATIVE
        XrtImportedBuffer* controlRing = nullptr;
        if (config_.kernelAbi.controlRingBufferArgIndex >= 0) {
            controlRing = importedControlRing(desc);
            if (controlRing == nullptr) {
                return Status::invalidArgument("XRT persistent-control-ring kernel requires an imported control-ring BO");
            }
        }
        if (APUSR_ENABLE_PERSISTENT_CONTROL_RING_EXECUTION != 0) {
            Status status = enqueuePersistentControlRingFrame(
                desc, controlRing, sampledStream, desc.frameIndex, profileDecision.sampleSequence);
            if (!status.succeeded()) {
                return status;
            }
            submissionStats_.frameWorklistCount += 1;
            submissionStats_.persistentDoorbellFrameCount += 1;
            if (sampledStream) {
                apusr::profile::recordCounter(apusr::profile::Counter{
                    .stream = sampledStream, .frameId = desc.frameIndex, .name = "xrt.frame_worklist",
                    .value = desc.dispatchCount, .value2 = desc.doorbellRingCapacity});
                apusr::profile::recordCounter(apusr::profile::Counter{
                    .stream = sampledStream, .frameId = desc.frameIndex, .name = "xrt.payload_arena_bytes",
                    .value = desc.ingressPayloadArena.byteSize, .value2 = desc.egressPayloadArena.byteSize});
            }
            recordSuccessfulProfileFrame();
            return Status::ok();
        }
        for (std::uint32_t dispatchIndex = 0; dispatchIndex < desc.dispatchCount; ++dispatchIndex) {
            const bool frameCommandRun = desc.dispatchCount == 1;
            Status status = enqueueLoweredDispatch(
                desc.dispatches[dispatchIndex], controlRing, frameCommandRun,
                sampledStream, desc.frameIndex, profileDecision.sampleSequence);
            if (!status.succeeded()) {
                return status;
            }
        }
#else
        for (std::uint32_t dispatchIndex = 0; dispatchIndex < desc.dispatchCount; ++dispatchIndex) {
            Status status = enqueue(desc.dispatches[dispatchIndex]);
            if (!status.succeeded()) {
                return status;
            }
        }
#endif
        submissionStats_.frameWorklistCount += 1;
        if (sampledStream) {
            apusr::profile::recordCounter(apusr::profile::Counter{
                .stream = sampledStream, .frameId = desc.frameIndex, .name = "xrt.frame_worklist",
                .value = desc.dispatchCount, .value2 = desc.doorbellRingCapacity});
            apusr::profile::recordCounter(apusr::profile::Counter{
                .stream = sampledStream, .frameId = desc.frameIndex, .name = "xrt.payload_arena_bytes",
                .value = desc.ingressPayloadArena.byteSize, .value2 = desc.egressPayloadArena.byteSize});
        }
        recordSuccessfulProfileFrame();
        return Status::ok();
    }

    [[nodiscard]] Status waitForLastDispatch(const NpuDispatchDesc& desc, const std::uint32_t timeoutMilliseconds) override
    {
        ensureProfileStream();
        const apusr::profile::StreamHandle sampledStream = apusr::profile::cpuEnabled() ? profileStream_ : apusr::profile::StreamHandle{};
        apusr::profile::CpuScope profileScope(sampledStream, 0u, 0u, "xrt.wait_diagnostic");
#if APUSR_XRT_NATIVE
        auto* input = static_cast<XrtImportedBuffer*>(desc.input.nativeBuffer.value);
        auto* output = static_cast<XrtImportedBuffer*>(desc.output.nativeBuffer.value);
        if (input == nullptr || output == nullptr) {
            return Status::invalidArgument("XRT diagnostic wait requires imported input/output token BO views");
        }

        try {
            XrtRunState* state = lastRunForBuffers(input, output);
            if (state == nullptr) {
                return Status::runtimeFailure("XRT diagnostic wait has no submitted run for the token buffers");
            }
            std::cv_status waitResult{};
            {
                apusr::profile::WaitScope wait(
                    sampledStream, desc.modelEpoch, 0u, "xrt.wait_diagnostic_completion",
                    apusr::profile::WaitKind::External, 0u, 0u,
                    static_cast<std::uint64_t>(timeoutMilliseconds) * 1'000'000ull,
                    "xrt_run_completion");
                waitResult = state->run.wait2(std::chrono::milliseconds{timeoutMilliseconds});
                wait.setResult(waitResult == std::cv_status::timeout
                    ? apusr::profile::WaitResult::Timeout
                    : apusr::profile::WaitResult::Satisfied);
            }
            if (waitResult == std::cv_status::timeout) {
                return Status::runtimeFailure("XRT diagnostic tile run timed out");
            }
            state->inFlight = false;
            return synchronizeCpuDiagnosticEgressReadback(desc, *output);
        } catch (const xrt::run::command_error& error) {
            XrtRunState* state = lastRunForBuffers(input, output);
            const ert_cmd_state observedState = state == nullptr ? ERT_CMD_STATE_NORESPONSE : state->run.state();
            return Status::runtimeFailure(xrtRunCommandErrorStatus("XRT diagnostic copy wait", error, observedState));
        } catch (const std::exception& error) {
            return exceptionStatus("XRT diagnostic copy wait", error);
        } catch (...) {
            return unknownExceptionStatus("XRT diagnostic copy wait");
        }
#else
        (void)desc;
        (void)timeoutMilliseconds;
        return Status::notImplemented("native AMD XRT backend is not built; configure on Windows with XRT_ROOT to wait for diagnostic runs");
#endif
    }

private:
    AmdXrtBackendConfig config_;
    NpuPayloadSyncStats payloadSyncStats_;
    NpuSubmissionStats submissionStats_;
    bool modelParametersLoaded_ = false;
    std::uint64_t loadedModelEpoch_ = 0;
    apusr::profile::StreamHandle profileStream_{};
    apusr::profile::SampleGate profileSampleGate_{};

    void ensureProfileStream() noexcept
    {
        if (!profileStream_ && apusr::profile::enabled()) {
            profileStream_ = apusr::profile::registerStream(apusr::profile::StreamDesc{
                .backend = apusr::profile::BackendApi::Xrt,
                .component = apusr::profile::Component::Npu,
                .name = "npu.xrt",
                .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                    apusr::profile::StreamCapabilityFrames |
                    apusr::profile::StreamCapabilityCounters |
                    apusr::profile::StreamCapabilityWaitIntervals,
            });
            profileSampleGate_.reset();
        }
    }

#if APUSR_XRT_NATIVE
    bool initialized_ = false;
    xrt::device device_;
    xrt::xclbin xclbin_;
    xrt::hw_context hwContext_;
    xrt::kernel kernel_;
    std::vector<std::unique_ptr<XrtFenceState>> fences_;
    std::vector<std::unique_ptr<XrtImportedBuffer>> importedBuffers_;
    std::vector<std::unique_ptr<XrtRunState>> runs_;
    std::unique_ptr<XrtInstructionBuffer> instructions_;
    std::unique_ptr<XrtInstructionBuffer> parameterLoadInstructions_;
    std::unordered_map<std::uint64_t, std::vector<XrtRunState*>> runPoolByTokenPair_;
    std::unordered_map<std::uint64_t, XrtRunState*> lastRunByTokenPair_;
    std::unique_ptr<XrtPersistentControllerState> persistentController_;

    [[nodiscard]] int tokenBufferKernelArgIndex(const NpuTokenBufferRole role) const
    {
        return role == NpuTokenBufferRole::Ingress ?
            config_.kernelAbi.inputTokenBufferArgIndex :
            config_.kernelAbi.outputTokenBufferArgIndex;
    }

    [[nodiscard]] Status enqueuePersistentControlRingFrame(
        const NpuFrameDispatchDesc& desc,
        const XrtImportedBuffer* controlRing,
        const apusr::profile::StreamHandle sampledStream,
        const std::uint64_t profileFrameId,
        const std::uint64_t sampleSequence)
    {
        if (controlRing == nullptr) {
            return Status::invalidArgument("persistent control-ring execution requires an imported control-ring BO");
        }
        if (desc.ingressPayloadArena.nativeBuffer.value == nullptr || desc.egressPayloadArena.nativeBuffer.value == nullptr) {
            return Status::invalidArgument("persistent control-ring execution requires imported ingress and egress payload arenas");
        }

        auto* residentParameterArena = static_cast<XrtImportedBuffer*>(desc.residentParameterArena.nativeBuffer.value);
        auto* skipSpillArena = static_cast<XrtImportedBuffer*>(desc.skipSpillArena.nativeBuffer.value);
        auto* ingressArena = static_cast<XrtImportedBuffer*>(desc.ingressPayloadArena.nativeBuffer.value);
        auto* egressArena = static_cast<XrtImportedBuffer*>(desc.egressPayloadArena.nativeBuffer.value);
        if (config_.kernelAbi.residentParameterBufferArgIndex >= 0 && residentParameterArena == nullptr) {
            return Status::invalidArgument("persistent control-ring execution requires an imported resident parameter arena for this kernel profile");
        }
        if (config_.kernelAbi.skipSpillBufferArgIndex >= 0 && skipSpillArena == nullptr) {
            return Status::invalidArgument("persistent control-ring execution requires an imported skip spill arena for this kernel profile");
        }
        if (config_.kernelAbi.residentParameterBufferArgIndex >= 0 && !modelParametersLoaded_) {
            return Status::runtimeFailure("persistent control-ring execution cannot start before the model-epoch parameter load completes");
        }
        if (config_.kernelAbi.residentParameterBufferArgIndex >= 0 && desc.modelEpoch != loadedModelEpoch_) {
            return Status::runtimeFailure("persistent control-ring execution model epoch does not match resident parameters");
        }
        if (ingressArena == nullptr || egressArena == nullptr) {
            return Status::invalidArgument("persistent control-ring execution requires valid XRT payload arena imports");
        }
        if (skipSpillArena != nullptr &&
            (skipSpillArena == residentParameterArena || skipSpillArena == ingressArena || skipSpillArena == egressArena)) {
            return Status::invalidArgument("persistent control-ring execution requires a skip arena distinct from resident and payload BOs");
        }

        try {
            if (persistentController_ != nullptr && persistentController_->started) {
                // The persistent-control-ring runtime_sequence is a launcher for
                // resident AIE DMA/core work. XRT command completion means the
                // launch command retired; it is not a per-frame completion or a
                // controller-failure signal. Keep the controller bound once and
                // publish later frame epochs through the shared control ring.
                return Status::ok();
            }

            Status initStatus = ensureInitialized();
            if (!initStatus.succeeded()) {
                return initStatus;
            }

            apusr::profile::CpuScope startScope(
                sampledStream, profileFrameId, sampleSequence, "xrt.persistent_controller_start");
            auto controller = std::make_unique<XrtPersistentControllerState>();
            controller->run = xrt::run{kernel_};
            controller->controlRing = controlRing;
            controller->residentParameterArena = residentParameterArena;
            controller->skipSpillArena = skipSpillArena;
            controller->ingressArena = ingressArena;
            controller->egressArena = egressArena;
            bindPersistentControllerArguments(controller->run, *controlRing, residentParameterArena, skipSpillArena, *ingressArena, *egressArena);
            controller->run.start();
            controller->started = true;
            persistentController_ = std::move(controller);
            submissionStats_.startCount += 1;
            submissionStats_.persistentControllerStartCount += 1;
            return Status::ok();
        } catch (const std::exception& error) {
            return exceptionStatus("XRT persistent control-ring enqueue", error);
        } catch (...) {
            return unknownExceptionStatus("XRT persistent control-ring enqueue");
        }
    }

    [[nodiscard]] Status enqueueLoweredDispatch(
        const NpuDispatchDesc& desc,
        const XrtImportedBuffer* controlRing,
        const bool frameCommandRun = false,
        const apusr::profile::StreamHandle sampledStream = {},
        const std::uint64_t profileFrameId = 0u,
        const std::uint64_t sampleSequence = 0u)
    {
#if APUSR_ENABLE_HOT_PATH_GUARDS
        Status status = ensureInitialized();
        if (!status.succeeded()) {
            return status;
        }
#endif

        auto* input = static_cast<XrtImportedBuffer*>(desc.input.nativeBuffer.value);
        auto* output = static_cast<XrtImportedBuffer*>(desc.output.nativeBuffer.value);
        auto* residentParameterArena = static_cast<XrtImportedBuffer*>(desc.residentParameterArena.nativeBuffer.value);
        auto* skipSpillArena = static_cast<XrtImportedBuffer*>(desc.skipSpillArena.nativeBuffer.value);
        auto* waitFence = static_cast<xrt::fence*>(desc.waitBeforeRun.fence.nativeNpuFence.value);
        auto* signalFence = static_cast<xrt::fence*>(desc.signalAfterRun.fence.nativeNpuFence.value);
        if (config_.kernelAbi.controlRingBufferArgIndex >= 0 && controlRing == nullptr) {
            return Status::invalidArgument("XRT persistent-control-ring kernel requires a control-ring BO when lowering token-band work");
        }
        if (config_.kernelAbi.residentParameterBufferArgIndex >= 0 && residentParameterArena == nullptr) {
            return Status::invalidArgument("XRT resident-parameter kernel requires an imported resident parameter arena");
        }
        if (config_.kernelAbi.skipSpillBufferArgIndex >= 0 && skipSpillArena == nullptr) {
            return Status::invalidArgument("XRT resident middle-core kernel requires an imported skip spill arena");
        }
        if (skipSpillArena != nullptr &&
            (skipSpillArena == residentParameterArena || skipSpillArena == input || skipSpillArena == output)) {
            return Status::invalidArgument("XRT skip spill arena must be distinct from resident and payload BOs");
        }
        if (config_.kernelAbi.residentParameterBufferArgIndex >= 0 && !modelParametersLoaded_) {
            return Status::runtimeFailure("XRT resident-parameter kernel cannot enter the hot path before the model-epoch parameter load completes");
        }
        if (config_.kernelAbi.residentParameterBufferArgIndex >= 0 && desc.modelEpoch != loadedModelEpoch_) {
            return Status::runtimeFailure("XRT dispatch model epoch does not match resident parameters");
        }

        // XRT fence cursors are an internal debugging aid. The product
        // contract is the explicit GPU-ready/NPU-done fence choreography.
        const NpuPayloadSyncStats beforeStats = payloadSyncStats_;

        try {
            Status syncStatus{};
            {
                apusr::profile::CpuScope scope(
                    sampledStream, profileFrameId, sampleSequence, "xrt.sync_ingress");
                syncStatus = synchronizeCpuDiagnosticIngress(desc, *input);
            }
            if (!syncStatus.succeeded()) {
                return syncStatus;
            }
            {
                apusr::profile::CpuScope scope(
                    sampledStream, profileFrameId, sampleSequence, "xrt.sync_egress_sentinel");
                syncStatus = synchronizeCpuDiagnosticEgressSentinel(desc, *output);
            }
            if (!syncStatus.succeeded()) {
                return syncStatus;
            }

            XrtRunState* state = nullptr;
            {
                apusr::profile::CpuScope scope(
                    sampledStream, profileFrameId, sampleSequence, "xrt.acquire_run");
                syncStatus = acquireRunForSubmission(input, output, state);
            }
            if (!syncStatus.succeeded()) {
                return syncStatus;
            }

            {
                apusr::profile::CpuScope scope(
                    sampledStream, profileFrameId, sampleSequence, "xrt.bind_submit");
                bindRunArguments(state->run, desc, *input, *output, controlRing);
                state->run.submit_wait(*waitFence);
                submissionStats_.submitWaitCount += 1;
                state->run.start();
                state->inFlight = true;
                submissionStats_.startCount += 1;
                state->run.submit_signal(*signalFence);
                submissionStats_.submitSignalCount += 1;
            }
            if (frameCommandRun) {
                submissionStats_.frameCommandRunCount += 1;
            } else {
                submissionStats_.tokenBandRunCount += 1;
            }

            if (isGpuQueuePayloadContract(desc.payloadContract) &&
                payloadSyncStatsChanged(beforeStats, payloadSyncStats_)) {
                return Status::runtimeFailure("product XRT dispatch attempted payload BO sync");
            }

            if (sampledStream) {
                apusr::profile::recordCounter(apusr::profile::Counter{
                    .stream = sampledStream, .frameId = profileFrameId, .name = "xrt.band_bytes",
                    .value = desc.band.inputByteSize, .value2 = desc.band.outputByteSize});
                apusr::profile::recordCounter(apusr::profile::Counter{
                    .stream = sampledStream, .frameId = profileFrameId, .name = "xrt.band_index_count",
                    .value = desc.band.bandIndex, .value2 = desc.band.bandCount});
            }
            return Status::ok();
        } catch (const std::exception& error) {
            return exceptionStatus(frameCommandRun ? "XRT frame-command enqueue" : "XRT token-band enqueue", error);
        } catch (...) {
            return unknownExceptionStatus(frameCommandRun ? "XRT frame-command enqueue" : "XRT token-band enqueue");
        }
    }

    [[nodiscard]] Status ensureInitialized()
    {
        if (initialized_) {
            return Status::ok();
        }
        if ((config_.xclbinPath.empty() && config_.xclbinData.empty())) {
            return Status::invalidArgument("AMD XRT backend requires a packaged xclbin");
        }
        if (config_.kernelName.empty()) {
            return Status::invalidArgument("AMD XRT backend requires a kernel name");
        }

        try {
            device_ = xrt::device{config_.deviceIndex};
            xclbin_ = config_.xclbinData.empty() ? xrt::xclbin{config_.xclbinPath} : xrt::xclbin{config_.xclbinData};
            const xrt::uuid uuid = device_.register_xclbin(xclbin_);
            hwContext_ = xrt::hw_context{device_, uuid, hwContextAccessMode(config_)};
            kernel_ = xrt::kernel{hwContext_, config_.kernelName};
            instructions_ = createInstructionBuffer(config_.instructionData, "hot-path MLIR-AIE instruction stream");
            if (!config_.parameterLoadInstructionData.empty()) {
                parameterLoadInstructions_ = createInstructionBuffer(
                    config_.parameterLoadInstructionData,
                    "model-epoch parameter-load MLIR-AIE instruction stream");
            }
            initialized_ = true;
            return Status::ok();
        } catch (const std::exception& error) {
            return exceptionStatus("XRT device/xclbin/hw_context/kernel initialization", error);
        } catch (...) {
            return unknownExceptionStatus("XRT device/xclbin/hw_context/kernel initialization");
        }
    }

    [[nodiscard]] std::unique_ptr<XrtInstructionBuffer> createInstructionBuffer(
        const std::vector<char>& instructionData,
        const char* description)
    {
        if (instructionData.empty()) {
            std::string message = description;
            message += " is empty";
            throw std::runtime_error(std::move(message));
        }
        if ((instructionData.size() % sizeof(std::uint32_t)) != 0) {
            std::string message = description;
            message += " is not uint32-aligned";
            throw std::runtime_error(std::move(message));
        }

        auto out = std::make_unique<XrtInstructionBuffer>();
        out->bo = xrt::bo{
            hwContext_,
            instructionData.size(),
            xrt::bo::flags::cacheable,
            kernelMemoryGroup(kernel_, config_.kernelAbi.instructionBufferArgIndex)};
        std::memcpy(out->bo.map<void*>(), instructionData.data(), instructionData.size());
        out->bo.sync(XCL_BO_SYNC_BO_TO_DEVICE);
        out->instructionCount = static_cast<std::uint32_t>(instructionData.size() / sizeof(std::uint32_t));
        return out;
    }

    [[nodiscard]] Status retireCompletedRuns()
    {
        for (const auto& state : runs_) {
            if (!state->inFlight) {
                continue;
            }
            try {
                if (state->run.state() < ERT_CMD_STATE_COMPLETED) {
                    continue;
                }
                const auto waitResult = state->run.wait2(std::chrono::milliseconds{1});
                if (waitResult == std::cv_status::timeout) {
                    continue;
                }
                state->inFlight = false;
            } catch (const xrt::run::command_error& error) {
                return Status::runtimeFailure(xrtRunCommandErrorStatus("XRT completed-run retirement", error, state->run.state()));
            } catch (const std::exception& error) {
                return exceptionStatus("XRT completed-run retirement", error);
            } catch (...) {
                return unknownExceptionStatus("XRT completed-run retirement");
            }
        }
        return Status::ok();
    }

    [[nodiscard]] Status acquireRunForSubmission(
        const XrtImportedBuffer* input,
        const XrtImportedBuffer* output,
        XrtRunState*& outState)
    {
        outState = nullptr;
        Status status = retireCompletedRuns();
        if (!status.succeeded()) {
            return status;
        }

        const std::uint64_t key = runKey(input, output);
        std::vector<XrtRunState*>& pool = runPoolByTokenPair_[key];
        for (XrtRunState* state : pool) {
            if (!state->inFlight) {
                outState = state;
                lastRunByTokenPair_[key] = state;
                return Status::ok();
            }
        }

        auto state = std::make_unique<XrtRunState>();
        state->run = xrt::run{kernel_};
        state->input = input;
        state->output = output;
        state->key = key;
        XrtRunState* raw = state.get();
        runs_.push_back(std::move(state));
        pool.push_back(raw);
        lastRunByTokenPair_[key] = raw;
        outState = raw;
        return Status::ok();
    }

    [[nodiscard]] XrtRunState* lastRunForBuffers(const XrtImportedBuffer* input, const XrtImportedBuffer* output) const
    {
        const std::uint64_t key = runKey(input, output);
        const auto found = lastRunByTokenPair_.find(key);
        return found == lastRunByTokenPair_.end() ? nullptr : found->second;
    }

    [[nodiscard]] Status synchronizeCpuDiagnosticIngress(const NpuDispatchDesc& desc, XrtImportedBuffer& input)
    {
        if (desc.payloadContract.ingressProducer != NpuPayloadProducer::CpuDiagnostic) {
            return Status::ok();
        }

        const std::uint64_t byteSize = ingressSyncBytes(desc);
        Status status = validatePayloadRange(desc.input, byteSize, "ingress");
        if (!status.succeeded()) {
            return status;
        }
        if (byteSize == 0) {
            return Status::ok();
        }

        input.bo.sync(
            XCL_BO_SYNC_BO_TO_DEVICE,
            static_cast<size_t>(byteSize),
            static_cast<size_t>(desc.input.byteOffset));
        payloadSyncStats_.toDeviceCount += 1;
        payloadSyncStats_.toDeviceBytes += byteSize;
        return Status::ok();
    }

    [[nodiscard]] Status synchronizeCpuDiagnosticEgressSentinel(const NpuDispatchDesc& desc, XrtImportedBuffer& output)
    {
        if (desc.payloadContract.egressConsumer != NpuPayloadConsumer::CpuDiagnostic) {
            return Status::ok();
        }

        const std::uint64_t byteSize = egressSyncBytes(desc);
        Status status = validatePayloadRange(desc.output, byteSize, "egress");
        if (!status.succeeded()) {
            return status;
        }
        if (byteSize == 0) {
            return Status::ok();
        }

        output.bo.sync(
            XCL_BO_SYNC_BO_TO_DEVICE,
            static_cast<size_t>(byteSize),
            static_cast<size_t>(desc.output.byteOffset));
        payloadSyncStats_.toDeviceCount += 1;
        payloadSyncStats_.toDeviceBytes += byteSize;
        return Status::ok();
    }

    [[nodiscard]] Status synchronizeCpuDiagnosticEgressReadback(const NpuDispatchDesc& desc, XrtImportedBuffer& output)
    {
        if (desc.payloadContract.egressConsumer != NpuPayloadConsumer::CpuDiagnostic) {
            return Status::ok();
        }

        const std::uint64_t byteSize = egressSyncBytes(desc);
        Status status = validatePayloadRange(desc.output, byteSize, "egress");
        if (!status.succeeded()) {
            return status;
        }
        if (byteSize == 0) {
            return Status::ok();
        }

        output.bo.sync(
            XCL_BO_SYNC_BO_FROM_DEVICE,
            static_cast<size_t>(byteSize),
            static_cast<size_t>(desc.output.byteOffset));
        payloadSyncStats_.fromDeviceCount += 1;
        payloadSyncStats_.fromDeviceBytes += byteSize;
        return Status::ok();
    }

    void bindParameterLoadArguments(
        xrt::run& run,
        const XrtImportedBuffer& residentParameterArena,
        const XrtImportedBuffer* skipSpillArena) const
    {
        const AmdXrtKernelAbi& abi = config_.kernelAbi;
        setU32Arg(run, abi.opcodeArgIndex, config_.opcode);
        if (parameterLoadInstructions_ != nullptr) {
            setBoArg(run, abi.instructionBufferArgIndex, parameterLoadInstructions_->bo);
            setU32Arg(run, abi.instructionCountArgIndex, parameterLoadInstructions_->instructionCount);
        }
        setBoArg(run, abi.residentParameterBufferArgIndex, residentParameterArena.bo);
        if (skipSpillArena != nullptr) {
            setBoArg(run, abi.skipSpillBufferArgIndex, skipSpillArena->bo);
        }
    }

    void bindPersistentControllerArguments(
        xrt::run& run,
        const XrtImportedBuffer& controlRing,
        const XrtImportedBuffer* residentParameterArena,
        const XrtImportedBuffer* skipSpillArena,
        const XrtImportedBuffer& ingressArena,
        const XrtImportedBuffer& egressArena) const
    {
        const AmdXrtKernelAbi& abi = config_.kernelAbi;
        setU32Arg(run, abi.opcodeArgIndex, config_.opcode);
        if (instructions_ != nullptr) {
            setBoArg(run, abi.instructionBufferArgIndex, instructions_->bo);
            setU32Arg(run, abi.instructionCountArgIndex, instructions_->instructionCount);
        }
        setBoArg(run, abi.controlRingBufferArgIndex, controlRing.bo);
        if (residentParameterArena != nullptr) {
            setBoArg(run, abi.residentParameterBufferArgIndex, residentParameterArena->bo);
        }
        if (skipSpillArena != nullptr) {
            setBoArg(run, abi.skipSpillBufferArgIndex, skipSpillArena->bo);
        }
        setBoArg(run, abi.inputTokenBufferArgIndex, ingressArena.bo);
        setBoArg(run, abi.outputTokenBufferArgIndex, egressArena.bo);
    }

    void bindRunArguments(
        xrt::run& run,
        const NpuDispatchDesc& desc,
        const XrtImportedBuffer& input,
        const XrtImportedBuffer& output,
        const XrtImportedBuffer* controlRing) const
    {
        const AmdXrtKernelAbi& abi = config_.kernelAbi;
        setU32Arg(run, abi.opcodeArgIndex, config_.opcode);
        if (instructions_ != nullptr) {
            setBoArg(run, abi.instructionBufferArgIndex, instructions_->bo);
            setU32Arg(run, abi.instructionCountArgIndex, instructions_->instructionCount);
        }
        if (controlRing != nullptr) {
            setBoArg(run, abi.controlRingBufferArgIndex, controlRing->bo);
        }
        if (desc.residentParameterArena.nativeBuffer.value != nullptr) {
            const auto* residentParameterArena = static_cast<const XrtImportedBuffer*>(desc.residentParameterArena.nativeBuffer.value);
            setBoArg(run, abi.residentParameterBufferArgIndex, residentParameterArena->bo);
        }
        if (desc.skipSpillArena.nativeBuffer.value != nullptr) {
            const auto* skipSpillArena = static_cast<const XrtImportedBuffer*>(desc.skipSpillArena.nativeBuffer.value);
            setBoArg(run, abi.skipSpillBufferArgIndex, skipSpillArena->bo);
        }
        setBoArg(run, abi.inputTokenBufferArgIndex, input.bo);
        setBoArg(run, abi.outputTokenBufferArgIndex, output.bo);
        setU32Arg(run, abi.bandIndexArgIndex, desc.band.bandIndex);
        setU32Arg(run, abi.bandCountArgIndex, desc.band.bandCount);
        setU32Arg(run, abi.inputFirstRowArgIndex, desc.band.inputFirstRow);
        setU32Arg(run, abi.inputRowCountArgIndex, desc.band.inputRowCount);
        setU32Arg(run, abi.outputFirstRowArgIndex, desc.band.outputFirstRow);
        setU32Arg(run, abi.outputRowCountArgIndex, desc.band.outputRowCount);
        setU32Arg(run, abi.renderWidthArgIndex, desc.renderSize.width);
        setU32Arg(run, abi.renderHeightArgIndex, desc.renderSize.height);
        setU32Arg(run, abi.firstBandFlagArgIndex, desc.band.firstBandInFrame ? 1U : 0U);
        setU32Arg(run, abi.lastBandFlagArgIndex, desc.band.lastBandInFrame ? 1U : 0U);
    }
#endif
};

} // namespace

NpuBackendPtr createAmdXrtNpuBackend(AmdXrtBackendConfig config)
{
    return std::make_shared<AmdXrtNpuBackend>(std::move(config));
}

} // namespace apusr::backends
