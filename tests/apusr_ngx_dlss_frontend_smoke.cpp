// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_ngx_dlss_dx12_frontend.h>
#include <apusr/apusr_frontend.h>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#include <d3d12.h>
#include <dxgi1_6.h>
#endif


struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

struct NgxFeatureRequirementCompat {
    unsigned int FeatureSupported = 4u;
    unsigned int MinHWArchitecture = 0u;
    char MinOSVersion[255] = {};
};

struct NgxFeatureDiscoveryPrefix {
    std::uint32_t sdkVersion = 0;
    std::uint32_t featureId = 0;
};

struct NgxVkSubresourceRangeAbi {
    std::uint32_t aspectMask = 0;
    std::uint32_t baseMipLevel = 0;
    std::uint32_t levelCount = 1;
    std::uint32_t baseArrayLayer = 0;
    std::uint32_t layerCount = 1;
};

struct NgxVkImageViewInfoAbi {
    std::uintptr_t imageView = 0;
    std::uintptr_t image = 0;
    NgxVkSubresourceRangeAbi subresourceRange;
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct NgxVkResourceAbi {
    NgxVkImageViewInfoAbi image;
    std::uint32_t type = 0;
    bool readWrite = false;
};

static_assert(sizeof(NgxVkResourceAbi) == 56);

const char kEParamSuperSamplingAvailable[] = {'#', '\x01', '\0'};
const char kEParamDlssOptimalSettingsCallback[] = {'#', '\x3e', '\0'};
const char kEParamOutWidth[] = {'#', '\x12', '\0'};
const char kEParamOutHeight[] = {'#', '\x13', '\0'};

extern "C" unsigned int NVSDK_NGX_D3D12_Init(
    unsigned long long, const wchar_t*, ID3D12Device*, const void*, unsigned int);
extern "C" unsigned int NVSDK_NGX_D3D12_Shutdown();
extern "C" unsigned int NVSDK_NGX_D3D12_GetCapabilityParameters(NVSDK_NGX_Parameter**);
extern "C" unsigned int NVSDK_NGX_D3D12_GetFeatureRequirements(void*, const void*, void*);
extern "C" unsigned int NVSDK_NGX_D3D12_GetScratchBufferSize(unsigned int, const NVSDK_NGX_Parameter*, std::size_t*);
extern "C" unsigned int NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter*);
extern "C" unsigned int NVSDK_NGX_D3D12_CreateFeature(
    ID3D12GraphicsCommandList*, unsigned int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
extern "C" unsigned int NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle*);
extern "C" unsigned int NVSDK_NGX_VULKAN_Init(
    unsigned long long, const wchar_t*, void*, void*, void*, void*, void*, const void*, unsigned int);
extern "C" unsigned int NVSDK_NGX_VULKAN_Init_Ext2(
    unsigned long long, const wchar_t*, void*, void*, void*, void*, void*, unsigned int, const void*);
extern "C" unsigned int NVSDK_NGX_VULKAN_Init_with_ProjectID(
    const char*, unsigned int, const char*, const wchar_t*, void*, void*, void*, void*, void*, const void*, unsigned int);
