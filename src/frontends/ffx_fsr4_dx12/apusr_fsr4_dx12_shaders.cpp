// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include <apusr/frontends/apusr_fsr4_dx12_shaders.h>
#include <apusr/adapters/apusr_fsr4_boundary.h>

#include <apusr/compat/apusr_ffx_api_abi.h>

#include "apusr_fsr4_dx12_shader_blobs.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
#endif

namespace apusr::frontends {

namespace {

#ifdef _WIN32

Status hresultStatus(const char* operation, const HRESULT hr, ID3DBlob* diagnosticBlob = nullptr)
{
    std::ostringstream message;
    message << operation << " failed with HRESULT 0x" << std::hex << static_cast<unsigned long>(hr);
    if (diagnosticBlob != nullptr && diagnosticBlob->GetBufferPointer() != nullptr && diagnosticBlob->GetBufferSize() > 0) {
        message << ": " << static_cast<const char*>(diagnosticBlob->GetBufferPointer());
    }
    return Status::runtimeFailure(message.str());
}

ID3D12Device* dx12Device(const NativeHandle handle)
{
    return static_cast<ID3D12Device*>(handle.value);
}

ID3D12GraphicsCommandList* dx12CommandList(const backends::Dx12RecordContext& context)
{
    return static_cast<ID3D12GraphicsCommandList*>(context.commandList.value);
}

ID3D12Resource* dx12Resource(const NativeHandle handle)
{
    return static_cast<ID3D12Resource*>(handle.value);
}

constexpr bool kDx12TraceEnabled =
#if defined(APUSR_ENABLE_DX12_TRACE) && APUSR_ENABLE_DX12_TRACE
    true;
#else
    false;
#endif

constexpr bool kDebugHalf4Sidecars =
#if defined(APUSR_ENABLE_DEBUG_HALF4_SIDECARS) && APUSR_ENABLE_DEBUG_HALF4_SIDECARS
    true;
#else
    false;
#endif

std::string pointerString(const void* pointer)
{
    std::ostringstream message;
    message << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(pointer);
    return message.str();
}

const char* dxgiFormatName(const DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_UNKNOWN:
        return "UNKNOWN";
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_UINT:
        return "R32G32B32A32_UINT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R32G32B32_FLOAT:
        return "R32G32B32_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT:
        return "R32G32_FLOAT";
    case DXGI_FORMAT_R32G32_TYPELESS:
        return "R32G32_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT:
        return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_TYPELESS:
        return "R16G16_TYPELESS";
    case DXGI_FORMAT_R16G16_UINT:
        return "R16G16_UINT";
    case DXGI_FORMAT_R16G16_SINT:
        return "R16G16_SINT";
    case DXGI_FORMAT_R8G8_UNORM:
        return "R8G8_UNORM";
    case DXGI_FORMAT_R8G8_TYPELESS:
        return "R8G8_TYPELESS";
    case DXGI_FORMAT_R8G8_UINT:
        return "R8G8_UINT";
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return "R8G8B8A8_SNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT:
        return "R16_FLOAT";
    case DXGI_FORMAT_R16_TYPELESS:
        return "R16_TYPELESS";
    case DXGI_FORMAT_R16_UINT:
        return "R16_UINT";
    case DXGI_FORMAT_R16_UNORM:
        return "R16_UNORM";
    case DXGI_FORMAT_R16_SNORM:
        return "R16_SNORM";
    case DXGI_FORMAT_R32_FLOAT:
        return "R32_FLOAT";
    case DXGI_FORMAT_R32_UINT:
        return "R32_UINT";
    case DXGI_FORMAT_R32_TYPELESS:
        return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT:
        return "D32_FLOAT";
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R24G8_TYPELESS:
        return "R24G8_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_D16_UNORM:
        return "D16_UNORM";
    case DXGI_FORMAT_R8_UNORM:
        return "R8_UNORM";
    case DXGI_FORMAT_R8_TYPELESS:
        return "R8_TYPELESS";
    case DXGI_FORMAT_R8_UINT:
        return "R8_UINT";
    case DXGI_FORMAT_R8_SINT:
        return "R8_SINT";
    default:
        return "OTHER";
    }
}

const char* dimensionName(const D3D12_RESOURCE_DIMENSION dimension)
{
    switch (dimension) {
    case D3D12_RESOURCE_DIMENSION_BUFFER:
        return "BUFFER";
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        return "TEXTURE1D";
    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        return "TEXTURE2D";
    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        return "TEXTURE3D";
    default:
        return "UNKNOWN";
    }
}

std::string dx12TraceDefaultDirectory()
{
    char explicitDir[MAX_PATH]{};
    const DWORD explicitSize = GetEnvironmentVariableA("APUSR_LOG_DIR", explicitDir, static_cast<DWORD>(sizeof(explicitDir)));
    if (explicitSize > 0 && explicitSize < sizeof(explicitDir)) {
        return explicitDir;
    }
    return "C:/NGXLogs/APUSR";
}

std::ofstream& dx12TraceFile()
{
    static std::ofstream file = []() {
        std::string path;
        char explicitPath[MAX_PATH]{};
        const DWORD explicitSize = GetEnvironmentVariableA("APUSR_DX12_TRACE_FILE", explicitPath, static_cast<DWORD>(sizeof(explicitPath)));
        if (explicitSize > 0 && explicitSize < sizeof(explicitPath)) {
            path = explicitPath;
        } else {
            path = dx12TraceDefaultDirectory() + "/apusr_dx12_trace.log";
        }

        std::error_code ec;
        const std::filesystem::path fsPath(path);
        if (fsPath.has_parent_path()) {
            std::filesystem::create_directories(fsPath.parent_path(), ec);
        }
        std::ofstream opened(path, std::ios::out | std::ios::trunc);
        if (opened) {
            opened << "APUSR DX12 trace started, pid=" << GetCurrentProcessId() << " path=" << path << '\n';
            opened.flush();
        }
        return opened;
    }();
    return file;
}

std::mutex& dx12TraceMutex()
{
    static std::mutex mutex;
    return mutex;
}

void writeTraceLine(const std::string& line)
{
    std::lock_guard<std::mutex> lock(dx12TraceMutex());
    std::ofstream& file = dx12TraceFile();
    if (!file) {
        return;
    }
    file << line << '\n';
    file.flush();
}

#define traceLine(lineExpression) \
    do { \
        if constexpr (kDx12TraceEnabled) { \
            writeTraceLine((lineExpression)); \
        } \
    } while (false)

std::string resourceDescString(ID3D12Resource* resource)
{
    if (resource == nullptr) {
        return "null";
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    std::ostringstream message;
    message << pointerString(resource)
            << " dim=" << dimensionName(desc.Dimension)
            << " format=" << dxgiFormatName(desc.Format) << "(" << static_cast<unsigned>(desc.Format) << ")"
            << " size=" << desc.Width << "x" << desc.Height
            << " mips=" << desc.MipLevels
            << " flags=0x" << std::hex << static_cast<unsigned>(desc.Flags);
    return message.str();
}

std::string formatViewString(const char* role, ID3D12Resource* resource, const SurfaceFormat ffxFormat, const DXGI_FORMAT viewFormat)
{
    std::ostringstream message;
    message << role << " resource=" << resourceDescString(resource)
            << " ffxFormat=" << static_cast<std::uint32_t>(ffxFormat)
            << " viewFormat=" << dxgiFormatName(viewFormat) << "(" << static_cast<unsigned>(viewFormat) << ")";
    return message.str();
}

std::string stateString(const std::uint64_t ffxState, const D3D12_RESOURCE_STATES dx12State)
{
    std::ostringstream message;
    message << "ffx=0x" << std::hex << ffxState << " dx12=0x" << static_cast<unsigned>(dx12State);
    return message.str();
}

Status traceInvalidArgument(const std::string& message)
{
    traceLine("ERROR: " + message);
    return Status::invalidArgument(message);
}

DXGI_FORMAT dxgiFormat(const SurfaceFormat format)
{
    switch (format) {
    case SurfaceFormat::Rgba32Float:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case SurfaceFormat::Rgba32Uint:
        return DXGI_FORMAT_R32G32B32A32_UINT;
    case SurfaceFormat::Rgba16Float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case SurfaceFormat::Rgb32Float:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case SurfaceFormat::Rg32Float:
        return DXGI_FORMAT_R32G32_FLOAT;
    case SurfaceFormat::Rg32Uint:
        return DXGI_FORMAT_R32G32_UINT;
    case SurfaceFormat::Rg16Float:
        return DXGI_FORMAT_R16G16_FLOAT;
    case SurfaceFormat::Rg16Uint:
        return DXGI_FORMAT_R16G16_UINT;
    case SurfaceFormat::Rg16Sint:
        return DXGI_FORMAT_R16G16_SINT;
    case SurfaceFormat::Rg8Unorm:
        return DXGI_FORMAT_R8G8_UNORM;
    case SurfaceFormat::Rg8Uint:
        return DXGI_FORMAT_R8G8_UINT;
    case SurfaceFormat::Rgba8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case SurfaceFormat::Rgba8Snorm:
        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case SurfaceFormat::Rgba8Srgb:
        return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case SurfaceFormat::Bgra8Unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case SurfaceFormat::Bgra8Srgb:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case SurfaceFormat::R11G11B10Float:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case SurfaceFormat::R10G10B10A2Unorm:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case SurfaceFormat::R16Float:
        return DXGI_FORMAT_R16_FLOAT;
    case SurfaceFormat::R16Uint:
        return DXGI_FORMAT_R16_UINT;
    case SurfaceFormat::R16Unorm:
        return DXGI_FORMAT_R16_UNORM;
    case SurfaceFormat::R16Snorm:
        return DXGI_FORMAT_R16_SNORM;
    case SurfaceFormat::R32Float:
        return DXGI_FORMAT_R32_FLOAT;
    case SurfaceFormat::R32Uint:
        return DXGI_FORMAT_R32_UINT;
    case SurfaceFormat::R8Unorm:
        return DXGI_FORMAT_R8_UNORM;
    case SurfaceFormat::R8Uint:
        return DXGI_FORMAT_R8_UINT;
    case SurfaceFormat::R8Sint:
        return DXGI_FORMAT_R8_SINT;
    case SurfaceFormat::R8Snorm:
        return DXGI_FORMAT_R8_SNORM;
    case SurfaceFormat::Unknown:
        return DXGI_FORMAT_UNKNOWN;
    }
    return DXGI_FORMAT_UNKNOWN;
}

std::uint32_t dispatchGroupCount(const std::uint32_t size, const std::uint32_t groupSize)
{
    return (size + groupSize - 1) / groupSize;
}

DXGI_FORMAT srvFormatFromResourceFormat(const DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;
    default:
        return format;
    }
}

DXGI_FORMAT uavFormatFromResourceFormat(const DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;
    default:
        return format;
    }
}

DXGI_FORMAT patchedFormatFromResourceAndFfx(ID3D12Resource* resource, const SurfaceFormat format)
{
    const DXGI_FORMAT ffxFormat = dxgiFormat(format);
    if (resource == nullptr) {
        return ffxFormat;
    }

    const DXGI_FORMAT resourceFormat = resource->GetDesc().Format;
    switch (resourceFormat) {
    case DXGI_FORMAT_UNKNOWN:
        return ffxFormat;

    // Match the FFX DX12 backend: keep the application resource format as the
    // authority, but let the FFX resource description select the SRGB view for
    // RGBA8 resources. Blindly trusting the FFX description can create an SRV
    // whose format is not compatible with the actual ID3D12Resource.
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return ffxFormat != DXGI_FORMAT_UNKNOWN ? ffxFormat : resourceFormat;

    // Depth resources need a shader-readable color-compatible view format.
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;

    default:
        return resourceFormat;
    }
}

DXGI_FORMAT viewFormat(ID3D12Resource* resource, const SurfaceFormat format, DXGI_FORMAT (*convert)(DXGI_FORMAT))
{
    return convert(patchedFormatFromResourceAndFfx(resource, format));
}

bool resourceFormatSupportsSrv(ID3D12Resource* resource, const DXGI_FORMAT view) noexcept
{
    if (resource == nullptr || view == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = view;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
        return false;
    }
    return (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
}

bool resourceFormatSupportsUav(ID3D12Resource* resource, const DXGI_FORMAT view) noexcept
{
    if (resource == nullptr || view == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    ComPtr<ID3D12Device> device;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&device))) || !device) {
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = view;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
        return false;
    }
    return (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

Status validateTextureSrv(const char* role, ID3D12Resource* resource, const SurfaceFormat format)
{
    if (resource == nullptr) {
        return traceInvalidArgument(std::string(role) + " SRV resource is null");
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const DXGI_FORMAT view = viewFormat(resource, format, srvFormatFromResourceFormat);
    traceLine(std::string("SRV ") + formatViewString(role, resource, format, view));
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return traceInvalidArgument(std::string(role) + " SRV resource is not a 2D texture: " + resourceDescString(resource));
    }
    if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        return traceInvalidArgument(std::string(role) + " resource denies shader-resource views: " + resourceDescString(resource));
    }
    if (view == DXGI_FORMAT_UNKNOWN) {
        return traceInvalidArgument(std::string(role) + " SRV view format is UNKNOWN: " + resourceDescString(resource));
    }
    if (!resourceFormatSupportsSrv(resource, view)) {
        return traceInvalidArgument(std::string(role) + " SRV view format is not shader-load supported: " + formatViewString(role, resource, format, view));
    }
    return Status::ok();
}

Status validateTextureUav(const char* role, ID3D12Resource* resource, const SurfaceFormat format)
{
    if (resource == nullptr) {
        return traceInvalidArgument(std::string(role) + " UAV resource is null");
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    const DXGI_FORMAT view = viewFormat(resource, format, uavFormatFromResourceFormat);
    traceLine(std::string("UAV ") + formatViewString(role, resource, format, view));
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return traceInvalidArgument(std::string(role) + " UAV resource is not a 2D texture: " + resourceDescString(resource));
    }
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return traceInvalidArgument(std::string(role) + " resource does not allow unordered-access views: " + resourceDescString(resource));
    }
    if (view == DXGI_FORMAT_UNKNOWN) {
        return traceInvalidArgument(std::string(role) + " UAV view format is UNKNOWN: " + resourceDescString(resource));
    }
    if (!resourceFormatSupportsUav(resource, view)) {
        return traceInvalidArgument(std::string(role) + " UAV view format is not typed-store supported: " + formatViewString(role, resource, format, view));
    }
    return Status::ok();
}

Status validateRootUavBuffer(const char* role, ID3D12Resource* resource)
{
    if (resource == nullptr) {
        return traceInvalidArgument(std::string(role) + " root UAV buffer is null");
    }

    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    traceLine(std::string("ROOT_UAV ") + role + " resource=" + resourceDescString(resource));
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        return traceInvalidArgument(std::string(role) + " root UAV is not a buffer resource: " + resourceDescString(resource));
    }
    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return traceInvalidArgument(std::string(role) + " buffer does not allow unordered access: " + resourceDescString(resource));
    }
    return Status::ok();
}

