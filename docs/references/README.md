<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# References

APUSR keeps repository-local notes that point to external architecture and toolchain references, but does not need to redistribute vendor manuals merely to document design assumptions.

- `aie_ml_programming_references.md` indexes AMD AIE-ML architecture/programming material used by the experimental NPU lane.
- `iron_air_lessons.md` records durable design lessons extracted from IRON and MLIR-AIR reference projects. APUSR does not depend on those projects at runtime.

When an external reference teaches something that affects APUSR, prefer to record the resulting APUSR-native invariant, manifest rule, analyzer check, generator pattern, or kernel contract rather than copying the source material into this directory.