extern "C" unsigned int NVSDK_NGX_VULKAN_Shutdown();
extern "C" unsigned int NVSDK_NGX_VULKAN_GetCapabilityParameters(NVSDK_NGX_Parameter**);
extern "C" unsigned int NVSDK_NGX_VULKAN_DestroyParameters(NVSDK_NGX_Parameter*);
extern "C" unsigned int NVSDK_NGX_VULKAN_GetFeatureRequirements(void*, void*, const void*, void*);
extern "C" unsigned int NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(const void*, unsigned int*, void**);
extern "C" unsigned int NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(void*, void*, const void*, unsigned int*, void**);
extern "C" unsigned int NVSDK_NGX_VULKAN_CreateFeature1(void*, void*, unsigned int, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
extern "C" unsigned int NVSDK_NGX_VULKAN_EvaluateFeature(void*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
extern "C" unsigned int NVSDK_NGX_VULKAN_ReleaseFeature(NVSDK_NGX_Handle*);
extern "C" unsigned int NVSDK_NGX_DLSS_GetOptimalSettingsCallback(NVSDK_NGX_Parameter*);
extern "C" unsigned int NVSDK_NGX_DLSS_GetStatsCallback(NVSDK_NGX_Parameter*);
extern "C" unsigned int NVSDK_NGX_GetAPIVersion();
extern "C" unsigned int NVSDK_NGX_GetApplicationId();
extern "C" unsigned int NVSDK_NGX_GetDriverVersion();
extern "C" unsigned int NVSDK_NGX_GetDriverVersionEx(unsigned int*, unsigned int, unsigned int*);
extern "C" unsigned int NVSDK_NGX_GetGPUArchitecture();
extern "C" unsigned int NVSDK_NGX_GetSnippetVersion();
extern "C" unsigned int NVSDK_NGX_SetInfoCallback(void*);
extern "C" unsigned int NVSDK_NGX_SetTelemetryEvaluateCallback(void*);
extern "C" unsigned int NGX_SNIPPETS_GetRequiredDriverSupport();
extern "C" void NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter*, const char*, unsigned int);
extern "C" void NVSDK_NGX_Parameter_SetI(NVSDK_NGX_Parameter*, const char*, int);
extern "C" void NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter*, const char*, void*);
extern "C" void NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter*, const char*, void*);
extern "C" void NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter*, const char*, float);
extern "C" unsigned int NVSDK_NGX_Parameter_GetUI(NVSDK_NGX_Parameter*, const char*, unsigned int*);
extern "C" unsigned int NVSDK_NGX_Parameter_GetULL(NVSDK_NGX_Parameter*, const char*, unsigned long long*);
extern "C" unsigned int NVSDK_NGX_Parameter_GetF(NVSDK_NGX_Parameter*, const char*, float*);
extern "C" unsigned int NVSDK_NGX_Parameter_GetVoidPointer(NVSDK_NGX_Parameter*, const char*, void**);
#if APUSR_ENABLE_VULKAN_BACKEND && !defined(_WIN32)
extern "C" void apusrFakeVulkanResetCounters();
extern "C" std::uint64_t apusrFakeVulkanDispatchCount();
extern "C" std::uint64_t apusrFakeVulkanPipelineCount();
#endif

namespace {

#if defined(_WIN32)
ID3D12Device* createWarpD3D12Device()
{
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) || factory == nullptr) {
        return nullptr;
    }

    IDXGIAdapter* warpAdapter = nullptr;
    const HRESULT enumResult = factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter));
    factory->Release();
    if (FAILED(enumResult) || warpAdapter == nullptr) {
        return nullptr;
    }

    ID3D12Device* device = nullptr;
    const HRESULT deviceResult = D3D12CreateDevice(
        warpAdapter,
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&device));
    warpAdapter->Release();
    return SUCCEEDED(deviceResult) ? device : nullptr;
}
#endif

apusr::NativeHandle handle(const std::uintptr_t value)
{
    return apusr::NativeHandle{reinterpret_cast<void*>(value)};
}

apusr::frontends::NgxDlssEvaluateDesc makeEvaluateDesc()
{
    return apusr::frontends::NgxDlssEvaluateDesc{
        .commandList = handle(0x1000),
        .device = handle(0x1001),
        .color = handle(0x2001),
        .depth = handle(0x2002),
        .motionVectors = handle(0x2003),
        .exposure = handle(0x2004),
        .reactiveMask = handle(0x2005),
        .output = handle(0x2006),
        .renderSize = apusr::RenderSize{.width = 1280u, .height = 720u},
        .displaySize = apusr::DisplaySize{.width = 1920u, .height = 1080u},
        .motionVectorSize = apusr::RenderSize{.width = 1920u, .height = 1080u},
        .jitter = apusr::Jitter{.x = 0.25f, .y = -0.375f},
        .previousJitter = apusr::Jitter{.x = -0.25f, .y = 0.375f},
        .motionVectorScale = apusr::MotionVectorScale{.x = 1.0f, .y = 1.0f},
        .qualityMode = apusr::frontends::NgxDlssPerfQualityMode::Balanced,
        .featureCreateFlags = apusr::frontends::kNgxDlssFeatureCreateFlagMvJittered |
            apusr::frontends::kNgxDlssFeatureCreateFlagDepthInverted,
        .frameIndex = 42u,
        .colorState = 0x40u,
        .depthState = 0x40u,
        .motionVectorState = 0x40u,
        .exposureState = 0x40u,
        .reactiveMaskState = 0x40u,
        .outputState = 0x8u,
        .resetHistory = true,
        .sharpeningRequested = true,
        .sharpness = 0.3f,
        .frameTimeDeltaMs = 16.6f,
        .preExposure = 1.25f,
    };
}

} // namespace

