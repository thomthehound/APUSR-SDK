// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_ngx_dlss_dx12_frontend.h>
#include <apusr/frontends/apusr_host_capture.h>
#include <apusr/frontends/apusr_dx12_queue_bridge.h>
#include <apusr/backends/apusr_fsr4_reference_dx12.h>
#include <apusr/backends/apusr_fsr4_reference_vulkan.h>
#include <apusr/models/apusr_fsr4_int8_graph.h>
#include <apusr/apusr_frontend.h>

#include "../common/apusr_host_discovery_log.h"
#include "../streamline_attach/apusr_streamline_attach_abi.h"
#include "../vulkan_host_capture/apusr_vulkan_host_capture_abi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <new>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>


#ifndef APUSR_REPO_ROOT
#define APUSR_REPO_ROOT "."
#endif

#ifndef APUSR_ENABLE_DX12_BACKEND
#define APUSR_ENABLE_DX12_BACKEND 0
#endif

#ifndef APUSR_ENABLE_VULKAN_BACKEND
#define APUSR_ENABLE_VULKAN_BACKEND 0
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <unknwn.h>
#if APUSR_ENABLE_DX12_BACKEND
#include <d3d12.h>
#endif
#endif

#ifndef NVSDK_CONV
#if defined(_MSC_VER)
#define NVSDK_CONV __cdecl
#else
#define NVSDK_CONV
#endif
#endif

struct ID3D11Resource;
struct ID3D12Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace {
constexpr std::size_t kNgxCompatScratchBufferSize = 50ull * 1024ull * 1024ull;
#if defined(_WIN32)
HMODULE gApusrNgxModule = nullptr;
HMODULE gApusrStreamlineAttachModule = nullptr;
HMODULE gApusrVulkanHostCaptureModule = nullptr;
std::mutex gApusrVulkanBootstrapMutex;
std::atomic<bool> gApusrVulkanBootstrapLogged{false};
using ApusrVulkanNegotiationFlagsFn = std::uint32_t(__cdecl*)(void*);
using ApusrVulkanResolveQueueFn = bool(__cdecl*)(void*, std::uint32_t, ApusrVulkanHostQueueInfo*);
std::atomic<ApusrStreamlineVulkanDlssEvaluationActiveFn> gApusrStreamlineEvaluationActiveFn{nullptr};
std::atomic<ApusrVulkanNegotiationFlagsFn> gApusrVulkanNegotiationFlagsFn{nullptr};
std::atomic<ApusrVulkanResolveQueueFn> gApusrVulkanResolveQueueFn{nullptr};
std::atomic<bool> gApusrVulkanIngressFunctionsReady{false};

HMODULE loadNgxSiblingModule(const wchar_t* fileName, HMODULE& cache)
{
    if (cache != nullptr) {
        return cache;
    }
    if (gApusrNgxModule == nullptr || fileName == nullptr) {
        return nullptr;
    }
    std::array<wchar_t, 32768> modulePath{};
    const DWORD chars = GetModuleFileNameW(
        gApusrNgxModule, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (chars == 0 || chars >= modulePath.size()) {
        return nullptr;
    }
    std::filesystem::path path(std::wstring(modulePath.data(), chars));
    path.replace_filename(fileName);
    cache = LoadLibraryW(path.c_str());
    return cache;
}

bool bootstrapApusrVulkanIngress()
{
    std::lock_guard<std::mutex> lock(gApusrVulkanBootstrapMutex);

    bool attached = false;
    if (HMODULE attachModule = loadNgxSiblingModule(
            L"apusr_streamline_attach.dll", gApusrStreamlineAttachModule)) {
        const auto attach = reinterpret_cast<ApusrAttachToLoadedStreamlineFn>(
            GetProcAddress(attachModule, "apusrAttachToLoadedStreamline"));
        const auto hint = reinterpret_cast<ApusrStreamlineAttachHintRenderApiFn>(
            GetProcAddress(attachModule, "apusrStreamlineAttachHintRenderApi"));
        gApusrStreamlineEvaluationActiveFn.store(
            reinterpret_cast<ApusrStreamlineVulkanDlssEvaluationActiveFn>(
                GetProcAddress(attachModule, "apusrStreamlineVulkanDlssEvaluationActive")),
            std::memory_order_release);
        // Establish Vulkan state/capture before detouring any stock Streamline
        // entry point. A direct NGX Vulkan host may have no Streamline module at
        // all; in that case the hint still installs the backend host capture.
        if (hint != nullptr) {
            hint(APUSR_STREAMLINE_RENDER_API_VULKAN);
        }
        if (attach != nullptr) {
            attached = attach();
        }
    }

    bool captureInstalled = false;
    if (HMODULE captureModule = loadNgxSiblingModule(
            L"apusr_vulkan_host_capture.dll", gApusrVulkanHostCaptureModule)) {
        using InstallFn = bool(__cdecl*)();
        const auto install = reinterpret_cast<InstallFn>(
            GetProcAddress(captureModule, "apusrInstallVulkanHostCapture"));
        gApusrVulkanNegotiationFlagsFn.store(
            reinterpret_cast<ApusrVulkanNegotiationFlagsFn>(
                GetProcAddress(captureModule, "apusrVulkanHostDeviceNegotiationFlags")),
            std::memory_order_release);
        gApusrVulkanResolveQueueFn.store(
            reinterpret_cast<ApusrVulkanResolveQueueFn>(
                GetProcAddress(captureModule, "apusrVulkanHostResolveQueue")),
            std::memory_order_release);
        captureInstalled = install != nullptr && install();
    }

    const bool functionsReady =
        gApusrStreamlineEvaluationActiveFn.load(std::memory_order_acquire) != nullptr &&
        gApusrVulkanNegotiationFlagsFn.load(std::memory_order_acquire) != nullptr &&
        gApusrVulkanResolveQueueFn.load(std::memory_order_acquire) != nullptr;
    gApusrVulkanIngressFunctionsReady.store(functionsReady, std::memory_order_release);

    if ((attached || captureInstalled) &&
        !gApusrVulkanBootstrapLogged.exchange(true, std::memory_order_acq_rel)) {
        apusr::host_discovery::eventf(
            "ngx", "vulkan.bootstrap",
            "stock_streamline_attach=%u host_capture=%u stock_interposer_preserved=1",
            attached ? 1u : 0u, captureInstalled ? 1u : 0u);
    }
    return captureInstalled;
}
#endif
}


namespace apusr::frontends {
namespace {

[[nodiscard]] bool nonZero(const RenderSize size) noexcept
{
    return size.width != 0 && size.height != 0;
}

[[nodiscard]] bool nonZero(const DisplaySize size) noexcept
{
    return size.width != 0 && size.height != 0;
}

} // namespace

UpscalerQualityMode ngxDlssQualityModeToApusr(const NgxDlssPerfQualityMode mode)
{
    switch (mode) {
    case NgxDlssPerfQualityMode::UltraPerformance:
        return UpscalerQualityMode::UltraPerformance;
    case NgxDlssPerfQualityMode::MaxPerformance:
        return UpscalerQualityMode::Performance;
    case NgxDlssPerfQualityMode::Balanced:
        return UpscalerQualityMode::Balanced;
    case NgxDlssPerfQualityMode::UltraQuality:
    case NgxDlssPerfQualityMode::MaxQuality:
        return UpscalerQualityMode::Quality;
    case NgxDlssPerfQualityMode::NativeAA:
        return UpscalerQualityMode::NativeAA;
    }
    return UpscalerQualityMode::Balanced;
}

UpscalerFeatureFlags decodeNgxDlssFeatureFlags(const std::uint32_t flags)
{
    UpscalerFeatureFlags decoded;
    decoded.highDynamicRange = (flags & kNgxDlssFeatureCreateFlagIsHdr) != 0u;
    decoded.displayResolutionMotionVectors = (flags & kNgxDlssFeatureCreateFlagMvLowRes) == 0u;
    decoded.motionVectorJitterCancellation = (flags & kNgxDlssFeatureCreateFlagMvJittered) != 0u;
    decoded.depthInverted = (flags & kNgxDlssFeatureCreateFlagDepthInverted) != 0u;
    decoded.autoExposure = (flags & kNgxDlssFeatureCreateFlagAutoExposure) != 0u;
    return decoded;
}

Status validateNgxDlssCreateDesc(const NgxDlssCreateDesc& desc)
{
    if (desc.device.value == nullptr) {
        return Status::invalidArgument("NGX/DLSS create requires a graphics device");
    }
    if (!nonZero(desc.renderSize)) {
        return Status::invalidArgument("NGX/DLSS create requires a non-zero render size");
    }
    if (!nonZero(desc.displaySize)) {
        return Status::invalidArgument("NGX/DLSS create requires a non-zero display size");
    }
    return Status::ok();
}

Status normalizeNgxDlssEvaluateDesc(
    const NgxDlssEvaluateDesc& desc,
    NgxDlssDispatchNormalization& outNormalization)
{
    outNormalization = NgxDlssDispatchNormalization{};

    if (desc.commandList.value == nullptr) {
        return Status::invalidArgument("NGX/DLSS DX12 evaluation requires a borrowed command list");
    }
    if (desc.color.value == nullptr) {
        return Status::invalidArgument("NGX/DLSS DX12 evaluation missing Color resource");
    }
    if (desc.depth.value == nullptr) {
        return Status::invalidArgument("NGX/DLSS DX12 evaluation missing Depth resource");
    }
    if (desc.motionVectors.value == nullptr) {
        return Status::invalidArgument("NGX/DLSS DX12 evaluation missing MotionVectors resource");
    }
    if (desc.output.value == nullptr) {
        return Status::invalidArgument("NGX/DLSS DX12 evaluation missing Output resource");
    }
    if (!nonZero(desc.renderSize)) {
        return Status::invalidArgument("NGX/DLSS DX12 evaluation requires a non-zero render size");
    }
    if (!nonZero(desc.displaySize)) {
        return Status::invalidArgument("NGX/DLSS DX12 evaluation requires a non-zero display size");
    }

    const RenderSize motionVectorSize = nonZero(desc.motionVectorSize) ? desc.motionVectorSize : desc.renderSize;

    UpscalerFrameOptions options;
    options.qualityMode = ngxDlssQualityModeToApusr(desc.qualityMode);
    options.features = decodeNgxDlssFeatureFlags(desc.featureCreateFlags);
    options.motionVectorScale = desc.motionVectorScale;
    options.resetHistory = desc.resetHistory;
    options.sharpeningRequested = desc.sharpeningRequested;
    options.sharpness = desc.sharpness;
    options.frameTimeDeltaMs = desc.frameTimeDeltaMs;
    options.preExposure = desc.preExposure == 0.0f ? 1.0f : desc.preExposure;

    FrameInputDesc& frameInput = outNormalization.frameInput;
    frameInput.frameIndex = desc.frameIndex;
    frameInput.command.api = FrameInputApi::Dx12;
    frameInput.command.device = desc.device;
    frameInput.command.commandListOrBuffer = desc.commandList;
    frameInput.constants.renderSize = desc.renderSize;
    frameInput.constants.displaySize = desc.displaySize;
    frameInput.constants.motionVectorSize = motionVectorSize;
    frameInput.constants.jitter = desc.jitter;
    frameInput.constants.previousJitter = desc.previousJitter;
    frameInput.constants.renderToDisplayScaleX = desc.displaySize.width != 0
        ? static_cast<float>(desc.renderSize.width) / static_cast<float>(desc.displaySize.width)
        : 1.0f;
    frameInput.constants.renderToDisplayScaleY = desc.displaySize.height != 0
        ? static_cast<float>(desc.renderSize.height) / static_cast<float>(desc.displaySize.height)
        : 1.0f;
    frameInput.constants.displayToRenderScaleX = desc.renderSize.width != 0
        ? static_cast<float>(desc.displaySize.width) / static_cast<float>(desc.renderSize.width)
        : 1.0f;
    frameInput.constants.displayToRenderScaleY = desc.renderSize.height != 0
        ? static_cast<float>(desc.displaySize.height) / static_cast<float>(desc.renderSize.height)
        : 1.0f;
    frameInput.options = options;

    auto append = [&](FrameInputSurfaceRole role, NativeHandle resource, SurfaceFormat format, RenderSize size, std::uint64_t nativeState) -> Status {
        return appendFrameInputSurface(frameInput, FrameInputSurfaceDesc{
            .role = role,
            .resource = resource,
            .view = {},
            .memory = {},
            .format = format,
            .size = size,
            .nativeStateOrLayout = nativeState,
        });
    };

    Status status = append(FrameInputSurfaceRole::Color, desc.color, desc.colorFormat, desc.renderSize, desc.colorState);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::Depth, desc.depth, desc.depthFormat, desc.renderSize, desc.depthState);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::MotionVectors, desc.motionVectors, desc.motionVectorFormat, motionVectorSize, desc.motionVectorState);
    if (!status.succeeded()) { return status; }
    status = append(
        FrameInputSurfaceRole::Output,
        desc.output,
        desc.outputFormat,
        RenderSize{.width = desc.displaySize.width, .height = desc.displaySize.height},
        desc.outputState);
    if (!status.succeeded()) { return status; }
    if (desc.exposure.value != nullptr) {
        status = append(FrameInputSurfaceRole::Exposure, desc.exposure, desc.exposureFormat, RenderSize{.width = 1u, .height = 1u}, desc.exposureState);
        if (!status.succeeded()) { return status; }
    }
    if (desc.reactiveMask.value != nullptr) {
        status = append(FrameInputSurfaceRole::ReactiveMask, desc.reactiveMask, desc.reactiveMaskFormat, desc.renderSize, desc.reactiveMaskState);
        if (!status.succeeded()) { return status; }
    }

    return makeRuntimeFrameDescFromFrameInput(frameInput, outNormalization.frame);
}

Status normalizeNgxDlssVulkanEvaluateDesc(
    const NgxDlssVulkanEvaluateDesc& desc,
    NgxDlssDispatchNormalization& outNormalization)
{
    outNormalization = NgxDlssDispatchNormalization{};

    if (desc.commandBuffer.value == nullptr) {
        return Status::invalidArgument("NGX/DLSS Vulkan evaluation requires a borrowed command buffer");
    }
    if (desc.device.value == nullptr) {
        return Status::invalidArgument("NGX/DLSS Vulkan evaluation requires a Vulkan device");
    }
    if (desc.graphicsQueueFamilyIndex == kFrameInputUnknownQueueFamily &&
        desc.computeQueueFamilyIndex == kFrameInputUnknownQueueFamily) {
        return Status::notImplemented("NGX/DLSS Vulkan evaluation has no host-captured graphics/compute queue-family contract");
    }
    if (desc.stateContract != NgxDlssVulkanStateContract::NgxEvaluatePrepared) {
        return Status::notImplemented("NGX/DLSS Vulkan evaluation has no trusted external-image state contract");
    }
    for (const auto* resource : {&desc.color, &desc.depth, &desc.motionVectors, &desc.output}) {
        if (resource->image.value == nullptr || resource->imageView.value == nullptr) {
            return Status::invalidArgument("NGX/DLSS Vulkan evaluation is missing a required image or image view");
        }
        if (resource->format == SurfaceFormat::Unknown || resource->nativeFormat == 0u) {
            return Status::invalidArgument("NGX/DLSS Vulkan evaluation contains an unsupported required image format");
        }
    }
    if (!desc.output.readWrite) {
        return Status::invalidArgument("NGX/DLSS Vulkan output must be storage/read-write capable");
    }
    if (!nonZero(desc.renderSize) || !nonZero(desc.displaySize)) {
        return Status::invalidArgument("NGX/DLSS Vulkan evaluation requires non-zero render and display sizes");
    }
    if (desc.colorSubrectBaseX != 0u || desc.colorSubrectBaseY != 0u ||
        desc.depthSubrectBaseX != 0u || desc.depthSubrectBaseY != 0u ||
        desc.motionVectorSubrectBaseX != 0u || desc.motionVectorSubrectBaseY != 0u ||
        desc.outputSubrectBaseX != 0u || desc.outputSubrectBaseY != 0u ||
        desc.transparencyMaskSubrectBaseX != 0u || desc.transparencyMaskSubrectBaseY != 0u ||
        desc.biasCurrentColorMaskSubrectBaseX != 0u || desc.biasCurrentColorMaskSubrectBaseY != 0u) {
        return Status::notImplemented("NGX/DLSS Vulkan non-zero resource subrect origins are not implemented yet");
    }

    const RenderSize motionVectorSize = nonZero(desc.motionVectorSize) ? desc.motionVectorSize : desc.renderSize;

    UpscalerFrameOptions options;
    options.qualityMode = ngxDlssQualityModeToApusr(desc.qualityMode);
    options.features = decodeNgxDlssFeatureFlags(desc.featureCreateFlags);
    options.motionVectorScale = desc.motionVectorScale;
    options.resetHistory = desc.resetHistory;
    options.sharpeningRequested = desc.sharpeningRequested;
    options.sharpness = desc.sharpness;
    options.frameTimeDeltaMs = desc.frameTimeDeltaMs;
    options.preExposure = desc.preExposure == 0.0f ? 1.0f : desc.preExposure;

    FrameInputDesc& frameInput = outNormalization.frameInput;
    frameInput.frameIndex = desc.frameIndex;
    frameInput.command.api = FrameInputApi::Vulkan;
    frameInput.command.device = desc.device;
    frameInput.command.commandListOrBuffer = desc.commandBuffer;
    frameInput.command.commandQueue = desc.commandQueue;
    frameInput.command.nativeQueueFamilyIndex = desc.commandQueueFamilyIndex;
    frameInput.constants.renderSize = desc.renderSize;
    frameInput.constants.displaySize = desc.displaySize;
    frameInput.constants.motionVectorSize = motionVectorSize;
    frameInput.constants.jitter = desc.jitter;
    frameInput.constants.previousJitter = desc.previousJitter;
    frameInput.constants.renderToDisplayScaleX = static_cast<float>(desc.renderSize.width) / static_cast<float>(desc.displaySize.width);
    frameInput.constants.renderToDisplayScaleY = static_cast<float>(desc.renderSize.height) / static_cast<float>(desc.displaySize.height);
    frameInput.constants.displayToRenderScaleX = static_cast<float>(desc.displaySize.width) / static_cast<float>(desc.renderSize.width);
    frameInput.constants.displayToRenderScaleY = static_cast<float>(desc.displaySize.height) / static_cast<float>(desc.renderSize.height);
    frameInput.options = options;

    // Stock sl.dlss establishes these evaluate layouts immediately before
    // entering the injected NGX context and restores the host-tagged states
    // afterward. APUSR's host-capture seam may narrow only Streamline's generic
    // ALL_COMMANDS stage scope; it does not take ownership of the transitions.
    // Do not apply this provenance to generic/native NGX callers: the NGX
    // Vulkan resource ABI itself does not carry image state.
    constexpr std::uint64_t kVkImageLayoutGeneral = 1u;
    constexpr std::uint64_t kVkImageLayoutShaderReadOnlyOptimal = 5u;
    constexpr std::uint64_t kVkPipelineStageComputeShaderBit = 0x00000800u;
    constexpr std::uint64_t kVkAccessShaderReadBit = 0x00000020u;
    constexpr std::uint64_t kVkAccessShaderWriteBit = 0x00000040u;
    auto append = [&](FrameInputSurfaceRole role, const NgxVulkanImageResourceDesc& resource, RenderSize size,
                      std::uint64_t layout, std::uint64_t access) -> Status {
        if (resource.image.value == nullptr) {
            return Status::ok();
        }
        return appendFrameInputSurface(frameInput, FrameInputSurfaceDesc{
            .role = role,
            .resource = resource.image,
            .view = resource.imageView,
            .memory = {},
            .format = resource.format,
            .size = size,
            .nativeFormat = resource.nativeFormat,
            .nativeAspectMask = resource.aspectMask,
            .nativeStateOrLayout = layout,
            .nativeAccessMask = access,
            .nativeStageMask = kVkPipelineStageComputeShaderBit,
            .subresourceBaseMip = resource.baseMipLevel,
            .subresourceMipCount = resource.levelCount,
            .subresourceBaseLayer = resource.baseArrayLayer,
            .subresourceLayerCount = resource.layerCount,
        });
    };

    Status status = append(FrameInputSurfaceRole::Color, desc.color, desc.renderSize, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::Depth, desc.depth, desc.renderSize, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::MotionVectors, desc.motionVectors, motionVectorSize, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::Output, desc.output, RenderSize{desc.displaySize.width, desc.displaySize.height}, kVkImageLayoutGeneral, kVkAccessShaderReadBit | kVkAccessShaderWriteBit);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::Exposure, desc.exposure, RenderSize{1u, 1u}, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::TransparencyCompositionMask, desc.transparencyMask, desc.renderSize, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::BiasCurrentColorMask, desc.biasCurrentColorMask, desc.renderSize, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::AnimatedTextureMask, desc.animatedTextureMask, desc.renderSize, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit);
    if (!status.succeeded()) { return status; }
    status = append(FrameInputSurfaceRole::ParticleMask, desc.particleMask, desc.renderSize, kVkImageLayoutShaderReadOnlyOptimal, kVkAccessShaderReadBit);
    if (!status.succeeded()) { return status; }

    return makeRuntimeFrameDescFromFrameInput(frameInput, outNormalization.frame);
}

} // namespace apusr::frontends

