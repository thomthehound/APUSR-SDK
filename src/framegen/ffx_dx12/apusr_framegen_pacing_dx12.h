// SPDX-License-Identifier: MIT
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// Copyright (c) 2026 thomthehound.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <dxgi1_6.h>

namespace apusr::framegen::dx12 {

void markPresentedBuffer(IDXGISwapChain4* swapchain, bool interpolated);
DWORD WINAPI interpolationThread(LPVOID context);

} // namespace apusr::framegen::dx12
