<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 DP4A-optimized checkpoint

This document tracks the DOT4/DP4A shader-locality lane. The checkpoint order is intentionally linear:

```text
strict-reference
  -> cpu-dp4a-tuned
      -> dp4a-optimized
          -> later wmma-optimized / npu-assisted lanes
```

The `dp4a-optimized` lane is the current distributable DOT4/DP4A performance checkpoint. It starts from the CPU/backend tuned runtime and embeds APUSR-owned optimized shaders in the normal product shader matrix. It is not an experiment-cache path, not an environment-variable override, and not a sidecar package.

## Current keeper

The keeper contains two front-C16 dispatch-preserving tiled shaders:

```text
shaders/dx12/optimized/apusr_fsr4_i8_pass1_tiled_exact.hlsl
  entry:    apusr_fsr4_i8_pass1_tiled_exact
  replaces: fsr4_pass_1

shaders/dx12/optimized/apusr_fsr4_i8_pass2_tiled_exact.hlsl
  entry:    apusr_fsr4_i8_pass2_tiled_exact
  replaces: fsr4_pass_2
```

Both shaders keep the generated dispatch ABI:

```text
[numthreads(64, 2, 1)]
output tile: 64x2x16
input halo:  66x4x16
LDS use:     4,224 bytes
preload:     264 Load4 C16-pixel reads instead of 1,056 scalar dword reads
pass1:       branchless padded Stage0 Load4 preload for all active tiles
pass2:       existing active-edge preload path, shifted to the new Stage1 offset
```

Pass0 now has a product-only `APUSR_FSR4_I8_PADDED_STAGE0_CARRIER` mode used by the `dp4a-optimized` front-C16 shader matrix. In that mode it writes active Stage0 C16 at padded origin `(1,1)`, clears the one-pixel halo and inactive final-strip dispatch tail, and shifts Stage1 output to `paddedStage0Bytes`. Strict-reference builds leave the dense imported Stage0 ABI unchanged.

In the `dp4a-optimized` build lane, the normal FSR4 DX12 shader-blob generator embeds the full product matrix into the DLL and substitutes these shaders for `fsr4_pass_1` and `fsr4_pass_2`. The output remains a single drop-in DLL.


## Pass0 -> pass1 scratch carrier audit

The current `dp4a-optimized` keeper promotes the padded Stage0 C16 carrier for the optimized front-C16 path. This makes pass1 cleaner by giving Stage0 a zeroed preload envelope and shifting the Stage1 boundary past that padded carrier. The important scratch fact is that this does **not** require another full C16 frame. It only requires the extra halo rows and columns:

```text
capacity  dense Stage0 C16       padded Stage0 C16      extra bytes   reserve after shifting Stage1
1080      960x540x16  = 8.29 MB  962x542x16  = 8.34 MB   48,064       96,192
2160      1920x1080x16 = 33.18 MB 1922x1082x16 = 33.27 MB 96,064       192,192
4320      3840x2160x16 = 132.71 MB 3842x2162x16 = 132.90 MB 192,064      384,192
```

Therefore the current declared scratch sizes already have enough reserve for the padded Stage0 C16 carrier because pass1 output now moves from `denseStage0Bytes` to `paddedStage0Bytes`. Keeping pass1 output at the dense reference offset while adding a padded Stage0 carrier would be unsafe: pass1 would overwrite the padded Stage0 halo while other pass1 groups could still be reading it.

The repo now has three planning aids for this:

```text
include/apusr/models/apusr_fsr4_int8_scratch_layout.h
tools/analysis/fsr4_int8_scratch_layout.py
models/fsr4_int8/scratch_layout_contract_v0.json
```

The C++ helper keeps these offsets available to runtime/resource planning. The Python tool parses the imported ML2Code HLSL tensor descriptors, emits a named-region scratch ABI report, and verifies that each proposed region layout fits, is contiguous, is aligned, and is non-overlapping. The checked-in JSON file is a snapshot of the strict dense layout and padded Stage0 layout across common unusual display classes.

Examples:

```powershell
# Human-readable report for one display, comparing current and padded layouts.
python tools/analysis/fsr4_int8_scratch_layout.py --repo-root . --display 3440x1440 --layout-variant all --check

# Refresh the machine-readable contract snapshot used for reviews/CI.
python tools/analysis/fsr4_int8_scratch_layout.py --repo-root . --common-weird-displays --middle-stages --layout-variant all --check --write-contract models/fsr4_int8/scratch_layout_contract_v0.json
```