namespace {

using NgxResult = std::uint32_t;
constexpr NgxResult kNgxSuccess = 0x00000001u;
constexpr NgxResult kNgxFail = 0xBAD00000u;
constexpr NgxResult kNgxFailFeatureNotSupported = kNgxFail | 1u;
constexpr NgxResult kNgxFailPlatformError = kNgxFail | 2u;
constexpr NgxResult kNgxFailFeatureNotFound = kNgxFail | 4u;
constexpr NgxResult kNgxFailInvalidParameter = kNgxFail | 5u;
constexpr NgxResult kNgxFailNotInitialized = kNgxFail | 7u;
constexpr NgxResult kNgxFailMissingInput = kNgxFail | 10u;

constexpr std::uint32_t kNgxFeatureSuperSampling = 1u;

[[nodiscard]] bool ngxFeatureDiscoveryLooksLikeSuperSampling(const void* featureDiscoveryInfo) noexcept
{
    if (featureDiscoveryInfo == nullptr) {
        return false;
    }
    // NVSDK_NGX_FeatureDiscoveryInfo begins with SDKVersion followed by
    // FeatureID.  Only advertise the SR/DLAA-shaped feature APUSR serves.
    const auto* words = static_cast<const std::uint32_t*>(featureDiscoveryInfo);
    return words[1] == kNgxFeatureSuperSampling;
}

constexpr unsigned int kNgxFeatureSupportResultSupported = 0u;
constexpr unsigned int kNgxFeatureSupportResultAdapterUnsupported = 4u;

struct VkExtensionPropertiesCompat {
    char extensionName[256]{};
    std::uint32_t specVersion = 0;
};

const char* kNgxVulkanDeviceExtensions[] = {
    "VK_KHR_shader_integer_dot_product",
    "VK_KHR_compute_shader_derivatives",
};

const VkExtensionPropertiesCompat kNgxVulkanDeviceExtensionProperties[] = {
    {
        .extensionName = "VK_KHR_shader_integer_dot_product",
        .specVersion = 1u,
    },
    {
        .extensionName = "VK_KHR_compute_shader_derivatives",
        .specVersion = 1u,
    },
};

const VkExtensionPropertiesCompat kNgxVulkanCoreDotDeviceExtensionProperties[] = {{
    .extensionName = "VK_KHR_compute_shader_derivatives",
    .specVersion = 1u,
}};

#if defined(_WIN32)
struct VkPhysicalDevicePropertiesApiVersionCompat {
    std::uint32_t apiVersion = 0;
    alignas(8) std::byte remainder[2044]{};
};

[[nodiscard]] bool vulkanPhysicalDeviceUsesCoreIntegerDotProduct(void* physicalDevice) noexcept
{
    if (physicalDevice == nullptr) {
        return false;
    }

    HMODULE vulkanModule = GetModuleHandleW(L"sl.interposer.dll");
    if (vulkanModule == nullptr) {
        vulkanModule = GetModuleHandleW(L"vulkan-1.dll");
    }
    if (vulkanModule == nullptr) {
        return false;
    }

    using GetPhysicalDevicePropertiesFn = void(WINAPI*)(void*, VkPhysicalDevicePropertiesApiVersionCompat*);
    const auto getProperties = reinterpret_cast<GetPhysicalDevicePropertiesFn>(
        GetProcAddress(vulkanModule, "vkGetPhysicalDeviceProperties"));
    if (getProperties == nullptr) {
        return false;
    }

    VkPhysicalDevicePropertiesApiVersionCompat properties{};
    getProperties(physicalDevice, &properties);
    const std::uint32_t major = (properties.apiVersion >> 22u) & 0x7fu;
    const std::uint32_t minor = (properties.apiVersion >> 12u) & 0x3ffu;
    return major > 1u || (major == 1u && minor >= 3u);
}
#endif

const char kEParamSuperSamplingAvailable[] = {'#', '\x01', '\0'};
const char kEParamWidth[] = {'#', '\x10', '\0'};
const char kEParamHeight[] = {'#', '\x11', '\0'};
const char kEParamOutWidth[] = {'#', '\x12', '\0'};
const char kEParamOutHeight[] = {'#', '\x13', '\0'};
const char kEParamSharpness[] = {'#', '\x14', '\0'};
const char kEParamOutput[] = {'#', '\x22', '\0'};
const char kEParamReset[] = {'#', '\x25', '\0'};
const char kEParamMotionVectors[] = {'#', '\x27', '\0'};
const char kEParamRectW[] = {'#', '\x2a', '\0'};
const char kEParamRectH[] = {'#', '\x2b', '\0'};
const char kEParamMvScaleX[] = {'#', '\x2c', '\0'};
const char kEParamMvScaleY[] = {'#', '\x2d', '\0'};
const char kEParamSizeInBytes[] = {'#', '\x30', '\0'};
const char kEParamCreationNodeMask[] = {'#', '\x35', '\0'};
const char kEParamVisibilityNodeMask[] = {'#', '\x36', '\0'};
const char kEParamMvOffsetX[] = {'#', '\x38', '\0'};
const char kEParamMvOffsetY[] = {'#', '\x39', '\0'};
const char kEParamResourceWidth[] = {'#', '\x3b', '\0'};
const char kEParamResourceHeight[] = {'#', '\x3c', '\0'};
const char kEParamDepth[] = {'#', '\x3d', '\0'};
const char kEParamDlssOptimalSettingsCallback[] = {'#', '\x3e', '\0'};
const char kEParamPerfQualityValue[] = {'#', '\x3f', '\0'};
const char kEParamRtxValue[] = {'#', '\x40', '\0'};
const char kEParamDlssMode[] = {'#', '\x41', '\0'};
const char kEParamOptLevel[] = {'#', '\x44', '\0'};
const char kEParamIsDevSnippetBranch[] = {'#', '\x45', '\0'};


constexpr unsigned int kNgxCompatApiVersion = 19u;
constexpr unsigned int kNgxCompatApplicationIdFallback = 0x0BADF00Du;
constexpr unsigned int kNgxCompatDriverVersionPacked = 0x23A0000u; // 570.00
constexpr unsigned int kNgxCompatDriverVersionMajor = 0x23Au; // 570
constexpr unsigned int kNgxCompatDriverVersionMinor = 0u;
constexpr unsigned int kNgxCompatGpuArchitectureTuring = 0x0160u;
constexpr unsigned int kNgxCompatSnippetVersion = 0x00030500u;

#ifndef APUSR_NGX_TRACE_COMPILED
#define APUSR_NGX_TRACE_COMPILED 1
#endif
static_assert(APUSR_NGX_TRACE_COMPILED == 1,
              "APUSR NGX trace must not be compiled out of host-facing builds");

enum class NgxTraceMode : std::uint8_t {
    Off,
    Sampled,
    Full,
};

NgxTraceMode ngxTraceMode()
{
    static const NgxTraceMode mode = [] {
        const char* value = std::getenv("APUSR_NGX_TRACE");
        if (value == nullptr || value[0] == '\0' || value[0] == '0') {
            return NgxTraceMode::Off;
        }
        if (std::strcmp(value, "full") == 0 || std::strcmp(value, "verbose") == 0 ||
            std::strcmp(value, "all") == 0) {
            return NgxTraceMode::Full;
        }
        return NgxTraceMode::Sampled;
    }();
    return mode;
}

bool ngxTraceEnabled()
{
    return ngxTraceMode() != NgxTraceMode::Off;
}

bool ngxTraceFull()
{
    return ngxTraceMode() == NgxTraceMode::Full;
}

std::string ngxDefaultTracePath()
{
    const char* explicitPath = std::getenv("APUSR_NGX_TRACE_FILE");
    if (explicitPath != nullptr && explicitPath[0] != '\0') {
        return explicitPath;
    }
    const char* explicitDir = std::getenv("APUSR_LOG_DIR");
    if (explicitDir != nullptr && explicitDir[0] != '\0') {
        return std::string(explicitDir) + "/apusr_ngx_dlss_trace.log";
    }
#if defined(_WIN32)
    return "C:/NGXLogs/APUSR/apusr_ngx_dlss_trace.log";
#else
    return "/tmp/APUSR/apusr_ngx_dlss_trace.log";
#endif
}

void ensureTraceDirectory(const std::string& path) noexcept
{
#if defined(_WIN32)
    (void)path;
    CreateDirectoryA("C:/NGXLogs", nullptr);
    const char* explicitDir = std::getenv("APUSR_LOG_DIR");
    if (explicitDir != nullptr && explicitDir[0] != '\0') {
        CreateDirectoryA(explicitDir, nullptr);
    } else {
        CreateDirectoryA("C:/NGXLogs/APUSR", nullptr);
    }
#else
    (void)path;
#endif
}

void ngxTrace(const char* message)
{
    if (!ngxTraceEnabled() || message == nullptr) {
        return;
    }
#if defined(_WIN32)
    OutputDebugStringA("[APUSR NGX] ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
#endif
    const std::string path = ngxDefaultTracePath();
    ensureTraceDirectory(path);
    static std::mutex mutex;
    static bool reset = false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!reset) {
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fclose(file);
        }
        reset = true;
    }
    if (FILE* file = std::fopen(path.c_str(), "ab")) {
        std::fprintf(file, "%s\n", message);
        std::fclose(file);
    }
}
void ngxTraceHot(const char* message, std::atomic<std::uint64_t>& counter,
                 const std::uint64_t early = 8u, const std::uint64_t period = 600u)
{
    if (!ngxTraceEnabled() || message == nullptr) {
        return;
    }
    if (ngxTraceFull()) {
        ngxTrace(message);
        return;
    }
    const std::uint64_t index = counter.fetch_add(1u, std::memory_order_relaxed);
    if (index < early || (period != 0u && (index % period) == 0u)) {
        ngxTrace(message);
    }
}

std::atomic<std::uint64_t> gNgxD3D12EvaluateTraceCounter{0};
std::atomic<std::uint64_t> gNgxVulkanEvaluateTraceCounter{0};

void ngxTraceFutureFrontendStub(const char* exportedName, const char* apiName)
{
    char message[192] = {};
    std::snprintf(
        message,
        sizeof(message),
        "%s: %s export present for compatibility/future frontend; currently unsupported by the FSR4 INT8 reference build",
        exportedName != nullptr ? exportedName : "NGX future frontend",
        apiName != nullptr ? apiName : "non-D3D12");
    ngxTrace(message);
}

struct NgxFeatureRequirementCompat {
    unsigned int FeatureSupported = kNgxFeatureSupportResultAdapterUnsupported;
    unsigned int MinHWArchitecture = 0;
    char MinOSVersion[255] = {};
};

struct NVSDK_NGX_Handle {
    unsigned int Id = 0;
    unsigned int Magic = 0;
    void* Context = nullptr;
};

constexpr unsigned int kNgxHandleMagic = 0x4E505852u; // NPXR

enum class ParameterType : std::uint32_t {
    UnsignedLongLong,
    Float,
    Double,
    UnsignedInt,
    Int,
    Pointer,
};

struct ParameterValue {
    ParameterType type = ParameterType::Pointer;
    unsigned long long u64 = 0;
    double f64 = 0.0;
    void* pointer = nullptr;
};

enum class NgxKnownParameter : std::uint16_t {
    Color,
    Input1,
    Depth,
    EParamDepth,
    DepthHighRes,
    MotionVectors,
    EParamMotionVectors,
    ExposureTexture,
    ReactiveMask,
    TransparencyMask,
    DisocclusionMask,
    Output,
    EParamOutput,
    Width,
    EParamWidth,
    ResourceWidth,
    EParamResourceWidth,
    RenderSubrectWidth,
    RectW,
    EParamRectW,
    Height,
    EParamHeight,
    ResourceHeight,
    EParamResourceHeight,
    RenderSubrectHeight,
    RectH,
    EParamRectH,
    OutWidth,
    EParamOutWidth,
    OutWidthDotted,
    ResourceOutWidth,
    OutRectW,
    TargetWidth,
    OutHeight,
    EParamOutHeight,
    OutHeightDotted,
    ResourceOutHeight,
    OutRectH,
    TargetHeight,
    PerfQualityValue,
    EParamPerfQualityValue,
    DlssPerfQualityValue,
    FeatureCreateFlags,
    FeatureCreateFlagsShort,
    JitterX,
    JitterY,
    MvScaleX,
    EParamMvScaleX,
    MvScaleY,
    EParamMvScaleY,
    Reset,
    EParamReset,
    Sharpness,
    EParamSharpness,
    PreExposure,
    DlssExposureScale,
    SizeInBytes,
    EParamSizeInBytes,
    DlssMode,
    EParamDlssMode,
    Count,
};

constexpr std::size_t kNgxKnownParameterCount = static_cast<std::size_t>(NgxKnownParameter::Count);

