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

#include <algorithm>
#include <initguid.h>
#include "apusr_framegen_swapchain_dx12.h"
#include "ffx_dx12/apusr_framegen_pacing_dx12.h"
#include "ffx_dx12/apusr_framegen_present_path_dx12.h"

#include "apusr_framegen_backend_dx12.h"
#include "apusr_framegen_ffx_dx12.h"
#include "apusr_framegen_dx12_resource_compat.h"
#include "apusr_framegen_ui_composition_dx12.h"


void apusr::framegen::dx12::recordDxgiPresentTiming(
    FrameinterpolationPresentInfo& presentInfo,
    const apusr::profile::StreamHandle stream,
    const std::uint64_t frameId,
    const std::uint64_t sampleSequence,
    const std::uint64_t presentBeginTick,
    const std::uint64_t presentEndTick,
    const bool generated) noexcept
{
    if (!stream || presentInfo.swapChain == nullptr)
        return;

    UINT presentCount = 0u;
    if (SUCCEEDED(presentInfo.swapChain->GetLastPresentCount(&presentCount)) && presentCount != 0u) {
        auto& entry = presentInfo.profilePresentTimingHistory[
            presentInfo.profilePresentTimingHead++ % presentInfo.profilePresentTimingHistory.size()];
        entry.presentCount = presentCount;
        entry.frameId = frameId;
        entry.sampleSequence = sampleSequence;
        entry.presentBeginTick = presentBeginTick;
        entry.presentEndTick = presentEndTick;
        entry.generated = generated;
        entry.valid = true;
    }

    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(presentInfo.swapChain->GetFrameStatistics(&stats)) || stats.PresentCount == 0u ||
        stats.SyncQPCTime.QuadPart <= 0)
        return;

    const FrameinterpolationPresentInfo::ProfilePresentTimingEntry* matched = nullptr;
    for (const auto& entry : presentInfo.profilePresentTimingHistory) {
        if (entry.valid && entry.presentCount == stats.PresentCount) {
            matched = &entry;
            break;
        }
    }
    if (matched == nullptr)
        return;

    const std::uint32_t eventFlags = matched->generated ? 1u : 0u;
    const std::uint64_t syncQpc = static_cast<std::uint64_t>(stats.SyncQPCTime.QuadPart);
    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = stream,
        .frameId = matched->frameId,
        .name = "display.dxgi_sync_qpc",
        .value = syncQpc,
        .value2 = stats.PresentCount,
        .flags = eventFlags,
    });
    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = stream,
        .frameId = matched->frameId,
        .name = "display.dxgi_refresh_counts",
        .value = stats.PresentRefreshCount,
        .value2 = stats.SyncRefreshCount,
        .flags = eventFlags,
    });
    apusr::profile::recordCounter(apusr::profile::Counter{
        .stream = stream,
        .frameId = matched->frameId,
        .name = "display.dxgi_sync_gpu_time",
        .value = static_cast<std::uint64_t>(stats.SyncGPUTime.QuadPart),
        .value2 = stats.PresentCount,
        .flags = eventFlags,
    });
    if (matched->presentBeginTick != 0u && syncQpc >= matched->presentBeginTick) {
        const std::uint64_t beginToSync = apusr::profile::durationNs(matched->presentBeginTick, syncQpc);
        const std::uint64_t returnToSync =
            matched->presentEndTick != 0u && syncQpc >= matched->presentEndTick
                ? apusr::profile::durationNs(matched->presentEndTick, syncQpc)
                : 0u;
        apusr::profile::recordCounter(apusr::profile::Counter{
            .stream = stream,
            .frameId = matched->frameId,
            .name = "display.present_to_sync_ns",
            .value = beginToSync,
            .value2 = returnToSync,
            .flags = eventFlags,
        });
    }
}


FfxErrorCode ffxRegisterFrameinterpolationUiResourceDX12(FfxSwapchain gameSwapChain, FfxApiResource uiResource, uint32_t flags)
{
    IDXGISwapChain4* swapChain = apusrFramegenSwapchainPtrDX12(gameSwapChain);

    FrameInterpolationSwapChainDX12* framinterpolationSwapchain = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&framinterpolationSwapchain))))
    {
        framinterpolationSwapchain->registerUiResource(uiResource, flags);

        SafeRelease(framinterpolationSwapchain);

        return FFX_OK;
    }

    return FFX_ERROR_INVALID_ARGUMENT;
}

ApusrFramegenResult apusrFramegenApplySwapchainConfig(const ApusrFramegenConfig* config) noexcept
{
    if (!config)
        return APUSR_FRAMEGEN_ERROR_INVALID_POINTER;

    ApusrFramegenResult result = APUSR_FRAMEGEN_ERROR_INVALID_ARGUMENT;

    if (config->swapChain)
    {
        IDXGISwapChain4*                 swapChain = apusrFramegenSwapchainPtrDX12(config->swapChain);
        FrameInterpolationSwapChainDX12* framinterpolationSwapchain = nullptr;
        if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&framinterpolationSwapchain))))
        {
            framinterpolationSwapchain->setFrameGenerationConfig(config);

            SafeRelease(framinterpolationSwapchain);
            
            result = APUSR_FRAMEGEN_OK;
        }
    }

    return result;
}

FfxErrorCode ffxConfigureFrameInterpolationSwapchainDX12(FfxSwapchain gameSwapChain, FfxFrameInterpolationSwapchainConfigureKey key, void* valuePtr)
{
    IDXGISwapChain4* swapChain = apusrFramegenSwapchainPtrDX12(gameSwapChain);
    
    FrameInterpolationSwapChainDX12* framinterpolationSwapchain = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&framinterpolationSwapchain))))
    {
        switch (key)
        {
            case FFX_FI_SWAPCHAIN_CONFIGURE_KEY_WAITCALLBACK:
                framinterpolationSwapchain->setWaitCallback(static_cast<FfxWaitCallbackFunc>(valuePtr));
            break;
            case FFX_FI_SWAPCHAIN_CONFIGURE_KEY_FRAMEPACINGTUNING:
                if (valuePtr != nullptr)
                {
                    framinterpolationSwapchain->setFramePacingTuning(static_cast<FfxApiSwapchainFramePacingTuning*>(valuePtr));
                }
            break;
        }
        SafeRelease(framinterpolationSwapchain);

        return FFX_OK;
    }

    return FFX_ERROR_INVALID_ARGUMENT;
}

