# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Keep profiler instrumentation observational rather than render-blocking."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DX12 = (ROOT / "src/backends/gfx_dx12/apusr_fsr4_reference_dx12.cpp").read_text(encoding="utf-8")


def main() -> int:
    assert '"gpu_timestamp_profiler.disabled"' in DX12
    assert "gpuTimestampProfilerDisabledAfterFailure_ = true" in DX12
    assert "gpuTimestampProfilerDisabledAfterFailure_ = false" in DX12
    start = DX12.find("Status recordNativeDispatches(")
    end = DX12.find("Dx12Fsr4ReferenceCreateDesc createDesc_", start)
    assert start >= 0 and end > start
    body = DX12[start:end]
    assert "ensureGpuTimestampProfilerCapacity(plan)" in body
    assert "return gpuTimestampStatus;" not in body
    assert "resetGpuTimestampProfiler();" in body
    print("APUSR profiler fail-open contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
