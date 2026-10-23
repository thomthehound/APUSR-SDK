// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/apusr_runtime.h>
#include <apusr/apusr_profiler.h>
#include <apusr/kernels/apusr_frame_conveyor_abi.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace apusr {

namespace {

apusr::profile::StreamHandle runtimeProfileStream(const GraphicsApi api) noexcept
{
    if (api == GraphicsApi::Vulkan) {
        static const apusr::profile::StreamHandle vulkan = apusr::profile::registerStream({
            .backend = apusr::profile::BackendApi::Vulkan,
            .component = apusr::profile::Component::Core,
            .name = "runtime.frame",
            .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                apusr::profile::StreamCapabilityFrames |
                apusr::profile::StreamCapabilityInstants |
                apusr::profile::StreamCapabilityStateTransitions |
                apusr::profile::StreamCapabilityFlows,
        });
        return vulkan;
    }
    static const apusr::profile::StreamHandle dx12 = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::Dx12,
        .component = apusr::profile::Component::Core,
        .name = "runtime.frame",
        .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
            apusr::profile::StreamCapabilityFrames |
            apusr::profile::StreamCapabilityInstants |
            apusr::profile::StreamCapabilityStateTransitions |
            apusr::profile::StreamCapabilityFlows,
    });
    return dx12;
}

apusr::profile::StreamHandle runtimeFallbackProfileStream() noexcept
{
    static const apusr::profile::StreamHandle stream = apusr::profile::registerStream({
        .backend = apusr::profile::BackendApi::None,
        .component = apusr::profile::Component::Core,
        .name = "runtime.frame",
        .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
            apusr::profile::StreamCapabilityFrames |
            apusr::profile::StreamCapabilityInstants |
            apusr::profile::StreamCapabilityStateTransitions |
            apusr::profile::StreamCapabilityFlows,
    });
    return stream;
}

} // namespace


[[nodiscard]] bool persistentControlRingExecution(const RuntimeConfig& config)
{
    return config.npuBackend != nullptr &&
        config.npuBackend->capabilities().persistentExecutionMode == NpuPersistentExecutionMode::PersistentControlRing;
}

[[nodiscard]] constexpr bool frameCommandConveyorStage(const NativeBringupStage stage)
{
    return productNativeStage(stage);
}

Status recordTokenBandExport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band);

Status recordFrameExport(
    RuntimeConfig& config,
    TokenStreamSlot& framePayloadSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment);

Status submitTokenBandExport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const TimelineFenceWait* reuseWait);

Status submitFrameExport(
    RuntimeConfig& config,
    TokenStreamSlot& framePayloadSlot,
    TokenStreamSlot& frameSyncSlot,
    const TimelineFenceWait* reuseWait);

Status scheduleTokenBandExport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band,
    const TimelineFenceWait* reuseWait);

Status scheduleTokenBandBorrowedExport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band);

Status scheduleTokenBandNpu(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band);

Status enqueueFrameNpuWorklist(
    RuntimeConfig& config,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const std::vector<TokenBandDesc>& bands,
    const std::vector<TokenStreamSlot*>& slots,
    std::vector<NpuDispatchDesc>& dispatchScratch,
    NpuDoorbellRingHeader& doorbellRingHeader,
    std::vector<NpuDoorbellTokenBandDesc>& doorbellRingDescriptors,
    NpuBufferView& doorbellControlRing,
    const NpuBufferView& ingressPayloadArena,
    const NpuBufferView& egressPayloadArena,
    const NpuBufferView& residentParameterArena,
    const NpuBufferView& skipSpillArena,
    NpuDoorbellRingStats& doorbellRingStats);


Status enqueueFrameCommandConveyor(
    RuntimeConfig& config,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    TokenStreamSlot& framePayloadSlot,
    TokenStreamSlot& frameSyncSlot,
    std::vector<NpuDispatchDesc>& dispatchScratch,
    NpuDoorbellRingHeader& doorbellRingHeader,
    std::vector<NpuDoorbellTokenBandDesc>& doorbellRingDescriptors,
    NpuBufferView& doorbellControlRing,
    const NpuBufferView& ingressPayloadArena,
    const NpuBufferView& egressPayloadArena,
    const NpuBufferView& residentParameterArena,
    const NpuBufferView& skipSpillArena,
    NpuDoorbellRingStats& doorbellRingStats);

Status submitFrameReadyAfterExports(
    RuntimeConfig& config,
    const std::vector<TokenStreamSlot*>& slots,
    TokenStreamSlot& frameSyncSlot);

Status scheduleTokenBandImportAfterFrameNpuDone(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    TokenStreamSlot& frameSyncSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band);

Status scheduleFrameImportAfterFrameNpuDone(
    RuntimeConfig& config,
    TokenStreamSlot& framePayloadSlot,
    TokenStreamSlot& frameSyncSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment);

Status scheduleTokenBandExportAndNpu(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band);

Status scheduleTokenBandFenceHandoffProbe(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band);

Status scheduleKernelTileProbe(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band,
    DiagnosticTileOverlayDesc* overlay);

Status scheduleKernelFrameOverlay(
    RuntimeConfig& config,
    TokenStreamSlot& scratchSlot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band,
    DiagnosticTileOverlayDesc& overlay);

Status scheduleTokenBandImport(
    RuntimeConfig& config,
    TokenStreamSlot& slot,
    const FrameDesc& frame,
    const SegmentManifest& segment,
    const TokenBandDesc& band);