FfxApiResource ffxGetFrameinterpolationTextureDX12(FfxSwapchain gameSwapChain)
{
    FfxApiResource                      res = { nullptr };
    IDXGISwapChain4* swapChain = apusrFramegenSwapchainPtrDX12(gameSwapChain);
    FrameInterpolationSwapChainDX12* framinterpolationSwapchain = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&framinterpolationSwapchain))))
    {
        res = framinterpolationSwapchain->interpolationOutput(0);

        SafeRelease(framinterpolationSwapchain);
    }
    return res;
}

FfxErrorCode ffxGetFrameinterpolationCommandlistDX12(FfxSwapchain gameSwapChain, FfxCommandList& gameCommandlist)
{
    // 1) query FrameInterpolationSwapChainDX12 from gameSwapChain
    // 2) call  FrameInterpolationSwapChainDX12::getInterpolationCommandList()
    IDXGISwapChain4* swapChain = apusrFramegenSwapchainPtrDX12(gameSwapChain);

    FrameInterpolationSwapChainDX12* framinterpolationSwapchain = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&framinterpolationSwapchain))))
    {
        gameCommandlist = framinterpolationSwapchain->getInterpolationCommandList();

        SafeRelease(framinterpolationSwapchain);

        return FFX_OK;
    }

    return FFX_ERROR_INVALID_ARGUMENT;
}

FfxErrorCode ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12(FfxSwapchain gameSwapChain, FfxApiEffectMemoryUsage* vramUsage)
{
    FfxErrorCode result = FFX_ERROR_INVALID_ARGUMENT;
    IDXGISwapChain4* swapChain = apusrFramegenSwapchainPtrDX12(gameSwapChain);
    
    FrameInterpolationSwapChainDX12* framinterpolationSwapchain = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&framinterpolationSwapchain))))
    {
        framinterpolationSwapchain->GetGpuMemoryUsage(vramUsage);
        SafeRelease(framinterpolationSwapchain);
        result = FFX_OK;
    }
    return result;
}

FfxErrorCode ffxFrameInterpolationSwapchainGetGpuMemoryUsageDX12V2(FfxDevice device, FfxApiDimensions2D* displaySize, FfxApiSurfaceFormat backbufferFormat, uint32_t backBufferCount, FfxApiDimensions2D* uiResourceSize, FfxApiSurfaceFormat uiResourceFormat, uint32_t flags, FfxApiEffectMemoryUsage* vramUsage)
{
    ApusrFramegenCreateResourceDescription replacementSwapBuffersCreateResourceDescs[FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT];
    ApusrFramegenCreateResourceDescription interpolationOutputsCreateResourceDescs[FFX_FRAME_INTERPOLATION_SWAP_CHAIN_INTERPOLATION_OUTPUTS_COUNT];
    ApusrFramegenCreateResourceDescription uiReplacementBufferCreateResourceDesc;
    uint64_t      size = 0;
    for (uint32_t i = 0; i < backBufferCount; i++)
    {
        replacementSwapBuffersCreateResourceDescs[i] = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, apusrFramegenSurfaceFormatFromFfx(backbufferFormat), displaySize->width, displaySize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"APUSR Frame Generation Replacement BackBuffer", 0, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
        FFX_VALIDATE(apusrFramegenResourceSizeDX12(reinterpret_cast<ID3D12Device*>(device), &replacementSwapBuffersCreateResourceDescs[i], &size));
        vramUsage->totalUsageInBytes += size;
    }

    for (uint32_t i = 0; i < FFX_FRAME_INTERPOLATION_SWAP_CHAIN_INTERPOLATION_OUTPUTS_COUNT; i ++)
    {
        interpolationOutputsCreateResourceDescs[i] = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, apusrFramegenSurfaceFormatFromFfx(backbufferFormat), displaySize->width, displaySize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"APUSR Frame Generation Interpolation Output", 0, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
        FFX_VALIDATE(apusrFramegenResourceSizeDX12(reinterpret_cast<ID3D12Device*>(device), &interpolationOutputsCreateResourceDescs[i], &size));
        vramUsage->totalUsageInBytes += size;
    }

    if (uiResourceFormat != FFX_API_SURFACE_FORMAT_UNKNOWN && flags & APUSR_FRAMEGEN_UI_COMPOSITION_INTERNAL_DOUBLE_BUFFER)
    {
        uiReplacementBufferCreateResourceDesc = { APUSR_FRAMEGEN_HEAP_DEFAULT, { APUSR_FRAMEGEN_RESOURCE_TYPE_TEXTURE2D, apusrFramegenSurfaceFormatFromFfx(uiResourceFormat), uiResourceSize->width, uiResourceSize->height, 1, 1, APUSR_FRAMEGEN_RESOURCE_FLAGS_NONE, APUSR_FRAMEGEN_RESOURCE_USAGE_UAV }, APUSR_FRAMEGEN_RESOURCE_STATE_UNORDERED_ACCESS, L"APUSR Frame Generation UI Replacement Buffer", 0, {APUSR_FRAMEGEN_RESOURCE_INIT_UNINITIALIZED} };
        FFX_VALIDATE(apusrFramegenResourceSizeDX12(reinterpret_cast<ID3D12Device*>(device), &uiReplacementBufferCreateResourceDesc, &size));
        vramUsage->totalUsageInBytes += size;
    }
    return FFX_OK;
}

