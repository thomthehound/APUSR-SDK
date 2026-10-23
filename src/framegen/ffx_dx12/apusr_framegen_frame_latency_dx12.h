// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>

#include <array>
#include <cstdint>

namespace apusr::framegen::dx12 {

// Owns the replacement DXGI frame-latency waitable object. The application
// receives duplicated semaphore handles; APUSR keeps the GPU-completion events,
// signaling thread, and original semaphore handle private to the swapchain.
class FrameLatencyWaitable final {
public:
    FrameLatencyWaitable() = default;
    ~FrameLatencyWaitable();

    FrameLatencyWaitable(const FrameLatencyWaitable&) = delete;
    FrameLatencyWaitable& operator=(const FrameLatencyWaitable&) = delete;

    HRESULT initialize(UINT latency, UINT capacity) noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool active() const noexcept { return semaphore_ != nullptr; }
    [[nodiscard]] HANDLE duplicateForApplication() const noexcept;

    HRESULT setLatency(UINT oldLatency, UINT newLatency) noexcept;
    HRESULT signalOnCompletion(ID3D12Fence* fence, UINT64 fenceValue) noexcept;

private:
    static DWORD WINAPI signalThreadEntry(LPVOID context) noexcept;
    DWORD signalThread() noexcept;
    void fillSemaphore() noexcept;

    HANDLE semaphore_ = nullptr;
    HANDLE shutdownEvent_ = nullptr;
    HANDLE signalThread_ = nullptr;
    std::array<HANDLE, MAXIMUM_WAIT_OBJECTS - 1> completionEvents_{};
    std::array<uint8_t, MAXIMUM_WAIT_OBJECTS - 1> completionPending_{};
    SRWLOCK completionLock_{};
    UINT capacity_ = 0;
    UINT nextCompletionSlot_ = 0;
};

} // namespace apusr::framegen::dx12
