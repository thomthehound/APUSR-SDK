// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_fsr4_dx12_runtime.h>

#include <algorithm>

#include <apusr/frontends/apusr_fsr4_dx12_embedded_artifacts.h>
#include <apusr/kernels/apusr_frame_conveyor_abi.h>
#include <apusr/kernels/apusr_middle_core_resident_abi.h>

#include <string>
#include <vector>

namespace apusr::frontends {

namespace {

constexpr std::uint32_t kProductXrtDeviceIndex = 0;
constexpr std::uint32_t kProductTokenRingDepth = 6;
constexpr std::uint32_t kProductTokenBandRows = kDefaultTokenBandRows;

#ifndef APUSR_PACKAGED_KERNEL_PROFILE
#define APUSR_PACKAGED_KERNEL_PROFILE 1
#endif

#ifndef APUSR_PACKAGED_MIDDLE_CORE_FRAME_CAPACITY
#define APUSR_PACKAGED_MIDDLE_CORE_FRAME_CAPACITY 2160
#endif

backends::AmdXrtKernelAbi packagedKernelAbi()
{
    backends::AmdXrtKernelAbi abi{};
    abi.residentParameterBufferArgIndex = kernels::middle_core::kResidentParameterBufferArgIndex;
    abi.inputTokenBufferArgIndex = kernels::middle_core::kInputTokenBufferArgIndex;
    abi.outputTokenBufferArgIndex = kernels::middle_core::kOutputTokenBufferArgIndex;
    abi.skipSpillBufferArgIndex = kernels::middle_core::kSkipSpillBufferArgIndex;
    return abi;
}

std::uint32_t packagedKernelOpcode()
{
    return kernels::middle_core::kKernelOpcode;
}

std::uint64_t packagedModelEpochParameterDmaBytes()
{
    return kernels::middle_core::kStage1To12ModelEpochParameterDmaBytes;
}

std::uint64_t packagedHotPathParameterDmaBytesPerRun()
{
    return kernels::middle_core::kStage1To12HotPathParameterDmaBytesPerRun;
}

std::uint64_t packagedSkipSpillArenaBytes()
{
    return kernels::middle_core::kStage1To12SkipSpillArenaBytes;
}

bool packagedKernelUsesResidentParameters()
{
    return true;
}

adapters::Fsr4ModelResolution packagedMiddleCoreFrameCapacity()
{
#if APUSR_PACKAGED_MIDDLE_CORE_FRAME_CAPACITY == 1080
    return adapters::Fsr4ModelResolution::R1080;
#elif APUSR_PACKAGED_MIDDLE_CORE_FRAME_CAPACITY == 2160
    return adapters::Fsr4ModelResolution::R2160;
#elif APUSR_PACKAGED_MIDDLE_CORE_FRAME_CAPACITY == 4320
    return adapters::Fsr4ModelResolution::R4320;
#else
#error APUSR_PACKAGED_MIDDLE_CORE_FRAME_CAPACITY must be 1080, 2160, or 4320
#endif
}

bool packagedKernelSupportsBoundaryCapacity(const adapters::Fsr4ModelResolution modelResolution)
{
    return modelResolution == packagedMiddleCoreFrameCapacity();
}

std::vector<char> copyBytes(const std::uint8_t* data, const std::size_t byteSize)
{
    if (data == nullptr || byteSize == 0) {
        return {};
    }
    const auto* begin = reinterpret_cast<const char*>(data);
    return std::vector<char>{begin, begin + byteSize};
}

} // namespace

Fsr4Dx12RuntimeConfigureDesc makeFsr4Dx12RuntimeConfigureDesc(
    const Fsr4Dx12PackagedRuntimeConfig& config,
    void* commandQueue)
{
    return Fsr4Dx12RuntimeConfigureDesc{
        .header = ffxConfigureDescHeader{.type = kConfigureDescTypeFsr4Dx12Runtime, .pNext = nullptr},
        .commandQueue = commandQueue,
        .xclbinPath = nullptr,
        .xclbinData = config.xclbinData,
        .instructionData = config.instructionData,
        .parameterLoadInstructionData = config.parameterLoadInstructionData,
        .residentParameterArenaData = config.residentParameterArenaData,
        .modelEpochParameterDmaBytes = config.modelEpochParameterDmaBytes,
        .hotPathParameterDmaBytesPerRun = config.hotPathParameterDmaBytesPerRun,
        .skipSpillArenaBytes = config.skipSpillArenaBytes,
        .kernelName = config.kernelName.c_str(),
        .xrtDeviceIndex = config.xrtDeviceIndex,
        .tokenRingDepth = config.tokenRingDepth,
        .tokenBandRows = config.tokenBandRows,
        .flags = config.flags,
        .kernelAbi = config.kernelAbi,
        .xrtOpcode = config.xrtOpcode,
    };
}

Status loadFsr4Dx12PackagedRuntimeConfig(Fsr4Dx12PackagedRuntimeConfig& outConfig)
{
    outConfig = Fsr4Dx12PackagedRuntimeConfig{};

    const EmbeddedFsr4Dx12Xclbin embedded = embeddedFsr4Dx12Xclbin();
    if (!embedded.available()) {
        return Status::ok();
    }

    if (!embedded.hasInstructions()) {
        return Status::invalidArgument("packaged native runtime requires a hot-path instruction stream");
    }
    if (packagedKernelUsesResidentParameters()) {
        if (!embedded.hasResidentParameterArena()) {
            return Status::invalidArgument("packaged resident middle-core runtime requires embedded resident parameter arenas");
        }
        if (!embedded.hasParameterLoadInstructions()) {
            return Status::invalidArgument("packaged resident middle-core runtime requires model-epoch parameter-load instructions");
        }
    }

    outConfig.available = true;
    outConfig.xclbinData = copyBytes(embedded.data, embedded.byteSize);
    outConfig.instructionData = copyBytes(embedded.instructionData, embedded.instructionByteSize);
    outConfig.parameterLoadInstructionData = copyBytes(
        embedded.parameterLoadInstructionData,
        embedded.parameterLoadInstructionByteSize);
    outConfig.residentParameterArenaData = copyBytes(
        embedded.residentParameterArenaData,
        embedded.residentParameterArenaByteSize);
    if (embedded.hasResidentParameterArenaSet()) {
        outConfig.residentParameterArenas.reserve(embedded.residentParameterArenaCount);
        for (std::size_t arenaIndex = 0; arenaIndex < embedded.residentParameterArenaCount; ++arenaIndex) {
            const EmbeddedFsr4Dx12ResidentParameterArena& arena = embedded.residentParameterArenas[arenaIndex];
            if (!arena.available()) {
                return Status::invalidArgument("packaged resident parameter arena table contains an incomplete entry");
            }
            outConfig.residentParameterArenas.push_back(Fsr4Dx12ResidentParameterArenaConfig{
                .modelPermutation = arena.modelPermutation,
                .parameterProfile = arena.parameterProfile,
                .data = copyBytes(arena.data, arena.byteSize),
            });
        }
    }
    outConfig.modelEpochParameterDmaBytes = packagedModelEpochParameterDmaBytes();
    outConfig.hotPathParameterDmaBytesPerRun = packagedHotPathParameterDmaBytesPerRun();
    outConfig.skipSpillArenaBytes = packagedSkipSpillArenaBytes();
    outConfig.kernelName = embedded.kernelName;
    outConfig.xrtDeviceIndex = kProductXrtDeviceIndex;
    outConfig.tokenRingDepth = kProductTokenRingDepth;
    outConfig.tokenBandRows = kProductTokenBandRows;
    outConfig.kernelAbi = packagedKernelAbi();
    outConfig.xrtOpcode = packagedKernelOpcode();
    return Status::ok();
}

Status selectFsr4Dx12PackagedRuntimeConfig(
    const Fsr4Dx12PackagedRuntimeConfig& baseConfig,
    const adapters::Fsr4RealBoundaryContract& boundary,
    Fsr4Dx12PackagedRuntimeConfig& outConfig)
{
    outConfig = baseConfig;
    if (!baseConfig.available || (!packagedKernelUsesResidentParameters() && baseConfig.residentParameterArenas.empty())) {
        return Status::ok();
    }

    if (!packagedKernelSupportsBoundaryCapacity(boundary.modelResolution)) {
        return Status::invalidArgument(
            "packaged stage1->stage12 middle-core capacity does not match the selected FSR4 model family; "
            "distributable builds must embed and select a capacity-matched kernel");
    }

    const std::string modelPermutation =
        adapters::fsr4ModelPermutationName(boundary.preset, boundary.modelResolution);
    const std::string parameterProfile =
        adapters::fsr4ParameterProfileNameForNpuTarget(boundary.npuTarget);

    if (!baseConfig.residentParameterArenas.empty()) {
        for (const Fsr4Dx12ResidentParameterArenaConfig& arena : baseConfig.residentParameterArenas) {
            if (arena.modelPermutation == modelPermutation && arena.parameterProfile == parameterProfile) {
                outConfig.residentParameterArenaData = arena.data;
                outConfig.residentParameterModelPermutation = arena.modelPermutation;
                outConfig.residentParameterProfile = arena.parameterProfile;
                outConfig.residentParameterArenas.clear();
                return Status::ok();
            }
        }
        return Status::invalidArgument(
            "packaged resident middle-core runtime does not contain the required FSR4 model parameter arena");
    }

    if (baseConfig.residentParameterArenaData.empty()) {
        return Status::invalidArgument("packaged resident middle-core runtime has no selected resident parameter arena");
    }

    outConfig.residentParameterModelPermutation = modelPermutation;
    outConfig.residentParameterProfile = parameterProfile;
    return Status::ok();
}


UpscalerFeatureFlags decodeFfxUpscaleFeatureFlags(const std::uint32_t flags)
{
    return UpscalerFeatureFlags{
        .highDynamicRange = (flags & FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE) != 0,
        .displayResolutionMotionVectors = (flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) != 0,
        .motionVectorJitterCancellation = (flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) != 0,
        .depthInverted = (flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) != 0,
        .depthInfinite = (flags & FFX_UPSCALE_ENABLE_DEPTH_INFINITE) != 0,
        .autoExposure = (flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) != 0,
        .dynamicResolution = (flags & FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION) != 0,
        .debugChecking = (flags & FFX_UPSCALE_ENABLE_DEBUG_CHECKING) != 0,
        .nonLinearColorSpace = (flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) != 0,
        .debugVisualization = (flags & FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION) != 0,
    };
}

UpscalerFeatureFlags applyFfxUpscaleDispatchFlags(UpscalerFeatureFlags features, const std::uint32_t dispatchFlags)
{
    features.colorSpaceSrgb = (dispatchFlags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB) != 0;
    features.colorSpacePq = (dispatchFlags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ) != 0;
    features.debugVisualization = features.debugVisualization || ((dispatchFlags & FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW) != 0);
    if (features.colorSpaceSrgb || features.colorSpacePq) {
        features.nonLinearColorSpace = false;
    }
    return features;
}

SurfaceFormat mapFfxSurfaceFormat(const std::uint32_t format)
{
    switch (format) {
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT:
        return SurfaceFormat::Rgba32Float;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT:
        return SurfaceFormat::Rgba32Uint;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT:
        return SurfaceFormat::Rgba16Float;
    case FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT:
        return SurfaceFormat::Rgb32Float;
    case FFX_API_SURFACE_FORMAT_R32G32_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R32G32_FLOAT:
        return SurfaceFormat::Rg32Float;
    case FFX_API_SURFACE_FORMAT_R32G32_UINT:
        return SurfaceFormat::Rg32Uint;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM:
        return SurfaceFormat::Rgba8Unorm;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM:
        return SurfaceFormat::Rgba8Snorm;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB:
        return SurfaceFormat::Rgba8Srgb;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM:
        return SurfaceFormat::Bgra8Unorm;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB:
        return SurfaceFormat::Bgra8Srgb;
    case FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT:
        return SurfaceFormat::R11G11B10Float;
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM:
        return SurfaceFormat::R10G10B10A2Unorm;
    case FFX_API_SURFACE_FORMAT_R16G16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT:
        return SurfaceFormat::Rg16Float;
    case FFX_API_SURFACE_FORMAT_R16G16_UINT:
        return SurfaceFormat::Rg16Uint;
    case FFX_API_SURFACE_FORMAT_R16G16_SINT:
        return SurfaceFormat::Rg16Sint;
    case FFX_API_SURFACE_FORMAT_R16_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R16_FLOAT:
        return SurfaceFormat::R16Float;
    case FFX_API_SURFACE_FORMAT_R16_UINT:
        return SurfaceFormat::R16Uint;
    case FFX_API_SURFACE_FORMAT_R16_UNORM:
        return SurfaceFormat::R16Unorm;
    case FFX_API_SURFACE_FORMAT_R16_SNORM:
        return SurfaceFormat::R16Snorm;
    case FFX_API_SURFACE_FORMAT_R32_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R32_FLOAT:
        return SurfaceFormat::R32Float;
    case FFX_API_SURFACE_FORMAT_R32_UINT:
        return SurfaceFormat::R32Uint;
    case FFX_API_SURFACE_FORMAT_R8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8_UNORM:
        return SurfaceFormat::R8Unorm;
    case FFX_API_SURFACE_FORMAT_R8_UINT:
        return SurfaceFormat::R8Uint;
    case FFX_API_SURFACE_FORMAT_R8_SNORM:
        return SurfaceFormat::R8Snorm;
    case FFX_API_SURFACE_FORMAT_R8G8_TYPELESS:
    case FFX_API_SURFACE_FORMAT_R8G8_UNORM:
        return SurfaceFormat::Rg8Unorm;
    case FFX_API_SURFACE_FORMAT_R8G8_UINT:
        return SurfaceFormat::Rg8Uint;
    default:
        return SurfaceFormat::Unknown;
    }
}

namespace {

// FidelityFX resource states are not D3D12_RESOURCE_STATES.  Their bit values
// intentionally form an API-neutral enum, and treating them as native D3D12
// states produces invalid ResourceBarrier calls.  Keep the DX12 numeric values
// local here so this FFX normalization unit remains buildable in non-Windows
// smoke configurations.
[[nodiscard]] constexpr std::uint64_t ffxResourceStateToD3D12State(const std::uint64_t ffxState) noexcept
{
    constexpr std::uint64_t d3d12Common = 0ull;
    constexpr std::uint64_t d3d12RenderTarget = 0x4ull;
    constexpr std::uint64_t d3d12UnorderedAccess = 0x8ull;
    constexpr std::uint64_t d3d12DepthWrite = 0x10ull;
    constexpr std::uint64_t d3d12NonPixelShaderResource = 0x40ull;
    constexpr std::uint64_t d3d12PixelShaderResource = 0x80ull;
    constexpr std::uint64_t d3d12IndirectArgument = 0x200ull;
    constexpr std::uint64_t d3d12CopyDest = 0x400ull;
    constexpr std::uint64_t d3d12CopySource = 0x800ull;
    constexpr std::uint64_t d3d12GenericRead = 0x1ull | d3d12NonPixelShaderResource | d3d12PixelShaderResource | d3d12CopySource | d3d12IndirectArgument;

    switch (ffxState) {
    case FFX_API_RESOURCE_STATE_COMMON:
    case FFX_API_RESOURCE_STATE_PRESENT:
        return d3d12Common;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
        return d3d12UnorderedAccess;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
        return d3d12NonPixelShaderResource;
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
        return d3d12PixelShaderResource;
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return d3d12NonPixelShaderResource | d3d12PixelShaderResource;
    case FFX_API_RESOURCE_STATE_COPY_SRC:
        return d3d12CopySource;
    case FFX_API_RESOURCE_STATE_COPY_DEST:
        return d3d12CopyDest;
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
        return d3d12GenericRead;
    case FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT:
        return d3d12IndirectArgument;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        return d3d12RenderTarget;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT:
        return d3d12DepthWrite;
    default:
        break;
    }

    std::uint64_t nativeState = 0ull;
    if ((ffxState & FFX_API_RESOURCE_STATE_UNORDERED_ACCESS) != 0ull) {
        nativeState |= d3d12UnorderedAccess;
    }
    if ((ffxState & FFX_API_RESOURCE_STATE_COMPUTE_READ) != 0ull) {
        nativeState |= d3d12NonPixelShaderResource;
    }
    if ((ffxState & FFX_API_RESOURCE_STATE_PIXEL_READ) != 0ull) {
        nativeState |= d3d12PixelShaderResource;
    }
    if ((ffxState & FFX_API_RESOURCE_STATE_COPY_SRC) != 0ull) {
        nativeState |= d3d12CopySource;
    }
    if ((ffxState & FFX_API_RESOURCE_STATE_COPY_DEST) != 0ull) {
        nativeState |= d3d12CopyDest;
    }
    if ((ffxState & FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT) != 0ull) {
        nativeState |= d3d12IndirectArgument;
    }
    if ((ffxState & FFX_API_RESOURCE_STATE_RENDER_TARGET) != 0ull) {
        nativeState |= d3d12RenderTarget;
    }
    if ((ffxState & FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT) != 0ull) {
        nativeState |= d3d12DepthWrite;
    }
    return nativeState == 0ull ? d3d12Common : nativeState;
}
} // namespace

SurfaceDesc normalizeFfxResource(const FfxApiResource& resource, const SurfaceRole role)
{
    SurfaceDesc surface{};
    surface.role = role;
    surface.resource = NativeHandle{resource.resource};
    surface.format = mapFfxSurfaceFormat(resource.description.format);
    surface.size = RenderSize{
        .width = resource.description.width,
        .height = resource.description.height,
    };
    surface.nativeState = ffxResourceStateToD3D12State(resource.state);
    return surface;
}

Status normalizeFfxFsr4Dispatch(
    const ffxDispatchDescUpscale& dispatch,
    const std::uint64_t frameIndex,
    const std::uint32_t createFlags,
    FfxFsr4DispatchNormalization& outNormalization,
    const DisplaySize maxActiveOutputSize)
{
    if (dispatch.commandList == nullptr) {
        return Status::invalidArgument("FSR4 dispatch requires a command list handle");
    }
    if (dispatch.renderSize.width == 0 || dispatch.renderSize.height == 0) {
        return Status::invalidArgument("FSR4 dispatch requires a non-zero render size");
    }

    const DisplaySize activeOutputSize{
        .width = dispatch.upscaleSize.width != 0 ? dispatch.upscaleSize.width : dispatch.output.description.width,
        .height = dispatch.upscaleSize.height != 0 ? dispatch.upscaleSize.height : dispatch.output.description.height,
    };
    if (activeOutputSize.width == 0 || activeOutputSize.height == 0) {
        return Status::invalidArgument("FSR4 dispatch requires a non-zero output/upscale size");
    }

    UpscalerFeatureFlags features = decodeFfxUpscaleFeatureFlags(createFlags);
    features = applyFfxUpscaleDispatchFlags(features, dispatch.flags);

    const float clampedSharpness = std::clamp(dispatch.sharpness, 0.0f, 1.0f);
    const bool enableSharpening = dispatch.enableSharpening && clampedSharpness > 0.0f;

    outNormalization = FfxFsr4DispatchNormalization{};
    outNormalization.commandList = dispatch.commandList;
    outNormalization.frame = FrameDesc{
        .frameIndex = frameIndex,
        .renderSize = RenderSize{.width = dispatch.renderSize.width, .height = dispatch.renderSize.height},
        .displaySize = activeOutputSize,
        .jitter = Jitter{.x = dispatch.jitterOffset.x, .y = dispatch.jitterOffset.y},
        .camera = CameraState{
            .nearPlane = dispatch.cameraNear,
            .farPlane = dispatch.cameraFar,
            .verticalFovRadians = dispatch.cameraFovAngleVertical,
        },
        .options = UpscalerFrameOptions{
            .motionVectorScale = MotionVectorScale{.x = dispatch.motionVectorScale.x, .y = dispatch.motionVectorScale.y},
            .features = features,
            .resetHistory = dispatch.reset,
            .hasDepth = dispatch.depth.resource != nullptr,
            .hasExposure = dispatch.exposure.resource != nullptr,
            .hasReactiveMask = dispatch.reactive.resource != nullptr,
            .hasTransparencyMask = dispatch.transparencyAndComposition.resource != nullptr,
            .sharpeningRequested = enableSharpening,
            .sharpness = clampedSharpness,
            .frameTimeDeltaMs = dispatch.frameTimeDelta,
            .preExposure = dispatch.preExposure,
            .viewSpaceToMetersFactor = dispatch.viewSpaceToMetersFactor,
        },
        .surfaces = FrameSurfaces{
            .color = normalizeFfxResource(dispatch.color, SurfaceRole::Color),
            .depth = normalizeFfxResource(dispatch.depth, SurfaceRole::Depth),
            .motionVectors = normalizeFfxResource(dispatch.motionVectors, SurfaceRole::MotionVectors),
            .exposure = normalizeFfxResource(dispatch.exposure, SurfaceRole::Exposure),
            .reactiveMask = normalizeFfxResource(dispatch.reactive, SurfaceRole::ReactiveMask),
            .transparencyComposition = normalizeFfxResource(dispatch.transparencyAndComposition, SurfaceRole::TransparencyComposition),
            .output = normalizeFfxResource(dispatch.output, SurfaceRole::Output),
        },
    };
    outNormalization.hasOptionalDepth = dispatch.depth.resource != nullptr;
    outNormalization.hasOptionalExposure = dispatch.exposure.resource != nullptr;
    outNormalization.hasReactiveMask = dispatch.reactive.resource != nullptr;
    outNormalization.hasTransparencyMask = dispatch.transparencyAndComposition.resource != nullptr;
    outNormalization.sharpeningRequested = enableSharpening;
    outNormalization.sharpness = clampedSharpness;
    outNormalization.frameTimeDeltaMs = dispatch.frameTimeDelta;
    outNormalization.preExposure = dispatch.preExposure;
    outNormalization.viewSpaceToMetersFactor = dispatch.viewSpaceToMetersFactor;
    outNormalization.dispatchFlags = dispatch.flags;
    const DisplaySize maxOutputForModelSelection{
        .width = maxActiveOutputSize.width != 0 ? maxActiveOutputSize.width : activeOutputSize.width,
        .height = maxActiveOutputSize.height != 0 ? maxActiveOutputSize.height : activeOutputSize.height,
    };
    outNormalization.realBoundary = adapters::makeFsr4RealBoundaryContract(
        outNormalization.frame.renderSize,
        outNormalization.frame.displaySize,
        maxOutputForModelSelection,
        features,
        adapters::NpuTargetFamily::Npu2Aie2p);
    return adapters::validateFsr4RealBoundaryContract(outNormalization.realBoundary);
}


} // namespace apusr::frontends