FfxErrorCode ffxReplaceSwapchainForFrameinterpolationDX12(FfxCommandQueue gameQueue, FfxSwapchain& gameSwapChain)
{
    FfxErrorCode     status            = FFX_ERROR_INVALID_ARGUMENT;
    IDXGISwapChain4* dxgiGameSwapChain = reinterpret_cast<IDXGISwapChain4*>(gameSwapChain);
    FFX_ASSERT(dxgiGameSwapChain);

    ID3D12CommandQueue* queue = reinterpret_cast<ID3D12CommandQueue*>(gameQueue);
    FFX_ASSERT(queue);

    // we just need the desc, release the real swapchain as we'll replace that with one doing frameinterpolation
    HWND                            hWnd;
    DXGI_SWAP_CHAIN_DESC1           desc1;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc;
    if (SUCCEEDED(dxgiGameSwapChain->GetDesc1(&desc1)) &&
        SUCCEEDED(dxgiGameSwapChain->GetFullscreenDesc(&fullscreenDesc)) &&
        SUCCEEDED(dxgiGameSwapChain->GetHwnd(&hWnd))
        )
    {
        FFX_ASSERT_MESSAGE(fullscreenDesc.Windowed == TRUE, "Illegal to release a fullscreen swap chain.");

        IDXGIFactory* dxgiFactory = getDXGIFactoryFromSwapChain(dxgiGameSwapChain);
        SafeRelease(dxgiGameSwapChain);

        FfxSwapchain proxySwapChain;
        status = ffxCreateFrameinterpolationSwapchainForHwndDX12(hWnd, &desc1, &fullscreenDesc, queue, dxgiFactory, proxySwapChain);
        if (status == FFX_OK)
        {
            gameSwapChain = proxySwapChain;
        }

        SafeRelease(dxgiFactory);
    }

    return status;
}

FfxErrorCode ffxCreateFrameinterpolationSwapchainDX12(const DXGI_SWAP_CHAIN_DESC*   desc,
                                                      ID3D12CommandQueue*           queue,
                                                      IDXGIFactory*                 dxgiFactory,
                                                      FfxSwapchain&                 outGameSwapChain)
{
    FFX_ASSERT(desc);
    FFX_ASSERT(queue);
    FFX_ASSERT(dxgiFactory);

    DXGI_SWAP_CHAIN_DESC1 desc1{};
    desc1.Width         = desc->BufferDesc.Width;
    desc1.Height        = desc->BufferDesc.Height;
    desc1.Format        = desc->BufferDesc.Format;
    desc1.SampleDesc    = desc->SampleDesc;
    desc1.BufferUsage   = desc->BufferUsage;
    desc1.BufferCount   = desc->BufferCount;
    desc1.SwapEffect    = desc->SwapEffect;
    desc1.Flags         = desc->Flags;

    // for clarity, params not part of DXGI_SWAP_CHAIN_DESC
    // implicit behavior of DXGI when you call the IDXGIFactory::CreateSwapChain
    desc1.Scaling       = DXGI_SCALING_STRETCH;
    desc1.AlphaMode     = DXGI_ALPHA_MODE_UNSPECIFIED;
    desc1.Stereo        = FALSE;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreenDesc{};
    fullscreenDesc.Scaling          = desc->BufferDesc.Scaling;
    fullscreenDesc.RefreshRate      = desc->BufferDesc.RefreshRate;
    fullscreenDesc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fullscreenDesc.Windowed         = desc->Windowed;

    return ffxCreateFrameinterpolationSwapchainForHwndDX12(desc->OutputWindow, &desc1, &fullscreenDesc, queue, dxgiFactory, outGameSwapChain);
}

FfxErrorCode ffxCreateFrameinterpolationSwapchainForHwndDX12(HWND                                   hWnd,
                                                             const DXGI_SWAP_CHAIN_DESC1*           desc1,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                             ID3D12CommandQueue*                    queue,
                                                             IDXGIFactory*                          dxgiFactory,
                                                             FfxSwapchain&                          outGameSwapChain)
{
    // don't assert fullscreenDesc, nullptr valid
    FFX_ASSERT(hWnd != 0);
    FFX_ASSERT(desc1);
    FFX_ASSERT(queue);
    FFX_ASSERT(dxgiFactory);

    FfxErrorCode err = FFX_ERROR_INVALID_ARGUMENT;

    IDXGIFactory2* dxgiFactory2 = nullptr;
    if (SUCCEEDED(dxgiFactory->QueryInterface(IID_PPV_ARGS(&dxgiFactory2))))
    {
        // Create proxy swapchain
        FrameInterpolationSwapChainDX12* fiSwapchain = new FrameInterpolationSwapChainDX12();
        if (fiSwapchain)
        {
            HRESULT hr = fiSwapchain->init(hWnd, desc1, fullscreenDesc, queue, dxgiFactory2);
            if (SUCCEEDED(hr))
            {
                outGameSwapChain = apusrFramegenSwapchainDX12(fiSwapchain);

                err = FFX_OK;
            }
            else
            {
                delete fiSwapchain;
                if (hr == E_ACCESSDENIED)
                    err = FFX_ERROR_ACCESS_DENIED;
                else
                    err = FFX_ERROR_INVALID_ARGUMENT;
            }
        }
        else
        {
            err = FFX_ERROR_OUT_OF_MEMORY;
        }

        SafeRelease(dxgiFactory2);
    }

    return err;
}

FfxErrorCode ffxWaitForPresents(FfxSwapchain gameSwapChain)
{
    IDXGISwapChain4* swapChain = apusrFramegenSwapchainPtrDX12(gameSwapChain);

    FrameInterpolationSwapChainDX12* framinterpolationSwapchain;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&framinterpolationSwapchain))))
    {
        framinterpolationSwapchain->waitForPresents();
        SafeRelease(framinterpolationSwapchain);

        return FFX_OK;
    }

    return FFX_ERROR_INVALID_ARGUMENT;
}

