// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "apusr_framegen_provider.h"

const ApusrFramegenProvider* apusrFramegenFindProvider(ffxStructType_t descType, std::uint64_t overrideId, void* device) noexcept;
const ApusrFramegenProvider* apusrFramegenAssociatedProvider(ffxContext* context) noexcept;
std::uint64_t apusrFramegenProviderCount(ffxStructType_t descType, void* device) noexcept;
std::uint64_t apusrFramegenProviderVersions(ffxStructType_t descType,
                                            void* device,
                                            std::uint64_t capacity,
                                            std::uint64_t* versionIds,
                                            const char** versionNames) noexcept;
