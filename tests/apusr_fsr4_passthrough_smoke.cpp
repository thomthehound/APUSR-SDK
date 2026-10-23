// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_fsr4_dx12_frontend.h>

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

} // namespace

int main()
{
    std::uint64_t providerCount = 0;
    ffxQueryDescGetVersions count{};
    count.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    count.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    count.outputCount = &providerCount;
    require(ffxQuery(nullptr, &count.header) == FFX_API_RETURN_OK,
            "controlled FrameGen passthrough provider should answer version-count queries");
    require(providerCount == 1, "controlled FrameGen passthrough provider should expose one provider");

    std::uint64_t providerId = 0;
    const char* providerName = nullptr;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versions.outputCount = &providerCount;
    versions.versionIds = &providerId;
    versions.versionNames = &providerName;
    require(ffxQuery(nullptr, &versions.header) == FFX_API_RETURN_OK,
            "controlled FrameGen passthrough provider should enumerate");
    require(providerCount == 1 && providerId != 0 && providerName != nullptr,
            "controlled FrameGen passthrough enumeration should return concrete identity");

    ffxContext context = nullptr;
    ffxCreateContextDescHeader create{};
    create.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    require(ffxCreateContext(&context, &create, nullptr) == FFX_API_RETURN_OK,
            "FrameGen context creation should delegate to the controlled passthrough provider");
    require(context != nullptr, "controlled passthrough provider should return a context");

    ffxQueryGetProviderVersion providerVersion{};
    providerVersion.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    require(ffxQuery(&context, &providerVersion.header) == FFX_API_RETURN_OK,
            "provider-version query on a passthrough context should delegate");
    require(providerVersion.versionId == providerId && providerVersion.versionName != nullptr,
            "passthrough context should preserve provider identity");

    require(ffxDestroyContext(&context, nullptr) == FFX_API_RETURN_OK,
            "FrameGen context destruction should delegate to the controlled passthrough provider");
    require(context == nullptr, "passthrough destroy should clear the context");

    std::cout << "APUSR FSR4 passthrough smoke: controlled FrameGen provider query/create/destroy delegation validated\n";
    return 0;
}