int main()
{
    const apusr::frontends::NgxDlssCreateDesc create{
        .device = handle(0x1001),
        .renderSize = apusr::RenderSize{.width = 1280u, .height = 720u},
        .displaySize = apusr::DisplaySize{.width = 1920u, .height = 1080u},
        .qualityMode = apusr::frontends::NgxDlssPerfQualityMode::Balanced,
    };
    assert(apusr::frontends::validateNgxDlssCreateDesc(create).succeeded());

    assert(apusr::frontends::ngxDlssQualityModeToApusr(apusr::frontends::NgxDlssPerfQualityMode::MaxQuality) ==
        apusr::UpscalerQualityMode::Quality);
    assert(apusr::frontends::ngxDlssQualityModeToApusr(apusr::frontends::NgxDlssPerfQualityMode::NativeAA) ==
        apusr::UpscalerQualityMode::NativeAA);

    const apusr::UpscalerFeatureFlags lowResMv = apusr::frontends::decodeNgxDlssFeatureFlags(
        apusr::frontends::kNgxDlssFeatureCreateFlagIsHdr |
        apusr::frontends::kNgxDlssFeatureCreateFlagMvLowRes |
        apusr::frontends::kNgxDlssFeatureCreateFlagAutoExposure);
    assert(lowResMv.highDynamicRange);
    assert(!lowResMv.displayResolutionMotionVectors);
    assert(lowResMv.autoExposure);

    apusr::frontends::NgxDlssEvaluateDesc desc = makeEvaluateDesc();
    apusr::frontends::NgxDlssDispatchNormalization normalized;
    apusr::Status status = apusr::frontends::normalizeNgxDlssEvaluateDesc(desc, normalized);
    assert(status.succeeded());
    assert(normalized.frameInput.surfaceCount == 6u);
    assert(normalized.frameInput.command.api == apusr::FrameInputApi::Dx12);
    assert(normalized.frameInput.command.commandListOrBuffer.value == desc.commandList.value);
    assert(normalized.frame.frameIndex == 42u);
    assert(normalized.frame.renderSize.width == 1280u);
    assert(normalized.frame.displaySize.height == 1080u);
    assert(normalized.frame.borrowedCommandList.value == desc.commandList.value);
    assert(normalized.frame.surfaces.color.resource.value == desc.color.value);
    assert(normalized.frame.surfaces.output.resource.value == desc.output.value);
    assert(normalized.frame.surfaces.motionVectors.size.width == 1920u);
    assert(normalized.frame.options.qualityMode == apusr::UpscalerQualityMode::Balanced);
    assert(normalized.frame.options.features.motionVectorJitterCancellation);
    assert(normalized.frame.options.features.depthInverted);
    assert(normalized.frame.options.hasDepth);
    assert(normalized.frame.options.hasExposure);
    assert(normalized.frame.options.hasReactiveMask);
    assert(normalized.frame.options.resetHistory);
    assert(normalized.frame.options.sharpeningRequested);
    assert(normalized.frame.options.preExposure == 1.25f);

    desc.color = {};
    status = apusr::frontends::normalizeNgxDlssEvaluateDesc(desc, normalized);
    assert(!status.succeeded());

    const auto vkImage = [](std::uintptr_t image, std::uintptr_t view, apusr::SurfaceFormat format,
                            std::uint32_t nativeFormat, std::uint32_t width, std::uint32_t height,
                            std::uint32_t aspectMask, bool readWrite = false) {
        return apusr::frontends::NgxVulkanImageResourceDesc{
            .image = handle(image),
            .imageView = handle(view),
            .format = format,
            .extent = apusr::RenderSize{.width = width, .height = height},
            .nativeFormat = nativeFormat,
            .aspectMask = aspectMask,
            .readWrite = readWrite,
        };
    };
    apusr::frontends::NgxDlssVulkanEvaluateDesc vkDesc{};
    vkDesc.commandBuffer = handle(0x3000);
    vkDesc.commandQueue = handle(0x3002);
    vkDesc.commandQueueFamilyIndex = 7u;
    vkDesc.graphicsQueueFamilyIndex = 7u;
    vkDesc.computeQueueFamilyIndex = 9u;
    vkDesc.device = handle(0x3001);
    vkDesc.stateContract = apusr::frontends::NgxDlssVulkanStateContract::NgxEvaluatePrepared;
    vkDesc.color = vkImage(0x4001, 0x5001, apusr::SurfaceFormat::Rgba16Float, 97u, 1280u, 720u, 1u);
    vkDesc.depth = vkImage(0x4002, 0x5002, apusr::SurfaceFormat::R32Float, 126u, 1280u, 720u, 2u);
    vkDesc.motionVectors = vkImage(0x4003, 0x5003, apusr::SurfaceFormat::Rg16Float, 83u, 1920u, 1080u, 1u);
    vkDesc.exposure = vkImage(0x4004, 0x5004, apusr::SurfaceFormat::R32Float, 100u, 1u, 1u, 1u);
    vkDesc.output = vkImage(0x4005, 0x5005, apusr::SurfaceFormat::Rgba16Float, 97u, 1920u, 1080u, 1u, true);
    vkDesc.renderSize = apusr::RenderSize{.width = 1280u, .height = 720u};
    vkDesc.displaySize = apusr::DisplaySize{.width = 1920u, .height = 1080u};
    vkDesc.motionVectorSize = apusr::RenderSize{.width = 1920u, .height = 1080u};
    vkDesc.qualityMode = apusr::frontends::NgxDlssPerfQualityMode::Balanced;
    vkDesc.preExposure = 1.0f;
    status = apusr::frontends::normalizeNgxDlssVulkanEvaluateDesc(vkDesc, normalized);
    assert(status.succeeded());
    assert(normalized.frameInput.command.api == apusr::FrameInputApi::Vulkan);
    assert(normalized.frameInput.command.commandQueue.value == handle(0x3002).value);
    assert(normalized.frameInput.command.nativeQueueFamilyIndex == 7u);
    const apusr::FrameInputSurfaceDesc* vkColor = apusr::findFrameInputSurface(normalized.frameInput, apusr::FrameInputSurfaceRole::Color);
    const apusr::FrameInputSurfaceDesc* vkOutput = apusr::findFrameInputSurface(normalized.frameInput, apusr::FrameInputSurfaceRole::Output);
    assert(vkColor != nullptr && vkColor->view.value == handle(0x5001).value);
    assert(vkColor->nativeFormat == 97u && vkColor->nativeAspectMask == 1u);
    assert(vkColor->nativeStateOrLayout == 5u);
    assert(vkColor->nativeStageMask == 0x800u && vkColor->nativeAccessMask == 0x20u);
    assert(vkOutput != nullptr && vkOutput->nativeStateOrLayout == 1u);
    assert(vkOutput->nativeStageMask == 0x800u && vkOutput->nativeAccessMask == 0x60u);
    vkDesc.colorSubrectBaseX = 1u;
    status = apusr::frontends::normalizeNgxDlssVulkanEvaluateDesc(vkDesc, normalized);
    assert(!status.succeeded() && status.code == apusr::StatusCode::NotImplemented);

    const apusr::FrontendCompatibilityDesc frontend = apusr::makeNgxDlssDx12DropInFrontend();
    assert(apusr::validateFrontendCompatibilityDesc(frontend).succeeded());
    assert(frontend.persona == apusr::FrontendPersona::NgxDlss);
    assert(frontend.abi == apusr::FrontendAbi::NvngxDlss);
    assert(frontend.ingressContract.protocol == apusr::UpscalerProtocol::Dlss);
    assert(frontend.deployment.outputBaseName == "nvngx_dlss");
    assert(frontend.deployment.packaging == apusr::FrontendPackaging::DropInSharedLibrary);
    assert(apusr::frontendNeedsOriginLedger(frontend));

    assert(apusrNgxDlssDx12FrontendAbiVersion() == 1u);
    assert(std::strcmp(apusrNgxDlssDx12FrontendOutputBaseName(), "nvngx_dlss") == 0);
    assert(apusrNgxDlssDx12FrontendUsesNvngxHooks());
    assert(!apusrNgxDlssDx12FrontendUsesThirdPartyModelArtifacts());
    assert(apusrNgxDlssDx12FrontendUsesApusrModelArtifacts());

    constexpr unsigned int kNgxSuccess = 1u;

    unsigned int driverVersions[2] = {};
    unsigned int driverVersionCount = 0u;
    assert(NVSDK_NGX_GetAPIVersion() == 19u);
    assert(NVSDK_NGX_GetApplicationId() == 0x0BADF00Du);
    assert(NVSDK_NGX_GetDriverVersion() == 0x23A0000u);
    assert(NVSDK_NGX_GetDriverVersionEx(driverVersions, 2u, &driverVersionCount) == kNgxSuccess);
    assert(driverVersionCount == 2u);
    assert(driverVersions[0] == 0x23Au);
    assert(driverVersions[1] == 0u);
    assert(NVSDK_NGX_GetGPUArchitecture() == 0x0160u);
    assert(NVSDK_NGX_GetSnippetVersion() == 0x00030500u);
    assert(NGX_SNIPPETS_GetRequiredDriverSupport() == 0x23A0000u);
    assert(NVSDK_NGX_SetInfoCallback(reinterpret_cast<void*>(0x1234)) == kNgxSuccess);
    assert(NVSDK_NGX_SetTelemetryEvaluateCallback(reinterpret_cast<void*>(0x5678)) == kNgxSuccess);
    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    assert(NVSDK_NGX_D3D12_GetCapabilityParameters(&capabilityParams) == kNgxSuccess);
    assert(capabilityParams != nullptr);
    unsigned int superSamplingAvailable = 0u;
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, "SuperSampling.Available", &superSamplingAvailable) == kNgxSuccess);
    assert(superSamplingAvailable == 1u);
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, kEParamSuperSamplingAvailable, &superSamplingAvailable) == kNgxSuccess);
    assert(superSamplingAvailable == 1u);
    void* optimalCallback = nullptr;
    assert(NVSDK_NGX_Parameter_GetVoidPointer(capabilityParams, "DLSSOptimalSettingsCallback", &optimalCallback) == kNgxSuccess);
    assert(optimalCallback != nullptr);
    void* optimalCallbackByEnum = nullptr;
    assert(NVSDK_NGX_Parameter_GetVoidPointer(capabilityParams, kEParamDlssOptimalSettingsCallback, &optimalCallbackByEnum) == kNgxSuccess);
    assert(optimalCallbackByEnum == optimalCallback);
    NgxFeatureRequirementCompat requirement{};
    NgxFeatureDiscoveryPrefix discoveryInfo{.sdkVersion = 0x13u, .featureId = 1u};
    assert(NVSDK_NGX_D3D12_GetFeatureRequirements(nullptr, &discoveryInfo, &requirement) == kNgxSuccess);
    assert(requirement.FeatureSupported == 0u);
    assert(std::strcmp(requirement.MinOSVersion, "10.0.10240.16384") == 0);

    NgxFeatureRequirementCompat unsupportedRequirement{};
    NgxFeatureDiscoveryPrefix unsupportedDiscoveryInfo{.sdkVersion = 0x13u, .featureId = 10u}; // Non-SR NGX feature ids fail closed.
    assert(NVSDK_NGX_D3D12_GetFeatureRequirements(nullptr, &unsupportedDiscoveryInfo, &unsupportedRequirement) != kNgxSuccess);
    assert(unsupportedRequirement.FeatureSupported != 0u);

    std::size_t scratchBytes = 0;
    assert(NVSDK_NGX_D3D12_GetScratchBufferSize(1u, capabilityParams, &scratchBytes) == kNgxSuccess);
    assert(scratchBytes == 50ull * 1024ull * 1024ull);
    float defaultSharpness = -1.0f;
    assert(NVSDK_NGX_Parameter_GetF(capabilityParams, "Sharpness", &defaultSharpness) == kNgxSuccess);
    assert(defaultSharpness == 0.0f);
    NVSDK_NGX_Parameter_SetUI(capabilityParams, "Width", 1920u);
    NVSDK_NGX_Parameter_SetUI(capabilityParams, "Height", 1080u);
    NVSDK_NGX_Parameter_SetI(capabilityParams, "PerfQualityValue", 1);
    assert(NVSDK_NGX_DLSS_GetOptimalSettingsCallback(capabilityParams) == kNgxSuccess);
    unsigned int recommendedWidth = 0;
    unsigned int recommendedHeight = 0;
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, "OutWidth", &recommendedWidth) == kNgxSuccess);
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, "OutHeight", &recommendedHeight) == kNgxSuccess);
    assert(recommendedWidth > 1000u && recommendedWidth < 1200u);
    assert(recommendedHeight > 600u && recommendedHeight < 700u);
    unsigned int dottedOutWidth = 0;
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, "Out.Width", &dottedOutWidth) == kNgxSuccess);
    assert(dottedOutWidth == recommendedWidth);
    unsigned int enumOutWidth = 0;
    unsigned int enumOutHeight = 0;
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, kEParamOutWidth, &enumOutWidth) == kNgxSuccess);
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, kEParamOutHeight, &enumOutHeight) == kNgxSuccess);
    assert(enumOutWidth == recommendedWidth);
    assert(enumOutHeight == recommendedHeight);

    // A second query must use the caller's newly supplied Width/Height rather than
    // the previous query's OutWidth/OutHeight values.
    NVSDK_NGX_Parameter_SetUI(capabilityParams, "Width", 2560u);
    NVSDK_NGX_Parameter_SetUI(capabilityParams, "Height", 1440u);
    assert(NVSDK_NGX_DLSS_GetOptimalSettingsCallback(capabilityParams) == kNgxSuccess);
    unsigned int repeatedWidth = 0;
    unsigned int repeatedHeight = 0;
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, "OutWidth", &repeatedWidth) == kNgxSuccess);
    assert(NVSDK_NGX_Parameter_GetUI(capabilityParams, "OutHeight", &repeatedHeight) == kNgxSuccess);
    assert(repeatedWidth > recommendedWidth);
    assert(repeatedHeight > recommendedHeight);

    NVSDK_NGX_Parameter* createParams = nullptr;
    assert(NVSDK_NGX_D3D12_GetCapabilityParameters(&createParams) == kNgxSuccess);
    assert(createParams != nullptr);
    NVSDK_NGX_Parameter_SetUI(createParams, "Width", 1280u);
    NVSDK_NGX_Parameter_SetUI(createParams, "Height", 720u);
    NVSDK_NGX_Parameter_SetUI(createParams, "OutWidth", 1920u);
    NVSDK_NGX_Parameter_SetUI(createParams, "OutHeight", 1080u);
    NVSDK_NGX_Parameter_SetI(createParams, "PerfQualityValue", 1);
