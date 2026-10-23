// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_common.h>
#include <apusr/apusr_graphics.h>
#include <apusr/apusr_runtime.h>
#include <apusr/apusr_upscaler_common.h>

#include <array>
#include <cstdint>
#include <span>

namespace apusr {

enum class FrameInputApi : std::uint32_t {
    Dx12,
    Vulkan,
    Dx11On12,
};

enum class FrameInputSurfaceRole : std::uint32_t {
    Color,
    Depth,
    MotionVectors,
    Output,
    Exposure,
    ReactiveMask,
    TransparencyCompositionMask,
    BiasCurrentColorMask,
    AnimatedTextureMask,
    ParticleMask,
};

inline constexpr std::uint32_t kFrameInputMaxSurfaces = 10;
inline constexpr std::uint64_t kFrameInputUnknownNativeState = UINT64_MAX;
inline constexpr std::uint32_t kFrameInputUnknownQueueFamily = UINT32_MAX;

struct FrameInputCommandContext {
    FrameInputApi api = FrameInputApi::Dx12;
    NativeHandle device;
    NativeHandle commandListOrBuffer;
    NativeHandle commandQueue;
    NativeHandle commandAllocator;
    NativeHandle nativeContext;
    std::uint32_t nativeQueueFamilyIndex = kFrameInputUnknownQueueFamily;
};

struct FrameInputSurfaceDesc {
    FrameInputSurfaceRole role = FrameInputSurfaceRole::Color;
    NativeHandle resource;
    NativeHandle view;
    NativeHandle memory;
    SurfaceFormat format = SurfaceFormat::Unknown;
    RenderSize size;
    // Exact API-native metadata retained for backends that cannot reconstruct it
    // losslessly from APUSR's portable surface format. Vulkan ingress uses this
    // for VkFormat and VkImageAspectFlags; DX12/D3D11 leave both at zero.
    std::uint64_t nativeFormat = 0;
    std::uint64_t nativeAspectMask = 0;
    std::uint64_t nativeStateOrLayout = kFrameInputUnknownNativeState;
    std::uint64_t nativeAccessMask = 0;
    std::uint64_t nativeStageMask = 0;
    std::uint32_t subresourceBaseMip = 0;
    std::uint32_t subresourceMipCount = 1;
    std::uint32_t subresourceBaseLayer = 0;
    std::uint32_t subresourceLayerCount = 1;
};

struct FrameInputConstants {
    RenderSize renderSize;
    DisplaySize displaySize;
    RenderSize motionVectorSize;
    Jitter jitter;
    Jitter previousJitter;
    Jitter motionVectorJitterCancellation;
    CameraState camera;
    float renderToDisplayScaleX = 1.0f;
    float renderToDisplayScaleY = 1.0f;
    float displayToRenderScaleX = 1.0f;
    float displayToRenderScaleY = 1.0f;
    std::int32_t jitterPhaseCount = 0;
};

// Canonical APUSR frame-ingress contract. Frontends translate their own API
// dialects into this cheap, POD-like structure; backends consume this or a
// backend-specific projection from it. The object intentionally owns no memory,
// performs no discovery, and carries opaque native handles/states so Vulkan and
// D3D11 can share the same seam without forcing DX12 assumptions into frontends.
struct FrameInputDesc {
    std::uint64_t frameIndex = 0;
    FrameInputCommandContext command;
    FrameInputConstants constants;
    UpscalerFrameOptions options;
    std::array<FrameInputSurfaceDesc, kFrameInputMaxSurfaces> surfaces{};
    std::uint32_t surfaceCount = 0;
};

[[nodiscard]] GraphicsApi graphicsApiForFrameInputApi(FrameInputApi api) noexcept;
[[nodiscard]] SurfaceRole surfaceRoleForFrameInputRole(FrameInputSurfaceRole role) noexcept;
[[nodiscard]] const char* frameInputSurfaceRoleName(FrameInputSurfaceRole role) noexcept;
[[nodiscard]] bool frameInputSurfaceRequiredForFsr4(FrameInputSurfaceRole role) noexcept;

[[nodiscard]] const FrameInputSurfaceDesc* findFrameInputSurface(
    const FrameInputDesc& desc,
    FrameInputSurfaceRole role) noexcept;
[[nodiscard]] Status appendFrameInputSurface(
    FrameInputDesc& desc,
    const FrameInputSurfaceDesc& surface);
[[nodiscard]] Status validateFrameInputForFsr4(const FrameInputDesc& desc);
[[nodiscard]] Status makeRuntimeFrameDescFromFrameInput(
    const FrameInputDesc& desc,
    FrameDesc& outFrame);

} // namespace apusr
