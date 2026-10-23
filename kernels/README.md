<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Kernels

APUSR keeps the current middle-core rewrite and the frame-conveyor benchmark in-tree.
The failed full-arena two-column middle-core attempt is quarantined under
`kernels/quarantine/` and is not selectable by CMake.

Current spatial-core profiles (`Spatial4Col` is product-canonical; `Spatial2Col` is diagnostic):

```text
apusr_middle_core_stage1_12_spatial2col
apusr_middle_core_stage1_12_spatial4col
```

Both profiles keep the same public ABI:

```text
load_params(param_arena)
sequence(param_arena, ingress_frame, egress_frame)
```

The host binds complete capacity-class frame slots. Internal work must be
microtile/cell-shaped. Full macro-package object FIFOs are forbidden because they
consume too much compute-tile memory and push the graph back into a linear package
pipeline.

Build knobs:

```text
-DAPUSR_AIE_KERNEL_PROFILE=MiddleCoreStage1To12Spatial2Col|MiddleCoreStage1To12Spatial4Col
-DAPUSR_MIDDLE_CORE_FRAME_CAPACITY=1080|2160|4320
-DAPUSR_AIE_TARGET=npu1|npu2
-DAPUSR_MIDDLE_CORE_WORKER_GROUPS=auto|1|2|4
-DAPUSR_BUILD_AIE_KERNELS=ON
-DAPUSR_AIE_GRAPH_PHASE=spatial-core|stage1-resident|stage2-resident|stage2-plane-window-resident|stage1_2-linked-resident|stage1_2-worker-ring-resident|stage1_4-worker-ring-resident|stage1_2-to-stage3_4-worker-ring-resident|stage3-down-resident|stage3_4-fused-resident|stage3_4-product-resident|front-c16-row|cell-conveyor|c16-halo
```

`APUSR_MIDDLE_CORE_FRAME_CAPACITY` is a compiled FSR4 model-family capacity class.
It is not the active user resolution, render resolution, display resolution, or
quality preset.

`APUSR_MIDDLE_CORE_WORKER_GROUPS=auto` fills the selected target with the selected worker width. Profile names describe worker width, not total physical column count. On an 8-column XDNA2-class target that means four diagnostic 2-column worker groups or two product 4-column worker groups. On a 4-column target it means two diagnostic 2-column worker groups or one product 4-column worker.

The spatial profile generators can dump the shared resource, frame-cell, and graph
plans. `spatial-core` is the current product-shaped graph spine and is the default
AIE graph selection. `cell-conveyor` and `c16-halo` remain as diagnostics for build
isolation only.

The repo-owned AIE builder passes model resolution, worker groups, and graph selection
as typed build arguments rather than a raw forwarded argv list, so generator flags
that begin with `--` are not re-parsed by the builder.

The current `spatial-core` graph has permanent tile/stream shape. It reads a row-major C16 frame through shim/memory-tile tensor access patterns, feeds the front tile with compact 18×10×C16 halo patches, runs the AIE-vector/SRS C16 spatial-3×3-plus-C32-expansion primitive, and then feeds the permanent C32/C64/C32/C16 spine. The active front path has no full-shard row ring, no left/right seam ObjectFifos, no joined seam-pair stream, and no per-cell host-visible DMA tasks. True frame-edge padding comes from the GPU-written one-pixel zero guard in the front ingress slot. Interior halos come from the row-major frame tensor view, not zero padding.

The spatial-core manifests include the active front-C16 halo-patch contract and resource budgets for every supported capacity/device pair. These budgets model the front hot object as one 2880-byte C16 halo patch, not a row arena.
