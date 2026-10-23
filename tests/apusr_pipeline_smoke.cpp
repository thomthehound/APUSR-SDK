// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/adapters/apusr_ffx_fsr4_adapter.h>
#include <apusr/apusr_pipeline.h>
#include <apusr/kernels/apusr_debug_tile_constants.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>


namespace {

struct TraceBuffer {
    std::unique_ptr<std::uint8_t[]> storage;
    void* alignedPointer = nullptr;
};

struct TraceState {
    std::vector<std::string> events;
    std::vector<TraceBuffer> tokenBuffers;
    std::uintptr_t nextHandle = 1;

    apusr::NativeHandle makeHandle()
    {
        return apusr::NativeHandle{reinterpret_cast<void*>(nextHandle++)};
    }

    apusr::NativeHandle makeBufferHandle(const std::uint64_t byteSize, const std::uint64_t alignment)
    {
        if (byteSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) - alignment) {
            return apusr::NativeHandle{};
        }

        auto storage = std::make_unique<std::uint8_t[]>(static_cast<std::size_t>(byteSize + alignment));
        const auto rawAddress = reinterpret_cast<std::uintptr_t>(storage.get());
        const auto alignedAddress = static_cast<std::uintptr_t>(apusr::alignUp(rawAddress, alignment));
        void* alignedPointer = reinterpret_cast<void*>(alignedAddress);

        tokenBuffers.push_back(TraceBuffer{
            .storage = std::move(storage),
            .alignedPointer = alignedPointer,
        });
        return apusr::NativeHandle{alignedPointer};
    }
};

class TraceGraphicsBackend final : public apusr::GraphicsBackend {
public:
    explicit TraceGraphicsBackend(std::shared_ptr<TraceState> trace) : trace_(std::move(trace)) {}

    [[nodiscard]] apusr::GraphicsApi api() const override { return apusr::GraphicsApi::Dx12; }

    [[nodiscard]] apusr::GraphicsBackendCapabilities capabilities() const override
    {
        return apusr::GraphicsBackendCapabilities{
            .sharedBufferHandleKind = apusr::SharedBufferHandleKind::HostUserPointer,
            .timelineFenceHandleKind = apusr::TimelineFenceHandleKind::Dx12Fence,
            .supportsSharedTokenBuffers = true,
            .supportsExternalTimelineFences = true,
            .supportsQueueSubmit = true,
            .supportsPersistentCommandContexts = true,
        };
    }