namespace {

Status validateRequiredSurface(const SurfaceDesc& surface, const char* name)
{
    if (surface.resource.value == nullptr) {
        return Status::invalidArgument(std::string("frame ") + name + " surface requires a native resource handle");
    }
    if (surface.format == SurfaceFormat::Unknown) {
        return Status::invalidArgument(std::string("frame ") + name + " surface requires a known format");
    }
    if (surface.size.width == 0 || surface.size.height == 0) {
        return Status::invalidArgument(std::string("frame ") + name + " surface requires a non-zero size");
    }
    return Status::ok();
}

Status validateFrame(const FrameDesc& frame)
{
    if (frame.renderSize.width == 0 || frame.renderSize.height == 0) {
        return Status::invalidArgument("frame render size must be non-zero");
    }
    if (frame.displaySize.width == 0 || frame.displaySize.height == 0) {
        return Status::invalidArgument("frame display size must be non-zero");
    }

    Status status = validateRequiredSurface(frame.surfaces.color, "color");
    if (!status.succeeded()) {
        return status;
    }
    status = validateRequiredSurface(frame.surfaces.motionVectors, "motion-vector");
    if (!status.succeeded()) {
        return status;
    }
    return validateRequiredSurface(frame.surfaces.output, "output");
}

[[nodiscard]] bool asyncFrameCompatible(const FrameDesc& pending, const FrameDesc& current)
{
    // Async import consumes frame-owned render-size sidecars from the pending
    // frame and only borrows the current frame's output surface. DRS render
    // sizes may change between frames; dropping those imports would starve
    // output under real dynamic-resolution workloads. Display-size changes
    // still invalidate the pending import because the output/history domain
    // changed.
    return pending.displaySize.width == current.displaySize.width &&
        pending.displaySize.height == current.displaySize.height;
}

[[nodiscard]] FrameDesc makeAsyncImportFrame(const FrameDesc& pending, const FrameDesc& current)
{
    FrameDesc importFrame = pending;
    importFrame.surfaces.output = current.surfaces.output;
    return importFrame;
}

[[nodiscard]] std::uint64_t frameIngressBytes(const SegmentManifest& segment)
{
    return tokenSlabBytes(segment.inputBoundary.shape);
}

[[nodiscard]] TokenShape zeroGuardedIngressShape(TokenShape shape)
{
    shape.layout = TokenLayout::ZeroGuardedRasterNhwcC16;
    return shape;
}

[[nodiscard]] std::uint64_t frameZeroGuardedIngressBytes(const SegmentManifest& segment)
{
    return tokenSlabBytes(zeroGuardedIngressShape(segment.inputBoundary.shape));
}

[[nodiscard]] std::uint64_t frameEgressBytes(const SegmentManifest& segment)
{
    return tokenSlabBytes(segment.outputBoundary.shape);
}

[[nodiscard]] std::uint64_t frameTransportBytes(const std::uint64_t semanticBytes)
{
    return alignUp(semanticBytes, kernels::frame_conveyor::kChunkBytes);
}


[[nodiscard]] std::uint64_t frameEgressTransportBytes(const SegmentManifest& segment)
{
    return frameTransportBytes(frameEgressBytes(segment));
}

[[nodiscard]] bool visibleNpuEgressImportEnabled(const RuntimeConfig& config)
{
    return config.npuProducesModelEgressFrame;
}

[[nodiscard]] RuntimeModelEpochSwitchDesc makeInitialModelEpochDesc(const RuntimeConfig& config)
{
    return RuntimeModelEpochSwitchDesc{
        .residentParameterArenaData = config.residentParameterArenaData,
        .residentParameterArenaBytes = config.residentParameterArenaBytes,
        .modelEpochParameterDmaBytes = config.modelEpochParameterDmaBytes,
        .hotPathParameterDmaBytesPerRun = config.hotPathParameterDmaBytesPerRun,
        .modelEpoch = config.modelEpoch,
        .drainTimeoutMilliseconds = 0,
        .mode = RuntimeModelEpochSwitchMode::DrainAndResetOutputHistory,
    };
}

Status validateFrameConveyorContract(const SegmentManifest& segment)
{
    if (APUSR_PACKAGED_KERNEL_PROFILE != 4) {
        return Status::ok();
    }

    const std::uint64_t ingressBytes = frameIngressBytes(segment);
    const std::uint64_t egressBytes = frameEgressBytes(segment);
    if (ingressBytes == 0 || egressBytes == 0) {
        return Status::invalidArgument("FrameConveyor requires non-empty C16 seam payloads");
    }
    if (ingressBytes != egressBytes) {
        return Status::invalidArgument("FrameConveyor requires equal ingress and egress semantic seam bytes");
    }

    const std::uint64_t transportBytes = frameTransportBytes(ingressBytes);
    if (transportBytes != frameEgressTransportBytes(segment)) {
        return Status::invalidArgument("FrameConveyor requires equal ingress and egress transport capacities");
    }
    if (transportBytes == 0 || transportBytes < ingressBytes || (transportBytes % kernels::frame_conveyor::kChunkBytes) != 0) {
        return Status::invalidArgument("FrameConveyor transport capacity must be a non-empty 4096-byte chunk multiple");
    }
    if (ingressBytes != static_cast<std::uint64_t>(kernels::frame_conveyor::kConfiguredFrameBytes) ||
        transportBytes != static_cast<std::uint64_t>(kernels::frame_conveyor::kConfiguredTransportBytes)) {
        return Status::invalidArgument(
            "FrameConveyor kernel byte contract must match the runtime C16 seam. Reconfigure APUSR_FRAME_CONVEYOR_FRAME_BYTES to the semantic seam bytes; transport capacity is aligned internally.");
    }

    return Status::ok();
}

} // namespace

RuntimeContext::RuntimeContext() = default;
RuntimeContext::~RuntimeContext() = default;
RuntimeContext::RuntimeContext(RuntimeContext&&) noexcept = default;
RuntimeContext& RuntimeContext::operator=(RuntimeContext&&) noexcept = default;

