// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_fsr4_dx12_runtime.h>

#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

FfxApiResource makeResource(void* handle, const std::uint32_t format, const std::uint32_t width, const std::uint32_t height)
{
    FfxApiResource resource{};
    resource.resource = handle;
    resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = format;
    resource.description.width = width;
    resource.description.height = height;
    resource.description.depth = 1;
    resource.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    return resource;
}

} // namespace

int main()
{
    require(apusr::frontends::mapFfxSurfaceFormat(FFX_API_SURFACE_FORMAT_R32G32_UINT) == apusr::SurfaceFormat::Rg32Uint,
            "SDK 2.3 R32G32_UINT must normalize to the native two-channel uint format");
    require(apusr::frontends::mapFfxSurfaceFormat(FFX_API_SURFACE_FORMAT_R8_SNORM) == apusr::SurfaceFormat::R8Snorm,
            "SDK 2.3 R8_SNORM must normalize to the native signed-normalized format");

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = reinterpret_cast<void*>(0x2000);
    dispatch.color = makeResource(reinterpret_cast<void*>(0x3000), FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, 1920, 1080);
    dispatch.depth = makeResource(reinterpret_cast<void*>(0x4000), FFX_API_SURFACE_FORMAT_R32_FLOAT, 1920, 1080);
    dispatch.motionVectors = makeResource(reinterpret_cast<void*>(0x5000), FFX_API_SURFACE_FORMAT_R16G16_FLOAT, 1920, 1080);
    dispatch.reactive = makeResource(reinterpret_cast<void*>(0x5100), FFX_API_SURFACE_FORMAT_R8_UNORM, 1920, 1080);
    dispatch.transparencyAndComposition = makeResource(reinterpret_cast<void*>(0x5200), FFX_API_SURFACE_FORMAT_R8G8_UNORM, 1920, 1080);
    dispatch.output = makeResource(reinterpret_cast<void*>(0x6000), FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, 3840, 2160);
    dispatch.renderSize = FfxApiDimensions2D{.width = 1920, .height = 1080};
    dispatch.upscaleSize = FfxApiDimensions2D{.width = 3840, .height = 2160};
    dispatch.jitterOffset = FfxApiFloatCoords2D{.x = 0.0f, .y = -0.16666667f};
    dispatch.motionVectorScale = FfxApiFloatCoords2D{.x = -3840.0f, .y = -2160.0f};
    dispatch.enableSharpening = true;
    dispatch.sharpness = 0.35f;
    dispatch.frameTimeDelta = 16.6f;
    dispatch.preExposure = 0.75f;
    dispatch.reset = true;
    dispatch.cameraNear = 0.1f;
    dispatch.cameraFar = 1000.0f;
    dispatch.cameraFovAngleVertical = 1.0f;
    dispatch.viewSpaceToMetersFactor = 100.0f;
    const std::uint32_t createFlags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
        FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS |
        FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION |
        FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
        FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION |
        FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

    apusr::frontends::FfxFsr4DispatchNormalization normalization{};
    require(apusr::frontends::normalizeFfxFsr4Dispatch(dispatch, 7, createFlags, normalization).succeeded(), "FSR4 dispatch normalization should succeed");
    require(normalization.frame.frameIndex == 7, "normalization should preserve the assigned frame index");
    require(normalization.frame.renderSize.width == 1920, "normalization should preserve render width");
    require(normalization.frame.displaySize.width == 3840, "normalization should preserve active output width");
    require(normalization.frame.surfaces.color.resource.value == reinterpret_cast<void*>(0x3000), "normalization should preserve color resource handle");
    require(normalization.frame.surfaces.output.resource.value == reinterpret_cast<void*>(0x6000), "normalization should preserve output resource handle");
    require(normalization.frame.surfaces.reactiveMask.resource.value == reinterpret_cast<void*>(0x5100), "normalization should preserve reactive mask resource handle");
    require(normalization.frame.surfaces.transparencyComposition.resource.value == reinterpret_cast<void*>(0x5200), "normalization should preserve transparency/composition resource handle");
    require(normalization.frame.surfaces.motionVectors.format == apusr::SurfaceFormat::Rg16Float, "normalization should preserve R16G16 motion-vector format");
    require(normalization.hasOptionalDepth, "normalization should record optional depth presence");
    require(!normalization.hasOptionalExposure, "normalization should record optional exposure absence");
    require(normalization.frame.options.hasDepth, "frame options should preserve depth presence");
    require(!normalization.frame.options.hasExposure, "frame options should preserve exposure absence");
    require(normalization.frame.options.hasReactiveMask, "frame options should preserve reactive mask presence");
    require(normalization.frame.options.hasTransparencyMask, "frame options should preserve transparency/composition presence");
    require(normalization.frame.options.resetHistory, "frame options should preserve resetHistory");
    require(normalization.frame.options.sharpeningRequested, "frame options should preserve sharpening enablement");
    require(normalization.frame.options.motionVectorScale.x == -3840.0f, "frame options should preserve motion-vector scale X");
    require(normalization.frame.options.motionVectorScale.y == -2160.0f, "frame options should preserve motion-vector scale Y");
    require(normalization.frame.options.frameTimeDeltaMs == 16.6f, "frame options should preserve frame time delta");
    require(normalization.frame.options.preExposure == 0.75f, "frame options should preserve pre-exposure");
    require(normalization.frame.options.viewSpaceToMetersFactor == 100.0f, "frame options should preserve world scale");
    require(normalization.frame.options.features.highDynamicRange, "feature flags should preserve HDR mode");
    require(normalization.frame.options.features.displayResolutionMotionVectors, "feature flags should preserve display-resolution motion-vector mode");
    require(normalization.frame.options.features.motionVectorJitterCancellation, "feature flags should preserve motion-vector jitter cancellation");
    require(normalization.frame.options.features.depthInverted, "feature flags should preserve inverted depth");
    require(!normalization.frame.options.features.depthInfinite, "feature flags should preserve non-infinite depth");
    require(!normalization.frame.options.features.autoExposure, "feature flags should preserve external exposure mode");
    require(normalization.frame.options.features.dynamicResolution, "feature flags should preserve dynamic-resolution mode");
    require(!normalization.frame.options.features.nonLinearColorSpace, "sRGB dispatch flag should override generic non-linear color-space create flag");
    require(normalization.frame.options.features.colorSpaceSrgb, "feature flags should preserve dispatch sRGB color-space mode");
    require(!normalization.frame.options.features.colorSpacePq, "feature flags should preserve non-PQ dispatch color-space mode");
    require(normalization.dispatchFlags == FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB, "normalization should preserve dispatch flags separately from create flags");
    require(normalization.realBoundary.preset == apusr::adapters::Fsr4ModelPreset::DynamicResolution, "dynamic-resolution dispatches must select the DRS boundary preset");
    require(normalization.realBoundary.modelResolution == apusr::adapters::Fsr4ModelResolution::R2160, "3840x2160 dispatch should select the 2160 model family");
    require(normalization.realBoundary.logicalWidth == 1920, "real boundary should use half of the display width");
    require(normalization.realBoundary.logicalHeight == 1080, "real boundary should use half of the display height");
    require(normalization.realBoundary.rowPitchBytes == 30720, "real boundary row pitch for 3840x2160 changed");

    dispatch.sharpness = -0.25f;
    dispatch.enableSharpening = true;
    require(apusr::frontends::normalizeFfxFsr4Dispatch(dispatch, 8, createFlags, normalization).succeeded(), "FFX dispatch normalization should clamp negative sharpening");
    require(!normalization.frame.options.sharpeningRequested, "zero/negative sharpness should not run RCAS");
    require(normalization.frame.options.sharpness == 0.0f, "negative sharpness should clamp to zero");
    dispatch.sharpness = 2.0f;
    require(apusr::frontends::normalizeFfxFsr4Dispatch(dispatch, 9, createFlags, normalization).succeeded(), "FFX dispatch normalization should clamp overrange sharpening");
    require(normalization.frame.options.sharpeningRequested, "positive clamped sharpness should run RCAS");
    require(normalization.frame.options.sharpness == 1.0f, "overrange sharpness should clamp to one");
    dispatch.sharpness = 0.35f;

    require(normalization.frame.surfaces.color.nativeState == 0x8ull, "FFX UAV state must normalize to D3D12 UAV state");
    dispatch.color.state = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    dispatch.output.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    require(apusr::frontends::normalizeFfxFsr4Dispatch(dispatch, 10, createFlags, normalization).succeeded(), "FFX compute-read state normalization should succeed");
    require(normalization.frame.surfaces.color.nativeState == 0x40ull, "FFX compute-read state must normalize to D3D12 non-pixel SRV state");
    require(normalization.frame.surfaces.output.nativeState == 0x8ull, "FFX unordered-access state must normalize to D3D12 UAV state");
    dispatch.color.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    dispatch.output.state = FFX_API_RESOURCE_STATE_RENDER_TARGET;
    require(apusr::frontends::normalizeFfxFsr4Dispatch(dispatch, 11, createFlags, normalization).succeeded(), "FFX pixel+compute state normalization should succeed");
    require(normalization.frame.surfaces.color.nativeState == 0xC0ull, "FFX pixel+compute-read state must normalize to D3D12 pixel|non-pixel SRV state");
    require(normalization.frame.surfaces.output.nativeState == 0x4ull, "FFX render-target state must normalize to D3D12 render-target state");
    dispatch.color.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    dispatch.output.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;

    dispatch.upscaleSize = FfxApiDimensions2D{.width = 1921, .height = 1079};
    dispatch.output.description.width = 1921;
    dispatch.output.description.height = 1079;
    require(apusr::frontends::normalizeFfxFsr4Dispatch(dispatch, 9, createFlags, normalization).succeeded(), "odd output extent normalization should succeed");
    require(normalization.frame.displaySize.width == 1921 && normalization.frame.displaySize.height == 1079, "normalization should preserve odd active output extent");
    require(normalization.realBoundary.activeOutputSize.width == 1921 && normalization.realBoundary.activeOutputSize.height == 1079, "real boundary should preserve active output extent");
    require(normalization.realBoundary.logicalWidth == 964 && normalization.realBoundary.logicalHeight == 540, "real boundary should use model output extent before halving");
    require(normalization.realBoundary.rowPitchBytes == 15616, "real boundary row pitch for odd output extent changed");

    dispatch.upscaleSize = FfxApiDimensions2D{.width = 3840, .height = 2160};
    dispatch.output.description.width = 3840;
    dispatch.output.description.height = 2160;

    dispatch.color.description.format = FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    dispatch.output.description.format = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM;
    dispatch.depth.description.format = FFX_API_SURFACE_FORMAT_R32_TYPELESS;
    dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ;
    require(apusr::frontends::normalizeFfxFsr4Dispatch(dispatch, 8, createFlags, normalization).succeeded(), "extended FFX format normalization should succeed");
    require(normalization.frame.surfaces.color.format == apusr::SurfaceFormat::R11G11B10Float, "normalization should support HDR packed color format");
    require(normalization.frame.surfaces.output.format == apusr::SurfaceFormat::R10G10B10A2Unorm, "normalization should support packed output format");
    require(normalization.frame.surfaces.depth.format == apusr::SurfaceFormat::R32Float, "normalization should canonicalize R32 typeless depth for SRV use");
    require(normalization.frame.options.features.colorSpacePq, "feature flags should preserve dispatch PQ color-space mode");
    require(!normalization.frame.options.features.colorSpaceSrgb, "PQ dispatch should not also mark sRGB mode");
    require(apusr::adapters::validateFsr4RealBoundaryContract(normalization.realBoundary).succeeded(), "normalized real boundary contract should validate");

    std::cout << "APUSR FSR4 normalization smoke: FFX descriptor normalization, SDK 2.3 formats, and real-game formats validated\n";
    return 0;
}
