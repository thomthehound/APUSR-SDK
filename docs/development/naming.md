<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# NPU and model-layout terminology

These terms describe experimental AIE/NPU layout contracts and existing ABI names. They do not describe the production GPU upscaling path.

## Experimental NPU terminology

- **Frame slot**: one complete C16 ingress or egress payload visible outside the NPU. The ingress slot is zero-guarded row-major C16. The egress slot is cell-major `m4x8` C16.
- **Spatial core**: the experimental NPU middle core built from 16×8 cells, tensor-DMA halo patch ingress, memory-tile staging, and consumer-side halos.
- **Cell** or **microtile**: the internal 16×8 work unit used by the spatial core. A C16 cell is 2,048 bytes.
- **m4x8 block-major cell layout**: the activation order `cell[y][x_group][c8_group][pixel_lane][c8]`. This is the NPU handoff contract for native 4-pixel × 8-channel vector blocks. C32/C64 use it internally; C16 uses it at egress so the GPU bookend consumes the NPU-native output layout directly.
- **Resident parameter arena**: model parameters loaded once per model epoch and bound separately from per-frame ingress/egress payloads.

## Names that should stay narrow

- **Frame conveyor**: legacy ABI/tooling name for the complete-frame-slot runtime and the standalone transport benchmark. Keep it in existing symbols, CMake options, manifests, and benchmark names where renaming would churn the ABI. New prose should prefer **frame-slot path** unless it is naming that exact ABI/tool.
- **Token band**: old scheduler vocabulary for a row-band payload descriptor. The experimental NPU frame-slot API must not expose host-visible token-band scheduling. Existing `TokenBandDesc` and doorbell fields are current lowering structures until the frame-slot API is renamed; do not describe them as the product unit.
- **Macro package**: the quarantined 64×32 C16 reference layout. It is not part of the product contract. The NPU ingress layout uses row-major C16 frame storage with tensor-DMA halo views; its egress layout is cell-major `m4x8` C16.
- **FSR4 logical stage numbers**: use these only when identifying model metadata or resident-parameter offsets. They must not define the spatial-core work unit, placement plan, halo policy names, or public runtime ABI.

## Resource-sharing wording

AIE-ML neighbor sharing is a data-movement advantage, not a license to double count memory.

Memory-tile plans may form an aggregate arena group across adjacent physical memory tiles, but every physical memory tile still keeps its own 512 KiB capacity limit. Compute-tile neighbor-memory visibility can remove copies and supply halos, but each compute tile still keeps its own 64 KiB data-memory and 16 KiB program-memory limits.
