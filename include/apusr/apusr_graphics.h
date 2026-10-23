// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_manifest.h>
#include <apusr/apusr_upscaler_common.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace apusr {

enum class GraphicsApi : std::uint32_t {
    Dx12,
    Vulkan,
    Dx11On12,
};

enum class SurfaceFormat : std::uint32_t {
    Unknown,
    Rgba32Float,
    Rgba32Uint,
    Rgba16Float,
    Rgb32Float,
    Rg32Float,
    Rg16Float,
    Rg16Uint,
    Rg16Sint,
    Rg8Unorm,
    Rg8Uint,
    Rgba8Unorm,
    Rgba8Snorm,
    Rgba8Srgb,
    Bgra8Unorm,
    Bgra8Srgb,
    R11G11B10Float,
    R10G10B10A2Unorm,
    R16Float,
    R16Uint,
    R16Unorm,
    R16Snorm,
    R32Float,
    R32Uint,
    R8Unorm,
    R8Uint,
    R8Sint,
    Rg32Uint,
    R8Snorm,
};

enum class SurfaceRole : std::uint32_t {
    Color,
    Depth,
    MotionVectors,
    Exposure,
    ReactiveMask,
    TransparencyComposition,
    Output,
};

enum class CommandPhase : std::uint32_t {
    ExportTokens,
    ImportTokens,
};

enum class SharedBufferHandleKind : std::uint32_t {
    None,
    Dx12Resource,
    VulkanBuffer,
    HostUserPointer,
};

enum class TimelineFenceHandleKind : std::uint32_t {
    None,
    Dx12Fence,
    VulkanTimelineSemaphore,
    HostTimeline,
};

struct NativeHandle {
    void* value = nullptr;
};

inline constexpr std::uint32_t kHostUserptrSharedBufferAlignmentBytes = 4096;

struct GraphicsBackendCapabilities {
    SharedBufferHandleKind sharedBufferHandleKind = SharedBufferHandleKind::None;
    TimelineFenceHandleKind timelineFenceHandleKind = TimelineFenceHandleKind::None;
    bool supportsSharedTokenBuffers = false;
    bool supportsExternalTimelineFences = false;
    bool supportsQueueSubmit = false;
    bool supportsPersistentCommandContexts = false;
};

struct SurfaceDesc {
    SurfaceRole role = SurfaceRole::Color;
    NativeHandle resource;
    SurfaceFormat format = SurfaceFormat::Unknown;
    RenderSize size;

    // API-specific state value supplied by a compatibility frontend. Core code
    // treats this as opaque; the owning graphics backend may use it to emit the
    // cheapest correct barriers at ingress/egress.
    std::uint64_t nativeState = 0;
};

struct SharedBufferDesc {
    std::uint64_t byteSize = 0;
    // Host-userptr shared buffers use this as both the byte-size alignment and
    // the required mapped-pointer alignment for NPU backend import.
    std::uint64_t alignment = kHostUserptrSharedBufferAlignmentBytes;
    const char* debugName = nullptr;
};

struct SharedBuffer {
    // nativeBuffer is the handle/pointer that the paired NPU backend imports.
    // For the initial DX12/XRT path this is a 4096-byte aligned host pointer used by XRT USERPTR.
    NativeHandle nativeBuffer;

    // nativeAllocation is the graphics-runtime allocation that keeps nativeBuffer alive.
    // For DX12 this is the backing ID3D12Resource*.
    NativeHandle nativeAllocation;

    // nativeGraphicsBuffer is the graphics API resource used by export/import command recording.
    // For DX12 this is also the backing ID3D12Resource*.
    NativeHandle nativeGraphicsBuffer;

    SharedBufferHandleKind handleKind = SharedBufferHandleKind::None;
    std::uint64_t byteSize = 0;
    std::uint64_t gpuVirtualAddress = 0;
};

struct TimelineFence {
    // nativeFence remains a compatibility alias for the object a backend should use when it
    // only needs one side of the interop pair. New backends should prefer the explicit fields.
    NativeHandle nativeFence;

    // Graphics-side fence object used by graphics queue Signal/Wait calls.
    NativeHandle nativeGraphicsFence;

