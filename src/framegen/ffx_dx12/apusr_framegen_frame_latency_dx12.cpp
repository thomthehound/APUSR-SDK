// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr_framegen_frame_latency_dx12.h"

#include <algorithm>

namespace apusr::framegen::dx12 {
namespace {
HRESULT lastErrorAsHresult() noexcept
{
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
}

void closeHandle(HANDLE& handle) noexcept
{
    if (!handle)
        return;
    CloseHandle(handle);
    handle = nullptr;
}
} // namespace

FrameLatencyWaitable::~FrameLatencyWaitable()
{
    shutdown();
}

HRESULT FrameLatencyWaitable::initialize(UINT latency, UINT capacity) noexcept
{
    shutdown();

    // One wait handle is reserved for shutdown. DXGI's maximum swap-chain
    // buffer count is comfortably below the Win32 multiple-object wait limit.
    if (capacity == 0 || capacity >= MAXIMUM_WAIT_OBJECTS)
        return E_INVALIDARG;

    capacity_ = capacity;
    latency = std::min(latency, capacity_);

    semaphore_ = CreateSemaphoreW(nullptr, static_cast<LONG>(latency), static_cast<LONG>(capacity_), nullptr);
    if (!semaphore_) {
        const HRESULT error = lastErrorAsHresult();
        shutdown();
        return error;
    }

    shutdownEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!shutdownEvent_) {
        const HRESULT error = lastErrorAsHresult();
        shutdown();
        return error;
    }

    for (UINT i = 0; i < capacity_; ++i) {
        HANDLE& event = completionEvents_[i];
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            const HRESULT error = lastErrorAsHresult();
            shutdown();
            return error;
        }
    }

    signalThread_ = CreateThread(nullptr, 0, &FrameLatencyWaitable::signalThreadEntry, this, 0, nullptr);
    if (!signalThread_) {
        const HRESULT error = lastErrorAsHresult();
        shutdown();
        return error;
    }

    SetThreadPriority(signalThread_, THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(signalThread_, L"APUSR Frame Latency Signal Thread");
    return S_OK;
}

void FrameLatencyWaitable::shutdown() noexcept
{
    if (signalThread_) {
        if (shutdownEvent_)
            SetEvent(shutdownEvent_);
        WaitForSingleObject(signalThread_, INFINITE);
        closeHandle(signalThread_);
    }

    for (UINT i = 0; i < capacity_; ++i) {
        closeHandle(completionEvents_[i]);
        completionPending_[i] = 0;
    }

    closeHandle(shutdownEvent_);
    closeHandle(semaphore_);
    capacity_ = 0;
    nextCompletionSlot_ = 0;
}

HANDLE FrameLatencyWaitable::duplicateForApplication() const noexcept
{
    if (!semaphore_)
        return nullptr;

    HANDLE duplicated = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(),
                         semaphore_,
                         GetCurrentProcess(),
                         &duplicated,
                         0,
                         FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return duplicated;
}

HRESULT FrameLatencyWaitable::setLatency(UINT oldLatency, UINT newLatency) noexcept
{
    if (!semaphore_)
        return S_OK;

    oldLatency = std::min(oldLatency, capacity_);
    newLatency = std::min(newLatency, capacity_);
    const int delta = static_cast<int>(newLatency) - static_cast<int>(oldLatency);

    if (delta > 0)
        return ReleaseSemaphore(semaphore_, delta, nullptr) ? S_OK : lastErrorAsHresult();

    for (int i = 0; i < -delta; ++i) {
        const DWORD waitResult = WaitForSingleObject(semaphore_, INFINITE);
        if (waitResult != WAIT_OBJECT_0)
            return waitResult == WAIT_FAILED ? lastErrorAsHresult() : E_FAIL;
    }
    return S_OK;
}

HRESULT FrameLatencyWaitable::signalOnCompletion(ID3D12Fence* fence, UINT64 fenceValue) noexcept
{
    if (!semaphore_)
        return S_OK;
    if (!fence || capacity_ == 0)
        return E_INVALIDARG;

    UINT slot = capacity_;
    AcquireSRWLockExclusive(&completionLock_);
    for (UINT offset = 0; offset < capacity_; ++offset) {
        const UINT candidate = (nextCompletionSlot_ + offset) % capacity_;
        if (completionPending_[candidate] == 0) {
            slot = candidate;
            completionPending_[candidate] = 1;
            nextCompletionSlot_ = (candidate + 1) % capacity_;
            break;
        }
    }
    ReleaseSRWLockExclusive(&completionLock_);

    if (slot == capacity_)
        return HRESULT_FROM_WIN32(ERROR_BUSY);

    const HRESULT result = fence->SetEventOnCompletion(fenceValue, completionEvents_[slot]);
    if (FAILED(result)) {
        AcquireSRWLockExclusive(&completionLock_);
        completionPending_[slot] = 0;
        ReleaseSRWLockExclusive(&completionLock_);
    }
    return result;
}

DWORD WINAPI FrameLatencyWaitable::signalThreadEntry(LPVOID context) noexcept
{
    auto* waitable = static_cast<FrameLatencyWaitable*>(context);
    return waitable ? waitable->signalThread() : ERROR_INVALID_PARAMETER;
}

DWORD FrameLatencyWaitable::signalThread() noexcept
{
    std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> waitHandles{};
    const DWORD waitHandleCount = capacity_ + 1;
    waitHandles[0] = shutdownEvent_;
    for (UINT i = 0; i < capacity_; ++i)
        waitHandles[i + 1] = completionEvents_[i];

    for (;;) {
        const DWORD waitResult =
            WaitForMultipleObjects(waitHandleCount, waitHandles.data(), FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0)
            break;
        if (waitResult < WAIT_OBJECT_0 + 1 ||
            waitResult >= WAIT_OBJECT_0 + waitHandleCount)
            break;

        const UINT slot = waitResult - WAIT_OBJECT_0 - 1;
        bool releaseCredit = false;
        AcquireSRWLockExclusive(&completionLock_);
        if (completionPending_[slot] != 0) {
            completionPending_[slot] = 0;
            releaseCredit = true;
        }
        ReleaseSRWLockExclusive(&completionLock_);

        if (releaseCredit && !ReleaseSemaphore(semaphore_, 1, nullptr) && GetLastError() != ERROR_TOO_MANY_POSTS)
            break;
    }

    // A duplicated application handle may outlive the swapchain. Fill the
    // semaphore before releasing APUSR's handle so a waiter cannot remain
    // stranded merely because Frame Generation was disabled or destroyed.
    fillSemaphore();
    return 0;
}

void FrameLatencyWaitable::fillSemaphore() noexcept
{
    if (!semaphore_)
        return;

    for (UINT i = 0; i < capacity_; ++i) {
        if (!ReleaseSemaphore(semaphore_, 1, nullptr))
            break;
    }
}

} // namespace apusr::framegen::dx12
