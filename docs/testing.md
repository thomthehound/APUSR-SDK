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

The script configures a clean C++23 test build, builds the `apusr_quality_tests` target, and runs tests labeled `quality`.

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

Hardware/game runs are expensive and should answer specific questions. Reuse existing profiles as controls when the workload remains valid; do not repeat a baseline merely for ritual symmetry.

When a result depends on a driver/hardware behavior that cannot be reproduced in CI, keep a deterministic source/IR contract test in the repository and document the hardware qualification requirement separately.

## NPU/AIE research

AIE compilation and hardware runs are outside the normal product quality gate. Use the narrowest kernel compile that answers the current question, then audit generated ELF/instruction mix before escalating to a larger compile or hardware timing run.

The experimental frame-slot bandwidth benchmark remains a standalone tool and is not a product runtime mode.