Status RuntimeContext::configure(RuntimeConfig config)
{
    if (config.tokenRingDepth == 0) {
        return Status::invalidArgument("token ring depth must be at least one");
    }
    if (config.doorbellRingFrameCapacity == 0) {
        return Status::invalidArgument("persistent doorbell ring frame capacity must be at least one");
    }
    if (!config.graphicsBackend) {
        return Status::missingBackend("APUSR requires a graphics backend");
    }
    if (!config.npuBackend) {
        return Status::missingBackend("APUSR requires an NPU backend");
    }
    if (config.manifest.segments.empty()) {
        return Status::invalidArgument("APUSR requires at least one segment manifest");
    }

    for (const SegmentManifest& segment : config.manifest.segments) {
        const Status status = validateSegmentManifest(segment);
        if (!status.succeeded()) {
            return status;
        }
    }

    const SegmentManifest& firstSegment = config.manifest.segments.front();
    const Status frameBridgeStatus = validateFrameConveyorContract(firstSegment);
    if (!frameBridgeStatus.succeeded()) {
        return frameBridgeStatus;
    }

    const NativeBringupStage nativeStage = compiledNativeBringupStage();
    std::uint32_t requiredRingDepth = 2u;
    if (!packagedNativeKernelProducesCompleteTransportFrame(nativeStage) ||
        !firstSegment.streamPlan.npuOwnsInternalStreamState) {
        const std::uint32_t effectiveLookahead = std::min(
            firstSegment.streamPlan.minImportLookaheadBands,
            firstSegment.streamPlan.bandCount > 0 ? firstSegment.streamPlan.bandCount - 1 : 0);
        requiredRingDepth = std::max(effectiveLookahead + 1, 2u);
    }
    if (config.tokenRingDepth < requiredRingDepth) {
        return Status::invalidArgument("token ring depth must cover the selected native frame schedule");
    }
    if (packagedNativeKernelProducesCompleteTransportFrame(nativeStage) &&
        !config.npuProducesCompleteTransportFrame) {
        return Status::invalidArgument("the selected native kernel must declare complete-frame transport coverage");
    }
    if (config.npuProducesModelEgressFrame && !config.npuProducesCompleteTransportFrame) {
        return Status::invalidArgument("model-egress import requires a complete native transport frame");
    }
    if (APUSR_PACKAGED_KERNEL_PROFILE == 4 && config.npuProducesModelEgressFrame) {
        return Status::invalidArgument("FrameConveyor is a transport echo and must not feed the FSR4 stage13/post bookend");
    }

    config_ = std::move(config);

    const Status backendStatus = validateBackends();
    if (!backendStatus.succeeded()) {
        return backendStatus;
    }

    lastFrameProducedOutput_ = false;
    asyncPreviousFramePending_ = false;
    exportAndXrtProbeSubmitted_ = false;
    asyncPreviousFrameIndex_ = 0;
    asyncPreviousFrame_ = FrameDesc{};
    residentModelEpoch_ = 0;
    residentModelEpochReady_ = false;

    const Status slotStatus = createTokenStreamSlots();
    if (!slotStatus.succeeded()) {
        tokenSlots_.clear();
        configured_ = false;
        return slotStatus;
    }

    const Status skipSpillArenaStatus = createSkipSpillArena();
    if (!skipSpillArenaStatus.succeeded()) {
        tokenSlots_.clear();
        skipSpillArenaBuffer_ = SharedBuffer{};
        npuSkipSpillArena_ = NpuBufferView{};
        configured_ = false;
        return skipSpillArenaStatus;
    }
    config_.skipSpillArena = npuSkipSpillArena_;

    const Status residentArenaStatus = createResidentParameterArena();
    if (!residentArenaStatus.succeeded()) {
        tokenSlots_.clear();
        residentParameterArenaBuffer_ = SharedBuffer{};
        npuResidentParameterArena_ = NpuBufferView{};
        skipSpillArenaBuffer_ = SharedBuffer{};
        npuSkipSpillArena_ = NpuBufferView{};
        configured_ = false;
        return residentArenaStatus;
    }
    config_.residentParameterArena = npuResidentParameterArena_;

    const Status ringStatus = createDoorbellRing();
    if (!ringStatus.succeeded()) {
        tokenSlots_.clear();
        doorbellRingDescriptors_.clear();
        doorbellControlRingBuffer_ = SharedBuffer{};
        npuDoorbellControlRing_ = NpuBufferView{};
        residentParameterArenaBuffer_ = SharedBuffer{};
        npuResidentParameterArena_ = NpuBufferView{};
        skipSpillArenaBuffer_ = SharedBuffer{};
        npuSkipSpillArena_ = NpuBufferView{};
        configured_ = false;
        return ringStatus;
    }

    profileSampleGate_.reset();
    profileLastGraphicsApi_ = 0xffffffffu;
    configured_ = true;
    return Status::ok();
}

Status RuntimeContext::switchModelEpochQuiescent(const RuntimeModelEpochSwitchDesc& desc)
{
    if (!configured_) {
        return Status::runtimeFailure("APUSR runtime has not been configured");
    }
    if (desc.mode != RuntimeModelEpochSwitchMode::DrainAndResetOutputHistory) {
        return Status::invalidArgument("unsupported model-epoch switch mode");
    }
    if (persistentControlRingExecution(config_)) {
        return Status::unsupportedBackend("persistent-control-ring execution cannot switch resident model epochs while the controller is running");
    }
    if (config_.residentParameterArena.byteSize == 0) {
        return Status::invalidArgument("model-epoch switch requires a resident-parameter runtime");
    }

    const Status drainStatus = config_.npuBackend->drainSubmittedWork(desc.drainTimeoutMilliseconds);
    if (!drainStatus.succeeded()) {
        return drainStatus;
    }

    const SharedBuffer oldArenaBuffer = residentParameterArenaBuffer_;
    const NpuBufferView oldNpuArena = npuResidentParameterArena_;
    const std::uint64_t oldArenaBytes = config_.residentParameterArenaBytes;
    const std::uint64_t oldEpochDmaBytes = config_.modelEpochParameterDmaBytes;
    const std::uint64_t oldHotPathDmaBytes = config_.hotPathParameterDmaBytesPerRun;
    const std::uint64_t oldModelEpoch = residentModelEpoch_;
    const bool oldEpochReady = residentModelEpochReady_;

    SharedBuffer newArenaBuffer{};
    NpuBufferView newNpuArena{};
    std::uint64_t newResidentModelEpoch = 0;
    bool newResidentModelEpochReady = false;
    const Status programStatus = programResidentParameterArena(
        desc,
        newArenaBuffer,
        newNpuArena,
        newResidentModelEpoch,
        newResidentModelEpochReady);
    if (!programStatus.succeeded()) {
        if (oldEpochReady) {
            const Status reloadStatus = reloadResidentParameterArena(
                oldArenaBuffer,
                oldNpuArena,
                oldArenaBytes,
                oldEpochDmaBytes,
                oldHotPathDmaBytes,
                oldModelEpoch);
            if (!reloadStatus.succeeded()) {
                residentModelEpochReady_ = false;
                return reloadStatus;
            }
        }
        return programStatus;
    }

    residentParameterArenaBuffer_ = newArenaBuffer;
    npuResidentParameterArena_ = newNpuArena;
    config_.residentParameterArena = newNpuArena;
    config_.residentParameterArenaData = nullptr;
    config_.residentParameterArenaBytes = desc.residentParameterArenaBytes;
    config_.modelEpochParameterDmaBytes = desc.modelEpochParameterDmaBytes;
    config_.hotPathParameterDmaBytesPerRun = desc.hotPathParameterDmaBytesPerRun;
    config_.modelEpoch = desc.modelEpoch;
    residentModelEpoch_ = newResidentModelEpoch;
    residentModelEpochReady_ = newResidentModelEpochReady;
    doorbellRingHeader_.reconfigureEpoch = residentModelEpoch_;

    // Epoch changes reset temporal continuity. Old completed output may be
    // visually usable, but importing it after publishing new parameters creates
    // a mixed-epoch history edge. Drop it and let the next frame rebuild.
    asyncPreviousFramePending_ = false;
    asyncPreviousFrameIndex_ = 0;
    asyncPreviousFrame_ = FrameDesc{};
    lastFrameProducedOutput_ = false;
    return Status::ok();
}

