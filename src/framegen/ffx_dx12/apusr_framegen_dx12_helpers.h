// This APUSR-maintained source is derived from the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.
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

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <apusr/apusr_profiler.h>

#include <comdef.h>
#include <synchapi.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

#include "ffx_assert.h"
#include "ffx_api_types.h"
#include "apusr_framegen_backend_types.h"

using ApusrFramegenWaitCallbackFunc = int32_t (*)(wchar_t* fenceName, uint64_t fenceValueToWaitFor);

constexpr UINT UNKNOWN_TIMER_RESOlUTION = 0;  //Timer resolution is not known. 

IDXGIFactory*           getDXGIFactoryFromSwapChain(IDXGISwapChain* swapChain);
bool                    isExclusiveFullscreen(IDXGISwapChain* swapChain);
void                    waitForPerformanceCount(const int64_t targetCount, const int64_t frequency, const UINT timerResolution, const UINT spinTime);
bool                    waitForFenceValue(ID3D12Fence* fence, UINT64 value, DWORD dwMilliseconds = INFINITE, ApusrFramegenWaitCallbackFunc waitCallback = nullptr, const bool waitForSingleObjectOnFence = false);
bool                    isTearingSupported(IDXGIFactory* dxgiFactory);
bool                    getMonitorLuminanceRange(IDXGISwapChain* swapChain, float* outMinLuminance, float* outMaxLuminance);
inline bool             isValidHandle(HANDLE handle);
IDXGIOutput6*           getMostRelevantOutputFromSwapChain(IDXGISwapChain* swapChain);
uint64_t                GetResourceGpuMemorySize(ID3D12Resource* resource);

namespace apusr::framegen::dx12 {
D3D12_RESOURCE_STATES toDx12ResourceState(FfxApiResourceState state) noexcept;
}

    // Safe release for interfaces
template<class Interface>
inline UINT SafeRelease(Interface*& pInterfaceToRelease)
{
    UINT refCount = std::numeric_limits<UINT>::max();
    if (pInterfaceToRelease != nullptr)
    {
        refCount = pInterfaceToRelease->Release();

        pInterfaceToRelease = nullptr;
    }

    return refCount;
}

inline void SafeCloseHandle(HANDLE& handle)
{
    if (handle)
    {
        CloseHandle(handle);
        handle = 0;
    }
}

// fix up format in case resource passed for SRV cannot be mapped
static DXGI_FORMAT convertFormatSrv(DXGI_FORMAT format)
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

        // Handle TYPELESS format for color: assume FLOAT for 16 and 32 bit channels, else UNORM
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

class Dx12Commands;
inline thread_local Dx12Commands* g_activeDx12GpuProfileCommands = nullptr;

class Dx12Commands
{
    ID3D12CommandQueue*        queue                = nullptr;
    ID3D12CommandAllocator*    allocator            = nullptr;
    ID3D12GraphicsCommandList* list                 = nullptr;
    ID3D12Fence*               fence                = nullptr;
    UINT64                     availableFenceValue  = 0;

    // Optional, lazily-created profiler resources. They live with the command
    // wrapper so the existing availability fence also tells us when timestamp
    // readback is safe. Normal/non-profiled command lists allocate none of it.
    static constexpr std::uint32_t kProfileMaxJobIntervals = APUSR_FRAMEGEN_MAX_GPU_JOBS * 2u;
    static constexpr std::uint32_t kProfileTimestampCapacity = 2u + kProfileMaxJobIntervals * 2u;

    struct ProfileJobInterval {
        char name[48]{};
        std::uint32_t beginQuery = 0u;
        std::uint32_t endQuery = 0u;
        std::uint32_t groupsX = 0u;
        std::uint32_t groupsY = 0u;
        std::uint32_t groupsZ = 0u;
        std::uint32_t profileLevel = 0xffffffffu;
    };