[[nodiscard]] bool cstrEqual(const char* a, const char* b) noexcept
{
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

[[nodiscard]] bool cstrEqualLiteral(const char* name, const char* literal) noexcept
{
    return name == literal || cstrEqual(name, literal);
}

[[nodiscard]] NgxKnownParameter ngxKnownParameterForName(const char* name) noexcept
{
    if (name == nullptr || name[0] == '\0') {
        return NgxKnownParameter::Count;
    }

    if (name[0] == '#') {
        if (cstrEqual(name, kEParamWidth)) { return NgxKnownParameter::EParamWidth; }
        if (cstrEqual(name, kEParamHeight)) { return NgxKnownParameter::EParamHeight; }
        if (cstrEqual(name, kEParamOutWidth)) { return NgxKnownParameter::EParamOutWidth; }
        if (cstrEqual(name, kEParamOutHeight)) { return NgxKnownParameter::EParamOutHeight; }
        if (cstrEqual(name, kEParamSharpness)) { return NgxKnownParameter::EParamSharpness; }
        if (cstrEqual(name, kEParamOutput)) { return NgxKnownParameter::EParamOutput; }
        if (cstrEqual(name, kEParamReset)) { return NgxKnownParameter::EParamReset; }
        if (cstrEqual(name, kEParamMotionVectors)) { return NgxKnownParameter::EParamMotionVectors; }
        if (cstrEqual(name, kEParamRectW)) { return NgxKnownParameter::EParamRectW; }
        if (cstrEqual(name, kEParamRectH)) { return NgxKnownParameter::EParamRectH; }
        if (cstrEqual(name, kEParamMvScaleX)) { return NgxKnownParameter::EParamMvScaleX; }
        if (cstrEqual(name, kEParamMvScaleY)) { return NgxKnownParameter::EParamMvScaleY; }
        if (cstrEqual(name, kEParamSizeInBytes)) { return NgxKnownParameter::EParamSizeInBytes; }
        if (cstrEqual(name, kEParamResourceWidth)) { return NgxKnownParameter::EParamResourceWidth; }
        if (cstrEqual(name, kEParamResourceHeight)) { return NgxKnownParameter::EParamResourceHeight; }
        if (cstrEqual(name, kEParamDepth)) { return NgxKnownParameter::EParamDepth; }
        if (cstrEqual(name, kEParamPerfQualityValue)) { return NgxKnownParameter::EParamPerfQualityValue; }
        if (cstrEqual(name, kEParamDlssMode)) { return NgxKnownParameter::EParamDlssMode; }
        return NgxKnownParameter::Count;
    }

    switch (name[0]) {
    case 'C':
        if (cstrEqualLiteral(name, "Color")) { return NgxKnownParameter::Color; }
        break;
    case 'D':
        if (cstrEqualLiteral(name, "Depth")) { return NgxKnownParameter::Depth; }
        if (cstrEqualLiteral(name, "DepthHighRes")) { return NgxKnownParameter::DepthHighRes; }
        if (cstrEqualLiteral(name, "DLSS.Input.Bias.Current.Color.Mask")) { return NgxKnownParameter::ReactiveMask; }
        if (cstrEqualLiteral(name, "DLSS.DisocclusionMask")) { return NgxKnownParameter::DisocclusionMask; }
        if (cstrEqualLiteral(name, "DLSS.Render.Subrect.Dimensions.Width")) { return NgxKnownParameter::RenderSubrectWidth; }
        if (cstrEqualLiteral(name, "DLSS.Render.Subrect.Dimensions.Height")) { return NgxKnownParameter::RenderSubrectHeight; }
        if (cstrEqualLiteral(name, "DLSS.Feature.Create.Flags")) { return NgxKnownParameter::FeatureCreateFlags; }
        if (cstrEqualLiteral(name, "DLSS.PerfQualityValue")) { return NgxKnownParameter::DlssPerfQualityValue; }
        if (cstrEqualLiteral(name, "DLSS.Pre.Exposure")) { return NgxKnownParameter::PreExposure; }
        if (cstrEqualLiteral(name, "DLSS.Exposure.Scale")) { return NgxKnownParameter::DlssExposureScale; }
        if (cstrEqualLiteral(name, "DLSSMode")) { return NgxKnownParameter::DlssMode; }
        break;
    case 'E':
        if (cstrEqualLiteral(name, "ExposureTexture")) { return NgxKnownParameter::ExposureTexture; }
        break;
    case 'F':
        if (cstrEqualLiteral(name, "Feature.Create.Flags")) { return NgxKnownParameter::FeatureCreateFlagsShort; }
        break;
    case 'H':
        if (cstrEqualLiteral(name, "Height")) { return NgxKnownParameter::Height; }
        break;
    case 'I':
        if (cstrEqualLiteral(name, "Input1")) { return NgxKnownParameter::Input1; }
        break;
    case 'J':
        if (cstrEqualLiteral(name, "Jitter.Offset.X")) { return NgxKnownParameter::JitterX; }
        if (cstrEqualLiteral(name, "Jitter.Offset.Y")) { return NgxKnownParameter::JitterY; }
        break;
    case 'M':
        if (cstrEqualLiteral(name, "MotionVectors")) { return NgxKnownParameter::MotionVectors; }
        if (cstrEqualLiteral(name, "MV.Scale.X")) { return NgxKnownParameter::MvScaleX; }
        if (cstrEqualLiteral(name, "MV.Scale.Y")) { return NgxKnownParameter::MvScaleY; }
        break;
    case 'O':
        if (cstrEqualLiteral(name, "Output")) { return NgxKnownParameter::Output; }
        if (cstrEqualLiteral(name, "OutWidth")) { return NgxKnownParameter::OutWidth; }
        if (cstrEqualLiteral(name, "OutHeight")) { return NgxKnownParameter::OutHeight; }
        if (cstrEqualLiteral(name, "Out.Width")) { return NgxKnownParameter::OutWidthDotted; }
        if (cstrEqualLiteral(name, "Out.Height")) { return NgxKnownParameter::OutHeightDotted; }
        if (cstrEqualLiteral(name, "OutRect.W")) { return NgxKnownParameter::OutRectW; }
        if (cstrEqualLiteral(name, "OutRect.H")) { return NgxKnownParameter::OutRectH; }
        break;
    case 'P':
        if (cstrEqualLiteral(name, "PerfQualityValue")) { return NgxKnownParameter::PerfQualityValue; }
        break;
    case 'R':
        if (cstrEqualLiteral(name, "Reset")) { return NgxKnownParameter::Reset; }
        if (cstrEqualLiteral(name, "ResourceWidth")) { return NgxKnownParameter::ResourceWidth; }
        if (cstrEqualLiteral(name, "ResourceHeight")) { return NgxKnownParameter::ResourceHeight; }
        if (cstrEqualLiteral(name, "ResourceOutWidth")) { return NgxKnownParameter::ResourceOutWidth; }
        if (cstrEqualLiteral(name, "ResourceOutHeight")) { return NgxKnownParameter::ResourceOutHeight; }
        if (cstrEqualLiteral(name, "Rect.W")) { return NgxKnownParameter::RectW; }
        if (cstrEqualLiteral(name, "Rect.H")) { return NgxKnownParameter::RectH; }
        break;
    case 'S':
        if (cstrEqualLiteral(name, "Sharpness")) { return NgxKnownParameter::Sharpness; }
        if (cstrEqualLiteral(name, "SizeInBytes")) { return NgxKnownParameter::SizeInBytes; }
        break;
    case 'T':
        if (cstrEqualLiteral(name, "TargetWidth")) { return NgxKnownParameter::TargetWidth; }
        if (cstrEqualLiteral(name, "TargetHeight")) { return NgxKnownParameter::TargetHeight; }
        if (cstrEqualLiteral(name, "TransparencyMask")) { return NgxKnownParameter::TransparencyMask; }
        break;
    case 'W':
        if (cstrEqualLiteral(name, "Width")) { return NgxKnownParameter::Width; }
        break;
    }
    return NgxKnownParameter::Count;
}

[[nodiscard]] std::size_t knownParameterIndex(const NgxKnownParameter slot) noexcept
{
    return static_cast<std::size_t>(slot);
}

struct NVSDK_NGX_Parameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;
    virtual NgxResult Get(const char* name, unsigned long long* outValue) const = 0;
    virtual NgxResult Get(const char* name, float* outValue) const = 0;
    virtual NgxResult Get(const char* name, double* outValue) const = 0;
    virtual NgxResult Get(const char* name, unsigned int* outValue) const = 0;
    virtual NgxResult Get(const char* name, int* outValue) const = 0;
    virtual NgxResult Get(const char* name, ID3D11Resource** outValue) const = 0;
    virtual NgxResult Get(const char* name, ID3D12Resource** outValue) const = 0;
    virtual NgxResult Get(const char* name, void** outValue) const = 0;
    virtual void Reset() = 0;
protected:
    virtual ~NVSDK_NGX_Parameter() = default;
};

class NgxParameterStore final : public NVSDK_NGX_Parameter {
public:
    void Set(const char* name, unsigned long long value) override { set(name, ParameterValue{.type = ParameterType::UnsignedLongLong, .u64 = value}); }
    void Set(const char* name, float value) override { set(name, ParameterValue{.type = ParameterType::Float, .f64 = static_cast<double>(value)}); }
    void Set(const char* name, double value) override { set(name, ParameterValue{.type = ParameterType::Double, .f64 = value}); }
    void Set(const char* name, unsigned int value) override { set(name, ParameterValue{.type = ParameterType::UnsignedInt, .u64 = value}); }
    void Set(const char* name, int value) override { set(name, ParameterValue{.type = ParameterType::Int, .u64 = static_cast<unsigned long long>(static_cast<long long>(value))}); }
    void Set(const char* name, ID3D11Resource* value) override { setPointer(name, value); }
    void Set(const char* name, ID3D12Resource* value) override { setPointer(name, value); }
    void Set(const char* name, void* value) override { setPointer(name, value); }

    NgxResult Get(const char* name, unsigned long long* outValue) const override
    {
        if (outValue == nullptr) { return kNgxFailInvalidParameter; }
        const ParameterValue* value = find(name);
        if (value == nullptr) { return kNgxFailInvalidParameter; }
        if (value->type == ParameterType::Float || value->type == ParameterType::Double) {
            *outValue = static_cast<unsigned long long>(value->f64);
        } else {
            *outValue = value->u64;
        }
        return kNgxSuccess;
    }

    NgxResult Get(const char* name, float* outValue) const override
    {
        if (outValue == nullptr) { return kNgxFailInvalidParameter; }
        const ParameterValue* value = find(name);
        if (value == nullptr) { return kNgxFailInvalidParameter; }
        *outValue = value->type == ParameterType::Pointer ? 0.0f : static_cast<float>(value->type == ParameterType::Float || value->type == ParameterType::Double ? value->f64 : static_cast<double>(value->u64));
        return kNgxSuccess;
    }

    NgxResult Get(const char* name, double* outValue) const override
    {
        if (outValue == nullptr) { return kNgxFailInvalidParameter; }
        const ParameterValue* value = find(name);
        if (value == nullptr) { return kNgxFailInvalidParameter; }
        *outValue = value->type == ParameterType::Pointer ? 0.0 : (value->type == ParameterType::Float || value->type == ParameterType::Double ? value->f64 : static_cast<double>(value->u64));
        return kNgxSuccess;
    }

    NgxResult Get(const char* name, unsigned int* outValue) const override
    {
        if (outValue == nullptr) { return kNgxFailInvalidParameter; }
        unsigned long long value = 0;
        const NgxResult result = Get(name, &value);
        if (result != kNgxSuccess) { return result; }
        *outValue = static_cast<unsigned int>(value);
        return kNgxSuccess;
    }

    NgxResult Get(const char* name, int* outValue) const override
    {
        if (outValue == nullptr) { return kNgxFailInvalidParameter; }
        unsigned long long value = 0;
        const NgxResult result = Get(name, &value);
        if (result != kNgxSuccess) { return result; }
        *outValue = static_cast<int>(static_cast<long long>(value));
        return kNgxSuccess;
    }

    NgxResult Get(const char* name, ID3D11Resource** outValue) const override { return getPointer(name, reinterpret_cast<void**>(outValue)); }
    NgxResult Get(const char* name, ID3D12Resource** outValue) const override { return getPointer(name, reinterpret_cast<void**>(outValue)); }
    NgxResult Get(const char* name, void** outValue) const override { return getPointer(name, outValue); }

    void Reset() override
    {
        knownPresent_.fill(false);
        values_.clear();
    }

private:
    void set(const char* name, ParameterValue value)
    {
        if (name == nullptr || name[0] == '\0') {
            return;
        }
        const NgxKnownParameter slot = ngxKnownParameterForName(name);
        if (slot != NgxKnownParameter::Count) {
            const std::size_t index = knownParameterIndex(slot);
            knownValues_[index] = value;
            knownPresent_[index] = true;
            return;
        }
        values_[std::string(name)] = value;
    }

    void setPointer(const char* name, void* value)
    {
        set(name, ParameterValue{.type = ParameterType::Pointer, .pointer = value});
    }

    [[nodiscard]] const ParameterValue* find(const char* name) const
    {
        if (name == nullptr) {
            return nullptr;
        }
        const NgxKnownParameter slot = ngxKnownParameterForName(name);
        if (slot != NgxKnownParameter::Count) {
            const std::size_t index = knownParameterIndex(slot);
            return knownPresent_[index] ? &knownValues_[index] : nullptr;
        }
        auto found = values_.find(name);
        return found == values_.end() ? nullptr : &found->second;
    }

    NgxResult getPointer(const char* name, void** outValue) const
    {
        if (outValue == nullptr) { return kNgxFailInvalidParameter; }
        const ParameterValue* value = find(name);
        if (value == nullptr || value->type != ParameterType::Pointer) { return kNgxFailInvalidParameter; }
        *outValue = value->pointer;
        return kNgxSuccess;
    }

    std::array<ParameterValue, kNgxKnownParameterCount> knownValues_{};
    std::array<bool, kNgxKnownParameterCount> knownPresent_{};
    std::unordered_map<std::string, ParameterValue> values_;
};

struct NgxFeatureContext {
    std::mutex mutex;
    NVSDK_NGX_Handle handle;
    apusr::FrameInputApi api = apusr::FrameInputApi::Dx12;
    apusr::frontends::NgxDlssCreateDesc create;
    std::unique_ptr<apusr::backends::Dx12Fsr4ReferenceRuntime> runtime;
    std::unique_ptr<apusr::backends::VulkanFsr4ReferenceRuntime> vulkanRuntime;
    apusr::NativeHandle resolvedCommandQueue;
    std::uint64_t queueResolveAttempts = 0;
    std::uint64_t pipelineKey = 0;
    bool referenceHasHistory = false;
    float referencePreviousPreExposure = 1.0f;
    apusr::Jitter referencePreviousJitter{};
    std::uint64_t nextFrameIndex = 0;
    std::string lastVulkanFailureFingerprint;
    bool vulkanContractLogged = false;
};

struct NgxGlobalState {
    std::mutex mutex;
    // The D3D12 and Vulkan device states are deliberately separate. Keeping the
    // D3D12 slot strongly typed prevents the NGX/Streamline compatibility path
    // from accidentally treating an opaque host pointer as a native D3D12 device.
    ID3D12Device* device = nullptr;
    void* vulkanInstance = nullptr;
    void* vulkanPhysicalDevice = nullptr;
    void* vulkanDevice = nullptr;
    std::uint32_t applicationId = kNgxCompatApplicationIdFallback;
    void* infoCallback = nullptr;
    void* telemetryEvaluateCallback = nullptr;
    std::uint32_t nextHandleId = 1;
    std::unordered_map<std::uint32_t, std::shared_ptr<NgxFeatureContext>> features;
    std::atomic<std::uint64_t> initCalls{0};
    std::atomic<std::uint64_t> evaluateCalls{0};
    std::atomic<std::uint64_t> failedEvaluateCalls{0};
    std::string lastError;
};

NgxGlobalState& ngxState()
{
    static NgxGlobalState state;
    return state;
}

[[nodiscard]] NgxFeatureContext* featureContextFromHandle(const NVSDK_NGX_Handle* handle) noexcept
{
    if (handle == nullptr || handle->Magic != kNgxHandleMagic || handle->Context == nullptr) {
        return nullptr;
    }
    return static_cast<NgxFeatureContext*>(handle->Context);
}

[[nodiscard]] const char* keyColor() { return "Color"; }
[[nodiscard]] const char* keyDepth() { return "Depth"; }
[[nodiscard]] const char* keyMotionVectors() { return "MotionVectors"; }
[[nodiscard]] const char* keyExposure() { return "ExposureTexture"; }
[[nodiscard]] const char* keyReactiveMask() { return "DLSS.Input.Bias.Current.Color.Mask"; }
[[nodiscard]] const char* keyOutput() { return "Output"; }
[[nodiscard]] const char* keyWidth() { return "Width"; }
[[nodiscard]] const char* keyHeight() { return "Height"; }
[[nodiscard]] const char* keyOutWidth() { return "OutWidth"; }
[[nodiscard]] const char* keyOutHeight() { return "OutHeight"; }
[[nodiscard]] const char* keyPerfQuality() { return "PerfQualityValue"; }
[[nodiscard]] const char* keyFeatureCreateFlags() { return "DLSS.Feature.Create.Flags"; }
[[nodiscard]] const char* keyJitterX() { return "Jitter.Offset.X"; }
[[nodiscard]] const char* keyJitterY() { return "Jitter.Offset.Y"; }
[[nodiscard]] const char* keyMvScaleX() { return "MV.Scale.X"; }
[[nodiscard]] const char* keyMvScaleY() { return "MV.Scale.Y"; }
[[nodiscard]] const char* keyReset() { return "Reset"; }
[[nodiscard]] const char* keySharpness() { return "Sharpness"; }
[[nodiscard]] const char* keyPreExposure() { return "DLSS.Pre.Exposure"; }

void setLastErrorUnlocked(NgxGlobalState& state, const apusr::Status& status)
{
    if (status.succeeded()) {
        state.lastError.clear();
        return;
    }
    state.lastError = status.message.empty() ? apusr::statusCodeName(status.code) : status.message;
    if (ngxTraceEnabled()) {
        ngxTrace((std::string("APUSR error: ") + state.lastError).c_str());
    }
}

void logVulkanEvaluateFailureOnce(NgxFeatureContext& feature, const char* stage, const apusr::Status& status)
{
    if (status.succeeded()) {
        feature.lastVulkanFailureFingerprint.clear();
        return;
    }
    const std::string message = status.message.empty() ? apusr::statusCodeName(status.code) : status.message;
    const std::string fingerprint = std::string(stage) + "|" + std::to_string(static_cast<unsigned>(status.code)) + "|" + message;
    if (feature.lastVulkanFailureFingerprint == fingerprint) {
        return;
    }
    feature.lastVulkanFailureFingerprint = fingerprint;
    apusr::host_discovery::eventf(
        "ngx", "vulkan.evaluate_failure",
        "stage=%s status=%s message=%s",
        stage, apusr::statusCodeName(status.code), message.c_str());
}

void logVulkanContractOnce(NgxFeatureContext& feature, const apusr::frontends::NgxDlssVulkanEvaluateDesc& desc)
{
    if (feature.vulkanContractLogged) {
        return;
    }
    feature.vulkanContractLogged = true;
    apusr::host_discovery::eventf(
        "ngx", "vulkan.evaluate_contract",
        "render=%ux%u display=%ux%u mv=%ux%u formats=color:%u depth:%u mv:%u output:%u backing=color:%ux%u depth:%ux%u mv:%ux%u output:%ux%u output_rw=%u subrect=color:%u,%u depth:%u,%u mv:%u,%u output:%u,%u",
        desc.renderSize.width, desc.renderSize.height,
        desc.displaySize.width, desc.displaySize.height,
        desc.motionVectorSize.width, desc.motionVectorSize.height,
        desc.color.nativeFormat, desc.depth.nativeFormat, desc.motionVectors.nativeFormat, desc.output.nativeFormat,
        desc.color.extent.width, desc.color.extent.height,
        desc.depth.extent.width, desc.depth.extent.height,
        desc.motionVectors.extent.width, desc.motionVectors.extent.height,
        desc.output.extent.width, desc.output.extent.height,
        desc.output.readWrite ? 1u : 0u,
        desc.colorSubrectBaseX, desc.colorSubrectBaseY,
        desc.depthSubrectBaseX, desc.depthSubrectBaseY,
        desc.motionVectorSubrectBaseX, desc.motionVectorSubrectBaseY,
        desc.outputSubrectBaseX, desc.outputSubrectBaseY);
}

[[nodiscard]] apusr::frontends::NgxDlssPerfQualityMode perfQualityFromInt(const int value) noexcept
{
    switch (value) {
    case 0: return apusr::frontends::NgxDlssPerfQualityMode::MaxPerformance;
    case 1: return apusr::frontends::NgxDlssPerfQualityMode::Balanced;
    case 2: return apusr::frontends::NgxDlssPerfQualityMode::MaxQuality;
    case 3: return apusr::frontends::NgxDlssPerfQualityMode::UltraPerformance;
    case 4: return apusr::frontends::NgxDlssPerfQualityMode::UltraQuality;
    case 5: return apusr::frontends::NgxDlssPerfQualityMode::NativeAA;
    default: return apusr::frontends::NgxDlssPerfQualityMode::Balanced;
    }
}

bool getInt(const NVSDK_NGX_Parameter* params, const char* name, int& value)
{
    if (params == nullptr) {
        return false;
    }
    return params->Get(name, &value) == kNgxSuccess;
}