Status RuntimeContext::executeFrame(const FrameDesc& frame)
{
    lastFrameProducedOutput_ = false;

    const bool profileActive = apusr::profile::enabled();
    apusr::profile::StreamHandle profileStream = profileActive ? runtimeFallbackProfileStream() : apusr::profile::StreamHandle{};
    apusr::profile::SampleDecision profileDecision{};
    std::uint64_t profileConfigurationHash = 0u;
    std::uint64_t profileFrameStart = 0u;
    bool profileBackendSwitched = false;
    if (profileActive && configured_ && config_.graphicsBackend) {
        const GraphicsApi api = config_.graphicsBackend->api();
        profileStream = runtimeProfileStream(api);
        const std::uint32_t apiValue = static_cast<std::uint32_t>(api);
        profileBackendSwitched = profileLastGraphicsApi_ != 0xffffffffu && profileLastGraphicsApi_ != apiValue;
        if (profileBackendSwitched && profileStream) {
            const char* detail = "backend transition";
            if (profileLastGraphicsApi_ == static_cast<std::uint32_t>(GraphicsApi::Dx12) && api == GraphicsApi::Vulkan)
                detail = "dx12->vulkan";
            else if (profileLastGraphicsApi_ == static_cast<std::uint32_t>(GraphicsApi::Vulkan) && api == GraphicsApi::Dx12)
                detail = "vulkan->dx12";
            else if (api == GraphicsApi::Dx11On12)
                detail = "->dx11on12";
            apusr::profile::recordInstant(profileStream, frame.frameIndex, "backend.switch", detail);
            apusr::profile::recordStateTransition({
                .stream = profileStream,
                .frameId = frame.frameIndex,
                .name = "graphics.backend",
                .reason = "runtime_switch",
                .previousValue = profileLastGraphicsApi_,
                .nextValue = apiValue,
            });
        } else if (profileLastGraphicsApi_ == 0xffffffffu && profileStream) {
            apusr::profile::recordStateTransition({
                .stream = profileStream,
                .frameId = frame.frameIndex,
                .name = "graphics.backend",
                .reason = "initial",
                .previousValue = 0xffffffffu,
                .nextValue = apiValue,
            });
        }
        profileLastGraphicsApi_ = apiValue;

        if (apusr::profile::cpuEnabled() && profileStream) {
            // Backend is a stream dimension, not part of the comparable
            // configuration. Force a fresh burst at the transition instead.
            profileConfigurationHash = 0x415055535254434full;
            profileConfigurationHash = apusr::profile::hashCombine(
                profileConfigurationHash,
                (static_cast<std::uint64_t>(frame.renderSize.width) << 32u) | frame.renderSize.height);
            profileConfigurationHash = apusr::profile::hashCombine(
                profileConfigurationHash,
                (static_cast<std::uint64_t>(frame.displaySize.width) << 32u) | frame.displaySize.height);
            profileDecision = profileSampleGate_.decide(
                profileConfigurationHash, frame.frameIndex, profileBackendSwitched);
            if (profileDecision.capture) {
                profileFrameStart = apusr::profile::cpuTick();
                apusr::profile::recordFlowEvent({
                    .stream = profileStream,
                    .frameId = frame.frameIndex,
                    .flowId = frame.frameIndex + 1u,
                    .name = "runtime.execute_frame",
                    .phase = apusr::profile::FlowPhase::Begin,
                    .value0 = profileConfigurationHash,
                });
            }
        }
    }

    const auto recordStage = [&](const char* name, const std::uint64_t beginTick) noexcept {
        if (profileDecision.capture && beginTick != 0u) {
            const std::uint64_t endTick = apusr::profile::cpuTick();
            apusr::profile::recordCpuInterval(apusr::profile::CpuInterval{
                .stream = profileStream,
                .frameId = frame.frameIndex,
                .sampleSequence = profileDecision.sampleSequence,
                .startCpuTick = beginTick,
                .endCpuTick = endTick,
                .name = name,
            });
            apusr::profile::recordFlowEvent({
                .stream = profileStream,
                .frameId = frame.frameIndex,
                .flowId = frame.frameIndex + 1u,
                .name = "runtime.execute_frame",
                .detail = name,
                .phase = apusr::profile::FlowPhase::Step,
                .value0 = apusr::profile::durationNs(beginTick, endTick),
            });
        }
    };
    const auto finish = [&](const char* stage, Status status) -> Status {
        if (!status.succeeded()) {
            apusr::profile::recordStatus(profileStream, frame.frameIndex, stage, status);
        }
        if (profileDecision.capture && profileFrameStart != 0u) {
            const std::uint64_t profileFrameEnd = apusr::profile::cpuTick();
            const std::uint64_t cpuTotalNs = apusr::profile::durationNs(profileFrameStart, profileFrameEnd);
            apusr::profile::recordFlowEvent({
                .stream = profileStream,
                .frameId = frame.frameIndex,
                .flowId = frame.frameIndex + 1u,
                .name = "runtime.execute_frame",
                .detail = stage,
                .phase = status.succeeded() ? apusr::profile::FlowPhase::End : apusr::profile::FlowPhase::Cancel,
                .value0 = cpuTotalNs,
                .value1 = static_cast<std::uint64_t>(status.code),
            });
            apusr::profile::recordCpuInterval(apusr::profile::CpuInterval{
                .stream = profileStream,
                .frameId = frame.frameIndex,
                .sampleSequence = profileDecision.sampleSequence,
                .startCpuTick = profileFrameStart,
                .endCpuTick = profileFrameEnd,
                .name = "execute_frame.total",
            });
            apusr::profile::recordFrameSample(apusr::profile::FrameSample{
                .stream = profileStream,
                .frameId = frame.frameIndex,
                .sampleSequence = profileDecision.sampleSequence,
                .burstSequence = profileDecision.burstSequence,
                .configurationHash = profileConfigurationHash,
                .burstFrame = profileDecision.burstFrame,
                .renderWidth = frame.renderSize.width,
                .renderHeight = frame.renderSize.height,
                .displayWidth = frame.displaySize.width,
                .displayHeight = frame.displaySize.height,
                .cpuTotalNs = cpuTotalNs,
                .flags = (profileDecision.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u) |
                    (!status.succeeded() ? apusr::profile::FrameFailed : 0u) |
                    (profileBackendSwitched ? apusr::profile::FrameBackendSwitched : 0u),
            });
        }
        return status;
    };

    if (!configured_) {
        return finish("execute_frame.configure", Status::runtimeFailure("APUSR runtime has not been configured"));
    }

    std::uint64_t stageStart = profileDecision.capture ? apusr::profile::cpuTick() : 0u;
    const Status frameStatus = validateFrame(frame);
    recordStage("execute_frame.validate", stageStart);
    if (!frameStatus.succeeded()) {
        return finish("execute_frame.validate", frameStatus);
    }

    if (config_.residentParameterArena.byteSize != 0 && !residentModelEpochReady_) {
        return finish("execute_frame.resident_epoch", Status::runtimeFailure("resident model epoch is not programmed"));
    }

    const SegmentManifest& segment = config_.manifest.segments.front();
    const std::uint32_t bandCount = segment.streamPlan.bandCount;
    const bool persistentControlRing = persistentControlRingExecution(config_);
    if (persistentControlRing && visibleNpuEgressImportEnabled(config_)) {
        return finish(
            "execute_frame.persistent_control_ring",
            Status::runtimeFailure("persistent-control-ring output requires completion-epoch import, not frame NPU-done fences"));
    }

    TokenStreamSlot& framePayloadSlot = acquireTokenStreamSlot(frame.frameIndex, 0, bandCount);
    TokenStreamSlot& frameSyncSlot = acquireFrameSyncSlot(frame.frameIndex, bandCount);

    if (asyncPreviousFramePending_ && visibleNpuEgressImportEnabled(config_)) {
        if (asyncFrameCompatible(asyncPreviousFrame_, frame)) {
            const FrameDesc importFrame = makeAsyncImportFrame(asyncPreviousFrame_, frame);
            TokenStreamSlot& previousPayloadSlot = acquireTokenStreamSlot(asyncPreviousFrameIndex_, 0, bandCount);
            TokenStreamSlot& previousFrameSyncSlot = acquireFrameSyncSlot(asyncPreviousFrameIndex_, bandCount);
            previousPayloadSlot.slotRetiredValue += 1;

            stageStart = profileDecision.capture ? apusr::profile::cpuTick() : 0u;
            const Status status = scheduleFrameImportAfterFrameNpuDone(
                config_,
                previousPayloadSlot,
                previousFrameSyncSlot,
                importFrame,
                segment);
            recordStage("execute_frame.import_previous", stageStart);
            if (!status.succeeded()) {
                return finish("execute_frame.import_previous", status);
            }
            lastFrameProducedOutput_ = true;
        } else {
            asyncPreviousFramePending_ = false;
            asyncPreviousFrame_ = FrameDesc{};
        }
    }

    TimelineFenceWait reuseWait{};
    const TimelineFenceWait* reuseWaitPtr = nullptr;
    if (visibleNpuEgressImportEnabled(config_) && framePayloadSlot.slotRetiredValue > 0) {
        reuseWait = TimelineFenceWait{.fence = framePayloadSlot.slotRetiredFence, .value = framePayloadSlot.slotRetiredValue};
        reuseWaitPtr = &reuseWait;
    } else if (!persistentControlRing && frameSyncSlot.npuDoneValue > 0) {
        reuseWait = TimelineFenceWait{.fence = frameSyncSlot.npuDoneFence, .value = frameSyncSlot.npuDoneValue};
        reuseWaitPtr = &reuseWait;
    }

    frameSyncSlot.gpuReadyValue += 1;
    frameSyncSlot.npuDoneValue += 1;

    stageStart = profileDecision.capture ? apusr::profile::cpuTick() : 0u;
    Status status = recordFrameExport(config_, framePayloadSlot, frame, segment);
    recordStage("execute_frame.record_export", stageStart);
    if (!status.succeeded()) {
        return finish("execute_frame.record_export", status);
    }

    stageStart = profileDecision.capture ? apusr::profile::cpuTick() : 0u;
    status = enqueueFrameCommandConveyor(
        config_,
        frame,
        segment,
        framePayloadSlot,
        frameSyncSlot,
        npuFrameDispatchScratch_,
        doorbellRingHeader_,
        doorbellRingDescriptors_,
        npuDoorbellControlRing_,
        npuIngressTokenArena_,
        npuEgressTokenArena_,
        npuResidentParameterArena_,
        npuSkipSpillArena_,
        doorbellRingStats_);
    recordStage("execute_frame.enqueue_conveyor", stageStart);
    if (!status.succeeded()) {
        return finish("execute_frame.enqueue_conveyor", status);
    }

    stageStart = profileDecision.capture ? apusr::profile::cpuTick() : 0u;
    status = submitFrameExport(config_, framePayloadSlot, frameSyncSlot, reuseWaitPtr);
    recordStage("execute_frame.submit_export", stageStart);
    if (!status.succeeded()) {
        return finish("execute_frame.submit_export", status);
    }

    asyncPreviousFrameIndex_ = frame.frameIndex;
    asyncPreviousFrame_ = frame;
    asyncPreviousFramePending_ = true;
    return finish("execute_frame", Status::ok());
}