void traceTransition(const char* role, ID3D12Resource* resource, const D3D12_RESOURCE_STATES before, const D3D12_RESOURCE_STATES after)
{
    if constexpr (!kDx12TraceEnabled) {
        (void)role;
        (void)resource;
        (void)before;
        (void)after;
        return;
    }

    std::ostringstream message;
    message << "BARRIER " << role
            << " before=0x" << std::hex << static_cast<unsigned>(before)
            << " after=0x" << static_cast<unsigned>(after)
            << " resource=" << resourceDescString(resource);
    traceLine(message.str());
}

D3D12_RESOURCE_STATES dx12StateFromFfxState(const std::uint64_t state)
{
    // SurfaceDesc::nativeState is normalized to the native D3D12 state at the FFX boundary.
    // Do not remap it through the FidelityFX API-neutral bit values a second time.
    if (state == 0ull || state == 0xFFFFFFFFull || state == UINT64_MAX) {
        return D3D12_RESOURCE_STATE_COMMON;
    }
    return static_cast<D3D12_RESOURCE_STATES>(state);
}
void appendTransition(
    std::vector<D3D12_RESOURCE_BARRIER>& barriers,
    ID3D12Resource* resource,
    const D3D12_RESOURCE_STATES before,
    const D3D12_RESOURCE_STATES after)
{
    if (resource == nullptr || before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barriers.push_back(barrier);
}

D3D12_RESOURCE_STATES tokenBufferState(
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES>& states,
    ID3D12Resource* resource)
{
    const auto found = states.find(resource);
    return found == states.end() ? D3D12_RESOURCE_STATE_COMMON : found->second;
}

Status validateByteAddressBufferRange(
    const char* label,
    const std::uint64_t bufferBytes,
    const std::uint64_t baseByteOffset,
    const std::uint64_t byteCount)
{
    if (byteCount == 0) {
        return Status::invalidArgument(std::string(label) + " requires a non-empty token byte range");
    }
    if (baseByteOffset > bufferBytes || byteCount > bufferBytes - baseByteOffset) {
        std::ostringstream message;
        message << label << " byte range exceeds the token buffer: offset=" << baseByteOffset
                << " bytes=" << byteCount << " buffer=" << bufferBytes;
        return Status::invalidArgument(message.str());
    }

    constexpr std::uint64_t kShaderAddressSpaceBytes =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1ull;
    if (baseByteOffset >= kShaderAddressSpaceBytes || byteCount > kShaderAddressSpaceBytes - baseByteOffset) {
        std::ostringstream message;
        message << label
                << " exceeds the DX12 ByteAddressBuffer 32-bit offset range: offset="
                << baseByteOffset << " bytes=" << byteCount;
        return Status::unsupportedBackend(message.str());
    }

    return Status::ok();
}

std::uint64_t tokenRowsByteCount(const TokenBoundary& boundary, const std::uint32_t rowCount)
{
    if (rowCount == boundary.shape.height) {
        return apusr::tokenSlabBytes(boundary.shape);
    }
    return apusr::tokenRowPitchBytes(boundary.shape) * static_cast<std::uint64_t>(rowCount);
}

struct TokenBandConstants {
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
    std::uint32_t tokenLogicalWidth = 0;
    std::uint32_t tokenPaddedWidth = 0;
    std::uint32_t tokenHeight = 0;
    std::uint32_t tokenRowPitchBytes = 0;
    std::uint32_t bandIndex = 0;
    std::uint32_t bandCount = 0;
    std::uint32_t inputFirstRow = 0;
    std::uint32_t inputRowCount = 0;
    std::uint32_t outputFirstRow = 0;
    std::uint32_t outputRowCount = 0;
    std::uint32_t tokenByteOffsetLow = 0;
    std::uint32_t tokenByteOffsetHigh = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float previousJitterX = 0.0f;
    float previousJitterY = 0.0f;
    float motionVectorScaleX = 1.0f;
    float motionVectorScaleY = 1.0f;
    float renderToDisplayScaleX = 1.0f;
    float renderToDisplayScaleY = 1.0f;
    float displayToRenderScaleX = 1.0f;
    float displayToRenderScaleY = 1.0f;
    float motionVectorJitterCancellationX = 0.0f;
    float motionVectorJitterCancellationY = 0.0f;
    float preExposure = 1.0f;
    float sharpness = 0.0f;
    float frameTimeDeltaMs = 0.0f;
    float viewSpaceToMetersFactor = 1.0f;
    std::uint32_t featureFlags = 0;
    std::uint32_t resetHistory = 0;
    std::uint32_t sharpeningRequested = 0;
    std::uint32_t hasExposure = 0;
    std::uint32_t modelPreset = 0;
    std::uint32_t egressFirstRow = 0;
    std::uint32_t egressRowCount = 0;
    std::uint32_t hasReactiveMask = 0;
    std::uint32_t hasTransparencyMask = 0;
    std::uint32_t tokenLayout = 0;
    std::uint32_t tokenTileColumns = 0;
    std::uint32_t tokenTileRows = 0;
    std::uint32_t tokenLaneGroups = 1;
};

struct AutoExposureConstants {
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t tileColumns = 0;
    std::uint32_t tileCount = 0;
    float preExposure = 1.0f;
    std::uint32_t featureFlags = 0;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};

struct RoundTripCopyVerifyConstants {
    std::uint32_t expectedByteOffsetLow = 0;
    std::uint32_t expectedByteOffsetHigh = 0;
    std::uint32_t actualByteOffsetLow = 0;
    std::uint32_t actualByteOffsetHigh = 0;
    std::uint32_t byteSizeLow = 0;
    std::uint32_t byteSizeHigh = 0;
    std::uint32_t wordCount = 0;
    std::uint32_t pad0 = 0;
};


std::uint32_t packFeatureFlags(const UpscalerFeatureFlags& flags)
{
    return (flags.highDynamicRange ? 1u : 0u) |
        (flags.displayResolutionMotionVectors ? (1u << 1u) : 0u) |
        (flags.motionVectorJitterCancellation ? (1u << 2u) : 0u) |
        (flags.depthInverted ? (1u << 3u) : 0u) |
        (flags.depthInfinite ? (1u << 4u) : 0u) |
        (flags.autoExposure ? (1u << 5u) : 0u) |
        (flags.dynamicResolution ? (1u << 6u) : 0u) |
        (flags.nonLinearColorSpace ? (1u << 8u) : 0u) |
        (flags.colorSpaceSrgb ? (1u << 9u) : 0u) |
        (flags.colorSpacePq ? (1u << 10u) : 0u);
}
std::uint32_t shaderTokenLayout(const TokenLayout layout)
{
    if (layout == TokenLayout::ZeroGuardedRasterNhwcC16) {
        return 4u;
    }
    if (layout == TokenLayout::CellM4x8C16) {
        return 5u;
    }
    if (layout == TokenLayout::LaneMajorCellC16) {
        return 3u;
    }
    if (layout == TokenLayout::LaneMajorPackageC16) {
        return 2u;
    }
    if (layout == TokenLayout::PackageMajorC16) {
        return 1u;
    }
    return 0u;
}

std::uint32_t shaderTokenTileColumns(const TokenShape& shape)
{
    return tokenTransportTileColumnCount(shape);
}

std::uint32_t shaderTokenTileRows(const TokenShape& shape)
{
    return tokenTransportTileRowCount(shape);
}

std::uint32_t shaderTokenLaneGroups(const TokenShape& shape)
{
    return tokenTransportLaneGroupCount(shape);
}


void fillFrameConstants(TokenBandConstants& constants, const UpscalerFrameConstants& frame, const UpscalerFrameOptions& options)
{
    constants.jitterX = frame.jitter.x;
    constants.jitterY = frame.jitter.y;
    constants.previousJitterX = frame.previousJitter.x;
    constants.previousJitterY = frame.previousJitter.y;
    const float motionVectorWidth = static_cast<float>(frame.motionVectorSize.width == 0 ? 1u : frame.motionVectorSize.width);
    const float motionVectorHeight = static_cast<float>(frame.motionVectorSize.height == 0 ? 1u : frame.motionVectorSize.height);
    constants.motionVectorScaleX = options.motionVectorScale.x / motionVectorWidth;
    constants.motionVectorScaleY = options.motionVectorScale.y / motionVectorHeight;
    constants.renderToDisplayScaleX = frame.renderToDisplayScaleX;
    constants.renderToDisplayScaleY = frame.renderToDisplayScaleY;
    constants.displayToRenderScaleX = frame.displayToRenderScaleX;
    constants.displayToRenderScaleY = frame.displayToRenderScaleY;
    constants.motionVectorJitterCancellationX = frame.motionVectorJitterCancellation.x;
    constants.motionVectorJitterCancellationY = frame.motionVectorJitterCancellation.y;
    constants.preExposure = options.preExposure;
    {
        const float clampedSharpness = std::clamp(options.sharpness, 0.0f, 1.0f);
        const float sharpnessStops = (-2.0f * clampedSharpness) + 2.0f;
        constants.sharpness = std::exp2(-sharpnessStops);
    }
    constants.frameTimeDeltaMs = options.frameTimeDeltaMs;
    constants.viewSpaceToMetersFactor = options.viewSpaceToMetersFactor;
    constants.featureFlags = packFeatureFlags(options.features);
    constants.resetHistory = options.resetHistory ? 1u : 0u;
    constants.sharpeningRequested = options.sharpeningRequested ? 1u : 0u;
    constants.hasExposure = options.hasExposure ? 1u : 0u;
    constants.hasReactiveMask = options.hasReactiveMask ? 1u : 0u;
    constants.hasTransparencyMask = options.hasTransparencyMask ? 1u : 0u;
    constants.modelPreset = static_cast<std::uint32_t>(adapters::selectFsr4ModelPreset(
        frame.renderSize,
        frame.displaySize,
        options.features));
}

TokenBandConstants makeConstants(const ExportPassDesc& desc)
{
    TokenBandConstants constants{
        .renderWidth = desc.frameConstants.renderSize.width,
        .renderHeight = desc.frameConstants.renderSize.height,
        .displayWidth = desc.frameConstants.displaySize.width,
        .displayHeight = desc.frameConstants.displaySize.height,
        .tokenLogicalWidth = desc.destinationBoundary.shape.width,
        .tokenPaddedWidth = tokenPaddedRowWidth(desc.destinationBoundary.shape),
        .tokenHeight = desc.destinationBoundary.shape.height,
        .tokenRowPitchBytes = static_cast<std::uint32_t>(apusr::tokenRowPitchBytes(desc.destinationBoundary.shape)),
        .bandIndex = desc.band.bandIndex,
        .bandCount = desc.band.bandCount,
        .inputFirstRow = desc.band.inputFirstRow,
        .inputRowCount = desc.band.inputRowCount,
        .outputFirstRow = desc.band.outputFirstRow,
        .outputRowCount = desc.band.outputRowCount,
        .tokenByteOffsetLow = static_cast<std::uint32_t>(desc.destinationByteOffset & 0xffffffffull),
        .tokenByteOffsetHigh = static_cast<std::uint32_t>(desc.destinationByteOffset >> 32),
    };
    fillFrameConstants(constants, desc.frameConstants, desc.frameOptions);
    constants.tokenLayout = shaderTokenLayout(desc.destinationBoundary.shape.layout);
    constants.tokenTileColumns = shaderTokenTileColumns(desc.destinationBoundary.shape);
    constants.tokenTileRows = shaderTokenTileRows(desc.destinationBoundary.shape);
    constants.tokenLaneGroups = shaderTokenLaneGroups(desc.destinationBoundary.shape);
    constants.egressFirstRow = desc.band.egressFirstRow;
    constants.egressRowCount = desc.band.egressRowCount;
    return constants;
}

TokenBandConstants makeConstants(const ImportPassDesc& desc)
{
    TokenBandConstants constants{
        .renderWidth = desc.frameConstants.renderSize.width,
        .renderHeight = desc.frameConstants.renderSize.height,
        .displayWidth = desc.frameConstants.displaySize.width,
        .displayHeight = desc.frameConstants.displaySize.height,
        .tokenLogicalWidth = desc.sourceBoundary.shape.width,
        .tokenPaddedWidth = tokenPaddedRowWidth(desc.sourceBoundary.shape),
        .tokenHeight = desc.sourceBoundary.shape.height,
        .tokenRowPitchBytes = static_cast<std::uint32_t>(apusr::tokenRowPitchBytes(desc.sourceBoundary.shape)),
        .bandIndex = desc.band.bandIndex,
        .bandCount = desc.band.bandCount,
        .inputFirstRow = desc.band.inputFirstRow,
        .inputRowCount = desc.band.inputRowCount,
        .outputFirstRow = desc.band.outputFirstRow,
        .outputRowCount = desc.band.outputRowCount,
        .tokenByteOffsetLow = static_cast<std::uint32_t>(desc.sourceByteOffset & 0xffffffffull),
        .tokenByteOffsetHigh = static_cast<std::uint32_t>(desc.sourceByteOffset >> 32),
    };
    fillFrameConstants(constants, desc.frameConstants, desc.frameOptions);
    constants.tokenLayout = shaderTokenLayout(desc.sourceBoundary.shape.layout);
    constants.tokenTileColumns = shaderTokenTileColumns(desc.sourceBoundary.shape);
    constants.tokenTileRows = shaderTokenTileRows(desc.sourceBoundary.shape);
    constants.tokenLaneGroups = shaderTokenLaneGroups(desc.sourceBoundary.shape);
    constants.egressFirstRow = desc.band.egressFirstRow;
    constants.egressRowCount = desc.band.egressRowCount;
    return constants;
}

AutoExposureConstants makeAutoExposureConstants(
    const RenderSize renderSize,
    const UpscalerFrameOptions& options,
    const std::uint32_t tileColumns,
    const std::uint32_t tileCount)
{
    AutoExposureConstants constants{};
    constants.renderWidth = renderSize.width;
    constants.renderHeight = renderSize.height;
    constants.tileColumns = tileColumns;
    constants.tileCount = tileCount;
    constants.preExposure = options.preExposure;
    constants.featureFlags = packFeatureFlags(options.features);
    return constants;
}

Status serializeRootSignature(
    const D3D12_ROOT_SIGNATURE_DESC& desc,
    ComPtr<ID3DBlob>& outSignatureBlob,
    const char* debugName)
{
    ComPtr<ID3DBlob> errorBlob;
    const HRESULT hr = D3D12SerializeRootSignature(
        &desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &outSignatureBlob,
        &errorBlob);
    if (FAILED(hr) || !outSignatureBlob) {
        return hresultStatus(debugName, hr, errorBlob.Get());
    }
    return Status::ok();
}

Status createComputePso(
    ID3D12Device* device,
    ID3D12RootSignature* rootSignature,
    const shader_blobs::ShaderBlob shader,
    ComPtr<ID3D12PipelineState>& outPso,
    const char* debugName)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rootSignature;
    desc.CS.pShaderBytecode = shader.data;
    desc.CS.BytecodeLength = shader.size;

    const HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&outPso));
    if (FAILED(hr) || !outPso) {
        return hresultStatus(debugName, hr);
    }
    return Status::ok();
}