    // NPU-side fence object used by the NPU queue/runtime wait/signal path.
    NativeHandle nativeNpuFence;

    // Exported OS/runtime handle used to import the same fence into the paired backend.
    NativeHandle nativeSharedHandle;

    TimelineFenceHandleKind handleKind = TimelineFenceHandleKind::None;
    std::uint64_t value = 0;
};

struct TimelineFenceWait {
    TimelineFence fence;
    std::uint64_t value = 0;
};

struct TimelineFenceSignal {
    TimelineFence fence;
    std::uint64_t value = 0;
};

struct CommandContext {
    NativeHandle commandList;
    NativeHandle commandAllocator;
    NativeHandle commandQueue;
    NativeHandle nativeContext;
    CommandPhase phase = CommandPhase::ExportTokens;
};

struct ExportPassDesc {
    std::uint64_t frameIndex = 0;
    SurfaceDesc color;
    SurfaceDesc depth;
    SurfaceDesc motionVectors;
    SurfaceDesc exposure;
    SurfaceDesc reactiveMask;
    SurfaceDesc transparencyComposition;
    DisplaySize displaySize;
    UpscalerFrameConstants frameConstants;
    UpscalerFrameOptions frameOptions;
    SharedBuffer destination;
    std::uint64_t destinationByteOffset = 0;
    TokenBoundary destinationBoundary;
    TokenBandDesc band;

    // Borrowed command-list export probes must leave game-owned resources in
    // their incoming states so the following FFX commands see the expected graph.
    bool restoreInputStatesAfterExport = false;
};

struct RoundTripCopyVerifyDesc {
    bool enabled = false;
    SharedBuffer expected;
    std::uint64_t expectedByteOffset = 0;
    std::uint64_t actualByteOffset = 0;
    std::uint64_t byteSize = 0;

    // The verifier reads its diagnostic result only after this graphics-queue
    // signal has completed. For debug copy verification this is the slot-retired
    // signal used by the import command list that records the verifier.
    TimelineFenceSignal completionSignal;
};

struct ImportPassDesc {
    std::uint64_t frameIndex = 0;
    SharedBuffer source;
    std::uint64_t sourceByteOffset = 0;
    TokenBoundary sourceBoundary;
    TokenBandDesc band;
    SurfaceDesc color;
    SurfaceDesc exposure;
    SurfaceDesc output;
    UpscalerFrameConstants frameConstants;
    UpscalerFrameOptions frameOptions;
    RoundTripCopyVerifyDesc roundTripCopyVerify;
};

struct GpuSubmitDesc {
    CommandContext commandContext;
    std::vector<TimelineFenceWait> waits;
    std::vector<TimelineFenceSignal> signals;
    std::string debugName;
};

class GraphicsBackend {
public:
    virtual ~GraphicsBackend() = default;

    [[nodiscard]] virtual GraphicsApi api() const = 0;
    [[nodiscard]] virtual GraphicsBackendCapabilities capabilities() const = 0;
    [[nodiscard]] virtual Status createTokenBuffer(const SharedBufferDesc& desc, SharedBuffer& outBuffer) = 0;
    [[nodiscard]] virtual Status importTimelineFence(TimelineFence& fence) = 0;
    [[nodiscard]] virtual Status createCommandContext(CommandPhase phase, CommandContext& outContext) = 0;
    [[nodiscard]] virtual Status resetCommandContext(CommandContext& commandContext) = 0;
    [[nodiscard]] virtual Status recordExportPass(CommandContext& commandContext, const ExportPassDesc& desc) = 0;
    [[nodiscard]] virtual Status recordImportPass(CommandContext& commandContext, const ImportPassDesc& desc) = 0;
    [[nodiscard]] virtual Status closeCommandContext(CommandContext& commandContext) = 0;
    [[nodiscard]] virtual Status submitGpuWork(const GpuSubmitDesc& desc) = 0;
};

using GraphicsBackendPtr = std::shared_ptr<GraphicsBackend>;

[[nodiscard]] const char* sharedBufferHandleKindName(SharedBufferHandleKind kind);
[[nodiscard]] const char* timelineFenceHandleKindName(TimelineFenceHandleKind kind);

} // namespace apusr
