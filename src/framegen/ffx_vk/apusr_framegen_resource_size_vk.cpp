// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "../apusr_framegen_backend.h"

#include <algorithm>
#include <cstdint>

namespace {

std::uint32_t bytesPerPixel(const ApusrFramegenSurfaceFormat format) noexcept
{
    switch (format) {
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_FLOAT: return 16;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32_FLOAT: return 12;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_UINT: return 8;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SRGB:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_SRGB:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R11G11B10_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP: return 4;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_FLOAT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UINT:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R16_SNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UINT: return 2;
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_TYPELESS:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_SNORM:
    case APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT: return 1;
    default: return 0;
    }
}

std::uint32_t resolvedMipCount(const ApusrFramegenResourceDescription& description) noexcept
{
    if (description.mipCount)
        return description.mipCount;
    std::uint32_t dimension = std::max(description.width, description.height);
    std::uint32_t count = 1;
    while (dimension > 1) {
        dimension >>= 1;
        ++count;
    }
    return count;
}

} // namespace

namespace apusr::framegen {

ApusrFramegenResult resourceAllocationSize(
    ApusrFramegenDevice,
    const ApusrFramegenCreateResourceDescription& description,
    std::uint64_t& sizeInBytes) noexcept
{
    const ApusrFramegenResourceDescription& resource = description.resourceDescription;
    if (resource.type == APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER) {
        if (!resource.size)
            return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
        sizeInBytes = resource.size;
        return APUSR_FRAMEGEN_OK;
    }
    if (resource.type != APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D || !resource.width || !resource.height)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    const std::uint32_t bpp = bytesPerPixel(resource.format);
    if (!bpp)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    std::uint64_t total = 0;
    std::uint32_t width = resource.width;
    std::uint32_t height = resource.height;
    for (std::uint32_t mip = 0; mip < resolvedMipCount(resource); ++mip) {
        total += static_cast<std::uint64_t>(width) * height * bpp;
        width = std::max(1u, width >> 1);
        height = std::max(1u, height >> 1);
    }
    sizeInBytes = total;
    return APUSR_FRAMEGEN_OK;
}

} // namespace apusr::framegen