    ID3D12QueryHeap*           profileQueryHeap     = nullptr;
    ID3D12Resource*            profileReadback      = nullptr;
    std::uint64_t*             profileReadbackData  = nullptr;
    apusr::profile::StreamHandle profileStream{};
    std::uint64_t              profileFrameId       = 0u;
    std::uint64_t              profileSampleSequence = 0u;
    std::uint64_t              profileGpuFrequency  = 0u;
    std::uint64_t              profileNativeQueue   = 0u;
    char                       profileIntervalName[48]{};
    std::unique_ptr<ProfileJobInterval[]> profileJobs;
    std::uint32_t              profileJobCount      = 0u;
    std::uint32_t              profileNextQuery     = 1u;
    std::uint32_t              profileTotalEndQuery = 0u;
    bool                       profileJobOpen       = false;
    bool                       profileRecording     = false;
    bool                       profilePending       = false;


    bool ensureProfileTimestampResources()
    {
        if (profileQueryHeap && profileReadback && profileReadbackData)
            return true;
        if (!queue)
            return false;

        ID3D12Device* device = nullptr;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))))
            return false;

        D3D12_QUERY_HEAP_DESC queryDesc{};
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryDesc.Count = kProfileTimestampCapacity;
        HRESULT hr = device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&profileQueryHeap));

        D3D12_HEAP_PROPERTIES heapProperties{};
        heapProperties.Type = D3D12_HEAP_TYPE_READBACK;
        heapProperties.CreationNodeMask = 1u;
        heapProperties.VisibleNodeMask = 1u;

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = sizeof(std::uint64_t) * kProfileTimestampCapacity;
        resourceDesc.Height = 1u;
        resourceDesc.DepthOrArraySize = 1u;
        resourceDesc.MipLevels = 1u;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1u;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(hr)) {
            hr = device->CreateCommittedResource(
                &heapProperties,
                D3D12_HEAP_FLAG_NONE,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&profileReadback));
        }
        if (SUCCEEDED(hr)) {
            D3D12_RANGE readRange{0u, sizeof(std::uint64_t) * kProfileTimestampCapacity};
            hr = profileReadback->Map(0u, &readRange, reinterpret_cast<void**>(&profileReadbackData));
        }
        device->Release();

        if (SUCCEEDED(hr)) {
            profileJobs.reset(new (std::nothrow) ProfileJobInterval[kProfileMaxJobIntervals]);
            if (!profileJobs)
                hr = E_OUTOFMEMORY;
        }
        if (FAILED(hr) || !profileQueryHeap || !profileReadback || !profileReadbackData || !profileJobs) {
            profileJobs.reset();
            if (profileReadbackData && profileReadback) {
                D3D12_RANGE noCpuWrites{0u, 0u};
                profileReadback->Unmap(0u, &noCpuWrites);
                profileReadbackData = nullptr;
            }
            SafeRelease(profileReadback);
            SafeRelease(profileQueryHeap);
            return false;
        }
        return true;
    }

    [[nodiscard]] std::uint64_t profileDurationNs(const std::uint64_t begin, const std::uint64_t end) const noexcept
    {
        if (profileGpuFrequency == 0u || end < begin)
            return 0u;
        const long double ns = static_cast<long double>(end - begin) * 1000000000.0L /
            static_cast<long double>(profileGpuFrequency);
        return static_cast<std::uint64_t>(ns);
    }

    void collectProfileGpuInterval()
    {
        if (!profilePending || !profileReadbackData || profileGpuFrequency == 0u)
            return;

        if (profileTotalEndQuery < kProfileTimestampCapacity) {
            const std::uint64_t begin = profileReadbackData[0];
            const std::uint64_t end = profileReadbackData[profileTotalEndQuery];
            const std::uint64_t durationNs = profileDurationNs(begin, end);
            if (durationNs != 0u || end == begin) {
                apusr::profile::recordGpuInterval(apusr::profile::GpuInterval{
                    .stream = profileStream,
                    .frameId = profileFrameId,
                    .sampleSequence = profileSampleSequence,
                    .name = profileIntervalName[0] ? profileIntervalName : "dx12.command_list",
                    .durationNs = durationNs,
                    .executionNs = durationNs,
                    .gpuBeginTick = begin,
                    .gpuExecutionBeginTick = begin,
                    .gpuEndTick = end,
                    .gpuFrequency = profileGpuFrequency,
                    .nativeQueue = profileNativeQueue,
                });
            }
        }

        for (std::uint32_t i = 0u; profileJobs && i < profileJobCount; ++i) {
            const ProfileJobInterval& job = profileJobs[i];
            if (job.beginQuery >= kProfileTimestampCapacity || job.endQuery >= kProfileTimestampCapacity)
                continue;
            const std::uint64_t begin = profileReadbackData[job.beginQuery];
            const std::uint64_t end = profileReadbackData[job.endQuery];
            const std::uint64_t durationNs = profileDurationNs(begin, end);
            if (durationNs == 0u && end != begin)
                continue;
            apusr::profile::recordGpuInterval(apusr::profile::GpuInterval{
                .stream = profileStream,
                .frameId = profileFrameId,
                .sampleSequence = profileSampleSequence,
                .name = job.name[0] ? job.name : "framegen.job",
                .durationNs = durationNs,
                .executionNs = durationNs,
                .groupsX = job.groupsX,
                .groupsY = job.groupsY,
                .groupsZ = job.groupsZ,
                .profileLevel = job.profileLevel,
                .gpuBeginTick = begin,
                .gpuExecutionBeginTick = begin,
                .gpuEndTick = end,
                .gpuFrequency = profileGpuFrequency,
                .nativeQueue = profileNativeQueue,
            });
        }

        profilePending = false;
        profileStream = {};
        profileIntervalName[0] = '\0';
        profileFrameId = 0u;
        profileSampleSequence = 0u;
        profileGpuFrequency = 0u;
        profileNativeQueue = 0u;
        profileJobCount = 0u;
        profileNextQuery = 1u;
        profileTotalEndQuery = 0u;
        profileJobOpen = false;
    }