bool FrameInterpolationSwapChainDX12::verifyBackbufferDuplicateResources()
{
    HRESULT hr = S_OK;

    ID3D12Device*   device = nullptr;
    ID3D12Resource* buffer = nullptr;
    if (SUCCEEDED(real()->GetBuffer(0, IID_PPV_ARGS(&buffer))))
    {
        if (SUCCEEDED(buffer->GetDevice(IID_PPV_ARGS(&device))))
        {
            auto bufferDesc = buffer->GetDesc();
            D3D12_CLEAR_VALUE clearValue{ bufferDesc.Format, 0.f, 0.f, 0.f, 1.f };

            D3D12_HEAP_PROPERTIES heapProperties{};
            D3D12_HEAP_FLAGS      heapFlags;
            buffer->GetHeapProperties(&heapProperties, &heapFlags);

            heapFlags &= ~D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
            heapFlags &= ~D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
            heapFlags &= ~D3D12_HEAP_FLAG_DENY_BUFFERS;
            heapFlags &= ~D3D12_HEAP_FLAG_ALLOW_DISPLAY;

            for (size_t i = 0; i < gameBufferCount; i++)
            {
                if (replacementSwapBuffers[i].resource == nullptr)
                {
                    
                    // create game render output resource
                    if (FAILED(device->CreateCommittedResource(&heapProperties,
                                                                heapFlags,
                                                                &bufferDesc,
                                                                D3D12_RESOURCE_STATE_PRESENT,
                                                                &clearValue,
                                                                IID_PPV_ARGS(&replacementSwapBuffers[i].resource))))
                    {
                        hr |= E_FAIL;
                    }
                    else
                    {
                        uint64_t resourceSize = GetResourceGpuMemorySize(replacementSwapBuffers[i].resource);
                        totalUsageInBytes += resourceSize;
                        replacementSwapBuffers[i].resource->SetName(L"APUSR Frame Generation Replacement BackBuffer");
                    }
                }
            }

            for (size_t i = 0; i < _countof(interpolationOutputs); i++)
            {
                // create interpolation output resource
                bufferDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (interpolationOutputs[i].resource == nullptr)
                {
                    if (FAILED(device->CreateCommittedResource(
                            &heapProperties, heapFlags, &bufferDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &clearValue, IID_PPV_ARGS(&interpolationOutputs[i].resource))))
                    {
                        hr |= E_FAIL;
                    }
                    else
                    {
                        uint64_t resourceSize = GetResourceGpuMemorySize(interpolationOutputs[i].resource);
                        totalUsageInBytes += resourceSize;
                        interpolationOutputs[i].resource->SetName(L"APUSR Frame Generation Interpolation Output");
                    }
                }
            }

            SafeRelease(device);
        }

        SafeRelease(realBackBuffer0);
        realBackBuffer0 = buffer;
    }

    return SUCCEEDED(hr);
}

HRESULT FrameInterpolationSwapChainDX12::init(HWND                                  hWnd,
                                              const DXGI_SWAP_CHAIN_DESC1*          desc,
                                              const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                              ID3D12CommandQueue* queue,
                                              IDXGIFactory2* dxgiFactory)
{
    FFX_ASSERT(desc);
    FFX_ASSERT(queue);
    FFX_ASSERT(dxgiFactory);

    // store values we modify, to return when application asks for info
    gameBufferCount    = desc->BufferCount;
    gameFlags          = desc->Flags;
    gameSwapEffect     = desc->SwapEffect;

    // set default ui composition / frame interpolation present function
    presentCallback    = ffxFrameInterpolationUiComposition;

    HRESULT hr = E_FAIL;

    if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&presentInfo.device))))
    {
        presentInfo.gameQueue       = queue;

        InitializeCriticalSection(&criticalSection);
        InitializeCriticalSection(&criticalSectionUpdateConfig);
        InitializeCriticalSection(&presentInfo.criticalSectionScheduledFrame);
        presentInfo.presentEvent       = CreateEvent(NULL, FALSE, FALSE, nullptr);
        presentInfo.interpolationEvent = CreateEvent(NULL, FALSE, TRUE, nullptr);
        presentInfo.pacerEvent         = CreateEvent(NULL, FALSE, FALSE, nullptr);
        tearingSupported               = isTearingSupported(dxgiFactory);

        // Create presentation queue
        D3D12_COMMAND_QUEUE_DESC presentQueueDesc = queue->GetDesc();
        presentQueueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
        presentQueueDesc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        presentQueueDesc.Priority                 = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        presentQueueDesc.NodeMask                 = 0;
        presentInfo.device->CreateCommandQueue(&presentQueueDesc, IID_PPV_ARGS(&presentInfo.presentQueue));
        presentInfo.presentQueue->SetName(L"APUSR Frame Generation Present Queue");

        // Setup pass-through swapchain default state is disabled/passthrough
        IDXGISwapChain1* pSwapChain1 = nullptr;

        DXGI_SWAP_CHAIN_DESC1 realDesc = getInterpolationEnabledSwapChainDescription(desc);
        hr = dxgiFactory->CreateSwapChainForHwnd(presentInfo.presentQueue, hWnd, &realDesc, fullscreenDesc, nullptr, &pSwapChain1);
        if (SUCCEEDED(hr) && queue)
        {
            if (SUCCEEDED(hr = pSwapChain1->QueryInterface(IID_PPV_ARGS(&presentInfo.swapChain))))
            {
                // Register proxy swapchain to the real swap chain object
                presentInfo.swapChain->SetPrivateData(IID_IFfxFrameInterpolationSwapChain, sizeof(FrameInterpolationSwapChainDX12*), this);

                SafeRelease(pSwapChain1);
            }
            else
            {
                FFX_ASSERT_MESSAGE(hr == S_OK, "Could not query swapchain interface. Application will crash.");
                return hr;
            }
        }
        else
        {
            FFX_ASSERT_MESSAGE(hr == S_OK, "Could not create replacement swapchain. Application will crash.");
            return hr;
        }

        // init min and lax luminance according to monitor metadata
        // in case app doesn't set it through SetHDRMetadata
        getMonitorLuminanceRange(presentInfo.swapChain, &minLuminance, &maxLuminance);

        presentInfo.device->CreateFence(gameFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.gameFence));
        presentInfo.gameFence->SetName(L"APUSR Frame Generation Game Fence");

        presentInfo.device->CreateFence(interpolationFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.interpolationFence));
        presentInfo.interpolationFence->SetName(L"APUSR Frame Generation Interpolation Fence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.presentFence));
        presentInfo.presentFence->SetName(L"APUSR Frame Generation Present Fence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.replacementBufferFence));
        presentInfo.replacementBufferFence->SetName(L"APUSR Frame Generation Replacement Buffer Fence");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.compositionFenceGPU));
        presentInfo.compositionFenceGPU->SetName(L"APUSR Frame Generation Composition Fence GPU");

        presentInfo.device->CreateFence(framesSentForPresentation, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&presentInfo.compositionFenceCPU));
        presentInfo.compositionFenceCPU->SetName(L"APUSR Frame Generation Composition Fence CPU");

        if ((desc->Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) == DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        {
            hr = frameLatencyWaitable.initialize(gameMaximumFrameLatency, FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT);
            if (FAILED(hr))
                return hr;
        }

        // Create interpolation queue
        D3D12_COMMAND_QUEUE_DESC queueDesc = queue->GetDesc();
        queueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        queueDesc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.Priority                 = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        queueDesc.NodeMask                 = 0;
        presentInfo.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&presentInfo.asyncComputeQueue));
        presentInfo.asyncComputeQueue->SetName(L"APUSR Frame Generation Async Compute Queue");

        // Default to dispatch interpolation workloads on the game queue
        presentInfo.interpolationQueue = presentInfo.gameQueue;

        profileStream = apusr::profile::registerStream(apusr::profile::StreamDesc{
            .backend = apusr::profile::BackendApi::Dx12,
            .component = apusr::profile::Component::FrameGeneration,
            .name = "framegen.present",
            .capabilities = apusr::profile::StreamCapabilityCpuIntervals |
                apusr::profile::StreamCapabilityFrames |
                apusr::profile::StreamCapabilityGpuIntervals |
                apusr::profile::StreamCapabilityCalibration |
                apusr::profile::StreamCapabilityCounters |
                apusr::profile::StreamCapabilityInstants |
                apusr::profile::StreamCapabilityErrors |
                apusr::profile::StreamCapabilityQueueEvents |
                apusr::profile::StreamCapabilityDecisions |
                apusr::profile::StreamCapabilityFlows |
                apusr::profile::StreamCapabilityThreadInfo |
                apusr::profile::StreamCapabilityWaitIntervals,
            .nativeDevice = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(presentInfo.device)),
            .nativeQueue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(presentInfo.presentQueue)),
        });
        profileSampleGate.reset();
        presentInfo.profileStream = profileStream;
        presentInfo.profileQueueSampleGate.reset();
        presentInfo.profileInterpolationSampleGate.reset();
    }

    return hr;
}

