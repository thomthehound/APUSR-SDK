// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "algorithm/apusr_framegen_algorithm_types.h"

namespace apusr::framegen::algorithm {

// Storage-level entry points let the external FidelityFX ABI use its oversized
// opaque contexts without leaking those compatibility types into the algorithms.
ApusrFramegenResult frameInterpolationContextCreateStorage(
    void* contextStorage,
    const ApusrFrameInterpolationContextDescription* description) noexcept;
ApusrFramegenResult frameInterpolationContextDestroyStorage(void* contextStorage) noexcept;
ApusrFramegenResult frameInterpolationContextGetGpuMemoryUsageStorage(
    void* contextStorage,
    ApusrFramegenMemoryUsage* usage) noexcept;
ApusrFramegenResult frameInterpolationGetSharedResourceDescriptionsStorage(
    void* contextStorage,
    ApusrFrameInterpolationSharedResourceDescriptions* descriptions) noexcept;
ApusrFramegenResult frameInterpolationPrepareStorage(
    void* contextStorage,
    const ApusrFrameInterpolationPrepareDescription* description) noexcept;
ApusrFramegenResult frameInterpolationDispatchStorage(
    void* contextStorage,
    const ApusrFrameInterpolationDispatchDescription* description) noexcept;
ApusrFramegenResult frameInterpolationEnqueueRefreshPipelineRequestStorage(void* contextStorage) noexcept;

ApusrFramegenResult opticalFlowContextCreateStorage(
    void* contextStorage,
    const ApusrOpticalFlowContextDescription* description) noexcept;
ApusrFramegenResult opticalFlowContextDestroyStorage(void* contextStorage) noexcept;
ApusrFramegenResult opticalFlowContextGetGpuMemoryUsageStorage(
    void* contextStorage,
    ApusrFramegenMemoryUsage* usage) noexcept;
ApusrFramegenResult opticalFlowGetSharedResourceDescriptionsStorage(
    void* contextStorage,
    ApusrOpticalFlowSharedResourceDescriptions* descriptions) noexcept;
ApusrFramegenResult opticalFlowDispatchStorage(
    void* contextStorage,
    const ApusrOpticalFlowDispatchDescription* description) noexcept;

} // namespace apusr::framegen::algorithm
