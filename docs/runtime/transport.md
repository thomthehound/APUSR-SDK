<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Frame-slot transport

The transport layer moves one complete FSR4 model-seam frame through the NPU with minimal host involvement.

The product C16 transport now has two explicit physical layouts:

- ingress: **row-major C16/NHWC-ish frame storage**, with a one-pixel zero guard for front spatial halos;
- egress: **cell-major `m4x8` C16 frame storage**, matching the NPU decoder's native 4-pixel × 8-channel vector stores.

The ingress layout is:

```text
row_stride_bytes = seam_width * 16
pixel_offset     = (y * seam_width + x) * 16
cell_x           = pixel_x / 16
cell_y           = pixel_y / 8
cell_origin      = ((cell_y * 8) * seam_width + (cell_x * 16)) * 16
```

Physical NPU columns still own contiguous vertical stripes, but that is a placement rule. It is not the ingress DDR ordering. The front graph uses tensor access patterns to gather each 18×10×C16 halo patch from the row-major frame.

The front ingress is a zero-guarded ingress frame over the row-major C16 frame:

```text
halo_view_width = seam_width + 2
halo_view_rows  = seam_rows + 2
```

This is the canonical hardware-backed front carrier. It is not a patch-major scratch frame and it does not duplicate one patch per output cell.

The egress layout is cell-major and `m4x8` inside each 16×8 C16 cell:

```text
cell_index      = cell_y * cell_columns + cell_x
block_index     = (local_y * 4 + x_group) * 2 + c8_group
byte_offset     = cell_index * 2048 + block_index * 32 + pixel_lane * 8 + c8
```

The GPU import shader consumes that layout directly, so the AIE decoder does not scalar-scatter its native vector result into row-major C16 just to cross the public seam.

The legacy 64×32 package layout is reference material only. The active `spatial-core` contract uses row-major C16 frame storage rather than the lane-major C16 cell slot, so frame halos remain tensor-DMA views instead of becoming a storage-layout problem.

The standalone `apusr_frame_conveyor_xrt_benchmark` remains the rough transport bandwidth check. The name is legacy ABI/tooling vocabulary; the benchmark measures the complete-frame-slot transport envelope. It is not a game path and should not grow into a second runtime mode.
