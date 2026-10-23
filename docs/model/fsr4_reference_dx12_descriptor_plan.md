<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 DX12 descriptor/register plan

This document records the APUSR-owned descriptor contract for the imported FFX FSR4 INT8 reference shaders.

The important preflight lesson is that the FFX provider resource-binding tables are **name maps**, not register-allocation maps.  The generated shaders use fixed HLSL register slots, and some names intentionally reuse the same slot in different dispatch scopes.  APUSR therefore tracks both pieces of information:

- FFX-compatible resource identifier, for cross-checking against the provider model.
- Shader register slot and dispatch scope, for actual DX12 descriptor patching.

## Dispatch scopes

Descriptor bindings are scoped because the same register can legally mean different things in different shaders.  For example, `u11` is `ScratchBuffer` in model pre/post work but `rw_rcas_output` in RCAS.  `t18` is `InitializerBuffer` in generated model work but `r_rcas_input` in RCAS.

Current scopes:

- `model-pre`
- `model-middle`
- `model-post`
- `spd-auto-exposure`
- `rcas`
- `debug-view`

The DX12 recorder must select descriptor bindings whose scope applies to the dispatch being recorded.  It must not blindly use one global sequential list.

## Register counts

The current DP4a reference runner uses a common root-signature shape large enough for the union of known scoped bindings:

| Descriptor space | Count | Highest register |
| --- | ---: | ---: |
| SRV `t#` | 21 | `t20` |
| UAV `u#` | 13 | `u12` |
| CBV `b#` | 2 | `b1` |
| Sampler `s#` | 1 | `s0` |

These are counts, not the number of live descriptors for every pass.  Most passes use a much smaller subset.

## Useful reflection facts

The imported FSR4 DP4a shaders reflect as follows for the balanced/R2160 reference case:

- Pre:
  - `MLSR_Optimized_Constants` at `b0`
  - `cbPass_Weights` at `b1`
  - `g_history_sampler` at `s0`
  - `r_history_color` at `t0`
  - `r_velocity` at `t1`
  - `r_input_color` at `t3`
  - `r_recurrent_0` at `t4`
  - `r_input_exposure` at `t6`
  - `rw_reprojected_color` at `u3`
  - `ScratchBuffer` at `u11`

- Middle pass 1 through 5 and 10 through 12:
  - `ScratchBuffer` at `u1`

- Middle pass 6 through 9:
  - `InitializerBuffer` at `t1`
  - `ScratchBuffer` at `u1`

- Post:
  - `MLSR_Optimized_Constants` at `b0`
  - `r_input_color` at `t3`
  - `r_input_exposure` at `t6`
  - `r_reprojected_color` at `t9`
  - `rw_history_color` at `u1`
  - `rw_mlsr_output_color` at `u2`
  - `rw_recurrent_0` at `u6`
  - `ScratchBuffer` at `u11`
  - `rw_debug_visualization` at `u12` when the debug-visualization permutation is compiled

- SPD auto exposure:
  - `AutoExposureSPDConstants` at `b0`
  - `r_input_color` at `t0`
  - `rw_spd_global_atomic` at `u0`
  - `rw_autoexp_mip_5` at `u1`
  - `rw_auto_exposure_texture` at `u2`

- RCAS:
  - `cbRCAS` at `b0`
  - `r_input_exposure` at `t6`
  - `r_rcas_input` at `t18`
  - `rw_rcas_output` at `u11`

The reference preflight script now has an `--include-utility` mode that also compiles SPD and RCAS.  SPD intentionally uses the utility define set, not the model define set, because defining `FFX_HALF=1` selects half-path helper callbacks that the FSR4 SPD shader does not implement.

## Current backend step

The DX12 runtime consumes this scoped register plan to create a persistent shader-visible CBV/SRV/UAV descriptor arena, patch active SRV/UAV descriptors into the current arena slice, bind root CBVs, and record the real compute dispatch sequence when a native command list is supplied. The root-signature policy is recorded in `fsr4_reference_dx12_root_signature_plan.md`.

The descriptor arena is a ring of full-frame descriptor layouts sized against the same order of magnitude as the FFX backend descriptor ring (`FFX_MAX_QUEUED_FRAMES * FFX_MAX_PASS_COUNT * FFX_MAX_RESOURCE_COUNT`). A recorded command list must never point at descriptor heap slots that can be overwritten by a later frame before the GPU executes it. This is not optional safety machinery; it is required D3D12 correctness and matches the FFX backend model of copying descriptors into a large shader-visible ring.

Each frame-ring slice contains a full SRV/UAV descriptor-table segment per dispatch.  Register aliases such as pre-pass `u11 = ScratchBuffer` and RCAS `u11 = rw_rcas_output` therefore receive different physical descriptor heap slots, while the dispatch-local table base makes both shaders see their expected `u11` binding.  A single frame-global table is invalid for these shaders.

The hot path remains intentionally constrained: no per-frame reflection, no root-signature rebuild, no descriptor-heap recreation under a stable layout, and no runtime shader compilation. Resource transitions are derived from the active scoped patch list for each dispatch.  UAV ordering follows the FFX backend shape: transition or UAV-barrier immediately before a dispatch/clear that binds the resource as UAV, not an unconditional post-dispatch barrier after every pass.  The debug-information UAV is cleared only for frames that actually schedule the debug-view overlay, matching FFX's optional debug clear job; the post shader still binds `rw_debug_visualization` when the debug-visualization permutation is compiled.

## Native output smoke

`apusr_fsr4_reference_dx12_output_smoke` is a controlled DX12 validation step above the structural recorder smoke. It initializes deterministic borrowed input resources, fills the output with a half-float sentinel, records the configured Balanced/R2160 reference dispatch chain, copies a small output region to a readback buffer, and checks that the output was actually written. This remains test-only scaffolding: it does not add readback, debug-layer, or progress work to the runtime hot path.
## Reference path vs native successor

The dispatch-local descriptor table layout is required for the current imported FFX shaders because the same HLSL register can have different meanings in different dispatch scopes.  This is a reference-path compatibility rule, not a desired permanent model-compiler design.  A future APUSR-native pipeline should emit stable logical resource bindings or bindless descriptor indices from a backend-neutral manifest so register aliasing is deliberate, generated, and testable rather than inferred from FFX shader conventions.
