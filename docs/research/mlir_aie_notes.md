<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# MLIR-AIE notes

Windows AIE builds use MLIR-AIE with Peano/LLVM-AIE. The active MLIR-AIE/IRON
environment should provide the relevant tools; `APUSR_PEANO_INSTALL_DIR` is
optional and must not be guessed.

Build rules:

- Use the repo-owned `tools/build_aie_kernel.py` wrapper.
- Compile distinct hot and parameter-load instruction streams when the manifest declares both.
- Reject Chess/xbridge tool paths on Windows.
- Pass `--model-resolution` from `APUSR_MIDDLE_CORE_FRAME_CAPACITY`.
- Pass `--worker-groups` from `APUSR_EFFECTIVE_MIDDLE_CORE_WORKER_GROUPS`.
- Run the middle-core resource-budget check before MLIR-AIE placement.
- Treat MLIR-AIE placement, program-memory pressure, local-memory pressure, FIFO placement, and DMA/channel pressure as the real kernel gate.

The active spatial-core profiles are `MiddleCoreStage1To12Spatial2Col` and
`MiddleCoreStage1To12Spatial4Col`. The full-arena two-column graph is
quarantined and should not be revived by patching compile errors.

The current product-shaped graph selection is:

```text
spatial-core
```

`spatial-core` routes the zero-guarded row-major C16 ingress frame through the
permanent spatial spine: C16 front → C32 encoder → C64 bottleneck → C32 return →
C16 decoder/egress. The front row uses tensor-DMA halo patches and real
vector/SRS math. C32/C64 internal handoffs are `m4x8` block-major, and the final
C16 egress slot is also cell-major `m4x8` so the GPU bookend consumes the
NPU-native output layout directly.

Diagnostic graphs remain available for build isolation:

```text
front-c16-row
cell-conveyor
c16-halo
```

They are not the product compile gate. New product work should modify the
`spatial-core` graph directly.

Build the active graph through CMake with `APUSR_BUILD_AIE_KERNELS=ON`. The CMake
default for `APUSR_AIE_GRAPH_PHASE` is `spatial-core`; set it to a diagnostic
phase only when isolating a build/toolchain problem.

### AIE object linkage

The graph uses the repo-owned Peano objects declared in each spatial middle-core
manifest under `aie_objects`:

```text
apusr_middle_core_c16_cell_copy.o
apusr_middle_core_vector_primitives.o
```

The build wrapper compiles those objects into the AIE output directory before
invoking `aiecc`; the `aie.iron` `Kernel(..., link_with=...)` path expects the object
basenames to be present beside the generated MLIR. Do not rely on `sources` alone
for AIE object linkage.

## External project references

`../references/iron_air_lessons.md` records the useful source-level lessons
from IRON and MLIR-AIR. Treat those projects as references, not dependencies.
IRON is useful for artifact-graph discipline, generated-MLIR naming,
kernel-object/archive linkage, and preflight resource checks. MLIR-AIR is useful
for L3/L2/L1 vocabulary, explicit DMA/channel/dependency modeling, and reduced
shim-DMA tests.

The active APUSR compiler path remains repo-owned `aie.iron` graph generation
through `tools/build_aie_kernel.py`, Peano kernel-object builds, MLIR-AIE
placement, and native C++/XRT runtime packaging. Do not add AIRRt, `aircc`, IRON
Python operator wrappers, or Python runtime dispatch to the product DLL.
