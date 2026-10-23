// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include "ffx_api.hpp"
#include "ffx_internal_types.h"
#include "../apusr_framegen_allocator.h"


#define VERIFY(_cond, _retcode) \
    do { if (!(_cond)) return (_retcode); } while (false)

#define TRY(_expr) \
    do { if (ffxReturnCode_t _rc = (_expr); _rc != FFX_API_RETURN_OK) return _rc; } while (false)

#define TRY2(_expr) \
    do { if (FfxErrorCode _rc = (_expr); _rc != FFX_OK) return FFX_API_RETURN_ERROR_RUNTIME_ERROR; } while (false)

class ApusrFramegenProvider {
public:
    virtual ~ApusrFramegenProvider() = default;

    virtual bool canProvide(ffxStructType_t type) const noexcept = 0;
    virtual bool isSupported(void* device) const noexcept { (void)device; return true; }
    virtual std::uint64_t id() const noexcept = 0;
    virtual const char* versionName() const noexcept = 0;

    virtual ffxReturnCode_t createContext(ffxContext* context,
                                          ffxCreateContextDescHeader* desc,
                                          const ApusrFramegenAllocator& allocator) const = 0;
    virtual ffxReturnCode_t destroyContext(ffxContext* context,
                                           const ApusrFramegenAllocator& allocator) const = 0;
    virtual ffxReturnCode_t configure(ffxContext* context, const ffxConfigureDescHeader* desc) const = 0;
    virtual ffxReturnCode_t query(ffxContext* context, ffxQueryDescHeader* desc) const = 0;
    virtual ffxReturnCode_t dispatch(ffxContext* context, const ffxDispatchDescHeader* desc) const = 0;
};

struct ApusrFramegenContextHeader {
    const ApusrFramegenProvider* provider = nullptr;
};

template <typename T>
static inline T ConvertEnum(std::uint32_t value)
{
    return static_cast<T>(value);
}

template <typename T>
static inline std::uint32_t ReverseConvertEnum(T value)
{
    return static_cast<std::uint32_t>(value);
}
