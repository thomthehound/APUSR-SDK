<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR middle-core 4-column spatial product rewrite

This is the canonical product placement after the Stage1→Stage2 producer-ring decision. The four-column macro-lane gives the graph separate memory-tile roles for ingress/front C16 producer rings, SkipA, SkipB/transition staging, and egress. It is intentionally not a patched copy of the quarantined graph.

The public ABI stays frame-conveyor:

```text
load_params(param_arena)
sequence(param_arena, ingress_frame, egress_frame)
```

The hot sequence binds complete capacity-class frame slots. The public front
ingress slot is row-major C16 with a one-pixel zero guard border. The graph uses
microtile/cell work units and tensor-DMA halo views instead of full macro-package
object FIFOs.

Design rules for this profile:

- macro-lane width: 4 AIE columns;
- normal hot FIFO element limit: 16 KB;
- preferred hot FIFO element size: 4 KB or smaller;
- FIFO elements larger than 8 KB must use depth 1;
- compute-tile reserved data memory budget: 48 KB before compiler proof;
- at most two serious linked kernels per compute tile before compile proof;
- no host-visible macro-token dispatch;
- no hot-path parameter DMA.

The default generator emits the active `spatial-core` spine. Diagnostic phases
remain available for placement/debug work only. `front-c16-row` isolates the
front vector/SRS row worker and snapshots back to C16. `cell-conveyor` validates
lane-major C16 cell transport through per-column shard taps, memory-tile
forwarding, and placed compute workers. `c16-halo` is a minimal halo diagnostic.

## First implementation contract

This profile imports the shared spatial plan in
`tools/analysis/middle_core_spatial_plan.py`. The owned cell is 16×8 at the
active channel width. The active front row reads a zero-guarded row-major C16
ingress frame through tensor-DMA halo taps and feeds compact 18×10×C16 patches
to the vector/SRS true-3×3 front primitive. The 4-column profile changes
placement pressure, not the logical algorithm.

Manual plan inspection:

```text
python src/apusr_middle_core_stage1_12_spatial4col.py --model-resolution 2160 --worker-groups auto --dump-plan summary
```

Frame/cell worker stripes can be inspected with:

```powershell
python src/apusr_middle_core_stage1_12_spatial4col.py --model-resolution 2160 --worker-groups auto --dump-plan frame
```


The default graph phase can be inspected with:

```powershell
python src/apusr_middle_core_stage1_12_spatial4col.py --model-resolution 2160 --worker-groups auto --dump-plan graph
```

Emit diagnostic MLIR-AIE phases with:

```powershell
python src/apusr_middle_core_stage1_12_spatial4col.py --model-resolution 2160 --worker-groups auto --emit-graph-phase front-c16-row
python src/apusr_middle_core_stage1_12_spatial4col.py --model-resolution 2160 --worker-groups auto --emit-graph-phase cell-conveyor
python src/apusr_middle_core_stage1_12_spatial4col.py --model-resolution 2160 --worker-groups auto --emit-graph-phase c16-halo
```