public:
    void release()
    {
        if (g_activeDx12GpuProfileCommands == this)
            g_activeDx12GpuProfileCommands = nullptr;
        if (profilePending && available())
            collectProfileGpuInterval();
        if (profileReadbackData && profileReadback) {
            D3D12_RANGE noCpuWrites{0u, 0u};
            profileReadback->Unmap(0u, &noCpuWrites);
            profileReadbackData = nullptr;
        }
        SafeRelease(profileReadback);
        SafeRelease(profileQueryHeap);
        profileJobs.reset();
        SafeRelease(allocator);
        SafeRelease(list);
        SafeRelease(fence);
    }

public:
    ~Dx12Commands()
    {
        release();
    }

    bool initiated()
    {
        return allocator != nullptr;
    }

    bool verify(ID3D12CommandQueue* pQueue)
    {
        HRESULT hr = initiated() ? S_OK : E_FAIL;

        if (FAILED(hr))
        {
            ID3D12Device* device = nullptr;
            if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&device))))
            {
                D3D12_COMMAND_QUEUE_DESC queueDesc = pQueue->GetDesc();
                if (SUCCEEDED(device->CreateCommandAllocator(queueDesc.Type, IID_PPV_ARGS(&allocator))))
                {
                    allocator->SetName(L"Dx12CommandPool::Allocator");
                    if (SUCCEEDED(device->CreateCommandList(queueDesc.NodeMask, queueDesc.Type, allocator, nullptr, IID_PPV_ARGS(&list))))
                    {
                        allocator->SetName(L"Dx12CommandPool::Commandlist");
                        list->Close();

                        if (SUCCEEDED(device->CreateFence(availableFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
                        {
                            hr = S_OK;
                        }
                    }
                }

                SafeRelease(device);
            }
        }

        if (FAILED(hr))
        {
            release();
        }

        return SUCCEEDED(hr);
    }

    void occupy(ID3D12CommandQueue* pQueue, const wchar_t* name)
    {
        if (profilePending && available())
            collectProfileGpuInterval();
        availableFenceValue++;
        queue = pQueue;
        allocator->SetName(name);
        list->SetName(name);
        fence->SetName(name);
    }

    ID3D12GraphicsCommandList* reset()
    {
        if (SUCCEEDED(allocator->Reset()))
        {
            if (SUCCEEDED(list->Reset(allocator, nullptr)))
            {
            }
        }

        return list;
    }

    ID3D12GraphicsCommandList* getList()
    {
        return list;
    }

    bool beginGpuProfile(
        const apusr::profile::StreamHandle stream,
        const std::uint64_t frameId,
        const std::uint64_t sampleSequence,
        const char* intervalName)
    {
        if (!apusr::profile::gpuEnabled() || !stream || !queue || !list || profilePending || profileRecording)
            return false;
        if (!ensureProfileTimestampResources()) {
            apusr::profile::recordError(
                stream, frameId, "dx12.timestamp_resources", apusr::StatusCode::RuntimeFailure,
                "Failed to create D3D12 timestamp resources for FrameGen command-list profiling");
            return false;
        }

        UINT64 gpuFrequency = 0u;
        if (FAILED(queue->GetTimestampFrequency(&gpuFrequency)) || gpuFrequency == 0u) {
            apusr::profile::recordError(
                stream, frameId, "dx12.timestamp_frequency", apusr::StatusCode::RuntimeFailure,
                "Failed to query the D3D12 FrameGen queue timestamp frequency");
            return false;
        }

        UINT64 gpuCalibration = 0u;
        UINT64 cpuCalibration = 0u;
        if (SUCCEEDED(queue->GetClockCalibration(&gpuCalibration, &cpuCalibration))) {
            apusr::profile::recordCalibration(apusr::profile::ClockCalibration{
                .stream = stream,
                .frameId = frameId,
                .sampleSequence = sampleSequence,
                .cpuTick = cpuCalibration,
                .gpuTick = gpuCalibration,
                .gpuFrequency = gpuFrequency,
                .nativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(queue)),
                .gpuValidBits = 64u,
            });
        }

        profileStream = stream;
        profileFrameId = frameId;
        profileSampleSequence = sampleSequence;
        profileGpuFrequency = gpuFrequency;
        profileNativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(queue));
        std::memset(profileIntervalName, 0, sizeof(profileIntervalName));
        if (intervalName) {
            std::strncpy(profileIntervalName, intervalName, sizeof(profileIntervalName) - 1u);
            profileIntervalName[sizeof(profileIntervalName) - 1u] = '\0';
        }
        profileJobCount = 0u;
        profileNextQuery = 1u;
        profileTotalEndQuery = 0u;
        profileJobOpen = false;
        list->EndQuery(profileQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, 0u);
        profileRecording = true;
        g_activeDx12GpuProfileCommands = this;
        return true;
    }

    bool beginGpuJobProfile(
        ID3D12GraphicsCommandList* commandList,
        const char* name,
        const std::uint32_t groupsX = 0u,
        const std::uint32_t groupsY = 0u,
        const std::uint32_t groupsZ = 0u,
        const std::uint32_t profileLevel = 0xffffffffu)
    {
        if (!profileRecording || !profileJobs || profileJobOpen || commandList != list ||
            profileJobCount >= kProfileMaxJobIntervals || profileNextQuery + 1u >= kProfileTimestampCapacity)
            return false;

        ProfileJobInterval& job = profileJobs[profileJobCount];
        std::memset(&job, 0, sizeof(job));
        if (name) {
            std::strncpy(job.name, name, sizeof(job.name) - 1u);
            job.name[sizeof(job.name) - 1u] = '\0';
        }
        job.groupsX = groupsX;
        job.groupsY = groupsY;
        job.groupsZ = groupsZ;
        job.profileLevel = profileLevel;
        job.beginQuery = profileNextQuery++;
        list->EndQuery(profileQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, job.beginQuery);
        profileJobOpen = true;
        return true;
    }

    void endGpuJobProfile(ID3D12GraphicsCommandList* commandList)
    {
        if (!profileRecording || !profileJobs || !profileJobOpen || commandList != list ||
            profileJobCount >= kProfileMaxJobIntervals || profileNextQuery >= kProfileTimestampCapacity)
            return;
        ProfileJobInterval& job = profileJobs[profileJobCount];
        job.endQuery = profileNextQuery++;
        list->EndQuery(profileQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, job.endQuery);
        ++profileJobCount;
        profileJobOpen = false;
    }

    void endGpuProfile()
    {
        if (!profileRecording || !profileQueryHeap || !profileReadback)
            return;
        if (g_activeDx12GpuProfileCommands == this)
            g_activeDx12GpuProfileCommands = nullptr;
        if (profileJobOpen) {
            endGpuJobProfile(list);
        }
        if (profileNextQuery >= kProfileTimestampCapacity) {
            const auto stream = profileStream;
            const std::uint64_t frameId = profileFrameId;
            cancelGpuProfile();
            apusr::profile::recordError(
                stream, frameId, "dx12.timestamp_capacity", apusr::StatusCode::RuntimeFailure,
                "D3D12 FrameGen timestamp query capacity was exhausted");
            return;
        }
        profileTotalEndQuery = profileNextQuery++;
        list->EndQuery(profileQueryHeap, D3D12_QUERY_TYPE_TIMESTAMP, profileTotalEndQuery);
        list->ResolveQueryData(
            profileQueryHeap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            0u,
            profileNextQuery,
            profileReadback,
            0u);
        profileRecording = false;
        profilePending = true;
    }

    void cancelGpuProfile()
    {
        if (g_activeDx12GpuProfileCommands == this)
            g_activeDx12GpuProfileCommands = nullptr;
        profileRecording = false;
        profilePending = false;
        profileStream = {};
        profileIntervalName[0] = '\0';
        profileFrameId = 0u;
        profileSampleSequence = 0u;
        profileGpuFrequency = 0u;
        profileNativeQueue = 0u;
        profileJobCount = 0u;
        profileNextQuery = 1u;
        profileTotalEndQuery = 0u;
        profileJobOpen = false;
    }

    void execute(bool listIsOpen = false)
    {
        if (listIsOpen)
        {
            list->Close();
        }

        ID3D12CommandList* pListsToExec[] = {list};
        queue->ExecuteCommandLists(_countof(pListsToExec), pListsToExec);
        queue->Signal(fence, availableFenceValue);
    }

    void drop(bool listIsOpen = false)
    {
        cancelGpuProfile();
        if (listIsOpen)
        {
            getList()->Close();
        }
        queue->Signal(fence, availableFenceValue);
    }

    bool available()
    {
        return fence->GetCompletedValue() >= availableFenceValue;
    }
};