bool getUInt(const NVSDK_NGX_Parameter* params, const char* name, unsigned int& value)
{
    if (params == nullptr) {
        return false;
    }
    return params->Get(name, &value) == kNgxSuccess;
}

bool getFloat(const NVSDK_NGX_Parameter* params, const char* name, float& value)
{
    if (params == nullptr) {
        return false;
    }
    return params->Get(name, &value) == kNgxSuccess;
}

bool getPointer(const NVSDK_NGX_Parameter* params, const char* name, void*& value)
{
    if (params == nullptr) {
        return false;
    }
    return params->Get(name, &value) == kNgxSuccess;
}

bool getUIntAny(const NVSDK_NGX_Parameter* params, std::initializer_list<const char*> names, unsigned int& value)
{
    for (const char* name : names) {
        if (getUInt(params, name, value)) {
            return true;
        }
    }
    return false;
}

bool getIntAny(const NVSDK_NGX_Parameter* params, std::initializer_list<const char*> names, int& value)
{
    for (const char* name : names) {
        if (getInt(params, name, value)) {
            return true;
        }
    }
    return false;
}

bool getFloatAny(const NVSDK_NGX_Parameter* params, std::initializer_list<const char*> names, float& value)
{
    for (const char* name : names) {
        if (getFloat(params, name, value)) {
            return true;
        }
    }
    return false;
}

bool getPointerAny(const NVSDK_NGX_Parameter* params, std::initializer_list<const char*> names, void*& value)
{
    for (const char* name : names) {
        if (getPointer(params, name, value)) {
            return true;
        }
    }
    return false;
}

// Minimal Vulkan ABI mirrors used by the NGX ingress seam.  APUSR deliberately
// does not require Vulkan headers in this frontend translation unit; these layouts
// match NVSDK_NGX_Resource_VK on the 64-bit game platforms APUSR targets.
struct NgxVkSubresourceRangeAbi {
    std::uint32_t aspectMask;
    std::uint32_t baseMipLevel;
    std::uint32_t levelCount;
    std::uint32_t baseArrayLayer;
    std::uint32_t layerCount;
};

struct NgxVkImageViewInfoAbi {
    std::uintptr_t imageView;
    std::uintptr_t image;
    NgxVkSubresourceRangeAbi subresourceRange;
    std::uint32_t format;
    std::uint32_t width;
    std::uint32_t height;
};

struct NgxVkBufferInfoAbi {
    std::uintptr_t buffer;
    std::uint32_t sizeInBytes;
};

union NgxVkResourceUnionAbi {
    NgxVkImageViewInfoAbi imageViewInfo;
    NgxVkBufferInfoAbi bufferInfo;
};

struct NgxVkResourceAbi {
    NgxVkResourceUnionAbi resource;
    std::uint32_t type;
    bool readWrite;
};

static_assert(sizeof(void*) == 8, "APUSR NGX Vulkan ingress requires a 64-bit process");
static_assert(std::is_standard_layout_v<NgxVkResourceAbi> && std::is_trivially_copyable_v<NgxVkResourceAbi>);
static_assert(sizeof(NgxVkImageViewInfoAbi) == 48);
static_assert(sizeof(NgxVkResourceAbi) == 56);

[[nodiscard]] apusr::SurfaceFormat surfaceFormatFromVkFormat(const std::uint32_t format) noexcept
{
    switch (format) {
    case 9: return apusr::SurfaceFormat::R8Unorm;
    case 10: return apusr::SurfaceFormat::R8Snorm;
    case 13: return apusr::SurfaceFormat::R8Uint;
    case 14: return apusr::SurfaceFormat::R8Sint;
    case 16: return apusr::SurfaceFormat::Rg8Unorm;
    case 20: return apusr::SurfaceFormat::Rg8Uint;
    case 37: return apusr::SurfaceFormat::Rgba8Unorm;
    case 38: return apusr::SurfaceFormat::Rgba8Snorm;
    case 43: return apusr::SurfaceFormat::Rgba8Srgb;
    case 44: return apusr::SurfaceFormat::Bgra8Unorm;
    case 50: return apusr::SurfaceFormat::Bgra8Srgb;
    case 58: // VK_FORMAT_A2R10G10B10_UNORM_PACK32
    case 64: // VK_FORMAT_A2B10G10R10_UNORM_PACK32
        return apusr::SurfaceFormat::R10G10B10A2Unorm;
    case 70: return apusr::SurfaceFormat::R16Unorm;
    case 71: return apusr::SurfaceFormat::R16Snorm;
    case 74: return apusr::SurfaceFormat::R16Uint;
    case 76: return apusr::SurfaceFormat::R16Float;
    case 81: return apusr::SurfaceFormat::Rg16Uint;
    case 82: return apusr::SurfaceFormat::Rg16Sint;
    case 83: return apusr::SurfaceFormat::Rg16Float;
    case 97: return apusr::SurfaceFormat::Rgba16Float;
    case 98: return apusr::SurfaceFormat::R32Uint;
    case 100: return apusr::SurfaceFormat::R32Float;
    case 101: return apusr::SurfaceFormat::Rg32Uint;
    case 103: return apusr::SurfaceFormat::Rg32Float;
    case 106: return apusr::SurfaceFormat::Rgb32Float;
    case 107: return apusr::SurfaceFormat::Rgba32Uint;
    case 109: return apusr::SurfaceFormat::Rgba32Float;
    case 122: return apusr::SurfaceFormat::R11G11B10Float;
    case 124: return apusr::SurfaceFormat::R16Unorm; // VK_FORMAT_D16_UNORM
    case 128: return apusr::SurfaceFormat::R16Unorm; // VK_FORMAT_D16_UNORM_S8_UINT
    case 125: // VK_FORMAT_X8_D24_UNORM_PACK32
    case 126: // VK_FORMAT_D32_SFLOAT
    case 129: // VK_FORMAT_D24_UNORM_S8_UINT
    case 130: // VK_FORMAT_D32_SFLOAT_S8_UINT
        return apusr::SurfaceFormat::R32Float; // depth semantic; nativeFormat remains exact
    default: return apusr::SurfaceFormat::Unknown;
    }
}

[[nodiscard]] constexpr bool vkFormatHasDepthAspect(const std::uint32_t format) noexcept
{
    switch (format) {
    case 124: // VK_FORMAT_D16_UNORM
    case 125: // VK_FORMAT_X8_D24_UNORM_PACK32
    case 126: // VK_FORMAT_D32_SFLOAT
    case 128: // VK_FORMAT_D16_UNORM_S8_UINT
    case 129: // VK_FORMAT_D24_UNORM_S8_UINT
    case 130: // VK_FORMAT_D32_SFLOAT_S8_UINT
        return true;
    default:
        return false;
    }
}

[[nodiscard]] apusr::Status decodeNgxVulkanImageResource(
    void* raw,
    const char* name,
    const bool required,
    apusr::frontends::NgxVulkanImageResourceDesc& out)
{
    out = {};
    if (raw == nullptr) {
        return required ? apusr::Status::invalidArgument(std::string("NGX/DLSS Vulkan missing ") + name) : apusr::Status::ok();
    }
    const auto* resource = static_cast<const NgxVkResourceAbi*>(raw);
    if (resource->type != 0u) {
        return apusr::Status::invalidArgument(std::string("NGX/DLSS Vulkan ") + name + " is not an image-view resource");
    }
    const auto& image = resource->resource.imageViewInfo;
    if (image.image == 0u || image.imageView == 0u || image.width == 0u || image.height == 0u) {
        return apusr::Status::invalidArgument(std::string("NGX/DLSS Vulkan ") + name + " has an incomplete image-view descriptor");
    }
    const apusr::SurfaceFormat portableFormat = surfaceFormatFromVkFormat(image.format);
    if (portableFormat == apusr::SurfaceFormat::Unknown) {
        return apusr::Status::invalidArgument(std::string("NGX/DLSS Vulkan ") + name + " uses an unsupported VkFormat value " + std::to_string(image.format));
    }
    out = apusr::frontends::NgxVulkanImageResourceDesc{
        .image = apusr::NativeHandle{reinterpret_cast<void*>(image.image)},
        .imageView = apusr::NativeHandle{reinterpret_cast<void*>(image.imageView)},
        .format = portableFormat,
        .extent = apusr::RenderSize{.width = image.width, .height = image.height},
        .nativeFormat = image.format,
        // Streamline v2.7 can synthesize a COLOR default range when the tag did
        // not carry an explicit Vulkan subresource range. The native VkFormat
        // is authoritative for depth resources, so normalize the aspect here.
        .aspectMask = vkFormatHasDepthAspect(image.format) ? 0x2u : (image.subresourceRange.aspectMask != 0u ? image.subresourceRange.aspectMask : 0x1u),
        .baseMipLevel = image.subresourceRange.baseMipLevel,
        .levelCount = image.subresourceRange.levelCount,
        .baseArrayLayer = image.subresourceRange.baseArrayLayer,
        .layerCount = image.subresourceRange.layerCount,
        .readWrite = resource->readWrite,
    };
    return apusr::Status::ok();
}

[[nodiscard]] apusr::frontends::NgxDlssCreateDesc createDescFromParams(void* device, const NVSDK_NGX_Parameter* params)
{
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int outWidth = 0;
    unsigned int outHeight = 0;
    int quality = 1;
    unsigned int flags = 0;
    getUIntAny(params, {keyWidth(), kEParamWidth, "ResourceWidth", kEParamResourceWidth, "DLSS.Render.Subrect.Dimensions.Width", "Rect.W", kEParamRectW}, width);
    getUIntAny(params, {keyHeight(), kEParamHeight, "ResourceHeight", kEParamResourceHeight, "DLSS.Render.Subrect.Dimensions.Height", "Rect.H", kEParamRectH}, height);
    getUIntAny(params, {keyOutWidth(), kEParamOutWidth, "Out.Width", "ResourceOutWidth", "OutRect.W", "TargetWidth"}, outWidth);
    getUIntAny(params, {keyOutHeight(), kEParamOutHeight, "Out.Height", "ResourceOutHeight", "OutRect.H", "TargetHeight"}, outHeight);
    getIntAny(params, {keyPerfQuality(), kEParamPerfQualityValue, "DLSS.PerfQualityValue"}, quality);
    getUIntAny(params, {keyFeatureCreateFlags(), "Feature.Create.Flags"}, flags);
    if (outWidth == 0u) { outWidth = width; }
    if (outHeight == 0u) { outHeight = height; }
    return apusr::frontends::NgxDlssCreateDesc{
        .device = apusr::NativeHandle{device},
        .renderSize = apusr::RenderSize{.width = width, .height = height},
        .displaySize = apusr::DisplaySize{.width = outWidth, .height = outHeight},
        .qualityMode = perfQualityFromInt(quality),
        .featureCreateFlags = flags,
    };
}

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
[[nodiscard]] apusr::SurfaceFormat surfaceFormatFromDxgiResourceFormat(
    const DXGI_FORMAT format,
    const apusr::SurfaceFormat fallback) noexcept
{
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return apusr::SurfaceFormat::Rgba32Float;
    case DXGI_FORMAT_R32G32B32A32_UINT: return apusr::SurfaceFormat::Rgba32Uint;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return apusr::SurfaceFormat::Rgba16Float;
    case DXGI_FORMAT_R32G32B32_FLOAT: return apusr::SurfaceFormat::Rgb32Float;
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_TYPELESS: return apusr::SurfaceFormat::Rg32Float;
    case DXGI_FORMAT_R32G32_UINT: return apusr::SurfaceFormat::Rg32Uint;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_TYPELESS: return apusr::SurfaceFormat::Rg16Float;
    case DXGI_FORMAT_R16G16_UINT: return apusr::SurfaceFormat::Rg16Uint;
    case DXGI_FORMAT_R16G16_SINT: return apusr::SurfaceFormat::Rg16Sint;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_TYPELESS: return apusr::SurfaceFormat::Rg8Unorm;
    case DXGI_FORMAT_R8G8_UINT: return apusr::SurfaceFormat::Rg8Uint;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return apusr::SurfaceFormat::Rgba8Unorm;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return apusr::SurfaceFormat::Rgba8Srgb;
    case DXGI_FORMAT_R8G8B8A8_SNORM: return apusr::SurfaceFormat::Rgba8Snorm;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return apusr::SurfaceFormat::Bgra8Unorm;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return apusr::SurfaceFormat::Bgra8Srgb;
    case DXGI_FORMAT_R11G11B10_FLOAT: return apusr::SurfaceFormat::R11G11B10Float;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return apusr::SurfaceFormat::R10G10B10A2Unorm;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS: return apusr::SurfaceFormat::R16Float;
    case DXGI_FORMAT_R16_UINT: return apusr::SurfaceFormat::R16Uint;
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_D16_UNORM: return apusr::SurfaceFormat::R16Unorm;
    case DXGI_FORMAT_R16_SNORM: return apusr::SurfaceFormat::R16Snorm;
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return apusr::SurfaceFormat::R32Float;
    case DXGI_FORMAT_R32_UINT: return apusr::SurfaceFormat::R32Uint;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_TYPELESS: return apusr::SurfaceFormat::R8Unorm;
    case DXGI_FORMAT_R8_UINT: return apusr::SurfaceFormat::R8Uint;
    case DXGI_FORMAT_R8_SINT: return apusr::SurfaceFormat::R8Sint;
    case DXGI_FORMAT_R8_SNORM: return apusr::SurfaceFormat::R8Snorm;
    default: return fallback;
    }
}

[[nodiscard]] apusr::SurfaceFormat formatFromD3D12Resource(
    void* resource,
    const apusr::SurfaceFormat fallback) noexcept
{
    if (resource == nullptr) {
        return fallback;
    }
    return surfaceFormatFromDxgiResourceFormat(
        static_cast<ID3D12Resource*>(resource)->GetDesc().Format,
        fallback);
}
#endif

[[nodiscard]] apusr::frontends::NgxDlssEvaluateDesc evaluateDescFromParams(
    NgxFeatureContext& feature,
    void* commandList,
    const NVSDK_NGX_Parameter* params)
{
    void* color = nullptr;
    void* depth = nullptr;
    void* motionVectors = nullptr;
    void* exposure = nullptr;
    void* reactive = nullptr;
    void* output = nullptr;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    // DLSS sharpness is mapped into the same RCAS path used by the FFX
    // frontend.  Presentation-side UI quirks are handled by capability/menu
    // gating; execution should still honor a real sharpness value when the host
    // supplies one.
    float sharpness = 0.0f;
    float preExposure = 1.0f;
    int reset = 0;
    getPointerAny(params, {keyColor(), "Input1", "Color"}, color);
    getPointerAny(params, {keyDepth(), kEParamDepth, "DepthHighRes"}, depth);
    getPointerAny(params, {keyMotionVectors(), kEParamMotionVectors}, motionVectors);
    getPointer(params, keyExposure(), exposure);
    getPointerAny(params, {keyReactiveMask(), "TransparencyMask", "DLSS.DisocclusionMask"}, reactive);
    getPointerAny(params, {keyOutput(), kEParamOutput}, output);
    getFloat(params, keyJitterX(), jitterX);
    getFloat(params, keyJitterY(), jitterY);
    getFloatAny(params, {keyMvScaleX(), kEParamMvScaleX}, mvScaleX);
    getFloatAny(params, {keyMvScaleY(), kEParamMvScaleY}, mvScaleY);
    getFloatAny(params, {keySharpness(), kEParamSharpness}, sharpness);
    sharpness = std::clamp(sharpness, 0.0f, 1.0f);
    getFloatAny(params, {keyPreExposure(), "DLSS.Exposure.Scale"}, preExposure);
    getIntAny(params, {keyReset(), kEParamReset}, reset);

    unsigned int renderWidth = feature.create.renderSize.width;
    unsigned int renderHeight = feature.create.renderSize.height;
    getUIntAny(params, {"DLSS.Render.Subrect.Dimensions.Width", "Rect.W", kEParamRectW, keyWidth(), kEParamWidth}, renderWidth);
    getUIntAny(params, {"DLSS.Render.Subrect.Dimensions.Height", "Rect.H", kEParamRectH, keyHeight(), kEParamHeight}, renderHeight);

    // The output/display extent is a feature-creation contract, not mutable
    // per-evaluate parameter state. Stock sl.dlss reuses its NGX parameter
    // object for DLSS optimal-settings queries; those callbacks deliberately
    // overwrite OutWidth/OutHeight with the recommended *render* dimensions.
    // Reading those scratch values during evaluation collapses display size to
    // render size (for example 2293x960 -> 2293x960 instead of 3440x1440).
    // Vulkan already preserves feature.create.displaySize for this reason.
    const unsigned int displayWidth = feature.create.displaySize.width;
    const unsigned int displayHeight = feature.create.displaySize.height;

    apusr::frontends::NgxDlssEvaluateDesc out{
        .commandList = apusr::NativeHandle{commandList},
        .device = feature.create.device,
        .color = apusr::NativeHandle{color},
        .depth = apusr::NativeHandle{depth},
        .motionVectors = apusr::NativeHandle{motionVectors},
        .exposure = apusr::NativeHandle{exposure},
        .reactiveMask = apusr::NativeHandle{reactive},
        .output = apusr::NativeHandle{output},
        .renderSize = apusr::RenderSize{.width = renderWidth, .height = renderHeight},
        .displaySize = apusr::DisplaySize{.width = displayWidth, .height = displayHeight},
        .motionVectorSize = (feature.create.featureCreateFlags & apusr::frontends::kNgxDlssFeatureCreateFlagMvLowRes) != 0u
            ? apusr::RenderSize{.width = renderWidth, .height = renderHeight}
            : apusr::RenderSize{.width = displayWidth, .height = displayHeight},
        .jitter = apusr::Jitter{.x = jitterX, .y = jitterY},
        .previousJitter = feature.referencePreviousJitter,
        .motionVectorScale = apusr::MotionVectorScale{.x = mvScaleX, .y = mvScaleY},
        .qualityMode = feature.create.qualityMode,
        .featureCreateFlags = feature.create.featureCreateFlags,
        .frameIndex = feature.nextFrameIndex++,
        .resetHistory = reset != 0,
        .sharpeningRequested = sharpness > 0.0f,
        .sharpness = sharpness,
        .preExposure = preExposure,
    };
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    unsigned int streamlinePreparedStates = 0u;
    if (getUInt(params, "APUSR.Streamline.D3D12.PreparedStates", streamlinePreparedStates) &&
        streamlinePreparedStates != 0u) {
        // Stock sl.dlss transitions all DLSS inputs to chi::eTextureRead and
        // the output to chi::eStorageRW immediately before calling its NGX
        // context. On D3D12 those map to PIXEL_SHADER_RESOURCE and
        // UNORDERED_ACCESS respectively. Preserve that exact StateBefore
        // contract instead of pretending borrowed resources arrive in COMMON.
        constexpr std::uint64_t inputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        constexpr std::uint64_t outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        out.colorState = color != nullptr ? inputState : 0u;
        out.depthState = depth != nullptr ? inputState : 0u;
        out.motionVectorState = motionVectors != nullptr ? inputState : 0u;
        out.exposureState = exposure != nullptr ? inputState : 0u;
        out.reactiveMaskState = reactive != nullptr ? inputState : 0u;
        out.outputState = output != nullptr ? outputState : 0u;
    }

    // NGX does not carry D3D12 resource formats as separate parameters.  The
    // actual ID3D12Resource descriptors are authoritative; using frontend
    // defaults here can manufacture an incompatible SRV/UAV for a game-owned
    // texture (for example R16G16B16A16 over an R8G8B8A8 output).
    out.colorFormat = formatFromD3D12Resource(color, out.colorFormat);
    out.depthFormat = formatFromD3D12Resource(depth, out.depthFormat);
    out.motionVectorFormat = formatFromD3D12Resource(motionVectors, out.motionVectorFormat);
    out.exposureFormat = formatFromD3D12Resource(exposure, out.exposureFormat);
    out.reactiveMaskFormat = formatFromD3D12Resource(reactive, out.reactiveMaskFormat);
    out.outputFormat = formatFromD3D12Resource(output, out.outputFormat);
#endif
    return out;
}