The padded Stage0 layout is not a shader-only patch. It is a coordinated contract:

```text
1. pass0 writes Stage0 at padded origin (1,1) and materializes the zero halo;
2. pass1 reads the padded Stage0 carrier with no edge checks;
3. pass1 writes Stage1 at paddedStage0Bytes;
4. pass2 reads Stage1 from paddedStage0Bytes and writes Stage2 at the existing offset 0;
5. downstream generated passes keep their current Stage2-and-later scratch layout.
```

That contract is especially important for non-capacity display sizes.  A 3440x1440 output uses a 1720x720 active C16 plane under the 2160-capacity model, so pass1 dispatches `27x360` groups with the current 64x2 tile.  The final group has 56 active pixels and 8 padding pixels; it is neither a whole 64-wide strip nor an even group count.  Capacity-sized 2160 and 4320 tensors are whole/even, but arbitrary monitor modes are not.

The optimized front-C16 path now treats capacity size and active display size as separate contracts. Scratch rows stay capacity-strided because the imported generated tensors are capacity-shaped. Pass0 materializes semantic padding outside the active tensor, including the inactive final-strip tail that pass1's branchless 66x4 preload can touch on odd, portrait, super-ultrawide, and surround modes. Pass2 still receives active-display dimensions from `MLSR_Optimized_Constants.width/height` because its Stage1 input is dense, not padded.

The scratch helper and analysis tool also report all generated middle-pass tile shapes, not only the front C16 passes:

```powershell
python tools/analysis/fsr4_int8_scratch_layout.py --repo-root . --common-weird-displays --middle-stages --check
```

Do not assume a whole or even number of macrotiles.  The product rule is: dispatch over the active aligned display tensor, read/write through the capacity-strided scratch layout, and explicitly zero semantic padding outside the active tensor.

## Why scratch changes are treated as a contract

Scratch is only memory at the allocation layer, but it is not just memory at the model layer.  The imported ML2Code shaders bake tensor byte offsets, tensor extents, row strides, and ping-pong reuse into generated HLSL.  APUSR also binds the same object through CPU planning, DX12 descriptors, future Vulkan descriptors, and eventually shared UMA/XRT/NPU access.  Changing size is easy; changing meaning is the risky part.

The main hazards are:

```text
1. producer/consumer ABI drift: pass0, pass1, pass2, post, and future NPU islands must agree on every byte offset and stride;
2. lifetime aliasing: several stage boundaries intentionally reuse the same scratch ranges after barriers, so moving one boundary can overwrite data another dispatch still needs;
3. active-vs-capacity confusion: unusual display modes often dispatch partial active tiles inside a larger capacity tensor;
4. cross-device coherency: UMA/NPU sharing will add placement, cache, fence, and ownership requirements on top of byte sizing;
5. resize behavior: changing scratch capacity at runtime can force descriptor/resource recreation and cause game-visible stalls.
```

So scratch layout changes are allowed, and we should make them when they buy performance or simplify the pipeline.  They should go through a named layout contract, helper/audit coverage, and shader/runtime updates together instead of being patched as isolated constants.

The practical workflow for scratch manipulation is:

```text
1. Add or update a named ScratchLayoutVariant in the C++ helper.
2. Mirror the proposed region layout in the Python audit tool.
3. Run the weird-display/middle-stage audit and refresh scratch_layout_contract_v0.json.
4. Update shader constants/resource binding only after the ABI report is clean.
5. Promote the variant by switching runtime planning to the named contract, not by copying offsets into shaders.
```

This keeps scratch work off the hot path: the expensive parsing/reporting happens in tools or tests, while runtime code receives simple constants and resource sizes.

## What is intentionally not promoted

The DP4A fusion experiments are not product behavior in this checkpoint.

The tested Stage1->2 and Stage1->2->3 fused DP4A variants deleted global scratch boundaries but were slower or slightly slower than the non-fused tiled keeper. The useful conclusion is that deleting scratch traffic is not enough when the fused producer work increases CU pressure, LDS footprint, register pressure, or occupancy loss.

Keep the fusion lessons, but do not ship those variants in the `dp4a-optimized` lane:

```text
not promoted:
  pass1-owned Stage1->2 fusion
  pass2-owned Stage1->2 fusion
  pass2-owned 2D/H8 Stage1->2 fusion
  pass3-owned Stage1->2->3 fusion
```

Future fusion work should move to a separate experimental lane, most likely after the individual DOT4/DP4A passes are tuned or after WMMA pointwise kernels exist.

## Pass1 status

