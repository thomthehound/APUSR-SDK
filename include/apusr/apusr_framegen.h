// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <apusr/apusr_graphics.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace apusr::framegen {

enum class PresentFrameType : std::uint32_t {
    Generated,
    Real,
    Count,
};

inline constexpr std::size_t kPresentFrameTypeCount = static_cast<std::size_t>(PresentFrameType::Count);

[[nodiscard]] constexpr std::size_t presentFrameIndex(PresentFrameType type) noexcept
{
    return static_cast<std::size_t>(type);
}

struct ScheduledFrame {
    bool enabled = false;
    NativeHandle resource;
    std::uint64_t interpolationCompletedFenceValue = 0;
    std::uint64_t presentIndex = 0;
    std::uint64_t presentTickDelta = 0;
};

// API-neutral scheduling state. Backends own native queue/fence execution and
// presentation callbacks; this object only describes which frames are due and
// the ordering/timing contract between them.
struct PresentSchedule {
    bool vsync = false;
    bool tearingSupported = false;
    bool usePremulAlphaComposite = false;
    bool drawDebugPacingLines = false;

    std::uint64_t interpolationCompletedFenceValue = 0;
    std::uint64_t replacementBufferFenceSignal = 0;
    std::uint64_t numFramesSentForPresentationBase = 0;
    std::uint32_t numFramesToPresent = 0;
    std::uint64_t frameId = 0;

    std::array<ScheduledFrame, kPresentFrameTypeCount> frames{};

    [[nodiscard]] ScheduledFrame& frame(PresentFrameType type) noexcept
    {
        return frames[presentFrameIndex(type)];
    }

    [[nodiscard]] const ScheduledFrame& frame(PresentFrameType type) const noexcept
    {
        return frames[presentFrameIndex(type)];
    }

    void clear() noexcept
    {
        *this = {};
    }
};

template <std::size_t Size, typename Type = double>
class MovingAverage final {
public:
    [[nodiscard]] Type average() const noexcept
    {
        if (updateCount_ < Size)
            return Type{};

        Type value{};
        for (const Type sample : history_)
            value += sample;
        return value / static_cast<Type>(Size);
    }

    [[nodiscard]] Type deviation() const noexcept
    {
        if (updateCount_ < Size)
            return Type{};

        const Type mean = average();
        Type variance{};
        for (const Type sample : history_) {
            const Type difference = sample - mean;
            variance += difference * difference;
        }
        return std::sqrt(variance / static_cast<Type>(Size));
    }

    void reset() noexcept
    {
        updateCount_ = 0;
        index_ = 0;
    }

    void update(Type value) noexcept
    {
        history_[index_] = value;
        index_ = (index_ + 1) % Size;
        ++updateCount_;
    }

private:
    std::array<Type, Size> history_{};
    std::size_t index_ = 0;
    std::uint64_t updateCount_ = 0;
};

// Computes the half-frame presentation target in arbitrary monotonic clock
// ticks. Keeping this math outside DXGI/QPC lets Vulkan reuse the same pacing
// policy with a different clock and native synchronization adapter.
class PacingEstimator final {
public:
    void reset() noexcept
    {
        frameTime_.reset();
    }

    void update(double frameDeltaTicks) noexcept
    {
        frameTime_.update(frameDeltaTicks);
    }

    [[nodiscard]] std::uint64_t targetDelta(
        std::int64_t clockFrequency,
        double safetyMarginSeconds,
        double varianceFactor) const noexcept
    {
        if (clockFrequency <= 0)
            return 0;

        const auto safetyMargin = static_cast<std::int64_t>(clockFrequency * safetyMarginSeconds);
        const auto conservativeAverage = static_cast<std::int64_t>(
            frameTime_.average() * 0.5 - frameTime_.deviation() * varianceFactor);
        return conservativeAverage > safetyMargin
            ? static_cast<std::uint64_t>(conservativeAverage - safetyMargin)
            : 0;
    }

private:
    MovingAverage<10, double> frameTime_{};
};

[[nodiscard]] inline bool shouldBlockForFence(
    std::int64_t previousPresentTicks,
    std::int64_t previousDeltaTicks,
    std::int64_t nowTicks,
    std::int64_t clockFrequency,
    bool blockingEnabled,
    std::int64_t thresholdMicroseconds = 2000) noexcept
{
    if (!blockingEnabled || previousPresentTicks == 0 || clockFrequency <= 0)
        return false;

    const std::int64_t targetDelta = (previousPresentTicks + previousDeltaTicks) - nowTicks;
    return (targetDelta * 1000000) / clockFrequency > thresholdMicroseconds;
}

} // namespace apusr::framegen