[[nodiscard]] apusr::Status resolveVulkanHostIngress(
    apusr::frontends::NgxDlssVulkanEvaluateDesc& desc);

[[nodiscard]] apusr::Status vulkanEvaluateDescFromParams(
    NgxFeatureContext& feature,
    void* commandBuffer,
    const NVSDK_NGX_Parameter* params,
    apusr::frontends::NgxDlssVulkanEvaluateDesc& out)
{
    unsigned int renderWidth = feature.create.renderSize.width;
    unsigned int renderHeight = feature.create.renderSize.height;
    getUInt(params, "DLSS.Render.Subrect.Dimensions.Width", renderWidth);
    getUInt(params, "DLSS.Render.Subrect.Dimensions.Height", renderHeight);

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    float sharpness = 0.0f;
    float preExposure = 1.0f;
    int reset = 0;
    getFloat(params, keyJitterX(), jitterX);
    getFloat(params, keyJitterY(), jitterY);
    getFloat(params, keyMvScaleX(), mvScaleX);
    getFloat(params, keyMvScaleY(), mvScaleY);
    getFloat(params, keySharpness(), sharpness);
    getFloat(params, keyPreExposure(), preExposure);
    getInt(params, keyReset(), reset);

    out = {};
    out.commandBuffer = apusr::NativeHandle{commandBuffer};
    out.device = feature.create.device;
    out.renderSize = apusr::RenderSize{.width = renderWidth, .height = renderHeight};
    out.displaySize = feature.create.displaySize;
    out.motionVectorSize = (feature.create.featureCreateFlags & apusr::frontends::kNgxDlssFeatureCreateFlagMvLowRes) != 0u
        ? apusr::RenderSize{.width = renderWidth, .height = renderHeight}
        : apusr::RenderSize{.width = feature.create.displaySize.width, .height = feature.create.displaySize.height};
    out.jitter = apusr::Jitter{.x = jitterX, .y = jitterY};
    out.previousJitter = feature.referencePreviousJitter;
    out.motionVectorScale = apusr::MotionVectorScale{.x = mvScaleX, .y = mvScaleY};
    out.qualityMode = feature.create.qualityMode;
    out.featureCreateFlags = feature.create.featureCreateFlags;
    out.frameIndex = feature.nextFrameIndex++;
    out.resetHistory = reset != 0;
    out.sharpeningRequested = sharpness > 0.0f;
    out.sharpness = sharpness;
    out.preExposure = preExposure;

    apusr::Status ingressStatus = resolveVulkanHostIngress(out);
    if (!ingressStatus.succeeded()) {
        return ingressStatus;
    }

    getUInt(params, "DLSS.Input.Color.Subrect.Base.X", out.colorSubrectBaseX);
    getUInt(params, "DLSS.Input.Color.Subrect.Base.Y", out.colorSubrectBaseY);
    getUInt(params, "DLSS.Input.Depth.Subrect.Base.X", out.depthSubrectBaseX);
    getUInt(params, "DLSS.Input.Depth.Subrect.Base.Y", out.depthSubrectBaseY);
    getUInt(params, "DLSS.Input.MV.Subrect.Base.X", out.motionVectorSubrectBaseX);
    getUInt(params, "DLSS.Input.MV.Subrect.Base.Y", out.motionVectorSubrectBaseY);
    getUInt(params, "DLSS.Output.Subrect.Base.X", out.outputSubrectBaseX);
    getUInt(params, "DLSS.Output.Subrect.Base.Y", out.outputSubrectBaseY);
    getUInt(params, "DLSS.Input.Translucency.Subrect.Base.X", out.transparencyMaskSubrectBaseX);
    getUInt(params, "DLSS.Input.Translucency.Subrect.Base.Y", out.transparencyMaskSubrectBaseY);
    getUInt(params, "DLSS.Input.Bias.Current.Color.Subrect.Base.X", out.biasCurrentColorMaskSubrectBaseX);
    getUInt(params, "DLSS.Input.Bias.Current.Color.Subrect.Base.Y", out.biasCurrentColorMaskSubrectBaseY);

    struct ResourceInput {
        const char* key;
        const char* label;
        bool required;
        apusr::frontends::NgxVulkanImageResourceDesc* output;
    };
    ResourceInput resources[] = {
        {keyColor(), "Color", true, &out.color},
        {keyDepth(), "Depth", true, &out.depth},
        {keyMotionVectors(), "MotionVectors", true, &out.motionVectors},
        {keyOutput(), "Output", true, &out.output},
        {keyExposure(), "ExposureTexture", false, &out.exposure},
        {"TransparencyMask", "TransparencyMask", false, &out.transparencyMask},
        {"DLSS.Input.Bias.Current.Color.Mask", "BiasCurrentColorMask", false, &out.biasCurrentColorMask},
        {"AnimatedTextureMask", "AnimatedTextureMask", false, &out.animatedTextureMask},
        {"IsParticleMask", "ParticleMask", false, &out.particleMask},
    };

    for (auto& input : resources) {
        void* raw = nullptr;
        getPointer(params, input.key, raw);
        apusr::Status status = decodeNgxVulkanImageResource(raw, input.label, input.required, *input.output);
        if (!status.succeeded()) {
            return status;
        }
    }
    return apusr::Status::ok();
}

[[nodiscard]] NgxResult statusToNgxResult(const apusr::Status& status)
{
    switch (status.code) {
    case apusr::StatusCode::Ok:
        return kNgxSuccess;
    case apusr::StatusCode::InvalidArgument:
        return kNgxFailInvalidParameter;
    case apusr::StatusCode::UnsupportedBackend:
    case apusr::StatusCode::MissingBackend:
    case apusr::StatusCode::NotImplemented:
        return kNgxFailPlatformError;
    case apusr::StatusCode::RuntimeFailure:
        return kNgxFail;
    }
    return kNgxFail;
}

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
[[nodiscard]] apusr::Status validateLiveD3D12ExecutionObjects(
    ID3D12Device* expectedDevice,
    ID3D12GraphicsCommandList* commandList,
    const char* phase)
{
    if (expectedDevice == nullptr) {
        return apusr::Status::invalidArgument("NGX/DLSS DX12 execution requires a device");
    }

    const HRESULT removedReason = expectedDevice->GetDeviceRemovedReason();
    if (FAILED(removedReason)) {
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "NGX/DLSS DX12 %s rejected an already-removed device (HRESULT 0x%08X)",
                      phase != nullptr ? phase : "execution",
                      static_cast<unsigned int>(removedReason));
        apusr::host_discovery::eventf("ngx", "D3D12.device_removed",
                                     "phase=%s device=%p hr=0x%08X",
                                     phase != nullptr ? phase : "execution",
                                     static_cast<void*>(expectedDevice),
                                     static_cast<unsigned int>(removedReason));
        return apusr::Status::runtimeFailure(message);
    }

    if (commandList == nullptr) {
        return apusr::Status::ok();
    }

    ID3D12Device* commandDevice = nullptr;
    const HRESULT getDeviceHr = commandList->GetDevice(IID_PPV_ARGS(&commandDevice));
    if (FAILED(getDeviceHr) || commandDevice == nullptr) {
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "NGX/DLSS DX12 %s could not resolve the command-list device (HRESULT 0x%08X)",
                      phase != nullptr ? phase : "execution",
                      static_cast<unsigned int>(getDeviceHr));
        return apusr::Status::runtimeFailure(message);
    }

    IUnknown* expectedIdentity = nullptr;
    IUnknown* commandIdentity = nullptr;
    const HRESULT expectedIdentityHr = expectedDevice->QueryInterface(IID_PPV_ARGS(&expectedIdentity));
    const HRESULT commandIdentityHr = commandDevice->QueryInterface(IID_PPV_ARGS(&commandIdentity));
    const bool sameDevice = SUCCEEDED(expectedIdentityHr) && SUCCEEDED(commandIdentityHr) &&
                            expectedIdentity != nullptr && commandIdentity != nullptr &&
                            expectedIdentity == commandIdentity;
    if (expectedIdentity != nullptr) expectedIdentity->Release();
    if (commandIdentity != nullptr) commandIdentity->Release();
    commandDevice->Release();

    if (!sameDevice) {
        apusr::host_discovery::eventf("ngx", "D3D12.command_device_mismatch",
                                     "phase=%s expected_device=%p command_list=%p",
                                     phase != nullptr ? phase : "execution",
                                     static_cast<void*>(expectedDevice),
                                     static_cast<void*>(commandList));
        return apusr::Status::runtimeFailure(
            "NGX/DLSS DX12 command list does not belong to the device used to create the feature");
    }
    return apusr::Status::ok();
}
#endif

[[nodiscard]] apusr::models::fsr4_int8::Preset referencePresetForNgxQuality(const apusr::frontends::NgxDlssPerfQualityMode quality) noexcept
{
    return apusr::models::fsr4_int8::presetForQualityMode(apusr::frontends::ngxDlssQualityModeToApusr(quality));
}

[[nodiscard]] apusr::Status resolveVulkanHostIngress(
    apusr::frontends::NgxDlssVulkanEvaluateDesc& desc)
{
#if defined(_WIN32)
    // All module discovery and GetProcAddress work is completed by the cold
    // Vulkan NGX bootstrap functions. Per-frame evaluation consumes only the
    // cached private ABI pointers and captured queue/device state.
    if (!gApusrVulkanIngressFunctionsReady.load(std::memory_order_acquire)) {
        return apusr::Status::runtimeFailure("APUSR Vulkan ingress bootstrap is incomplete");
    }
    const auto evaluationActive = gApusrStreamlineEvaluationActiveFn.load(std::memory_order_acquire);
    const auto negotiationFlags = gApusrVulkanNegotiationFlagsFn.load(std::memory_order_acquire);
    const auto resolveQueue = gApusrVulkanResolveQueueFn.load(std::memory_order_acquire);
    if (evaluationActive == nullptr || !evaluationActive()) {
        return apusr::Status::notImplemented("direct/native NGX Vulkan image-state provenance is not implemented");
    }

    const std::uint32_t requirementFlags = negotiationFlags(desc.device.value);
    if ((requirementFlags & APUSR_VK_HOST_REQUIRED_MASK) != APUSR_VK_HOST_REQUIRED_MASK) {
        return apusr::Status::runtimeFailure("Vulkan device is missing APUSR native host-capture/DOT4 contract");
    }

    ApusrVulkanHostQueueInfo graphics{};
    ApusrVulkanHostQueueInfo compute{};
    const bool haveGraphics = resolveQueue(desc.device.value, APUSR_VK_HOST_QUEUE_GRAPHICS, &graphics);
    const bool haveCompute = resolveQueue(desc.device.value, APUSR_VK_HOST_QUEUE_COMPUTE, &compute);
    if (!haveGraphics && !haveCompute) {
        return apusr::Status::notImplemented("Vulkan device has no captured graphics/compute queue contract");
    }
    if (!haveGraphics) {
        graphics = compute;
    }
    if (!haveCompute) {
        compute = graphics;
    }

    desc.commandQueue = apusr::NativeHandle{graphics.queue};
    desc.graphicsQueueFamilyIndex = graphics.familyIndex;
    desc.computeQueueFamilyIndex = compute.familyIndex;
    desc.stateContract = apusr::frontends::NgxDlssVulkanStateContract::NgxEvaluatePrepared;
    return apusr::Status::ok();
#else
    (void)desc;
    return apusr::Status::notImplemented("NGX/DLSS Vulkan host capture is currently Windows-only");
#endif
}

[[nodiscard]] std::string getenvString(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
}

#if APUSR_ENABLE_VULKAN_BACKEND
[[nodiscard]] std::uint32_t vulkanFsr4RequiredSubgroupSize(const apusr::NativeHandle device)
{
    const std::string value = getenvString("APUSR_VULKAN_FSR4_SUBGROUP_SIZE");
    if (value.empty() || value == "auto") {
        return 0u;
    }

    const std::uint32_t requested = value == "32" ? 32u : value == "64" ? 64u : 0u;
    if (requested == 0u) {
        apusr::host_discovery::eventf(
            "ngx", "vulkan.fsr4_subgroup_selection",
            "requested=%s selected=auto reason=invalid_value expected=auto_or_32_or_64",
            value.c_str());
        return 0u;
    }

#if defined(_WIN32)
    const auto negotiationFlags = gApusrVulkanNegotiationFlagsFn.load(std::memory_order_acquire);
    const std::uint32_t flags = negotiationFlags != nullptr ? negotiationFlags(device.value) : 0u;
    const std::uint32_t requiredFlag = requested == 32u
        ? APUSR_VK_HOST_FSR4_SUBGROUP32
        : APUSR_VK_HOST_FSR4_SUBGROUP64;
    if ((flags & requiredFlag) != requiredFlag) {
        apusr::host_discovery::eventf(
            "ngx", "vulkan.fsr4_subgroup_selection",
            "requested=%u selected=auto reason=host_capability_unavailable negotiation_flags=0x%08x",
            requested, flags);
        return 0u;
    }
    apusr::host_discovery::eventf(
        "ngx", "vulkan.fsr4_subgroup_selection",
        "requested=%u selected=%u negotiation_flags=0x%08x",
        requested, requested, flags);
    return requested;
#else
    (void)device;
    apusr::host_discovery::eventf(
        "ngx", "vulkan.fsr4_subgroup_selection",
        "requested=%u selected=auto reason=host_capture_unavailable",
        requested);
    return 0u;
#endif
}
#endif

[[nodiscard]] apusr::models::fsr4_int8::PipelineVariant buildPipelineVariant() noexcept
{
    return apusr::models::fsr4_int8::PipelineVariant::Dp4aOptimized;
}

#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
[[nodiscard]] apusr::models::fsr4_int8::Preset referencePresetForNgx(const apusr::frontends::NgxDlssEvaluateDesc& desc) noexcept
{
    return referencePresetForNgxQuality(desc.qualityMode);
}

[[nodiscard]] apusr::backends::Dx12Fsr4ReferenceCreateDesc makeNgxReferenceCreateDesc(
    const apusr::NativeHandle device,
    const apusr::frontends::NgxDlssEvaluateDesc& desc,
    const apusr::frontends::NgxDlssDispatchNormalization& normalized)
{
    apusr::backends::Dx12Fsr4ReferenceCreateDesc create{};
    create.device = device;
    create.repoRoot = getenvString("APUSR_FSR4_REFERENCE_REPO_ROOT");
    create.runner.maxRenderSize = desc.renderSize;
    create.runner.maxUpscaleSize = desc.displaySize;
    create.runner.features = normalized.frame.options.features;
    create.runner.pipelineVariant = buildPipelineVariant();
    create.runner.initialPreset = referencePresetForNgx(desc);
    create.runner.allowDebugVisualization = false;
    create.runner.supportsWmma = false;
    create.shaderProvider.enablePackagedCache = true;
    create.shaderProvider.enableEmbeddedCache = true;
    create.shaderProvider.allowRuntimeCompilation = false;
    create.shaderProvider.requireCompletePrecompiledCache = false;
    create.shaderProvider.packagedCacheRoot = getenvString("APUSR_FSR4_REFERENCE_DX12_SHADER_CACHE_ROOT");
    create.reflectionManifestPath = getenvString("APUSR_FSR4_REFERENCE_SHADER_RESOURCE_MANIFEST");
    create.enableReflectionManifestValidation = !create.reflectionManifestPath.empty();
    create.requireCompleteReflectionCoverage = false;
    create.requireInitializerBytes = true;
    return create;
}

[[nodiscard]] apusr::Status makeNgxReferenceFrameDesc(
    NgxFeatureContext& feature,
    const apusr::frontends::NgxDlssEvaluateDesc& desc,
    const apusr::frontends::NgxDlssDispatchNormalization& normalized,
    apusr::backends::Dx12Fsr4ReferenceFrameDesc& outFrame)
{
    outFrame = apusr::backends::Dx12Fsr4ReferenceFrameDesc{};
    outFrame.commandList = desc.commandList;
    outFrame.runner.frame = normalized.frame;
    outFrame.runner.maxUpscaleSize = desc.displaySize;
    outFrame.runner.residentModelPreset = referencePresetForNgx(desc);
    outFrame.runner.previousPreExposure = feature.referencePreviousPreExposure;
    outFrame.runner.previousJitter[0] = normalized.frame.previousJitter.x;
    outFrame.runner.previousJitter[1] = normalized.frame.previousJitter.y;
    outFrame.runner.forceReset = desc.resetHistory || !feature.referenceHasHistory;

    outFrame.externalResources.inputColor = normalized.frame.surfaces.color.resource;
    outFrame.externalResources.inputDepth = normalized.frame.surfaces.depth.resource;
    outFrame.externalResources.inputMotionVectors = normalized.frame.surfaces.motionVectors.resource;
    outFrame.externalResources.inputExposure = normalized.frame.surfaces.exposure.resource;
    outFrame.externalResources.output = normalized.frame.surfaces.output.resource;
    outFrame.externalResources.inputColorFormat = normalized.frame.surfaces.color.format;
    outFrame.externalResources.inputDepthFormat = normalized.frame.surfaces.depth.format;
    outFrame.externalResources.inputMotionVectorsFormat = normalized.frame.surfaces.motionVectors.format;
    outFrame.externalResources.inputExposureFormat = normalized.frame.surfaces.exposure.format;
    outFrame.externalResources.outputFormat = normalized.frame.surfaces.output.format;
    outFrame.externalResources.inputColorState = normalized.frame.surfaces.color.nativeState;
    outFrame.externalResources.inputDepthState = normalized.frame.surfaces.depth.nativeState;
    outFrame.externalResources.inputMotionVectorsState = normalized.frame.surfaces.motionVectors.nativeState;
    outFrame.externalResources.inputExposureState = normalized.frame.surfaces.exposure.nativeState;
    outFrame.externalResources.outputState = normalized.frame.surfaces.output.nativeState;
    outFrame.externalResources.restoreExternalResourceStatesAfterDispatch = false;
    return apusr::Status::ok();
}

