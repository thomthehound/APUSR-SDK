// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/compat/apusr_ffx_api_abi.h>

#include <cstdint>

namespace {

constexpr std::uint64_t kFramegenProviderId = 0x4150555354464755ull;
constexpr char kFramegenProviderName[] = "APUSR controlled test FrameGen provider";

struct FakeContext {
    const void* providerMarker = nullptr;
    std::uint32_t magic = 0;
};

FakeContext gContext{};

} // namespace

extern "C" {

FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(
    ffxContext* context,
    ffxCreateContextDescHeader* desc,
    const ffxAllocationCallbacks*)
{
    if (context == nullptr || desc == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->type != FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION) {
        return FFX_API_RETURN_NO_PROVIDER;
    }
    *context = &gContext;
    return FFX_API_RETURN_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*)
{
    if (context == nullptr || *context != &gContext) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext*, const ffxConfigureDescHeader*)
{
    return FFX_API_RETURN_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    if (desc == nullptr) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
        auto* query = reinterpret_cast<ffxQueryDescGetVersions*>(desc);
        if (query->createDescType != FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION || query->outputCount == nullptr) {
            return FFX_API_RETURN_NO_PROVIDER;
        }
        const std::uint64_t capacity = *query->outputCount;
        *query->outputCount = 1;
        if (capacity != 0) {
            if (query->versionIds != nullptr) {
                query->versionIds[0] = kFramegenProviderId;
            }
            if (query->versionNames != nullptr) {
                query->versionNames[0] = kFramegenProviderName;
            }
        }
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION && context != nullptr && *context == &gContext) {
        auto* query = reinterpret_cast<ffxQueryGetProviderVersion*>(desc);
        query->versionId = kFramegenProviderId;
        query->versionName = kFramegenProviderName;
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext*, const ffxDispatchDescHeader*)
{
    return FFX_API_RETURN_OK;
}

} // extern "C"
