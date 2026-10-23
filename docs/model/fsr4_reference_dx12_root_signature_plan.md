<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 DX12 root-signature plan

This document records the root-signature contract for the APUSR-owned DX12 FSR4 INT8 reference backend.

The descriptor/register plan is scoped by dispatch, but the DX12 recorder still needs a concrete root-signature policy.  APUSR should not copy the FFX backend wholesale, but it should preserve the parts of the FFX root-signature behavior that matter for correctness.

## FFX-compatible policy

The FFX DX12 backend creates descriptor tables for UAV and SRV ranges, root CBV descriptors for constant buffers, and static samplers.  APUSR mirrors that policy instead of putting constant buffers into the shader-visible CBV/SRV/UAV heap.

The current APUSR plan is:

| Root parameter | Kind | Register range |
| ---: | --- | --- |
| 0 | UAV descriptor table | `u0..u12` |
| 1 | SRV descriptor table | `t0..t20` |
| 2 | root CBV | `b0` |
| 3 | root CBV | `b1` |

Static sampler:

| Register | Name | Filter | Address mode |
| --- | --- | --- | --- |
| `s0` | `g_history_sampler` | linear | clamp |

## Important b0 reuse rule

Several shaders use `b0`, but not for the same semantic role:

- FSR4 pre/post use `MLSR_Optimized_Constants` at `b0`.
- SPD uses `AutoExposureSPDConstants` at `b0`.
- RCAS uses `cbRCAS` at `b0`.

A single superset root signature must therefore have **one** `b0` root parameter.  The recorder will bind different constant-buffer GPU virtual addresses to that same root parameter before each dispatch.  Creating separate root parameters for FSR4/SPD/RCAS constants would duplicate the same `b0` register and is not a valid global root-signature shape.

`b1` is used only by the FSR4 pre pass for `cbPass_Weights`.

## Descriptor heap layout

The planned shader-visible CBV/SRV/UAV heap currently uses only SRV and UAV descriptors:

| Segment | Base offset | Count |
| --- | ---: | ---: |
| SRV | 0 | 21 |
| UAV | 21 | 13 |

Total shader-visible CBV/SRV/UAV descriptors: 34.

Constant buffers are root descriptors, not heap descriptors.  The static sampler is baked into the root signature.

## Recorder implications

For each dispatch, the recorder must:

1. Bind the common root signature.
2. Bind the shader-visible descriptor heap.
3. Set the UAV table root descriptor to the dispatch-specific UAV table segment.
4. Set the SRV table root descriptor to the dispatch-specific SRV table segment.
5. Set `b0` to the dispatch-specific primary constant buffer:
   - FSR4 optimized constants for pre/middle/post/debug where needed.
   - SPD constants for SPD.
   - RCAS constants for RCAS.
6. Set `b1` to pass-weights constants before the pre pass.
7. Dispatch the selected PSO.

The DX12 runtime now creates this heap once when a native device is available and patches active SRV/UAV slots from the scoped descriptor patch plan.  CBVs are uploaded into a small multi-frame upload ring and bound as root descriptors.  The current per-frame constant region is 1792 bytes: 256 bytes each for FSR4, SPD, and RCAS constants, followed by the 1024-byte pass-weights constant block generated from the preset-specific pre-pass table.

Descriptor register numbers are not globally unique across the FSR4 dispatches.  For example, `u11` is the pre-pass scratch buffer in one shader and the RCAS output in another.  The recorder therefore allocates a full SRV/UAV table segment per dispatch and binds the dispatch-local segment before each dispatch, matching the FFX backend's per-job descriptor-table behavior.  Reusing one frame-global descriptor table is invalid.

The recorder patches only the scoped resources needed by the current frame plan.  Descriptor validity, resource-state transitions, and restore behavior remain dispatch-scope aware.  The FFX DX12 replacement path restores borrowed external resources to their host-supplied initial states by default, matching the FidelityFX provider unregister-resource contract.  This restore behavior is now fixed; it is no longer a product/build mode.