FrameInterpolationSwapChainDX12::FrameInterpolationSwapChainDX12()
{

}

FrameInterpolationSwapChainDX12::~FrameInterpolationSwapChainDX12()
{
    shutdown();
}

UINT FrameInterpolationSwapChainDX12::getInterpolationEnabledSwapChainFlags(UINT nonAdjustedFlags)
{
    UINT flags = nonAdjustedFlags;

    // The DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT flag changes the D3D runtime behavior for fences
    // We will make our own waitable object for the app to wait on, but we need to keep the flag 
    flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    if (tearingSupported)
    {
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    return flags;
}

DXGI_SWAP_CHAIN_DESC1 FrameInterpolationSwapChainDX12::getInterpolationEnabledSwapChainDescription(const DXGI_SWAP_CHAIN_DESC1* nonAdjustedDesc)
{
    DXGI_SWAP_CHAIN_DESC1 fiDesc = *nonAdjustedDesc;

    // adjust swap chain descriptor to fit FI requirements
    fiDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    fiDesc.BufferCount = 3;
    fiDesc.Flags = getInterpolationEnabledSwapChainFlags(fiDesc.Flags);

    return fiDesc;
}

IDXGISwapChain4* FrameInterpolationSwapChainDX12::real()
{
    return presentInfo.swapChain;
}

HRESULT FrameInterpolationSwapChainDX12::shutdown()
{
    //m_pDevice will be nullptr already shutdown
    if (presentInfo.device)
    {
        destroyReplacementResources();
        
        EnterCriticalSection(&criticalSection);
        killPresenterThread();
        releaseUiBlitGpuResources();
        LeaveCriticalSection(&criticalSection);

        SafeCloseHandle(presentInfo.presentEvent);
        SafeCloseHandle(presentInfo.interpolationEvent);
        SafeCloseHandle(presentInfo.pacerEvent);

        frameLatencyWaitable.shutdown();

        // if we failed initialization, we may not have an interpolation queue or fence
        if (presentInfo.interpolationQueue)
        {
            if (presentInfo.interpolationFence)
            {
                presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, ++interpolationFenceValue);
                waitForFenceValue(presentInfo.interpolationFence, interpolationFenceValue, INFINITE, presentInfo.waitCallback);
            }
        }
        
        SafeRelease(presentInfo.asyncComputeQueue);
        SafeRelease(presentInfo.presentQueue);
        SafeRelease(presentInfo.profileAdapter);

        SafeRelease(presentInfo.interpolationFence);
        SafeRelease(presentInfo.presentFence);
        SafeRelease(presentInfo.replacementBufferFence);
        SafeRelease(presentInfo.compositionFenceGPU);
        SafeRelease(presentInfo.compositionFenceCPU);

        std::ignore = SafeRelease(presentInfo.swapChain);

        if (presentInfo.gameFence)
        {
            waitForFenceValue(presentInfo.gameFence, gameFenceValue, INFINITE, presentInfo.waitCallback);
        }
        SafeRelease(presentInfo.gameFence);

        DeleteCriticalSection(&criticalSection);
        DeleteCriticalSection(&criticalSectionUpdateConfig);
        DeleteCriticalSection(&presentInfo.criticalSectionScheduledFrame);

        std::ignore = SafeRelease(presentInfo.device);
    }

    return S_OK;
}

bool FrameInterpolationSwapChainDX12::killPresenterThread()
{
    if (interpolationThreadHandle != NULL)
    {
        // prepare present CPU thread for shutdown
        presentInfo.shutdown = true;

        // signal event to allow thread to finish
        SetEvent(presentInfo.presentEvent);
        WaitForSingleObject(interpolationThreadHandle, INFINITE);
        SafeCloseHandle(interpolationThreadHandle);
    }

    return interpolationThreadHandle == nullptr;
}