Pass1 is the measured win in this lane. The optimized shader keeps the working `64x2` dispatch shape and uses a C16-pixel `Load4` tile preload:

```text
older preload: 66 * 4 * 4 scalar dword iterations = 1,056 loop iterations/group
current preload: 66 * 4 C16-pixel Load4 iterations = 264 loop iterations/group
```

The shader also uses the short-lifetime pointwise form for the C16->C32->C16 block. That state-shape change was measured neutral on the current test machine, but it does not change correctness or the Stage1 boundary and keeps the code ready for later pointwise/WMMA work.

## Pass2 status

Pass2 is included as a conservative preparation/tuning step. It is neutral to trivially faster on the current test machine, and it keeps the front-C16 region coherent for future per-pass tuning:

```text
input:  Stage1 C16 scratch at paddedStage0Bytes offset
output: Stage2 C16 scratch at offset 0
```

Pass2 is not fused in the keeper. It receives the same dispatch-preserving C16 `Load4` locality treatment as pass1, plus the short-lifetime pointwise form, while preserving the exact Stage2 boundary.

## Next DOT4/DP4A work

The next distribution-oriented work should tune more individual passes before trying more fusion:

```text
1. Preserve strict reference and cpu-dp4a-tuned lanes.
2. Keep dp4a-optimized as the distributable DOT4/DP4A lane.
3. Tune passes one at a time with exact stage-boundary output.
4. Promote only measured wins or clear neutrals that simplify the next pass.
5. Move WMMA work to a separate capability-gated lane later.
```

The completed pass-3/6 and pass-9/11 dependency-scheduling experiments are no
longer part of the distribution path. They established two useful facts:

1. `4.0.2b`-style compatibility scheduling preserves native signed packed DP4A;
   it does not require scalar fallback.
2. The compatibility transformation is not free on the current newer AMD
   target. Passes 9 and 11 each regressed by about 6.4% when fully de-chained,
   and the whole-upscaler median increased by about 1.0% in the controlled
   extension capture.

Mainline therefore keeps the faster reference-style accumulation for passes 3,
6, 9, and 11 and does not carry RDNA2 compatibility switches through the CMake
or shader-generation path. The source shadows, exact DXIL shapes, measured
costs, and reproduction script are preserved under
`experiments/rdna2_dp4a_compat/` for a possible future RDNA2-specific branch.

The broader 4.1.1 observation remains useful for ordinary optimization: most
inspected neural passes use shallow packed-DOT4 dependency chains. Treat that as
a pass-specific scheduling lead to be measured on current targets, not as a
compatibility requirement and not as a blanket instruction to de-chain.

`tools/analysis/dxil_dp4a_schedule.py` remains the preferred DXIL-level audit
for packed-DOT4 call count and direct dependency depth. Native ISA/register/
occupancy tooling is only warranted when a current-target timing result makes
those lower-level questions material.

Good candidates for later DOT4/DP4A tuning should be chosen from measured cost and locality shape, not from stage order alone.

## Larger island target remains research

The original larger memory-locality target remains informative but is not part of this keeper:

```text
Stage1 ConvNext C16
  -> exact local Stage1 C16 boundary
Stage2 ConvNext C16
  -> exact local Stage2 C16 boundary
  -> SkipA sideband store
Stage3 k2/s2 C16->C32 downsample
```

For a 3840x2160 output / 1920x1080 C16 model plane:

```text
C16 plane bytes = 1920 * 1080 * 16 = 33,177,600 bytes
Stage1 global write + Stage2 global read = 66,355,200 bytes/frame
Stage2 main-path global read for Stage3   = 33,177,600 bytes/frame
Total semantic traffic target             = 99,532,800 bytes/frame
```

The SkipA store is still required because the decoder consumes it later; it is a tee, not a deleted boundary.

The DP4A fusion experiments showed that this target probably needs either better planner-aware kernels across more passes or WMMA/NPU acceleration for the dense producer work before it can beat the simpler non-fused keeper.

## Correctness gates

Future shader work should be tested at stage boundaries before judging final RGB:

```text
Stage1-only:     strict Stage1 output equals candidate Stage1 output
Stage2-only:     strict Stage2 output equals candidate Stage2 output
Stage1+2:        strict Stage2 output equals candidate Stage2 output
Stage1+2+3:      strict Stage3 output equals candidate Stage3 output, and strict SkipA equals candidate SkipA sideband
```

Final-image tests are not enough for this work. They can hide a boundary rounding error until later recurrent/decoder stages amplify it.
