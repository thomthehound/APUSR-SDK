<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Experimental AIE middle-core spatial graph

The selected experimental NPU front-carrier design is **row-major C16 frame + tensor-DMA halo patch gather**. Row-ring/seam-exchange carriers are excluded from the product `spatial-core` path because they turn halo access into a storage-layout dependency.

The public ingress and egress objects remain complete model-seam frame slots, but they no longer use the same physical layout. Ingress is row-major C16 with channel-inner pixels. Egress is cell-major C16 with `m4x8` order inside each 16×8 cell. Physical-column stripes are scheduling/placement metadata, not a separate public layout.

The graph still assigns contiguous vertical stripes to physical NPU columns, but that ownership is a compiler/runtime placement rule. It is no longer encoded by physically lane-major DDR ordering.

## Experimental spatial graph spine

`spatial-core` is the current experimental NPU graph. Its active front path is:

```text
row-major C16 frame slot
  -> shim/memory-tile tensor access pattern
  -> dense 18×10×C16 front halo patch
  -> AIE-vector/SRS C16 spatial 3×3
  -> AIE-vector/SRS C16→m4x8 C32 expansion
  -> m4x8 block-major C32/C64/C32 internal spine
  -> m4x8 block-major C16 cell drain
  -> GPU import shader reads m4x8 C16 directly
```

The front compute tile never sees a full shard row. It consumes one compact 2880-byte halo patch per 16×8 output cell. The tensor access pattern supplies left, right, top, bottom, and diagonal halo pixels from the source frame. True frame edges use a one-pixel zero-guarded C16 ingress frame written by the GPU bookend.

This removes the failed product ideas:

```text
no full-shard C16 row rings in compute-tile L1
no active left/right seam ObjectFifos
no joined seam-pair stream
no explicit top/bottom zero rows in AIE L1
no overlapping DDR patch scratch writes
no per-cell host-visible DMA tasks
```

The active primitive is `apusr_middle_core_front_c16_halo_patch_expand_srs_to_c32_cell_i8`. Its params remain tile-local resident model-epoch state, not hot-path streams.

After the front spatial ingest, C32 and C64 activation cells use an internal `m4x8` block-major layout:

```text
cell[y][x_group][c8_group][pixel_lane][c8]
```

One native 4-pixel × 8-channel result is therefore contiguous as a 32-byte vector block. The public egress frame also uses `m4x8` C16 cell order so the decoder can store the final C16 result with vector stores. Row-major C32/C64 is not an internal NPU handoff layout.

## Why this is the right front carrier

The logical output unit is still a 16×8 C16 cell, but the spatial front kernel needs the consumer view:

```text
owned cell:       16×8×C16  = 2048 bytes
1-pixel halo view:18×10×C16 = 2880 bytes
```

That is small enough for a hot compute-tile object, unlike a full shard row. The DDR read overhead is bounded and regular, and it uses the DMA/tensor machinery intended for tiled views rather than making the graph manually stitch seams.

The product rule is now direct: halo formation belongs in shim/memory-tile tensor addressing. AIE kernels should receive compact working sets and spend local memory on math, not row caches.

## Remaining work

The C32→C64, C64→C32, and C32→C16 spine rows now use the same resident-parameter vector/SRS primitive shape as the front row. Their current resident buffers are identity/debug parameter packs, but the hot-path contract is no longer scalar copy scaffolding. The next math work is to replace those identity packs with generated model weights while preserving the internal `m4x8` C32/C64 handoff layout.

The parameter expansion inside the AIE primitive remains a tracked cleanup item. Bias and scale are currently stored compactly and broadcast into `m4x8` vector blocks at runtime. That keeps parameter memory small, but it costs scalar-looking staging instructions. Revisit this after the real middle-row weights land; pre-expanded resident bias/scale blocks are likely the right product form if the audit shows the staging cost is visible.
