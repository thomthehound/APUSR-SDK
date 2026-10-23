// APUSR DX12 FrameGen backend derived from the FidelityFX SDK DX12 backend.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "apusr_framegen_backend_interface.h"
#include "ffx_util.h"
#include "ffx_assert.h"

#include "apusr_framegen_backend_dx12.h"
#include "apusr_framegen_dx12_helpers.h"
#include <apusr/apusr_profiler.h>


#include <algorithm>
#include <cstring> // memcpy, memset
#include <cwchar> // wcscpy_s
#include <memoryapi.h> // for VirtualAlloc
#include <tuple> // std::ignore
#include <limits> // std::numeric_limits

#define APUSR_FRAMEGEN_RETURN_ON_ERROR(condition, error) \
    do { if (!(condition)) return (error); } while (false)

#ifndef FFX_DX12_ENABLE_GPU_MARKERS
#define FFX_DX12_ENABLE_GPU_MARKERS 0
#endif

static_assert(sizeof(ApusrFramegenGpuJob) <= 1024, "Queued APUSR FrameGen GPU jobs must stay compact.");

#if FFX_DX12_ENABLE_GPU_MARKERS
// PIX instrumentation is only enabled if one of the preprocessor symbols USE_PIX, DBG, _DEBUG, PROFILE, or PROFILE_BUILD is defined.
// ref: https://devblogs.microsoft.com/pix/winpixeventruntime/
#ifndef USE_PIX
#define USE_PIX  // Should enable it at anytime, as we already have a runtime switch for this purpose
#endif           // #ifndef USE_PIX

#if defined(__has_include)
#if __has_include(<pix3.h>)
#include <pix3.h>
#else
#include "pix3_stub.h"  // Fallback shim keeps cross-platform builds self-contained.
#endif
#else
#include "pix3_stub.h"  // Older toolchains skip PIX instead of hard failing on __has_include.
#endif

#ifdef __clang__
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wpointer-bool-conversion"
#endif

static bool s_PIXDLLLoaded = false;

typedef void(WINAPI* BeginEventOnCommandList)(ID3D12GraphicsCommandList* commandList, UINT64 color, _In_ PCSTR formatString);
typedef void(WINAPI* EndEventOnCommandList)(ID3D12GraphicsCommandList* commandList);

BeginEventOnCommandList pixBeginEventOnCommandList;
EndEventOnCommandList   pixEndEventOnCommandList;

#endif // FFX_DX12_ENABLE_GPU_MARKERS

// DX12 prototypes for functions in the backend interface
ApusrFramegenVersion GetSDKVersionDX12(ApusrFramegenBackendInterface* backendInterface);
ApusrFramegenResult GetEffectGpuMemoryUsageDX12(ApusrFramegenBackendInterface* backendInterface, uint32_t effectContextId, ApusrFramegenMemoryUsage* outVramUsage);
ApusrFramegenResult CreateBackendContextDX12(ApusrFramegenBackendInterface* backendInterface, uint32_t* effectContextId);
ApusrFramegenResult GetDeviceCapabilitiesDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenDeviceCapabilities* deviceCapabilities);
ApusrFramegenResult DestroyBackendContextDX12(ApusrFramegenBackendInterface* backendInterface, uint32_t effectContextId);
ApusrFramegenResult CreateResourceDX12(ApusrFramegenBackendInterface* backendInterface, const ApusrFramegenCreateResourceDescription* desc, uint32_t effectContextId, ApusrFramegenResource* outTexture);
ApusrFramegenResult DestroyResourceDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenResource resource, uint32_t effectContextId);
ApusrFramegenResult RegisterResourceDX12(ApusrFramegenBackendInterface* backendInterface, const ApusrFramegenExternalResource* inResource, uint32_t effectContextId, ApusrFramegenResource* outResourceInternal);
ApusrFramegenExternalResource GetResourceDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenResource resource);
ApusrFramegenResult UnregisterResourcesDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenCommandList commandList, uint32_t effectContextId);
ApusrFramegenResourceDescription GetResourceDescriptorDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenResource resource);
ApusrFramegenResult StageConstantBufferDataDX12(ApusrFramegenBackendInterface* backendInterface, void* data, uint32_t size, ApusrFramegenConstantBuffer* constantBuffer);
ApusrFramegenResult CreatePipelineDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenShaderBlob* pShaderBlob, const ApusrFramegenPipelineDescription*  desc, uint32_t effectContextId, ApusrFramegenPipeline* outPass);
ApusrFramegenResult DestroyPipelineDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenPipeline* pipeline, uint32_t effectContextId);
ApusrFramegenResult ScheduleGpuJobDX12(ApusrFramegenBackendInterface* backendInterface, const ApusrFramegenGpuJob* job);
ApusrFramegenResult ExecuteGpuJobsDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenCommandList commandList, uint32_t effectContextId);


typedef struct BackendContext_DX12 {

    // store for resources and resourceViews
    typedef struct Resource
    {
#ifdef _DEBUG
        wchar_t                 resourceName[64] = {};
#endif
        ID3D12Resource*         resourcePtr;
        ApusrFramegenResourceDescription  resourceDescription;
        ApusrFramegenResourceState       initialState;
        ApusrFramegenResourceState       currentState;
        uint32_t                srvDescIndex;
        uint32_t                uavDescIndex;
        uint32_t                uavDescCount;
        uint64_t                allocationSize;
        bool                    allocationAccounted;
    } Resource;

    uint32_t refCount;
    uint32_t maxEffectContexts;

    ID3D12Device*           device = nullptr;

    ApusrFramegenGpuJob*   pGpuJobs;
    uint32_t                gpuJobCount;

    ID3D12DescriptorHeap*   descHeapSrvCpu;
    ID3D12DescriptorHeap*   descHeapUavCpu;
    ID3D12DescriptorHeap*   descHeapUavGpu;

    uint32_t                     descriptorSizeCbvSrvUav;
    D3D12_CPU_DESCRIPTOR_HANDLE  descHeapSrvCpuStart;
    D3D12_CPU_DESCRIPTOR_HANDLE  descHeapUavCpuStart;
    D3D12_CPU_DESCRIPTOR_HANDLE  descHeapUavGpuCpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE  descHeapUavGpuStart;
    D3D12_CPU_DESCRIPTOR_HANDLE  descRingCpuStart;
    D3D12_GPU_DESCRIPTOR_HANDLE  descRingGpuStart;

    uint32_t                descRingBufferSize;
    uint32_t                descRingBufferBase;
    ID3D12DescriptorHeap*   descRingBuffer;

    uint8_t*                pStagingRingBuffer;
    uint32_t                stagingRingBufferBase = 0;

    D3D12_RESOURCE_BARRIER  barriers[APUSR_FRAMEGEN_MAX_BARRIERS];
    uint32_t                barrierCount;

    apusr::profile::StreamHandle profileStream{};
    apusr::profile::SampleGate profileSampleGate{};
    std::uint64_t profileFrameSequence = 0u;

    typedef struct EffectContext {

        // Resource allocation
        uint32_t            nextStaticResource;
        uint32_t            nextDynamicResource;

        // UAV offsets
        uint32_t            nextStaticUavDescriptor;
        uint32_t            nextDynamicUavDescriptor;



        // Usage
        bool                active;

        // VRAM usage
        ApusrFramegenMemoryUsage vramUsage;

        // Sampling configuration is tracked per logical effect context. Normal
        // FrameGen execution alternates among stable contexts, so comparing the
        // current context against the immediately previous context would turn
        // sampled mode into near-continuous capture.
        std::uint64_t profileConfigurationHash;
        bool profileHaveConfiguration;

    } EffectContext;

    // Resource holder
    Resource*                   pResources;
    EffectContext*              pEffectContexts;

    // Allocation defaults
    ApusrFramegenConstantAllocation FallbackConstantAllocator(void* data, uint64_t dataSize);
    void*                       constantBufferMem;
    ID3D12Resource*             constantBufferResource;
    uint32_t                    constantBufferSize;
    uint32_t                    constantBufferOffset;

} BackendContext_DX12;

static void releaseBackendDx12Infrastructure(BackendContext_DX12* backendContext) noexcept
{
    if (backendContext->constantBufferMem && backendContext->constantBufferResource) {
        backendContext->constantBufferResource->Unmap(0, nullptr);
        backendContext->constantBufferMem = nullptr;
    }
    if (backendContext->constantBufferResource) {
        backendContext->constantBufferResource->Release();
        backendContext->constantBufferResource = nullptr;
    }
    backendContext->constantBufferOffset = 0;
    backendContext->constantBufferSize = 0;

    if (backendContext->descHeapSrvCpu) {
        backendContext->descHeapSrvCpu->Release();
        backendContext->descHeapSrvCpu = nullptr;
    }
    if (backendContext->descHeapUavCpu) {
        backendContext->descHeapUavCpu->Release();
        backendContext->descHeapUavCpu = nullptr;
    }
    if (backendContext->descHeapUavGpu) {
        backendContext->descHeapUavGpu->Release();
        backendContext->descHeapUavGpu = nullptr;
    }
    if (backendContext->descRingBuffer) {
        backendContext->descRingBuffer->Release();
        backendContext->descRingBuffer = nullptr;
    }
    if (backendContext->device) {
        backendContext->device->Release();
        backendContext->device = nullptr;
    }
}

static size_t scratchMemorySizeDx12(size_t maxContexts)
{
    uint32_t resourceArraySize          = FFX_ALIGN_UP(maxContexts * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT * sizeof(BackendContext_DX12::Resource), sizeof(uint64_t));
    uint32_t contextArraySize           = FFX_ALIGN_UP(maxContexts * sizeof(BackendContext_DX12::EffectContext), sizeof(uint32_t));
    uint32_t stagingRingBufferArraySize = FFX_ALIGN_UP(maxContexts * APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE, sizeof(uint32_t));
    uint32_t gpuJobDescArraySize        = FFX_ALIGN_UP(maxContexts * APUSR_FRAMEGEN_MAX_GPU_JOBS * sizeof(ApusrFramegenGpuJob), sizeof(uint32_t));

    return FFX_ALIGN_UP(sizeof(BackendContext_DX12) + resourceArraySize + contextArraySize + stagingRingBufferArraySize + gpuJobDescArraySize, sizeof(uint64_t));
}

// populate interface with DX12 pointers.
static ApusrFramegenResult initBackendDx12(
    ApusrFramegenBackendInterface* backendInterface,
    ID3D12Device* device,
    void* scratchBuffer,
    size_t scratchBufferSize,
    size_t maxContexts) {


    APUSR_FRAMEGEN_RETURN_ON_ERROR(
        backendInterface,
        APUSR_FRAMEGEN_ERROR_INVALID_POINTER);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(
        scratchBuffer,
        APUSR_FRAMEGEN_ERROR_INVALID_POINTER);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(
        scratchBufferSize >= scratchMemorySizeDx12(maxContexts),
        APUSR_FRAMEGEN_ERROR_INSUFFICIENT_MEMORY);

    backendInterface->getVersion = GetSDKVersionDX12;
    backendInterface->getGpuMemoryUsage = GetEffectGpuMemoryUsageDX12;
    backendInterface->createContext = CreateBackendContextDX12;
    backendInterface->getDeviceCapabilities = GetDeviceCapabilitiesDX12;
    backendInterface->destroyContext = DestroyBackendContextDX12;
    backendInterface->createResource = CreateResourceDX12;
    backendInterface->destroyResource = DestroyResourceDX12;
    backendInterface->getResource = GetResourceDX12;
    backendInterface->registerResource = RegisterResourceDX12;
    backendInterface->unregisterResources = UnregisterResourcesDX12;
    backendInterface->getResourceDescription = GetResourceDescriptorDX12;
    backendInterface->stageConstants = StageConstantBufferDataDX12;
    backendInterface->createPipeline = CreatePipelineDX12;
    backendInterface->destroyPipeline = DestroyPipelineDX12;
    backendInterface->scheduleJob = ScheduleGpuJobDX12;
    backendInterface->executeJobs = ExecuteGpuJobsDX12;

    // Memory assignments
    backendInterface->scratchBuffer = scratchBuffer;
    backendInterface->scratchBufferSize = scratchBufferSize;

    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;

    APUSR_FRAMEGEN_RETURN_ON_ERROR(
        !backendContext->refCount,
        APUSR_FRAMEGEN_ERROR_BACKEND_API);

    // Clear everything out
    memset(backendContext, 0, sizeof(*backendContext));

    // Set the device
    backendInterface->device = device;


    // Assign the max number of contexts we'll be using
    backendContext->maxEffectContexts = (uint32_t)maxContexts;

    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenCommandList apusrFramegenCommandListDX12(ID3D12CommandList* cmdList)
{
    FFX_ASSERT(NULL != cmdList);
    return reinterpret_cast<ApusrFramegenCommandList>(cmdList);
}


// Wrap a DX12 resource in the backend-native resource vocabulary.
ApusrFramegenExternalResource apusrFramegenExternalResourceDX12(
    const ID3D12Resource* dx12Resource,
    ApusrFramegenResourceDescription resourceDescription,
    ApusrFramegenResourceState state)
{
    return {
        reinterpret_cast<void*>(const_cast<ID3D12Resource*>(dx12Resource)),
        resourceDescription,
        state,
    };
}

ApusrFramegenResult ffxLoadPixDll(const wchar_t* pixDllPath)
{
#if FFX_DX12_ENABLE_GPU_MARKERS
    // Only do this once
    if (s_PIXDLLLoaded)
        return APUSR_FRAMEGEN_OK;

    HMODULE module = LoadLibraryW(pixDllPath);

    if (!module)
    {
        return APUSR_FRAMEGEN_ERROR_INVALID_PATH;
    }

    // Get handles to PIXBeginEvent and PIXEndEvent
    pixBeginEventOnCommandList = (BeginEventOnCommandList)GetProcAddress(module, "PIXBeginEventOnCommandList");
    pixEndEventOnCommandList   = (EndEventOnCommandList)GetProcAddress(module, "PIXEndEventOnCommandList");
    if (!pixBeginEventOnCommandList || !pixEndEventOnCommandList)
    {
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;
    }

    s_PIXDLLLoaded = true;
#else
    (void)pixDllPath;
#endif // FFX_DX12_ENABLE_GPU_MARKERS

    return APUSR_FRAMEGEN_OK;
}

void TIF(HRESULT result)
{
    if (FAILED(result)) {

        wchar_t errorMessage[256];
        memset(errorMessage, 0, 256);
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, result, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errorMessage, 255, NULL);
        char errA[256];
        size_t returnSize;
        wcstombs_s(&returnSize, errA, 255, errorMessage, 255);
#ifdef _DEBUG
        std::ignore = MessageBoxW(NULL, errorMessage, L"Error", MB_OK);
#endif
        throw 1;
    }
}

ApusrFramegenConstantAllocation BackendContext_DX12::FallbackConstantAllocator(void* data, uint64_t dataSize)
{
    // The job queue, barriers, descriptors, and upload ring share this serialized backend context.
    ApusrFramegenConstantAllocation allocation{};
    if (!constantBufferMem)
    {
        // create dynamic ring buffer for constant uploads
        constantBufferSize = FFX_ALIGN_UP(APUSR_FRAMEGEN_CONSTANT_BUFFER_ENTRY_SIZE, 256) * maxEffectContexts * APUSR_FRAMEGEN_MAX_PASSES_PER_CONTEXT * APUSR_FRAMEGEN_MAX_QUEUED_FRAMES; // Size aligned to 256

        D3D12_RESOURCE_DESC constDesc = {};
        constDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        constDesc.Alignment = 0;
        constDesc.Width = constantBufferSize;
        constDesc.Height = 1;
        constDesc.DepthOrArraySize = 1;
        constDesc.MipLevels = 1;
        constDesc.Format = DXGI_FORMAT_UNKNOWN;
        constDesc.SampleDesc.Count = 1;
        constDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        TIF(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &constDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&constantBufferResource)));
        constantBufferResource->SetName(L"APUSR FrameGen Dynamic Constant Ring");

        // map it
        TIF(constantBufferResource->Map(0, nullptr,
            (void**)&constantBufferMem));
        constantBufferOffset = 0;
    }

    FFX_ASSERT(constantBufferMem);

    uint32_t size = FFX_ALIGN_UP(dataSize, 256);

    // wrap as needed 
    if (constantBufferOffset + size >= constantBufferSize)
        constantBufferOffset = 0;

    void* pBuffer = (void*)((uint8_t*)(constantBufferMem)+constantBufferOffset);
    memcpy(pBuffer, data, (size_t)dataSize);

    D3D12_GPU_VIRTUAL_ADDRESS bufferViewDesc = constantBufferResource->GetGPUVirtualAddress() + constantBufferOffset;

    // update the offset
    constantBufferOffset += size;

    allocation.gpuAddress = static_cast<uint64_t>(bufferViewDesc);

    return allocation;
}

