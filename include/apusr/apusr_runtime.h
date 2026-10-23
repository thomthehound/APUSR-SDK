// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>
#include <apusr/apusr_manifest.h>
#include <apusr/apusr_npu.h>
#include <apusr/apusr_profiler.h>
#include <apusr/apusr_upscaler_common.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace apusr {

struct RuntimeRequirements {
    bool requireSharedTokenBuffers = true;
    bool requireExternalTimelineFences = true;
    bool requireQueueSubmit = true;
    bool requirePersistentCommandContexts = true;
    bool requireNpuSharedBufferImport = true;
    bool requireNpuExternalFenceWaitSignal = true;
    bool requirePersistentNpuDispatchObjects = true;
};

// APUSR's product native ABI is one complete frame slot per run. The NPU
// program owns every internal macro-lane, halo, seam, and row traversal. The
// FrameConveyorNative name is retained for the existing ABI/tooling surface;
// older token/probe stages are not valid product runtime modes.
enum class NativeBringupStage : std::uint32_t {
    FrameConveyorNative = 4,
};

#ifndef APUSR_PACKAGED_KERNEL_PROFILE
#define APUSR_PACKAGED_KERNEL_PROFILE 1
#endif

[[nodiscard]] constexpr bool productNativeStage(const NativeBringupStage stage)
{
    return stage == NativeBringupStage::FrameConveyorNative;
}

[[nodiscard]] constexpr bool packagedNativeKernelProducesCompleteTransportFrame(const NativeBringupStage)
{
    return APUSR_PACKAGED_KERNEL_PROFILE == 4 || APUSR_PACKAGED_KERNEL_PROFILE == 17;
}

[[nodiscard]] constexpr bool packagedNativeKernelProducesModelEgressFrame(const NativeBringupStage)
{
    return APUSR_PACKAGED_KERNEL_PROFILE == 17;
}

[[nodiscard]] constexpr NativeBringupStage compiledNativeBringupStage()
{
    return NativeBringupStage::FrameConveyorNative;
}

enum class RuntimeModelEpochSwitchMode : std::uint32_t {
    // Drain submitted NPU work, load the new resident arena, and drop pending
    // old-epoch output/history. Visual discontinuity is allowed at the switch.
    DrainAndResetOutputHistory = 0,
};

struct RuntimeModelEpochSwitchDesc {
    const void* residentParameterArenaData = nullptr;
    std::uint64_t residentParameterArenaBytes = 0;
    std::uint64_t modelEpochParameterDmaBytes = 0;
    std::uint64_t hotPathParameterDmaBytesPerRun = 0;
    std::uint64_t modelEpoch = 0;
    std::uint32_t drainTimeoutMilliseconds = 10000;
    RuntimeModelEpochSwitchMode mode = RuntimeModelEpochSwitchMode::DrainAndResetOutputHistory;
};

struct RuntimeConfig {
    VendorPolicy vendorPolicy = VendorPolicy::PerformanceFirst;
    RuntimeRequirements requirements;
    std::uint32_t tokenRingDepth = 6;
    std::uint32_t doorbellRingFrameCapacity = 3;
    RuntimeManifest manifest;

    // Complete transport means the NPU wrote a full egress slot. Model egress
    // means that slot is semantically valid fsr4.logical_stage_13.input data.
    // A transport echo must not feed the FSR4 post bookend.
    bool npuProducesCompleteTransportFrame = false;
    bool npuProducesModelEgressFrame = false;

    // Product frame-slot imports only semantic model egress. Transport echoes
    // belong in separate diagnostics, not in the runtime ABI.
    bool diagnosticImportTransportEchoToOutput = false;

    // Optional model parameter arena for product-shaped middle-core profiles.
    // When present, RuntimeContext allocates one aligned shared BO, copies this
    // payload at configuration time, and binds it through the resident-parameter
    // kernel argument instead of copying weights through frame tokens.
    const void* residentParameterArenaData = nullptr;
    std::uint64_t residentParameterArenaBytes = 0;
    std::uint64_t modelEpochParameterDmaBytes = 0;
    std::uint64_t hotPathParameterDmaBytesPerRun = 0;
    std::uint64_t modelEpoch = 1;
    NpuBufferView residentParameterArena;

    // Combined persistent SkipA/SkipB arena for resident middle-core profiles.
    // Short-lived activation rings stay inside the NPU graph; this BO is only
    // for skip tensors whose lifetime crosses many logical stages.
    std::uint64_t skipSpillArenaBytes = 0;
    NpuBufferView skipSpillArena;

    GraphicsBackendPtr graphicsBackend;
    NpuBackendPtr npuBackend;
};

struct FrameSurfaces {
    SurfaceDesc color;
    SurfaceDesc depth;
    SurfaceDesc motionVectors;
    SurfaceDesc exposure;
    SurfaceDesc reactiveMask;
    SurfaceDesc transparencyComposition;
    SurfaceDesc output;
};

struct FrameDesc {
    std::uint64_t frameIndex = 0;
    RenderSize renderSize;
    DisplaySize displaySize;
    Jitter jitter;
    Jitter previousJitter;
    CameraState camera;
    UpscalerFrameOptions options;
    FrameSurfaces surfaces;