bool FrameInterpolationSwapChainDX12::spawnPresenterThread()
{
    if (interpolationThreadHandle == NULL)
    {
        presentInfo.shutdown      = false;
        interpolationThreadHandle = CreateThread(nullptr, 0, apusr::framegen::dx12::interpolationThread, reinterpret_cast<void*>(&presentInfo), 0, nullptr);
        
        FFX_ASSERT(interpolationThreadHandle != NULL);

        if (interpolationThreadHandle != 0)
        {
            SetThreadPriority(interpolationThreadHandle, THREAD_PRIORITY_HIGHEST);
            SetThreadDescription(interpolationThreadHandle, L"APUSR Frame Generation Interpolation Thread");
        }

        SetEvent(presentInfo.interpolationEvent);
    }

    return interpolationThreadHandle != NULL;
}

void FrameInterpolationSwapChainDX12::discardOutstandingInterpolationCommandLists()
{
    // drop any outstanding interpolaton command lists
    for (size_t i = 0; i < _countof(registeredInterpolationCommandLists); i++)
    {
        if (registeredInterpolationCommandLists[i] != nullptr)
        {
            registeredInterpolationCommandLists[i]->drop(true);
            registeredInterpolationCommandLists[i] = nullptr;
        }
    }
}

void FrameInterpolationSwapChainDX12::setFrameGenerationConfig(ApusrFramegenConfig const* config)
{
    EnterCriticalSection(&criticalSectionUpdateConfig);

    FFX_ASSERT(config);

    // if config is a pointer to the internal config ::present called this function to apply the changes
    bool applyChangesNow = (config == &nextFrameGenerationConfig);
        
    ApusrFramegenPresentCallbackFunc inputPresentCallback    = (nullptr != config->presentCallback) ? config->presentCallback : ffxFrameInterpolationUiComposition;
    void*                  inputPresentCallbackCtx = (nullptr != config->presentCallback) ? config->presentCallbackContext : nullptr;
    ID3D12CommandQueue*    inputInterpolationQueue = config->allowAsyncWorkloads ? presentInfo.asyncComputeQueue : presentInfo.gameQueue;

    // if this is called externally just copy the new config to the internal copy to avoid potentially stalling on criticalSection
    if (!applyChangesNow)
    {  
        nextFrameGenerationConfig = *config;

        // in case of actual reconfiguration: apply the changes immediately
        if ( presentInfo.interpolationQueue != inputInterpolationQueue 
           || interpolationEnabled != config->frameGenerationEnabled 
           || presentCallback != inputPresentCallback
           || presentCallbackContext != inputPresentCallbackCtx 
           || frameGenerationCallback != config->frameGenerationCallback
           || frameGenerationCallbackContext != config->frameGenerationCallbackContext
           || drawDebugPacingLines != config->drawDebugPacingLines)
        {
            applyChangesNow = true;
        }
    }

    if (applyChangesNow)
    {
        EnterCriticalSection(&criticalSection);

        currentFrameID          = config->frameID;
        presentInterpolatedOnly = config->onlyPresentInterpolated;
        interpolationRect       = config->interpolationRect;
        drawDebugPacingLines    = config->drawDebugPacingLines;

        if (presentInfo.interpolationQueue != inputInterpolationQueue)
        {
            waitForPresents();
            discardOutstandingInterpolationCommandLists();

            // change interpolation queue and reset fence value
            presentInfo.interpolationQueue = inputInterpolationQueue;
            interpolationFenceValue        = 0;
            presentInfo.interpolationQueue->Signal(presentInfo.interpolationFence, interpolationFenceValue);
        }

        if (interpolationEnabled != config->frameGenerationEnabled || presentCallback != inputPresentCallback ||
            frameGenerationCallback != config->frameGenerationCallback || configFlags != (FfxFrameInterpolationDispatchFlags)config->flags ||
            presentCallbackContext != inputPresentCallbackCtx || frameGenerationCallbackContext != config->frameGenerationCallbackContext)
        {
            waitForPresents();
            presentCallback                = inputPresentCallback;
            presentCallbackContext         = inputPresentCallbackCtx;
            frameGenerationCallback        = config->frameGenerationCallback;
            configFlags                    = FfxFrameInterpolationDispatchFlags(config->flags);
            frameGenerationCallbackContext = config->frameGenerationCallbackContext;

            // handle interpolation mode change
            if (interpolationEnabled != config->frameGenerationEnabled)
            {
                interpolationEnabled = config->frameGenerationEnabled;
                if (interpolationEnabled)
                {
                    frameInterpolationResetCondition = true;
                    nextPresentWaitValue             = framesSentForPresentation;

                    spawnPresenterThread();
                }
                else
                {
                    killPresenterThread();
                }
            }
        }
        LeaveCriticalSection(&criticalSection);
    }

    LeaveCriticalSection(&criticalSectionUpdateConfig);
}

bool FrameInterpolationSwapChainDX12::destroyReplacementResources()
{
    HRESULT hr = S_OK;

    EnterCriticalSection(&criticalSection);

    waitForPresents();

    const bool recreatePresenterThread = interpolationThreadHandle != nullptr;
    if (recreatePresenterThread)
    {
        killPresenterThread();
    }

    discardOutstandingInterpolationCommandLists();

    {
        for (size_t i = 0; i < _countof(replacementSwapBuffers); i++)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(replacementSwapBuffers[i].resource);
            totalUsageInBytes -= resourceSize;
            replacementSwapBuffers[i].destroy();
        }

        SafeRelease(realBackBuffer0);
        
        for (size_t i = 0; i < _countof(interpolationOutputs); i++)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(interpolationOutputs[i].resource);
            totalUsageInBytes -= resourceSize;
            interpolationOutputs[i].destroy();
        }

        if (uiReplacementBuffer.resource !=nullptr)
        {
            uint64_t resourceSize = GetResourceGpuMemorySize(uiReplacementBuffer.resource);
            totalUsageInBytes -= resourceSize;
        }
        
        uiReplacementBuffer.destroy();
    }

    // reset counters used in buffer management
    framesSentForPresentation        = 0;
    nextPresentWaitValue             = 0;
    replacementSwapBufferIndex       = 0;
    presentCount                     = 0;
    interpolationFenceValue          = 0;
    gameFenceValue                   = 0;

    // if we didn't init correctly, some parameters may not exist
    if (presentInfo.gameFence)
    {
        presentInfo.gameFence->Signal(gameFenceValue);
    }
    
    if (presentInfo.interpolationFence)
    {
        presentInfo.interpolationFence->Signal(interpolationFenceValue);
    }
    
    if (presentInfo.presentFence)
    {
        presentInfo.presentFence->Signal(framesSentForPresentation);
    }

    if (presentInfo.replacementBufferFence)
    {
        presentInfo.replacementBufferFence->Signal(framesSentForPresentation);
    }

    if (presentInfo.compositionFenceGPU)
    {
        presentInfo.compositionFenceGPU->Signal(framesSentForPresentation);
    }

    if (presentInfo.compositionFenceCPU)
    {
        presentInfo.compositionFenceCPU->Signal(framesSentForPresentation);
    }

    frameInterpolationResetCondition = true;

    if (recreatePresenterThread)
    {
        spawnPresenterThread();
    }

    discardOutstandingInterpolationCommandLists();

    LeaveCriticalSection(&criticalSection);

    return SUCCEEDED(hr);
}