#if defined(_WIN32)
    ID3D12Device* d3d12TestDevice = createWarpD3D12Device();
    assert(d3d12TestDevice != nullptr);
#else
    ID3D12Device* d3d12TestDevice = reinterpret_cast<ID3D12Device*>(0x1001);
#endif
    assert(NVSDK_NGX_D3D12_Init(0, nullptr, d3d12TestDevice, nullptr, 0) == kNgxSuccess);
    NVSDK_NGX_Handle* ngxHandle = nullptr;
    assert(NVSDK_NGX_D3D12_CreateFeature(nullptr, 1u, createParams, &ngxHandle) == kNgxSuccess);
    assert(ngxHandle != nullptr);
    assert(NVSDK_NGX_D3D12_ReleaseFeature(ngxHandle) == kNgxSuccess);
    assert(NVSDK_NGX_D3D12_DestroyParameters(createParams) == kNgxSuccess);
    assert(NVSDK_NGX_D3D12_DestroyParameters(capabilityParams) == kNgxSuccess);
    assert(NVSDK_NGX_D3D12_Shutdown() == kNgxSuccess);
#if defined(_WIN32)
    d3d12TestDevice->Release();
#endif

    void* vkInstance = reinterpret_cast<void*>(0x3000);
    void* vkPhysicalDevice = reinterpret_cast<void*>(0x3001);
    void* vkDevice = reinterpret_cast<void*>(0x3002);
    void* vkCommandBuffer = reinterpret_cast<void*>(0x3003);
    assert(NVSDK_NGX_VULKAN_Init_Ext2(
               123u, nullptr, vkInstance, vkPhysicalDevice, vkDevice, nullptr, nullptr, 0x13u, nullptr) == kNgxSuccess);
    assert(NVSDK_NGX_VULKAN_Init(
               123u, nullptr, vkInstance, vkPhysicalDevice, vkDevice, nullptr, nullptr, nullptr, 0x13u) == kNgxSuccess);

    NgxFeatureRequirementCompat vkRequirement{};
    assert(NVSDK_NGX_VULKAN_GetFeatureRequirements(vkInstance, vkPhysicalDevice, &discoveryInfo, &vkRequirement) == kNgxSuccess);
    assert(vkRequirement.FeatureSupported == 0u);
    unsigned int extensionCount = 99u;
    void* extensionProperties = reinterpret_cast<void*>(0x1);
    assert(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&discoveryInfo, &extensionCount, &extensionProperties) == kNgxSuccess);
    assert(extensionCount == 0u && extensionProperties == nullptr);
    extensionCount = 99u;
    struct VkExtensionPropertiesCompat {
        char extensionName[256];
        std::uint32_t specVersion;
    } deviceExtensions[2]{};
    extensionProperties = deviceExtensions;
    assert(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(vkInstance, vkPhysicalDevice, &discoveryInfo, &extensionCount, &extensionProperties) == kNgxSuccess);
    assert(extensionCount == 2u);
    assert(std::strcmp(deviceExtensions[0].extensionName, "VK_KHR_shader_integer_dot_product") == 0);
    assert(std::strcmp(deviceExtensions[1].extensionName, "VK_KHR_compute_shader_derivatives") == 0);

    NVSDK_NGX_Parameter* vkParams = nullptr;
    assert(NVSDK_NGX_VULKAN_GetCapabilityParameters(&vkParams) == kNgxSuccess && vkParams != nullptr);
    void* statsCallbackRaw = nullptr;
    assert(NVSDK_NGX_Parameter_GetVoidPointer(vkParams, "DLSSGetStatsCallback", &statsCallbackRaw) == kNgxSuccess);
    assert(statsCallbackRaw != nullptr);
    using StatsCallback = unsigned int (*)(NVSDK_NGX_Parameter*);
    auto statsCallback = reinterpret_cast<StatsCallback>(statsCallbackRaw);
    NVSDK_NGX_Parameter_SetUI(vkParams, "OutWidth", 2560u);
    NVSDK_NGX_Parameter_SetUI(vkParams, "OutHeight", 1440u);
    assert(statsCallback(vkParams) == kNgxSuccess);
    unsigned long long estimatedBytes = 0;
    assert(NVSDK_NGX_Parameter_GetULL(vkParams, "SizeInBytes", &estimatedBytes) == kNgxSuccess);
    assert(estimatedBytes == 2560ull * 1440ull * 31ull);