    NativeHandle borrowedCommandList;
};

struct DiagnosticTileOverlayDesc {
    bool available = false;
    SharedBuffer source;
    TokenBoundary sourceBoundary;
    TokenBandDesc band;
};

struct TokenStreamSlot {
    std::uint32_t index = 0;
    std::uint32_t frameSlotIndex = 0;
    bool framePayloadSlot = false;
    SharedBuffer ingressTokenSlab;
    SharedBuffer egressTokenSlab;
    std::uint64_t ingressByteOffset = 0;
    std::uint64_t egressByteOffset = 0;
    NpuBufferView npuIngressTokenSlab;
    NpuBufferView npuEgressTokenSlab;
    TimelineFence gpuReadyFence;
    TimelineFence npuDoneFence;
    TimelineFence slotRetiredFence;
    std::uint64_t gpuReadyValue = 0;
    std::uint64_t npuDoneValue = 0;
    std::uint64_t slotRetiredValue = 0;
    CommandContext exportCommands;
    CommandContext importCommands;
};

class RuntimeContext {
public:
    RuntimeContext();
    ~RuntimeContext();

    RuntimeContext(const RuntimeContext&) = delete;
    RuntimeContext& operator=(const RuntimeContext&) = delete;
    RuntimeContext(RuntimeContext&&) noexcept;
    RuntimeContext& operator=(RuntimeContext&&) noexcept;

    [[nodiscard]] Status configure(RuntimeConfig config);
    [[nodiscard]] Status switchModelEpochQuiescent(const RuntimeModelEpochSwitchDesc& desc);
    [[nodiscard]] Status executeFrame(const FrameDesc& frame);
    [[nodiscard]] const RuntimeConfig& config() const;
    [[nodiscard]] bool configured() const;
    [[nodiscard]] bool lastFrameProducedOutput() const;
    [[nodiscard]] const DiagnosticTileOverlayDesc& lastDiagnosticTileOverlay() const;
    [[nodiscard]] NpuSubmissionStats npuSubmissionStats() const;

private:
    RuntimeConfig config_;
    std::vector<TokenStreamSlot> tokenSlots_;
    std::vector<TokenBandDesc> tokenBandScratch_;
    std::vector<TokenStreamSlot*> frameSlotScratch_;
    std::vector<TimelineFenceWait> reuseWaitScratch_;
    std::vector<std::uint8_t> hasReuseWaitScratch_;
    std::vector<NpuDispatchDesc> npuFrameDispatchScratch_;
    SharedBuffer ingressTokenArena_;
    SharedBuffer egressTokenArena_;
    NpuBufferView npuIngressTokenArena_;
    NpuBufferView npuEgressTokenArena_;
    NpuDoorbellRingHeader doorbellRingHeader_;
    std::vector<NpuDoorbellTokenBandDesc> doorbellRingDescriptors_;
    SharedBuffer doorbellControlRingBuffer_;
    NpuBufferView npuDoorbellControlRing_;
    SharedBuffer residentParameterArenaBuffer_;
    NpuBufferView npuResidentParameterArena_;
    SharedBuffer skipSpillArenaBuffer_;
    NpuBufferView npuSkipSpillArena_;
    NpuDoorbellRingStats doorbellRingStats_;
    std::uint64_t residentModelEpoch_ = 0;
    bool residentModelEpochReady_ = false;
    bool configured_ = false;
    bool lastFrameProducedOutput_ = false;
    bool asyncPreviousFramePending_ = false;
    bool exportAndXrtProbeSubmitted_ = false;
    std::uint64_t asyncPreviousFrameIndex_ = 0;
    FrameDesc asyncPreviousFrame_;
    DiagnosticTileOverlayDesc diagnosticTileOverlay_;
    profile::SampleGate profileSampleGate_;
    std::uint32_t profileLastGraphicsApi_ = 0xffffffffu;

    [[nodiscard]] Status validateBackends() const;
    [[nodiscard]] Status createTokenStreamSlots();
    [[nodiscard]] Status programResidentParameterArena(
        const RuntimeModelEpochSwitchDesc& desc,
        SharedBuffer& outArenaBuffer,
        NpuBufferView& outNpuArena,
        std::uint64_t& outResidentModelEpoch,
        bool& outResidentModelEpochReady);
    [[nodiscard]] Status createResidentParameterArena();
    [[nodiscard]] Status createSkipSpillArena();
    [[nodiscard]] Status reloadResidentParameterArena(
        const SharedBuffer& arenaBuffer,
        const NpuBufferView& npuArena,
        std::uint64_t residentParameterArenaBytes,
        std::uint64_t modelEpochParameterDmaBytes,
        std::uint64_t hotPathParameterDmaBytesPerRun,
        std::uint64_t modelEpoch);
    [[nodiscard]] Status createDoorbellRing();
    [[nodiscard]] TokenStreamSlot& acquireTokenStreamSlot(std::uint64_t frameIndex, std::uint32_t bandIndex, std::uint32_t bandCount);
    [[nodiscard]] TokenStreamSlot& acquireFrameSyncSlot(std::uint64_t frameIndex, std::uint32_t bandCount);
};

} // namespace apusr