    [[nodiscard]] apusr::Status createTokenBuffer(const apusr::SharedBufferDesc& desc, apusr::SharedBuffer& outBuffer) override
    {
        if (desc.byteSize == 0) {
            return apusr::Status::invalidArgument("test token buffer size must be non-zero");
        }
        const std::uint64_t alignedByteSize = apusr::alignUp(desc.byteSize, desc.alignment);
        outBuffer.nativeBuffer = trace_->makeBufferHandle(alignedByteSize, desc.alignment);
        if (outBuffer.nativeBuffer.value == nullptr) {
            return apusr::Status::runtimeFailure("test token buffer allocation overflowed");
        }
        outBuffer.nativeAllocation = trace_->makeHandle();
        outBuffer.handleKind = apusr::SharedBufferHandleKind::HostUserPointer;
        outBuffer.byteSize = alignedByteSize;
        trace_->events.push_back("gfx.create_token_band_buffer_" + std::to_string(outBuffer.byteSize));
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status importTimelineFence(apusr::TimelineFence& fence) override
    {
        if (fence.nativeSharedHandle.value == nullptr || fence.handleKind != apusr::TimelineFenceHandleKind::Dx12Fence) {
            return apusr::Status::runtimeFailure("graphics backend must import an NPU-created DX12 timeline fence");
        }
        fence.nativeGraphicsFence = trace_->makeHandle();
        fence.nativeFence = fence.nativeGraphicsFence;
        trace_->events.push_back("gfx.import_timeline_fence");
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status createCommandContext(const apusr::CommandPhase phase, apusr::CommandContext& outContext) override
    {
        outContext.phase = phase;
        outContext.commandAllocator = trace_->makeHandle();
        outContext.commandList = trace_->makeHandle();
        outContext.commandQueue = trace_->makeHandle();
        trace_->events.push_back(phase == apusr::CommandPhase::ExportTokens ? "gfx.create_export_context" : "gfx.create_import_context");
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status resetCommandContext(apusr::CommandContext& commandContext) override
    {
        trace_->events.push_back(commandContext.phase == apusr::CommandPhase::ExportTokens ? "gfx.reset_export" : "gfx.reset_import");
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status recordExportPass(apusr::CommandContext& commandContext, const apusr::ExportPassDesc& desc) override
    {
        if (commandContext.phase != apusr::CommandPhase::ExportTokens) {
            return apusr::Status::runtimeFailure("export pass recorded into the wrong command context");
        }
        if (desc.destination.byteSize == 0 || desc.destinationBoundary.byteSize == 0 || desc.band.outputRowCount == 0) {
            return apusr::Status::runtimeFailure("export pass did not receive a token-band destination");
        }
        trace_->events.push_back("gfx.record_export_band_" + std::to_string(desc.band.bandIndex));
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status recordImportPass(apusr::CommandContext& commandContext, const apusr::ImportPassDesc& desc) override
    {
        if (commandContext.phase != apusr::CommandPhase::ImportTokens) {
            return apusr::Status::runtimeFailure("import pass recorded into the wrong command context");
        }
        if (desc.source.byteSize == 0 || desc.sourceBoundary.byteSize == 0 || desc.band.outputRowCount == 0) {
            return apusr::Status::runtimeFailure("import pass did not receive a token-band source");
        }
        trace_->events.push_back("gfx.record_import_band_" + std::to_string(desc.band.bandIndex));
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status closeCommandContext(apusr::CommandContext& commandContext) override
    {
        trace_->events.push_back(commandContext.phase == apusr::CommandPhase::ExportTokens ? "gfx.close_export" : "gfx.close_import");
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status submitGpuWork(const apusr::GpuSubmitDesc& desc) override
    {
        if (desc.commandContext.commandList.value == nullptr) {
            if (desc.waits.empty() && desc.signals.size() == 1 && desc.signals.front().value != 0) {
                trace_->events.push_back("gfx.signal_ready_" + std::to_string(desc.signals.front().value));
                return apusr::Status::ok();
            }
            if (!desc.waits.empty() && desc.signals.size() == 1 && desc.signals.front().value != 0) {
                trace_->events.push_back(
                    "gfx.wait_exports_" + std::to_string(desc.waits.size()) +
                    "_signal_ready_" + std::to_string(desc.signals.front().value));
                return apusr::Status::ok();
            }
            if (desc.waits.size() == 1 && desc.signals.empty() && desc.waits.front().value != 0) {
                trace_->events.push_back("gfx.wait_done_" + std::to_string(desc.waits.front().value));
                return apusr::Status::ok();
            }
            return apusr::Status::runtimeFailure("queue-only submit must contain a useful wait or signal");
        }

        if (desc.commandContext.phase == apusr::CommandPhase::ExportTokens) {
            if (desc.signals.size() != 1 || desc.signals.front().value == 0) {
                return apusr::Status::runtimeFailure("export submit must signal the GPU-ready fence");
            }
            if (desc.waits.empty()) {
                trace_->events.push_back("gfx.submit_export_signal_" + std::to_string(desc.signals.front().value));
            } else if (desc.waits.size() == 1) {
                trace_->events.push_back(
                    "gfx.submit_export_wait_retired_" + std::to_string(desc.waits.front().value)
                    + "_signal_" + std::to_string(desc.signals.front().value));
            } else {
                return apusr::Status::runtimeFailure("export submit received an invalid wait set");
            }
            return apusr::Status::ok();
        }

        if (desc.waits.size() != 1 || desc.signals.size() != 1 || desc.waits.front().value == 0 || desc.signals.front().value == 0) {
            return apusr::Status::runtimeFailure("import submit must wait on NPU-done and signal slot-retired");
        }
        trace_->events.push_back(
            "gfx.submit_import_wait_" + std::to_string(desc.waits.front().value)
            + "_signal_retired_" + std::to_string(desc.signals.front().value));
        return apusr::Status::ok();
    }

private:
    std::shared_ptr<TraceState> trace_;
    void* lastInput_ = nullptr;
    void* lastOutput_ = nullptr;
};

class TraceNpuBackend final : public apusr::NpuBackend {
public:
    explicit TraceNpuBackend(std::shared_ptr<TraceState> trace) : trace_(std::move(trace)) {}

    [[nodiscard]] apusr::NpuVendor vendor() const override { return apusr::NpuVendor::AmdXdna; }
    [[nodiscard]] apusr::NpuRuntimeKind runtimeKind() const override { return apusr::NpuRuntimeKind::AmdXrt; }

    [[nodiscard]] apusr::NpuBackendCapabilities capabilities() const override
    {
        return apusr::NpuBackendCapabilities{
            .vendor = apusr::NpuVendor::AmdXdna,
            .runtimeKind = apusr::NpuRuntimeKind::AmdXrt,
            .importedBufferHandleKind = apusr::SharedBufferHandleKind::HostUserPointer,
            .importedFenceHandleKind = apusr::TimelineFenceHandleKind::Dx12Fence,
            .preferredSubmissionGranularity = apusr::NpuSubmissionGranularity::FrameWorklist,
            .persistentExecutionMode = apusr::NpuPersistentExecutionMode::FrameWorklistLowering,
            .persistentDoorbellAbiVersion = apusr::kNpuPersistentDoorbellAbiVersion,
            .supportsSharedBufferImport = true,
            .supportsExternalTimelineFenceWait = true,
            .supportsExternalTimelineFenceSignal = true,
            .supportsPersistentDispatchObjects = true,
            .supportsQuiescentDrain = true,
            .supportsFrameWorklists = true,
        };
    }

    [[nodiscard]] apusr::Status createTimelineFence(const std::uint64_t initialValue, apusr::TimelineFence& outFence) override
    {
        outFence.nativeNpuFence = trace_->makeHandle();
        outFence.nativeSharedHandle = trace_->makeHandle();
        outFence.nativeFence = outFence.nativeNpuFence;
        outFence.handleKind = apusr::TimelineFenceHandleKind::Dx12Fence;
        outFence.value = initialValue;
        trace_->events.push_back("npu.create_timeline_fence");
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status importSharedBuffer(
        const apusr::SharedBuffer& graphicsBuffer,
        const apusr::NpuTokenBufferRole role,
        apusr::NpuBufferView& outView) override
    {
        if (graphicsBuffer.byteSize == 0) {
            return apusr::Status::invalidArgument("cannot import an empty token buffer");
        }
        if ((reinterpret_cast<std::uintptr_t>(graphicsBuffer.nativeBuffer.value) % apusr::kHostUserptrSharedBufferAlignmentBytes) != 0) {
            return apusr::Status::invalidArgument("test NPU token buffer import requires the shared USERPTR alignment contract");
        }
        if (role != apusr::NpuTokenBufferRole::Ingress && role != apusr::NpuTokenBufferRole::Egress) {
            return apusr::Status::invalidArgument("unknown token buffer role");
        }
        outView.sharedBuffer = graphicsBuffer;
        outView.nativeBuffer = trace_->makeHandle();
        outView.byteOffset = 0;
        outView.byteSize = graphicsBuffer.byteSize;
        trace_->events.push_back("npu.import_shared_token_band_buffer");
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status importControlBuffer(
        const apusr::SharedBuffer& graphicsBuffer,
        const apusr::NpuControlBufferRole role,
        apusr::NpuBufferView& outView) override
    {
        const bool doorbellRole = role == apusr::NpuControlBufferRole::DoorbellRing;
        const bool residentParameterRole = role == apusr::NpuControlBufferRole::ResidentParameterArena;
        const bool skipSpillRole = role == apusr::NpuControlBufferRole::SkipSpillArena;
        if (!doorbellRole && !residentParameterRole && !skipSpillRole) {
            return apusr::Status::invalidArgument("test NPU received an unknown control-buffer role");
        }
        if (graphicsBuffer.nativeBuffer.value == nullptr) {
            return apusr::Status::invalidArgument("test NPU requires a mapped control buffer");
        }
        if ((reinterpret_cast<std::uintptr_t>(graphicsBuffer.nativeBuffer.value) % apusr::kHostUserptrSharedBufferAlignmentBytes) != 0) {
            return apusr::Status::invalidArgument("test NPU control-buffer import requires the shared USERPTR alignment contract");
        }
        outView = apusr::NpuBufferView{
            .sharedBuffer = graphicsBuffer,
            .nativeBuffer = apusr::NativeHandle{},
            .byteOffset = 0,
            .byteSize = graphicsBuffer.byteSize,
        };
        const char* event = doorbellRole ?
            "npu.import_doorbell_control_ring" :
            (residentParameterRole ? "npu.import_resident_parameter_arena" : "npu.import_skip_spill_arena");
        trace_->events.push_back(event);
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status loadModelParameters(const apusr::NpuModelEpochParameterLoadDesc& desc) override
    {
        if (desc.hotPathParameterDmaBytesPerRun != 0) {
            return apusr::Status::runtimeFailure("resident middle-core hot path must stay parameter-DMA-free");
        }
        if (desc.residentParameterArena.sharedBuffer.nativeBuffer.value == nullptr ||
            desc.residentParameterArenaBytes == 0 || desc.modelEpochParameterDmaBytes == 0) {
            return apusr::Status::runtimeFailure("model-epoch parameter load requires a mapped resident arena");
        }
        if (desc.modelEpoch == 0) {
            return apusr::Status::runtimeFailure("model-epoch parameter load requires a non-zero epoch");
        }
        if (desc.modelEpochParameterDmaBytes > desc.residentParameterArenaBytes) {
            return apusr::Status::runtimeFailure("model-epoch parameter load exceeds resident arena size");
        }
        if (desc.skipSpillArenaBytes != 0 &&
            (desc.skipSpillArena.sharedBuffer.nativeBuffer.value == nullptr || desc.skipSpillArena.byteSize < desc.skipSpillArenaBytes)) {
            return apusr::Status::runtimeFailure("model-epoch parameter load requires the persistent SkipA/SkipB arena");
        }
        if (desc.skipSpillArena.sharedBuffer.nativeBuffer.value != nullptr &&
            desc.skipSpillArena.sharedBuffer.nativeBuffer.value == desc.residentParameterArena.sharedBuffer.nativeBuffer.value) {
            return apusr::Status::runtimeFailure("skip arena must not alias the resident parameter arena");
        }
        modelParametersLoaded_ = true;
        loadedModelEpoch_ = desc.modelEpoch;
        stats_.modelEpochParameterLoadCount += 1;
        stats_.modelEpochParameterDmaBytes += desc.modelEpochParameterDmaBytes;
        stats_.residentParameterArenaBytes = desc.residentParameterArenaBytes;
        stats_.skipSpillArenaBytes = desc.skipSpillArenaBytes;
        stats_.hotPathParameterDmaBytesPerRun = desc.hotPathParameterDmaBytesPerRun;
        stats_.residentModelEpoch = desc.modelEpoch;
        trace_->events.push_back("npu.load_model_epoch_parameters");
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::NpuSubmissionStats submissionStats() const override
    {
        return stats_;
    }

    [[nodiscard]] apusr::Status drainSubmittedWork(const std::uint32_t timeoutMilliseconds) override
    {
        (void)timeoutMilliseconds;
        stats_.modelEpochDrainCount += 1;
        trace_->events.push_back("npu.drain_submitted_work");
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status enqueue(const apusr::NpuDispatchDesc& desc) override
    {
        if (desc.waitBeforeRun.value == 0 || desc.signalAfterRun.value == 0) {
            return apusr::Status::runtimeFailure("NPU dispatch must carry concrete fence values");
        }
        if (desc.input.byteSize == 0 || desc.output.byteSize == 0 || desc.band.outputRowCount == 0) {
            return apusr::Status::runtimeFailure("NPU dispatch must use imported token-band views");
        }
        if (desc.residentParameterArena.sharedBuffer.nativeBuffer.value != nullptr && !modelParametersLoaded_) {
            return apusr::Status::runtimeFailure("resident-parameter dispatch ran before model-epoch parameter load");
        }
        if (desc.residentParameterArena.sharedBuffer.nativeBuffer.value != nullptr && desc.modelEpoch != loadedModelEpoch_) {
            return apusr::Status::runtimeFailure("resident-parameter dispatch used the wrong model epoch");
        }
        if (desc.skipSpillArena.sharedBuffer.nativeBuffer.value != nullptr) {
            if (desc.skipSpillArena.byteSize == 0) {
                return apusr::Status::runtimeFailure("skip arena dispatch view must have a non-zero size");
            }
            if (desc.skipSpillArena.sharedBuffer.nativeBuffer.value == desc.input.sharedBuffer.nativeBuffer.value ||
                desc.skipSpillArena.sharedBuffer.nativeBuffer.value == desc.output.sharedBuffer.nativeBuffer.value ||
                desc.skipSpillArena.sharedBuffer.nativeBuffer.value == desc.residentParameterArena.sharedBuffer.nativeBuffer.value) {
                return apusr::Status::runtimeFailure("skip arena dispatch view must not alias payload or resident buffers");
            }
        }

        if (desc.payloadContract.ingressProducer != apusr::NpuPayloadProducer::GpuQueue ||
            desc.payloadContract.egressConsumer != apusr::NpuPayloadConsumer::GpuQueue) {
            return apusr::Status::runtimeFailure("product NPU dispatch must declare queue-owned payloads");
        }
        if (desc.band.bandCount != 1 || !desc.band.firstBandInFrame || !desc.band.lastBandInFrame) {
            return apusr::Status::runtimeFailure("frame conveyor dispatch must be one host-visible frame work item");
        }
        if (desc.payloadContract.ingressByteSize < desc.band.inputByteSize ||
            desc.payloadContract.egressByteSize < desc.band.outputByteSize) {
            return apusr::Status::runtimeFailure("frame conveyor dispatch must cover one full-frame transport slot");
        }

        trace_->events.push_back(
            "npu.enqueue_band_" + std::to_string(desc.band.bandIndex)
            + "_wait_" + std::to_string(desc.waitBeforeRun.value)
            + "_signal_" + std::to_string(desc.signalAfterRun.value));
        lastInput_ = desc.input.sharedBuffer.nativeBuffer.value;
        lastOutput_ = desc.output.sharedBuffer.nativeBuffer.value;
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status enqueueFrame(const apusr::NpuFrameDispatchDesc& desc) override
    {
        if (desc.dispatchCount == 0 || desc.dispatches == nullptr ||
            desc.doorbellRingHeader == nullptr || desc.doorbellDescriptors == nullptr) {
            return apusr::Status::runtimeFailure("test NPU frame worklist requires a staged doorbell ring range");
        }
        if (desc.doorbellRingHeader->descriptorCapacity == 0 ||
            desc.doorbellRingHeader->executionMode != static_cast<std::uint32_t>(apusr::NpuPersistentExecutionMode::FrameWorklistLowering) ||
            desc.doorbellRingCapacity != desc.doorbellRingHeader->descriptorCapacity ||
            desc.doorbellStartIndex >= desc.doorbellRingCapacity ||
            desc.dispatchCount > (desc.doorbellRingCapacity - desc.doorbellStartIndex)) {
            return apusr::Status::runtimeFailure("test NPU frame worklist has an invalid staged doorbell range");
        }
        if (desc.doorbellControlRing.sharedBuffer.nativeBuffer.value == nullptr ||
            desc.doorbellControlRing.byteSize < desc.doorbellRingHeader->descriptorOffsetBytes) {
            return apusr::Status::runtimeFailure("test NPU frame worklist requires a serialized doorbell control-ring buffer");
        }
        if (desc.residentParameterArena.sharedBuffer.nativeBuffer.value != nullptr && desc.modelEpoch != loadedModelEpoch_) {
            return apusr::Status::runtimeFailure("test NPU frame worklist used the wrong model epoch");
        }
        if (desc.doorbellRingHeader->reconfigureEpoch != desc.modelEpoch) {
            return apusr::Status::runtimeFailure("test NPU frame worklist did not publish the resident model epoch");
        }
        const auto* controlBytes = static_cast<const std::uint8_t*>(desc.doorbellControlRing.sharedBuffer.nativeBuffer.value);
        if (std::memcmp(controlBytes, desc.doorbellRingHeader, sizeof(apusr::NpuDoorbellRingHeader)) != 0) {
            return apusr::Status::runtimeFailure("test NPU frame worklist control-ring header bytes are stale");
        }
        if (desc.doorbellDescriptorByteOffset !=
            apusr::kNpuPersistentDoorbellDescriptorOffset +
                (static_cast<std::uint64_t>(desc.doorbellStartIndex) * apusr::kNpuPersistentDoorbellDescriptorStride)) {
            return apusr::Status::runtimeFailure("test NPU frame worklist has an invalid descriptor byte offset");
        }
        trace_->events.push_back(
            "npu.enqueue_frame_" + std::to_string(desc.frameIndex) +
            "_dispatches_" + std::to_string(desc.dispatchCount));
        for (std::uint32_t dispatchIndex = 0; dispatchIndex < desc.dispatchCount; ++dispatchIndex) {
            const apusr::NpuDispatchDesc& dispatch = desc.dispatches[dispatchIndex];
            const apusr::NpuDoorbellTokenBandDesc& doorbell = desc.doorbellDescriptors[dispatchIndex];
            if (dispatch.modelEpoch != desc.modelEpoch) {
                return apusr::Status::runtimeFailure("test NPU dispatch did not carry the resident model epoch");
            }
            if (std::memcmp(
                    controlBytes + desc.doorbellDescriptorByteOffset +
                        (static_cast<std::uint64_t>(dispatchIndex) * apusr::kNpuPersistentDoorbellDescriptorStride),
                    &doorbell,
                    sizeof(doorbell)) != 0) {
                return apusr::Status::runtimeFailure("test NPU frame worklist control-ring descriptor bytes are stale");
            }
            if (doorbell.kind != static_cast<std::uint32_t>(apusr::NpuDoorbellDescriptorKind::TokenBand) ||
                doorbell.abiVersion != apusr::kNpuPersistentDoorbellAbiVersion ||
                doorbell.byteSize != apusr::kNpuPersistentDoorbellDescriptorStride ||
                doorbell.sequence != desc.doorbellStartSequence + dispatchIndex ||
                doorbell.frameIndex != desc.frameIndex ||
                doorbell.tokenSlotIndex != dispatch.tokenSlotIndex ||
                doorbell.bandIndex != dispatch.band.bandIndex ||
                doorbell.bandCount != dispatch.band.bandCount ||
                doorbell.inputByteSize != dispatch.payloadContract.ingressByteSize ||
                doorbell.outputByteSize != dispatch.payloadContract.egressByteSize) {
                return apusr::Status::runtimeFailure("test NPU frame worklist doorbell descriptor does not match dispatch");
            }
            apusr::Status status = enqueue(dispatch);
            if (!status.succeeded()) {
                return status;
            }
        }
        return apusr::Status::ok();
    }

    [[nodiscard]] apusr::Status waitForLastDispatch(const apusr::NpuDispatchDesc& desc, const std::uint32_t timeoutMilliseconds) override
    {
        (void)desc;
        (void)timeoutMilliseconds;
        auto* input = static_cast<std::uint8_t*>(lastInput_);
        auto* output = static_cast<std::uint8_t*>(lastOutput_);
        if (input == nullptr || output == nullptr) {
            return apusr::Status::runtimeFailure("test NPU wait requires a prior enqueue");
        }
        for (std::uint32_t y = 0; y < apusr::kernels::debug_tile::kDefaultMicroTileOutputRows; ++y) {
            for (std::uint32_t x = 0; x < apusr::kernels::debug_tile::kDefaultMicroTileOutputWidth; ++x) {
                for (std::uint32_t channel = 0; channel < apusr::kernels::debug_tile::kModelChannels; ++channel) {
                    std::uint32_t sum = 0u;
                    for (std::uint32_t kernelY = 0; kernelY < 3u; ++kernelY) {
                        for (std::uint32_t kernelX = 0; kernelX < 3u; ++kernelX) {
                            const std::uint32_t inputIndex =
                                (((y + kernelY) * apusr::kernels::debug_tile::kWorstCaseInputWindowWidth +
                                     (x + kernelX)) *
                                    apusr::kernels::debug_tile::kModelChannels) +
                                channel;
                            sum += input[inputIndex];
                        }
                    }
                    const std::uint32_t outputIndex =
                        ((y * apusr::kernels::debug_tile::kDefaultMicroTileOutputWidth + x) *
                            apusr::kernels::debug_tile::kModelChannels) +
                        channel;
                    output[outputIndex] = static_cast<std::uint8_t>(sum);
                }
            }
        }
        trace_->events.push_back("npu.wait_for_last_dispatch");
        return apusr::Status::ok();
    }

private:
    std::shared_ptr<TraceState> trace_;
    apusr::NpuSubmissionStats stats_;
    bool modelParametersLoaded_ = false;
    std::uint64_t loadedModelEpoch_ = 0;
    void* lastInput_ = nullptr;
    void* lastOutput_ = nullptr;
};

bool containsOrdered(const std::vector<std::string>& events, const std::vector<std::string>& expected)
{
    std::size_t cursor = 0;
    for (const std::string& event : events) {
        if (cursor < expected.size() && event == expected[cursor]) {
            ++cursor;
        }
    }
    return cursor == expected.size();
}

apusr::SurfaceDesc makeSurface(
    apusr::SurfaceRole role,
    apusr::NativeHandle handle,
    apusr::SurfaceFormat format,
    const apusr::RenderSize size)
{
    return apusr::SurfaceDesc{
        .role = role,
        .resource = handle,
        .format = format,
        .size = size,
    };
}

} // namespace


apusr::PipelineFrameDesc makeFrame(TraceState& trace, const std::uint64_t frameIndex)
{
    const apusr::RenderSize renderSize{128, 16};
    const apusr::DisplaySize displaySize{256, 32};

    return apusr::PipelineFrameDesc{
        .frameIndex = frameIndex,
        .renderSize = renderSize,
        .displaySize = displaySize,
        .surfaces = apusr::FrameSurfaces{
            .color = makeSurface(apusr::SurfaceRole::Color, trace.makeHandle(), apusr::SurfaceFormat::Rgba16Float, renderSize),
            .motionVectors = makeSurface(apusr::SurfaceRole::MotionVectors, trace.makeHandle(), apusr::SurfaceFormat::Rgba16Float, renderSize),
            .output = makeSurface(apusr::SurfaceRole::Output, trace.makeHandle(), apusr::SurfaceFormat::Rgba16Float, apusr::RenderSize{displaySize.width, displaySize.height}),
        },
        .borrowedCommandList = trace.makeHandle(),
    };
}

int main()
{
    auto trace = std::make_shared<TraceState>();
    auto graphics = std::make_shared<TraceGraphicsBackend>(trace);
    auto npu = std::make_shared<TraceNpuBackend>(trace);

    const apusr::RenderSize renderSize{128, 16};
    apusr::adapters::Fsr4AdapterConfig adapterConfig{};
    adapterConfig.maxRenderSize = renderSize;
    adapterConfig.maxActiveOutputSize = apusr::DisplaySize{256, 32};
    adapterConfig.tokenRingDepth = 3;
    adapterConfig.tokenBandRows = 8;

    std::array<std::uint8_t, 4096> residentParameters{};
    for (std::size_t byteIndex = 0; byteIndex < residentParameters.size(); ++byteIndex) {
        residentParameters[byteIndex] = static_cast<std::uint8_t>(byteIndex & 0xffu);
    }

    constexpr std::uint64_t skipSpillArenaBytes = apusr::kHostUserptrSharedBufferAlignmentBytes * 3u;

    apusr::PipelineSession pipeline;
    apusr::RuntimeConfig runtimeConfig = apusr::adapters::makeFsr4RuntimeConfig(adapterConfig, graphics, npu);
    runtimeConfig.residentParameterArenaData = residentParameters.data();
    runtimeConfig.residentParameterArenaBytes = residentParameters.size();
    runtimeConfig.modelEpochParameterDmaBytes = 1024;
    runtimeConfig.hotPathParameterDmaBytesPerRun = 0;
    runtimeConfig.skipSpillArenaBytes = skipSpillArenaBytes;
    runtimeConfig.modelEpoch = 42;
    const apusr::Status configureStatus = pipeline.configure(std::move(runtimeConfig));
    if (!configureStatus.succeeded()) {
        std::cerr << "configure failed: " << configureStatus.message << '\n';
        return 1;
    }

    const apusr::NativeBringupStage nativeStage = apusr::compiledNativeBringupStage();
    if (!apusr::productNativeStage(nativeStage)) {
        std::cerr << "pipeline smoke requires the product frame-conveyor runtime\n";
        return 1;
    }
    const bool selectedKernelImportsVisibleEgress = apusr::packagedNativeKernelProducesModelEgressFrame(nativeStage);
    for (std::uint64_t frameIndex = 0; frameIndex < 3; ++frameIndex) {
        const apusr::Status frameStatus = pipeline.submitFrame(makeFrame(*trace, frameIndex));
        if (!frameStatus.succeeded()) {
            std::cerr << "submitFrame failed: " << frameStatus.message << '\n';
            return 1;
        }
        const bool expectNativeOutput = selectedKernelImportsVisibleEgress && frameIndex > 0;
        if (pipeline.lastSubmitProducedOutput() != expectNativeOutput) {
            std::cerr << "native output production state did not match the product frame-conveyor contract\n";
            return 1;
        }
    }

    const std::vector<std::string> expected = selectedKernelImportsVisibleEgress ?
        std::vector<std::string>{
            "gfx.record_export_band_0",
            "npu.enqueue_frame_0_dispatches_1",
            "npu.enqueue_band_0_wait_1_signal_1",
            "gfx.submit_export_signal_1",
            "gfx.record_import_band_0",
            "gfx.submit_import_wait_1_signal_retired_1",
            "gfx.record_export_band_0",
            "npu.enqueue_frame_1_dispatches_1",
            "npu.enqueue_band_0_wait_1_signal_1",
            "gfx.submit_export_signal_1",
            "gfx.record_import_band_0",
            "gfx.submit_import_wait_1_signal_retired_1",
            "gfx.record_export_band_0",
            "npu.enqueue_frame_2_dispatches_1",
            "npu.enqueue_band_0_wait_1_signal_1",
            "gfx.submit_export_signal_1",
        } :
        std::vector<std::string>{
            "gfx.record_export_band_0",
            "npu.enqueue_frame_0_dispatches_1",
            "npu.enqueue_band_0_wait_1_signal_1",
            "gfx.submit_export_signal_1",
            "gfx.record_export_band_0",
            "npu.enqueue_frame_1_dispatches_1",
            "npu.enqueue_band_0_wait_1_signal_1",
            "gfx.submit_export_signal_1",
            "gfx.record_export_band_0",
            "npu.enqueue_frame_2_dispatches_1",
            "npu.enqueue_band_0_wait_1_signal_1",
            "gfx.submit_export_signal_1",
        };

    if (!containsOrdered(trace->events, expected)) {
        std::cerr << "pipeline did not follow the expected frame-conveyor slot reuse order\n";
        for (const std::string& event : trace->events) {
            std::cerr << "  " << event << '\n';
        }
        return 1;
    }

    apusr::NpuSubmissionStats stats = pipeline.npuSubmissionStats();
    if (stats.modelEpochParameterLoadCount != 1 ||
        stats.modelEpochParameterDmaBytes != 1024 ||
        stats.residentParameterArenaBytes != residentParameters.size() ||
        stats.skipSpillArenaBytes != skipSpillArenaBytes ||
        stats.hotPathParameterDmaBytesPerRun != 0 ||
        stats.residentModelEpoch != 42) {
        std::cerr << "pipeline did not load resident parameters once at model epoch configure time\n";
        return 1;
    }

    std::array<std::uint8_t, 4096> nextResidentParameters{};
    for (std::size_t byteIndex = 0; byteIndex < nextResidentParameters.size(); ++byteIndex) {
        nextResidentParameters[byteIndex] = static_cast<std::uint8_t>((byteIndex + 17u) & 0xffu);
    }

    const apusr::Status switchStatus = pipeline.switchModelEpochQuiescent(apusr::RuntimeModelEpochSwitchDesc{
        .residentParameterArenaData = nextResidentParameters.data(),
        .residentParameterArenaBytes = nextResidentParameters.size(),
        .modelEpochParameterDmaBytes = 2048,
        .hotPathParameterDmaBytesPerRun = 0,
        .modelEpoch = 43,
        .drainTimeoutMilliseconds = 1,
        .mode = apusr::RuntimeModelEpochSwitchMode::DrainAndResetOutputHistory,
    });
    if (!switchStatus.succeeded()) {
        std::cerr << "model-epoch switch failed: " << switchStatus.message << '\n';
        return 1;
    }

    const apusr::Status postSwitchFrameStatus = pipeline.submitFrame(makeFrame(*trace, 3));
    if (!postSwitchFrameStatus.succeeded()) {
        std::cerr << "post-switch submitFrame failed: " << postSwitchFrameStatus.message << '\n';
        return 1;
    }
    if (selectedKernelImportsVisibleEgress && pipeline.lastSubmitProducedOutput()) {
        std::cerr << "model-epoch switch imported old-epoch output instead of resetting history\n";
        return 1;
    }

    stats = pipeline.npuSubmissionStats();
    if (stats.modelEpochDrainCount != 1 ||
        stats.modelEpochParameterLoadCount != 2 ||
        stats.modelEpochParameterDmaBytes != 3072 ||
        stats.residentParameterArenaBytes != nextResidentParameters.size() ||
        stats.skipSpillArenaBytes != skipSpillArenaBytes ||
        stats.hotPathParameterDmaBytesPerRun != 0 ||
        stats.residentModelEpoch != 43) {
        std::cerr << "pipeline did not load the second resident epoch through the quiescent switch path\n";
        return 1;
    }

    if (!containsOrdered(trace->events, std::vector<std::string>{
            "npu.drain_submitted_work",
            "npu.load_model_epoch_parameters",
            "npu.enqueue_frame_3_dispatches_1",
        })) {
        std::cerr << "model-epoch switch did not drain before loading the replacement parameters\n";
        return 1;
    }

    if (stats.doorbellRing.reserveCount != 4 ||
        stats.doorbellRing.commitCount != 4 ||
        stats.doorbellRing.stagedDescriptorCount != 4 ||
        stats.doorbellRing.occupancyHighWatermark != 1 ||
        stats.doorbellRing.controlRingWriteCount != 12 ||
        stats.doorbellRing.controlRingWriteBytes != 2048 ||
        stats.doorbellRing.publishedSequence != 4 ||
        stats.doorbellRing.retiredSequence != 4 ||
        stats.doorbellRing.completedSequence != 0) {
        std::cerr << "pipeline did not stage one product frame-conveyor work item per frame\n";
        return 1;
    }

    std::cout << (selectedKernelImportsVisibleEgress
        ? "APUSR pipeline smoke: product frame-conveyor export/NPU/import path validated\n"
        : "APUSR pipeline smoke: product frame-conveyor export/enqueue path validated\n");
    return 0;
}