bool FrameInterpolationSwapChainDX12::waitForPresents()
{
    // wait for interpolation to finish
    waitForFenceValue(presentInfo.gameFence, gameFenceValue, INFINITE, presentInfo.waitCallback);
    waitForFenceValue(presentInfo.interpolationFence, interpolationFenceValue, INFINITE, presentInfo.waitCallback);
    waitForFenceValue(presentInfo.presentFence, framesSentForPresentation, INFINITE, presentInfo.waitCallback);

    return true;
}

FfxApiResource FrameInterpolationSwapChainDX12::interpolationOutput(int index)
{
    index = interpolationBufferIndex;

    FfxApiResourceDescription interpolateDesc = apusrFramegenResourceDescriptionDX12(interpolationOutputs[index].resource);
    return apusrFramegenResourceDX12(interpolationOutputs[index].resource, interpolateDesc, nullptr, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

//IUnknown
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::QueryInterface(REFIID riid, void** ppvObject)
{

    const GUID guidReplacements[] = {
        __uuidof(this),
        IID_IUnknown,
        IID_IDXGIObject,
        IID_IDXGIDeviceSubObject,
        IID_IDXGISwapChain,
        IID_IDXGISwapChain1,
        IID_IDXGISwapChain2,
        IID_IDXGISwapChain3,
        IID_IDXGISwapChain4,
        IID_IFfxFrameInterpolationSwapChain
    };

    for (auto guid : guidReplacements)
    {
        if (IsEqualGUID(riid, guid) == TRUE)
        {
            AddRef();
            *ppvObject = this;
            return S_OK;
        }
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::AddRef()
{
    InterlockedIncrement(&refCount);

    return refCount;
}

ULONG STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::Release()
{
    ULONG ref = InterlockedDecrement(&refCount);
    if (ref != 0)
    {
        return refCount;
    }

    delete this;

    return 0;
}

// IDXGIObject
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return real()->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return real()->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return real()->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetParent(REFIID riid, void** ppParent)
{
    return real()->GetParent(riid, ppParent);
}

// IDXGIDeviceSubObject
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetDevice(REFIID riid, void** ppDevice)
{
    return real()->GetDevice(riid, ppDevice);
}

void FrameInterpolationSwapChainDX12::registerUiResource(FfxApiResource uiResource, uint32_t flags)
{
    EnterCriticalSection(&criticalSection);

    presentInfo.currentUiSurface = uiResource;
    presentInfo.uiCompositionFlags = flags;
    if (nullptr == uiResource.resource)
        presentInfo.uiCompositionFlags &= ~APUSR_FRAMEGEN_UI_COMPOSITION_INTERNAL_DOUBLE_BUFFER;

    LeaveCriticalSection(&criticalSection);
}

void FrameInterpolationSwapChainDX12::setWaitCallback(FfxWaitCallbackFunc waitCallbackFunc)
{
    presentInfo.waitCallback = waitCallbackFunc;
}

void FrameInterpolationSwapChainDX12::setFramePacingTuning(const FfxApiSwapchainFramePacingTuning* framePacingTuning)
{
    presentInfo.safetyMarginInSec = static_cast<double> (framePacingTuning->safetyMarginInMs) / 1000.0;
    presentInfo.varianceFactor = static_cast<double> (framePacingTuning->varianceFactor);
    presentInfo.allowHybridSpin = framePacingTuning->allowHybridSpin;
    presentInfo.hybridSpinTime = framePacingTuning->hybridSpinTime;
    presentInfo.allowWaitForSingleObjectOnFence = framePacingTuning->allowWaitForSingleObjectOnFence;
}

void FrameInterpolationSwapChainDX12::GetGpuMemoryUsage(FfxApiEffectMemoryUsage* vramUsage)
{
    vramUsage->totalUsageInBytes = totalUsageInBytes;
    vramUsage->aliasableUsageInBytes = aliasableUsageInBytes;
}

    // IDXGISwapChain1
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::Present(UINT SyncInterval, UINT Flags)
{
    return apusr::framegen::dx12::PresentPath::present(*this, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    EnterCriticalSection(&criticalSection);

    HRESULT hr = E_FAIL;

    if (riid == IID_ID3D12Resource || riid == IID_ID3D12Resource1 || riid == IID_ID3D12Resource2)
    {
        if (verifyBackbufferDuplicateResources())
        {
            ID3D12Resource* pBuffer = replacementSwapBuffers[Buffer].resource;

            pBuffer->AddRef();
            *ppSurface = pBuffer;

            hr = S_OK;
        }
    }

    LeaveCriticalSection(&criticalSection);

    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    return real()->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    return real()->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    HRESULT hr = real()->GetDesc(pDesc);

    // hide interpolation swapchaindesc to keep FI transparent for ISV
    if (SUCCEEDED(hr))
    {
        //update values we changed
        pDesc->BufferCount  = gameBufferCount;
        pDesc->Flags        = gameFlags;
        pDesc->SwapEffect   = gameSwapEffect;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
    if (BufferCount > FFX_FRAME_INTERPOLATION_SWAP_CHAIN_MAX_BUFFER_COUNT)
        return DXGI_ERROR_INVALID_CALL;

    destroyReplacementResources();

    EnterCriticalSection(&criticalSection);

    const UINT fiAdjustedFlags = getInterpolationEnabledSwapChainFlags(SwapChainFlags);
    const UINT resizedGameBufferCount = BufferCount > 0 ? BufferCount : gameBufferCount;
    const UINT resizedMaximumFrameLatency = std::min(gameMaximumFrameLatency, resizedGameBufferCount);

    const HRESULT hr = real()->ResizeBuffers(0 /* preserve internal count */, Width, Height, NewFormat, fiAdjustedFlags);
    if (SUCCEEDED(hr)) {
        const HRESULT latencyHr = frameLatencyWaitable.setLatency(gameMaximumFrameLatency, resizedMaximumFrameLatency);
        if (FAILED(latencyHr)) {
            LeaveCriticalSection(&criticalSection);
            return latencyHr;
        }

        gameBufferCount = resizedGameBufferCount;
        gameMaximumFrameLatency = resizedMaximumFrameLatency;
        gameFlags = SwapChainFlags;
    }

    LeaveCriticalSection(&criticalSection);
    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return real()->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetContainingOutput(IDXGIOutput** ppOutput)
{
    HRESULT hr = DXGI_ERROR_INVALID_CALL;

    if (ppOutput)
    {
        *ppOutput = getMostRelevantOutputFromSwapChain(real());
        if (*ppOutput)
            hr = S_OK;
    }
    
    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return real()->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetLastPresentCount(UINT* pLastPresentCount)
{
    return real()->GetLastPresentCount(pLastPresentCount);
}

// IDXGISwapChain1
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    HRESULT hr = real()->GetDesc1(pDesc);

    // hide interpolation swapchaindesc to keep FI transparent for ISV
    if (SUCCEEDED(hr))
    {
        //update values we changed
        pDesc->BufferCount  = gameBufferCount;
        pDesc->Flags        = gameFlags;
        pDesc->SwapEffect   = gameSwapEffect;
    }

    return hr;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    return real()->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetHwnd(HWND* pHwnd)
{
    return real()->GetHwnd(pHwnd);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return real()->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS*)
{
    return Present(SyncInterval, PresentFlags);
}

BOOL STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::IsTemporaryMonoSupported(void)
{
    return real()->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return real()->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return real()->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return real()->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return real()->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return real()->GetRotation(pRotation);
}

// IDXGISwapChain2
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetSourceSize(UINT Width, UINT Height)
{
    return real()->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return real()->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetMaximumFrameLatency(UINT MaxLatency)
{
    const UINT newMaximumFrameLatency = std::min(MaxLatency, gameBufferCount);
    const HRESULT hr = frameLatencyWaitable.setLatency(gameMaximumFrameLatency, newMaximumFrameLatency);
    if (FAILED(hr))
        return hr;

    gameMaximumFrameLatency = newMaximumFrameLatency;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (pMaxLatency)
    {
        *pMaxLatency = gameMaximumFrameLatency;
    }

    return pMaxLatency ? S_OK : DXGI_ERROR_INVALID_CALL;
}

HANDLE STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetFrameLatencyWaitableObject(void)
{
    return frameLatencyWaitable.duplicateForApplication();
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return real()->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return real()->GetMatrixTransform(pMatrix);
}

// IDXGISwapChain3
UINT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::GetCurrentBackBufferIndex(void)
{
    EnterCriticalSection(&criticalSection);

    UINT result = (UINT)replacementSwapBufferIndex;

    LeaveCriticalSection(&criticalSection);

    return result;
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport)
{
    return real()->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    switch (ColorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ;
        break;
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        backBufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;
        break;
    default:
        break;
    }

    return real()->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::ResizeBuffers1(
    UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*)
{
    // ResizeBuffers1 carries per-buffer node masks and presentation queues. The
    // current single-adapter proxy cannot honor that contract, so fail instead
    // of reporting success without resizing anything.
    return DXGI_ERROR_UNSUPPORTED;
}

// IDXGISwapChain4
HRESULT STDMETHODCALLTYPE FrameInterpolationSwapChainDX12::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (Size > 0 && pMetaData != nullptr)
    {
        DXGI_HDR_METADATA_HDR10* HDR10MetaData = NULL;

        switch (Type)
        {
        case DXGI_HDR_METADATA_TYPE_NONE:
            break;
        case DXGI_HDR_METADATA_TYPE_HDR10:
            HDR10MetaData = static_cast<DXGI_HDR_METADATA_HDR10*>(pMetaData);
            break;
        case DXGI_HDR_METADATA_TYPE_HDR10PLUS:
            break;
        }

        FFX_ASSERT_MESSAGE(HDR10MetaData != NULL, "FSR3 Frame interpolaton pxory swapchain: could not initialize HDR metadata");

        if (HDR10MetaData)
        {
            minLuminance = HDR10MetaData->MinMasteringLuminance / 10000.0f;
            maxLuminance = float(HDR10MetaData->MaxMasteringLuminance);
        }
    }

    return real()->SetHDRMetaData(Type, Size, pMetaData);
}

ID3D12GraphicsCommandList* FrameInterpolationSwapChainDX12::getInterpolationCommandList()
{
    EnterCriticalSection(&criticalSection);

    ID3D12GraphicsCommandList* dx12CommandList = nullptr;

    // store active backbuffer index to the command list, used to verify list usage later
    if (interpolationEnabled) {
        ID3D12Resource* currentBackBuffer = nullptr;
        const UINT      currentBackBufferIndex = GetCurrentBackBufferIndex();
        if (SUCCEEDED(GetBuffer(currentBackBufferIndex, IID_PPV_ARGS(&currentBackBuffer))))
        {
            Dx12Commands* registeredCommands = registeredInterpolationCommandLists[currentBackBufferIndex];

            // drop if already existing
            if (registeredCommands != nullptr)
            {
                registeredCommands->drop(true);
                registeredCommands = nullptr;
            }

            registeredCommands = presentInfo.commandPool.get(presentInfo.interpolationQueue, L"getInterpolationCommandList()");
            FFX_ASSERT(registeredCommands);

            dx12CommandList = registeredCommands->reset();

            registeredInterpolationCommandLists[currentBackBufferIndex] = registeredCommands;

            SafeRelease(currentBackBuffer);
        }
    }
    
    LeaveCriticalSection(&criticalSection);

    return dx12CommandList;
}
