// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_fsr4_dx12_frontend.h>
#include <cstdint>
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

std::uint32_t debugErrorCount = 0;

void debugCallback(const std::uint32_t type, const wchar_t*)
{
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        ++debugErrorCount;
    }
}

} // namespace

int main()
{
    std::uint64_t providerCount = 0;
    ffxQueryDescGetVersions countVersions{};
    countVersions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    countVersions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    countVersions.outputCount = &providerCount;
    require(ffxQuery(nullptr, &countVersions.header) == FFX_API_RETURN_OK, "provider version count query should succeed");
    require(providerCount == 1, "provider version count query should expose one FSR4-compatible provider");

    std::uint64_t providerId = 0;
    const char* providerName = nullptr;
    ffxQueryDescGetVersions getVersions{};
    getVersions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    getVersions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    getVersions.outputCount = &providerCount;
    getVersions.versionIds = &providerId;
    getVersions.versionNames = &providerName;
    require(ffxQuery(nullptr, &getVersions.header) == FFX_API_RETURN_OK, "provider version enumeration should succeed");
    require(providerCount == 1, "provider version enumeration should return one FSR4-compatible provider");
    require(providerId != 0, "provider version enumeration should return a concrete provider id");
    require(providerName != nullptr, "provider version enumeration should return a provider name");

    std::uint64_t frameGenerationProviderCount = 0;
    ffxQueryDescGetVersions frameGenerationVersions{};
    frameGenerationVersions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    frameGenerationVersions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    frameGenerationVersions.outputCount = &frameGenerationProviderCount;
    require(ffxQuery(nullptr, &frameGenerationVersions.header) == FFX_API_RETURN_NO_PROVIDER,
            "APUSR should not claim the FidelityFX Frame Generation provider when no real passthrough provider is present");

    ffxContext frameGenerationContext = nullptr;
    ffxCreateContextDescHeader frameGenerationCreate{};
    frameGenerationCreate.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    require(ffxCreateContext(&frameGenerationContext, &frameGenerationCreate, nullptr) == FFX_API_RETURN_NO_PROVIDER,
            "APUSR should delegate or reject Frame Generation contexts instead of treating them as upscaling contexts");
    require(frameGenerationContext == nullptr, "unhandled Frame Generation create must leave the context null");

    float qualityRatio = 0.0f;
    ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode ratioQuery{};
    ratioQuery.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE;
    ratioQuery.qualityMode = 1;
    ratioQuery.pOutUpscaleRatio = &qualityRatio;
    require(ffxQuery(nullptr, &ratioQuery.header) == FFX_API_RETURN_OK, "quality ratio query should succeed");
    require(qualityRatio == 1.5f, "quality mode should report the expected per-dimension ratio");

    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    ffxQueryDescUpscaleGetRenderResolutionFromQualityMode renderResolutionQuery{};
    renderResolutionQuery.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
    renderResolutionQuery.displayWidth = 2560;
    renderResolutionQuery.displayHeight = 1440;
    renderResolutionQuery.qualityMode = 1;
    renderResolutionQuery.pOutRenderWidth = &renderWidth;
    renderResolutionQuery.pOutRenderHeight = &renderHeight;
    require(ffxQuery(nullptr, &renderResolutionQuery.header) == FFX_API_RETURN_OK, "render resolution query should succeed");
    require(renderWidth == 1706 && renderHeight == 960, "Quality render-resolution query must use the FSR integer contract");

    ffxConfigureDescGlobalDebug globalDebug{};
    globalDebug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    globalDebug.effectId = FFX_API_EFFECT_ID_UPSCALE;
    globalDebug.fpMessage = &debugCallback;
    globalDebug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
    require(ffxConfigure(nullptr, &globalDebug.header) == FFX_API_RETURN_OK, "SDK 2.3 GlobalDebug should be accepted before context creation");

    ffxContext invalidVersionContext = nullptr;
    ffxCreateContextDescUpscaleVersion invalidVersion{};
    invalidVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    invalidVersion.version = FFX_UPSCALER_MAKE_VERSION(5u, 0u, 0u);
    ffxCreateContextDescUpscale invalidVersionCreate{};
    invalidVersionCreate.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    invalidVersionCreate.header.pNext = &invalidVersion.header;
    invalidVersionCreate.maxRenderSize = FfxApiDimensions2D{.width = 1920, .height = 1080};
    invalidVersionCreate.maxUpscaleSize = FfxApiDimensions2D{.width = 3840, .height = 2160};
    require(ffxCreateContext(&invalidVersionContext, &invalidVersionCreate.header, nullptr) == FFX_API_RETURN_ERROR_PARAMETER,
            "future unsupported upscaler API versions must fail instead of silently assuming ABI compatibility");
    require(invalidVersionContext == nullptr, "invalid API version must not create a context");
    require(debugErrorCount == 1, "invalid API version should reach the configured SDK 2.3 global debug callback");

    ffxContext context = nullptr;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = nullptr;

    ffxCreateContextDescUpscaleVersion apiVersion{};
    apiVersion.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    apiVersion.version = FFX_UPSCALER_VERSION;
    apiVersion.header.pNext = &backend.header;

    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = &apiVersion.header;
    create.maxRenderSize = FfxApiDimensions2D{.width = 1920, .height = 1080};
    create.maxUpscaleSize = FfxApiDimensions2D{.width = 3840, .height = 2160};
    require(ffxCreateContext(&context, &create.header, nullptr) == FFX_API_RETURN_OK, "FSR4 ABI context creation should succeed");
    require(context != nullptr, "FSR4 ABI context should be non-null");
    const auto loaderProviderWord = *static_cast<const void* const*>(context);
    require(loaderProviderWord != nullptr, "FSR4 ABI context should start with a loader-visible provider pointer");
    require(reinterpret_cast<std::uintptr_t>(loaderProviderWord) != 0x41505553u, "FSR4 ABI context must not expose APUSR magic where the FFX loader expects a provider pointer");

    ffxQueryGetProviderVersion providerVersion{};
    providerVersion.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    require(ffxQuery(&context, &providerVersion.header) == FFX_API_RETURN_OK, "context provider-version query should succeed");
    require(providerVersion.versionId == providerId, "context provider-version query should match enumerated provider id");
    require(providerVersion.versionName != nullptr, "context provider-version query should return a provider name");

    ffxQueryDescUpscaleGetResourceRequirements resourceRequirements{};
    resourceRequirements.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
    require(ffxQuery(&context, &resourceRequirements.header) == FFX_API_RETURN_OK, "resource-requirements query should succeed");
    require((resourceRequirements.required_resources & FFX_API_QUERY_RESOURCE_INPUT_COLOR) != 0, "resource requirements should require color input");
    require((resourceRequirements.required_resources & FFX_API_QUERY_RESOURCE_INPUT_MV) != 0, "resource requirements should require motion vectors");

    FfxApiEffectMemoryUsage memoryUsage{};
    ffxQueryDescUpscaleGetGPUMemoryUsage memoryQuery{};
    memoryQuery.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    memoryQuery.gpuMemoryUsageUpscaler = &memoryUsage;
    require(ffxQuery(&context, &memoryQuery.header) == FFX_API_RETURN_OK, "memory-usage query should succeed");

    int32_t phaseCount = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount phaseQuery{};
    phaseQuery.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    phaseQuery.renderWidth = 1920;
    phaseQuery.displayWidth = 3840;
    phaseQuery.pOutPhaseCount = &phaseCount;
    require(ffxQuery(&context, &phaseQuery.header) == FFX_API_RETURN_OK, "jitter phase query should succeed");
    require(phaseCount == 32, "2x upscale should use the expected native jitter phase count");

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    ffxQueryDescUpscaleGetJitterOffset jitterQuery{};
    jitterQuery.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jitterQuery.index = 0;
    jitterQuery.phaseCount = phaseCount;
    jitterQuery.pOutX = &jitterX;
    jitterQuery.pOutY = &jitterY;
    require(ffxQuery(&context, &jitterQuery.header) == FFX_API_RETURN_OK, "jitter offset query should succeed");
    require(jitterX == 0.0f, "first Halton x jitter should match APUSR native common primitive");
    require(jitterY < 0.0f, "first Halton y jitter should match APUSR native common primitive");

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = reinterpret_cast<void*>(0x2000);
    dispatch.color = makeResource(reinterpret_cast<void*>(0x3000), FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, 1920, 1080);
    dispatch.depth = makeResource(reinterpret_cast<void*>(0x4000), FFX_API_SURFACE_FORMAT_R32_FLOAT, 1920, 1080);
    dispatch.motionVectors = makeResource(reinterpret_cast<void*>(0x5000), FFX_API_SURFACE_FORMAT_R16G16_FLOAT, 1920, 1080);
    dispatch.output = makeResource(reinterpret_cast<void*>(0x6000), FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, 3840, 2160);
    dispatch.renderSize = FfxApiDimensions2D{.width = 1920, .height = 1080};
    dispatch.upscaleSize = FfxApiDimensions2D{.width = 3840, .height = 2160};
    dispatch.jitterOffset = FfxApiFloatCoords2D{.x = jitterX, .y = jitterY};
    dispatch.cameraNear = 0.1f;
    dispatch.cameraFar = 1000.0f;
    dispatch.cameraFovAngleVertical = 1.0f;

    // Keep this test on the exported ABI surface. Descriptor normalization is
    // covered by apusr_fsr4_normalization_smoke so Windows import libraries
    // do not need to expose APUSR-internal C++ helper symbols.

    require(ffxDispatch(&context, &dispatch.header) == FFX_API_RETURN_ERROR_RUNTIME_ERROR, "FSR4 dispatch should fail when no real route is configured");

    ApusrFsr4Dx12FrontendTelemetry telemetry{};
    require(apusrFsr4Dx12FrontendGetTelemetry(&context, &telemetry), "frontend telemetry should be queryable before context destruction");
    require(telemetry.totalDispatches == 1, "frontend telemetry should count attempted dispatches");
    require(telemetry.nativeApusrDispatches == 0, "frontend telemetry should not count native dispatch without fast-path configure");
    require(telemetry.failedDispatches == 1, "frontend telemetry should count failed real-route dispatches");
    require(ffxDestroyContext(&context, nullptr) == FFX_API_RETURN_OK, "FSR4 ABI context destruction should succeed");
    require(context == nullptr, "destroy should clear the public context handle");

    std::cout << "APUSR FSR4 ABI smoke: SDK 2.3 version/debug compatibility, provider enumeration, FFX ingress, loader-compatible context header, and hard-fail telemetry validated\n";
    return 0;
}
