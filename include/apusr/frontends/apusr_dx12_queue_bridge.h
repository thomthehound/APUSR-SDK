// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>

#include <cstdint>

namespace apusr::frontends {

enum class Dx12QueueSourceKind : std::uint32_t {
    ExplicitNativeConfig = 0,
    FfxFrameGenerationSwapchain = 1,
    DxgiSwapChain = 2,
    D3d12CreateCommandQueue = 3,
    D3d12ExecuteCommandLists = 4,
};

struct Dx12QueueRegistrationDesc {
    NativeHandle device;
    NativeHandle queue;
    NativeHandle swapChain;
    Dx12QueueSourceKind source = Dx12QueueSourceKind::ExplicitNativeConfig;
    std::uint32_t nodeMask = 0;
    std::uint64_t frameIndex = 0;
};

struct Dx12ResolvedQueue {
    NativeHandle device;
    NativeHandle queue;
    NativeHandle swapChain;
    Dx12QueueSourceKind source = Dx12QueueSourceKind::ExplicitNativeConfig;
    bool available = false;
};

struct Dx12QueueBridgeStats {
    std::uint64_t registrations = 0;
    std::uint64_t replacements = 0;
    std::uint64_t resolves = 0;
    std::uint64_t misses = 0;
    std::uint64_t discoveryInstallAttempts = 0;
    std::uint64_t discoveryHooksInstalled = 0;
    std::uint64_t discoveryCallbacks = 0;
    std::uint64_t discoveryUninstalls = 0;
    std::uint64_t deviceCaptureInstallAttempts = 0;
    std::uint64_t deviceCaptureHooksInstalled = 0;
    std::uint64_t deviceCreateCommandQueueCallbacks = 0;
    std::uint64_t deviceCaptureUninstalls = 0;
    std::uintptr_t lastDevice = 0;
    std::uintptr_t lastQueue = 0;
    Dx12QueueSourceKind lastSource = Dx12QueueSourceKind::ExplicitNativeConfig;
};

// Registers a direct graphics queue discovered outside the APUSR hot path. The
// bridge keeps a COM reference on Windows so cached queues remain valid until the
// bridge entry is replaced or cleared.
[[nodiscard]] Status registerDx12Queue(const Dx12QueueRegistrationDesc& desc);

// Resolves the cached direct queue for a D3D12 device. This is intended for
// context/configuration time; the steady-state dispatch path should store the
// resolved queue pointer in the frontend context.
[[nodiscard]] Dx12ResolvedQueue resolveDx12Queue(NativeHandle device);

// Installs a cold-path ID3D12Device::CreateCommandQueue capture hook on the
// supplied device vtable. This records direct queues at creation time and is
// setup-only infrastructure; it is not part of APUSR frame dispatch.
[[nodiscard]] Status installDx12DeviceQueueCapture(NativeHandle device);

// Removes CreateCommandQueue capture hooks installed by installDx12DeviceQueueCapture.
void uninstallDx12DeviceQueueCapture();

// Installs a cold-path ExecuteCommandLists discovery hook by patching the D3D12
// command-queue vtable reached from this device. The hook unregisters itself
// after the first direct queue is captured, so the steady-state path uses the
// cached queue pointer with no hook branch.
[[nodiscard]] Status installDx12QueueDiscovery(NativeHandle device);

// Removes the ExecuteCommandLists discovery hook if it is still installed.
// This is setup/teardown infrastructure only; do not call it from dispatch.
void uninstallDx12QueueDiscovery();

// Clears cached queues. Test/setup utility only; do not use in the frame path.
void clearDx12QueueBridge();

[[nodiscard]] Dx12QueueBridgeStats dx12QueueBridgeStats();

[[nodiscard]] const char* dx12QueueSourceName(Dx12QueueSourceKind source);

} // namespace apusr::frontends