#if APUSR_ENABLE_VULKAN_BACKEND && !defined(_WIN32)
    NVSDK_NGX_Parameter_SetUI(vkParams, "Width", 1920u);
    NVSDK_NGX_Parameter_SetUI(vkParams, "Height", 1080u);
    NVSDK_NGX_Parameter_SetUI(vkParams, "OutWidth", 3840u);
    NVSDK_NGX_Parameter_SetUI(vkParams, "OutHeight", 2160u);
    NVSDK_NGX_Parameter_SetUI(vkParams, "DLSS.Render.Subrect.Dimensions.Width", 1920u);
    NVSDK_NGX_Parameter_SetUI(vkParams, "DLSS.Render.Subrect.Dimensions.Height", 1080u);
    NVSDK_NGX_Parameter_SetI(vkParams, "PerfQualityValue", 0);
    NVSDK_NGX_Parameter_SetUI(
        vkParams,
        "DLSS.Feature.Create.Flags",
        apusr::frontends::kNgxDlssFeatureCreateFlagMvLowRes |
            apusr::frontends::kNgxDlssFeatureCreateFlagDepthInverted |
            apusr::frontends::kNgxDlssFeatureCreateFlagAutoExposure);
    NVSDK_NGX_Parameter_SetF(vkParams, "MV.Scale.X", 1920.0f);
    NVSDK_NGX_Parameter_SetF(vkParams, "MV.Scale.Y", 1080.0f);
    NVSDK_NGX_Parameter_SetF(vkParams, "Sharpness", 0.5f);

    NgxVkResourceAbi vkColorResource{{0x5100, 0x5000, {1u, 0u, 1u, 0u, 1u}, 97u, 1920u, 1080u}, 0u, false};
    NgxVkResourceAbi vkDepthResource{{0x5300, 0x5200, {2u, 0u, 1u, 0u, 1u}, 126u, 1920u, 1080u}, 0u, false};
    NgxVkResourceAbi vkMotionResource{{0x5500, 0x5400, {1u, 0u, 1u, 0u, 1u}, 83u, 1920u, 1080u}, 0u, false};
    NgxVkResourceAbi vkOutputResource{{0x5700, 0x5600, {1u, 0u, 1u, 0u, 1u}, 97u, 3840u, 2160u}, 0u, true};
    NVSDK_NGX_Parameter_SetVoidPointer(vkParams, "Color", &vkColorResource);
    NVSDK_NGX_Parameter_SetVoidPointer(vkParams, "Depth", &vkDepthResource);
    NVSDK_NGX_Parameter_SetVoidPointer(vkParams, "MotionVectors", &vkMotionResource);
    NVSDK_NGX_Parameter_SetVoidPointer(vkParams, "Output", &vkOutputResource);

    apusrFakeVulkanResetCounters();
    NVSDK_NGX_Handle* vkFeature = nullptr;
    assert(NVSDK_NGX_VULKAN_CreateFeature1(vkDevice, vkCommandBuffer, 1u, vkParams, &vkFeature) == kNgxSuccess);
    assert(vkFeature != nullptr);
    const unsigned int vkEvaluateResult = NVSDK_NGX_VULKAN_EvaluateFeature(vkCommandBuffer, vkFeature, vkParams, nullptr);
    assert(vkEvaluateResult != kNgxSuccess);
    assert(apusrFakeVulkanPipelineCount() == 0u);
    assert(apusrFakeVulkanDispatchCount() == 0u);
    assert(NVSDK_NGX_VULKAN_ReleaseFeature(vkFeature) == kNgxSuccess);
#else
    // Native-handle execution is covered by platform Vulkan tests. Do not feed fabricated
    // handles into a real Windows Vulkan loader from this frontend ABI smoke.
    (void)vkCommandBuffer;
#endif
    assert(NVSDK_NGX_VULKAN_DestroyParameters(vkParams) == kNgxSuccess);
    assert(NVSDK_NGX_VULKAN_Shutdown() == kNgxSuccess);

    std::cout << "APUSR NGX/DLSS frontend smoke: DX12 ingress plus Vulkan NGX-to-recorder contract\n";
    return 0;
}
