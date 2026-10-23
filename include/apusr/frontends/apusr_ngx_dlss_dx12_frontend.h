// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>
#include <apusr/apusr_frame_desc.h>
#include <apusr/apusr_upscaler_common.h>

#include <cstdint>

#if defined(_WIN32) && defined(APUSR_NGX_DLSS_DX12_FRONTEND_EXPORTS)
// Windows exports are defined once in apusr_ngx_dlss_exports.def so the
// nvngx.dll forwarding alias can be generated from the same ABI surface.
#define APUSR_NGX_DLSS_DX12_FRONTEND_API
#elif defined(_WIN32)
#define APUSR_NGX_DLSS_DX12_FRONTEND_API __declspec(dllimport)
#else
#define APUSR_NGX_DLSS_DX12_FRONTEND_API __attribute__((visibility("default")))
#endif

#ifndef NVSDK_CONV
#if defined(_MSC_VER)
#define NVSDK_CONV __cdecl
#else
#define NVSDK_CONV
#endif
#endif

namespace apusr::frontends {

enum class NgxDlssPerfQualityMode : std::uint32_t {
    MaxPerformance = 0,
    Balanced = 1,
    MaxQuality = 2,
    UltraPerformance = 3,
    UltraQuality = 4,
    NativeAA = 5,
};

enum NgxDlssFeatureCreateFlags : std::uint32_t {
    kNgxDlssFeatureCreateFlagIsHdr = 1u << 0u,
    kNgxDlssFeatureCreateFlagMvLowRes = 1u << 1u,
    kNgxDlssFeatureCreateFlagMvJittered = 1u << 2u,
    kNgxDlssFeatureCreateFlagDepthInverted = 1u << 3u,
    kNgxDlssFeatureCreateFlagAutoExposure = 1u << 6u,
};

struct NgxDlssCreateDesc {
    NativeHandle device;
    RenderSize renderSize;
    DisplaySize displaySize;
    NgxDlssPerfQualityMode qualityMode = NgxDlssPerfQualityMode::Balanced;
    std::uint32_t featureCreateFlags = 0;
    bool outputSubrectsEnabled = false;
};

struct NgxDlssEvaluateDesc {
    NativeHandle commandList;
    NativeHandle device;
    NativeHandle color;
    NativeHandle depth;
    NativeHandle motionVectors;
    NativeHandle exposure;
    NativeHandle reactiveMask;
    NativeHandle output;
    RenderSize renderSize;
    DisplaySize displaySize;
    RenderSize motionVectorSize;
    Jitter jitter;
    Jitter previousJitter;
    MotionVectorScale motionVectorScale;
    NgxDlssPerfQualityMode qualityMode = NgxDlssPerfQualityMode::Balanced;
    std::uint32_t featureCreateFlags = 0;
    std::uint64_t frameIndex = 0;
    SurfaceFormat colorFormat = SurfaceFormat::Rgba16Float;
    SurfaceFormat depthFormat = SurfaceFormat::R32Float;
    SurfaceFormat motionVectorFormat = SurfaceFormat::Rg16Float;
    SurfaceFormat exposureFormat = SurfaceFormat::R32Float;
    SurfaceFormat reactiveMaskFormat = SurfaceFormat::R8Unorm;
    SurfaceFormat outputFormat = SurfaceFormat::Rgba16Float;
    std::uint64_t colorState = 0;
    std::uint64_t depthState = 0;
    std::uint64_t motionVectorState = 0;
    std::uint64_t exposureState = 0;
    std::uint64_t reactiveMaskState = 0;
    std::uint64_t outputState = 0;
    bool resetHistory = false;
    bool sharpeningRequested = false;
    float sharpness = 0.0f;
    float frameTimeDeltaMs = 0.0f;
    float preExposure = 1.0f;
};

struct NgxVulkanImageResourceDesc {
    NativeHandle image;
    NativeHandle imageView;
    SurfaceFormat format = SurfaceFormat::Unknown;
    RenderSize extent;
    std::uint32_t nativeFormat = 0;
    std::uint32_t aspectMask = 0;
    std::uint32_t baseMipLevel = 0;
    std::uint32_t levelCount = 1;
    std::uint32_t baseArrayLayer = 0;
    std::uint32_t layerCount = 1;
    bool readWrite = false;
};

enum class NgxDlssVulkanStateContract : std::uint32_t {
    Unknown,
    NgxEvaluatePrepared,
};

struct NgxDlssVulkanEvaluateDesc {
    NativeHandle commandBuffer;
    NativeHandle device;
    NativeHandle commandQueue;
    std::uint32_t commandQueueFamilyIndex = kFrameInputUnknownQueueFamily;
    std::uint32_t graphicsQueueFamilyIndex = kFrameInputUnknownQueueFamily;
    std::uint32_t computeQueueFamilyIndex = kFrameInputUnknownQueueFamily;
    NgxDlssVulkanStateContract stateContract = NgxDlssVulkanStateContract::Unknown;
    NgxVulkanImageResourceDesc color;
    NgxVulkanImageResourceDesc depth;
    NgxVulkanImageResourceDesc motionVectors;
    NgxVulkanImageResourceDesc exposure;
    NgxVulkanImageResourceDesc transparencyMask;
    NgxVulkanImageResourceDesc biasCurrentColorMask;
    NgxVulkanImageResourceDesc animatedTextureMask;
    NgxVulkanImageResourceDesc particleMask;
    NgxVulkanImageResourceDesc output;
    RenderSize renderSize;
    DisplaySize displaySize;
    RenderSize motionVectorSize;
    Jitter jitter;
    Jitter previousJitter;
    MotionVectorScale motionVectorScale;
    NgxDlssPerfQualityMode qualityMode = NgxDlssPerfQualityMode::Balanced;
    std::uint32_t featureCreateFlags = 0;
    std::uint64_t frameIndex = 0;
    std::uint32_t colorSubrectBaseX = 0;
    std::uint32_t colorSubrectBaseY = 0;
    std::uint32_t depthSubrectBaseX = 0;
    std::uint32_t depthSubrectBaseY = 0;
    std::uint32_t motionVectorSubrectBaseX = 0;
    std::uint32_t motionVectorSubrectBaseY = 0;
    std::uint32_t outputSubrectBaseX = 0;
    std::uint32_t outputSubrectBaseY = 0;
    std::uint32_t transparencyMaskSubrectBaseX = 0;
    std::uint32_t transparencyMaskSubrectBaseY = 0;
    std::uint32_t biasCurrentColorMaskSubrectBaseX = 0;
    std::uint32_t biasCurrentColorMaskSubrectBaseY = 0;
    bool resetHistory = false;
    bool sharpeningRequested = false;
    float sharpness = 0.0f;
    float frameTimeDeltaMs = 0.0f;
    float preExposure = 1.0f;
};

struct NgxDlssDispatchNormalization {
    FrameInputDesc frameInput;
    FrameDesc frame;
};

[[nodiscard]] UpscalerQualityMode ngxDlssQualityModeToApusr(NgxDlssPerfQualityMode mode);
[[nodiscard]] UpscalerFeatureFlags decodeNgxDlssFeatureFlags(std::uint32_t flags);
[[nodiscard]] Status normalizeNgxDlssEvaluateDesc(
    const NgxDlssEvaluateDesc& desc,
    NgxDlssDispatchNormalization& outNormalization);
[[nodiscard]] Status normalizeNgxDlssVulkanEvaluateDesc(
    const NgxDlssVulkanEvaluateDesc& desc,
    NgxDlssDispatchNormalization& outNormalization);
[[nodiscard]] Status validateNgxDlssCreateDesc(const NgxDlssCreateDesc& desc);

} // namespace apusr::frontends