D3D12_SHADER_RESOURCE_VIEW_DESC textureSrvDesc(ID3D12Resource* resource, const SurfaceFormat format)
{
    D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = viewFormat(resource, format, srvFormatFromResourceFormat);
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = resourceDesc.MipLevels == 0 ? 1 : resourceDesc.MipLevels;
    return desc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC textureUavDesc(ID3D12Resource* resource, const SurfaceFormat format)
{
    D3D12_RESOURCE_DESC resourceDesc = resource->GetDesc();
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = viewFormat(resource, format, uavFormatFromResourceFormat);
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.PlaneSlice = 0;
    (void)resourceDesc;
    return desc;
}

struct DescriptorHeapEntry {
    ComPtr<ID3D12DescriptorHeap> heap;
    std::uintptr_t key = 0;
};

Status createInternalRgba16Texture(
    ID3D12Device* device,
    const DisplaySize size,
    const wchar_t* debugName,
    ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment = 0;
    resourceDesc.Width = size.width;
    resourceDesc.Height = size.height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&outResource));
    if (FAILED(hr) || !outResource) {
        return hresultStatus("ID3D12Device::CreateCommittedResource(import history)", hr);
    }
    outResource->SetName(debugName);
    return Status::ok();
}

Status createInternalR32Texture(
    ID3D12Device* device,
    const std::uint32_t width,
    const std::uint32_t height,
    const wchar_t* debugName,
    ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_R32_FLOAT;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&outResource));
    if (FAILED(hr) || !outResource) {
        return hresultStatus("ID3D12Device::CreateCommittedResource(auto exposure texture)", hr);
    }
    outResource->SetName(debugName);
    return Status::ok();
}

Status createInternalR32UintTexture(
    ID3D12Device* device,
    const std::uint32_t width,
    const std::uint32_t height,
    const wchar_t* debugName,
    ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_R32_UINT;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&outResource));
    if (FAILED(hr) || !outResource) {
        return hresultStatus("ID3D12Device::CreateCommittedResource(async sidecar R32_UINT texture)", hr);
    }
    outResource->SetName(debugName);
    return Status::ok();
}

Status createInternalModelSidecarTexture(
    ID3D12Device* device,
    const std::uint32_t width,
    const std::uint32_t height,
    const wchar_t* debugName,
    ComPtr<ID3D12Resource>& outResource)
{
    if (kDebugHalf4Sidecars) {
        return createInternalRgba16Texture(device, DisplaySize{width, height}, debugName, outResource);
    }
    return createInternalR32UintTexture(device, width, height, debugName, outResource);
}

Status createInternalByteAddressBuffer(
    ID3D12Device* device,
    const std::uint64_t byteSize,
    const wchar_t* debugName,
    ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = byteSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    const HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&outResource));
    if (FAILED(hr) || !outResource) {
        return hresultStatus("ID3D12Device::CreateCommittedResource(auto exposure tiles)", hr);
    }
    outResource->SetName(debugName);
    return Status::ok();
}


Status createReadbackBuffer(
    ID3D12Device* device,
    const std::uint64_t byteSize,
    const wchar_t* debugName,
    ComPtr<ID3D12Resource>& outResource)
{
    D3D12_HEAP_PROPERTIES heapProperties{};
    heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 1;
    heapProperties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = byteSize;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    const HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&outResource));
    if (FAILED(hr) || !outResource) {
        return hresultStatus("ID3D12Device::CreateCommittedResource(round-trip verifier readback)", hr);
    }
    outResource->SetName(debugName);
    return Status::ok();
}

D3D12_UNORDERED_ACCESS_VIEW_DESC rawBufferUavDesc(const std::uint64_t byteSize)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = static_cast<UINT>(byteSize / 4u);
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.CounterOffsetInBytes = 0;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    return desc;
}

#endif

} // namespace

namespace {
#ifdef _WIN32
constexpr std::uint32_t kAsyncBookendFrameSlots = 3;

struct AsyncBookendFrameResources {
    ComPtr<ID3D12Resource> modelInputColor;
    ComPtr<ID3D12Resource> reprojectedColor;
    ComPtr<ID3D12Resource> exposureTexture;
    RenderSize renderSize{};
    DisplaySize displaySize{};
};

constexpr std::uint32_t kRoundTripCopyVerifySlots = 8;
constexpr std::uint32_t kRoundTripCopyVerifyResultBytes = 16;
constexpr std::uint32_t kRoundTripCopyVerifyThreads = 256;

struct RoundTripCopyVerifyResources {
    ComPtr<ID3D12Resource> resultBuffer;
    ComPtr<ID3D12Resource> readbackBuffer;
    ComPtr<ID3D12DescriptorHeap> clearUavHeap;
    void* mappedReadback = nullptr;
    std::uint64_t pendingFrameIndex = 0;
    std::uint64_t pendingByteSize = 0;
    ComPtr<ID3D12Fence> completionFence;
    std::uint64_t completionValue = 0;
    bool pending = false;
};
#endif
} // namespace

struct Fsr4Dx12TokenShaderRecorder::Impl {
#ifdef _WIN32
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> autoExposureReduceRootSignature;
    ComPtr<ID3D12RootSignature> autoExposureResolveRootSignature;
    ComPtr<ID3D12RootSignature> captureModelInputRootSignature;
    ComPtr<ID3D12RootSignature> exportRootSignature;
    ComPtr<ID3D12RootSignature> importRootSignature;
    ComPtr<ID3D12RootSignature> roundTripCopyVerifyRootSignature;
    ComPtr<ID3D12RootSignature> rcasRootSignature;
    ComPtr<ID3D12PipelineState> autoExposureReducePso;
    ComPtr<ID3D12PipelineState> autoExposureResolvePso;
    ComPtr<ID3D12PipelineState> captureModelInputPso;
    ComPtr<ID3D12PipelineState> exportPso;
    ComPtr<ID3D12PipelineState> importPso;
    ComPtr<ID3D12PipelineState> roundTripCopyVerifyPso;
    ComPtr<ID3D12PipelineState> rcasPso;
    UINT descriptorIncrement = 0;
    std::unordered_map<std::uintptr_t, DescriptorHeapEntry> autoExposureDescriptorHeaps;
    std::unordered_map<std::uintptr_t, DescriptorHeapEntry> captureDescriptorHeaps;
    std::unordered_map<std::uintptr_t, DescriptorHeapEntry> exportSrvHeaps;
    std::unordered_map<std::uintptr_t, DescriptorHeapEntry> importDescriptorHeaps;
    std::unordered_map<std::uintptr_t, DescriptorHeapEntry> rcasDescriptorHeaps;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> tokenBufferStates;
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> internalTextureStates;
    ComPtr<ID3D12Resource> historyColor[2];
    ComPtr<ID3D12Resource> recurrentColor[2];
    std::array<AsyncBookendFrameResources, kAsyncBookendFrameSlots> asyncFrames;
    std::array<RoundTripCopyVerifyResources, kRoundTripCopyVerifySlots> roundTripCopyVerifySlots;
    ComPtr<ID3D12Resource> rcasInput;
    ComPtr<ID3D12Resource> autoExposureTexture;
    ComPtr<ID3D12Resource> autoExposureTiles;
    RenderSize autoExposureSize{};
    std::uint32_t autoExposureTileColumns = 0;
    std::uint32_t autoExposureTileRows = 0;
    std::uint32_t autoExposureTileCapacity = 0;
    DisplaySize historySize{};
    std::uint32_t historyReadIndex = 0;
    std::uint32_t historyWriteIndex = 1;
    bool historyInitialized = false;
#endif
};


#ifdef _WIN32
template <typename StateMapT>
void forgetInternalState(StateMapT& states, ID3D12Resource* resource)
{
    if (resource != nullptr) {
        states.erase(resource);
    }
}

D3D12_SHADER_RESOURCE_VIEW_DESC r32TextureSrvDesc();
D3D12_UNORDERED_ACCESS_VIEW_DESC r32TextureUavDesc();
D3D12_SHADER_RESOURCE_VIEW_DESC modelSidecarTextureSrvDesc();
D3D12_UNORDERED_ACCESS_VIEW_DESC modelSidecarTextureUavDesc();
SurfaceFormat modelSidecarSurfaceFormat();

template <typename ImplT>
Status ensureInternalHistoryResources(ImplT& impl, const DisplaySize historySize)
{
    if (historySize.width == 0 || historySize.height == 0) {
        return Status::invalidArgument("FSR4 DX12 history resources require non-zero output size");
    }

    const bool recreateHistory = impl.historySize.width != historySize.width ||
        impl.historySize.height != historySize.height ||
        impl.historyColor[0] == nullptr || impl.historyColor[1] == nullptr ||
        impl.recurrentColor[0] == nullptr || impl.recurrentColor[1] == nullptr ||
        impl.rcasInput == nullptr;
    if (!recreateHistory) {
        return Status::ok();
    }

    forgetInternalState(impl.internalTextureStates, impl.historyColor[0].Get());
    forgetInternalState(impl.internalTextureStates, impl.historyColor[1].Get());
    forgetInternalState(impl.internalTextureStates, impl.recurrentColor[0].Get());
    forgetInternalState(impl.internalTextureStates, impl.recurrentColor[1].Get());
    forgetInternalState(impl.internalTextureStates, impl.rcasInput.Get());
    impl.historyColor[0].Reset();
    impl.historyColor[1].Reset();
    impl.recurrentColor[0].Reset();
    impl.recurrentColor[1].Reset();
    impl.rcasInput.Reset();
    impl.exportSrvHeaps.clear();
    impl.importDescriptorHeaps.clear();
    impl.rcasDescriptorHeaps.clear();
    impl.historySize = historySize;
    impl.historyReadIndex = 0;
    impl.historyWriteIndex = 1;
    impl.historyInitialized = false;

    Status status = createInternalRgba16Texture(impl.device.Get(), historySize, L"APUSR FSR4 history color 0", impl.historyColor[0]);
    if (!status.succeeded()) {
        return status;
    }
    status = createInternalRgba16Texture(impl.device.Get(), historySize, L"APUSR FSR4 history color 1", impl.historyColor[1]);
    if (!status.succeeded()) {
        return status;
    }
    status = createInternalRgba16Texture(impl.device.Get(), historySize, L"APUSR FSR4 recurrent 0", impl.recurrentColor[0]);
    if (!status.succeeded()) {
        return status;
    }
    status = createInternalRgba16Texture(impl.device.Get(), historySize, L"APUSR FSR4 recurrent 1", impl.recurrentColor[1]);
    if (!status.succeeded()) {
        return status;
    }
    status = createInternalRgba16Texture(impl.device.Get(), historySize, L"APUSR FSR4 RCAS input", impl.rcasInput);
    if (!status.succeeded()) {
        return status;
    }

    impl.internalTextureStates[impl.historyColor[0].Get()] = D3D12_RESOURCE_STATE_COMMON;
    impl.internalTextureStates[impl.historyColor[1].Get()] = D3D12_RESOURCE_STATE_COMMON;
    impl.internalTextureStates[impl.recurrentColor[0].Get()] = D3D12_RESOURCE_STATE_COMMON;
    impl.internalTextureStates[impl.recurrentColor[1].Get()] = D3D12_RESOURCE_STATE_COMMON;
    impl.internalTextureStates[impl.rcasInput.Get()] = D3D12_RESOURCE_STATE_COMMON;
    return Status::ok();
}



template <typename ImplT>
Status ensureAsyncBookendFrameResources(
    ImplT& impl,
    const std::uint32_t frameSlot,
    const RenderSize renderSize,
    const DisplaySize displaySize)
{
    if (frameSlot >= kAsyncBookendFrameSlots) {
        return Status::invalidArgument("FSR4 DX12 async bookend frame slot is outside the resource ring");
    }
    if (renderSize.width == 0 || renderSize.height == 0 || displaySize.width == 0 || displaySize.height == 0) {
        return Status::invalidArgument("FSR4 DX12 async bookend resources require non-zero render and display sizes");
    }

    AsyncBookendFrameResources& frame = impl.asyncFrames[frameSlot];
    const bool recreate = frame.renderSize.width != renderSize.width ||
        frame.renderSize.height != renderSize.height ||
        frame.displaySize.width != displaySize.width ||
        frame.displaySize.height != displaySize.height ||
        frame.modelInputColor == nullptr ||
        frame.reprojectedColor == nullptr ||
        frame.exposureTexture == nullptr;
    if (!recreate) {
        return Status::ok();
    }

    forgetInternalState(impl.internalTextureStates, frame.modelInputColor.Get());
    forgetInternalState(impl.internalTextureStates, frame.reprojectedColor.Get());
    forgetInternalState(impl.internalTextureStates, frame.exposureTexture.Get());
    frame.modelInputColor.Reset();
    frame.reprojectedColor.Reset();
    frame.exposureTexture.Reset();
    impl.captureDescriptorHeaps.clear();
    impl.exportSrvHeaps.clear();
    impl.importDescriptorHeaps.clear();
    impl.rcasDescriptorHeaps.clear();
    frame.renderSize = renderSize;
    frame.displaySize = displaySize;

    const wchar_t* modelInputName = kDebugHalf4Sidecars ?
        L"APUSR FSR4 async debug half4 model input color" :
        L"APUSR FSR4 async packed model input color";
    Status status = createInternalModelSidecarTexture(impl.device.Get(), renderSize.width, renderSize.height, modelInputName, frame.modelInputColor);
    if (!status.succeeded()) {
        return status;
    }
    const wchar_t* reprojectedName = kDebugHalf4Sidecars ?
        L"APUSR FSR4 async debug half4 reprojected color" :
        L"APUSR FSR4 async packed reprojected color";
    status = createInternalModelSidecarTexture(impl.device.Get(), displaySize.width, displaySize.height, reprojectedName, frame.reprojectedColor);
    if (!status.succeeded()) {
        return status;
    }
    status = createInternalR32Texture(impl.device.Get(), 2u, 1u, L"APUSR FSR4 async frame exposure", frame.exposureTexture);
    if (!status.succeeded()) {
        return status;
    }

    impl.internalTextureStates[frame.modelInputColor.Get()] = D3D12_RESOURCE_STATE_COMMON;
    impl.internalTextureStates[frame.reprojectedColor.Get()] = D3D12_RESOURCE_STATE_COMMON;
    impl.internalTextureStates[frame.exposureTexture.Get()] = D3D12_RESOURCE_STATE_COMMON;
    return Status::ok();
}


