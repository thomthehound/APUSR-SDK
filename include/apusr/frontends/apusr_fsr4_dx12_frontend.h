// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>

#include <apusr/compat/apusr_ffx_api_abi.h>

#if defined(_WIN32) && defined(APUSR_FSR4_DX12_FRONTEND_EXPORTS)
#define APUSR_FSR4_DX12_FRONTEND_API __declspec(dllexport)
#elif defined(_WIN32)
#define APUSR_FSR4_DX12_FRONTEND_API __declspec(dllimport)
#else
#define APUSR_FSR4_DX12_FRONTEND_API __attribute__((visibility("default")))
#endif

struct ApusrFsr4Dx12FrontendTelemetry {
    std::uint64_t totalDispatches = 0;
    std::uint64_t nativeApusrDispatches = 0;
    std::uint64_t referenceDx12Dispatches = 0;
    std::uint64_t failedDispatches = 0;
};

enum ApusrDx12QueueBridgeSource : std::uint32_t {
    APUSR_DX12_QUEUE_SOURCE_EXPLICIT_NATIVE_CONFIG = 0,
    APUSR_DX12_QUEUE_SOURCE_FFX_FRAME_GENERATION_SWAPCHAIN = 1,
    APUSR_DX12_QUEUE_SOURCE_DXGI_SWAPCHAIN = 2,
    APUSR_DX12_QUEUE_SOURCE_D3D12_CREATE_COMMAND_QUEUE = 3,
    APUSR_DX12_QUEUE_SOURCE_D3D12_EXECUTE_COMMAND_LISTS = 4,
};

struct ApusrDx12QueueBridgeStats {
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
    std::uint32_t lastSource = APUSR_DX12_QUEUE_SOURCE_EXPLICIT_NATIVE_CONFIG;
};

extern "C" {

APUSR_FSR4_DX12_FRONTEND_API std::uint32_t apusrFsr4Dx12FrontendAbiVersion();
APUSR_FSR4_DX12_FRONTEND_API const char* apusrFsr4Dx12FrontendName();
APUSR_FSR4_DX12_FRONTEND_API const char* apusrFsr4Dx12FrontendOutputBaseName();
APUSR_FSR4_DX12_FRONTEND_API bool apusrFsr4Dx12FrontendUsesFidelityFxHooks();
APUSR_FSR4_DX12_FRONTEND_API bool apusrFsr4Dx12FrontendUsesThirdPartyModelArtifacts();
APUSR_FSR4_DX12_FRONTEND_API bool apusrFsr4Dx12FrontendGetTelemetry(
    ffxContext* context,
    ApusrFsr4Dx12FrontendTelemetry* outTelemetry);

APUSR_FSR4_DX12_FRONTEND_API bool apusrDx12QueueBridgeRegister(
    void* device,
    void* queue,
    std::uint32_t source);
APUSR_FSR4_DX12_FRONTEND_API bool apusrDx12QueueBridgeResolve(
    void* device,
    void** outQueue,
    std::uint32_t* outSource);
APUSR_FSR4_DX12_FRONTEND_API bool apusrDx12QueueBridgeInstallDeviceQueueCapture(void* device);
APUSR_FSR4_DX12_FRONTEND_API void apusrDx12QueueBridgeUninstallDeviceQueueCapture();
APUSR_FSR4_DX12_FRONTEND_API bool apusrDx12QueueBridgeInstallDiscovery(void* device);
APUSR_FSR4_DX12_FRONTEND_API void apusrDx12QueueBridgeUninstallDiscovery();
APUSR_FSR4_DX12_FRONTEND_API void apusrDx12QueueBridgeClear();
APUSR_FSR4_DX12_FRONTEND_API bool apusrDx12QueueBridgeGetStats(ApusrDx12QueueBridgeStats* outStats);

}
