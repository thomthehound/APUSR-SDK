<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# IRON and MLIR-AIR lessons for APUSR

`IRON` and `mlir-air` are reference projects only. APUSR does not depend on either
project at build time or runtime, and APUSR should not be reshaped to look like
those projects. Keep using the repo-owned `aie.iron` graph generators, Peano
kernel objects, native C++/XRT runtime, and embedded product artifacts.

The value of these projects is experience: they show mature ways to organize NPU
artifacts, tensor DMA views, channel/fifo dataflow, memory hierarchy, and reduced
hardware tests. Mine those ideas when they clarify APUSR's own product path.

## Useful IRON lessons

IRON is closest to APUSR's current authoring layer because it builds directly on
MLIR-AIE Python bindings and `aie.iron`. Its operator library is mostly
LLM/bfloat16-shaped, so the kernels are usually not reusable for APUSR. The
build and graph-organization patterns are useful.

### Artifact graph discipline

Relevant source files:

```text
IRON/iron/common/base.py
IRON/iron/common/compilation/base.py
IRON/iron/common/operator_bases.py
```

IRON models generated MLIR, kernel objects, archives, xclbins, and instruction
streams as explicit artifacts with dependencies. APUSR should keep moving in this
direction instead of treating generated MLIR, `.o`, `.xclbin`, and `insts.bin`
outputs as incidental side effects.

APUSR-specific rule:

```text
manifest/contract -> generated MLIR + AIE objects -> xclbin + insts.bin -> embedded product artifact
```

Every capacity/device/profile artifact should be reproducible from a manifest and
should have a predictable name. The runtime should select artifacts from product
metadata, not from ad-hoc paths or user-provided knobs.

### Python is a build-time tool, not the product runtime

Relevant source files:

```text
IRON/README.md
IRON/iron/common/base.py
```

IRON's normal operator path exposes Python wrappers around generated NPU kernels.
That is appropriate for an operator library and tests. APUSR's product path is a
Windows game DLL. Python belongs in generators, analyzers, packers, and tests.
The shipping runtime stays native C++/XRT with embedded or packaged artifacts.

### Compile-time specialization is acceptable

Relevant source files:

```text
IRON/iron/common/base.py
IRON/iron/operators/gemm/design.py
```

IRON bakes operator parameters such as device, shape, column count, tile size, and
precision into generated artifacts. That supports APUSR's capacity-class approach:
compile model-capacity/device/profile artifacts ahead of time, then keep per-frame
runtime state small.

APUSR should keep these axes explicit:

```text
model family / quality preset / capacity class / NPU target / spatial profile / lane fill
```

Do not bake active render resolution, display resolution, or frame-varying state
into the compiled graph.

### Resource checks should happen before placement

Relevant source files:

```text
IRON/iron/common/operator_bases.py
IRON/iron/common/utils.py
```

IRON checks device-level limits such as shim-DMA budget before trying to compile.
APUSR should keep its resource-budget scripts in front of MLIR-AIE placement. A
compile failure is late feedback; the repo should reject obviously impossible
hot-object sizes, DMA pressure, memory-tile pressure, and lane fills before
`aiecc` runs.

### Kernel-object and archive linkage is worth keeping explicit

Relevant source files:

```text
IRON/iron/common/compilation/base.py
IRON/iron/common/operator_bases.py
```

IRON treats AIE C/C++ kernels as first-class artifacts and can bundle support
objects into an archive when needed. APUSR already declares `aie_objects` in the
spatial manifests. Keep that explicit. Do not rely on incidental source discovery
or generated MLIR strings alone to decide what gets linked.

### Fused runtime sequences are a reference, not the hot path

Relevant source files:

```text
IRON/iron/common/fusion.py
IRON/iron/common/compilation/fusion.py
```

IRON's fusion code is useful as a reference for composing multiple generated
operators and reusing child artifacts. APUSR should not import that machinery.
The useful lesson is narrower: one product artifact may contain multiple logical
phases, but the public runtime contract should stay simple.

For APUSR that means:

```text
load_params(param_arena)
sequence(param_arena, ingress_frame, egress_frame)
```

A future model epoch may compile or select separate parameter-load and hot-frame
instruction streams. It should not expose per-cell, per-row, or per-stage host
dispatch.

## Useful MLIR-AIR lessons

MLIR-AIR is less directly useful as a dependency because it brings a separate AIR
dialect, AIR runtime, `aircc`, and lowering stack. It is very useful as a source
of dataflow vocabulary and reduced DMA/channel examples.

### Use the L3/L2/L1 model as documentation language

Relevant source file:

```text
mlir-air/docs/AIRComputeModel.md
```

AIR's memory hierarchy maps cleanly onto APUSR's mental model:

```text
L3 = host-visible/external frame and parameter buffers
L2 = memory-tile or segment-level on-device storage
L1 = compute-tile local scratch and short hot FIFOs
```

