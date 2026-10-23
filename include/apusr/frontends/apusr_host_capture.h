// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>

#include <cstdint>

namespace apusr::frontends {

enum class HostCaptureQueueKind : std::uint32_t {
    Unknown = 0,
    Direct = 1,
    Compute = 2,
    Copy = 3,
};

enum class HostCaptureSourceKind : std::uint32_t {
    ExplicitNativeConfig = 0,
    FfxFrameGenerationSwapchain = 1,
    DxgiSwapChain = 2,
    D3d12CreateCommandQueue = 3,
    D3d12ExecuteCommandLists = 4,
    NgxParameterBag = 5,
    D3d11ImmediateContext = 6,
    D3d11On12WrappedQueue = 7,
    VulkanDeviceQueue = 8,
    VulkanQueueSubmit = 9,
};

struct HostCaptureQueueRegistrationDesc {
    GraphicsApi api = GraphicsApi::Dx12;
    NativeHandle device;
    NativeHandle queue;
    NativeHandle swapChain;
    HostCaptureQueueKind queueKind = HostCaptureQueueKind::Direct;
    HostCaptureSourceKind source = HostCaptureSourceKind::ExplicitNativeConfig;
    std::uint32_t nodeMask = 0;
    std::uint64_t frameIndex = 0;
};

struct HostCaptureResolvedQueue {
    GraphicsApi api = GraphicsApi::Dx12;
    NativeHandle device;
    NativeHandle queue;
    NativeHandle swapChain;
    HostCaptureQueueKind queueKind = HostCaptureQueueKind::Unknown;
    HostCaptureSourceKind source = HostCaptureSourceKind::ExplicitNativeConfig;
    std::uint32_t nodeMask = 0;
    bool available = false;
};

struct HostCaptureStats {
    std::uint64_t registrations = 0;
    std::uint64_t duplicateRegistrations = 0;
    std::uint64_t queueCandidates = 0;
    std::uint64_t queueRecordUpdates = 0;
    std::uint64_t swapChainAssociations = 0;
    std::uint64_t resolvesByDevice = 0;
    std::uint64_t resolvesBySwapChain = 0;
    std::uint64_t misses = 0;
    std::uintptr_t lastDevice = 0;
    std::uintptr_t lastQueue = 0;
    std::uintptr_t lastSwapChain = 0;
    HostCaptureSourceKind lastSource = HostCaptureSourceKind::ExplicitNativeConfig;
};

// Records a host-observed queue candidate. HostCapture intentionally keeps a set
// of queue records per device instead of collapsing to one queue per device;
// frontends can then resolve by swapchain first and by device only as a fallback.
// D3D11, D3D11On12, and Vulkan frontends should register observations here from
// API boundary calls, then cache the resolved execution queue/context in their
// feature state. Do not probe backends repeatedly from a frame evaluator.
[[nodiscard]] Status registerHostQueue(const HostCaptureQueueRegistrationDesc& desc);

// Resolves the best queue currently associated with this swapchain. This is the
// preferred path when a frontend can identify the swapchain used for presentation.
[[nodiscard]] HostCaptureResolvedQueue resolveHostQueueForSwapChain(
    GraphicsApi api,
    NativeHandle swapChain,
    HostCaptureQueueKind preferredKind = HostCaptureQueueKind::Direct);

// Resolves the best queue currently associated with this device. This is a
// fallback for frontends that have dispatch resources or a device, but no
// reliable swapchain association yet.
[[nodiscard]] HostCaptureResolvedQueue resolveHostQueueForDevice(
    GraphicsApi api,
    NativeHandle device,
    HostCaptureQueueKind preferredKind = HostCaptureQueueKind::Direct);

// Clears host-capture observations. Test/setup utility only; do not call from a
// frame path.
void clearHostCapture();

[[nodiscard]] HostCaptureStats hostCaptureStats();
[[nodiscard]] const char* hostCaptureSourceName(HostCaptureSourceKind source);
[[nodiscard]] const char* hostCaptureQueueKindName(HostCaptureQueueKind kind);

} // namespace apusr::frontends