const RuntimeConfig& RuntimeContext::config() const
{
    return config_;
}

bool RuntimeContext::configured() const
{
    return configured_;
}

bool RuntimeContext::lastFrameProducedOutput() const
{
    return lastFrameProducedOutput_;
}

const DiagnosticTileOverlayDesc& RuntimeContext::lastDiagnosticTileOverlay() const
{
    return diagnosticTileOverlay_;
}

NpuSubmissionStats RuntimeContext::npuSubmissionStats() const
{
    NpuSubmissionStats stats = config_.npuBackend ? config_.npuBackend->submissionStats() : NpuSubmissionStats{};
    stats.doorbellRing = doorbellRingStats_;
    if (residentModelEpochReady_) {
        stats.residentModelEpoch = residentModelEpoch_;
    }
    return stats;
}

Status RuntimeContext::validateBackends() const
{
    const RuntimeRequirements& requirements = config_.requirements;
    const GraphicsBackendCapabilities graphicsCaps = config_.graphicsBackend->capabilities();
    const NpuBackendCapabilities npuCaps = config_.npuBackend->capabilities();

    if (requirements.requireSharedTokenBuffers && !graphicsCaps.supportsSharedTokenBuffers) {
        return Status::unsupportedBackend("graphics backend does not support shared token buffers");
    }
    if (requirements.requireExternalTimelineFences && !graphicsCaps.supportsExternalTimelineFences) {
        return Status::unsupportedBackend("graphics backend does not support external timeline fences");
    }
    if (requirements.requireQueueSubmit && !graphicsCaps.supportsQueueSubmit) {
        return Status::unsupportedBackend("graphics backend does not expose queue submission");
    }
    if (requirements.requirePersistentCommandContexts && !graphicsCaps.supportsPersistentCommandContexts) {
        return Status::unsupportedBackend("graphics backend does not support persistent command contexts");
    }
    if (requirements.requireNpuSharedBufferImport && !npuCaps.supportsSharedBufferImport) {
        return Status::unsupportedBackend("NPU backend does not support shared-buffer import");
    }
    if (requirements.requireNpuExternalFenceWaitSignal &&
        (!npuCaps.supportsExternalTimelineFenceWait || !npuCaps.supportsExternalTimelineFenceSignal)) {
        return Status::unsupportedBackend("NPU backend does not support external timeline fence wait/signal");
    }
    if (requirements.requirePersistentNpuDispatchObjects && !npuCaps.supportsPersistentDispatchObjects) {
        return Status::unsupportedBackend("NPU backend does not support persistent dispatch objects");
    }
    if (graphicsCaps.sharedBufferHandleKind == SharedBufferHandleKind::None) {
        return Status::unsupportedBackend("graphics backend did not declare a shared-buffer handle kind");
    }
    if (npuCaps.importedBufferHandleKind == SharedBufferHandleKind::None) {
        return Status::unsupportedBackend("NPU backend did not declare an imported-buffer handle kind");
    }
    if (graphicsCaps.timelineFenceHandleKind == TimelineFenceHandleKind::None) {
        return Status::unsupportedBackend("graphics backend did not declare a timeline-fence handle kind");
    }
    if (npuCaps.importedFenceHandleKind == TimelineFenceHandleKind::None) {
        return Status::unsupportedBackend("NPU backend did not declare an imported-fence handle kind");
    }
    if (graphicsCaps.sharedBufferHandleKind != npuCaps.importedBufferHandleKind) {
        return Status::unsupportedBackend("graphics/NPU shared-buffer handle kinds do not match");
    }
    if (graphicsCaps.timelineFenceHandleKind != npuCaps.importedFenceHandleKind) {
        return Status::unsupportedBackend("graphics/NPU timeline-fence handle kinds do not match");
    }

    return Status::ok();
}

