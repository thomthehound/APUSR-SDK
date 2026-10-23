// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <cstdint>

#ifndef APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES
#define APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES 19906560
#endif

#ifndef APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS
#define APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS 4
#endif

#ifndef APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS
#define APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS 2
#endif

namespace apusr::kernels::frame_conveyor_benchmark {

inline constexpr std::uint32_t alignUpU32(const std::uint32_t value, const std::uint32_t alignment)
{
    return alignment == 0u ? value : ((value + alignment - 1u) / alignment) * alignment;
}

inline constexpr std::uint32_t kKernelOpcode = 3u;
inline constexpr std::int32_t kOpcodeArgIndex = 0;
inline constexpr std::int32_t kInstructionBufferArgIndex = 1;
inline constexpr std::int32_t kInstructionCountArgIndex = 2;
inline constexpr std::int32_t kInputFrameBufferArgIndex = 3;
inline constexpr std::int32_t kOutputFrameBufferArgIndex = 4;
inline constexpr std::uint32_t kChunkBytes = 4096u;
inline constexpr std::uint32_t kChunkWords = kChunkBytes / sizeof(std::uint32_t);
inline constexpr std::uint32_t kConfiguredFrameBytes = APUSR_FRAME_CONVEYOR_BENCHMARK_FRAME_BYTES;
inline constexpr std::uint32_t kConfiguredTransportBytes = alignUpU32(kConfiguredFrameBytes, kChunkBytes);
inline constexpr std::uint32_t kConfiguredTransportPaddingBytes = kConfiguredTransportBytes - kConfiguredFrameBytes;
inline constexpr std::uint32_t kConfiguredTransportWords = kConfiguredTransportBytes / sizeof(std::uint32_t);
inline constexpr std::uint32_t kConfiguredFrameChunks = kConfiguredTransportBytes / kChunkBytes;
inline constexpr std::uint32_t kColumnCount = APUSR_FRAME_CONVEYOR_BENCHMARK_COLUMNS;
inline constexpr std::uint32_t kDmaChannelsPerColumn = APUSR_FRAME_CONVEYOR_BENCHMARK_CHANNELS;
inline constexpr std::uint32_t kParallelDmaStreams = kColumnCount * kDmaChannelsPerColumn;
inline constexpr std::uint32_t kBaseChunksPerStream =
    kParallelDmaStreams == 0u ? 0u : kConfiguredFrameChunks / kParallelDmaStreams;
inline constexpr std::uint32_t kExtraChunkStreams =
    kParallelDmaStreams == 0u ? 0u : kConfiguredFrameChunks % kParallelDmaStreams;
inline constexpr std::uint32_t kMaxChunksPerStream = kBaseChunksPerStream + (kExtraChunkStreams == 0u ? 0u : 1u);

} // namespace apusr::kernels::frame_conveyor_benchmark