// fix up format in case resource passed for UAV cannot be mapped
static DXGI_FORMAT convertFormatUav(DXGI_FORMAT format)
{
    switch (format) 
    {
        // Handle Depth
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

        // Handle color: assume FLOAT for 16 and 32 bit channels, else UNORM
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

// fix up format in case resource passed for SRV cannot be mapped
static DXGI_FORMAT convertFormatSrvBackend(DXGI_FORMAT format)
{
    switch (format) 
    {
        // Handle Depth
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

        // Handle color: assume FLOAT for 16 and 32 bit channels, else UNORM
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

D3D12_RESOURCE_STATES apusrFramegenDx12State(ApusrFramegenResourceState state)
{
    switch (state) {

        case APUSR_FRAMEGEN_RESOURCE_STATE_GENERIC_READ:
            return D3D12_RESOURCE_STATE_GENERIC_READ;
        case APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS:
            return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        case APUSR_FRAMEGEN_RESOURCE_STATE_PIXEL_COMPUTE_READ:
            return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ:
            return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case APUSR_FRAMEGEN_RESOURCE_STATE_PIXEL_READ:
            return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        case APUSR_FRAMEGEN_RESOURCE_STATE_COPY_SRC:
            return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST:
            return D3D12_RESOURCE_STATE_COPY_DEST;
        case APUSR_FRAMEGEN_RESOURCE_STATE_INDIRECT_ARGUMENT:
            return D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case APUSR_FRAMEGEN_RESOURCE_STATE_PRESENT:
            return D3D12_RESOURCE_STATE_PRESENT;
        case APUSR_FRAMEGEN_RESOURCE_STATE_COMMON:
            return D3D12_RESOURCE_STATE_COMMON;
        case APUSR_FRAMEGEN_RESOURCE_STATE_RENDER_TARGET:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case APUSR_FRAMEGEN_RESOURCE_STATE_DEPTH_ATTACHMENT:
            return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        default:
            FFX_ASSERT_MESSAGE(false, "Resource state not yet supported");
            return D3D12_RESOURCE_STATE_COMMON;
    }
}

DXGI_FORMAT apusrFramegenDxgiFormat(ApusrFramegenSurfaceFormat surfaceFormat)
{
    switch (surfaceFormat) 
    {
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_TYPELESS):
            return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_UINT):
            return DXGI_FORMAT_R32G32B32A32_UINT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_FLOAT):
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_TYPELESS):
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_FLOAT):
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32_FLOAT):
            return DXGI_FORMAT_R32G32B32_FLOAT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_TYPELESS):
            return DXGI_FORMAT_R32G32_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_FLOAT):
            return DXGI_FORMAT_R32G32_FLOAT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_UINT):
            return DXGI_FORMAT_R32G32_UINT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT):
            return DXGI_FORMAT_R32_UINT;
        case(APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_TYPELESS):
            return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case(APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_UNORM):
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_TYPELESS):
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_UNORM):
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SRGB):
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SNORM):
            return DXGI_FORMAT_R8G8B8A8_SNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_TYPELESS):
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_UNORM):
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_SRGB):
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R11G11B10_FLOAT):
            return DXGI_FORMAT_R11G11B10_FLOAT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_TYPELESS):
            return DXGI_FORMAT_R16G16_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_FLOAT):
            return DXGI_FORMAT_R16G16_FLOAT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_UINT):
            return DXGI_FORMAT_R16G16_UINT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT):
            return DXGI_FORMAT_R16G16_SINT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16_TYPELESS):
            return DXGI_FORMAT_R16_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16_FLOAT):
            return DXGI_FORMAT_R16_FLOAT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UINT):
            return DXGI_FORMAT_R16_UINT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UNORM):
            return DXGI_FORMAT_R16_UNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R16_SNORM):
            return DXGI_FORMAT_R16_SNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8_TYPELESS):
            return DXGI_FORMAT_R8_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UNORM):
            return DXGI_FORMAT_R8_UNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8_SNORM):
            return DXGI_FORMAT_R8_SNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT):
            return DXGI_FORMAT_R8_UINT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UINT):
            return DXGI_FORMAT_R8G8_UINT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_TYPELESS):
            return DXGI_FORMAT_R8G8_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM):
            return DXGI_FORMAT_R8G8_UNORM;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32_TYPELESS):
            return DXGI_FORMAT_R32_TYPELESS;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT):
            return DXGI_FORMAT_R32_FLOAT;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP):
            return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        case (APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN):
            return DXGI_FORMAT_UNKNOWN;

        default:
            FFX_ASSERT_MESSAGE(false, "Format not yet supported");
            return DXGI_FORMAT_UNKNOWN;
    }
}

D3D12_RESOURCE_FLAGS apusrFramegenDx12ResourceFlags(ApusrFramegenResourceUsage flags)
{
    D3D12_RESOURCE_FLAGS dx12ResourceFlags = D3D12_RESOURCE_FLAG_NONE;
    if (flags & APUSR_FRAMEGEN_RESOURCE_USAGE_RENDERTARGET) dx12ResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (flags & APUSR_FRAMEGEN_RESOURCE_USAGE_UAV) dx12ResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return dx12ResourceFlags;
}

ApusrFramegenSurfaceFormat apusrFramegenSurfaceFormatDX12(DXGI_FORMAT format)
{
    switch (format) {

        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_TYPELESS;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32_FLOAT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_UINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32B32A32_UINT;

        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16B16A16_FLOAT;

        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_FLOAT;
        case DXGI_FORMAT_R32G32_UINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32G32_UINT;

        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT;

        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT;

        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT;

        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R10G10B10A2_UNORM;
        
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R11G11B10_FLOAT;

        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SRGB;
        case DXGI_FORMAT_R8G8B8A8_SNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8B8A8_SNORM;

        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_B8G8R8A8_SRGB;

        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_FLOAT;
        case DXGI_FORMAT_R16G16_UINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_UINT;
        case DXGI_FORMAT_R16G16_SINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R16G16_SINT;
        case DXGI_FORMAT_R32_UINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32_UINT;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R32_FLOAT;

        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UINT;
        case DXGI_FORMAT_R8G8_UNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8G8_UNORM;

        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_FLOAT;
        case DXGI_FORMAT_R16_UINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UINT;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_UNORM;
        case DXGI_FORMAT_R16_SNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R16_SNORM;

        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_A8_UNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UNORM;
        case DXGI_FORMAT_R8_SNORM:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8_SNORM;
        case DXGI_FORMAT_R8_UINT:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R8_UINT;

        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_R9G9B9E5_SHAREDEXP;

        case DXGI_FORMAT_UNKNOWN:
            return APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
        default:
            FFX_ASSERT_MESSAGE(false, "Format not yet supported");
            return APUSR_FRAMEGEN_SURFACE_FORMAT_UNKNOWN;
    }
}

bool IsDepthDX12(DXGI_FORMAT format)
{
    return (format == DXGI_FORMAT_D16_UNORM) || 
           (format == DXGI_FORMAT_D32_FLOAT) || 
           (format == DXGI_FORMAT_D24_UNORM_S8_UINT) ||
           (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
}

bool IsStencilDX12(DXGI_FORMAT format)
{
    return (format == DXGI_FORMAT_D24_UNORM_S8_UINT) || (format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
}

ApusrFramegenResourceDescription apusrFramegenNativeResourceDescriptionDX12(const ID3D12Resource* pResource, ApusrFramegenResourceUsage additionalUsages)
{
    ApusrFramegenResourceDescription resourceDescription = {};

    // This is valid
    if (!pResource)
        return resourceDescription;

    if (pResource)
    {
        D3D12_RESOURCE_DESC desc = const_cast<ID3D12Resource*>(pResource)->GetDesc();
        
        if( desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            resourceDescription.flags  = APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE;
            resourceDescription.usage  = APUSR_FRAMEGEN_RESOURCE_USAGE_UAV;
            resourceDescription.size  = (uint32_t)desc.Width;
            resourceDescription.stride = (uint32_t)desc.Height;
            resourceDescription.format = apusrFramegenSurfaceFormatDX12(desc.Format);

            // What should we initialize this to?? No case for this yet
            resourceDescription.depth    = 0;
            resourceDescription.mipCount = 0;

            // Set the type
            resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER;
        }
        else
        {
            // Set flags properly for resource registration
            resourceDescription.flags     = APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE;
           
            // Check for depth use
            resourceDescription.usage     = IsDepthDX12(desc.Format) ? APUSR_FRAMEGEN_RESOURCE_USAGE_DEPTHTARGET : APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY;
            
            if (IsStencilDX12(desc.Format))
                resourceDescription.usage = (ApusrFramegenResourceUsage)(resourceDescription.usage | APUSR_FRAMEGEN_RESOURCE_USAGE_STENCILTARGET);

            // Unordered access use
            if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
                resourceDescription.usage = (ApusrFramegenResourceUsage)(resourceDescription.usage | APUSR_FRAMEGEN_RESOURCE_USAGE_UAV);

            // Resource-specific supplemental use flags
            resourceDescription.usage    = (ApusrFramegenResourceUsage)(resourceDescription.usage | additionalUsages);

            resourceDescription.width    = (uint32_t)desc.Width;
            resourceDescription.height   = (uint32_t)desc.Height;
            resourceDescription.depth    = desc.DepthOrArraySize;
            resourceDescription.mipCount = desc.MipLevels;
            resourceDescription.format   = apusrFramegenSurfaceFormatDX12(desc.Format);

            switch (desc.Dimension)
            {
            case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
                resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE1D;
                break;
            case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
                if ((additionalUsages & APUSR_FRAMEGEN_RESOURCE_USAGE_ARRAYVIEW) != 0)
                    resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D;
                else if (desc.DepthOrArraySize == 1)
                    resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D;
                else if (desc.DepthOrArraySize == 6)
                    resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE_CUBE;
                else
                    resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D;
                break;
            case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
                resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE3D;
                break;
            default:
                FFX_ASSERT_MESSAGE(false, "APUSR FrameGen DX12: unsupported texture dimension.");
                break;
            }
        }
    }

    return resourceDescription;
}


ID3D12Resource* getDX12ResourcePtr(BackendContext_DX12* backendContext, int32_t resourceIndex)
{
    FFX_ASSERT(NULL != backendContext);
    return reinterpret_cast<ID3D12Resource*>(backendContext->pResources[resourceIndex].resourcePtr);
}

void beginMarkerDX12(BackendContext_DX12* backendContext, ID3D12GraphicsCommandList* pCmdList, const wchar_t* label)
{
    FFX_ASSERT(nullptr != backendContext);
    FFX_ASSERT(nullptr != pCmdList);

#if FFX_DX12_ENABLE_GPU_MARKERS
    if (s_PIXDLLLoaded)
    {
        char strLabel[FFX_RESOURCE_NAME_SIZE];
        WideCharToMultiByte(CP_UTF8, 0, label, -1, strLabel, int(std::size(strLabel)), nullptr, nullptr);
        pixBeginEventOnCommandList(pCmdList, 0, strLabel);
    }
#else
    (void)backendContext;
    (void)pCmdList;
    (void)label;
#endif // FFX_DX12_ENABLE_GPU_MARKERS
}

void endMarkerDX12(BackendContext_DX12* backendContext, ID3D12GraphicsCommandList* pCmdList)
{
    FFX_ASSERT(nullptr != backendContext);
    FFX_ASSERT(nullptr != pCmdList);

#if FFX_DX12_ENABLE_GPU_MARKERS
    if (s_PIXDLLLoaded)
    {
        pixEndEventOnCommandList(pCmdList);
    }
#else
    (void)backendContext;
    (void)pCmdList;
#endif // FFX_DX12_ENABLE_GPU_MARKERS
}

void addBarrier(BackendContext_DX12* backendContext, ApusrFramegenResource* resource, ApusrFramegenResourceState newState)
{
    FFX_ASSERT(NULL != backendContext);
    FFX_ASSERT(NULL != resource);

    ID3D12Resource* dx12Resource = getDX12ResourcePtr(backendContext, resource->internalIndex);
    D3D12_RESOURCE_BARRIER* barrier = &backendContext->barriers[backendContext->barrierCount];

    FFX_ASSERT(NULL != dx12Resource);
    FFX_ASSERT(backendContext->barrierCount < APUSR_FRAMEGEN_MAX_BARRIERS);

    ApusrFramegenResourceState* currentState = &backendContext->pResources[resource->internalIndex].currentState;

    if ((*currentState & newState) != newState) {

        *barrier = {};
        barrier->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier->Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier->Transition.pResource = dx12Resource;
        barrier->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier->Transition.StateBefore = apusrFramegenDx12State(*currentState);
        barrier->Transition.StateAfter = apusrFramegenDx12State(newState);

        *currentState = newState;
        ++backendContext->barrierCount;

    }
    else if (newState == APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS) {

        *barrier = {};
        barrier->Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier->Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier->UAV.pResource = dx12Resource;
        ++backendContext->barrierCount;
    }
}

void flushBarriers(BackendContext_DX12* backendContext, ID3D12GraphicsCommandList* dx12CommandList)
{
    FFX_ASSERT(NULL != backendContext);
    FFX_ASSERT(NULL != dx12CommandList);

    if (backendContext->barrierCount > 0) {

        dx12CommandList->ResourceBarrier(backendContext->barrierCount, backendContext->barriers);
        backendContext->barrierCount = 0;
    }
}

static void setDescriptorHeapIfNeeded(
    ID3D12GraphicsCommandList* dx12CommandList,
    ID3D12DescriptorHeap* descriptorHeap,
    ID3D12DescriptorHeap*& boundDescriptorHeap)
{
    FFX_ASSERT(nullptr != dx12CommandList);
    FFX_ASSERT(nullptr != descriptorHeap);

    if (boundDescriptorHeap != descriptorHeap)
    {
        dx12CommandList->SetDescriptorHeaps(1, &descriptorHeap);
        boundDescriptorHeap = descriptorHeap;
    }
}

static void copyDescriptorsBatched(
    ID3D12Device* device,
    uint32_t descriptorCount,
    const D3D12_CPU_DESCRIPTOR_HANDLE* destinations,
    const D3D12_CPU_DESCRIPTOR_HANDLE* sources)
{
    if (descriptorCount == 1)
    {
        device->CopyDescriptorsSimple(1, destinations[0], sources[0], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    else if (descriptorCount > 1)
    {
        device->CopyDescriptors(descriptorCount, destinations, nullptr, descriptorCount, sources, nullptr, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}

//////////////////////////////////////////////////////////////////////////
// DX12 back end implementation

ApusrFramegenVersion GetSDKVersionDX12(ApusrFramegenBackendInterface*)
{
    return APUSR_FRAMEGEN_BACKEND_VERSION;
}

uint64_t GetResourceGpuMemorySizeDX12(ID3D12Resource* resource)
{
    uint64_t      size = 0;
    D3D12_RESOURCE_ALLOCATION_INFO allocInfo = {};
    if (resource)
    {
        D3D12_RESOURCE_DESC desc = resource->GetDesc();
        ID3D12Device* pDevice = nullptr;
        if (SUCCEEDED(resource->GetDevice(IID_PPV_ARGS(&pDevice))))
        {
            allocInfo = pDevice->GetResourceAllocationInfo(0, 1, &desc);
            size = allocInfo.SizeInBytes;
            pDevice->Release();
        }
    }

    return size;
}

ApusrFramegenResult GetEffectGpuMemoryUsageDX12(ApusrFramegenBackendInterface* backendInterface, uint32_t effectContextId, ApusrFramegenMemoryUsage* outVramUsage)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != outVramUsage);

    BackendContext_DX12*                backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;
    BackendContext_DX12::EffectContext& effectContext  = backendContext->pEffectContexts[effectContextId];

    *outVramUsage = effectContext.vramUsage;

    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenResourceSizeDX12(ID3D12Device* device, const ApusrFramegenCreateResourceDescription* createResourceDescription, uint64_t* sizeInBytes)
{
    FFX_ASSERT(NULL != device);
    FFX_ASSERT(NULL != createResourceDescription);
    FFX_ASSERT(NULL != sizeInBytes);

    if (createResourceDescription->heapType == APUSR_FRAMEGEN_HEAP_UPLOAD)
    {
        *sizeInBytes = 0u;
        return APUSR_FRAMEGEN_OK;
    }

    D3D12_RESOURCE_DESC dx12ResourceDescription = {};
    dx12ResourceDescription.Format = DXGI_FORMAT_UNKNOWN;
    dx12ResourceDescription.Width = 1;
    dx12ResourceDescription.Height = 1;
    dx12ResourceDescription.MipLevels = 1;
    dx12ResourceDescription.DepthOrArraySize = 1;
    dx12ResourceDescription.SampleDesc.Count = 1;
    dx12ResourceDescription.Flags = apusrFramegenDx12ResourceFlags((ApusrFramegenResourceUsage)createResourceDescription->resourceDescription.usage);

    switch (createResourceDescription->resourceDescription.type) {

    case APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER:
        dx12ResourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        dx12ResourceDescription.Width = createResourceDescription->resourceDescription.width;
        dx12ResourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        break;

    case APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE1D:
        dx12ResourceDescription.Format = apusrFramegenDxgiFormat((ApusrFramegenSurfaceFormat)createResourceDescription->resourceDescription.format);
        dx12ResourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        dx12ResourceDescription.Width = createResourceDescription->resourceDescription.width;
        dx12ResourceDescription.DepthOrArraySize = UINT16(createResourceDescription->resourceDescription.depth);
        dx12ResourceDescription.MipLevels = UINT16(createResourceDescription->resourceDescription.mipCount);
        break;

    case APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D:
        dx12ResourceDescription.Format = apusrFramegenDxgiFormat((ApusrFramegenSurfaceFormat)createResourceDescription->resourceDescription.format);
        dx12ResourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dx12ResourceDescription.Width = createResourceDescription->resourceDescription.width;
        dx12ResourceDescription.Height = createResourceDescription->resourceDescription.height;
        dx12ResourceDescription.DepthOrArraySize = UINT16(createResourceDescription->resourceDescription.depth);
        dx12ResourceDescription.MipLevels = UINT16(createResourceDescription->resourceDescription.mipCount);
        break;

    case APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE_CUBE:
    case APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE3D:
        dx12ResourceDescription.Format = apusrFramegenDxgiFormat((ApusrFramegenSurfaceFormat)createResourceDescription->resourceDescription.format);
        dx12ResourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        dx12ResourceDescription.Width = createResourceDescription->resourceDescription.width;
        dx12ResourceDescription.Height = createResourceDescription->resourceDescription.height;
        dx12ResourceDescription.DepthOrArraySize = UINT16(createResourceDescription->resourceDescription.depth);
        dx12ResourceDescription.MipLevels = UINT16(createResourceDescription->resourceDescription.mipCount);
        break;

    default:
        break;
    }
    ID3D12Device* pDevice = nullptr;
    D3D12_RESOURCE_ALLOCATION_INFO allocInfo = {};
    if (SUCCEEDED(((ID3D12Device*) device)->QueryInterface(IID_PPV_ARGS(&pDevice))))
    {
        allocInfo = pDevice->GetResourceAllocationInfo(0, 1, &dx12ResourceDescription);
        pDevice->Release();
        *sizeInBytes = allocInfo.SizeInBytes;
        if (allocInfo.SizeInBytes != UINT64_MAX)
        {
            return APUSR_FRAMEGEN_OK;
        }
    }
    return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;
    
}

// initialize the DX12 backend
ApusrFramegenResult CreateBackendContextDX12(ApusrFramegenBackendInterface* backendInterface, uint32_t* effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != backendInterface->device);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(effectContextId, APUSR_FRAMEGEN_ERROR_INVALID_POINTER);

    ID3D12Device* dx12Device = reinterpret_cast<ID3D12Device*>(backendInterface->device);
    BackendContext_DX12* backendContext = reinterpret_cast<BackendContext_DX12*>(backendInterface->scratchBuffer);

    // Shared descriptor infrastructure lives only while at least one effect context is active.
    if (!backendContext->refCount) {
        dx12Device->AddRef();
        backendContext->device = dx12Device;
        backendContext->profileStream = apusr::profile::registerStream(apusr::profile::StreamDesc{
            .backend = apusr::profile::BackendApi::Dx12,
            .component = apusr::profile::Component::FrameGeneration,
            .name = "framegen.compute",
            .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                apusr::profile::StreamCapabilityFrames |
                apusr::profile::StreamCapabilityCounters |
                apusr::profile::StreamCapabilityErrors |
                apusr::profile::StreamCapabilityResourceEvents |
                apusr::profile::StreamCapabilityGpuWorkloadInfo,
            .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(dx12Device)),
        });
        backendContext->profileSampleGate.reset();
        backendContext->profileFrameSequence = 0u;
        const uint32_t gpuJobDescArraySize = FFX_ALIGN_UP(backendContext->maxEffectContexts * APUSR_FRAMEGEN_MAX_GPU_JOBS * sizeof(ApusrFramegenGpuJob), sizeof(uint32_t));
        const uint32_t resourceArraySize = FFX_ALIGN_UP(backendContext->maxEffectContexts * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT * sizeof(BackendContext_DX12::Resource), sizeof(uint64_t));
        const uint32_t stagingRingBufferArraySize = FFX_ALIGN_UP(backendContext->maxEffectContexts * APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE, sizeof(uint32_t));
        const uint32_t contextArraySize = FFX_ALIGN_UP(backendContext->maxEffectContexts * sizeof(BackendContext_DX12::EffectContext), sizeof(uint32_t));

        uint8_t* pMem = reinterpret_cast<uint8_t*>(backendContext + 1);

        backendContext->pGpuJobs = reinterpret_cast<ApusrFramegenGpuJob*>(pMem);
        memset(backendContext->pGpuJobs, 0, gpuJobDescArraySize);
        pMem += gpuJobDescArraySize;

        backendContext->pResources = reinterpret_cast<BackendContext_DX12::Resource*>(pMem);
        memset(backendContext->pResources, 0, resourceArraySize);
        pMem += resourceArraySize;

        backendContext->pStagingRingBuffer = pMem;
        memset(backendContext->pStagingRingBuffer, 0, stagingRingBufferArraySize);
        pMem += stagingRingBufferArraySize;

        backendContext->pEffectContexts = reinterpret_cast<BackendContext_DX12::EffectContext*>(pMem);
        memset(backendContext->pEffectContexts, 0, contextArraySize);

        D3D12_DESCRIPTOR_HEAP_DESC descHeap{};
        descHeap.NumDescriptors = APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT * backendContext->maxEffectContexts;
        descHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

        if (FAILED(dx12Device->CreateDescriptorHeap(&descHeap, IID_PPV_ARGS(&backendContext->descHeapSrvCpu))) ||
            FAILED(dx12Device->CreateDescriptorHeap(&descHeap, IID_PPV_ARGS(&backendContext->descHeapUavCpu)))) {
            releaseBackendDx12Infrastructure(backendContext);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }

        descHeap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dx12Device->CreateDescriptorHeap(&descHeap, IID_PPV_ARGS(&backendContext->descHeapUavGpu)))) {
            releaseBackendDx12Infrastructure(backendContext);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }

        descHeap.NumDescriptors = APUSR_FRAMEGEN_DESCRIPTOR_RING_SIZE * backendContext->maxEffectContexts;
        backendContext->descRingBufferSize = descHeap.NumDescriptors;
        backendContext->descRingBufferBase = 0;
        if (FAILED(dx12Device->CreateDescriptorHeap(&descHeap, IID_PPV_ARGS(&backendContext->descRingBuffer)))) {
            releaseBackendDx12Infrastructure(backendContext);
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }

        backendContext->descriptorSizeCbvSrvUav = dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        backendContext->descHeapSrvCpuStart = backendContext->descHeapSrvCpu->GetCPUDescriptorHandleForHeapStart();
        backendContext->descHeapUavCpuStart = backendContext->descHeapUavCpu->GetCPUDescriptorHandleForHeapStart();
        backendContext->descHeapUavGpuCpuStart = backendContext->descHeapUavGpu->GetCPUDescriptorHandleForHeapStart();
        backendContext->descHeapUavGpuStart = backendContext->descHeapUavGpu->GetGPUDescriptorHandleForHeapStart();
        backendContext->descRingCpuStart = backendContext->descRingBuffer->GetCPUDescriptorHandleForHeapStart();
        backendContext->descRingGpuStart = backendContext->descRingBuffer->GetGPUDescriptorHandleForHeapStart();
    }

    for (uint32_t i = 0; i < backendContext->maxEffectContexts; ++i) {
        BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[i];
        if (effectContext.active)
            continue;

        *effectContextId = i;
        effectContext.active = true;
        effectContext.nextStaticResource = (i * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT) + 1;
        effectContext.nextDynamicResource = (i * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT) + APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT - 1;
        effectContext.nextStaticUavDescriptor = i * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT;
        effectContext.nextDynamicUavDescriptor = (i * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT) + APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT - 1;
        effectContext.profileConfigurationHash = 0u;
        effectContext.profileHaveConfiguration = false;
        ++backendContext->refCount;
        return APUSR_FRAMEGEN_OK;
    }

    return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
}

// query device capabilities to select the optimal shader permutation
ApusrFramegenResult GetDeviceCapabilitiesDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenDeviceCapabilities* deviceCapabilities)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != backendInterface->device);
    FFX_ASSERT(NULL != deviceCapabilities);
    ID3D12Device* dx12Device = reinterpret_cast<ID3D12Device*>(backendInterface->device);

    // Check if we have shader model 6.6
    D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_6 };
    if (SUCCEEDED(dx12Device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(D3D12_FEATURE_DATA_SHADER_MODEL)))) {

        switch (shaderModel.HighestShaderModel) {

        case D3D_SHADER_MODEL_5_1:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_5_1;
            break;

        case D3D_SHADER_MODEL_6_0:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_0;
            break;

        case D3D_SHADER_MODEL_6_1:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_1;
            break;

        case D3D_SHADER_MODEL_6_2:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_2;
            break;

        case D3D_SHADER_MODEL_6_3:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_3;
            break;

        case D3D_SHADER_MODEL_6_4:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_4;
            break;

        case D3D_SHADER_MODEL_6_5:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_5;
            break;

        case D3D_SHADER_MODEL_6_6:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_6;
            break;

        default:
            deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_6_6;
            break;
        }
    }
    else {

        deviceCapabilities->maximumSupportedShaderModel = APUSR_FRAMEGEN_SHADER_MODEL_5_1;
    }

    // check if we can force wave64 mode.
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 d3d12Options1 = {};
    if (SUCCEEDED(dx12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &d3d12Options1, sizeof(d3d12Options1)))) {

        const uint32_t waveLaneCountMin = d3d12Options1.WaveLaneCountMin;
        const uint32_t waveLaneCountMax = d3d12Options1.WaveLaneCountMax;
        deviceCapabilities->waveLaneCountMin = waveLaneCountMin;
        deviceCapabilities->waveLaneCountMax = waveLaneCountMax;
    }

    // check if we have 16bit floating point.
    D3D12_FEATURE_DATA_D3D12_OPTIONS d3d12Options = {};
    if (SUCCEEDED(dx12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &d3d12Options, sizeof(d3d12Options)))) {

        deviceCapabilities->fp16Supported = bool(d3d12Options.MinPrecisionSupport & D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 d3d12Options4 = {};
    if (SUCCEEDED(dx12Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &d3d12Options4, sizeof(d3d12Options4)))) {

        deviceCapabilities->fp16Supported &= bool(d3d12Options4.Native16BitShaderOpsSupported);
    }

    return APUSR_FRAMEGEN_OK;
}