Status RuntimeContext::createTokenStreamSlots()
{
    tokenSlots_.clear();
    ingressTokenArena_ = SharedBuffer{};
    egressTokenArena_ = SharedBuffer{};
    npuIngressTokenArena_ = NpuBufferView{};
    npuEgressTokenArena_ = NpuBufferView{};

    const SegmentManifest& segment = config_.manifest.segments.front();
    const std::uint64_t ingressFrameBytes = frameZeroGuardedIngressBytes(segment);
    const std::uint64_t egressFrameBytes = frameEgressBytes(segment);
    const std::uint64_t ingressTransportBytes = frameTransportBytes(ingressFrameBytes);
    const std::uint64_t egressTransportBytes = frameEgressTransportBytes(segment);
    if (ingressFrameBytes == 0 || egressFrameBytes == 0 || ingressTransportBytes == 0 || egressTransportBytes == 0) {
        return Status::invalidArgument("frame payload slots require non-empty C16 seams");
    }

    const std::uint32_t slotsPerFrame = 2u;
    tokenSlots_.reserve(config_.tokenRingDepth * slotsPerFrame);

    for (std::uint32_t frameSlotIndex = 0; frameSlotIndex < config_.tokenRingDepth; ++frameSlotIndex) {
        SharedBuffer ingressFrameSlab;
        SharedBufferDesc ingressDesc{
            .byteSize = ingressTransportBytes,
            .alignment = kHostUserptrSharedBufferAlignmentBytes,
            .debugName = "APUSR ingress frame transport slot",
        };
        Status status = config_.graphicsBackend->createTokenBuffer(ingressDesc, ingressFrameSlab);
        if (!status.succeeded()) {
            return status;
        }

        SharedBuffer egressFrameSlab;
        SharedBufferDesc egressDesc{
            .byteSize = egressTransportBytes,
            .alignment = kHostUserptrSharedBufferAlignmentBytes,
            .debugName = "APUSR egress frame transport slot",
        };
        status = config_.graphicsBackend->createTokenBuffer(egressDesc, egressFrameSlab);
        if (!status.succeeded()) {
            return status;
        }

        NpuBufferView npuIngressFrameSlab;
        status = config_.npuBackend->importSharedBuffer(
            ingressFrameSlab,
            NpuTokenBufferRole::Ingress,
            npuIngressFrameSlab);
        if (!status.succeeded()) {
            return status;
        }

        NpuBufferView npuEgressFrameSlab;
        status = config_.npuBackend->importSharedBuffer(
            egressFrameSlab,
            NpuTokenBufferRole::Egress,
            npuEgressFrameSlab);
        if (!status.succeeded()) {
            return status;
        }

        for (std::uint32_t localSlotIndex = 0; localSlotIndex < slotsPerFrame; ++localSlotIndex) {
            TokenStreamSlot slot{};
            slot.index = static_cast<std::uint32_t>(tokenSlots_.size());
            slot.frameSlotIndex = frameSlotIndex;
            slot.framePayloadSlot = localSlotIndex == 0;
            slot.ingressTokenSlab = ingressFrameSlab;
            slot.egressTokenSlab = egressFrameSlab;
            slot.npuIngressTokenSlab = npuIngressFrameSlab;
            slot.npuEgressTokenSlab = npuEgressFrameSlab;

            status = config_.npuBackend->createTimelineFence(0, slot.gpuReadyFence);
            if (!status.succeeded()) {
                return status;
            }
            status = config_.graphicsBackend->importTimelineFence(slot.gpuReadyFence);
            if (!status.succeeded()) {
                return status;
            }

            status = config_.npuBackend->createTimelineFence(0, slot.npuDoneFence);
            if (!status.succeeded()) {
                return status;
            }
            status = config_.graphicsBackend->importTimelineFence(slot.npuDoneFence);
            if (!status.succeeded()) {
                return status;
            }

            status = config_.npuBackend->createTimelineFence(0, slot.slotRetiredFence);
            if (!status.succeeded()) {
                return status;
            }
            status = config_.graphicsBackend->importTimelineFence(slot.slotRetiredFence);
            if (!status.succeeded()) {
                return status;
            }

            status = config_.graphicsBackend->createCommandContext(CommandPhase::ExportTokens, slot.exportCommands);
            if (!status.succeeded()) {
                return status;
            }

            status = config_.graphicsBackend->createCommandContext(CommandPhase::ImportTokens, slot.importCommands);
            if (!status.succeeded()) {
                return status;
            }

            tokenSlots_.push_back(slot);
        }
    }

    return Status::ok();
}