extern "C" {

APUSR_NGX_DLSS_DX12_FRONTEND_API std::uint32_t apusrNgxDlssDx12FrontendAbiVersion();
APUSR_NGX_DLSS_DX12_FRONTEND_API const char* apusrNgxDlssDx12FrontendName();
APUSR_NGX_DLSS_DX12_FRONTEND_API const char* apusrNgxDlssDx12FrontendOutputBaseName();
APUSR_NGX_DLSS_DX12_FRONTEND_API bool apusrNgxDlssDx12FrontendUsesNvngxHooks();
APUSR_NGX_DLSS_DX12_FRONTEND_API bool apusrNgxDlssDx12FrontendUsesThirdPartyModelArtifacts();
// True when this frontend records APUSR-carried model work rather than forwarding into NVIDIA's embedded DLSS runtime.
APUSR_NGX_DLSS_DX12_FRONTEND_API bool apusrNgxDlssDx12FrontendUsesApusrModelArtifacts();
APUSR_NGX_DLSS_DX12_FRONTEND_API const char* apusrNgxDlssDx12FrontendModelArtifactPolicy();

#if defined(_WIN32)
// Cross-DLL host-capture bridge used by the DXGI proxy when it observes a
// D3D12 swapchain being created from an ID3D12CommandQueue.  The proxy lives
// in dxgi.dll and must not rely on apusr_core's static HostCapture state being
// shared with this nvngx_dlss.dll payload.
APUSR_NGX_DLSS_DX12_FRONTEND_API bool NVSDK_CONV apusrNgxDlssRegisterD3D12CommandQueueForSwapChain(void* swapChain, void* commandQueue);
#endif

}
