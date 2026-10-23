// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#include "apusr/kernels/apusr_token_planner.h"

namespace apusr::kernels::planner {

static_assert(pixelsPerStreamToken(streamTokenForChannels(16u)) == 16u);
static_assert(pixelsPerStreamToken(streamTokenForChannels(32u)) == 8u);
static_assert(pixelsPerStreamToken(streamTokenForChannels(64u)) == 4u);
static_assert(evaluateMicroTile(MicroTileCandidate{16u, 8u, 16u, 1u, OperatorShape{3u, 3u, 1u, 1u}}, streamTokenForChannels(16u)).liveActivationBytes == 4928u);

} // namespace apusr::kernels::planner