Status RuntimeContext::programResidentParameterArena(
    const RuntimeModelEpochSwitchDesc& desc,
    SharedBuffer& outArenaBuffer,
    NpuBufferView& outNpuArena,
    std::uint64_t& outResidentModelEpoch,
    bool& outResidentModelEpochReady)
{
    outArenaBuffer = SharedBuffer{};
    outNpuArena = NpuBufferView{};
    outResidentModelEpoch = 0;
    outResidentModelEpochReady = false;

    if (desc.hotPathParameterDmaBytesPerRun != 0) {
        return Status::invalidArgument("product middle-core profiles must report zero hot-path parameter DMA bytes");
    }
    if (desc.residentParameterArenaBytes == 0) {
        if (desc.residentParameterArenaData != nullptr) {
            return Status::invalidArgument("resident parameter arena data requires a non-zero byte size");
        }
        if (desc.modelEpochParameterDmaBytes != 0) {
            return Status::invalidArgument("model-epoch parameter DMA requires a resident parameter arena");
        }
        return Status::ok();
    }
    if (desc.residentParameterArenaData == nullptr) {
        return Status::invalidArgument("resident parameter arena byte size requires a data pointer");
    }
    if (desc.modelEpoch == 0) {
        return Status::invalidArgument("resident parameter arena requires a non-zero model epoch");
    }
    if (desc.modelEpochParameterDmaBytes == 0) {
        return Status::invalidArgument("resident parameter arena requires an explicit model-epoch parameter load");
    }
    if (desc.modelEpochParameterDmaBytes > desc.residentParameterArenaBytes) {
        return Status::invalidArgument("model-epoch parameter DMA bytes cannot exceed the resident parameter arena size");
    }

    SharedBufferDesc arenaDesc{
        .byteSize = desc.residentParameterArenaBytes,
        .alignment = kHostUserptrSharedBufferAlignmentBytes,
        .debugName = "APUSR resident middle-core parameter arena",
    };

    Status status = config_.graphicsBackend->createTokenBuffer(arenaDesc, outArenaBuffer);
    if (!status.succeeded()) {
        return status;
    }
    if (outArenaBuffer.nativeBuffer.value == nullptr ||
        outArenaBuffer.byteSize < desc.residentParameterArenaBytes) {
        return Status::runtimeFailure("resident parameter arena allocation did not expose the requested mapped bytes");
    }

    std::memcpy(
        outArenaBuffer.nativeBuffer.value,
        desc.residentParameterArenaData,
        static_cast<std::size_t>(desc.residentParameterArenaBytes));

    status = config_.npuBackend->importControlBuffer(
        outArenaBuffer,
        NpuControlBufferRole::ResidentParameterArena,
        outNpuArena);
    if (!status.succeeded()) {
        return status;
    }

    status = reloadResidentParameterArena(
        outArenaBuffer,
        outNpuArena,
        desc.residentParameterArenaBytes,
        desc.modelEpochParameterDmaBytes,
        desc.hotPathParameterDmaBytesPerRun,
        desc.modelEpoch);
    if (!status.succeeded()) {
        return status;
    }

    outResidentModelEpoch = desc.modelEpoch;
    outResidentModelEpochReady = true;
    return Status::ok();
}

Status RuntimeContext::createResidentParameterArena()
{
    residentParameterArenaBuffer_ = SharedBuffer{};
    npuResidentParameterArena_ = NpuBufferView{};
    config_.residentParameterArena = NpuBufferView{};
    residentModelEpoch_ = 0;
    residentModelEpochReady_ = false;

    Status status = programResidentParameterArena(
        makeInitialModelEpochDesc(config_),
        residentParameterArenaBuffer_,
        npuResidentParameterArena_,
        residentModelEpoch_,
        residentModelEpochReady_);
    if (!status.succeeded()) {
        return status;
    }

    config_.residentParameterArena = npuResidentParameterArena_;
    return Status::ok();
}