[[nodiscard]] inline Dx12Commands* activeDx12GpuProfileCommands(
    ID3D12GraphicsCommandList* commandList) noexcept
{
    Dx12Commands* active = g_activeDx12GpuProfileCommands;
    return active && active->getList() == commandList ? active : nullptr;
}

template <size_t Capacity>
class Dx12CommandPool
{
public:

private:
    CRITICAL_SECTION criticalSection{};
    Dx12Commands     buffer[4 /* D3D12_COMMAND_LIST_TYPE_COPY == 3 */][Capacity] = {};

public:

    Dx12CommandPool()
    {
        InitializeCriticalSection(&criticalSection);
    }

    ~Dx12CommandPool()
    {
        EnterCriticalSection(&criticalSection);

        for (size_t type = 0; type < 4; type++)
        {
            for (size_t idx = 0; idx < Capacity; idx++)
            {
                auto& cmds = buffer[type][idx];
                while (cmds.initiated() && !cmds.available())
                {
                    // wait for list to be idling
                }
                cmds.release();
            }
        }

        LeaveCriticalSection(&criticalSection);

        DeleteCriticalSection(&criticalSection);
    }

    Dx12Commands* get(ID3D12CommandQueue* pQueue, const wchar_t* name)
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = pQueue->GetDesc();

        EnterCriticalSection(&criticalSection);

        Dx12Commands* pCommands = nullptr;
        for (size_t idx = 0; idx < Capacity && (pCommands == nullptr); idx++)
        {
            auto& cmds = buffer[queueDesc.Type][idx];
            if (cmds.verify(pQueue) && cmds.available())
            {
                pCommands = &cmds;
            }
        }

        FFX_ASSERT(pCommands);

        pCommands->occupy(pQueue, name);
        LeaveCriticalSection(&criticalSection);

        return pCommands;
    }
};
