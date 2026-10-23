// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 thomthehound.

#pragma once

#include <string>

namespace apusr {

void traceDiagnosticLine(const std::string& line);

#if defined(APUSR_ENABLE_DX12_TRACE) && APUSR_ENABLE_DX12_TRACE
#define APUSR_TRACE_DIAGNOSTIC(line) ::apusr::traceDiagnosticLine((line))
#else
#define APUSR_TRACE_DIAGNOSTIC(line) ((void)0)
#endif

} // namespace apusr
