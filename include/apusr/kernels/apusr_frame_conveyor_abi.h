// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>

#ifndef APUSR_FRAME_CONVEYOR_FRAME_BYTES
#define APUSR_FRAME_CONVEYOR_FRAME_BYTES 0
#endif

#ifndef APUSR_FRAME_CONVEYOR_COLUMNS
#define APUSR_FRAME_CONVEYOR_COLUMNS 4
#endif

#ifndef APUSR_FRAME_CONVEYOR_CHANNELS
#define APUSR_FRAME_CONVEYOR_CHANNELS 2
#endif

namespace apusr::kernels::frame_conveyor {

inline constexpr std::uint32_t alignUpU32(const std::uint32_t value, const std::uint32_t alignment)
{
    return alignment == 0u ? value : ((value + alignment - 1u) / alignment) * alignment;
}

inline constexpr std::uint32_t kKernelOpcode = 17u;
inline constexpr std::int32_t kControlRingBufferArgIndex = -1;
inline constexpr std::int32_t kInputTokenBufferArgIndex = 3;
inline constexpr std::int32_t kOutputTokenBufferArgIndex = 4;
inline constexpr std::uint32_t kChunkBytes = 4096;
inline constexpr std::uint32_t kChunkWords = kChunkBytes / sizeof(std::uint32_t);
inline constexpr std::uint32_t kConfiguredFrameBytes = APUSR_FRAME_CONVEYOR_FRAME_BYTES;
inline constexpr std::uint32_t kConfiguredTransportBytes = alignUpU32(kConfiguredFrameBytes, kChunkBytes);
inline constexpr std::uint32_t kConfiguredTransportPaddingBytes = kConfiguredTransportBytes - kConfiguredFrameBytes;
inline constexpr std::uint32_t kConfiguredTransportWords = kConfiguredTransportBytes / sizeof(std::uint32_t);
inline constexpr std::uint32_t kConfiguredFrameChunks = kConfiguredTransportBytes / kChunkBytes;
inline constexpr std::uint32_t kColumnCount = APUSR_FRAME_CONVEYOR_COLUMNS;
inline constexpr std::uint32_t kDmaChannelsPerColumn = APUSR_FRAME_CONVEYOR_CHANNELS;
inline constexpr std::uint32_t kParallelDmaStreams = kColumnCount * kDmaChannelsPerColumn;
inline constexpr std::uint32_t kBaseChunksPerStream =
    kParallelDmaStreams == 0 ? 0 : kConfiguredFrameChunks / kParallelDmaStreams;
inline constexpr std::uint32_t kExtraChunkStreams =
    kParallelDmaStreams == 0 ? 0 : kConfiguredFrameChunks % kParallelDmaStreams;
inline constexpr std::uint32_t kMaxChunksPerStream = kBaseChunksPerStream + (kExtraChunkStreams == 0 ? 0 : 1);

} // namespace apusr::kernels::frame_conveyor