// deinitialize the DX12 backend
ApusrFramegenResult DestroyBackendContextDX12(ApusrFramegenBackendInterface* backendInterface, uint32_t effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);
    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;
    APUSR_FRAMEGEN_RETURN_ON_ERROR(backendContext->refCount > 0, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(effectContextId < backendContext->maxEffectContexts, APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE);

    // Delete any resources allocated by this context
    BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];
    APUSR_FRAMEGEN_RETURN_ON_ERROR(effectContext.active, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);
    for (uint32_t currentStaticResourceIndex = effectContextId * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT; currentStaticResourceIndex < (uint32_t)effectContext.nextStaticResource; ++currentStaticResourceIndex) {
        if (backendContext->pResources[currentStaticResourceIndex].resourcePtr) {
            FFX_ASSERT_MESSAGE(false, "APUSR FrameGen DX12: resource was not destroyed before backend-context teardown.");
            ApusrFramegenResource internalResource = { (int32_t)currentStaticResourceIndex };
            DestroyResourceDX12(backendInterface, internalResource, effectContextId);
        }
    }

    // Free up for use by another context
    effectContext.nextStaticResource = 0;
    effectContext.active = false;
    effectContext.profileConfigurationHash = 0u;
    effectContext.profileHaveConfiguration = false;

    // Decrement ref count
    --backendContext->refCount;

    if (!backendContext->refCount) {
        backendContext->gpuJobCount = 0;
        backendContext->barrierCount = 0;
        releaseBackendDx12Infrastructure(backendContext);
    }

    return APUSR_FRAMEGEN_OK;
}

// create a internal resource that will stay alive until effect gets shut down
ApusrFramegenResult CreateResourceDX12(
    ApusrFramegenBackendInterface* backendInterface,
    const ApusrFramegenCreateResourceDescription* createResourceDescription,
    uint32_t effectContextId,
    ApusrFramegenResource* outTexture
)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != createResourceDescription);
    FFX_ASSERT(NULL != outTexture);
    FFX_ASSERT_MESSAGE(createResourceDescription->initData.type != APUSR_FRAMEGEN_RESOURCE_INIT_INVALID,
                       "InitData type cannot be APUSR_FRAMEGEN_RESOURCE_INIT_INVALID. Please explicitly specify the resource initialization type.");


    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;
    BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];
    ID3D12Device* dx12Device = backendContext->device;

    uint64_t resourceSize = 0;
    FFX_ASSERT(NULL != dx12Device);

    D3D12_HEAP_PROPERTIES dx12HeapProperties = {};

    switch (createResourceDescription->heapType)
    {
    case APUSR_FRAMEGEN_HEAP_DEFAULT:
        dx12HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        break;
    case APUSR_FRAMEGEN_HEAP_UPLOAD:
        dx12HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
        break;
    case APUSR_FRAMEGEN_HEAP_READBACK:
        dx12HeapProperties.Type = D3D12_HEAP_TYPE_READBACK;
        break;
    default:
        dx12HeapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        break;
    }

    FFX_ASSERT(effectContext.nextStaticResource + 1 < effectContext.nextDynamicResource);

    outTexture->internalIndex = effectContext.nextStaticResource++;
    BackendContext_DX12::Resource* backendResource = &backendContext->pResources[outTexture->internalIndex];
    backendResource->resourceDescription = createResourceDescription->resourceDescription;
    backendResource->allocationSize = 0u;
    backendResource->allocationAccounted = false;

    const auto& initData = createResourceDescription->initData;

    D3D12_RESOURCE_DESC dx12ResourceDescription = {};
    dx12ResourceDescription.Format              = DXGI_FORMAT_UNKNOWN;
    dx12ResourceDescription.Width               = 1;
    dx12ResourceDescription.Height              = 1;
    dx12ResourceDescription.MipLevels           = 1;
    dx12ResourceDescription.DepthOrArraySize    = 1;
    dx12ResourceDescription.SampleDesc.Count    = 1;
    dx12ResourceDescription.Flags               = apusrFramegenDx12ResourceFlags((ApusrFramegenResourceUsage)backendResource->resourceDescription.usage);

    switch (createResourceDescription->resourceDescription.type) {

    case APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER:
        dx12ResourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        dx12ResourceDescription.Width = createResourceDescription->resourceDescription.width;
        dx12ResourceDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        break;

    case APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE1D:
        dx12ResourceDescription.Format = apusrFramegenDxgiFormat((ApusrFramegenSurfaceFormat)createResourceDescription->resourceDescription.format);
        dx12ResourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
        dx12ResourceDescription.Width = createResourceDescription->resourceDescription.width;
        dx12ResourceDescription.DepthOrArraySize = UINT16(createResourceDescription->resourceDescription.depth);
        dx12ResourceDescription.MipLevels = UINT16(createResourceDescription->resourceDescription.mipCount);
        break;

    case APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D:
        dx12ResourceDescription.Format    = apusrFramegenDxgiFormat((ApusrFramegenSurfaceFormat)createResourceDescription->resourceDescription.format);
        dx12ResourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dx12ResourceDescription.Width = createResourceDescription->resourceDescription.width;
        dx12ResourceDescription.Height = createResourceDescription->resourceDescription.height;
        dx12ResourceDescription.DepthOrArraySize = UINT16(createResourceDescription->resourceDescription.depth);
        dx12ResourceDescription.MipLevels = UINT16(createResourceDescription->resourceDescription.mipCount);
        break;

    case APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE_CUBE:
    case APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE3D:
        dx12ResourceDescription.Format    = apusrFramegenDxgiFormat((ApusrFramegenSurfaceFormat)createResourceDescription->resourceDescription.format);
        dx12ResourceDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        dx12ResourceDescription.Width = createResourceDescription->resourceDescription.width;
        dx12ResourceDescription.Height = createResourceDescription->resourceDescription.height;
        dx12ResourceDescription.DepthOrArraySize = UINT16(createResourceDescription->resourceDescription.depth);
        dx12ResourceDescription.MipLevels = UINT16(createResourceDescription->resourceDescription.mipCount);
        break;

    default:
        break;
    }

    ID3D12Resource* dx12Resource = nullptr;
    const auto accountResource = [&](const uint64_t size) {
        backendResource->allocationSize = size;
        backendResource->allocationAccounted = true;
        effectContext.vramUsage.totalUsageInBytes += size;
        if ((createResourceDescription->resourceDescription.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE) != 0) {
            effectContext.vramUsage.aliasableUsageInBytes += size;
        }
        if (backendContext->profileStream && size != 0u) {
            apusr::profile::recordCounter({
                .stream = backendContext->profileStream,
                .name = "memory.resource_alloc_bytes",
                .value = size,
                .value2 = static_cast<std::uint64_t>(outTexture->internalIndex),
                .flags = effectContextId,
            });
            apusr::profile::recordCounter({
                .stream = backendContext->profileStream,
                .name = "memory.effect_usage_bytes",
                .value = effectContext.vramUsage.totalUsageInBytes,
                .value2 = effectContext.vramUsage.aliasableUsageInBytes,
                .flags = effectContextId,
            });
            apusr::profile::recordResourceEvent({
                .stream = backendContext->profileStream,
                .name = "framegen.effect_resource",
                .detail = createResourceDescription->resourceDescription.type == APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER
                    ? "buffer" : "texture",
                .operation = apusr::profile::ResourceOperation::Allocate,
                .resourceId = (static_cast<std::uint64_t>(effectContextId) << 32u) |
                    static_cast<std::uint32_t>(outTexture->internalIndex),
                .nativeHandle = reinterpret_cast<std::uint64_t>(dx12Resource),
                .bytes = size,
                .alignment = createResourceDescription->resourceDescription.alignment,
                .heapOrMemoryType = static_cast<std::uint32_t>(createResourceDescription->heapType),
                .flags = (createResourceDescription->resourceDescription.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE) != 0
                    ? 1u : 0u,
            });
        }
    };
    if (createResourceDescription->heapType == APUSR_FRAMEGEN_HEAP_UPLOAD) {

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT dx12Footprint = {};

        UINT rowCount;
        UINT64 rowSizeInBytes;
        UINT64 totalBytes;

        dx12Device->GetCopyableFootprints(&dx12ResourceDescription, 0, 1, 0, &dx12Footprint, &rowCount, &rowSizeInBytes, &totalBytes);

        D3D12_HEAP_PROPERTIES dx12UploadHeapProperties = {};
        dx12UploadHeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC dx12UploadBufferDescription = {};

        dx12UploadBufferDescription.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        dx12UploadBufferDescription.Width = totalBytes;
        dx12UploadBufferDescription.Height = 1;
        dx12UploadBufferDescription.DepthOrArraySize = 1;
        dx12UploadBufferDescription.MipLevels = 1;
        dx12UploadBufferDescription.Format = DXGI_FORMAT_UNKNOWN;
        dx12UploadBufferDescription.SampleDesc.Count = 1;
        dx12UploadBufferDescription.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        TIF(dx12Device->CreateCommittedResource(&dx12HeapProperties, D3D12_HEAP_FLAG_NONE, &dx12UploadBufferDescription, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&dx12Resource)));
        resourceSize = GetResourceGpuMemorySizeDX12(dx12Resource);

        backendResource->initialState = APUSR_FRAMEGEN_RESOURCE_STATE_GENERIC_READ;
        backendResource->currentState = APUSR_FRAMEGEN_RESOURCE_STATE_GENERIC_READ;

        D3D12_RANGE dx12EmptyRange = {};
        void* uploadBufferData = nullptr;
        TIF(dx12Resource->Map(0, &dx12EmptyRange, &uploadBufferData));

        const uint8_t* src = static_cast<uint8_t*>(initData.buffer);
        uint8_t* dst = static_cast<uint8_t*>(uploadBufferData);
        for (uint32_t currentRowIndex = 0; currentRowIndex < rowCount; ++currentRowIndex) {

            if (initData.type == APUSR_FRAMEGEN_RESOURCE_INIT_BUFFER)
            {
                memcpy(dst, src, (size_t)rowSizeInBytes);
                src += rowSizeInBytes;
            }
            else if (initData.type == APUSR_FRAMEGEN_RESOURCE_INIT_VALUE)
            {
                memset(dst, initData.value, (size_t)rowSizeInBytes);
            }
            dst += dx12Footprint.Footprint.RowPitch;
        }

        dx12Resource->Unmap(0, nullptr);
        dx12Resource->SetName(createResourceDescription->name);
        backendResource->resourcePtr = dx12Resource;

#ifdef _DEBUG
        wcscpy_s(backendResource->resourceName, createResourceDescription->name);
