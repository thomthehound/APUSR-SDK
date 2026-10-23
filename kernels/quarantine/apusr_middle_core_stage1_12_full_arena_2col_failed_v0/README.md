<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR middle-core stage1→stage12 full resident arena graph

This profile is the product middle-core graph. The public runtime boundary is the
frame conveyor: the host binds one ingress frame slot and one egress frame slot,
and the AIE graph owns the internal macro-lane work.

The graph uses the full 122,880-byte resident parameter arena and splits
parameter movement from hot execution:

```text
load_params(param_arena): model-epoch parameter load, 122,048 bytes
sequence(param_arena, ingress_frame, egress_frame): hot frame execution, zero parameter DMA
```

The ingress and egress frame slots are packed arrays of 64×32 C16 macro-lane
packages. Their size comes from the selected FSR4 model-family capacity
(1080, 2160, or 4320 class), not from the user's active resolution, quality
preset, or any local test setup. The checked-in compiled profile currently
defaults to the 2160 capacity class; distributable builds must package and
select the capacity-matched graph. The hot sequence streams every compiled lane stripe inside one XRT run. Each two-column macro-lane group gets one contiguous ingress DMA task and strided egress DMA drains for the stage12 output streams; macro-packages are consumed by object FIFOs and cores inside that frame-level schedule. For each internal package, the AIE graph expands the C16 stage0 package into the sixteen 18×10×C16 stage1 halos before the stage1 workers run. Stage1 halos, SkipA,
SkipB, stage5 input, stage8 input, and stage10 output are graph-internal
object-FIFO traffic.

Macro-lane packages are implementation detail. They are not a host-visible ABI.
The GPU bookends write/read this lane-major package-C16 frame-slot layout directly. There is no raster-to-package repack stage in the product path.