[[nodiscard]] std::uint64_t ngxReferencePipelineKey(
    const apusr::NativeHandle device,
    const apusr::frontends::NgxDlssEvaluateDesc& desc,
    const apusr::frontends::NgxDlssDispatchNormalization& normalized) noexcept
{
    std::uint64_t key = 0x4e47585245464938ull; // NGXREFI8
    key ^= reinterpret_cast<std::uintptr_t>(device.value) + 0x9e3779b97f4a7c15ull + (key << 6) + (key >> 2);
    key ^= static_cast<std::uint64_t>(desc.renderSize.width) << 1;
    key ^= static_cast<std::uint64_t>(desc.renderSize.height) << 17;
    key ^= static_cast<std::uint64_t>(desc.displaySize.width) << 33;
    key ^= static_cast<std::uint64_t>(desc.displaySize.height) << 49;
    key ^= static_cast<std::uint64_t>(referencePresetForNgx(desc)) << 8;
    key ^= normalized.frame.options.hasExposure ? 0x10ull : 0ull;
    key ^= normalized.frame.options.hasReactiveMask ? 0x20ull : 0ull;
    key ^= normalized.frame.options.features.depthInverted ? 0x40ull : 0ull;
    key ^= normalized.frame.options.features.autoExposure ? 0x80ull : 0ull;
    return key;
}

[[nodiscard]] apusr::Status ensureRuntimeInitialized(
    NgxFeatureContext& feature,
    const apusr::frontends::NgxDlssEvaluateDesc& desc,
    const apusr::frontends::NgxDlssDispatchNormalization& normalized,
    apusr::backends::Dx12Fsr4ReferenceFrameDesc& outFrame)
{
    apusr::NativeHandle device = desc.device;
    if (device.value == nullptr) {
        return apusr::Status::invalidArgument("NGX/DLSS DX12 evaluation requires the device captured at feature creation");
    }

    if (feature.resolvedCommandQueue.value == nullptr &&
        (feature.queueResolveAttempts < 8u || ((feature.queueResolveAttempts & 63u) == 0u))) {
        const apusr::frontends::HostCaptureResolvedQueue queue = apusr::frontends::resolveHostQueueForDevice(
            apusr::GraphicsApi::Dx12,
            device,
            apusr::frontends::HostCaptureQueueKind::Direct);
        if (queue.available) {
            feature.resolvedCommandQueue = queue.queue;
        }
    }
    feature.queueResolveAttempts += 1;

    apusr::Status status = makeNgxReferenceFrameDesc(feature, desc, normalized, outFrame);
    if (!status.succeeded()) {
        return status;
    }
    outFrame.commandQueue = feature.resolvedCommandQueue;

    const std::uint64_t pipelineKey = ngxReferencePipelineKey(device, desc, normalized);
    if (!feature.runtime || feature.pipelineKey != pipelineKey) {
        auto runtime = apusr::backends::createDx12Fsr4ReferenceRuntime();
        if (!runtime) {
            return apusr::Status::missingBackend("could not create NGX/DLSS FSR4 INT8 DX12 reference runtime");
        }
        const apusr::backends::Dx12Fsr4ReferenceCreateDesc create = makeNgxReferenceCreateDesc(device, desc, normalized);
        status = runtime->initialize(create);
        if (!status.succeeded()) {
            return status;
        }
        status = runtime->prepareFrameCache(outFrame);
        if (!status.succeeded()) {
            return status;
        }
        feature.runtime = std::move(runtime);
        feature.pipelineKey = pipelineKey;
        feature.referenceHasHistory = false;
        feature.referencePreviousPreExposure = 1.0f;
        feature.referencePreviousJitter = {};
    }
    return apusr::Status::ok();
}

#endif

#if APUSR_ENABLE_VULKAN_BACKEND
[[nodiscard]] std::filesystem::path vulkanShaderPackPath()
{
#if defined(_WIN32)
    if (gApusrNgxModule == nullptr) {
        return {};
    }
    std::array<wchar_t, 32768> modulePath{};
    const DWORD chars = GetModuleFileNameW(gApusrNgxModule, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (chars == 0 || chars >= modulePath.size()) {
        return {};
    }
    std::filesystem::path path(std::wstring(modulePath.data(), chars));
    return path.replace_filename(L"apusr_fsr4_int8_vulkan.shaderpack");
#else
    return {};
#endif
}

[[nodiscard]] apusr::backends::VulkanFsr4ReferenceCreateDesc makeNgxVulkanReferenceCreateDesc(
    const NgxFeatureContext& feature,
    const apusr::frontends::NgxDlssVulkanEvaluateDesc& desc,
    const apusr::frontends::NgxDlssDispatchNormalization& normalized)
{
    apusr::backends::VulkanFsr4ReferenceCreateDesc create{};
    NgxGlobalState& state = ngxState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        create.instance = apusr::NativeHandle{state.vulkanInstance};
        create.physicalDevice = apusr::NativeHandle{state.vulkanPhysicalDevice};
        create.device = apusr::NativeHandle{state.vulkanDevice};
    }
    create.graphicsQueueFamilyIndex = desc.graphicsQueueFamilyIndex;
    create.computeQueueFamilyIndex = desc.computeQueueFamilyIndex;
    create.repoRoot = getenvString("APUSR_FSR4_REFERENCE_REPO_ROOT");
    create.shaderPackPath = vulkanShaderPackPath();
    create.shaderPack = {};
    // NGX feature creation already carries the render-resolution contract for
    // the selected quality mode. Using display size here made Vulkan's FSR4
    // runtime believe a 2x Performance input (for example 1720x720 ->
    // 3440x1440) had a 3440x1440 maximum render surface. Besides inflating
    // render-sized intermediates, that value also enters the FSR4 constants.
    // Match the mature DX12 frontend: capacity the model at render size and
    // keep display size solely as the upscale/output capacity.
    create.runner.maxRenderSize = feature.create.renderSize.width != 0u && feature.create.renderSize.height != 0u
        ? feature.create.renderSize
        : desc.renderSize;
    create.runner.maxUpscaleSize = feature.create.displaySize;
    create.runner.features = normalized.frame.options.features;
    create.runner.pipelineVariant = buildPipelineVariant();
    create.runner.initialPreset = referencePresetForNgxQuality(desc.qualityMode);
    create.runner.allowDebugVisualization = false;
    create.runner.supportsWmma = false;
    create.requiredSubgroupSize = vulkanFsr4RequiredSubgroupSize(create.device);
    create.requireInitializerBytes = true;
    return create;
}

[[nodiscard]] apusr::Status makeNgxVulkanReferenceFrameDesc(
    NgxFeatureContext& feature,
    const apusr::frontends::NgxDlssVulkanEvaluateDesc& desc,
    const apusr::frontends::NgxDlssDispatchNormalization& normalized,
    apusr::backends::VulkanFsr4ReferenceFrameDesc& outFrame)
{
    outFrame = {};
    outFrame.commandBuffer = desc.commandBuffer;
    outFrame.frameInput = normalized.frameInput;
    outFrame.runner.frame = normalized.frame;
    outFrame.runner.maxUpscaleSize = desc.displaySize;
    outFrame.runner.residentModelPreset = referencePresetForNgxQuality(desc.qualityMode);
    outFrame.runner.previousPreExposure = feature.referencePreviousPreExposure;
    outFrame.runner.previousJitter[0] = normalized.frame.previousJitter.x;
    outFrame.runner.previousJitter[1] = normalized.frame.previousJitter.y;
    outFrame.runner.forceReset = desc.resetHistory || !feature.referenceHasHistory;
    return apusr::Status::ok();
}

[[nodiscard]] apusr::Status recordVulkanFeature(
    NgxFeatureContext& feature,
    const apusr::frontends::NgxDlssVulkanEvaluateDesc& desc,
    const apusr::frontends::NgxDlssDispatchNormalization& normalized)
{
    apusr::backends::VulkanFsr4ReferenceFrameDesc frame;
    apusr::Status status = makeNgxVulkanReferenceFrameDesc(feature, desc, normalized, frame);
    if (!status.succeeded()) {
        return status;
    }
    if (!feature.vulkanRuntime) {
        auto runtime = apusr::backends::createVulkanFsr4ReferenceRuntime();
        if (!runtime) {
            return apusr::Status::missingBackend("could not create NGX/DLSS FSR4 INT8 Vulkan reference runtime");
        }
        const auto create = makeNgxVulkanReferenceCreateDesc(feature, desc, normalized);
        status = runtime->initialize(create);
        if (!status.succeeded()) {
            return status;
        }
        status = runtime->prepareFrameCache(frame);
        if (!status.succeeded()) {
            return status;
        }
        feature.vulkanRuntime = std::move(runtime);
        feature.referenceHasHistory = false;
        feature.referencePreviousPreExposure = 1.0f;
        feature.referencePreviousJitter = {};
        frame.runner.forceReset = true;
    }
    status = feature.vulkanRuntime->recordFrame(frame);
    if (status.succeeded()) {
        feature.referenceHasHistory = true;
        feature.referencePreviousPreExposure = normalized.frame.options.preExposure;
        feature.referencePreviousJitter = normalized.frame.jitter;
    }
    return status;
}
#endif

[[nodiscard]] apusr::Status recordFeature(
    NgxFeatureContext& feature,
    const apusr::frontends::NgxDlssEvaluateDesc& desc,
    const apusr::frontends::NgxDlssDispatchNormalization& normalized)
{
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    apusr::backends::Dx12Fsr4ReferenceFrameDesc referenceFrame;
    apusr::Status status = ensureRuntimeInitialized(feature, desc, normalized, referenceFrame);
    if (!status.succeeded()) {
        return status;
    }
    status = feature.runtime->recordFrame(referenceFrame);
    if (status.succeeded()) {
        feature.referenceHasHistory = true;
        feature.referencePreviousPreExposure = normalized.frame.options.preExposure;
        feature.referencePreviousJitter = normalized.frame.jitter;
    }
    return status;
#else
    (void)feature;
    (void)desc;
    (void)normalized;
    return apusr::Status::notImplemented("NGX/DLSS DX12 reference backend recording is available only in Windows DX12 builds");
#endif
}


#define APUSR_NGX_EXPORT extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API

} // namespace

namespace {

} // namespace


void setCommonCapabilityParameters(NVSDK_NGX_Parameter* parameters);

extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API std::uint32_t apusrNgxDlssDx12FrontendAbiVersion()
{
    return 1u;
}

extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API const char* apusrNgxDlssDx12FrontendName()
{
    return "APUSR NGX/DLSS DX12 compatibility frontend with APUSR FSR4 INT8 reference backend";
}

extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API const char* apusrNgxDlssDx12FrontendOutputBaseName()
{
    return "nvngx_dlss";
}

extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API bool apusrNgxDlssDx12FrontendUsesNvngxHooks()
{
    return true;
}

extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API bool apusrNgxDlssDx12FrontendUsesThirdPartyModelArtifacts()
{
    // This answers a narrow packaging/legal question: the nvngx compatibility DLL
    // does not embed or forward into NVIDIA's proprietary DLSS model/runtime data.
    return false;
}

extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API bool apusrNgxDlssDx12FrontendUsesApusrModelArtifacts()
{
    return true;
}

extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API const char* apusrNgxDlssDx12FrontendModelArtifactPolicy()
{
    return "records the imported FSR4 INT8 reference pipeline; does not call NVIDIA's embedded DLSS model/runtime";
}

#if defined(_WIN32)
extern "C" APUSR_NGX_DLSS_DX12_FRONTEND_API bool NVSDK_CONV apusrNgxDlssRegisterD3D12CommandQueueForSwapChain(void* swapChain, void* commandQueue)
{
    ngxTrace("apusrNgxDlssRegisterD3D12CommandQueueForSwapChain");
#if APUSR_ENABLE_DX12_BACKEND
    if (swapChain == nullptr || commandQueue == nullptr) {
        return false;
    }

    ID3D12CommandQueue* queue = nullptr;
    HRESULT hr = static_cast<IUnknown*>(commandQueue)->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr) || queue == nullptr) {
        return false;
    }

    ID3D12Device* device = nullptr;
    hr = queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (FAILED(hr) || device == nullptr) {
        queue->Release();
        return false;
    }

    const D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
    const apusr::Status status = apusr::frontends::registerDx12Queue(apusr::frontends::Dx12QueueRegistrationDesc{
        .device = apusr::NativeHandle{device},
        .queue = apusr::NativeHandle{queue},
        .swapChain = apusr::NativeHandle{swapChain},
        .source = apusr::frontends::Dx12QueueSourceKind::DxgiSwapChain,
        .nodeMask = queueDesc.NodeMask,
        .frameIndex = 0,
    });

    device->Release();
    queue->Release();
    if (!status.succeeded()) {
        return false;
    }
    return true;
#else
    (void)swapChain;
    (void)commandQueue;
    return false;
#endif
}
#endif

APUSR_NGX_EXPORT unsigned int NVSDK_CONV NVSDK_NGX_GetAPIVersion()
{
    ngxTrace("NVSDK_NGX_GetAPIVersion");
    return kNgxCompatApiVersion;
}

APUSR_NGX_EXPORT unsigned int NVSDK_CONV NVSDK_NGX_GetApplicationId()
{
    ngxTrace("NVSDK_NGX_GetApplicationId");
    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.applicationId;
}

