<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Testing policy

Tests should protect real product behavior and non-obvious engineering contracts.

## Quality gate

Run:

```powershell
python .\tools\quality_check.py
```

or:

```bash
python3 tools/quality_check.py
```

The script configures a clean C++23 test build, builds the `apusr_quality_tests` target, and runs tests labeled `quality`. It reports the entire test set rather than stopping after the first failed contract.

## Continuous integration

GitHub Actions runs the quality suite on `windows-latest` and `ubuntu-26.04`. The Windows job initializes the x64 MSVC environment; Linux uses its native compiler. Shader-product freshness checks account for checkout line endings while still detecting source changes.

Tests that require a native D3D12 GPU report **Skipped** if the runner exposes only the Microsoft software adapter or no suitable device. A skip is not evidence of GPU execution or numerical output parity. The other D3D12 compile, descriptor, runtime-plan, and shader checks remain mandatory. The Windows job also repeats the Vulkan frame-generation scheduling contract to check asynchronous timing behavior.

Passing CI establishes build and contract integrity. GPU performance, fidelity, real device features, and game integration require hardware-qualified testing.

## What belongs in the suite

Keep tests that would catch a real regression in:

- API-neutral graph/resource contracts;
- frontend ABI translation;
- D3D12/Vulkan backend behavior;
- shader product generation/selection;
- packed-dot and storage-lowering invariants;
- frame-generation scheduling/resource lifetime;
- host-capture/Streamline compatibility contracts;
- profiler behavior and fail-open/disabled overhead rules.

Avoid tests that merely prove a file exists, duplicate a compiler error, or preserve a retired implementation mode with no product relevance.

## Reference versus candidate paths

Performance experiments must not silently redefine the correctness baseline. When a backend-specific optimization changes source/lowering, retain a reference or contract test capable of proving equivalent model/resource behavior.

A candidate is promoted because correctness **and** measured performance justify it, not because its source looks simpler or its binary is smaller.

## Hardware testing

Hardware and game testing should answer specific questions. Reuse a valid baseline for the same workload and configuration, and repeat measurements when the code, GPU, driver, or workload has materially changed.

When a result depends on a driver/hardware behavior that cannot be reproduced in CI, keep a deterministic source/IR contract test in the repository and document the hardware qualification requirement separately.

## NPU/AIE research

AIE compilation and hardware runs are outside the normal product quality gate. Use the narrowest kernel compile that answers the current question, then audit generated ELF/instruction mix before escalating to a larger compile or hardware timing run.

The experimental frame-slot bandwidth benchmark remains a standalone tool and is not a product runtime mode.