template <typename ImplT>
Status ensureAutoExposureResources(ImplT& impl, const RenderSize renderSize)
{
    if (renderSize.width == 0 || renderSize.height == 0) {
        return Status::invalidArgument("FSR4 DX12 auto exposure requires non-zero render size");
    }

    const std::uint32_t tileColumns = dispatchGroupCount(renderSize.width, 16);
    const std::uint32_t tileRows = dispatchGroupCount(renderSize.height, 16);
    const std::uint32_t tileCount = tileColumns * tileRows;
    const bool recreate = impl.autoExposureSize.width != renderSize.width ||
        impl.autoExposureSize.height != renderSize.height ||
        impl.autoExposureTexture == nullptr ||
        impl.autoExposureTiles == nullptr ||
        impl.autoExposureTileCapacity < tileCount;
    if (!recreate) {
        return Status::ok();
    }

    forgetInternalState(impl.internalTextureStates, impl.autoExposureTexture.Get());
    forgetInternalState(impl.internalTextureStates, impl.autoExposureTiles.Get());
    impl.autoExposureTexture.Reset();
    impl.autoExposureTiles.Reset();
    impl.autoExposureDescriptorHeaps.clear();
    impl.exportSrvHeaps.clear();
    impl.importDescriptorHeaps.clear();
    impl.rcasDescriptorHeaps.clear();
    impl.autoExposureSize = renderSize;
    impl.autoExposureTileColumns = tileColumns;
    impl.autoExposureTileRows = tileRows;
    impl.autoExposureTileCapacity = tileCount;

    Status status = createInternalR32Texture(impl.device.Get(), 2u, 1u, L"APUSR FSR4 auto exposure", impl.autoExposureTexture);
    if (!status.succeeded()) {
        return status;
    }
    status = createInternalByteAddressBuffer(
        impl.device.Get(),
        static_cast<std::uint64_t>(std::max(tileCount, 1u)) * 8ull,
        L"APUSR FSR4 auto exposure tile log-luma",
        impl.autoExposureTiles);
    if (!status.succeeded()) {
        return status;
    }

    impl.internalTextureStates[impl.autoExposureTexture.Get()] = D3D12_RESOURCE_STATE_COMMON;
    impl.internalTextureStates[impl.autoExposureTiles.Get()] = D3D12_RESOURCE_STATE_COMMON;
    return Status::ok();
}

bool needsAutoExposurePass(const UpscalerFrameOptions& options)
{
    return options.features.autoExposure && !options.features.nonLinearColorSpace;
}