#endif
        accountResource(resourceSize);
        return APUSR_FRAMEGEN_OK;

    }
    else {

        const ApusrFramegenResourceState resourceStates =
            ((initData.type != APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED) && (createResourceDescription->heapType != APUSR_FRAMEGEN_HEAP_UPLOAD))
                ? APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST
                : createResourceDescription->initialState;
        // Buffers ignore any input state and create in common (but issue a warning)
        const D3D12_RESOURCE_STATES dx12ResourceStates = dx12ResourceDescription.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_RESOURCE_STATE_COMMON : apusrFramegenDx12State(resourceStates);

        TIF(dx12Device->CreateCommittedResource(&dx12HeapProperties, D3D12_HEAP_FLAG_NONE, &dx12ResourceDescription, dx12ResourceStates, nullptr, IID_PPV_ARGS(&dx12Resource)));
        resourceSize = GetResourceGpuMemorySizeDX12(dx12Resource);
        backendResource->initialState = resourceStates;
        backendResource->currentState = resourceStates;

        dx12Resource->SetName(createResourceDescription->name);
        backendResource->resourcePtr = dx12Resource;

#ifdef _DEBUG
        wcscpy_s(backendResource->resourceName, createResourceDescription->name);
#endif

        // Create SRVs and UAVs
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC dx12UavDescription = {};
            D3D12_SHADER_RESOURCE_VIEW_DESC dx12SrvDescription = {};
            D3D12_RESOURCE_DESC dx12Desc = dx12Resource->GetDesc();
            dx12UavDescription.Format = convertFormatUav(dx12Desc.Format);
            dx12SrvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            dx12SrvDescription.Format = convertFormatSrvBackend(dx12Desc.Format);

            bool requestArrayView = (createResourceDescription->resourceDescription.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_ARRAYVIEW) != 0;

            switch (dx12Desc.Dimension) {

            case D3D12_RESOURCE_DIMENSION_BUFFER:
                dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                break;

            case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
                if (dx12Desc.DepthOrArraySize > 1 || requestArrayView)
                {
                    dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                    dx12UavDescription.Texture1DArray.ArraySize = dx12Desc.DepthOrArraySize;
                    dx12UavDescription.Texture1DArray.FirstArraySlice = 0;
                    dx12UavDescription.Texture1DArray.MipSlice = 0;

                    dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                    dx12SrvDescription.Texture1DArray.ArraySize = dx12Desc.DepthOrArraySize;
                    dx12SrvDescription.Texture1DArray.FirstArraySlice = 0;
                    dx12SrvDescription.Texture1DArray.MipLevels = dx12Desc.MipLevels;
                    dx12SrvDescription.Texture1DArray.MostDetailedMip = 0;
                }
                else
                {
                    dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                    dx12UavDescription.Texture1D.MipSlice = 0;

                    dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                    dx12SrvDescription.Texture1D.MipLevels = dx12Desc.MipLevels;
                    dx12SrvDescription.Texture1D.MostDetailedMip = 0;
                }
                break;

            case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
            {
                if (dx12Desc.DepthOrArraySize > 1 || requestArrayView)
                {
                    dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                    dx12UavDescription.Texture2DArray.ArraySize = dx12Desc.DepthOrArraySize;
                    dx12UavDescription.Texture2DArray.FirstArraySlice = 0;
                    dx12UavDescription.Texture2DArray.MipSlice = 0;
                    dx12UavDescription.Texture2DArray.PlaneSlice = 0;

                    dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    dx12SrvDescription.Texture2DArray.ArraySize = dx12Desc.DepthOrArraySize;
                    dx12SrvDescription.Texture2DArray.FirstArraySlice = 0;
                    dx12SrvDescription.Texture2DArray.MipLevels = dx12Desc.MipLevels;
                    dx12SrvDescription.Texture2DArray.MostDetailedMip = 0;
                    dx12SrvDescription.Texture2DArray.PlaneSlice = 0;
                }
                else
                {
                    dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    dx12UavDescription.Texture2D.MipSlice = 0;
                    dx12UavDescription.Texture2D.PlaneSlice = 0;

                    dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    dx12SrvDescription.Texture2D.MipLevels = dx12Desc.MipLevels;
                    dx12SrvDescription.Texture2D.MostDetailedMip = 0;
                    dx12SrvDescription.Texture2D.PlaneSlice = 0;
                }
                break;
            }

            case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
                dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                dx12SrvDescription.Texture3D.MipLevels = dx12Resource->GetDesc().MipLevels;
                dx12SrvDescription.Texture3D.MostDetailedMip = 0;
                break;

            default:
                break;
            }

            if (dx12Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {

                dx12SrvDescription.Buffer.FirstElement        = 0;
                dx12SrvDescription.Buffer.StructureByteStride = backendResource->resourceDescription.stride;
                uint32_t stride                               = backendResource->resourceDescription.stride; 
                if (backendResource->resourceDescription.stride == 0)
                {
                    stride = sizeof(uint32_t);
                    dx12SrvDescription.Format = DXGI_FORMAT_R32_TYPELESS;
                    dx12SrvDescription.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                }
                dx12SrvDescription.Buffer.NumElements = backendResource->resourceDescription.size / stride;
                
                D3D12_CPU_DESCRIPTOR_HANDLE dx12CpuHandle     = backendContext->descHeapSrvCpuStart;
                dx12CpuHandle.ptr += outTexture->internalIndex * backendContext->descriptorSizeCbvSrvUav;
                dx12Device->CreateShaderResourceView(dx12Resource, &dx12SrvDescription, dx12CpuHandle);

                // UAV
                if (dx12Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {

                    FFX_ASSERT(effectContext.nextStaticUavDescriptor + 1 < effectContext.nextDynamicUavDescriptor);
                    backendResource->uavDescCount = 1;
                    backendResource->uavDescIndex = effectContext.nextStaticUavDescriptor++;

                    dx12UavDescription.Format = dx12SrvDescription.Format;

                    dx12UavDescription.Buffer.FirstElement = 0;
                    dx12UavDescription.Buffer.StructureByteStride = backendResource->resourceDescription.stride;
                    dx12UavDescription.Buffer.NumElements          = backendResource->resourceDescription.size / stride;
                    dx12UavDescription.Buffer.CounterOffsetInBytes = 0;
                    if (backendResource->resourceDescription.stride == 0)
                    {
                        dx12UavDescription.Buffer.Flags |= D3D12_BUFFER_UAV_FLAG_RAW;
                    }

                    dx12CpuHandle = backendContext->descHeapUavGpuCpuStart;
                    dx12CpuHandle.ptr += (backendResource->uavDescIndex) * backendContext->descriptorSizeCbvSrvUav;
                    dx12Device->CreateUnorderedAccessView(dx12Resource, 0, &dx12UavDescription, dx12CpuHandle);

                    dx12CpuHandle = backendContext->descHeapUavCpuStart;
                    dx12CpuHandle.ptr += (backendResource->uavDescIndex) * backendContext->descriptorSizeCbvSrvUav;
                    dx12Device->CreateUnorderedAccessView(dx12Resource, 0, &dx12UavDescription, dx12CpuHandle);

                    effectContext.nextStaticUavDescriptor++;
                }
            }
            else {
                // CPU readable
                D3D12_CPU_DESCRIPTOR_HANDLE dx12CpuHandle = backendContext->descHeapSrvCpuStart;
                dx12CpuHandle.ptr += outTexture->internalIndex * backendContext->descriptorSizeCbvSrvUav;
                dx12Device->CreateShaderResourceView(dx12Resource, &dx12SrvDescription, dx12CpuHandle);

                // UAV
                if (dx12Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {

                    const int32_t uavDescriptorCount = (dx12Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? dx12Desc.MipLevels : 1;
                    FFX_ASSERT(effectContext.nextStaticUavDescriptor + uavDescriptorCount < effectContext.nextDynamicUavDescriptor);

                    backendResource->uavDescCount = uavDescriptorCount;
                    backendResource->uavDescIndex = effectContext.nextStaticUavDescriptor;

                    for (int32_t currentMipIndex = 0; currentMipIndex < uavDescriptorCount; ++currentMipIndex) {

                        if (createResourceDescription->resourceDescription.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE3D)
                        {
                            dx12UavDescription.Texture3D.MipSlice    = currentMipIndex;
                            dx12UavDescription.Texture3D.FirstWSlice = currentMipIndex;
                            dx12UavDescription.Texture3D.WSize       = createResourceDescription->resourceDescription.depth;
                        }
                        else if (createResourceDescription->resourceDescription.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D)
                            dx12UavDescription.Texture2D.MipSlice = currentMipIndex;
                        else if (createResourceDescription->resourceDescription.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE1D)
                            dx12UavDescription.Texture1D.MipSlice = currentMipIndex;

                        dx12CpuHandle = backendContext->descHeapUavGpuCpuStart;
                        dx12CpuHandle.ptr += (backendResource->uavDescIndex + currentMipIndex) * backendContext->descriptorSizeCbvSrvUav;
                        dx12Device->CreateUnorderedAccessView(dx12Resource, 0, &dx12UavDescription, dx12CpuHandle);

                        dx12CpuHandle = backendContext->descHeapUavCpuStart;
                        dx12CpuHandle.ptr += (backendResource->uavDescIndex + currentMipIndex) * backendContext->descriptorSizeCbvSrvUav;
                        dx12Device->CreateUnorderedAccessView(dx12Resource, 0, &dx12UavDescription, dx12CpuHandle);
                    }

                    effectContext.nextStaticUavDescriptor += uavDescriptorCount;
                }
            }
        }

        // create upload resource and upload job
        if (initData.type != APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED) {

            ApusrFramegenResource copySrc;
            ApusrFramegenCreateResourceDescription uploadDescription = { *createResourceDescription };
            uploadDescription.heapType = APUSR_FRAMEGEN_HEAP_UPLOAD;
            uploadDescription.resourceDescription.usage = APUSR_FRAMEGEN_RESOURCE_USAGE_READ_ONLY;
            uploadDescription.initialState = APUSR_FRAMEGEN_RESOURCE_STATE_GENERIC_READ;

            backendInterface->createResource(backendInterface, &uploadDescription, effectContextId, &copySrc);

            // setup the upload job
            ApusrFramegenGpuJob copyJob = { APUSR_FRAMEGEN_JOB_COPY };
            APUSR_FRAMEGEN_SET_JOB_LABEL(copyJob, L"Resource Initialization Copy");
            copyJob.copyJob.src = copySrc;
            copyJob.copyJob.dst = *outTexture;
            copyJob.copyJob.srcOffset = 0;
            copyJob.copyJob.dstOffset = 0;
            copyJob.copyJob.size      = 0;

            backendInterface->scheduleJob(backendInterface, &copyJob);
        }
    }

    accountResource(resourceSize);
    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult DestroyResourceDX12(
    ApusrFramegenBackendInterface* backendInterface,
    ApusrFramegenResource resource,
	uint32_t effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);

    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;
	BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];
	if ((resource.internalIndex >= int32_t(effectContextId * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT)) && (resource.internalIndex < int32_t(effectContext.nextStaticResource))) {
		ID3D12Resource* dx12Resource = getDX12ResourcePtr(backendContext, resource.internalIndex);

		if (dx12Resource) {

            BackendContext_DX12::Resource& backendResource = backendContext->pResources[resource.internalIndex];
            const uint64_t resourceSize = backendResource.allocationAccounted ? backendResource.allocationSize : 0u;
            const bool aliasable = (backendResource.resourceDescription.flags & APUSR_FRAMEGEN_RESOURCE_FLAGS_ALIASABLE) != 0;

			dx12Resource->Release();

            if (backendResource.allocationAccounted) {
                const bool totalUnderflow = effectContext.vramUsage.totalUsageInBytes < resourceSize;
                const bool aliasUnderflow = aliasable && effectContext.vramUsage.aliasableUsageInBytes < resourceSize;
                if ((totalUnderflow || aliasUnderflow) && backendContext->profileStream) {
                    apusr::profile::recordError(
                        backendContext->profileStream, 0u, "memory.accounting", apusr::StatusCode::RuntimeFailure,
                        "FrameGen DX12 resource accounting underflow prevented");
                }
                effectContext.vramUsage.totalUsageInBytes = totalUnderflow
                    ? 0u : effectContext.vramUsage.totalUsageInBytes - resourceSize;
                if (aliasable) {
                    effectContext.vramUsage.aliasableUsageInBytes = aliasUnderflow
                        ? 0u : effectContext.vramUsage.aliasableUsageInBytes - resourceSize;
                }
                backendResource.allocationAccounted = false;
                backendResource.allocationSize = 0u;
            }

            if (backendContext->profileStream && resourceSize != 0u) {
                apusr::profile::recordCounter({
                    .stream = backendContext->profileStream,
                    .name = "memory.resource_free_bytes",
                    .value = resourceSize,
                    .value2 = static_cast<std::uint64_t>(resource.internalIndex),
                    .flags = effectContextId,
                });
                apusr::profile::recordCounter({
                    .stream = backendContext->profileStream,
                    .name = "memory.effect_usage_bytes",
                    .value = effectContext.vramUsage.totalUsageInBytes,
                    .value2 = effectContext.vramUsage.aliasableUsageInBytes,
                    .flags = effectContextId,
                });
                apusr::profile::recordResourceEvent({
                    .stream = backendContext->profileStream,
                    .name = "framegen.effect_resource",
                    .detail = "resource",
                    .operation = apusr::profile::ResourceOperation::Free,
                    .resourceId = (static_cast<std::uint64_t>(effectContextId) << 32u) |
                        static_cast<std::uint32_t>(resource.internalIndex),
                    .nativeHandle = reinterpret_cast<std::uint64_t>(dx12Resource),
                    .bytes = resourceSize,
                    .flags = aliasable ? 1u : 0u,
                });
            }

			backendContext->pResources[resource.internalIndex].resourcePtr = nullptr;
		}
        
        return APUSR_FRAMEGEN_OK;
	}

	return APUSR_FRAMEGEN_ERROR_OUT_OF_RANGE;
}

DXGI_FORMAT patchDxgiFormatWithApusrUsage(DXGI_FORMAT dxResFmt, ApusrFramegenSurfaceFormat format)
{
    DXGI_FORMAT declaredFormat = apusrFramegenDxgiFormat(format);
    DXGI_FORMAT fmt = dxResFmt;

    switch (fmt)
    {
    case DXGI_FORMAT_UNKNOWN:
        return declaredFormat;

    // fixup RGBA8 with SRGB flag passed in the ApusrFramegenSurfaceFormat
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return declaredFormat;
    
    // fixup depth formats as apusrFramegenDxgiFormat will result in wrong format
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
        break;
    }
    return fmt;
}

ApusrFramegenResult RegisterResourceDX12(
    ApusrFramegenBackendInterface* backendInterface,
    const ApusrFramegenExternalResource* inResource,
    uint32_t effectContextId,
    ApusrFramegenResource* outResource
)
{
    FFX_ASSERT(NULL != backendInterface);

    BackendContext_DX12* backendContext = (BackendContext_DX12*)(backendInterface->scratchBuffer);
    ID3D12Device* dx12Device = reinterpret_cast<ID3D12Device*>(backendContext->device);
    ID3D12Resource* dx12Resource = reinterpret_cast<ID3D12Resource*>(inResource->resource);
    BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];

    ApusrFramegenResourceState state = inResource->state;

    if (dx12Resource == nullptr) {

        outResource->internalIndex = 0; // Always maps to FFX_<feature>_RESOURCE_IDENTIFIER_NULL;
        return APUSR_FRAMEGEN_OK;
    }

    FFX_ASSERT(effectContext.nextDynamicResource > effectContext.nextStaticResource);
    outResource->internalIndex = effectContext.nextDynamicResource--;

    BackendContext_DX12::Resource* backendResource = &backendContext->pResources[outResource->internalIndex];
    backendResource->resourcePtr = dx12Resource;
    backendResource->initialState = state;
    backendResource->currentState = state;

    // create resource views
    if (dx12Resource) {

        D3D12_UNORDERED_ACCESS_VIEW_DESC dx12UavDescription = {};
        D3D12_SHADER_RESOURCE_VIEW_DESC dx12SrvDescription = {};
        D3D12_RESOURCE_DESC dx12Desc = dx12Resource->GetDesc();

        // we still want to respect the format provided in the description for SRGB or TYPELESS resources
        DXGI_FORMAT descFormat = patchDxgiFormatWithApusrUsage(dx12Desc.Format, inResource->description.format);

        dx12UavDescription.Format = convertFormatUav(descFormat);
        // Will support something other than this only where there is an actual need for it
        dx12SrvDescription.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dx12SrvDescription.Format = convertFormatSrvBackend(descFormat);

        bool requestArrayView = (inResource->description.usage & APUSR_FRAMEGEN_RESOURCE_USAGE_ARRAYVIEW) != 0;

        switch (dx12Desc.Dimension) {

        case D3D12_RESOURCE_DIMENSION_BUFFER:
            dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            backendResource->resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_BUFFER;
            backendResource->resourceDescription.size = inResource->description.size;
            backendResource->resourceDescription.stride = inResource->description.stride;
            backendResource->resourceDescription.alignment = 0;
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
            if (dx12Desc.DepthOrArraySize > 1 || requestArrayView)
            {
                dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                dx12UavDescription.Texture1DArray.ArraySize = dx12Desc.DepthOrArraySize;
                dx12UavDescription.Texture1DArray.FirstArraySlice = 0;
                dx12UavDescription.Texture1DArray.MipSlice = 0;

                dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                dx12SrvDescription.Texture1DArray.ArraySize = dx12Desc.DepthOrArraySize;
                dx12SrvDescription.Texture1DArray.FirstArraySlice = 0;
                dx12SrvDescription.Texture1DArray.MipLevels = dx12Desc.MipLevels;
                dx12SrvDescription.Texture1DArray.MostDetailedMip = 0;
            }
            else
            {
                dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
                dx12UavDescription.Texture1D.MipSlice = 0;

                dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                dx12SrvDescription.Texture1D.MipLevels = dx12Desc.MipLevels;
                dx12SrvDescription.Texture1D.MostDetailedMip = 0;
            }

            backendResource->resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE1D;
            backendResource->resourceDescription.format = inResource->description.format;
            backendResource->resourceDescription.width = inResource->description.width;
            backendResource->resourceDescription.mipCount = inResource->description.mipCount;
            backendResource->resourceDescription.depth = inResource->description.depth;
            break;

        case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        {
            if (dx12Desc.DepthOrArraySize > 1 || requestArrayView)
            {
                dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                dx12UavDescription.Texture2DArray.ArraySize = dx12Desc.DepthOrArraySize;
                dx12UavDescription.Texture2DArray.FirstArraySlice = 0;
                dx12UavDescription.Texture2DArray.MipSlice = 0;
                dx12UavDescription.Texture2DArray.PlaneSlice = 0;

                dx12SrvDescription.ViewDimension = inResource->description.type == APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE_CUBE ? D3D12_SRV_DIMENSION_TEXTURECUBE : D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                dx12SrvDescription.Texture2DArray.ArraySize = dx12Desc.DepthOrArraySize;
                dx12SrvDescription.Texture2DArray.FirstArraySlice = 0;
                dx12SrvDescription.Texture2DArray.MipLevels = dx12Desc.MipLevels;
                dx12SrvDescription.Texture2DArray.MostDetailedMip = 0;
                dx12SrvDescription.Texture2DArray.PlaneSlice = 0;
            }
            else
            {
                dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                dx12UavDescription.Texture2D.MipSlice = 0;
                dx12UavDescription.Texture2D.PlaneSlice = 0;

                dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                dx12SrvDescription.Texture2D.MipLevels = dx12Desc.MipLevels;
                dx12SrvDescription.Texture2D.MostDetailedMip = 0;
                dx12SrvDescription.Texture2D.PlaneSlice = 0;
            }

            backendResource->resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D;
            backendResource->resourceDescription.format = inResource->description.format;
            backendResource->resourceDescription.width = inResource->description.width;
            backendResource->resourceDescription.height = inResource->description.height;
            backendResource->resourceDescription.mipCount = inResource->description.mipCount;
            backendResource->resourceDescription.depth = inResource->description.depth;
            break;
        }

        case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
            dx12UavDescription.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            dx12UavDescription.Texture3D.FirstWSlice      = 0; // Bind all W slices
            dx12UavDescription.Texture3D.WSize            = std::numeric_limits<UINT>::max();
            dx12SrvDescription.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            dx12SrvDescription.Texture3D.MipLevels = dx12Desc.MipLevels;
            dx12SrvDescription.Texture3D.MostDetailedMip = 0;

            backendResource->resourceDescription.type = APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE3D;
            backendResource->resourceDescription.format = inResource->description.format;
            backendResource->resourceDescription.width = inResource->description.width;
            backendResource->resourceDescription.height = inResource->description.height;
            backendResource->resourceDescription.mipCount = inResource->description.mipCount;
            backendResource->resourceDescription.depth = inResource->description.depth;
            break;

        default:
            break;
        }

        if (dx12Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {

            // UAV
            if (dx12Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {

                FFX_ASSERT(effectContext.nextDynamicUavDescriptor > effectContext.nextStaticUavDescriptor);
                backendResource->uavDescCount = 1;
                backendResource->uavDescIndex = effectContext.nextDynamicUavDescriptor--;

                dx12UavDescription.Format = DXGI_FORMAT_UNKNOWN;
                dx12UavDescription.Buffer.FirstElement = 0;
                dx12UavDescription.Buffer.StructureByteStride = backendResource->resourceDescription.stride;
                dx12UavDescription.Buffer.NumElements = backendResource->resourceDescription.size / backendResource->resourceDescription.stride;
                dx12UavDescription.Buffer.CounterOffsetInBytes = 0;

                D3D12_CPU_DESCRIPTOR_HANDLE dx12CpuHandle = backendContext->descHeapUavGpuCpuStart;
                dx12CpuHandle.ptr += (backendResource->uavDescIndex) * backendContext->descriptorSizeCbvSrvUav;
                dx12Device->CreateUnorderedAccessView(dx12Resource, 0, &dx12UavDescription, dx12CpuHandle);

                dx12CpuHandle = backendContext->descHeapUavCpuStart;
                dx12CpuHandle.ptr += (backendResource->uavDescIndex) * backendContext->descriptorSizeCbvSrvUav;
                dx12Device->CreateUnorderedAccessView(dx12Resource, 0, &dx12UavDescription, dx12CpuHandle);
            }

            {
                dx12SrvDescription.Format = DXGI_FORMAT_UNKNOWN;
                dx12SrvDescription.Buffer.FirstElement        = 0;
                dx12SrvDescription.Buffer.StructureByteStride = backendResource->resourceDescription.stride;
                dx12SrvDescription.Buffer.NumElements         = backendResource->resourceDescription.size / backendResource->resourceDescription.stride;
                D3D12_CPU_DESCRIPTOR_HANDLE dx12CpuHandle     = backendContext->descHeapSrvCpuStart;
                dx12CpuHandle.ptr += outResource->internalIndex * backendContext->descriptorSizeCbvSrvUav;
                dx12Device->CreateShaderResourceView(dx12Resource, &dx12SrvDescription, dx12CpuHandle);
                backendResource->srvDescIndex = outResource->internalIndex;
            }
        }
        else {

            // CPU readable
            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = backendContext->descHeapSrvCpuStart;
            cpuHandle.ptr += outResource->internalIndex * backendContext->descriptorSizeCbvSrvUav;
            dx12Device->CreateShaderResourceView(dx12Resource, &dx12SrvDescription, cpuHandle);
            backendResource->srvDescIndex = outResource->internalIndex;

            // UAV
            if (dx12Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) {

                const int32_t uavDescriptorsCount = (dx12Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? dx12Desc.MipLevels : 1;
                FFX_ASSERT(effectContext.nextDynamicUavDescriptor - uavDescriptorsCount + 1 > effectContext.nextStaticUavDescriptor);

                backendResource->uavDescCount = uavDescriptorsCount;
                backendResource->uavDescIndex = effectContext.nextDynamicUavDescriptor - uavDescriptorsCount + 1;

                for (int32_t currentMipIndex = 0; currentMipIndex < uavDescriptorsCount; ++currentMipIndex) {

                    switch (dx12Desc.Dimension)
                    {
                    case D3D12_RESOURCE_DIMENSION_BUFFER:
                        break;

                    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
                    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
                    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
                        // TextureXD<Y>.MipSlice values map to same mem
                        dx12UavDescription.Texture2D.MipSlice = currentMipIndex;
                        break;

                    default:
                        FFX_ASSERT_MESSAGE(false, "Invalid View Dimension");
                        break;
                    }

                    cpuHandle = backendContext->descHeapUavGpuCpuStart;
                    cpuHandle.ptr += (backendResource->uavDescIndex + currentMipIndex) * backendContext->descriptorSizeCbvSrvUav;
                    dx12Device->CreateUnorderedAccessView(dx12Resource, 0, &dx12UavDescription, cpuHandle);

                    cpuHandle = backendContext->descHeapUavCpuStart;
                    cpuHandle.ptr += (backendResource->uavDescIndex + currentMipIndex) * backendContext->descriptorSizeCbvSrvUav;
                    dx12Device->CreateUnorderedAccessView(dx12Resource, 0, &dx12UavDescription, cpuHandle);
                }

                effectContext.nextDynamicUavDescriptor -= uavDescriptorsCount;
            }
        }
    }

    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenExternalResource GetResourceDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenResource inResource)
{
    FFX_ASSERT(nullptr != backendInterface);
    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;

    const ApusrFramegenResourceDescription resourceDescription = backendInterface->getResourceDescription(backendInterface, inResource);
    return {
        reinterpret_cast<void*>(backendContext->pResources[inResource.internalIndex].resourcePtr),
        resourceDescription,
        backendContext->pResources[inResource.internalIndex].currentState,
    };
}

// dispose dynamic resources: This should be called at the end of the frame
ApusrFramegenResult UnregisterResourcesDX12(ApusrFramegenBackendInterface* backendInterface, ApusrFramegenCommandList commandList, uint32_t effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);
    BackendContext_DX12* backendContext = (BackendContext_DX12*)(backendInterface->scratchBuffer);
    BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];

    // Walk back all the resources that don't belong to us and reset them to their initial state
    for (uint32_t resourceIndex = ++effectContext.nextDynamicResource; resourceIndex < (effectContextId * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT) + APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT; ++resourceIndex)
    {
        ApusrFramegenResource internalResource;
        internalResource.internalIndex = resourceIndex;

        BackendContext_DX12::Resource* backendResource = &backendContext->pResources[resourceIndex];
        addBarrier(backendContext, &internalResource, backendResource->initialState);
    }

    FFX_ASSERT(nullptr != commandList);
    ID3D12GraphicsCommandList* pCmdList = reinterpret_cast<ID3D12GraphicsCommandList*>(commandList);

    flushBarriers(backendContext, pCmdList);

    effectContext.nextDynamicResource      = (effectContextId * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT) + APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT - 1;
    effectContext.nextDynamicUavDescriptor = (effectContextId * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT) + APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT - 1;

    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResourceDescription GetResourceDescriptorDX12(
    ApusrFramegenBackendInterface* backendInterface,
    ApusrFramegenResource resource)
{
    FFX_ASSERT(NULL != backendInterface);

    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;

    ApusrFramegenResourceDescription resourceDescription = backendContext->pResources[resource.internalIndex].resourceDescription;
    return resourceDescription;
}

ApusrFramegenResult StageConstantBufferDataDX12(ApusrFramegenBackendInterface* backendInterface, void* data, uint32_t size, ApusrFramegenConstantBuffer* constantBuffer)
{
    FFX_ASSERT(NULL != backendInterface);
    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;

    if (data && constantBuffer)
    {
        if ((backendContext->stagingRingBufferBase + FFX_ALIGN_UP(size, 256)) >= APUSR_FRAMEGEN_CONSTANT_BUFFER_RING_SIZE)
            backendContext->stagingRingBufferBase = 0;

        uint32_t* dstPtr = (uint32_t*)(backendContext->pStagingRingBuffer + backendContext->stagingRingBufferBase);

        memcpy(dstPtr, data, size);

        constantBuffer->data            = dstPtr;
        constantBuffer->num32BitEntries = size / sizeof(uint32_t);

        backendContext->stagingRingBufferBase += FFX_ALIGN_UP(size, 256);

        return APUSR_FRAMEGEN_OK;
    }
    else
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;
}

D3D12_TEXTURE_ADDRESS_MODE apusrFramegenAddressModeDX12(const ApusrFramegenAddressMode addressMode)
{
    switch (addressMode)
    {
    case APUSR_FRAMEGEN_ADDRESS_WRAP:
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case APUSR_FRAMEGEN_ADDRESS_MIRROR:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case APUSR_FRAMEGEN_ADDRESS_CLAMP:
        return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case APUSR_FRAMEGEN_ADDRESS_BORDER:
        return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case APUSR_FRAMEGEN_ADDRESS_MIRROR_ONCE:
        return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default:
        FFX_ASSERT_MESSAGE(false, "Unsupported addressing mode requested. Please implement");
        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        break;
    }
}

ApusrFramegenResult CreatePipelineDX12(
    ApusrFramegenBackendInterface* backendInterface,
    ApusrFramegenShaderBlob* pShaderBlob,
    const ApusrFramegenPipelineDescription* pipelineDescription,
    uint32_t                     effectContextId,
    ApusrFramegenPipeline* outPipeline)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != pipelineDescription);

    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;
    ID3D12Device* dx12Device = backendContext->device;
    FFX_ASSERT(pShaderBlob->data && pShaderBlob->size);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->srvTextureCount <= APUSR_FRAMEGEN_MAX_SRVS, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->srvBufferCount <= APUSR_FRAMEGEN_MAX_SRVS, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->uavTextureCount <= APUSR_FRAMEGEN_MAX_UAVS, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->uavBufferCount <= APUSR_FRAMEGEN_MAX_UAVS, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);
    APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->cbvCount <= APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);

    bool hasAmdIntrinUav = false;


    static const uint32_t AmdIntrinsicsSpaceId = 2147420894;

    // Set up one root signature per pipeline. Execution caches identical consecutive binds.
    {
        FFX_ASSERT(pipelineDescription->samplerCount <= APUSR_FRAMEGEN_MAX_SAMPLERS);
        const size_t samplerCount = pipelineDescription->samplerCount;
        D3D12_STATIC_SAMPLER_DESC dx12SamplerDescriptions[APUSR_FRAMEGEN_MAX_SAMPLERS];
        for (uint32_t currentSamplerIndex = 0; currentSamplerIndex < samplerCount; ++currentSamplerIndex) {

            D3D12_STATIC_SAMPLER_DESC dx12SamplerDesc = {};

            dx12SamplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            dx12SamplerDesc.MinLOD = 0.f;
            dx12SamplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
            dx12SamplerDesc.MipLODBias = 0.f;
            dx12SamplerDesc.MaxAnisotropy = 16;
            dx12SamplerDesc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            dx12SamplerDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            dx12SamplerDesc.AddressU = apusrFramegenAddressModeDX12(pipelineDescription->samplers[currentSamplerIndex].addressModeU);
            dx12SamplerDesc.AddressV = apusrFramegenAddressModeDX12(pipelineDescription->samplers[currentSamplerIndex].addressModeV);
            dx12SamplerDesc.AddressW = apusrFramegenAddressModeDX12(pipelineDescription->samplers[currentSamplerIndex].addressModeW);

            switch (pipelineDescription->samplers[currentSamplerIndex].filter)
            {
            case APUSR_FRAMEGEN_FILTER_MINMAGMIP_POINT:
                dx12SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                break;
            case APUSR_FRAMEGEN_FILTER_MINMAGMIP_LINEAR:
                dx12SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                break;
            case APUSR_FRAMEGEN_FILTER_MINMAGLINEARMIP_POINT:
                dx12SamplerDesc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                break;

            default:
                FFX_ASSERT_MESSAGE(false, "Unsupported filter type requested. Please implement");
                break;
            }

            dx12SamplerDescriptions[currentSamplerIndex] = dx12SamplerDesc;
            dx12SamplerDescriptions[currentSamplerIndex].ShaderRegister = (UINT)currentSamplerIndex;
        }

        // storage for maximum number of descriptor ranges.
        const int32_t maximumDescriptorRangeSize = 3;
        D3D12_DESCRIPTOR_RANGE dx12Ranges[maximumDescriptorRangeSize] = {};
        int32_t currentDescriptorRangeIndex = 0;

        // storage for maximum number of root parameters.
        const int32_t maximumRootParameters = 10;
        D3D12_ROOT_PARAMETER dx12RootParameters[maximumRootParameters] = {};
        int32_t currentRootParameterIndex = 0;

        int32_t uavCount              = 0;
        int32_t maxUavSlotIndex       = 0;

        for (uint32_t uavIndex = 0; uavIndex < pShaderBlob->uavBufferCount; uavIndex++)
        {
            if (pShaderBlob->boundUAVBufferSpaces[uavIndex] == AmdIntrinsicsSpaceId)
            {
                // amd intrinsics space
                hasAmdIntrinUav = true;
                continue;
            }
            APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->boundUAVBufferSpaces[uavIndex] == 0, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);

            uint32_t bindCount = pShaderBlob->boundUAVBufferCounts[uavIndex];

            uavCount += bindCount;

            if (pShaderBlob->boundUAVBuffers[uavIndex] > uint32_t(maxUavSlotIndex))
                maxUavSlotIndex = pShaderBlob->boundUAVBuffers[uavIndex] + (bindCount - 1);
        }

        for (uint32_t uavIndex = 0; uavIndex < pShaderBlob->uavTextureCount; uavIndex++)
        {
            APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->boundUAVTextureSpaces[uavIndex] == 0, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);

            uint32_t bindCount = pShaderBlob->boundUAVTextureCounts[uavIndex];

            uavCount += bindCount;

            if (pShaderBlob->boundUAVTextures[uavIndex] > uint32_t(maxUavSlotIndex))
                maxUavSlotIndex = pShaderBlob->boundUAVTextures[uavIndex] + (bindCount - 1);
        }

        if (uavCount > 0)
            uavCount = (maxUavSlotIndex + 1) > uavCount ? (maxUavSlotIndex + 1) : uavCount;

        int32_t srvCount              = 0;
        int32_t maxSrvSlotIndex       = 0;

        for (uint32_t srvIndex = 0; srvIndex < pShaderBlob->srvBufferCount; srvIndex++)
        {
            APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->boundSRVBufferSpaces[srvIndex] == 0, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);

            uint32_t bindCount = pShaderBlob->boundSRVBufferCounts[srvIndex];

            srvCount += bindCount;

            if (pShaderBlob->boundSRVBuffers[srvIndex] > uint32_t(maxSrvSlotIndex))
                maxSrvSlotIndex = pShaderBlob->boundSRVBuffers[srvIndex] + (bindCount - 1);
        }

        for (uint32_t srvIndex = 0; srvIndex < pShaderBlob->srvTextureCount; srvIndex++)
        {
            APUSR_FRAMEGEN_RETURN_ON_ERROR(pShaderBlob->boundSRVTextureSpaces[srvIndex] == 0, APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT);

            uint32_t bindCount = pShaderBlob->boundSRVTextureCounts[srvIndex];

            srvCount += bindCount;

            if (pShaderBlob->boundSRVTextures[srvIndex] > uint32_t(maxSrvSlotIndex))
                maxSrvSlotIndex = pShaderBlob->boundSRVTextures[srvIndex] + (bindCount - 1);
        }

        if (srvCount > 0)
            srvCount = (maxSrvSlotIndex + 1) > srvCount ? (maxSrvSlotIndex + 1) : srvCount;

        if (uavCount > 0) {

            FFX_ASSERT(currentDescriptorRangeIndex < maximumDescriptorRangeSize);
            D3D12_DESCRIPTOR_RANGE* dx12DescriptorRange = &dx12Ranges[currentDescriptorRangeIndex];
            memset(dx12DescriptorRange, 0, sizeof(D3D12_DESCRIPTOR_RANGE));
            dx12DescriptorRange->OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            dx12DescriptorRange->RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            dx12DescriptorRange->BaseShaderRegister = 0;
            dx12DescriptorRange->NumDescriptors = uavCount;
            currentDescriptorRangeIndex++;

            FFX_ASSERT(currentRootParameterIndex < maximumRootParameters);
            D3D12_ROOT_PARAMETER* dx12RootParameterSlot = &dx12RootParameters[currentRootParameterIndex];
            memset(dx12RootParameterSlot, 0, sizeof(D3D12_ROOT_PARAMETER));
            dx12RootParameterSlot->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            dx12RootParameterSlot->DescriptorTable.NumDescriptorRanges = 1;
            currentRootParameterIndex++;
        }

        if (srvCount > 0) {

            FFX_ASSERT(currentDescriptorRangeIndex < maximumDescriptorRangeSize);
            D3D12_DESCRIPTOR_RANGE* dx12DescriptorRange = &dx12Ranges[currentDescriptorRangeIndex];
            memset(dx12DescriptorRange, 0, sizeof(D3D12_DESCRIPTOR_RANGE));
            dx12DescriptorRange->OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            dx12DescriptorRange->RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            dx12DescriptorRange->BaseShaderRegister = 0;
            dx12DescriptorRange->NumDescriptors = srvCount;
            currentDescriptorRangeIndex++;

            FFX_ASSERT(currentRootParameterIndex < maximumRootParameters);
            D3D12_ROOT_PARAMETER* dx12RootParameterSlot = &dx12RootParameters[currentRootParameterIndex];
            memset(dx12RootParameterSlot, 0, sizeof(D3D12_ROOT_PARAMETER));
            dx12RootParameterSlot->ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            dx12RootParameterSlot->DescriptorTable.NumDescriptorRanges = 1;
            currentRootParameterIndex++;
        }

        // Setup the descriptor table bindings for the above
        for (int32_t currentRangeIndex = 0; currentRangeIndex < currentDescriptorRangeIndex; currentRangeIndex++) {

            dx12RootParameters[currentRangeIndex].DescriptorTable.pDescriptorRanges = &dx12Ranges[currentRangeIndex];
        }

        for (int32_t currentRootConstantIndex = 0; currentRootConstantIndex < (int32_t)pShaderBlob->cbvCount; currentRootConstantIndex++)
        {
            FFX_ASSERT(currentRootParameterIndex < maximumRootParameters);
            D3D12_ROOT_PARAMETER* rootParameterSlot = &dx12RootParameters[currentRootParameterIndex];
            memset(rootParameterSlot, 0, sizeof(D3D12_ROOT_PARAMETER));
            rootParameterSlot->ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParameterSlot->Constants.ShaderRegister = pShaderBlob->boundConstantBuffers[currentRootConstantIndex];
            currentRootParameterIndex++;
        }

        if (hasAmdIntrinUav)
        {
            D3D12_ROOT_PARAMETER* slot = &dx12RootParameters[currentRootParameterIndex];
            slot->ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            slot->Descriptor.RegisterSpace = AmdIntrinsicsSpaceId;
            slot->Descriptor.ShaderRegister = 0;
            slot->ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            currentRootParameterIndex++;
        }

        D3D12_ROOT_SIGNATURE_DESC dx12RootSignatureDescription = {};
        dx12RootSignatureDescription.NumParameters = currentRootParameterIndex;
        dx12RootSignatureDescription.pParameters = dx12RootParameters;
        dx12RootSignatureDescription.NumStaticSamplers = (UINT)samplerCount;
        dx12RootSignatureDescription.pStaticSamplers = dx12SamplerDescriptions;

        ID3DBlob* outBlob = nullptr;
        ID3DBlob* errorBlob = nullptr;

        //Query D3D12SerializeRootSignature from d3d12.dll handle
        typedef HRESULT(__stdcall* D3D12SerializeRootSignatureType)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

        //Do not pass hD3D12 handle to the FreeLibrary function, as GetModuleHandle will not increment refcount
        HMODULE d3d12ModuleHandle = GetModuleHandleW(L"D3D12.dll");

        if (NULL != d3d12ModuleHandle) {

            D3D12SerializeRootSignatureType dx12SerializeRootSignatureType = (D3D12SerializeRootSignatureType)GetProcAddress(d3d12ModuleHandle, "D3D12SerializeRootSignature");

            if (nullptr != dx12SerializeRootSignatureType) {
                
                HRESULT result = dx12SerializeRootSignatureType(&dx12RootSignatureDescription, D3D_ROOT_SIGNATURE_VERSION_1, &outBlob, &errorBlob);
                if (errorBlob != nullptr) {

                    errorBlob->Release();
                } 
                if (FAILED(result)) {

                    if (outBlob != nullptr) {
                        
                        outBlob->Release();
                    }  
                    return APUSR_FRAMEGEN_ERROR_BACKEND_API;
                }

                result = dx12Device->CreateRootSignature(0, outBlob->GetBufferPointer(), outBlob->GetBufferSize(), IID_PPV_ARGS(reinterpret_cast<ID3D12RootSignature**>(&outPipeline->rootSignature)));
                if (outBlob != nullptr) {
                    
                    outBlob->Release();
                }  
                if (FAILED(result)) {

                    return APUSR_FRAMEGEN_ERROR_BACKEND_API;
                }
            } else {
                return APUSR_FRAMEGEN_ERROR_BACKEND_API;
            }
        } else {
            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
    }

    ID3D12RootSignature* dx12RootSignature = reinterpret_cast<ID3D12RootSignature*>(outPipeline->rootSignature);

    // Only set the command signature if this is setup as an indirect workload
    if (pipelineDescription->indirectWorkload)
    {
        D3D12_INDIRECT_ARGUMENT_DESC argumentDescs = { D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH };
        D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc = {};
        commandSignatureDesc.pArgumentDescs = &argumentDescs;
        commandSignatureDesc.NumArgumentDescs = 1;
        commandSignatureDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);

        HRESULT result = dx12Device->CreateCommandSignature(&commandSignatureDesc, nullptr, IID_PPV_ARGS(reinterpret_cast<ID3D12CommandSignature**>(&outPipeline->cmdSignature)));
        if (FAILED(result)) {

            return APUSR_FRAMEGEN_ERROR_BACKEND_API;
        }
    }
    else
    {
        outPipeline->cmdSignature = nullptr;
    }

    uint32_t flattenedSrvTextureCount = 0;

    for (uint32_t srvIndex = 0; srvIndex < pShaderBlob->srvTextureCount; ++srvIndex)
    {
        uint32_t slotIndex  = pShaderBlob->boundSRVTextures[srvIndex];
        uint32_t spaceIndex = pShaderBlob->boundSRVTextureSpaces[srvIndex];
        uint32_t bindCount  = pShaderBlob->boundSRVTextureCounts[srvIndex];

        for (uint32_t arrayIndex = 0; arrayIndex < bindCount; arrayIndex++)
        {
            uint32_t bindingIndex = flattenedSrvTextureCount++;

            outPipeline->srvTextureBindings[bindingIndex].slotIndex  = slotIndex;
            outPipeline->srvTextureBindings[bindingIndex].arrayIndex = arrayIndex;
            outPipeline->srvTextureBindings[bindingIndex].name = pShaderBlob->boundSRVTextureNames[srvIndex];
        }
    }

    outPipeline->srvTextureCount = flattenedSrvTextureCount;
    FFX_ASSERT(outPipeline->srvTextureCount < APUSR_FRAMEGEN_MAX_SRVS);

    uint32_t flattenedUavTextureCount = 0;

    for (uint32_t uavIndex = 0; uavIndex < pShaderBlob->uavTextureCount; ++uavIndex)
    {
        uint32_t slotIndex  = pShaderBlob->boundUAVTextures[uavIndex];
        uint32_t spaceIndex = pShaderBlob->boundUAVTextureSpaces[uavIndex];
        uint32_t bindCount  = pShaderBlob->boundUAVTextureCounts[uavIndex];

        for (uint32_t arrayIndex = 0; arrayIndex < bindCount; arrayIndex++)
        {
            uint32_t bindingIndex = flattenedUavTextureCount++;

            outPipeline->uavTextureBindings[bindingIndex].slotIndex  = slotIndex;
            outPipeline->uavTextureBindings[bindingIndex].arrayIndex = arrayIndex;
            outPipeline->uavTextureBindings[bindingIndex].name = pShaderBlob->boundUAVTextureNames[uavIndex];
        }
    }

    outPipeline->uavTextureCount = flattenedUavTextureCount;
    FFX_ASSERT(outPipeline->uavTextureCount < APUSR_FRAMEGEN_MAX_UAVS);

    uint32_t flattenedSrvBufferCount = 0;

    for (uint32_t srvIndex = 0; srvIndex < pShaderBlob->srvBufferCount; ++srvIndex)
    {
        uint32_t slotIndex  = pShaderBlob->boundSRVBuffers[srvIndex];
        uint32_t spaceIndex = pShaderBlob->boundSRVBufferSpaces[srvIndex];
        uint32_t bindCount  = pShaderBlob->boundSRVBufferCounts[srvIndex];

        for (uint32_t arrayIndex = 0; arrayIndex < bindCount; arrayIndex++)
        {
            uint32_t bindingIndex = flattenedSrvBufferCount++;

            outPipeline->srvBufferBindings[bindingIndex].slotIndex  = slotIndex;
            outPipeline->srvBufferBindings[bindingIndex].arrayIndex = arrayIndex;
            outPipeline->srvBufferBindings[bindingIndex].name = pShaderBlob->boundSRVBufferNames[srvIndex];
        }
    }

    outPipeline->srvBufferCount = flattenedSrvBufferCount;
    FFX_ASSERT(outPipeline->srvBufferCount < APUSR_FRAMEGEN_MAX_SRVS);

    uint32_t flattenedUavBufferCount = 0;

    for (uint32_t uavIndex = 0; uavIndex < pShaderBlob->uavBufferCount; ++uavIndex)
    {
        uint32_t slotIndex  = pShaderBlob->boundUAVBuffers[uavIndex];
        uint32_t spaceIndex = pShaderBlob->boundUAVBufferSpaces[uavIndex];
        uint32_t bindCount  = pShaderBlob->boundUAVBufferCounts[uavIndex];

        // Skip static resources and intrinsics
        if (spaceIndex == AmdIntrinsicsSpaceId)
            continue;

        for (uint32_t arrayIndex = 0; arrayIndex < bindCount; arrayIndex++)
        {
            uint32_t bindingIndex = flattenedUavBufferCount++;

            outPipeline->uavBufferBindings[bindingIndex].slotIndex  = slotIndex;
            outPipeline->uavBufferBindings[bindingIndex].arrayIndex = arrayIndex;
            outPipeline->uavBufferBindings[bindingIndex].name = pShaderBlob->boundUAVBufferNames[uavIndex];
        }
    }

    outPipeline->uavBufferCount = flattenedUavBufferCount;
    FFX_ASSERT(outPipeline->uavBufferCount < APUSR_FRAMEGEN_MAX_UAVS);

    const uint32_t dynamicUavCount = outPipeline->uavTextureCount + outPipeline->uavBufferCount;
    outPipeline->uavDescriptorTableSize = dynamicUavCount ? dynamicUavCount + 1 : 0;
    for (uint32_t i = 0; i < outPipeline->uavTextureCount; ++i)
    {
        outPipeline->uavDescriptorTableSize = (std::max)(outPipeline->uavDescriptorTableSize,
            outPipeline->uavTextureBindings[i].slotIndex + outPipeline->uavTextureBindings[i].arrayIndex + 1);
    }
    for (uint32_t i = 0; i < outPipeline->uavBufferCount; ++i)
    {
        outPipeline->uavDescriptorTableSize = (std::max)(outPipeline->uavDescriptorTableSize,
            outPipeline->uavBufferBindings[i].slotIndex + outPipeline->uavBufferBindings[i].arrayIndex + 1);
    }

    const uint32_t dynamicSrvCount = outPipeline->srvTextureCount + outPipeline->srvBufferCount;
    outPipeline->srvDescriptorTableSize = dynamicSrvCount ? dynamicSrvCount + 1 : 0;
    for (uint32_t i = 0; i < outPipeline->srvTextureCount; ++i)
    {
        outPipeline->srvDescriptorTableSize = (std::max)(outPipeline->srvDescriptorTableSize,
            outPipeline->srvTextureBindings[i].slotIndex + outPipeline->srvTextureBindings[i].arrayIndex + 1);
    }
    for (uint32_t i = 0; i < outPipeline->srvBufferCount; ++i)
    {
        outPipeline->srvDescriptorTableSize = (std::max)(outPipeline->srvDescriptorTableSize,
            outPipeline->srvBufferBindings[i].slotIndex + outPipeline->srvBufferBindings[i].arrayIndex + 1);
    }

    for (uint32_t cbIndex = 0; cbIndex < pShaderBlob->cbvCount; ++cbIndex)
    {
        outPipeline->constantBufferBindings[cbIndex].slotIndex  = pShaderBlob->boundConstantBuffers[cbIndex];
        outPipeline->constantBufferBindings[cbIndex].arrayIndex = 1;
        outPipeline->constantBufferBindings[cbIndex].name = pShaderBlob->boundConstantBufferNames[cbIndex];
    }

    outPipeline->constCount = pShaderBlob->cbvCount;
    FFX_ASSERT(outPipeline->constCount < APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS);

    BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];


    // create the PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC dx12PipelineStateDescription = {};
    dx12PipelineStateDescription.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    dx12PipelineStateDescription.pRootSignature = dx12RootSignature;
    dx12PipelineStateDescription.CS.pShaderBytecode = pShaderBlob->data;
    dx12PipelineStateDescription.CS.BytecodeLength = pShaderBlob->size;

    if (FAILED(dx12Device->CreateComputePipelineState(&dx12PipelineStateDescription, IID_PPV_ARGS(reinterpret_cast<ID3D12PipelineState**>(&outPipeline->pipeline)))))
        return APUSR_FRAMEGEN_ERROR_BACKEND_API;

    // Set the pipeline name
    reinterpret_cast<ID3D12PipelineState*>(outPipeline->pipeline)->SetName(pipelineDescription->name);
    wcscpy_s(outPipeline->name, pipelineDescription->name);
    outPipeline->shaderEffect = static_cast<std::uint8_t>(pShaderBlob->effect);
    outPipeline->shaderPassId = pShaderBlob->passId;
    outPipeline->shaderPermutationOptions = pShaderBlob->permutationOptions;
    outPipeline->shaderByteSize = static_cast<std::uint32_t>((std::min)(
        static_cast<std::size_t>(pShaderBlob->size), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    outPipeline->requiredSubgroupSize = 0u;

    if (backendContext->profileStream && apusr::profile::gpuEnabled()) {
        char profileName[48]{};
        std::size_t out = 0u;
        while (out + 1u < std::size(profileName) && pipelineDescription->name[out] != L'\0') {
            const wchar_t c = pipelineDescription->name[out];
            profileName[out] = (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '?';
            ++out;
        }
        profileName[out] = '\0';
        std::uint64_t workloadId = apusr::profile::hashString(profileName);
        workloadId = apusr::profile::hashCombine(workloadId, static_cast<std::uint64_t>(pShaderBlob->effect));
        workloadId = apusr::profile::hashCombine(workloadId, pShaderBlob->passId);
        workloadId = apusr::profile::hashCombine(workloadId, pShaderBlob->permutationOptions);
        apusr::profile::recordGpuWorkloadInfo(apusr::profile::GpuWorkloadInfo{
            .stream = backendContext->profileStream,
            .name = profileName,
            .detail = "dxil.compute",
            .workloadId = workloadId,
            .nativePipeline = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(outPipeline->pipeline)),
            .codeHash = apusr::profile::hashString(std::string_view(
                reinterpret_cast<const char*>(pShaderBlob->data), pShaderBlob->size)),
            .codeBytes = pShaderBlob->size,
            .flags = pShaderBlob->permutationOptions,
        });
    }

    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult DestroyPipelineDX12(
    ApusrFramegenBackendInterface* backendInterface,
    ApusrFramegenPipeline* pipeline,
    uint32_t)
{
    FFX_ASSERT(backendInterface != nullptr);
    if (!pipeline) {
        return APUSR_FRAMEGEN_OK;
    }

    // destroy Rootsignature
    ID3D12RootSignature* dx12RootSignature = reinterpret_cast<ID3D12RootSignature*>(pipeline->rootSignature);
    if (dx12RootSignature) {
        dx12RootSignature->Release();
    }
    pipeline->rootSignature = nullptr;

    // destroy CmdSignature
    ID3D12CommandSignature* dx12CmdSignature = reinterpret_cast<ID3D12CommandSignature*>(pipeline->cmdSignature);
    if (dx12CmdSignature) {
        dx12CmdSignature->Release();
    }
    pipeline->cmdSignature = nullptr;

    // destroy pipeline
    ID3D12PipelineState* dx12Pipeline = reinterpret_cast<ID3D12PipelineState*>(pipeline->pipeline);
    if (dx12Pipeline) {
        dx12Pipeline->Release();
    }
    pipeline->pipeline = nullptr;

    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult ScheduleGpuJobDX12(
    ApusrFramegenBackendInterface* backendInterface,
    const ApusrFramegenGpuJob* job
)
{
    FFX_ASSERT(NULL != backendInterface);
    FFX_ASSERT(NULL != job);

    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;

    FFX_ASSERT(backendContext->gpuJobCount < APUSR_FRAMEGEN_MAX_GPU_JOBS);

    ApusrFramegenGpuJob& queuedJob = backendContext->pGpuJobs[backendContext->gpuJobCount++];
    queuedJob.jobType = job->jobType;

#if FFX_DX12_ENABLE_GPU_MARKERS
    APUSR_FRAMEGEN_SET_JOB_LABEL(queuedJob, job->jobLabel);
#endif

    switch (job->jobType)
    {
    case APUSR_FRAMEGEN_JOB_COMPUTE:
    {
        const ApusrFramegenComputeJob& source = job->computeJob;
        ApusrFramegenComputeJob& destination = queuedJob.computeJob;
        const ApusrFramegenPipeline* pipeline = source.pipeline;
        FFX_ASSERT(pipeline != nullptr);

        destination.pipeline = pipeline;
        destination.dimensions[0] = source.dimensions[0];
        destination.dimensions[1] = source.dimensions[1];
        destination.dimensions[2] = source.dimensions[2];
        destination.cmdArgument = source.cmdArgument;
        destination.cmdArgumentOffset = source.cmdArgumentOffset;
        destination.flags = source.flags;
        destination.profileLevel = source.profileLevel;

        memcpy(destination.srvTextures, source.srvTextures, pipeline->srvTextureCount * sizeof(ApusrFramegenTextureSrv));
        memcpy(destination.srvBuffers, source.srvBuffers, pipeline->srvBufferCount * sizeof(ApusrFramegenBufferSrv));
        memcpy(destination.uavTextures, source.uavTextures, pipeline->uavTextureCount * sizeof(ApusrFramegenTextureUav));
        memcpy(destination.uavBuffers, source.uavBuffers, pipeline->uavBufferCount * sizeof(ApusrFramegenBufferUav));
        memcpy(destination.constantBuffers, source.constantBuffers, pipeline->constCount * sizeof(ApusrFramegenConstantBuffer));
        break;
    }
    case APUSR_FRAMEGEN_JOB_CLEAR_FLOAT:
        queuedJob.clearJob = job->clearJob;
        break;
    case APUSR_FRAMEGEN_JOB_COPY:
        queuedJob.copyJob = job->copyJob;
        break;
    case APUSR_FRAMEGEN_JOB_BARRIER:
        queuedJob.barrierJob = job->barrierJob;
        break;
    case APUSR_FRAMEGEN_JOB_DISCARD:
        queuedJob.discardJob = job->discardJob;
        break;
    default:
        break;
    }

    return APUSR_FRAMEGEN_OK;
}

struct FallbackConstantAllocationCacheEntry
{
    const uint32_t*      data = nullptr;
    uint32_t             num32BitEntries = 0;
    ApusrFramegenConstantAllocation allocation{};
};

static ApusrFramegenResult executeGpuJobCompute(BackendContext_DX12*       backendContext,
                                         ApusrFramegenGpuJob*      job,
                                         ID3D12GraphicsCommandList* dx12CommandList,
                                         uint32_t                  effectContextId,
                                         ID3D12DescriptorHeap*&     boundDescriptorHeap,
                                         ID3D12RootSignature*&      boundComputeRootSignature,
                                         ID3D12PipelineState*&      boundComputePipelineState,
                                         FallbackConstantAllocationCacheEntry (&fallbackConstantCache)[APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS])
{
    ID3D12Device* dx12Device = reinterpret_cast<ID3D12Device*>(backendContext->device);
    const ApusrFramegenPipeline* pipelinePtr = job->computeJob.pipeline;
    FFX_ASSERT(pipelinePtr != nullptr);
    const ApusrFramegenPipeline& pipeline = *pipelinePtr;
 
    // Set descriptor head for binding
    ID3D12DescriptorHeap* dx12DescriptorHeap = reinterpret_cast<ID3D12DescriptorHeap*>(backendContext->descRingBuffer);
    const uint32_t descriptorSize = backendContext->descriptorSizeCbvSrvUav;
    const D3D12_CPU_DESCRIPTOR_HANDLE ringCpuStart = backendContext->descRingCpuStart;
    const D3D12_GPU_DESCRIPTOR_HANDLE ringGpuStart = backendContext->descRingGpuStart;
    const D3D12_CPU_DESCRIPTOR_HANDLE srvCpuStart = backendContext->descHeapSrvCpuStart;
    const D3D12_CPU_DESCRIPTOR_HANDLE uavCpuStart = backendContext->descHeapUavCpuStart;

    ID3D12RootSignature* dx12RootSignature = reinterpret_cast<ID3D12RootSignature*>(pipeline.rootSignature);
    if (boundComputeRootSignature != dx12RootSignature)
    {
        dx12CommandList->SetComputeRootSignature(dx12RootSignature);
        boundComputeRootSignature = dx12RootSignature;
    }

    // Keep descriptor-heap binds stable across consecutive compute jobs.
    setDescriptorHeapIfNeeded(dx12CommandList, dx12DescriptorHeap, boundDescriptorHeap);

    uint32_t descriptorTableIndex = 0;

    // bind texture & buffer UAVs (note the binding order here MUST match the root signature mapping order from CreatePipeline!)
    {
        const uint32_t uavDescriptorTableSize = pipeline.uavDescriptorTableSize;

        if (uavDescriptorTableSize)
        {
            // check if this fits into the ringbuffer, loop if not fitting
            if (backendContext->descRingBufferBase + uavDescriptorTableSize > APUSR_FRAMEGEN_DESCRIPTOR_RING_SIZE * backendContext->maxEffectContexts)
                backendContext->descRingBufferBase = 0;

            D3D12_GPU_DESCRIPTOR_HANDLE gpuView = ringGpuStart;
            gpuView.ptr += backendContext->descRingBufferBase * descriptorSize;

            D3D12_CPU_DESCRIPTOR_HANDLE descriptorDestinations[APUSR_FRAMEGEN_MAX_UAVS * 2];
            D3D12_CPU_DESCRIPTOR_HANDLE descriptorSources[APUSR_FRAMEGEN_MAX_UAVS * 2];
            uint32_t descriptorCopyCount = 0;

            // Set Texture UAVs
            for (uint32_t currentPipelineUavIndex = 0; currentPipelineUavIndex < pipeline.uavTextureCount; ++currentPipelineUavIndex) {

                if((job->computeJob.flags & APUSR_FRAMEGEN_JOB_FLAG_SKIP_BARRIERS) == 0)
                {
                    addBarrier(backendContext, &job->computeJob.uavTextures[currentPipelineUavIndex].resource, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS);
                }

                const ApusrFramegenResourceBinding binding = pipeline.uavTextureBindings[currentPipelineUavIndex];

                // source: UAV of resource to bind
                const uint32_t resourceIndex = job->computeJob.uavTextures[currentPipelineUavIndex].resource.internalIndex;
                const uint32_t uavIndex = backendContext->pResources[resourceIndex].uavDescIndex + job->computeJob.uavTextures[currentPipelineUavIndex].mip;

                // where to bind it
                const uint32_t currentUavResourceIndex = binding.slotIndex + binding.arrayIndex;

                D3D12_CPU_DESCRIPTOR_HANDLE srcHandle = uavCpuStart;
                srcHandle.ptr += uavIndex * descriptorSize;

                D3D12_CPU_DESCRIPTOR_HANDLE cpuView = ringCpuStart;
                cpuView.ptr += backendContext->descRingBufferBase * descriptorSize;
                cpuView.ptr += currentUavResourceIndex * descriptorSize;

                descriptorDestinations[descriptorCopyCount] = cpuView;
                descriptorSources[descriptorCopyCount] = srcHandle;
                ++descriptorCopyCount;
            }

            // Set Buffer UAVs
            for (uint32_t currentPipelineUavIndex = 0; currentPipelineUavIndex < pipeline.uavBufferCount; ++currentPipelineUavIndex) {
                
                // continue if this is a null resource.
                if (job->computeJob.uavBuffers[currentPipelineUavIndex].resource.internalIndex == 0)
                    continue;

                if((job->computeJob.flags & APUSR_FRAMEGEN_JOB_FLAG_SKIP_BARRIERS) == 0)
                {
                    addBarrier(backendContext, &job->computeJob.uavBuffers[currentPipelineUavIndex].resource, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS);
                }

                const ApusrFramegenResourceBinding binding = pipeline.uavBufferBindings[currentPipelineUavIndex];

                // where to bind it
                const uint32_t currentUavResourceIndex = binding.slotIndex + binding.arrayIndex;

                D3D12_CPU_DESCRIPTOR_HANDLE cpuView = ringCpuStart;
                cpuView.ptr += backendContext->descRingBufferBase * descriptorSize;
                cpuView.ptr += currentUavResourceIndex * descriptorSize;

                if (job->computeJob.uavBuffers[currentPipelineUavIndex].size > 0)
                {
                    // if size is non-zero create a dynamic descriptor directly on the GPU heap
                    ID3D12Resource* buffer = getDX12ResourcePtr(backendContext, job->computeJob.uavBuffers[currentPipelineUavIndex].resource.internalIndex);
                    FFX_ASSERT(buffer != NULL);

                    D3D12_UNORDERED_ACCESS_VIEW_DESC dx12UavDescription = {};

                    bool     isStructured = job->computeJob.uavBuffers[currentPipelineUavIndex].stride > 0;
                    uint32_t stride       = isStructured ? job->computeJob.uavBuffers[currentPipelineUavIndex].stride : sizeof(uint32_t);

                    dx12UavDescription.Format                      = isStructured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                    dx12UavDescription.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
                    dx12UavDescription.Buffer.FirstElement         = job->computeJob.uavBuffers[currentPipelineUavIndex].offset / stride;
                    dx12UavDescription.Buffer.NumElements          = job->computeJob.uavBuffers[currentPipelineUavIndex].size / stride;
                    dx12UavDescription.Buffer.StructureByteStride  = isStructured ? stride : 0;
                    dx12UavDescription.Buffer.CounterOffsetInBytes = 0;
                    dx12UavDescription.Buffer.Flags                = isStructured ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW;

                    dx12Device->CreateUnorderedAccessView(buffer, 0, &dx12UavDescription, cpuView);
                }
                else
                {
                    // if size is zero assume it is a static descriptor and copy it from the CPU heap
                    const uint32_t resourceIndex = job->computeJob.uavBuffers[currentPipelineUavIndex].resource.internalIndex;
          
                    const uint32_t uavIndex  = backendContext->pResources[resourceIndex].uavDescIndex;

                    D3D12_CPU_DESCRIPTOR_HANDLE srcHandle = uavCpuStart;
                    srcHandle.ptr += uavIndex * descriptorSize;

                    descriptorDestinations[descriptorCopyCount] = cpuView;
                    descriptorSources[descriptorCopyCount] = srcHandle;
                    ++descriptorCopyCount;
                }
            }

            copyDescriptorsBatched(dx12Device, descriptorCopyCount, descriptorDestinations, descriptorSources);

            backendContext->descRingBufferBase += uavDescriptorTableSize;
            dx12CommandList->SetComputeRootDescriptorTable(descriptorTableIndex++, gpuView);
        }
    }

    // bind texture & buffer SRVs
    {
        // Set a baseline minimal value
        const uint32_t srvDescriptorTableSize = pipeline.srvDescriptorTableSize;

        if (srvDescriptorTableSize)
        {
            // check if this fits into the ringbuffer, loop if not fitting
            if (backendContext->descRingBufferBase + srvDescriptorTableSize > APUSR_FRAMEGEN_DESCRIPTOR_RING_SIZE * backendContext->maxEffectContexts)
            {
                backendContext->descRingBufferBase = 0;
            }

            D3D12_GPU_DESCRIPTOR_HANDLE gpuView = ringGpuStart;
            gpuView.ptr += backendContext->descRingBufferBase * descriptorSize;

            D3D12_CPU_DESCRIPTOR_HANDLE descriptorDestinations[APUSR_FRAMEGEN_MAX_SRVS * 2];
            D3D12_CPU_DESCRIPTOR_HANDLE descriptorSources[APUSR_FRAMEGEN_MAX_SRVS * 2];
            uint32_t descriptorCopyCount = 0;

            for (uint32_t currentPipelineSrvIndex = 0; currentPipelineSrvIndex < pipeline.srvTextureCount; ++currentPipelineSrvIndex)
            {
                if (job->computeJob.srvTextures[currentPipelineSrvIndex].resource.internalIndex == 0)
                    break;

                if((job->computeJob.flags & APUSR_FRAMEGEN_JOB_FLAG_SKIP_BARRIERS) == 0)
                {
                    addBarrier(backendContext, &job->computeJob.srvTextures[currentPipelineSrvIndex].resource, APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ);
                }

                const ApusrFramegenResourceBinding binding = pipeline.srvTextureBindings[currentPipelineSrvIndex];

                // source: SRV of resource to bind
                const uint32_t              resourceIndex = job->computeJob.srvTextures[currentPipelineSrvIndex].resource.internalIndex;
                D3D12_CPU_DESCRIPTOR_HANDLE srcHandle     = srvCpuStart;
                srcHandle.ptr += resourceIndex * descriptorSize;

                // Where to bind it
                uint32_t currentSrvResourceIndex = binding.slotIndex + binding.arrayIndex;

                D3D12_CPU_DESCRIPTOR_HANDLE cpuView = ringCpuStart;
                cpuView.ptr += backendContext->descRingBufferBase * descriptorSize;
                cpuView.ptr += currentSrvResourceIndex * descriptorSize;

                descriptorDestinations[descriptorCopyCount] = cpuView;
                descriptorSources[descriptorCopyCount] = srcHandle;
                ++descriptorCopyCount;
            }

            // Set Buffer SRVs
            for (uint32_t currentPipelineSrvIndex = 0; currentPipelineSrvIndex < pipeline.srvBufferCount; ++currentPipelineSrvIndex)
            {
                // continue if this is a null resource.
                if (job->computeJob.srvBuffers[currentPipelineSrvIndex].resource.internalIndex == 0)
                    continue;

                if((job->computeJob.flags & APUSR_FRAMEGEN_JOB_FLAG_SKIP_BARRIERS) == 0)
                {
                    addBarrier(backendContext, &job->computeJob.srvBuffers[currentPipelineSrvIndex].resource, APUSR_FRAMEGEN_RESOURCE_STATE_COMPUTE_READ);
                }

                const ApusrFramegenResourceBinding binding = pipeline.srvBufferBindings[currentPipelineSrvIndex];

                // where to bind it
                const uint32_t currentSrvResourceIndex = binding.slotIndex + binding.arrayIndex;

                D3D12_CPU_DESCRIPTOR_HANDLE cpuView = ringCpuStart;
                cpuView.ptr += backendContext->descRingBufferBase * descriptorSize;
                cpuView.ptr += currentSrvResourceIndex * descriptorSize;

                if (job->computeJob.srvBuffers[currentPipelineSrvIndex].size > 0)
                {
                    // if size is non-zero create a dynamic descriptor directly on the GPU heap
                    ID3D12Resource* buffer = getDX12ResourcePtr(backendContext, job->computeJob.srvBuffers[currentPipelineSrvIndex].resource.internalIndex);
                    FFX_ASSERT(buffer != NULL);

                    D3D12_SHADER_RESOURCE_VIEW_DESC dx12SrvDescription = {};

                    bool     isStructured = job->computeJob.srvBuffers[currentPipelineSrvIndex].stride > 0;
                    uint32_t stride       = isStructured ? job->computeJob.srvBuffers[currentPipelineSrvIndex].stride : sizeof(uint32_t);

                    dx12SrvDescription.Format                     = isStructured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
                    dx12SrvDescription.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
                    dx12SrvDescription.Buffer.FirstElement        = job->computeJob.srvBuffers[currentPipelineSrvIndex].offset / stride;
                    dx12SrvDescription.Buffer.NumElements         = job->computeJob.srvBuffers[currentPipelineSrvIndex].size / stride;
                    dx12SrvDescription.Buffer.StructureByteStride = isStructured ? stride : 0;
                    dx12SrvDescription.Buffer.Flags               = isStructured ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW;
                    dx12SrvDescription.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

                    dx12Device->CreateShaderResourceView(buffer, &dx12SrvDescription, cpuView);
                }
                else
                {
                    // source: SRV of buffer to bind
                    const uint32_t resourceIndex = job->computeJob.srvBuffers[currentPipelineSrvIndex].resource.internalIndex;

                    D3D12_CPU_DESCRIPTOR_HANDLE srcHandle = srvCpuStart;
                    srcHandle.ptr += resourceIndex * descriptorSize;

                    descriptorDestinations[descriptorCopyCount] = cpuView;
                    descriptorSources[descriptorCopyCount] = srcHandle;
                    ++descriptorCopyCount;
                }
            }

            copyDescriptorsBatched(dx12Device, descriptorCopyCount, descriptorDestinations, descriptorSources);

            backendContext->descRingBufferBase += srvDescriptorTableSize;
            dx12CommandList->SetComputeRootDescriptorTable(descriptorTableIndex++, gpuView);
        }
    }

    BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];

    // If we are dispatching indirectly, transition the argument resource to indirect argument
    if (pipeline.cmdSignature)
    {
        addBarrier(backendContext, &job->computeJob.cmdArgument, APUSR_FRAMEGEN_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }

    flushBarriers(backendContext, dx12CommandList);

    // bind pipeline
    ID3D12PipelineState* dx12PipelineStateObject = reinterpret_cast<ID3D12PipelineState*>(pipeline.pipeline);
    if (boundComputePipelineState != dx12PipelineStateObject)
    {
        dx12CommandList->SetPipelineState(dx12PipelineStateObject);
        boundComputePipelineState = dx12PipelineStateObject;
    }

    // copy data to constant buffer and bind
    {
        for (uint32_t currentRootConstantIndex = 0; currentRootConstantIndex < pipeline.constCount; ++currentRootConstantIndex) {

            const ApusrFramegenConstantBuffer& constantBuffer = job->computeJob.constantBuffers[currentRootConstantIndex];
            const uint64_t dataSize = constantBuffer.num32BitEntries * sizeof(uint32_t);
            FallbackConstantAllocationCacheEntry& cached = fallbackConstantCache[currentRootConstantIndex];
            if (cached.data != constantBuffer.data || cached.num32BitEntries != constantBuffer.num32BitEntries)
            {
                cached.data = constantBuffer.data;
                cached.num32BitEntries = constantBuffer.num32BitEntries;
                cached.allocation = backendContext->FallbackConstantAllocator(constantBuffer.data, dataSize);
            }
            const ApusrFramegenConstantAllocation allocation = cached.allocation;

            D3D12_GPU_VIRTUAL_ADDRESS bufferViewDesc = D3D12_GPU_VIRTUAL_ADDRESS(allocation.gpuAddress);
            dx12CommandList->SetComputeRootConstantBufferView(descriptorTableIndex + currentRootConstantIndex, bufferViewDesc);
        }
    }

    // Dispatch (or dispatch indirect)
    if (pipeline.cmdSignature)
    {
        const uint32_t resourceIndex = job->computeJob.cmdArgument.internalIndex;
        ID3D12Resource* pResource = backendContext->pResources[resourceIndex].resourcePtr;

        dx12CommandList->ExecuteIndirect(reinterpret_cast<ID3D12CommandSignature*>(pipeline.cmdSignature), 1, pResource, job->computeJob.cmdArgumentOffset, nullptr, 0);
    }
    else
    {
        dx12CommandList->Dispatch(job->computeJob.dimensions[0], job->computeJob.dimensions[1], job->computeJob.dimensions[2]);
    }

    return APUSR_FRAMEGEN_OK;
}

static ApusrFramegenResult executeGpuJobCopy(BackendContext_DX12* backendContext, ApusrFramegenGpuJob* job, ID3D12GraphicsCommandList* dx12CommandList)
{
    ID3D12Device* dx12Device = reinterpret_cast<ID3D12Device*>(backendContext->device);

    ID3D12Resource* dx12ResourceSrc = getDX12ResourcePtr(backendContext, job->copyJob.src.internalIndex);
    ID3D12Resource* dx12ResourceDst = getDX12ResourcePtr(backendContext, job->copyJob.dst.internalIndex);
    D3D12_RESOURCE_DESC dx12ResourceDescriptionDst = dx12ResourceDst->GetDesc();
    D3D12_RESOURCE_DESC dx12ResourceDescriptionSrc = dx12ResourceSrc->GetDesc();

    addBarrier(backendContext, &job->copyJob.src, APUSR_FRAMEGEN_RESOURCE_STATE_COPY_SRC);
    addBarrier(backendContext, &job->copyJob.dst, APUSR_FRAMEGEN_RESOURCE_STATE_COPY_DEST);
    flushBarriers(backendContext, dx12CommandList);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT dx12Footprint = {};
    UINT rowCount;
    UINT64 rowSizeInBytes;
    UINT64 totalBytes;
    dx12Device->GetCopyableFootprints(&dx12ResourceDescriptionDst, 0, 1, 0, &dx12Footprint, &rowCount, &rowSizeInBytes, &totalBytes);

    if (dx12ResourceDescriptionDst.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        dx12CommandList->CopyBufferRegion(dx12ResourceDst,
                                          job->copyJob.dstOffset,
                                          dx12ResourceSrc,
                                          job->copyJob.srcOffset,
                                          job->copyJob.size > 0 ? job->copyJob.size : totalBytes);
    }
    else if (dx12ResourceDescriptionSrc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    {
        // TODO: account for source buffer offset
        D3D12_TEXTURE_COPY_LOCATION dx12SourceLocation = {};
        dx12SourceLocation.pResource = dx12ResourceSrc;
        dx12SourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dx12SourceLocation.PlacedFootprint = dx12Footprint;

        D3D12_TEXTURE_COPY_LOCATION dx12DestinationLocation = {};
        dx12DestinationLocation.pResource = dx12ResourceDst;
        dx12DestinationLocation.SubresourceIndex = 0;
        dx12DestinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        dx12CommandList->CopyTextureRegion(&dx12DestinationLocation, 0, 0, 0, &dx12SourceLocation, nullptr);
    }
    else
    {
        D3D12_TEXTURE_COPY_LOCATION dx12SourceLocation = {};
        dx12SourceLocation.pResource = dx12ResourceSrc;
        dx12SourceLocation.SubresourceIndex = 0;
        dx12SourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        D3D12_TEXTURE_COPY_LOCATION dx12DestinationLocation = {};
        dx12DestinationLocation.pResource = dx12ResourceDst;
        dx12DestinationLocation.SubresourceIndex = 0;
        dx12DestinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

        dx12CommandList->CopyTextureRegion(&dx12DestinationLocation, 0, 0, 0, &dx12SourceLocation, nullptr);
    }

    return APUSR_FRAMEGEN_OK;
}

static ApusrFramegenResult executeGpuJobBarrier(BackendContext_DX12* backendContext, ApusrFramegenGpuJob* job, ID3D12GraphicsCommandList* dx12CommandList)
{
    addBarrier(backendContext, &job->barrierJob.resource, job->barrierJob.newState);
    flushBarriers(backendContext, dx12CommandList);

    return APUSR_FRAMEGEN_OK;
}

static ApusrFramegenResult executeGpuJobTimestamp(BackendContext_DX12*, ApusrFramegenGpuJob*, ID3D12GraphicsCommandList*)
{
    return APUSR_FRAMEGEN_OK;
}

static ApusrFramegenResult executeGpuJobClearFloat(BackendContext_DX12* backendContext, ApusrFramegenGpuJob* job, ID3D12GraphicsCommandList* dx12CommandList, ID3D12DescriptorHeap*& boundDescriptorHeap)
{
    uint32_t idx = job->clearJob.target.internalIndex;
    BackendContext_DX12::Resource ffxResource = backendContext->pResources[idx];
    ID3D12Resource* dx12Resource = reinterpret_cast<ID3D12Resource*>(ffxResource.resourcePtr);
    uint32_t uavIndex = ffxResource.uavDescIndex;

    D3D12_CPU_DESCRIPTOR_HANDLE dx12CpuHandle = backendContext->descHeapUavCpuStart;
    dx12CpuHandle.ptr += uavIndex * backendContext->descriptorSizeCbvSrvUav;

    D3D12_GPU_DESCRIPTOR_HANDLE dx12GpuHandle = backendContext->descHeapUavGpuStart;
    dx12GpuHandle.ptr += uavIndex * backendContext->descriptorSizeCbvSrvUav;

    setDescriptorHeapIfNeeded(dx12CommandList, backendContext->descHeapUavGpu, boundDescriptorHeap);

    addBarrier(backendContext, &job->clearJob.target, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS);
    flushBarriers(backendContext, dx12CommandList);

    uint32_t clearColorAsUint[4];
    clearColorAsUint[0] = reinterpret_cast<uint32_t&> (job->clearJob.color[0]);
    clearColorAsUint[1] = reinterpret_cast<uint32_t&> (job->clearJob.color[1]);
    clearColorAsUint[2] = reinterpret_cast<uint32_t&> (job->clearJob.color[2]);
    clearColorAsUint[3] = reinterpret_cast<uint32_t&> (job->clearJob.color[3]);
    dx12CommandList->ClearUnorderedAccessViewUint(dx12GpuHandle, dx12CpuHandle, dx12Resource, clearColorAsUint, 0, nullptr);

    return APUSR_FRAMEGEN_OK;
}

static ApusrFramegenResult executeGpuJobDiscard(BackendContext_DX12*       backendContext,
                                         ApusrFramegenGpuJob*      job,
                                         ID3D12GraphicsCommandList* dx12CommandList)
{
    uint32_t                            idx           = job->discardJob.target.internalIndex;
    BackendContext_DX12::Resource       ffxResource   = backendContext->pResources[idx];
    ID3D12Resource*                     dx12Resource  = reinterpret_cast<ID3D12Resource*>(ffxResource.resourcePtr);

    addBarrier(backendContext, &job->discardJob.target, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS);
    flushBarriers(backendContext, dx12CommandList);

    dx12CommandList->DiscardResource(dx12Resource, nullptr);

    return APUSR_FRAMEGEN_OK;
}

namespace {

const char* profileJobTypeName(const ApusrFramegenJobType type) noexcept
{
    switch (type) {
    case APUSR_FRAMEGEN_JOB_CLEAR_FLOAT: return "job.clear_float";
    case APUSR_FRAMEGEN_JOB_COPY: return "job.copy";
    case APUSR_FRAMEGEN_JOB_COMPUTE: return "job.compute";
    case APUSR_FRAMEGEN_JOB_BARRIER: return "job.barrier";
    case APUSR_FRAMEGEN_JOB_DISCARD: return "job.discard";
    default: return "job.unknown";
    }
}

void profileJobName(const ApusrFramegenGpuJob& job, char (&name)[48]) noexcept
{
    // Normal release builds deliberately do not copy every scheduled job label
    // merely for markers.  For sampled profiler frames, compute jobs already
    // retain a stable pipeline name, so derive the useful pass name here and
    // keep the profiler completely off the schedule hot path when disabled.
    const wchar_t* wideName = nullptr;
    if (job.jobType == APUSR_FRAMEGEN_JOB_COMPUTE && job.computeJob.pipeline != nullptr &&
        job.computeJob.pipeline->name[0] != L'\0') {
        wideName = job.computeJob.pipeline->name;
    }
#if defined(FFX_DX12_ENABLE_GPU_MARKERS) && FFX_DX12_ENABLE_GPU_MARKERS
    else if (job.jobLabel[0] != L'\0') {
        wideName = job.jobLabel;
    }
#endif

    if (wideName == nullptr) {
        const char* fallback = profileJobTypeName(job.jobType);
        std::strncpy(name, fallback, sizeof(name) - 1u);
        name[sizeof(name) - 1u] = '\0';
        return;
    }

    std::size_t out = 0u;
    while (out + 1u < sizeof(name) && wideName[out] != L'\0') {
        const wchar_t c = wideName[out];
        name[out] = (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '?';
        ++out;
    }
    name[out] = '\0';
}

} // namespace

ApusrFramegenResult ExecuteGpuJobsDX12(
    ApusrFramegenBackendInterface* backendInterface,
    ApusrFramegenCommandList commandList, 
    uint32_t effectContextId)
{
    FFX_ASSERT(NULL != backendInterface);
    BackendContext_DX12* backendContext = (BackendContext_DX12*)backendInterface->scratchBuffer;

    FFX_ASSERT(nullptr != commandList);
    ID3D12GraphicsCommandList* dx12CommandList = reinterpret_cast<ID3D12GraphicsCommandList*>(commandList);

    const std::uint64_t profileFrameId = backendContext->profileStream ? ++backendContext->profileFrameSequence : 0u;
    std::uint32_t jobTypeCounts[5] = {};
    std::uint64_t totalWorkgroups = 0u;
    constexpr std::uint64_t kFramegenDx12ProfileSeed = 0x4150555346474458ull; // "APUSFGDX"
    std::uint64_t samplingConfigurationHash = kFramegenDx12ProfileSeed;
    std::uint64_t workloadHash = kFramegenDx12ProfileSeed;
    // The job list intentionally alternates among several normal FrameGen batches.
    // Sample on the effect's realized resource shape instead; keep batch identity
    // as workload telemetry so normal cycling cannot restart every sampled burst.
    const bool profilePerformanceEnabled = backendContext->profileStream && apusr::profile::cpuEnabled();
    if (profilePerformanceEnabled) {
        const BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];
        const std::uint32_t staticBase = effectContextId * APUSR_FRAMEGEN_MAX_RESOURCES_PER_CONTEXT + 1u;
        for (std::uint32_t resourceIndex = staticBase; resourceIndex < effectContext.nextStaticResource; ++resourceIndex) {
            const BackendContext_DX12::Resource& resource = backendContext->pResources[resourceIndex];
            if (resource.resourcePtr == nullptr) {
                continue;
            }
            const ApusrFramegenResourceDescription& desc = resource.resourceDescription;
            samplingConfigurationHash = apusr::profile::hashCombine(samplingConfigurationHash, resourceIndex - staticBase);
            samplingConfigurationHash = apusr::profile::hashCombine(samplingConfigurationHash, static_cast<std::uint64_t>(desc.type));
            samplingConfigurationHash = apusr::profile::hashCombine(samplingConfigurationHash, static_cast<std::uint64_t>(desc.format));
            samplingConfigurationHash = apusr::profile::hashCombine(samplingConfigurationHash,
                (static_cast<std::uint64_t>(desc.width) << 32u) | desc.height);
            samplingConfigurationHash = apusr::profile::hashCombine(samplingConfigurationHash,
                (static_cast<std::uint64_t>(desc.depth) << 32u) | desc.mipCount);
            samplingConfigurationHash = apusr::profile::hashCombine(samplingConfigurationHash, static_cast<std::uint64_t>(desc.flags));
            samplingConfigurationHash = apusr::profile::hashCombine(samplingConfigurationHash, static_cast<std::uint64_t>(desc.usage));
        }
        workloadHash = apusr::profile::hashCombine(workloadHash, backendContext->gpuJobCount);
        for (std::uint32_t i = 0; i < backendContext->gpuJobCount; ++i) {
            const ApusrFramegenGpuJob& job = backendContext->pGpuJobs[i];
            workloadHash = apusr::profile::hashCombine(workloadHash, static_cast<std::uint64_t>(job.jobType));
            if (job.jobType <= APUSR_FRAMEGEN_JOB_DISCARD) {
                ++jobTypeCounts[static_cast<std::uint32_t>(job.jobType)];
            }
            if (job.jobType == APUSR_FRAMEGEN_JOB_COMPUTE) {
                const std::uint64_t workgroups = static_cast<std::uint64_t>(job.computeJob.dimensions[0]) *
                    job.computeJob.dimensions[1] * job.computeJob.dimensions[2];
                totalWorkgroups += workgroups;
                workloadHash = apusr::profile::hashCombine(workloadHash, job.computeJob.profileLevel);
            }
        }
    }
    bool samplingConfigurationChanged = false;
    if (profilePerformanceEnabled) {
        BackendContext_DX12::EffectContext& effectContext = backendContext->pEffectContexts[effectContextId];
        samplingConfigurationChanged = !effectContext.profileHaveConfiguration ||
            effectContext.profileConfigurationHash != samplingConfigurationHash;
        if (samplingConfigurationChanged) {
            effectContext.profileHaveConfiguration = true;
            effectContext.profileConfigurationHash = samplingConfigurationHash;
        }
    }
    const apusr::profile::SampleDecision profileDecision = profilePerformanceEnabled
        ? backendContext->profileSampleGate.decideStable(
            profileFrameId, samplingConfigurationChanged)
        : apusr::profile::SampleDecision{};
    const bool captureCpu = profileDecision.capture && apusr::profile::cpuEnabled();
    const std::uint64_t executeBeginTick = captureCpu ? apusr::profile::cpuTick() : 0u;

    ApusrFramegenResult errorCode = APUSR_FRAMEGEN_OK;
    std::uint32_t failedJobIndex = UINT32_MAX;
    const char* failedJobType = "execute_gpu_jobs";
    ID3D12DescriptorHeap* boundDescriptorHeap = nullptr;
    ID3D12RootSignature* boundComputeRootSignature = nullptr;
    ID3D12PipelineState* boundComputePipelineState = nullptr;
    FallbackConstantAllocationCacheEntry fallbackConstantCache[APUSR_FRAMEGEN_MAX_CONSTANT_BUFFERS] = {};
    Dx12Commands* gpuJobProfiler = activeDx12GpuProfileCommands(dx12CommandList);

    // execute all GpuJobs
    for (uint32_t currentGpuJobIndex = 0; currentGpuJobIndex < backendContext->gpuJobCount; ++currentGpuJobIndex) {

        ApusrFramegenGpuJob* GpuJob = &backendContext->pGpuJobs[currentGpuJobIndex];
        char profileName[48]{};
        if (captureCpu || gpuJobProfiler) {
            profileJobName(*GpuJob, profileName);
        }
        const std::uint64_t jobBeginTick = captureCpu ? apusr::profile::cpuTick() : 0u;
        const bool gpuJobOpen = gpuJobProfiler && gpuJobProfiler->beginGpuJobProfile(
            dx12CommandList,
            profileName,
            GpuJob->jobType == APUSR_FRAMEGEN_JOB_COMPUTE ? GpuJob->computeJob.dimensions[0] : 0u,
            GpuJob->jobType == APUSR_FRAMEGEN_JOB_COMPUTE ? GpuJob->computeJob.dimensions[1] : 0u,
            GpuJob->jobType == APUSR_FRAMEGEN_JOB_COMPUTE ? GpuJob->computeJob.dimensions[2] : 0u,
            GpuJob->jobType == APUSR_FRAMEGEN_JOB_COMPUTE ? GpuJob->computeJob.profileLevel : 0xffffffffu);

#if FFX_DX12_ENABLE_GPU_MARKERS
        // GPU markers are useful for captures, but normal builds should not pay per-job marker overhead.
        if (GpuJob->jobLabel[0]) {
            beginMarkerDX12(backendContext, dx12CommandList, GpuJob->jobLabel);
        }
#endif

        switch (GpuJob->jobType) {

            case APUSR_FRAMEGEN_JOB_CLEAR_FLOAT:
                errorCode = executeGpuJobClearFloat(backendContext, GpuJob, dx12CommandList, boundDescriptorHeap);
                break;

            case APUSR_FRAMEGEN_JOB_COPY:
                errorCode = executeGpuJobCopy(backendContext, GpuJob, dx12CommandList);
                break;

            case APUSR_FRAMEGEN_JOB_COMPUTE:
                errorCode = executeGpuJobCompute(backendContext, GpuJob, dx12CommandList, effectContextId, boundDescriptorHeap, boundComputeRootSignature, boundComputePipelineState, fallbackConstantCache);
                break;

            case APUSR_FRAMEGEN_JOB_BARRIER:
                errorCode = executeGpuJobBarrier(backendContext, GpuJob, dx12CommandList);
                break;

            case APUSR_FRAMEGEN_JOB_DISCARD:
                errorCode = executeGpuJobDiscard(backendContext, GpuJob, dx12CommandList);
                break;

            default:
                break;
        }

#if FFX_DX12_ENABLE_GPU_MARKERS
        if (GpuJob->jobLabel[0]) {
            endMarkerDX12(backendContext, dx12CommandList);
        }
#endif

        if (gpuJobOpen) {
            gpuJobProfiler->endGpuJobProfile(dx12CommandList);
        }

        if (jobBeginTick != 0u) {
            apusr::profile::recordCpuInterval(apusr::profile::CpuInterval{
                .stream = backendContext->profileStream,
                .frameId = profileFrameId,
                .sampleSequence = profileDecision.sampleSequence,
                .startCpuTick = jobBeginTick,
                .endCpuTick = apusr::profile::cpuTick(),
                .name = profileName,
                .flags = GpuJob->jobType == APUSR_FRAMEGEN_JOB_COMPUTE ? GpuJob->computeJob.profileLevel : 0xffffffffu,
            });
        }

        if (errorCode != APUSR_FRAMEGEN_OK) {
            failedJobIndex = currentGpuJobIndex;
            failedJobType = profileJobTypeName(GpuJob->jobType);
            break;
        }
    }

    const std::uint64_t executeEndTick = captureCpu ? apusr::profile::cpuTick() : 0u;
    if (profileDecision.capture) {
        apusr::profile::recordFrameSample(apusr::profile::FrameSample{
            .stream = backendContext->profileStream,
            .frameId = profileFrameId,
            .sampleSequence = profileDecision.sampleSequence,
            .burstSequence = profileDecision.burstSequence,
            .configurationHash = samplingConfigurationHash,
            .burstFrame = profileDecision.burstFrame,
            .cpuTotalNs = apusr::profile::durationNs(executeBeginTick, executeEndTick),
            .cpuRecordNs = apusr::profile::durationNs(executeBeginTick, executeEndTick),
            .totalWorkgroups = totalWorkgroups,
            .flags = (profileDecision.configurationChanged ? apusr::profile::FrameConfigurationChanged : 0u) |
                (errorCode != APUSR_FRAMEGEN_OK ? apusr::profile::FrameFailed : 0u),
        });
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = backendContext->profileStream, .frameId = profileFrameId, .name = "jobs.compute",
            .value = jobTypeCounts[APUSR_FRAMEGEN_JOB_COMPUTE], .value2 = totalWorkgroups});
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = backendContext->profileStream, .frameId = profileFrameId, .name = "jobs.auxiliary",
            .value = static_cast<std::uint64_t>(jobTypeCounts[APUSR_FRAMEGEN_JOB_CLEAR_FLOAT]) + jobTypeCounts[APUSR_FRAMEGEN_JOB_COPY],
            .value2 = static_cast<std::uint64_t>(jobTypeCounts[APUSR_FRAMEGEN_JOB_BARRIER]) + jobTypeCounts[APUSR_FRAMEGEN_JOB_DISCARD]});
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = backendContext->profileStream, .frameId = profileFrameId, .name = "jobs.workload_hash",
            .value = workloadHash, .value2 = backendContext->gpuJobCount});
    }

    if (errorCode != APUSR_FRAMEGEN_OK && backendContext->profileStream) {
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = backendContext->profileStream, .frameId = profileFrameId, .name = "backend.error_code",
            .value = static_cast<std::uint64_t>(errorCode), .value2 = failedJobIndex});
        apusr::profile::recordError(
            backendContext->profileStream, profileFrameId, failedJobType, apusr::StatusCode::RuntimeFailure,
            "DX12 frame-generation GPU job recording failed",
            static_cast<std::uint64_t>(errorCode),
            failedJobIndex);
    }

    // check the execute function returned cleanly.
    APUSR_FRAMEGEN_RETURN_ON_ERROR(
        errorCode == APUSR_FRAMEGEN_OK,
        APUSR_FRAMEGEN_ERROR_BACKEND_API);

    backendContext->gpuJobCount = 0;

    return APUSR_FRAMEGEN_OK;
}

ApusrFramegenResult apusrFramegenCreateDx12Backend(
    ID3D12Device* device,
    ApusrFramegenBackendInterface* backendInterface,
    size_t contexts,
    const ApusrFramegenAllocator& allocator) noexcept
{
    if (!device || !backendInterface || contexts == 0)
        return APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    const size_t scratchSize = scratchMemorySizeDx12(contexts);
    void* scratch = allocator.alloc(scratchSize);
    if (!scratch)
        return APUSR_FRAMEGEN_ERROR_OUT_OF_MEMORY;
    memset(scratch, 0, scratchSize);

    const ApusrFramegenResult result = initBackendDx12(
        backendInterface,
        device,
        scratch,
        scratchSize,
        contexts);
    if (result != APUSR_FRAMEGEN_OK)
        allocator.dealloc(scratch);
    return result;
}
