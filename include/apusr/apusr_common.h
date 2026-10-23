// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>
#include <string>

namespace apusr {

enum class StatusCode : std::uint32_t {
    Ok = 0,
    InvalidArgument,
    UnsupportedBackend,
    MissingBackend,
    RuntimeFailure,
    NotImplemented,
};

struct Status {
    StatusCode code = StatusCode::Ok;
    std::string message;

    [[nodiscard]] static Status ok();
    [[nodiscard]] static Status invalidArgument(std::string message);
    [[nodiscard]] static Status unsupportedBackend(std::string message);
    [[nodiscard]] static Status missingBackend(std::string message);
    [[nodiscard]] static Status runtimeFailure(std::string message);
    [[nodiscard]] static Status notImplemented(std::string message);
    [[nodiscard]] bool succeeded() const;
};

[[nodiscard]] const char* statusCodeName(StatusCode code);

struct RenderSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct DisplaySize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Jitter {
    float x = 0.0f;
    float y = 0.0f;
};

struct CameraState {
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    float verticalFovRadians = 0.0f;
};

enum class VendorPolicy : std::uint32_t {
    PerformanceFirst = 0,
    PortableWhenFree,
};

} // namespace apusr