This is documentation language, not a required IR layer. It helps keep the design
clear: frame slots and resident parameter arenas live at L3; reusable stage
buffers and larger activation arenas belong in L2; compact patches, `m4x8` cells,
accumulators, and short handoffs belong in L1.

### Treat DMA, channels, and dependencies as explicit program structure

Relevant source files:

```text
mlir-air/docs/AIRComputeModel.md
mlir-air/docs/AIRAsyncConcurrency.md
```

AIR separates bulk movement, channels, and token ordering. APUSR should follow the
same discipline in its own manifests and analyzers:

- DMA describes tensor movement and layout views.
- Object FIFOs/channels describe producer-consumer handoff.
- Dependencies describe correctness ordering.
- Concurrency is a resource promise and must be budgeted.

Do not hide these under vague "stage runs after stage" language. If a path needs
overlap, the graph and budget should say what resources are simultaneously live.

### Tensor DMA views are the right shape for halos

Relevant source files:

```text
mlir-air/programming_examples/shim_dma_2d/README.md
mlir-air/test/airhost/30_2d_mb_shim_dma/README.md
mlir-air/test/airhost/31_3d_mb_shim_dma/README.md
mlir-air/test/airhost/32_4d_mb_shim_dma/README.md
mlir-air/test/airhost/36_4d_mb_multi_shim_dma_all_channels/README.md
mlir-air/test/airhost/37_4d_mb_multi_shim_dma_broadcast/README.md
mlir-air/test/airhost/38_4d_mb_shim_dma_channel_stress/README.md
```

The AIR shim-DMA examples reinforce APUSR's current front-carrier choice: keep a
single row-major ingress frame and use tensor-addressed DMA to gather compact
working sets. Do not materialize a patch-major DDR scratch frame and do not move
full shard rows through compute-tile FIFOs.

APUSR's front path remains:

```text
row-major guarded C16 ingress frame
  -> tensor-DMA 18x10xC16 halo patch view
  -> vector/SRS front primitive
  -> m4x8 C32 cell output
```

### DMA zero padding is useful, but not the front-edge contract

Relevant source file:

```text
mlir-air/test/xrt/52_dma_pad_passthrough/run.py
```

AIR has a useful reduced test for DMA padding. That supports keeping memory-tile
or DMA zero-padding in APUSR's toolbox for channel/granularity padding.

The current front-stage frame-edge policy remains different: true frame edges use
a GPU-written one-pixel zero guard. That keeps the front halo view regular and
avoids making correctness depend on a more complex DMA padding path before the
rest of the middle core is stable.

### The bottleneck example validates the graph shape, not the kernel code

Relevant source files:

```text
mlir-air/programming_examples/bottleneck/bottleneck.py
mlir-air/programming_examples/bottleneck/conv2dk1.cc
mlir-air/programming_examples/bottleneck/conv2dk3.cc
mlir-air/programming_examples/bottleneck/conv2dk1_skip.cc
```

The AIR bottleneck example is useful because it combines int8 activations,
weights, 1x1/3x3 convolution stages, broadcast, shared local buffers, and a skip
connection. That is conceptually closer to APUSR than IRON's LLM operators.

Do not copy its kernels as product kernels. The useful lesson is that a spatial
NPU graph can keep a residual path and staged weights resident while moving small
row/cell work units through the fabric. APUSR's product kernels should still be
written around the current vector/SRS `m4x8` contract.

### AIR lowering is a fallback study path, not the main compiler path

Relevant source file:

```text
mlir-air/docs/aircc.md
```

`aircc` lowers AIR through its own control/runtime path and generates AIE code and
host libraries. That is too much machinery for APUSR's product path. Use AIR
examples to make reduced tests and to understand DMA/channel behavior. Do not add
AIR as a required layer unless the direct `aie.iron` path hits a hard technical
wall.

## Near-term mining targets

1. Compare APUSR's generated spatial MLIR against IRON's artifact naming and
   dependency model. Make generated artifacts easier to audit and reproduce.
2. Add resource-budget checks that explicitly model shim-DMA/channel pressure in
   the same spirit as IRON's preflight checks.
3. Use AIR's 2D/3D/4D shim-DMA tests as reference cases when validating APUSR's
   halo-gather metadata.
4. Keep the AIR DMA-padding test as the reference for future channel padding, but
   leave the front edge on the guarded-frame contract.
5. Revisit the AIR bottleneck example when replacing identity/debug middle-row
   parameter packs with real C32/C64/C32/C16 weights and skip/residual behavior.

## Dependency boundary

These projects may inform docs, analyzers, reduced tests, and generator design.
They must not become required runtime dependencies for the APUSR DLL. Any idea
borrowed from them should be reduced to an APUSR-native manifest, analyzer rule,
generator pattern, or kernel contract.