template <typename ImplT>
Status recordAutoExposurePass(
    ImplT& impl,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* color,
    const SurfaceFormat colorFormat,
    D3D12_RESOURCE_STATES& colorState,
    const ExportPassDesc& desc)
{
    const RenderSize activeRenderSize = desc.frameConstants.renderSize;
    Status status = ensureAutoExposureResources(impl, activeRenderSize);
    if (!status.succeeded()) {
        return status;
    }

    const std::uintptr_t heapKey = reinterpret_cast<std::uintptr_t>(color) ^
        (reinterpret_cast<std::uintptr_t>(impl.autoExposureTexture.Get()) << 1u);
    DescriptorHeapEntry& entry = impl.autoExposureDescriptorHeaps[heapKey];
    if (!entry.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 2;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        traceLine("AUTO_EXPOSURE create SRV/UAV descriptor heap");
        const HRESULT hr = impl.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&entry.heap));
        if (FAILED(hr) || !entry.heap) {
            return hresultStatus("ID3D12Device::CreateDescriptorHeap(auto exposure)", hr);
        }
        entry.key = heapKey;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = entry.heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = textureSrvDesc(color, colorFormat);
        impl.device->CreateShaderResourceView(color, &colorSrvDesc, handle);
        handle.ptr += impl.descriptorIncrement;
        const D3D12_UNORDERED_ACCESS_VIEW_DESC exposureUavDesc = r32TextureUavDesc();
        impl.device->CreateUnorderedAccessView(impl.autoExposureTexture.Get(), nullptr, &exposureUavDesc, handle);
    }

    ID3D12Resource* tiles = impl.autoExposureTiles.Get();
    ID3D12Resource* exposureTexture = impl.autoExposureTexture.Get();
    const D3D12_RESOURCE_STATES tilesBefore = tokenBufferState(impl.internalTextureStates, tiles);
    const D3D12_RESOURCE_STATES exposureBefore = tokenBufferState(impl.internalTextureStates, exposureTexture);

    std::vector<D3D12_RESOURCE_BARRIER> transitions;
    appendTransition(transitions, color, colorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, tiles, tilesBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    appendTransition(transitions, exposureTexture, exposureBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!transitions.empty()) {
        traceLine("AUTO_EXPOSURE ResourceBarrier count=" + std::to_string(transitions.size()));
        commandList->ResourceBarrier(static_cast<UINT>(transitions.size()), transitions.data());
    }
    colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl.internalTextureStates[tiles] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    impl.internalTextureStates[exposureTexture] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ID3D12DescriptorHeap* heaps[] = {entry.heap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    const AutoExposureConstants constants = makeAutoExposureConstants(
        activeRenderSize,
        desc.frameOptions,
        impl.autoExposureTileColumns,
        impl.autoExposureTileColumns * impl.autoExposureTileRows);

    commandList->SetComputeRootSignature(impl.autoExposureReduceRootSignature.Get());
    commandList->SetPipelineState(impl.autoExposureReducePso.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(AutoExposureConstants) / sizeof(std::uint32_t), &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, entry.heap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootUnorderedAccessView(2, tiles->GetGPUVirtualAddress());
    traceLine("AUTO_EXPOSURE reduce Dispatch groups=" + std::to_string(impl.autoExposureTileColumns) + "x" + std::to_string(impl.autoExposureTileRows));
    commandList->Dispatch(impl.autoExposureTileColumns, impl.autoExposureTileRows, 1);

    D3D12_RESOURCE_BARRIER tileUavBarrier{};
    tileUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    tileUavBarrier.UAV.pResource = tiles;
    commandList->ResourceBarrier(1, &tileUavBarrier);

    commandList->SetComputeRootSignature(impl.autoExposureResolveRootSignature.Get());
    commandList->SetPipelineState(impl.autoExposureResolvePso.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(AutoExposureConstants) / sizeof(std::uint32_t), &constants, 0);
    commandList->SetComputeRootUnorderedAccessView(1, tiles->GetGPUVirtualAddress());
    D3D12_GPU_DESCRIPTOR_HANDLE exposureUav = entry.heap->GetGPUDescriptorHandleForHeapStart();
    exposureUav.ptr += static_cast<SIZE_T>(impl.descriptorIncrement);
    commandList->SetComputeRootDescriptorTable(2, exposureUav);
    traceLine("AUTO_EXPOSURE resolve Dispatch groups=1x1");
    commandList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER exposureUavBarrier{};
    exposureUavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    exposureUavBarrier.UAV.pResource = exposureTexture;
    commandList->ResourceBarrier(1, &exposureUavBarrier);

    std::vector<D3D12_RESOURCE_BARRIER> exposureToSrv;
    appendTransition(exposureToSrv, exposureTexture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!exposureToSrv.empty()) {
        commandList->ResourceBarrier(static_cast<UINT>(exposureToSrv.size()), exposureToSrv.data());
    }
    impl.internalTextureStates[exposureTexture] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return Status::ok();
}



ID3D12Fence* dx12FenceFromTimeline(const TimelineFence& fence)
{
    if (fence.nativeGraphicsFence.value != nullptr) {
        return static_cast<ID3D12Fence*>(fence.nativeGraphicsFence.value);
    }
    return static_cast<ID3D12Fence*>(fence.nativeFence.value);
}

Status reduceRoundTripCopyVerifySlot(RoundTripCopyVerifyResources& slot)
{
    if (!slot.pending) {
        return Status::ok();
    }
    if (!slot.completionFence || slot.completionFence->GetCompletedValue() < slot.completionValue) {
        return Status::ok();
    }
    if (slot.mappedReadback == nullptr) {
        return Status::runtimeFailure("frame-conveyor copy verifier readback buffer is not mapped");
    }

    const auto* words = static_cast<const std::uint32_t*>(slot.mappedReadback);
    const std::uint32_t mismatchCount = words[0];
    const std::uint32_t firstMismatch = words[1];
    const std::uint32_t expectedXor = words[2];
    const std::uint32_t actualXor = words[3];

    std::ostringstream message;
    message << "ROUNDTRIP_COPY_VERIFY frame=" << slot.pendingFrameIndex
            << " bytes=" << slot.pendingByteSize
            << " mismatches=" << mismatchCount
            << " firstMismatch=";
    if (firstMismatch == std::numeric_limits<std::uint32_t>::max()) {
        message << "none";
    } else {
        message << firstMismatch;
    }
    message << " expectedXor=0x" << std::hex << expectedXor
            << " actualXor=0x" << actualXor;
    traceLine(message.str());

    slot.pending = false;
    slot.completionFence.Reset();
    slot.completionValue = 0;
    if (mismatchCount != 0) {
        return Status::runtimeFailure("frame-conveyor copy verifier found ingress/egress token mismatches; see DX12 trace for the first mismatch offset");
    }
    return Status::ok();
}

template <typename ImplT>
Status drainRoundTripCopyVerifier(ImplT& impl)
{
    for (RoundTripCopyVerifyResources& slot : impl.roundTripCopyVerifySlots) {
        Status status = reduceRoundTripCopyVerifySlot(slot);
        if (!status.succeeded()) {
            return status;
        }
    }
    return Status::ok();
}

Status ensureRoundTripCopyVerifyResources(
    ID3D12Device* device,
    RoundTripCopyVerifyResources& slot)
{
    if (slot.resultBuffer != nullptr && slot.readbackBuffer != nullptr && slot.clearUavHeap != nullptr && slot.mappedReadback != nullptr) {
        return Status::ok();
    }

    if (slot.readbackBuffer != nullptr && slot.mappedReadback != nullptr) {
        D3D12_RANGE emptyRange{0, 0};
        slot.readbackBuffer->Unmap(0, &emptyRange);
        slot.mappedReadback = nullptr;
    }
    slot.resultBuffer.Reset();
    slot.readbackBuffer.Reset();
    slot.clearUavHeap.Reset();

    Status status = createInternalByteAddressBuffer(
        device,
        kRoundTripCopyVerifyResultBytes,
        L"APUSR frame-conveyor copy verifier result",
        slot.resultBuffer);
    if (!status.succeeded()) {
        return status;
    }
    status = createReadbackBuffer(
        device,
        kRoundTripCopyVerifyResultBytes,
        L"APUSR frame-conveyor copy verifier readback",
        slot.readbackBuffer);
    if (!status.succeeded()) {
        return status;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT heapHr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&slot.clearUavHeap));
    if (FAILED(heapHr) || !slot.clearUavHeap) {
        return hresultStatus("ID3D12Device::CreateDescriptorHeap(round-trip verifier clear UAV)", heapHr);
    }

    const D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = rawBufferUavDesc(kRoundTripCopyVerifyResultBytes);
    device->CreateUnorderedAccessView(
        slot.resultBuffer.Get(),
        nullptr,
        &uavDesc,
        slot.clearUavHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RANGE readRange{0, kRoundTripCopyVerifyResultBytes};
    const HRESULT mapHr = slot.readbackBuffer->Map(0, &readRange, &slot.mappedReadback);
    if (FAILED(mapHr) || slot.mappedReadback == nullptr) {
        return hresultStatus("ID3D12Resource::Map(round-trip verifier readback)", mapHr);
    }
    return Status::ok();
}

template <typename ImplT>
Status recordRoundTripCopyVerify(
    ImplT& impl,
    ID3D12GraphicsCommandList* commandList,
    const ImportPassDesc& desc)
{
    Status status = drainRoundTripCopyVerifier(impl);
    if (!status.succeeded()) {
        return status;
    }
    if (!desc.roundTripCopyVerify.enabled) {
        return Status::ok();
    }
    if (desc.roundTripCopyVerify.byteSize == 0) {
        return Status::invalidArgument("frame-conveyor copy verifier requires a non-empty byte range");
    }
    status = validateByteAddressBufferRange(
        "roundtrip.verify.expectedIngress",
        desc.roundTripCopyVerify.expected.byteSize,
        desc.roundTripCopyVerify.expectedByteOffset,
        desc.roundTripCopyVerify.byteSize);
    if (!status.succeeded()) {
        return status;
    }
    status = validateByteAddressBufferRange(
        "roundtrip.verify.actualEgress",
        desc.source.byteSize,
        desc.roundTripCopyVerify.actualByteOffset,
        desc.roundTripCopyVerify.byteSize);
    if (!status.succeeded()) {
        return status;
    }

    ID3D12Resource* expected = dx12Resource(desc.roundTripCopyVerify.expected.nativeGraphicsBuffer);
    ID3D12Resource* actual = dx12Resource(desc.source.nativeGraphicsBuffer);
    if (expected == nullptr || actual == nullptr) {
        return Status::invalidArgument("frame-conveyor copy verifier requires GPU-visible ingress and egress buffers");
    }
    ID3D12Fence* completionFence = dx12FenceFromTimeline(desc.roundTripCopyVerify.completionSignal.fence);
    if (completionFence == nullptr || desc.roundTripCopyVerify.completionSignal.value == 0) {
        return Status::invalidArgument("frame-conveyor copy verifier requires the import command-list completion signal");
    }

    RoundTripCopyVerifyResources& slot = impl.roundTripCopyVerifySlots[desc.frameIndex % impl.roundTripCopyVerifySlots.size()];
    status = reduceRoundTripCopyVerifySlot(slot);
    if (!status.succeeded()) {
        return status;
    }
    if (slot.pending) {
        return Status::runtimeFailure("frame-conveyor copy verifier result slot is still pending; increase verifier slot count or reduce GPU queue depth");
    }
    status = ensureRoundTripCopyVerifyResources(impl.device.Get(), slot);
    if (!status.succeeded()) {
        return status;
    }

    std::vector<D3D12_RESOURCE_BARRIER> transitions;
    const D3D12_RESOURCE_STATES expectedBefore = tokenBufferState(impl.tokenBufferStates, expected);
    const D3D12_RESOURCE_STATES actualBefore = tokenBufferState(impl.tokenBufferStates, actual);
    const D3D12_RESOURCE_STATES resultBefore = tokenBufferState(impl.internalTextureStates, slot.resultBuffer.Get());
    traceTransition("roundtrip.verify.expectedIngress", expected, expectedBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("roundtrip.verify.actualEgress", actual, actualBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("roundtrip.verify.result", slot.resultBuffer.Get(), resultBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    appendTransition(transitions, expected, expectedBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, actual, actualBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, slot.resultBuffer.Get(), resultBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!transitions.empty()) {
        traceLine("ROUNDTRIP_COPY_VERIFY ResourceBarrier count=" + std::to_string(transitions.size()));
        commandList->ResourceBarrier(static_cast<UINT>(transitions.size()), transitions.data());
    }
    impl.tokenBufferStates[expected] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl.tokenBufferStates[actual] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl.internalTextureStates[slot.resultBuffer.Get()] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    const std::uint32_t clearValues[4]{0u, std::numeric_limits<std::uint32_t>::max(), 0u, 0u};
    commandList->ClearUnorderedAccessViewUint(
        slot.clearUavHeap->GetGPUDescriptorHandleForHeapStart(),
        slot.clearUavHeap->GetCPUDescriptorHandleForHeapStart(),
        slot.resultBuffer.Get(),
        clearValues,
        0,
        nullptr);

    const std::uint32_t byteSize = static_cast<std::uint32_t>(desc.roundTripCopyVerify.byteSize);
    const std::uint32_t wordCount = (byteSize + 3u) / 4u;
    const RoundTripCopyVerifyConstants constants{
        .expectedByteOffsetLow = static_cast<std::uint32_t>(desc.roundTripCopyVerify.expectedByteOffset),
        .expectedByteOffsetHigh = 0,
        .actualByteOffsetLow = static_cast<std::uint32_t>(desc.roundTripCopyVerify.actualByteOffset),
        .actualByteOffsetHigh = 0,
        .byteSizeLow = byteSize,
        .byteSizeHigh = 0,
        .wordCount = wordCount,
        .pad0 = 0,
    };

    commandList->SetComputeRootSignature(impl.roundTripCopyVerifyRootSignature.Get());
    commandList->SetPipelineState(impl.roundTripCopyVerifyPso.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(RoundTripCopyVerifyConstants) / sizeof(std::uint32_t), &constants, 0);
    commandList->SetComputeRootShaderResourceView(1, desc.roundTripCopyVerify.expected.gpuVirtualAddress);
    commandList->SetComputeRootShaderResourceView(2, desc.source.gpuVirtualAddress);
    commandList->SetComputeRootUnorderedAccessView(3, slot.resultBuffer->GetGPUVirtualAddress());
    const std::uint32_t groupCount = dispatchGroupCount(wordCount, kRoundTripCopyVerifyThreads);
    traceLine("ROUNDTRIP_COPY_VERIFY Dispatch groups=" + std::to_string(groupCount) + " words=" + std::to_string(wordCount) + " bytes=" + std::to_string(byteSize));
    commandList->Dispatch(groupCount, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = slot.resultBuffer.Get();
    commandList->ResourceBarrier(1, &uavBarrier);

    std::vector<D3D12_RESOURCE_BARRIER> copyTransitions;
    appendTransition(copyTransitions, slot.resultBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!copyTransitions.empty()) {
        commandList->ResourceBarrier(static_cast<UINT>(copyTransitions.size()), copyTransitions.data());
    }
    impl.internalTextureStates[slot.resultBuffer.Get()] = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->CopyBufferRegion(slot.readbackBuffer.Get(), 0, slot.resultBuffer.Get(), 0, kRoundTripCopyVerifyResultBytes);

    slot.pending = true;
    slot.pendingFrameIndex = desc.frameIndex;
    slot.pendingByteSize = desc.roundTripCopyVerify.byteSize;
    slot.completionFence = completionFence;
    slot.completionValue = desc.roundTripCopyVerify.completionSignal.value;
    return Status::ok();
}

template <typename ImplT>
Status recordCaptureModelInputPass(
    ImplT& impl,
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* inputColor,
    const SurfaceFormat inputColorFormat,
    D3D12_RESOURCE_STATES& inputColorState,
    ID3D12Resource* inputExposure,
    const SurfaceFormat inputExposureFormat,
    D3D12_RESOURCE_STATES inputExposureState,
    AsyncBookendFrameResources& frameResources,
    const ExportPassDesc& desc)
{
    ID3D12Resource* modelInput = frameResources.modelInputColor.Get();
    ID3D12Resource* frameExposure = frameResources.exposureTexture.Get();
    if (modelInput == nullptr || frameExposure == nullptr) {
        return Status::invalidArgument("FSR4 DX12 capture pass requires async frame-owned output textures");
    }

    const std::uintptr_t heapKey = reinterpret_cast<std::uintptr_t>(inputColor) ^
        (reinterpret_cast<std::uintptr_t>(inputExposure) << 1u) ^
        (reinterpret_cast<std::uintptr_t>(modelInput) << 2u) ^
        (reinterpret_cast<std::uintptr_t>(frameExposure) << 3u);
    DescriptorHeapEntry& entry = impl.captureDescriptorHeaps[heapKey];
    if (!entry.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 4;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        traceLine("CAPTURE create SRV/UAV descriptor heap");
        const HRESULT hr = impl.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&entry.heap));
        if (FAILED(hr) || !entry.heap) {
            return hresultStatus("ID3D12Device::CreateDescriptorHeap(capture model input)", hr);
        }
        entry.key = heapKey;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = entry.heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = textureSrvDesc(inputColor, inputColorFormat);
        impl.device->CreateShaderResourceView(inputColor, &colorSrvDesc, handle);
        handle.ptr += impl.descriptorIncrement;
        if (inputExposure != nullptr) {
            const D3D12_SHADER_RESOURCE_VIEW_DESC exposureSrvDesc = inputExposure == impl.autoExposureTexture.Get()
                ? r32TextureSrvDesc()
                : textureSrvDesc(inputExposure, inputExposureFormat);
            impl.device->CreateShaderResourceView(inputExposure, &exposureSrvDesc, handle);
        } else {
            D3D12_SHADER_RESOURCE_VIEW_DESC nullExposureSrvDesc{};
            nullExposureSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            nullExposureSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullExposureSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullExposureSrvDesc.Texture2D.MipLevels = 1;
            impl.device->CreateShaderResourceView(nullptr, &nullExposureSrvDesc, handle);
        }
        handle.ptr += impl.descriptorIncrement;
        const D3D12_UNORDERED_ACCESS_VIEW_DESC modelInputUavDesc = modelSidecarTextureUavDesc();
        impl.device->CreateUnorderedAccessView(modelInput, nullptr, &modelInputUavDesc, handle);
        handle.ptr += impl.descriptorIncrement;
        const D3D12_UNORDERED_ACCESS_VIEW_DESC exposureUavDesc = r32TextureUavDesc();
        impl.device->CreateUnorderedAccessView(frameExposure, nullptr, &exposureUavDesc, handle);
    }

    std::vector<D3D12_RESOURCE_BARRIER> transitions;
    const D3D12_RESOURCE_STATES modelInputBefore = tokenBufferState(impl.internalTextureStates, modelInput);
    const D3D12_RESOURCE_STATES frameExposureBefore = tokenBufferState(impl.internalTextureStates, frameExposure);
    appendTransition(transitions, inputColor, inputColorState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (inputExposure != nullptr) {
        appendTransition(transitions, inputExposure, inputExposureState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    appendTransition(transitions, modelInput, modelInputBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    appendTransition(transitions, frameExposure, frameExposureBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!transitions.empty()) {
        traceLine("CAPTURE ResourceBarrier count=" + std::to_string(transitions.size()));
        commandList->ResourceBarrier(static_cast<UINT>(transitions.size()), transitions.data());
    }
    inputColorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (inputExposure == impl.autoExposureTexture.Get()) {
        impl.internalTextureStates[inputExposure] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    }
    impl.internalTextureStates[modelInput] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    impl.internalTextureStates[frameExposure] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ID3D12DescriptorHeap* heaps[] = {entry.heap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(impl.captureModelInputRootSignature.Get());
    commandList->SetPipelineState(impl.captureModelInputPso.Get());
    const TokenBandConstants constants = makeConstants(desc);
    commandList->SetComputeRoot32BitConstants(0, sizeof(TokenBandConstants) / sizeof(std::uint32_t), &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, entry.heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_GPU_DESCRIPTOR_HANDLE uavTable = entry.heap->GetGPUDescriptorHandleForHeapStart();
    uavTable.ptr += static_cast<SIZE_T>(impl.descriptorIncrement) * 2u;
    commandList->SetComputeRootDescriptorTable(2, uavTable);

    const std::uint32_t groupCountX = dispatchGroupCount(desc.frameConstants.renderSize.width, 16);
    const std::uint32_t groupCountY = dispatchGroupCount(desc.frameConstants.renderSize.height, 8);
    traceLine("CAPTURE Dispatch groups=" + std::to_string(groupCountX) + "x" + std::to_string(groupCountY));
    commandList->Dispatch(groupCountX, groupCountY, 1);

    D3D12_RESOURCE_BARRIER uavBarriers[2]{};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = modelInput;
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = frameExposure;
    commandList->ResourceBarrier(2, uavBarriers);
    return Status::ok();
}

D3D12_SHADER_RESOURCE_VIEW_DESC rgba16TextureSrvDesc()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC nullScalarTextureSrvDesc()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC r32TextureSrvDesc()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    return desc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC r32TextureUavDesc()
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.PlaneSlice = 0;
    return desc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC rgba16TextureUavDesc()
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.PlaneSlice = 0;
    return desc;
}

D3D12_SHADER_RESOURCE_VIEW_DESC r32UintTextureSrvDesc()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    return desc;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC r32UintTextureUavDesc()
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.PlaneSlice = 0;
    return desc;
}

SurfaceFormat modelSidecarSurfaceFormat()
{
    return kDebugHalf4Sidecars ? SurfaceFormat::Rgba16Float : SurfaceFormat::R32Uint;
}

D3D12_SHADER_RESOURCE_VIEW_DESC modelSidecarTextureSrvDesc()
{
    return kDebugHalf4Sidecars ? rgba16TextureSrvDesc() : r32UintTextureSrvDesc();
}

D3D12_UNORDERED_ACCESS_VIEW_DESC modelSidecarTextureUavDesc()
{
    return kDebugHalf4Sidecars ? rgba16TextureUavDesc() : r32UintTextureUavDesc();
}

shader_blobs::ShaderBlob captureModelInputShader()
{
    return kDebugHalf4Sidecars ? shader_blobs::fsr4CaptureModelInputHalf4() : shader_blobs::fsr4CaptureModelInputPacked();
}

shader_blobs::ShaderBlob exportTokensShader()
{
    return kDebugHalf4Sidecars ? shader_blobs::fsr4ExportTokensHalf4() : shader_blobs::fsr4ExportTokensPacked();
}

shader_blobs::ShaderBlob importTokensShader()
{
    return kDebugHalf4Sidecars ? shader_blobs::fsr4ImportTokensHalf4() : shader_blobs::fsr4ImportTokensPacked();
}

#endif

Fsr4Dx12TokenShaderRecorder::Fsr4Dx12TokenShaderRecorder() : impl_(std::make_unique<Impl>()) {}
Fsr4Dx12TokenShaderRecorder::~Fsr4Dx12TokenShaderRecorder() = default;
Fsr4Dx12TokenShaderRecorder::Fsr4Dx12TokenShaderRecorder(Fsr4Dx12TokenShaderRecorder&&) noexcept = default;
Fsr4Dx12TokenShaderRecorder& Fsr4Dx12TokenShaderRecorder::operator=(Fsr4Dx12TokenShaderRecorder&&) noexcept = default;

Status Fsr4Dx12TokenShaderRecorder::initialize(const NativeHandle deviceHandle)
{
#ifdef _WIN32
    ID3D12Device* rawDevice = dx12Device(deviceHandle);
    if (rawDevice == nullptr) {
        return Status::invalidArgument("FSR4 DX12 token shader recorder requires an ID3D12Device");
    }

    impl_->device = rawDevice;
    impl_->descriptorIncrement = rawDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    Status status = Status::ok();

    D3D12_DESCRIPTOR_RANGE autoExposureReduceSrvRange{};
    autoExposureReduceSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    autoExposureReduceSrvRange.NumDescriptors = 1;
    autoExposureReduceSrvRange.BaseShaderRegister = 0;
    autoExposureReduceSrvRange.RegisterSpace = 0;
    autoExposureReduceSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER autoExposureReduceParams[3]{};
    autoExposureReduceParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    autoExposureReduceParams[0].Constants.ShaderRegister = 0;
    autoExposureReduceParams[0].Constants.RegisterSpace = 0;
    autoExposureReduceParams[0].Constants.Num32BitValues = sizeof(AutoExposureConstants) / sizeof(std::uint32_t);
    autoExposureReduceParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    autoExposureReduceParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    autoExposureReduceParams[1].DescriptorTable.NumDescriptorRanges = 1;
    autoExposureReduceParams[1].DescriptorTable.pDescriptorRanges = &autoExposureReduceSrvRange;
    autoExposureReduceParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    autoExposureReduceParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    autoExposureReduceParams[2].Descriptor.ShaderRegister = 0;
    autoExposureReduceParams[2].Descriptor.RegisterSpace = 0;
    autoExposureReduceParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC autoExposureReduceRootDesc{};
    autoExposureReduceRootDesc.NumParameters = 3;
    autoExposureReduceRootDesc.pParameters = autoExposureReduceParams;
    autoExposureReduceRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> autoExposureReduceRootBlob;
    status = serializeRootSignature(autoExposureReduceRootDesc, autoExposureReduceRootBlob, "APUSR auto exposure reduce root signature");
    if (!status.succeeded()) {
        return status;
    }
    HRESULT hr = rawDevice->CreateRootSignature(
        0,
        autoExposureReduceRootBlob->GetBufferPointer(),
        autoExposureReduceRootBlob->GetBufferSize(),
        IID_PPV_ARGS(&impl_->autoExposureReduceRootSignature));
    if (FAILED(hr) || !impl_->autoExposureReduceRootSignature) {
        return hresultStatus("ID3D12Device::CreateRootSignature(auto exposure reduce)", hr);
    }
    status = createComputePso(
        rawDevice,
        impl_->autoExposureReduceRootSignature.Get(),
        shader_blobs::fsr4AutoExposureReduce(),
        impl_->autoExposureReducePso,
        "APUSR auto exposure reduce PSO");
    if (!status.succeeded()) {
        return status;
    }

    D3D12_DESCRIPTOR_RANGE autoExposureResolveUavRange{};
    autoExposureResolveUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    autoExposureResolveUavRange.NumDescriptors = 1;
    autoExposureResolveUavRange.BaseShaderRegister = 1;
    autoExposureResolveUavRange.RegisterSpace = 0;
    autoExposureResolveUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER autoExposureResolveParams[3]{};
    autoExposureResolveParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    autoExposureResolveParams[0].Constants.ShaderRegister = 0;
    autoExposureResolveParams[0].Constants.RegisterSpace = 0;
    autoExposureResolveParams[0].Constants.Num32BitValues = sizeof(AutoExposureConstants) / sizeof(std::uint32_t);
    autoExposureResolveParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    autoExposureResolveParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    autoExposureResolveParams[1].Descriptor.ShaderRegister = 0;
    autoExposureResolveParams[1].Descriptor.RegisterSpace = 0;
    autoExposureResolveParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    autoExposureResolveParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    autoExposureResolveParams[2].DescriptorTable.NumDescriptorRanges = 1;
    autoExposureResolveParams[2].DescriptorTable.pDescriptorRanges = &autoExposureResolveUavRange;
    autoExposureResolveParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC autoExposureResolveRootDesc{};
    autoExposureResolveRootDesc.NumParameters = 3;
    autoExposureResolveRootDesc.pParameters = autoExposureResolveParams;
    autoExposureResolveRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> autoExposureResolveRootBlob;
    status = serializeRootSignature(autoExposureResolveRootDesc, autoExposureResolveRootBlob, "APUSR auto exposure resolve root signature");
    if (!status.succeeded()) {
        return status;
    }
    hr = rawDevice->CreateRootSignature(
        0,
        autoExposureResolveRootBlob->GetBufferPointer(),
        autoExposureResolveRootBlob->GetBufferSize(),
        IID_PPV_ARGS(&impl_->autoExposureResolveRootSignature));
    if (FAILED(hr) || !impl_->autoExposureResolveRootSignature) {
        return hresultStatus("ID3D12Device::CreateRootSignature(auto exposure resolve)", hr);
    }
    status = createComputePso(
        rawDevice,
        impl_->autoExposureResolveRootSignature.Get(),
        shader_blobs::fsr4AutoExposureResolve(),
        impl_->autoExposureResolvePso,
        "APUSR auto exposure resolve PSO");
    if (!status.succeeded()) {
        return status;
    }


    D3D12_DESCRIPTOR_RANGE captureSrvRange{};
    captureSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    captureSrvRange.NumDescriptors = 2;
    captureSrvRange.BaseShaderRegister = 0;
    captureSrvRange.RegisterSpace = 0;
    captureSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE captureUavRange{};
    captureUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    captureUavRange.NumDescriptors = 2;
    captureUavRange.BaseShaderRegister = 0;
    captureUavRange.RegisterSpace = 0;
    captureUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER captureParams[3]{};
    captureParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    captureParams[0].Constants.ShaderRegister = 0;
    captureParams[0].Constants.RegisterSpace = 0;
    captureParams[0].Constants.Num32BitValues = sizeof(TokenBandConstants) / sizeof(std::uint32_t);
    captureParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    captureParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    captureParams[1].DescriptorTable.NumDescriptorRanges = 1;
    captureParams[1].DescriptorTable.pDescriptorRanges = &captureSrvRange;
    captureParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    captureParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    captureParams[2].DescriptorTable.NumDescriptorRanges = 1;
    captureParams[2].DescriptorTable.pDescriptorRanges = &captureUavRange;
    captureParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC captureRootDesc{};
    captureRootDesc.NumParameters = 3;
    captureRootDesc.pParameters = captureParams;
    captureRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> captureRootBlob;
    status = serializeRootSignature(captureRootDesc, captureRootBlob, "APUSR capture model-input root signature");
    if (!status.succeeded()) {
        return status;
    }
    hr = rawDevice->CreateRootSignature(
        0,
        captureRootBlob->GetBufferPointer(),
        captureRootBlob->GetBufferSize(),
        IID_PPV_ARGS(&impl_->captureModelInputRootSignature));
    if (FAILED(hr) || !impl_->captureModelInputRootSignature) {
        return hresultStatus("ID3D12Device::CreateRootSignature(capture model input)", hr);
    }
    status = createComputePso(
        rawDevice,
        impl_->captureModelInputRootSignature.Get(),
        captureModelInputShader(),
        impl_->captureModelInputPso,
        "APUSR capture model-input PSO");
    if (!status.succeeded()) {
        return status;
    }

    D3D12_DESCRIPTOR_RANGE exportSrvRange{};
    exportSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    exportSrvRange.NumDescriptors = 8;
    exportSrvRange.BaseShaderRegister = 0;
    exportSrvRange.RegisterSpace = 0;
    exportSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE exportUavRange{};
    exportUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    exportUavRange.NumDescriptors = 1;
    exportUavRange.BaseShaderRegister = 1;
    exportUavRange.RegisterSpace = 0;
    exportUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER exportParams[4]{};
    exportParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    exportParams[0].Constants.ShaderRegister = 0;
    exportParams[0].Constants.RegisterSpace = 0;
    exportParams[0].Constants.Num32BitValues = sizeof(TokenBandConstants) / sizeof(std::uint32_t);
    exportParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    exportParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    exportParams[1].DescriptorTable.NumDescriptorRanges = 1;
    exportParams[1].DescriptorTable.pDescriptorRanges = &exportSrvRange;
    exportParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    exportParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    exportParams[2].Descriptor.ShaderRegister = 0;
    exportParams[2].Descriptor.RegisterSpace = 0;
    exportParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    exportParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    exportParams[3].DescriptorTable.NumDescriptorRanges = 1;
    exportParams[3].DescriptorTable.pDescriptorRanges = &exportUavRange;
    exportParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC exportStaticSampler{};
    exportStaticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    exportStaticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    exportStaticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    exportStaticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    exportStaticSampler.MipLODBias = 0.0f;
    exportStaticSampler.MaxAnisotropy = 1;
    exportStaticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    exportStaticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    exportStaticSampler.MinLOD = 0.0f;
    exportStaticSampler.MaxLOD = D3D12_FLOAT32_MAX;
    exportStaticSampler.ShaderRegister = 0;
    exportStaticSampler.RegisterSpace = 0;
    exportStaticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC exportRootDesc{};
    exportRootDesc.NumParameters = 4;
    exportRootDesc.pParameters = exportParams;
    exportRootDesc.NumStaticSamplers = 1;
    exportRootDesc.pStaticSamplers = &exportStaticSampler;
    exportRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> exportRootBlob;
    status = serializeRootSignature(exportRootDesc, exportRootBlob, "APUSR export root signature");
    if (!status.succeeded()) {
        return status;
    }
    hr = rawDevice->CreateRootSignature(
        0,
        exportRootBlob->GetBufferPointer(),
        exportRootBlob->GetBufferSize(),
        IID_PPV_ARGS(&impl_->exportRootSignature));
    if (FAILED(hr) || !impl_->exportRootSignature) {
        return hresultStatus("ID3D12Device::CreateRootSignature(export)", hr);
    }

    status = createComputePso(rawDevice, impl_->exportRootSignature.Get(), exportTokensShader(), impl_->exportPso, "APUSR export PSO");
    if (!status.succeeded()) {
        return status;
    }

    D3D12_DESCRIPTOR_RANGE importSrvRange{};
    importSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    importSrvRange.NumDescriptors = 3;
    importSrvRange.BaseShaderRegister = 1;
    importSrvRange.RegisterSpace = 0;
    importSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE importUavRange{};
    importUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    importUavRange.NumDescriptors = 3;
    importUavRange.BaseShaderRegister = 0;
    importUavRange.RegisterSpace = 0;
    importUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER importParams[4]{};
    importParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    importParams[0].Constants.ShaderRegister = 0;
    importParams[0].Constants.RegisterSpace = 0;
    importParams[0].Constants.Num32BitValues = sizeof(TokenBandConstants) / sizeof(std::uint32_t);
    importParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    importParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    importParams[1].Descriptor.ShaderRegister = 0;
    importParams[1].Descriptor.RegisterSpace = 0;
    importParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    importParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    importParams[2].DescriptorTable.NumDescriptorRanges = 1;
    importParams[2].DescriptorTable.pDescriptorRanges = &importSrvRange;
    importParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    importParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    importParams[3].DescriptorTable.NumDescriptorRanges = 1;
    importParams[3].DescriptorTable.pDescriptorRanges = &importUavRange;
    importParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC importRootDesc{};
    importRootDesc.NumParameters = 4;
    importRootDesc.pParameters = importParams;
    importRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> importRootBlob;
    status = serializeRootSignature(importRootDesc, importRootBlob, "APUSR import root signature");
    if (!status.succeeded()) {
        return status;
    }
    hr = rawDevice->CreateRootSignature(
        0,
        importRootBlob->GetBufferPointer(),
        importRootBlob->GetBufferSize(),
        IID_PPV_ARGS(&impl_->importRootSignature));
    if (FAILED(hr) || !impl_->importRootSignature) {
        return hresultStatus("ID3D12Device::CreateRootSignature(import)", hr);
    }

    status = createComputePso(rawDevice, impl_->importRootSignature.Get(), importTokensShader(), impl_->importPso, "APUSR import PSO");
    if (!status.succeeded()) {
        return status;
    }

    D3D12_ROOT_PARAMETER verifyParams[4]{};
    verifyParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    verifyParams[0].Constants.ShaderRegister = 0;
    verifyParams[0].Constants.RegisterSpace = 0;
    verifyParams[0].Constants.Num32BitValues = sizeof(RoundTripCopyVerifyConstants) / sizeof(std::uint32_t);
    verifyParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    verifyParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    verifyParams[1].Descriptor.ShaderRegister = 0;
    verifyParams[1].Descriptor.RegisterSpace = 0;
    verifyParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    verifyParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    verifyParams[2].Descriptor.ShaderRegister = 1;
    verifyParams[2].Descriptor.RegisterSpace = 0;
    verifyParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    verifyParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    verifyParams[3].Descriptor.ShaderRegister = 0;
    verifyParams[3].Descriptor.RegisterSpace = 0;
    verifyParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC verifyRootDesc{};
    verifyRootDesc.NumParameters = 4;
    verifyRootDesc.pParameters = verifyParams;
    verifyRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> verifyRootBlob;
    status = serializeRootSignature(verifyRootDesc, verifyRootBlob, "APUSR frame-conveyor copy verifier root signature");
    if (!status.succeeded()) {
        return status;
    }
    hr = rawDevice->CreateRootSignature(
        0,
        verifyRootBlob->GetBufferPointer(),
        verifyRootBlob->GetBufferSize(),
        IID_PPV_ARGS(&impl_->roundTripCopyVerifyRootSignature));
    if (FAILED(hr) || !impl_->roundTripCopyVerifyRootSignature) {
        return hresultStatus("ID3D12Device::CreateRootSignature(frame-conveyor copy verifier)", hr);
    }
    status = createComputePso(
        rawDevice,
        impl_->roundTripCopyVerifyRootSignature.Get(),
        shader_blobs::roundTripCopyVerify(),
        impl_->roundTripCopyVerifyPso,
        "APUSR frame-conveyor copy verifier PSO");
    if (!status.succeeded()) {
        return status;
    }

    D3D12_DESCRIPTOR_RANGE rcasSrvRange{};
    rcasSrvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rcasSrvRange.NumDescriptors = 2;
    rcasSrvRange.BaseShaderRegister = 0;
    rcasSrvRange.RegisterSpace = 0;
    rcasSrvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE rcasUavRange{};
    rcasUavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rcasUavRange.NumDescriptors = 1;
    rcasUavRange.BaseShaderRegister = 0;
    rcasUavRange.RegisterSpace = 0;
    rcasUavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rcasParams[3]{};
    rcasParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rcasParams[0].Constants.ShaderRegister = 0;
    rcasParams[0].Constants.RegisterSpace = 0;
    rcasParams[0].Constants.Num32BitValues = sizeof(TokenBandConstants) / sizeof(std::uint32_t);
    rcasParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rcasParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rcasParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rcasParams[1].DescriptorTable.pDescriptorRanges = &rcasSrvRange;
    rcasParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rcasParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rcasParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rcasParams[2].DescriptorTable.pDescriptorRanges = &rcasUavRange;
    rcasParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rcasRootDesc{};
    rcasRootDesc.NumParameters = 3;
    rcasRootDesc.pParameters = rcasParams;
    rcasRootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> rcasRootBlob;
    status = serializeRootSignature(rcasRootDesc, rcasRootBlob, "APUSR RCAS root signature");
    if (!status.succeeded()) {
        return status;
    }
    hr = rawDevice->CreateRootSignature(
        0,
        rcasRootBlob->GetBufferPointer(),
        rcasRootBlob->GetBufferSize(),
        IID_PPV_ARGS(&impl_->rcasRootSignature));
    if (FAILED(hr) || !impl_->rcasRootSignature) {
        return hresultStatus("ID3D12Device::CreateRootSignature(RCAS)", hr);
    }

    return createComputePso(rawDevice, impl_->rcasRootSignature.Get(), shader_blobs::fsr4Rcas(), impl_->rcasPso, "APUSR RCAS PSO");
#else
    (void)deviceHandle;
    return Status::notImplemented("FSR4 DX12 token shader recorder is only available in Windows builds");
#endif
}

Status Fsr4Dx12TokenShaderRecorder::recordExport(backends::Dx12RecordContext& context, const ExportPassDesc& desc)
{
#ifdef _WIN32
    ID3D12GraphicsCommandList* commandList = dx12CommandList(context);
    ID3D12Resource* color = dx12Resource(desc.color.resource);
    ID3D12Resource* depth = dx12Resource(desc.depth.resource);
    ID3D12Resource* motionVectors = dx12Resource(desc.motionVectors.resource);
    ID3D12Resource* externalExposure = dx12Resource(desc.exposure.resource);
    ID3D12Resource* exposure = externalExposure;
    ID3D12Resource* reactiveMask = dx12Resource(desc.reactiveMask.resource);
    ID3D12Resource* transparencyMask = dx12Resource(desc.transparencyComposition.resource);
    ID3D12Resource* tokenBuffer = dx12Resource(desc.destination.nativeGraphicsBuffer);
    if (commandList == nullptr || color == nullptr || depth == nullptr || motionVectors == nullptr || tokenBuffer == nullptr) {
        return Status::invalidArgument("FSR4 DX12 export recording requires color/depth/motion-vector resources and an ingress token buffer");
    }

    traceLine("BEGIN recordExport frame-band commandList=" + pointerString(commandList));
    traceLine("EXPORT colorState " + stateString(desc.color.nativeState, dx12StateFromFfxState(desc.color.nativeState)));
    traceLine("EXPORT depthState " + stateString(desc.depth.nativeState, dx12StateFromFfxState(desc.depth.nativeState)));
    traceLine("EXPORT motionState " + stateString(desc.motionVectors.nativeState, dx12StateFromFfxState(desc.motionVectors.nativeState)));
    if (externalExposure != nullptr) {
        traceLine("EXPORT exposureState " + stateString(desc.exposure.nativeState, dx12StateFromFfxState(desc.exposure.nativeState)));
    }
    if (reactiveMask != nullptr) {
        traceLine("EXPORT reactiveMaskState " + stateString(desc.reactiveMask.nativeState, dx12StateFromFfxState(desc.reactiveMask.nativeState)));
    }
    if (transparencyMask != nullptr) {
        traceLine("EXPORT transparencyMaskState " + stateString(desc.transparencyComposition.nativeState, dx12StateFromFfxState(desc.transparencyComposition.nativeState)));
    }

    Status historyStatus = ensureInternalHistoryResources(*impl_, desc.frameConstants.displaySize);
    if (!historyStatus.succeeded()) {
        return historyStatus;
    }

    const std::uint32_t asyncFrameSlot = static_cast<std::uint32_t>(desc.frameIndex % kAsyncBookendFrameSlots);
    Status asyncResourceStatus = ensureAsyncBookendFrameResources(*impl_, asyncFrameSlot, desc.frameConstants.renderSize, desc.frameConstants.displaySize);
    if (!asyncResourceStatus.succeeded()) {
        return asyncResourceStatus;
    }
    AsyncBookendFrameResources& asyncFrame = impl_->asyncFrames[asyncFrameSlot];

    const bool captureThisBand = desc.band.firstBandInFrame;
    ID3D12Resource* gameColor = color;
    const SurfaceFormat gameColorFormat = desc.color.format;
    D3D12_RESOURCE_STATES gameColorBefore = dx12StateFromFfxState(desc.color.nativeState);
    if (captureThisBand) {
        if (needsAutoExposurePass(desc.frameOptions)) {
            Status autoExposureStatus = recordAutoExposurePass(*impl_, commandList, gameColor, gameColorFormat, gameColorBefore, desc);
            if (!autoExposureStatus.succeeded()) {
                return autoExposureStatus;
            }
            exposure = impl_->autoExposureTexture.Get();
        }

        const bool captureInternalAutoExposure = exposure != nullptr && exposure == impl_->autoExposureTexture.Get();
        const SurfaceFormat captureExposureFormat = captureInternalAutoExposure ? SurfaceFormat::R32Float : desc.exposure.format;
        const D3D12_RESOURCE_STATES captureExposureState = captureInternalAutoExposure
            ? tokenBufferState(impl_->internalTextureStates, exposure)
            : dx12StateFromFfxState(desc.exposure.nativeState);
        Status captureStatus = recordCaptureModelInputPass(
            *impl_,
            commandList,
            gameColor,
            gameColorFormat,
            gameColorBefore,
            exposure,
            captureExposureFormat,
            captureExposureState,
            asyncFrame,
            desc);
        if (!captureStatus.succeeded()) {
            return captureStatus;
        }
    }

    color = asyncFrame.modelInputColor.Get();
    exposure = asyncFrame.exposureTexture.Get();
    ID3D12Resource* historyRead = impl_->historyColor[impl_->historyReadIndex].Get();
    ID3D12Resource* recurrentRead = impl_->recurrentColor[impl_->historyReadIndex].Get();
    ID3D12Resource* reprojectedColor = asyncFrame.reprojectedColor.Get();
    D3D12_RESOURCE_STATES colorBefore = tokenBufferState(impl_->internalTextureStates, color);

    Status validationStatus = validateTextureSrv("export.modelInputColor", color, modelSidecarSurfaceFormat());
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }
    validationStatus = validateTextureSrv("export.depth", depth, desc.depth.format);
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }
    validationStatus = validateTextureSrv("export.motionVectors", motionVectors, desc.motionVectors.format);
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }
    const bool internalFrameExposure = exposure != nullptr && exposure == asyncFrame.exposureTexture.Get();
    const SurfaceFormat exposureFormat = internalFrameExposure ? SurfaceFormat::R32Float : desc.exposure.format;
    if (exposure != nullptr && !internalFrameExposure) {
        validationStatus = validateTextureSrv("export.exposure", exposure, exposureFormat);
        if (!validationStatus.succeeded()) {
            return validationStatus;
        }
    }
    if (reactiveMask != nullptr) {
        validationStatus = validateTextureSrv("export.reactiveMask", reactiveMask, desc.reactiveMask.format);
        if (!validationStatus.succeeded()) {
            return validationStatus;
        }
    }
    if (transparencyMask != nullptr) {
        validationStatus = validateTextureSrv("export.transparencyMask", transparencyMask, desc.transparencyComposition.format);
        if (!validationStatus.succeeded()) {
            return validationStatus;
        }
    }
    validationStatus = validateRootUavBuffer("export.destinationTokens", tokenBuffer);
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }
    validationStatus = validateByteAddressBufferRange(
        "export.destinationTokens",
        desc.destination.byteSize,
        desc.destinationByteOffset,
        tokenRowsByteCount(desc.destinationBoundary, desc.band.inputRowCount));
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }

    const std::uintptr_t heapKey = reinterpret_cast<std::uintptr_t>(color) ^
        (reinterpret_cast<std::uintptr_t>(depth) << 1u) ^
        (reinterpret_cast<std::uintptr_t>(motionVectors) << 2u) ^
        (reinterpret_cast<std::uintptr_t>(exposure) << 3u) ^
        (reinterpret_cast<std::uintptr_t>(historyRead) << 4u) ^
        (reinterpret_cast<std::uintptr_t>(recurrentRead) << 5u) ^
        (reinterpret_cast<std::uintptr_t>(reprojectedColor) << 6u) ^
        (reinterpret_cast<std::uintptr_t>(reactiveMask) << 7u) ^
        (reinterpret_cast<std::uintptr_t>(transparencyMask) << 8u);
    DescriptorHeapEntry& entry = impl_->exportSrvHeaps[heapKey];
    if (!entry.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 9;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        traceLine("EXPORT create SRV descriptor heap");
        const HRESULT hr = impl_->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&entry.heap));
        if (FAILED(hr) || !entry.heap) {
            return hresultStatus("ID3D12Device::CreateDescriptorHeap(export SRVs)", hr);
        }
        entry.key = heapKey;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = entry.heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = modelSidecarTextureSrvDesc();
        traceLine(std::string("EXPORT CreateShaderResourceView color view=") + dxgiFormatName(colorSrvDesc.Format));
        impl_->device->CreateShaderResourceView(color, &colorSrvDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        const D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = textureSrvDesc(depth, desc.depth.format);
        traceLine(std::string("EXPORT CreateShaderResourceView depth view=") + dxgiFormatName(depthSrvDesc.Format));
        impl_->device->CreateShaderResourceView(depth, &depthSrvDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        const D3D12_SHADER_RESOURCE_VIEW_DESC motionVectorSrvDesc = textureSrvDesc(motionVectors, desc.motionVectors.format);
        traceLine(std::string("EXPORT CreateShaderResourceView motionVectors view=") + dxgiFormatName(motionVectorSrvDesc.Format));
        impl_->device->CreateShaderResourceView(motionVectors, &motionVectorSrvDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        D3D12_SHADER_RESOURCE_VIEW_DESC exposureSrvDesc{};
        if (exposure != nullptr) {
            exposureSrvDesc = internalFrameExposure ? r32TextureSrvDesc() : textureSrvDesc(exposure, exposureFormat);
            traceLine(std::string("EXPORT CreateShaderResourceView exposure view=") + dxgiFormatName(exposureSrvDesc.Format));
            impl_->device->CreateShaderResourceView(exposure, &exposureSrvDesc, handle);
        } else {
            exposureSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            exposureSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            exposureSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            exposureSrvDesc.Texture2D.MipLevels = 1;
            traceLine("EXPORT CreateShaderResourceView exposure null view=R32_FLOAT");
            impl_->device->CreateShaderResourceView(nullptr, &exposureSrvDesc, handle);
        }
        handle.ptr += impl_->descriptorIncrement;
        const D3D12_SHADER_RESOURCE_VIEW_DESC historySrvDesc = rgba16TextureSrvDesc();
        impl_->device->CreateShaderResourceView(historyRead, &historySrvDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        impl_->device->CreateShaderResourceView(recurrentRead, &historySrvDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        if (reactiveMask != nullptr) {
            const D3D12_SHADER_RESOURCE_VIEW_DESC reactiveSrvDesc = textureSrvDesc(reactiveMask, desc.reactiveMask.format);
            impl_->device->CreateShaderResourceView(reactiveMask, &reactiveSrvDesc, handle);
        } else {
            const D3D12_SHADER_RESOURCE_VIEW_DESC reactiveSrvDesc = nullScalarTextureSrvDesc();
            impl_->device->CreateShaderResourceView(nullptr, &reactiveSrvDesc, handle);
        }
        handle.ptr += impl_->descriptorIncrement;
        if (transparencyMask != nullptr) {
            const D3D12_SHADER_RESOURCE_VIEW_DESC transparencySrvDesc = textureSrvDesc(transparencyMask, desc.transparencyComposition.format);
            impl_->device->CreateShaderResourceView(transparencyMask, &transparencySrvDesc, handle);
        } else {
            const D3D12_SHADER_RESOURCE_VIEW_DESC transparencySrvDesc = nullScalarTextureSrvDesc();
            impl_->device->CreateShaderResourceView(nullptr, &transparencySrvDesc, handle);
        }
        handle.ptr += impl_->descriptorIncrement;
        const D3D12_UNORDERED_ACCESS_VIEW_DESC reprojectedUavDesc = modelSidecarTextureUavDesc();
        impl_->device->CreateUnorderedAccessView(reprojectedColor, nullptr, &reprojectedUavDesc, handle);
    }

    std::vector<D3D12_RESOURCE_BARRIER> transitions;
    const D3D12_RESOURCE_STATES depthBefore = dx12StateFromFfxState(desc.depth.nativeState);
    const D3D12_RESOURCE_STATES motionBefore = dx12StateFromFfxState(desc.motionVectors.nativeState);
    const D3D12_RESOURCE_STATES exposureBefore = internalFrameExposure ?
        tokenBufferState(impl_->internalTextureStates, exposure) :
        dx12StateFromFfxState(desc.exposure.nativeState);
    const D3D12_RESOURCE_STATES reactiveBefore = dx12StateFromFfxState(desc.reactiveMask.nativeState);
    const D3D12_RESOURCE_STATES transparencyBefore = dx12StateFromFfxState(desc.transparencyComposition.nativeState);
    const D3D12_RESOURCE_STATES historyReadBefore = tokenBufferState(impl_->internalTextureStates, historyRead);
    const D3D12_RESOURCE_STATES recurrentReadBefore = tokenBufferState(impl_->internalTextureStates, recurrentRead);
    const D3D12_RESOURCE_STATES reprojectedBefore = tokenBufferState(impl_->internalTextureStates, reprojectedColor);
    const D3D12_RESOURCE_STATES tokenBefore = tokenBufferState(impl_->tokenBufferStates, tokenBuffer);
    traceTransition("export.color", color, colorBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("export.depth", depth, depthBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("export.motionVectors", motionVectors, motionBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (exposure != nullptr) {
        traceTransition("export.exposure", exposure, exposureBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (reactiveMask != nullptr) {
        traceTransition("export.reactiveMask", reactiveMask, reactiveBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (transparencyMask != nullptr) {
        traceTransition("export.transparencyMask", transparencyMask, transparencyBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    traceTransition("export.historyRead", historyRead, historyReadBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("export.recurrentRead", recurrentRead, recurrentReadBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("export.reprojectedColor", reprojectedColor, reprojectedBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    traceTransition("export.destinationTokens", tokenBuffer, tokenBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    appendTransition(transitions, color, colorBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, depth, depthBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, motionVectors, motionBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (exposure != nullptr) {
        appendTransition(transitions, exposure, exposureBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (reactiveMask != nullptr) {
        appendTransition(transitions, reactiveMask, reactiveBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (transparencyMask != nullptr) {
        appendTransition(transitions, transparencyMask, transparencyBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    appendTransition(transitions, historyRead, historyReadBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, recurrentRead, recurrentReadBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, reprojectedColor, reprojectedBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    appendTransition(
        transitions,
        tokenBuffer,
        tokenBefore,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!transitions.empty()) {
        traceLine("EXPORT ResourceBarrier count=" + std::to_string(transitions.size()));
        commandList->ResourceBarrier(static_cast<UINT>(transitions.size()), transitions.data());
    }
    impl_->tokenBufferStates[tokenBuffer] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    impl_->internalTextureStates[color] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->internalTextureStates[exposure] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->internalTextureStates[historyRead] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->internalTextureStates[recurrentRead] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->internalTextureStates[reprojectedColor] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ID3D12DescriptorHeap* heaps[] = {entry.heap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(impl_->exportRootSignature.Get());
    commandList->SetPipelineState(impl_->exportPso.Get());

    TokenBandConstants constants = makeConstants(desc);
    if (!impl_->historyInitialized) {
        constants.resetHistory = 1u;
    }
    commandList->SetComputeRoot32BitConstants(0, sizeof(TokenBandConstants) / sizeof(std::uint32_t), &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, entry.heap->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRootUnorderedAccessView(2, desc.destination.gpuVirtualAddress);
    D3D12_GPU_DESCRIPTOR_HANDLE reprojectedUavTable = entry.heap->GetGPUDescriptorHandleForHeapStart();
    reprojectedUavTable.ptr += static_cast<SIZE_T>(impl_->descriptorIncrement) * 8u;
    commandList->SetComputeRootDescriptorTable(3, reprojectedUavTable);

    const std::uint32_t groupCountX = dispatchGroupCount(constants.tokenPaddedWidth, 16);
    const std::uint32_t groupCountY = dispatchGroupCount(constants.inputRowCount, 4);
    traceLine("EXPORT Dispatch groups=" + std::to_string(groupCountX) + "x" + std::to_string(groupCountY) +
        " tokenLogicalWidth=" + std::to_string(constants.tokenLogicalWidth) +
        " tokenPaddedWidth=" + std::to_string(constants.tokenPaddedWidth) +
        " rowPitchBytes=" + std::to_string(constants.tokenRowPitchBytes) +
        " inputRows=" + std::to_string(constants.inputRowCount));
    commandList->Dispatch(groupCountX, groupCountY, 1);
    D3D12_RESOURCE_BARRIER uavBarriers[2]{};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uavBarriers[0].UAV.pResource = tokenBuffer;
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uavBarriers[1].UAV.pResource = reprojectedColor;
    commandList->ResourceBarrier(2, uavBarriers);

    if (desc.restoreInputStatesAfterExport) {
        std::vector<D3D12_RESOURCE_BARRIER> restoreTransitions;
        if (captureThisBand) {
            appendTransition(restoreTransitions, gameColor, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dx12StateFromFfxState(desc.color.nativeState));
        }
        appendTransition(restoreTransitions, depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dx12StateFromFfxState(desc.depth.nativeState));
        appendTransition(
            restoreTransitions,
            motionVectors,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            dx12StateFromFfxState(desc.motionVectors.nativeState));
        if (captureThisBand && externalExposure != nullptr) {
            appendTransition(restoreTransitions, externalExposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dx12StateFromFfxState(desc.exposure.nativeState));
        }
        if (reactiveMask != nullptr) {
            appendTransition(restoreTransitions, reactiveMask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dx12StateFromFfxState(desc.reactiveMask.nativeState));
        }
        if (transparencyMask != nullptr) {
            appendTransition(restoreTransitions, transparencyMask, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dx12StateFromFfxState(desc.transparencyComposition.nativeState));
        }
        if (!restoreTransitions.empty()) {
            traceLine("EXPORT restore ResourceBarrier count=" + std::to_string(restoreTransitions.size()));
            commandList->ResourceBarrier(static_cast<UINT>(restoreTransitions.size()), restoreTransitions.data());
        }
    }

    traceLine("END recordExport");
    return Status::ok();
#else
    (void)context;
    (void)desc;
    return Status::notImplemented("FSR4 DX12 export shader recording is only available in Windows builds");
#endif
}

Status Fsr4Dx12TokenShaderRecorder::recordImport(backends::Dx12RecordContext& context, const ImportPassDesc& desc)
{
#ifdef _WIN32
    if (impl_ == nullptr || !impl_->importRootSignature || !impl_->importPso) {
        return Status::invalidArgument("FSR4 DX12 import shader recorder is not initialized");
    }
    if (desc.roundTripCopyVerify.enabled && (!impl_->roundTripCopyVerifyRootSignature || !impl_->roundTripCopyVerifyPso)) {
        return Status::invalidArgument("FSR4 DX12 frame-conveyor copy verifier is not initialized");
    }

    ID3D12GraphicsCommandList* commandList = dx12CommandList(context);
    ID3D12Resource* tokenBuffer = dx12Resource(desc.source.nativeGraphicsBuffer);
    ID3D12Resource* output = dx12Resource(desc.output.resource);
    if (commandList == nullptr || tokenBuffer == nullptr || output == nullptr) {
        return Status::invalidArgument("FSR4 DX12 import recording requires egress tokens and output resources");
    }

    traceLine("BEGIN recordImport commandList=" + pointerString(commandList));
    Status copyVerifyStatus = recordRoundTripCopyVerify(*impl_, commandList, desc);
    if (!copyVerifyStatus.succeeded()) {
        return copyVerifyStatus;
    }

    Status validationStatus = validateRootUavBuffer("import.sourceTokens", tokenBuffer);
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }
    validationStatus = validateByteAddressBufferRange(
        "import.sourceTokens",
        desc.source.byteSize,
        desc.sourceByteOffset,
        tokenRowsByteCount(desc.sourceBoundary, desc.band.egressRowCount));
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }
    validationStatus = validateTextureUav("import.output", output, desc.output.format);
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }

    const DisplaySize historySize = desc.frameConstants.displaySize;
    Status historyStatus = ensureInternalHistoryResources(*impl_, historySize);
    if (!historyStatus.succeeded()) {
        return historyStatus;
    }
    const std::uint32_t asyncFrameSlot = static_cast<std::uint32_t>(desc.frameIndex % kAsyncBookendFrameSlots);
    Status asyncFrameStatus = ensureAsyncBookendFrameResources(*impl_, asyncFrameSlot, desc.frameConstants.renderSize, historySize);
    if (!asyncFrameStatus.succeeded()) {
        return asyncFrameStatus;
    }
    AsyncBookendFrameResources& asyncFrame = impl_->asyncFrames[asyncFrameSlot];

    if (desc.band.firstBandInFrame) {
        impl_->historyWriteIndex = 1u - impl_->historyReadIndex;
    }

    ID3D12Resource* color = asyncFrame.modelInputColor.Get();
    ID3D12Resource* exposure = asyncFrame.exposureTexture.Get();
    ID3D12Resource* reprojectedColor = asyncFrame.reprojectedColor.Get();
    ID3D12Resource* historyWrite = impl_->historyColor[impl_->historyWriteIndex].Get();
    ID3D12Resource* recurrentWrite = impl_->recurrentColor[impl_->historyWriteIndex].Get();
    const bool runRcas = desc.frameOptions.sharpeningRequested;
    ID3D12Resource* importOutput = runRcas ? impl_->rcasInput.Get() : output;

    validationStatus = validateTextureSrv("import.modelInputColor", color, modelSidecarSurfaceFormat());
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }
    validationStatus = validateTextureSrv("import.frameExposure", exposure, SurfaceFormat::R32Float);
    if (!validationStatus.succeeded()) {
        return validationStatus;
    }

    const std::uintptr_t heapKey = reinterpret_cast<std::uintptr_t>(importOutput) ^
        (reinterpret_cast<std::uintptr_t>(color) << 1u) ^
        (reinterpret_cast<std::uintptr_t>(exposure) << 2u) ^
        (reinterpret_cast<std::uintptr_t>(reprojectedColor) << 3u) ^
        (reinterpret_cast<std::uintptr_t>(historyWrite) << 4u) ^
        (reinterpret_cast<std::uintptr_t>(recurrentWrite) << 5u);
    DescriptorHeapEntry& entry = impl_->importDescriptorHeaps[heapKey];
    if (!entry.heap) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 6;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        traceLine("IMPORT create SRV/UAV descriptor heap");
        const HRESULT hr = impl_->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&entry.heap));
        if (FAILED(hr) || !entry.heap) {
            return hresultStatus("ID3D12Device::CreateDescriptorHeap(import descriptors)", hr);
        }
        entry.key = heapKey;

        D3D12_CPU_DESCRIPTOR_HANDLE handle = entry.heap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_SHADER_RESOURCE_VIEW_DESC colorSrvDesc = modelSidecarTextureSrvDesc();
        impl_->device->CreateShaderResourceView(color, &colorSrvDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        const D3D12_SHADER_RESOURCE_VIEW_DESC exposureSrvDesc = r32TextureSrvDesc();
        impl_->device->CreateShaderResourceView(exposure, &exposureSrvDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        const D3D12_SHADER_RESOURCE_VIEW_DESC reprojectedSrvDesc = modelSidecarTextureSrvDesc();
        impl_->device->CreateShaderResourceView(reprojectedColor, &reprojectedSrvDesc, handle);
        handle.ptr += impl_->descriptorIncrement;

        const D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc = runRcas
            ? textureUavDesc(importOutput, SurfaceFormat::Rgba16Float)
            : textureUavDesc(importOutput, desc.output.format);
        impl_->device->CreateUnorderedAccessView(importOutput, nullptr, &outputUavDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        D3D12_UNORDERED_ACCESS_VIEW_DESC historyUavDesc{};
        historyUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        historyUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        historyUavDesc.Texture2D.MipSlice = 0;
        historyUavDesc.Texture2D.PlaneSlice = 0;
        impl_->device->CreateUnorderedAccessView(historyWrite, nullptr, &historyUavDesc, handle);
        handle.ptr += impl_->descriptorIncrement;
        impl_->device->CreateUnorderedAccessView(recurrentWrite, nullptr, &historyUavDesc, handle);
    }

    std::vector<D3D12_RESOURCE_BARRIER> transitions;
    const D3D12_RESOURCE_STATES tokenBefore = tokenBufferState(impl_->tokenBufferStates, tokenBuffer);
    const D3D12_RESOURCE_STATES colorBefore = tokenBufferState(impl_->internalTextureStates, color);
    const D3D12_RESOURCE_STATES exposureBefore = tokenBufferState(impl_->internalTextureStates, exposure);
    const D3D12_RESOURCE_STATES outputBefore = dx12StateFromFfxState(desc.output.nativeState);
    const D3D12_RESOURCE_STATES importOutputBefore = runRcas
        ? tokenBufferState(impl_->internalTextureStates, importOutput)
        : outputBefore;
    const D3D12_RESOURCE_STATES reprojectedBefore = tokenBufferState(impl_->internalTextureStates, reprojectedColor);
    const D3D12_RESOURCE_STATES historyWriteBefore = tokenBufferState(impl_->internalTextureStates, historyWrite);
    const D3D12_RESOURCE_STATES recurrentWriteBefore = tokenBufferState(impl_->internalTextureStates, recurrentWrite);
    traceTransition("import.sourceTokens", tokenBuffer, tokenBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("import.modelInputColor", color, colorBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("import.frameExposure", exposure, exposureBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition(runRcas ? "import.rcasInput" : "import.output", importOutput, importOutputBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    traceTransition("import.reprojectedColor", reprojectedColor, reprojectedBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    traceTransition("import.historyWrite", historyWrite, historyWriteBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    traceTransition("import.recurrentWrite", recurrentWrite, recurrentWriteBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    appendTransition(transitions, tokenBuffer, tokenBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, color, colorBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, exposure, exposureBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, importOutput, importOutputBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    appendTransition(transitions, reprojectedColor, reprojectedBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    appendTransition(transitions, historyWrite, historyWriteBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    appendTransition(transitions, recurrentWrite, recurrentWriteBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!transitions.empty()) {
        traceLine("IMPORT ResourceBarrier count=" + std::to_string(transitions.size()));
        commandList->ResourceBarrier(static_cast<UINT>(transitions.size()), transitions.data());
    }
    impl_->tokenBufferStates[tokenBuffer] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->internalTextureStates[color] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->internalTextureStates[exposure] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    impl_->internalTextureStates[reprojectedColor] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (runRcas) {
        impl_->internalTextureStates[importOutput] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    impl_->internalTextureStates[historyWrite] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    impl_->internalTextureStates[recurrentWrite] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    ID3D12DescriptorHeap* heaps[] = {entry.heap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(impl_->importRootSignature.Get());
    commandList->SetPipelineState(impl_->importPso.Get());

    const TokenBandConstants constants = makeConstants(desc);
    commandList->SetComputeRoot32BitConstants(0, sizeof(TokenBandConstants) / sizeof(std::uint32_t), &constants, 0);
    commandList->SetComputeRootShaderResourceView(1, desc.source.gpuVirtualAddress);
    commandList->SetComputeRootDescriptorTable(2, entry.heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_GPU_DESCRIPTOR_HANDLE uavTable = entry.heap->GetGPUDescriptorHandleForHeapStart();
    uavTable.ptr += static_cast<SIZE_T>(impl_->descriptorIncrement) * 3u;
    commandList->SetComputeRootDescriptorTable(3, uavTable);

    const std::uint32_t groupCountX = dispatchGroupCount(constants.tokenLogicalWidth, 16);
    const std::uint32_t groupCountY = dispatchGroupCount(constants.outputRowCount, 4);
    traceLine("IMPORT Dispatch groups=" + std::to_string(groupCountX) + "x" + std::to_string(groupCountY) +
        " tokenLogicalWidth=" + std::to_string(constants.tokenLogicalWidth) +
        " rowPitchBytes=" + std::to_string(constants.tokenRowPitchBytes) +
        " outputRows=" + std::to_string(constants.outputRowCount) +
        " egressRows=" + std::to_string(constants.egressRowCount));
    commandList->Dispatch(groupCountX, groupCountY, 1);

    D3D12_RESOURCE_BARRIER uavBarriers[3]{};
    UINT uavBarrierCount = 0;
    auto appendUavBarrier = [&](ID3D12Resource* resource) {
        if (resource == nullptr) {
            return;
        }
        D3D12_RESOURCE_BARRIER& barrier = uavBarriers[uavBarrierCount++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = resource;
    };

    // When RCAS is enabled, importOutput is transitioned from UAV to SRV below;
    // that transition provides the required ordering for the RCAS read.  Keep a
    // UAV barrier for the direct-output path, where the resource remains a UAV.
    if (!runRcas) {
        appendUavBarrier(importOutput);
    }
    appendUavBarrier(historyWrite);
    appendUavBarrier(recurrentWrite);
    if (uavBarrierCount != 0u) {
        commandList->ResourceBarrier(uavBarrierCount, uavBarriers);
    }

    if (runRcas && desc.band.lastBandInFrame) {
        const std::uintptr_t rcasHeapKey = reinterpret_cast<std::uintptr_t>(impl_->rcasInput.Get()) ^
            (reinterpret_cast<std::uintptr_t>(exposure) << 1u) ^
            (reinterpret_cast<std::uintptr_t>(output) << 2u);
        DescriptorHeapEntry& rcasEntry = impl_->rcasDescriptorHeaps[rcasHeapKey];
        if (!rcasEntry.heap) {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.NumDescriptors = 3;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            traceLine("RCAS create SRV/UAV descriptor heap");
            const HRESULT hr = impl_->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rcasEntry.heap));
            if (FAILED(hr) || !rcasEntry.heap) {
                return hresultStatus("ID3D12Device::CreateDescriptorHeap(RCAS descriptors)", hr);
            }
            rcasEntry.key = rcasHeapKey;

            D3D12_CPU_DESCRIPTOR_HANDLE handle = rcasEntry.heap->GetCPUDescriptorHandleForHeapStart();
            const D3D12_SHADER_RESOURCE_VIEW_DESC rcasInputSrvDesc = rgba16TextureSrvDesc();
            impl_->device->CreateShaderResourceView(impl_->rcasInput.Get(), &rcasInputSrvDesc, handle);
            handle.ptr += impl_->descriptorIncrement;
            const D3D12_SHADER_RESOURCE_VIEW_DESC exposureSrvDesc = r32TextureSrvDesc();
            impl_->device->CreateShaderResourceView(exposure, &exposureSrvDesc, handle);
            handle.ptr += impl_->descriptorIncrement;
            const D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc = textureUavDesc(output, desc.output.format);
            impl_->device->CreateUnorderedAccessView(output, nullptr, &outputUavDesc, handle);
        }

        D3D12_RESOURCE_BARRIER rcasTransitions[2]{};
        UINT rcasTransitionCount = 0;
        auto appendRcasTransition = [&](
            ID3D12Resource* resource,
            const D3D12_RESOURCE_STATES before,
            const D3D12_RESOURCE_STATES after) {
            if (resource == nullptr || before == after) {
                return;
            }
            D3D12_RESOURCE_BARRIER& barrier = rcasTransitions[rcasTransitionCount++];
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
        };

        const D3D12_RESOURCE_STATES rcasInputBefore = tokenBufferState(impl_->internalTextureStates, impl_->rcasInput.Get());
        traceTransition("rcas.input", impl_->rcasInput.Get(), rcasInputBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        traceTransition("rcas.output", output, outputBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        appendRcasTransition(impl_->rcasInput.Get(), rcasInputBefore, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        appendRcasTransition(output, outputBefore, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (rcasTransitionCount != 0u) {
            traceLine("RCAS ResourceBarrier count=" + std::to_string(rcasTransitionCount));
            commandList->ResourceBarrier(rcasTransitionCount, rcasTransitions);
        }
        impl_->internalTextureStates[impl_->rcasInput.Get()] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        ID3D12DescriptorHeap* rcasHeaps[] = {rcasEntry.heap.Get()};
        commandList->SetDescriptorHeaps(1, rcasHeaps);
        commandList->SetComputeRootSignature(impl_->rcasRootSignature.Get());
        commandList->SetPipelineState(impl_->rcasPso.Get());
        commandList->SetComputeRoot32BitConstants(0, sizeof(TokenBandConstants) / sizeof(std::uint32_t), &constants, 0);
        commandList->SetComputeRootDescriptorTable(1, rcasEntry.heap->GetGPUDescriptorHandleForHeapStart());
        D3D12_GPU_DESCRIPTOR_HANDLE rcasUavTable = rcasEntry.heap->GetGPUDescriptorHandleForHeapStart();
        rcasUavTable.ptr += static_cast<SIZE_T>(impl_->descriptorIncrement) * 2u;
        commandList->SetComputeRootDescriptorTable(2, rcasUavTable);

        const std::uint32_t rcasGroupsX = dispatchGroupCount(constants.displayWidth, 16);
        const std::uint32_t rcasGroupsY = dispatchGroupCount(constants.displayHeight, 8);
        traceLine("RCAS Dispatch groups=" + std::to_string(rcasGroupsX) + "x" + std::to_string(rcasGroupsY));
        commandList->Dispatch(rcasGroupsX, rcasGroupsY, 1);

        D3D12_RESOURCE_BARRIER outputBarrier{};
        outputBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        outputBarrier.UAV.pResource = output;
        commandList->ResourceBarrier(1, &outputBarrier);
    }

    if (desc.band.lastBandInFrame) {
        impl_->historyReadIndex = impl_->historyWriteIndex;
        impl_->historyInitialized = true;
    }
    traceLine("END recordImport");
    return Status::ok();
#else
    (void)context;
    (void)desc;
    return Status::notImplemented("FSR4 DX12 import shader recording is only available in Windows builds");
#endif
}

Status Fsr4Dx12TokenShaderRecorder::recordExportCallback(
    backends::Dx12RecordContext& context,
    const ExportPassDesc& desc,
    void* userData)
{
    if (userData == nullptr) {
        return Status::invalidArgument("FSR4 DX12 export callback requires shader recorder user data");
    }
    return static_cast<Fsr4Dx12TokenShaderRecorder*>(userData)->recordExport(context, desc);
}

Status Fsr4Dx12TokenShaderRecorder::recordImportCallback(
    backends::Dx12RecordContext& context,
    const ImportPassDesc& desc,
    void* userData)
{
    if (userData == nullptr) {
        return Status::invalidArgument("FSR4 DX12 import callback requires shader recorder user data");
    }
    return static_cast<Fsr4Dx12TokenShaderRecorder*>(userData)->recordImport(context, desc);
}


} // namespace apusr::frontends
