// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "ffx_dx12/apusr_framegen_frame_latency_dx12.h"

#include <cassert>
#include <iostream>

int main()
{
    apusr::framegen::dx12::FrameLatencyWaitable waitable;
    assert(SUCCEEDED(waitable.initialize(1, 3)));

    HANDLE first = waitable.duplicateForApplication();
    HANDLE second = waitable.duplicateForApplication();
    assert(first != nullptr);
    assert(second != nullptr);
    assert(first != second);

    // Changing maximum latency must expose exactly that many credits. The GPU
    // completion worker must not manufacture startup credits of its own.
    assert(SUCCEEDED(waitable.setLatency(1, 3)));
    assert(SUCCEEDED(waitable.setLatency(3, 1)));
    assert(WaitForSingleObject(first, 0) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(second, 0) == WAIT_TIMEOUT);

    // Multiple completions can become ready before the worker runs. They must
    // remain distinct credits instead of coalescing through one auto-reset event.
    ID3D12Device* device = nullptr;
    assert(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))));

    ID3D12Fence* fence = nullptr;
    assert(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))));
    assert(SUCCEEDED(waitable.signalOnCompletion(fence, 1)));
    assert(SUCCEEDED(waitable.signalOnCompletion(fence, 2)));
    assert(SUCCEEDED(waitable.signalOnCompletion(fence, 3)));
    assert(SUCCEEDED(fence->Signal(3)));

    assert(WaitForSingleObject(second, 1000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(second, 1000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(second, 1000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(second, 0) == WAIT_TIMEOUT);

    fence->Release();
    device->Release();

    // Each caller owns an independent handle. Closing one must not invalidate
    // either APUSR's internal handle or a second caller's handle.
    CloseHandle(first);
    first = nullptr;

    waitable.shutdown();

    // Shutdown fills the semaphore before APUSR drops its reference so an
    // application still waiting on its duplicate cannot be stranded.
    assert(WaitForSingleObject(second, 1000) == WAIT_OBJECT_0);
    CloseHandle(second);

    assert(waitable.duplicateForApplication() == nullptr);
    std::cout << "APUSR frame-latency waitable: exact credits, completion fan-in, independent handles, and shutdown lifetime validated\n";
    return 0;
}