Status RuntimeContext::createSkipSpillArena()
{
    skipSpillArenaBuffer_ = SharedBuffer{};
    npuSkipSpillArena_ = NpuBufferView{};
    config_.skipSpillArena = NpuBufferView{};

    if (config_.skipSpillArenaBytes == 0) {
        return Status::ok();
    }
    if (config_.skipSpillArenaBytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Status::invalidArgument("skip spill arena size does not fit size_t");
    }

    SharedBufferDesc arenaDesc{
        .byteSize = config_.skipSpillArenaBytes,
        .alignment = kHostUserptrSharedBufferAlignmentBytes,
        .debugName = "APUSR resident middle-core SkipA/SkipB spill arena",
    };

    Status status = config_.graphicsBackend->createTokenBuffer(arenaDesc, skipSpillArenaBuffer_);
    if (!status.succeeded()) {
        return status;
    }
    if (skipSpillArenaBuffer_.nativeBuffer.value == nullptr ||
        skipSpillArenaBuffer_.byteSize < config_.skipSpillArenaBytes) {
        return Status::runtimeFailure("skip spill arena allocation did not expose the requested mapped bytes");
    }

    std::memset(
        skipSpillArenaBuffer_.nativeBuffer.value,
        0,
        static_cast<std::size_t>(config_.skipSpillArenaBytes));

    status = config_.npuBackend->importControlBuffer(
        skipSpillArenaBuffer_,
        NpuControlBufferRole::SkipSpillArena,
        npuSkipSpillArena_);
    if (!status.succeeded()) {
        return status;
    }
    if (npuSkipSpillArena_.byteSize < config_.skipSpillArenaBytes) {
        return Status::runtimeFailure("imported skip spill arena is smaller than the runtime skip contract");
    }

    config_.skipSpillArena = npuSkipSpillArena_;
    return Status::ok();
}

Status RuntimeContext::reloadResidentParameterArena(
    const SharedBuffer& arenaBuffer,
    const NpuBufferView& npuArena,
    const std::uint64_t residentParameterArenaBytes,
    const std::uint64_t modelEpochParameterDmaBytes,
    const std::uint64_t hotPathParameterDmaBytesPerRun,
    const std::uint64_t modelEpoch)
{
    if (residentParameterArenaBytes == 0 && modelEpochParameterDmaBytes == 0) {
        return Status::ok();
    }
    if (arenaBuffer.nativeBuffer.value == nullptr || npuArena.sharedBuffer.nativeBuffer.value == nullptr) {
        return Status::runtimeFailure("resident parameter arena reload requires the previous mapped arena");
    }

    NpuModelEpochParameterLoadDesc loadDesc{
        .residentParameterArena = npuArena,
        .skipSpillArena = npuSkipSpillArena_,
        .segment = config_.manifest.segments.empty() ? nullptr : &config_.manifest.segments.front(),
        .residentParameterArenaBytes = residentParameterArenaBytes,
        .skipSpillArenaBytes = config_.skipSpillArenaBytes,
        .modelEpochParameterDmaBytes = modelEpochParameterDmaBytes,
        .hotPathParameterDmaBytesPerRun = hotPathParameterDmaBytesPerRun,
        .modelEpoch = modelEpoch,
    };
    return config_.npuBackend->loadModelParameters(loadDesc);
}

Status RuntimeContext::createDoorbellRing()
{
    const SegmentManifest& segment = config_.manifest.segments.front();
    if (segment.streamPlan.bandCount == 0) {
        return Status::invalidArgument("persistent doorbell ring requires a valid frame stream plan");
    }

    const std::uint64_t descriptorsPerFrame = 1u;
    const std::uint64_t capacity64 = descriptorsPerFrame * config_.doorbellRingFrameCapacity;
    if (capacity64 == 0 || capacity64 > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return Status::invalidArgument("persistent doorbell ring capacity does not fit the ABI header");
    }

    doorbellRingHeader_ = NpuDoorbellRingHeader{};
    doorbellRingHeader_.executionMode = static_cast<std::uint32_t>(
        config_.npuBackend->capabilities().persistentExecutionMode);
    doorbellRingHeader_.descriptorCapacity = static_cast<std::uint32_t>(capacity64);
    doorbellRingHeader_.reconfigureEpoch = residentModelEpoch_;
    doorbellRingDescriptors_.assign(static_cast<std::size_t>(capacity64), NpuDoorbellTokenBandDesc{});

    const std::uint64_t controlRingBytes =
        kNpuPersistentDoorbellDescriptorOffset +
        (capacity64 * kNpuPersistentDoorbellDescriptorStride);
    SharedBufferDesc controlRingDesc{
        .byteSize = controlRingBytes,
        .alignment = kHostUserptrSharedBufferAlignmentBytes,
        .debugName = "APUSR persistent doorbell control ring",
    };

    Status status = config_.graphicsBackend->createTokenBuffer(controlRingDesc, doorbellControlRingBuffer_);
    if (!status.succeeded()) {
        return status;
    }

    status = config_.npuBackend->importControlBuffer(
        doorbellControlRingBuffer_,
        NpuControlBufferRole::DoorbellRing,
        npuDoorbellControlRing_);
    if (!status.succeeded()) {
        return status;
    }

    doorbellRingStats_ = NpuDoorbellRingStats{};
    doorbellRingStats_.publishedSequence = doorbellRingHeader_.publishedSequence;
    doorbellRingStats_.retiredSequence = doorbellRingHeader_.retiredSequence;
    doorbellRingStats_.completedSequence = doorbellRingHeader_.completedSequence;
    return Status::ok();
}

TokenStreamSlot& RuntimeContext::acquireTokenStreamSlot(
    const std::uint64_t frameIndex,
    const std::uint32_t bandIndex,
    const std::uint32_t bandCount)
{
    (void)bandIndex;
    (void)bandCount;
    const std::uint32_t slotsPerFrame = 2u;
    const std::uint32_t frameSlotIndex = static_cast<std::uint32_t>(frameIndex % config_.tokenRingDepth);
    return tokenSlots_[frameSlotIndex * slotsPerFrame];
}

TokenStreamSlot& RuntimeContext::acquireFrameSyncSlot(
    const std::uint64_t frameIndex,
    const std::uint32_t bandCount)
{
    (void)bandCount;
    const std::uint32_t slotsPerFrame = 2u;
    const std::uint32_t frameSlotIndex = static_cast<std::uint32_t>(frameIndex % config_.tokenRingDepth);
    return tokenSlots_[(frameSlotIndex * slotsPerFrame) + 1u];
}

} // namespace apusr