APUSR_NGX_EXPORT unsigned int NVSDK_CONV NVSDK_NGX_GetDriverVersion()
{
    ngxTrace("NVSDK_NGX_GetDriverVersion");
    return kNgxCompatDriverVersionPacked;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_GetDriverVersionEx(
    unsigned int* versions,
    unsigned int inputVersionCount,
    unsigned int* totalDriverVersionCount)
{
    ngxTrace("NVSDK_NGX_GetDriverVersionEx");
    if (versions == nullptr && totalDriverVersionCount == nullptr) {
        return kNgxFailInvalidParameter;
    }
    if (totalDriverVersionCount != nullptr) {
        *totalDriverVersionCount = 2u;
    }
    if (versions != nullptr && inputVersionCount >= 1u) {
        versions[0] = kNgxCompatDriverVersionMajor;
        if (inputVersionCount >= 2u) {
            versions[1] = kNgxCompatDriverVersionMinor;
        }
    }
    return kNgxSuccess;
}

APUSR_NGX_EXPORT unsigned int NVSDK_CONV NVSDK_NGX_GetGPUArchitecture()
{
    ngxTrace("NVSDK_NGX_GetGPUArchitecture");
    return kNgxCompatGpuArchitectureTuring;
}

APUSR_NGX_EXPORT unsigned int NVSDK_CONV NVSDK_NGX_GetSnippetVersion()
{
    ngxTrace("NVSDK_NGX_GetSnippetVersion");
    return kNgxCompatSnippetVersion;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_SetInfoCallback(void* callback)
{
    ngxTrace("NVSDK_NGX_SetInfoCallback");
    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.infoCallback = callback;
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_SetTelemetryEvaluateCallback(void* callback)
{
    ngxTrace("NVSDK_NGX_SetTelemetryEvaluateCallback");
    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.telemetryEvaluateCallback = callback;
    return kNgxSuccess;
}

APUSR_NGX_EXPORT unsigned int NVSDK_CONV NGX_SNIPPETS_GetRequiredDriverSupport()
{
    ngxTrace("NGX_SNIPPETS_GetRequiredDriverSupport");
    return kNgxCompatDriverVersionPacked;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_Init(
    unsigned long long applicationId,
    const wchar_t*,
    ID3D12Device* device,
    const void*,
    unsigned int)
{
    ngxTrace("NVSDK_NGX_D3D12_Init");
    apusr::host_discovery::eventf("ngx", "D3D12_Init.called", "device=%p application_id=%llu",
                                 static_cast<void*>(device), applicationId);
    {
        NgxGlobalState& state = ngxState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.device = device;
        state.applicationId = applicationId != 0ull ? static_cast<std::uint32_t>(applicationId) : state.applicationId;
        state.initCalls.fetch_add(1, std::memory_order_relaxed);
        state.lastError.clear();
    }
    if (device != nullptr) {
        (void)apusr::frontends::installDx12DeviceQueueCapture(apusr::NativeHandle{device});
        (void)apusr::frontends::installDx12QueueDiscovery(apusr::NativeHandle{device});
    }
    return device != nullptr ? kNgxSuccess : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_Init_Ext(
    unsigned long long applicationId,
    const wchar_t* applicationDataPath,
    ID3D12Device* device,
    unsigned int sdkVersion,
    const void* featureInfo)
{
    ngxTrace("NVSDK_NGX_D3D12_Init_Ext");
    return NVSDK_NGX_D3D12_Init(applicationId, applicationDataPath, device, featureInfo, sdkVersion);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_Init_ProjectID(
    const char*,
    unsigned int,
    const char*,
    const wchar_t* applicationDataPath,
    ID3D12Device* device,
    unsigned int sdkVersion,
    const void* featureInfo)
{
    ngxTrace("NVSDK_NGX_D3D12_Init_ProjectID");
    return NVSDK_NGX_D3D12_Init(0, applicationDataPath, device, featureInfo, sdkVersion);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_Init_with_ProjectID(
    const char*,
    unsigned int,
    const char*,
    const wchar_t*,
    ID3D12Device* device,
    const void*,
    unsigned int)
{
    ngxTrace("NVSDK_NGX_D3D12_Init_with_ProjectID");
    return NVSDK_NGX_D3D12_Init(0, nullptr, device, nullptr, 0);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_Shutdown()
{
    apusr::frontends::uninstallDx12QueueDiscovery();
    apusr::frontends::uninstallDx12DeviceQueueCapture();
    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.device = nullptr;
    state.features.clear();
    state.lastError.clear();
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_Shutdown1(ID3D12Device*)
{
    return NVSDK_NGX_D3D12_Shutdown();
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_D3D12_AllocateParameters");
    if (outParameters == nullptr) {
        return kNgxFailInvalidParameter;
    }
    *outParameters = new (std::nothrow) NgxParameterStore();
    return *outParameters != nullptr ? kNgxSuccess : kNgxFail;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_GetParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_D3D12_GetParameters");
    const NgxResult result = NVSDK_NGX_D3D12_AllocateParameters(outParameters);
    if (result == kNgxSuccess) {
        ::setCommonCapabilityParameters(*outParameters);
    }
    return result;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_DLSS_GetOptimalSettingsCallback(NVSDK_NGX_Parameter* parameters)
{
    ngxTrace("NVSDK_NGX_DLSS_GetOptimalSettingsCallback");
    if (parameters == nullptr) {
        return kNgxFailInvalidParameter;
    }

    unsigned int displayWidth = 0;
    unsigned int displayHeight = 0;
    int quality = 1;
    // NGX callers provide the requested display/output resolution in Width/Height
    // and expect this callback to overwrite OutWidth/OutHeight with the recommended
    // render resolution.  Prefer Width/Height so a prior callback's OutWidth does
    // not become stale input on subsequent settings queries.
    if (!getUIntAny(parameters, {keyWidth(), kEParamWidth, "ResourceOutWidth", "TargetWidth", keyOutWidth(), kEParamOutWidth}, displayWidth) ||
        !getUIntAny(parameters, {keyHeight(), kEParamHeight, "ResourceOutHeight", "TargetHeight", keyOutHeight(), kEParamOutHeight}, displayHeight) ||
        !getIntAny(parameters, {keyPerfQuality(), kEParamPerfQualityValue, "DLSS.PerfQualityValue"}, quality)) {
        return kNgxFailInvalidParameter;
    }

    float ratio = 1.7f;
    switch (perfQualityFromInt(quality)) {
    case apusr::frontends::NgxDlssPerfQualityMode::UltraPerformance: ratio = 3.0f; break;
    case apusr::frontends::NgxDlssPerfQualityMode::MaxPerformance: ratio = 2.0f; break;
    case apusr::frontends::NgxDlssPerfQualityMode::Balanced: ratio = 1.7f; break;
    case apusr::frontends::NgxDlssPerfQualityMode::MaxQuality: ratio = 1.5f; break;
    case apusr::frontends::NgxDlssPerfQualityMode::UltraQuality: ratio = 1.3f; break;
    case apusr::frontends::NgxDlssPerfQualityMode::NativeAA: ratio = 1.0f; break;
    }

    const unsigned int renderWidth = std::max(1u, static_cast<unsigned int>(static_cast<float>(displayWidth) / ratio));
    const unsigned int renderHeight = std::max(1u, static_cast<unsigned int>(static_cast<float>(displayHeight) / ratio));
    const float scale = static_cast<float>(renderWidth) / static_cast<float>(std::max(1u, displayWidth));

    parameters->Set("Scale", scale);
    parameters->Set("SuperSampling.ScaleFactor", scale);
    parameters->Set(keyOutWidth(), renderWidth);
    parameters->Set(keyOutHeight(), renderHeight);
    parameters->Set(kEParamOutWidth, renderWidth);
    parameters->Set(kEParamOutHeight, renderHeight);
    parameters->Set("Out.Width", renderWidth);
    parameters->Set("Out.Height", renderHeight);
    parameters->Set("DLSS.Get.Dynamic.Min.Render.Width", std::min(renderWidth, std::max(1u, displayWidth / 2u)));
    parameters->Set("DLSS.Get.Dynamic.Min.Render.Height", std::min(renderHeight, std::max(1u, displayHeight / 2u)));
    parameters->Set("DLSS.Get.Dynamic.Max.Render.Width", displayWidth);
    parameters->Set("DLSS.Get.Dynamic.Max.Render.Height", displayHeight);
    parameters->Set("SizeInBytes", static_cast<unsigned long long>(displayWidth) * static_cast<unsigned long long>(displayHeight) * 31ull);
    parameters->Set(kEParamSizeInBytes, static_cast<unsigned long long>(displayWidth) * static_cast<unsigned long long>(displayHeight) * 31ull);
    parameters->Set("DLSSMode", 0u);
    parameters->Set(kEParamDlssMode, 0u);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_DLSS_GetStatsCallback(NVSDK_NGX_Parameter* parameters)
{
    ngxTrace("NVSDK_NGX_DLSS_GetStatsCallback");
    if (parameters == nullptr) {
        return kNgxFailInvalidParameter;
    }

    unsigned int width = 0;
    unsigned int height = 0;
    getUIntAny(parameters, {keyWidth(), kEParamWidth, keyOutWidth(), kEParamOutWidth}, width);
    getUIntAny(parameters, {keyHeight(), kEParamHeight, keyOutHeight(), kEParamOutHeight}, height);
    if (width == 0u || height == 0u) {
        width = 1920u;
        height = 1080u;
    }
    const unsigned long long estimatedBytes =
        static_cast<unsigned long long>(width) * static_cast<unsigned long long>(height) * 31ull;
    parameters->Set("SizeInBytes", estimatedBytes);
    parameters->Set(kEParamSizeInBytes, estimatedBytes);
    return kNgxSuccess;
}

void setCommonCapabilityParameters(NVSDK_NGX_Parameter* parameters)
{
    if (parameters == nullptr) {
        return;
    }
    parameters->Set("SuperSampling.Available", 1u);
    parameters->Set(kEParamSuperSamplingAvailable, 1u);
    parameters->Set("SuperSampling.NeedsUpdatedDriver", 0u);
    parameters->Set("SuperSampling.MinDriverVersionMajor", 0u);
    parameters->Set("SuperSampling.MinDriverVersionMinor", 0u);
    parameters->Set("SuperSampling.FeatureInitResult", 0u);
    parameters->Set("FeatureSupported", kNgxFeatureSupportResultSupported);
    parameters->Set("MinHWArchitecture", kNgxCompatGpuArchitectureTuring);
    parameters->Set("NGX.Minimum.Driver.Version", kNgxCompatDriverVersionPacked);
    parameters->Set("NGX.Driver.Version", kNgxCompatDriverVersionPacked);
    parameters->Set("NGX.API.Version", kNgxCompatApiVersion);
    parameters->Set("NGX.Snippet.Version", kNgxCompatSnippetVersion);
    parameters->Set("Snippet.OptLevel", 0u);
    parameters->Set(kEParamOptLevel, 0u);
    parameters->Set("Snippet.IsDevBranch", 0u);
    parameters->Set(kEParamIsDevSnippetBranch, 0u);
    parameters->Set("Sharpness", 0.0f);
    parameters->Set(kEParamSharpness, 0.0f);
    parameters->Set("MV.Scale.X", 1.0f);
    parameters->Set(kEParamMvScaleX, 1.0f);
    parameters->Set("MV.Scale.Y", 1.0f);
    parameters->Set(kEParamMvScaleY, 1.0f);
    parameters->Set("MV.Offset.X", 0.0f);
    parameters->Set(kEParamMvOffsetX, 0.0f);
    parameters->Set("MV.Offset.Y", 0.0f);
    parameters->Set(kEParamMvOffsetY, 0.0f);
    parameters->Set("DLSS.Exposure.Scale", 1.0f);
    parameters->Set("PerfQualityValue", 1);
    parameters->Set(kEParamPerfQualityValue, 1);
    parameters->Set("FreeMemOnReleaseFeature", 0u);
    parameters->Set("CreationNodeMask", 1u);
    parameters->Set(kEParamCreationNodeMask, 1u);
    parameters->Set("VisibilityNodeMask", 1u);
    parameters->Set(kEParamVisibilityNodeMask, 1u);
    parameters->Set("DLSS.Enable.Output.Subrects", 1u);
    parameters->Set("RTXValue", 0u);
    parameters->Set(kEParamRtxValue, 0u);
    parameters->Set("DLSS.Hint.Render.Preset.DLAA", 0u);
    parameters->Set("DLSS.Hint.Render.Preset.UltraQuality", 0u);
    parameters->Set("DLSS.Hint.Render.Preset.Quality", 0u);
    parameters->Set("DLSS.Hint.Render.Preset.Balanced", 0u);
    parameters->Set("DLSS.Hint.Render.Preset.Performance", 0u);
    parameters->Set("DLSS.Hint.Render.Preset.UltraPerformance", 0u);
    parameters->Set("DLSSOptimalSettingsCallback", reinterpret_cast<void*>(&NVSDK_NGX_DLSS_GetOptimalSettingsCallback));
    parameters->Set(kEParamDlssOptimalSettingsCallback, reinterpret_cast<void*>(&NVSDK_NGX_DLSS_GetOptimalSettingsCallback));
    parameters->Set("DLSSGetStatsCallback", reinterpret_cast<void*>(&NVSDK_NGX_DLSS_GetStatsCallback));
    parameters->Set("SizeInBytes", 1920ull * 1080ull * 31ull);
    parameters->Set(kEParamSizeInBytes, 1920ull * 1080ull * 31ull);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_GetCapabilityParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_D3D12_GetCapabilityParameters");
    apusr::host_discovery::event("ngx", "D3D12_GetCapabilityParameters.called");
    const NgxResult result = NVSDK_NGX_D3D12_AllocateParameters(outParameters);
    if (result != kNgxSuccess) {
        return result;
    }
    ::setCommonCapabilityParameters(*outParameters);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_PopulateParameters_Impl(NVSDK_NGX_Parameter* parameters)
{
    ngxTrace("NVSDK_NGX_D3D12_PopulateParameters_Impl");
    if (parameters == nullptr) {
        return kNgxFailInvalidParameter;
    }
    ::setCommonCapabilityParameters(parameters);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter* parameters)
{
    ngxTrace("NVSDK_NGX_D3D12_DestroyParameters");
    delete static_cast<NgxParameterStore*>(parameters);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_AllocateParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_AllocateParameters");
    return NVSDK_NGX_D3D12_AllocateParameters(outParameters);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_GetParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_GetParameters");
    return NVSDK_NGX_D3D12_GetParameters(outParameters);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_GetCapabilityParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_GetCapabilityParameters");
    return NVSDK_NGX_D3D12_GetCapabilityParameters(outParameters);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_DestroyParameters(NVSDK_NGX_Parameter* parameters)
{
    ngxTrace("NVSDK_NGX_DestroyParameters");
    return NVSDK_NGX_D3D12_DestroyParameters(parameters);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_UpdateFeature(const void*, unsigned int feature)
{
    ngxTrace("NVSDK_NGX_UpdateFeature");
    return feature == kNgxFeatureSuperSampling ? kNgxSuccess : kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_ProcessCommand(const char*, const char*, void*)
{
    ngxTrace("NVSDK_NGX_ProcessCommand");
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_GetScratchBufferSize(
    unsigned int feature,
    const NVSDK_NGX_Parameter*,
    std::size_t* outSizeInBytes)
{
    if (outSizeInBytes == nullptr) {
        return kNgxFailInvalidParameter;
    }
    if (feature != kNgxFeatureSuperSampling) {
        *outSizeInBytes = 0;
        return kNgxFailFeatureNotSupported;
    }
    *outSizeInBytes = kNgxCompatScratchBufferSize;
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList* commandList,
    unsigned int feature,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** outHandle)
{
    ngxTrace("NVSDK_NGX_D3D12_CreateFeature");
    apusr::host_discovery::eventf("ngx", "D3D12_CreateFeature.called",
                                 "feature=0x%08X command_list=%p", feature, static_cast<void*>(commandList));
    if (feature != kNgxFeatureSuperSampling) {
        return kNgxFailFeatureNotSupported;
    }
    if (outHandle == nullptr || parameters == nullptr) {
        return kNgxFailInvalidParameter;
    }

    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.device == nullptr) {
        return kNgxFailNotInitialized;
    }
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
    {
        const apusr::Status executionStatus = validateLiveD3D12ExecutionObjects(state.device, commandList, "create_feature");
        if (!executionStatus.succeeded()) {
            setLastErrorUnlocked(state, executionStatus);
            return statusToNgxResult(executionStatus);
        }
    }
#endif

    apusr::frontends::NgxDlssCreateDesc create = createDescFromParams(state.device, parameters);
    apusr::host_discovery::eventf(
        "ngx", "D3D12_CreateFeature.contract",
        "render=%ux%u output=%ux%u quality=%u flags=0x%08X",
        create.renderSize.width, create.renderSize.height,
        create.displaySize.width, create.displaySize.height,
        static_cast<unsigned int>(create.qualityMode), create.featureCreateFlags);
    apusr::Status status = apusr::frontends::validateNgxDlssCreateDesc(create);
    if (!status.succeeded()) {
        setLastErrorUnlocked(state, status);
        return statusToNgxResult(status);
    }

    auto context = std::make_shared<NgxFeatureContext>();
    context->handle.Id = state.nextHandleId++;
    context->handle.Magic = kNgxHandleMagic;
    context->handle.Context = context.get();
    context->api = apusr::FrameInputApi::Dx12;
    context->create = create;
    *outHandle = &context->handle;
    state.features[context->handle.Id] = std::move(context);
    state.lastError.clear();
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle* handle)
{
    if (handle == nullptr) {
        return kNgxFailInvalidParameter;
    }
    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.features.find(handle->Id);
    if (found == state.features.end()) {
        return kNgxFailFeatureNotFound;
    }
    found->second->handle.Magic = 0;
    found->second->handle.Context = nullptr;
    state.features.erase(found);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_GetFeatureRequirements(
    void*,
    const void* featureDiscoveryInfo,
    void* outSupported)
{
    ngxTrace("NVSDK_NGX_D3D12_GetFeatureRequirements");
    if (featureDiscoveryInfo == nullptr) {
        return kNgxFailInvalidParameter;
    }
    const bool supported = ngxFeatureDiscoveryLooksLikeSuperSampling(featureDiscoveryInfo);
    if (outSupported != nullptr) {
        auto* requirement = static_cast<NgxFeatureRequirementCompat*>(outSupported);
        requirement->FeatureSupported = supported ? kNgxFeatureSupportResultSupported : kNgxFeatureSupportResultAdapterUnsupported;
        requirement->MinHWArchitecture = 0u;
        std::strncpy(requirement->MinOSVersion, "10.0.10240.16384", sizeof(requirement->MinOSVersion) - 1u);
    }
    return supported ? kNgxSuccess : kNgxFailFeatureNotSupported;
}


namespace {
std::atomic<std::uint64_t> g_apusrNgxHostDiscoveryEvaluateLogCounter{0};

bool shouldLogNgxEvaluateHostDiscovery(std::uint32_t result)
{
    if (!apusr::host_discovery::enabled()) {
        return false;
    }
    const std::uint64_t index = g_apusrNgxHostDiscoveryEvaluateLogCounter.fetch_add(1, std::memory_order_relaxed);
    return result != kNgxSuccess || index < 8u || (index % 600u) == 0u;
}
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(
    ID3D12GraphicsCommandList* commandList,
    const NVSDK_NGX_Handle* handle,
    NVSDK_NGX_Parameter* parameters,
    void*)
{
    ngxTraceHot("NVSDK_NGX_D3D12_EvaluateFeature", gNgxD3D12EvaluateTraceCounter);
    if (commandList == nullptr || handle == nullptr || parameters == nullptr) {
        return kNgxFailInvalidParameter;
    }

    NgxFeatureContext* feature = featureContextFromHandle(handle);
    if (feature == nullptr) {
        return kNgxFailFeatureNotFound;
    }
    ngxState().evaluateCalls.fetch_add(1, std::memory_order_relaxed);

    apusr::Status status = apusr::Status::ok();
    apusr::RenderSize evaluatedRenderSize{};
    apusr::DisplaySize evaluatedDisplaySize{};
    {
        std::lock_guard<std::mutex> featureLock(feature->mutex);
#if defined(_WIN32) && APUSR_ENABLE_DX12_BACKEND
        status = validateLiveD3D12ExecutionObjects(
            static_cast<ID3D12Device*>(feature->create.device.value), commandList, "evaluate_feature");
#endif
        const apusr::frontends::NgxDlssEvaluateDesc desc = evaluateDescFromParams(*feature, commandList, parameters);
        evaluatedRenderSize = desc.renderSize;
        evaluatedDisplaySize = desc.displaySize;
        apusr::frontends::NgxDlssDispatchNormalization normalized;
        if (status.succeeded()) {
            status = apusr::frontends::normalizeNgxDlssEvaluateDesc(desc, normalized);
        }
        if (status.succeeded()) {
            status = recordFeature(*feature, desc, normalized);
        } else {
        }
    }

    if (!status.succeeded()) {
        NgxGlobalState& state = ngxState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.failedEvaluateCalls.fetch_add(1, std::memory_order_relaxed);
        setLastErrorUnlocked(state, status);
        const std::uint32_t result = statusToNgxResult(status);
        if (shouldLogNgxEvaluateHostDiscovery(result)) {
            apusr::host_discovery::eventf("ngx", "D3D12_EvaluateFeature",
                                         "result=0x%08X command_list=%p handle=%p render=%ux%u output=%ux%u",
                                         result, static_cast<void*>(commandList), static_cast<const void*>(handle),
                                         evaluatedRenderSize.width, evaluatedRenderSize.height,
                                         evaluatedDisplaySize.width, evaluatedDisplaySize.height);
        }
        return result;
    }
    if (shouldLogNgxEvaluateHostDiscovery(kNgxSuccess)) {
        apusr::host_discovery::eventf("ngx", "D3D12_EvaluateFeature",
                                     "result=0x%08X command_list=%p handle=%p render=%ux%u output=%ux%u",
                                     kNgxSuccess, static_cast<void*>(commandList), static_cast<const void*>(handle),
                                     evaluatedRenderSize.width, evaluatedRenderSize.height,
                                     evaluatedDisplaySize.width, evaluatedDisplaySize.height);
    }
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(
    ID3D12GraphicsCommandList* commandList,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    void*)
{
    return NVSDK_NGX_D3D12_EvaluateFeature(
        commandList,
        handle,
        const_cast<NVSDK_NGX_Parameter*>(parameters),
        nullptr);
}

APUSR_NGX_EXPORT void NVSDK_CONV NVSDK_NGX_Parameter_SetULL(NVSDK_NGX_Parameter* parameters, const char* name, unsigned long long value)
{
    if (parameters != nullptr) { parameters->Set(name, value); }
}

APUSR_NGX_EXPORT void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter* parameters, const char* name, float value)
{
    if (parameters != nullptr) { parameters->Set(name, value); }
}

APUSR_NGX_EXPORT void NVSDK_CONV NVSDK_NGX_Parameter_SetD(NVSDK_NGX_Parameter* parameters, const char* name, double value)
{
    if (parameters != nullptr) { parameters->Set(name, value); }
}

APUSR_NGX_EXPORT void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter* parameters, const char* name, unsigned int value)
{
    if (parameters != nullptr) { parameters->Set(name, value); }
}

APUSR_NGX_EXPORT void NVSDK_CONV NVSDK_NGX_Parameter_SetI(NVSDK_NGX_Parameter* parameters, const char* name, int value)
{
    if (parameters != nullptr) { parameters->Set(name, value); }
}

APUSR_NGX_EXPORT void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d11Resource(NVSDK_NGX_Parameter* parameters, const char* name, ID3D11Resource* value)
{
    if (parameters != nullptr) { parameters->Set(name, value); }
}

APUSR_NGX_EXPORT void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter* parameters, const char* name, ID3D12Resource* value)
{
    if (parameters != nullptr) { parameters->Set(name, value); }
}

APUSR_NGX_EXPORT void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter* parameters, const char* name, void* value)
{
    if (parameters != nullptr) { parameters->Set(name, value); }
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_Parameter_GetULL(NVSDK_NGX_Parameter* parameters, const char* name, unsigned long long* outValue)
{
    return parameters != nullptr ? parameters->Get(name, outValue) : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_Parameter_GetF(NVSDK_NGX_Parameter* parameters, const char* name, float* outValue)
{
    return parameters != nullptr ? parameters->Get(name, outValue) : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_Parameter_GetD(NVSDK_NGX_Parameter* parameters, const char* name, double* outValue)
{
    return parameters != nullptr ? parameters->Get(name, outValue) : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_Parameter_GetUI(NVSDK_NGX_Parameter* parameters, const char* name, unsigned int* outValue)
{
    return parameters != nullptr ? parameters->Get(name, outValue) : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_Parameter_GetI(NVSDK_NGX_Parameter* parameters, const char* name, int* outValue)
{
    return parameters != nullptr ? parameters->Get(name, outValue) : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_Parameter_GetD3d11Resource(NVSDK_NGX_Parameter* parameters, const char* name, ID3D11Resource** outValue)
{
    return parameters != nullptr ? parameters->Get(name, outValue) : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_Parameter_GetD3d12Resource(NVSDK_NGX_Parameter* parameters, const char* name, ID3D12Resource** outValue)
{
    return parameters != nullptr ? parameters->Get(name, outValue) : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_Parameter_GetVoidPointer(NVSDK_NGX_Parameter* parameters, const char* name, void** outValue)
{
    return parameters != nullptr ? parameters->Get(name, outValue) : kNgxFailInvalidParameter;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_Init(unsigned long long, const wchar_t*, void*, const void*, unsigned int)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_Init", "D3D11");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_Init_Ext(unsigned long long, const wchar_t*, void*, unsigned int, const void*)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_Init_Ext", "D3D11");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_Shutdown()
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_Shutdown", "D3D11");
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_Shutdown1(void*)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_Shutdown1", "D3D11");
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_GetScratchBufferSize(unsigned int, const NVSDK_NGX_Parameter*, std::size_t* outSizeInBytes)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_GetScratchBufferSize", "D3D11");
    if (outSizeInBytes != nullptr) { *outSizeInBytes = 0; }
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_GetFeatureRequirements(void*, const void*, void* outSupported)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_GetFeatureRequirements", "D3D11");
    if (outSupported != nullptr) {
        auto* requirement = static_cast<NgxFeatureRequirementCompat*>(outSupported);
        requirement->FeatureSupported = kNgxFeatureSupportResultAdapterUnsupported;
        requirement->MinHWArchitecture = kNgxCompatGpuArchitectureTuring;
        std::strncpy(requirement->MinOSVersion, "10.0.10240.16384", sizeof(requirement->MinOSVersion) - 1u);
    }
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_CreateFeature(void*, unsigned int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_CreateFeature", "D3D11");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_EvaluateFeature(void*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, void*)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_EvaluateFeature", "D3D11");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_ReleaseFeature(NVSDK_NGX_Handle*)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_ReleaseFeature", "D3D11");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_D3D11_PopulateParameters_Impl(NVSDK_NGX_Parameter* parameters)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_D3D11_PopulateParameters_Impl", "D3D11");
    if (parameters == nullptr) { return kNgxFailInvalidParameter; }
    setCommonCapabilityParameters(parameters);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_Init(
    unsigned long long applicationId,
    const wchar_t*,
    void* instance,
    void* physicalDevice,
    void* device,
    ...)
{
    ngxTrace("NVSDK_NGX_VULKAN_Init");
#if defined(_WIN32)
    (void)bootstrapApusrVulkanIngress();
#endif
    if (instance == nullptr || physicalDevice == nullptr || device == nullptr) {
        return kNgxFailInvalidParameter;
    }
    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.vulkanInstance = instance;
    state.vulkanPhysicalDevice = physicalDevice;
    state.vulkanDevice = device;
    state.applicationId = applicationId != 0ull ? static_cast<std::uint32_t>(applicationId) : state.applicationId;
    state.initCalls.fetch_add(1, std::memory_order_relaxed);
    state.lastError.clear();
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_Init_with_ProjectID(
    const char*,
    unsigned int,
    const char*,
    const wchar_t*,
    void* instance,
    void* physicalDevice,
    void* device,
    ...)
{
    ngxTrace("NVSDK_NGX_VULKAN_Init_with_ProjectID");
    return NVSDK_NGX_VULKAN_Init(0ull, nullptr, instance, physicalDevice, device);
}

// Older NGX loaders resolve these names.  They share the same state initialization
// and do not alter APUSR's Vulkan resource/command-buffer contract.
APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_Init_Ext(
    unsigned long long applicationId,
    const wchar_t* applicationDataPath,
    void* instance,
    void* physicalDevice,
    void* device,
    unsigned int,
    const void*)
{
    ngxTrace("NVSDK_NGX_VULKAN_Init_Ext");
    return NVSDK_NGX_VULKAN_Init(applicationId, applicationDataPath, instance, physicalDevice, device);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_Init_Ext2(
    unsigned long long applicationId,
    const wchar_t* applicationDataPath,
    void* instance,
    void* physicalDevice,
    void* device,
    void*,
    void*,
    unsigned int,
    const void*)
{
    ngxTrace("NVSDK_NGX_VULKAN_Init_Ext2");
    return NVSDK_NGX_VULKAN_Init(applicationId, applicationDataPath, instance, physicalDevice, device);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_Shutdown()
{
    ngxTrace("NVSDK_NGX_VULKAN_Shutdown");
    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto it = state.features.begin(); it != state.features.end();) {
        if (it->second->api == apusr::FrameInputApi::Vulkan) {
            it->second->handle.Magic = 0u;
            it->second->handle.Context = nullptr;
            it = state.features.erase(it);
        } else {
            ++it;
        }
    }
    state.vulkanInstance = nullptr;
    state.vulkanPhysicalDevice = nullptr;
    state.vulkanDevice = nullptr;
    state.lastError.clear();
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_Shutdown1(void*)
{
    return NVSDK_NGX_VULKAN_Shutdown();
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_AllocateParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_VULKAN_AllocateParameters");
    if (outParameters == nullptr) {
        return kNgxFailInvalidParameter;
    }
    *outParameters = new (std::nothrow) NgxParameterStore();
    return *outParameters != nullptr ? kNgxSuccess : kNgxFail;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_GetParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_VULKAN_GetParameters");
    const NgxResult result = NVSDK_NGX_VULKAN_AllocateParameters(outParameters);
    if (result == kNgxSuccess) {
        ::setCommonCapabilityParameters(*outParameters);
    }
    return result;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_GetCapabilityParameters(NVSDK_NGX_Parameter** outParameters)
{
    ngxTrace("NVSDK_NGX_VULKAN_GetCapabilityParameters");
    return NVSDK_NGX_VULKAN_GetParameters(outParameters);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_DestroyParameters(NVSDK_NGX_Parameter* parameters)
{
    ngxTrace("NVSDK_NGX_VULKAN_DestroyParameters");
    delete static_cast<NgxParameterStore*>(parameters);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_GetScratchBufferSize(
    unsigned int feature,
    const NVSDK_NGX_Parameter*,
    std::size_t* outSizeInBytes)
{
    if (outSizeInBytes == nullptr) {
        return kNgxFailInvalidParameter;
    }
    if (feature != kNgxFeatureSuperSampling) {
        *outSizeInBytes = 0u;
        return kNgxFailFeatureNotSupported;
    }
    *outSizeInBytes = kNgxCompatScratchBufferSize;
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_GetFeatureRequirements(
    void*,
    void*,
    const void* featureDiscoveryInfo,
    void* outSupported)
{
    ngxTrace("NVSDK_NGX_VULKAN_GetFeatureRequirements");
#if defined(_WIN32)
    (void)bootstrapApusrVulkanIngress();
#endif
    if (featureDiscoveryInfo == nullptr) {
        return kNgxFailInvalidParameter;
    }
    const bool supported = ngxFeatureDiscoveryLooksLikeSuperSampling(featureDiscoveryInfo);
    if (outSupported != nullptr) {
        auto* requirement = static_cast<NgxFeatureRequirementCompat*>(outSupported);
        requirement->FeatureSupported = supported ? kNgxFeatureSupportResultSupported : kNgxFeatureSupportResultAdapterUnsupported;
        requirement->MinHWArchitecture = 0u;
        std::strncpy(requirement->MinOSVersion, "10.0.10240.16384", sizeof(requirement->MinOSVersion) - 1u);
    }
    return supported ? kNgxSuccess : kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_RequiredExtensions(
    unsigned int* outInstanceExtCount,
    const char*** outInstanceExts,
    unsigned int* outDeviceExtCount,
    const char*** outDeviceExts)
{
#if defined(_WIN32)
    (void)bootstrapApusrVulkanIngress();
#endif
    if (outInstanceExtCount != nullptr) { *outInstanceExtCount = 0u; }
    if (outInstanceExts != nullptr) { *outInstanceExts = nullptr; }
    if (outDeviceExtCount != nullptr) {
        *outDeviceExtCount = static_cast<unsigned int>(std::size(kNgxVulkanDeviceExtensions));
    }
    if (outDeviceExts != nullptr) { *outDeviceExts = kNgxVulkanDeviceExtensions; }
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
    const void* featureDiscoveryInfo,
    unsigned int* outExtensionCount,
    void** outExtensionProperties)
{
    if (featureDiscoveryInfo == nullptr || outExtensionCount == nullptr) {
        return kNgxFailInvalidParameter;
    }
    if (!ngxFeatureDiscoveryLooksLikeSuperSampling(featureDiscoveryInfo)) {
        return kNgxFailFeatureNotSupported;
    }
    *outExtensionCount = 0u;
    if (outExtensionProperties != nullptr) { *outExtensionProperties = nullptr; }
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
    void*,
    void* physicalDevice,
    const void* featureDiscoveryInfo,
    unsigned int* outExtensionCount,
    void** outExtensionProperties)
{
#if defined(_WIN32)
    (void)bootstrapApusrVulkanIngress();
#endif
    if (featureDiscoveryInfo == nullptr || outExtensionCount == nullptr) {
        return kNgxFailInvalidParameter;
    }
    if (!ngxFeatureDiscoveryLooksLikeSuperSampling(featureDiscoveryInfo)) {
        return kNgxFailFeatureNotSupported;
    }
#if defined(_WIN32)
    const bool integerDotIsCore = vulkanPhysicalDeviceUsesCoreIntegerDotProduct(physicalDevice);
#else
    const bool integerDotIsCore = false;
    (void)physicalDevice;
#endif
    const VkExtensionPropertiesCompat* requiredExtensions = integerDotIsCore
        ? kNgxVulkanCoreDotDeviceExtensionProperties
        : kNgxVulkanDeviceExtensionProperties;
    const std::size_t requiredExtensionCount = integerDotIsCore
        ? std::size(kNgxVulkanCoreDotDeviceExtensionProperties)
        : std::size(kNgxVulkanDeviceExtensionProperties);

    *outExtensionCount = static_cast<unsigned int>(requiredExtensionCount);
    if (outExtensionProperties != nullptr) {
        if (*outExtensionProperties == nullptr) {
            *outExtensionProperties = const_cast<VkExtensionPropertiesCompat*>(requiredExtensions);
        } else {
            std::memcpy(
                *outExtensionProperties,
                requiredExtensions,
                requiredExtensionCount * sizeof(VkExtensionPropertiesCompat));
        }
    }
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_CreateFeature1(
    void* device,
    void* commandBuffer,
    unsigned int feature,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** outHandle);

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_CreateFeature(
    void* commandBuffer,
    unsigned int feature,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** outHandle)
{
    ngxTrace("NVSDK_NGX_VULKAN_CreateFeature");
    NgxGlobalState& state = ngxState();
    void* device = nullptr;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        device = state.vulkanDevice;
    }
    if (device == nullptr) {
        return kNgxFailNotInitialized;
    }
    return NVSDK_NGX_VULKAN_CreateFeature1(device, commandBuffer, feature, parameters, outHandle);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_CreateFeature1(
    void* device,
    void* commandBuffer,
    unsigned int feature,
    NVSDK_NGX_Parameter* parameters,
    NVSDK_NGX_Handle** outHandle)
{
    ngxTrace("NVSDK_NGX_VULKAN_CreateFeature1");
    if (feature != kNgxFeatureSuperSampling) {
        return kNgxFailFeatureNotSupported;
    }
    if (device == nullptr || commandBuffer == nullptr || parameters == nullptr || outHandle == nullptr) {
        return kNgxFailInvalidParameter;
    }

    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.vulkanDevice == nullptr) {
        return kNgxFailNotInitialized;
    }
    if (state.vulkanDevice != device) {
        return kNgxFailInvalidParameter;
    }

    apusr::frontends::NgxDlssCreateDesc create = createDescFromParams(device, parameters);
    apusr::Status status = apusr::frontends::validateNgxDlssCreateDesc(create);
    if (!status.succeeded()) {
        setLastErrorUnlocked(state, status);
        return statusToNgxResult(status);
    }

    auto context = std::make_shared<NgxFeatureContext>();
    context->handle.Id = state.nextHandleId++;
    context->handle.Magic = kNgxHandleMagic;
    context->handle.Context = context.get();
    context->api = apusr::FrameInputApi::Vulkan;
    context->create = create;
    *outHandle = &context->handle;
    state.features[context->handle.Id] = std::move(context);
    state.lastError.clear();
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_EvaluateFeature(
    void* commandBuffer,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    void*)
{
    ngxTraceHot("NVSDK_NGX_VULKAN_EvaluateFeature", gNgxVulkanEvaluateTraceCounter);
    if (commandBuffer == nullptr || handle == nullptr || parameters == nullptr) {
        return kNgxFailInvalidParameter;
    }
    NgxFeatureContext* feature = featureContextFromHandle(handle);
    if (feature == nullptr || feature->api != apusr::FrameInputApi::Vulkan) {
        return kNgxFailFeatureNotFound;
    }

    apusr::Status status = apusr::Status::ok();
    {
        std::lock_guard<std::mutex> featureLock(feature->mutex);
        apusr::frontends::NgxDlssVulkanEvaluateDesc desc;
        status = vulkanEvaluateDescFromParams(*feature, commandBuffer, parameters, desc);
        if (!status.succeeded()) {
            logVulkanEvaluateFailureOnce(*feature, "decode", status);
        }

        apusr::frontends::NgxDlssDispatchNormalization normalized;
        if (status.succeeded()) {
            logVulkanContractOnce(*feature, desc);
            status = apusr::frontends::normalizeNgxDlssVulkanEvaluateDesc(desc, normalized);
            if (!status.succeeded()) {
                logVulkanEvaluateFailureOnce(*feature, "normalize", status);
            }
        }
        if (status.succeeded()) {
#if APUSR_ENABLE_VULKAN_BACKEND
            status = recordVulkanFeature(*feature, desc, normalized);
#else
            status = apusr::Status::notImplemented("NGX/DLSS Vulkan backend is disabled in this build");
#endif
            if (!status.succeeded()) {
                logVulkanEvaluateFailureOnce(*feature, "record", status);
            } else {
                logVulkanEvaluateFailureOnce(*feature, "ok", status);
            }
        }
    }

    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    setLastErrorUnlocked(state, status);
    return statusToNgxResult(status);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_EvaluateFeature_C(
    void* commandBuffer,
    const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters,
    void* callback)
{
    return NVSDK_NGX_VULKAN_EvaluateFeature(commandBuffer, handle, parameters, callback);
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_ReleaseFeature(NVSDK_NGX_Handle* handle)
{
    if (handle == nullptr) {
        return kNgxFailInvalidParameter;
    }
    NgxGlobalState& state = ngxState();
    std::lock_guard<std::mutex> lock(state.mutex);
    auto found = state.features.find(handle->Id);
    if (found == state.features.end() || found->second->api != apusr::FrameInputApi::Vulkan) {
        return kNgxFailFeatureNotFound;
    }
    found->second->handle.Magic = 0u;
    found->second->handle.Context = nullptr;
    state.features.erase(found);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_VULKAN_PopulateParameters_Impl(NVSDK_NGX_Parameter* parameters)
{
    if (parameters == nullptr) { return kNgxFailInvalidParameter; }
    setCommonCapabilityParameters(parameters);
    return kNgxSuccess;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_DirectSR_Init(void*, void*, void*)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_DirectSR_Init", "DirectSR");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_DirectSR_Query(void*, void*)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_DirectSR_Query", "DirectSR");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_DirectSR_Create(void*, void*, void**)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_DirectSR_Create", "DirectSR");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_DirectSR_Execute(void*, void*)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_DirectSR_Execute", "DirectSR");
    return kNgxFailFeatureNotSupported;
}

APUSR_NGX_EXPORT NgxResult NVSDK_CONV NVSDK_NGX_DirectSR_Destroy(void*)
{
    ngxTraceFutureFrontendStub("NVSDK_NGX_DirectSR_Destroy", "DirectSR");
    return kNgxSuccess;
}

#if defined(_WIN32)
extern "C" BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        gApusrNgxModule = module;
        DisableThreadLibraryCalls(module);
        apusr::host_discovery::moduleLoaded("ngx", module);
        apusr::host_discovery::event("host", "verdict", "loaded=ngx next=wait_for_ngx_init_or_capability_query");
    }
    return TRUE;
}
#endif
