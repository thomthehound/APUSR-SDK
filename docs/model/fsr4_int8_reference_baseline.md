<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 reference baseline

The working DX12 drop-in path is now the APUSR FSR4 INT8 reference baseline.  Its job is narrow and strict:

```text
behave like the imported ffxint8 INT8 implementation
or fail
```

The baseline is not a playground for quality/performance experiments.  It is the comparison fixture for every later backend, model rewrite, WMMA pass, and NPU island.

## Baseline ownership

The baseline owns these API-neutral pieces:

- preset and frame-capacity selection
- FSR4 permutation-key packing
- pass order and dispatch grids
- imported shader artifact selection
- pass-0 weights and initializer selection
- optimized/upscaler constants, SPD constants, RCAS constants
- logical resource inventory and lifetime roles
- exact per-dispatch binding/access contract
- reset/history/recurrent/exposure behavior
- optional RCAS and debug-view scheduling rules

Only backend lowering is API-specific:

- DX12 descriptor heaps, root signatures, command lists, resource barriers, and DXIL PSOs
- future Vulkan descriptor sets/buffers, command buffers, image layouts, pipeline barriers, and SPIR-V pipelines
- future D3D11 bridge view translation and command-ownership handoff

A future backend must consume the same API-neutral reference plan before lowering it to its own API.  It must not rediscover model semantics in the frame path.


## Drop-in packaging

The DX12 reference baseline is a real drop-in artifact, not a repo-side development fixture.  The canonical distributable provider file is:

```text
amd_fidelityfx_dx12.dll
```

The DLL must carry the reference shaders and initializers it needs for exposed FSR4 INT8 modes.  A build directory, repo checkout, external shader cache, reflection manifest, or runtime compiler may be useful for development, but none of them may be required for the release drop-in path.

## Acceptance criteria

The reference baseline must not contain fake output routes, approximate bookends, placeholder parameters, runtime shader guessing, or validation-only frame work.  Developer diagnostics are allowed only when explicitly enabled and must not be part of release hot-path execution.

## Descriptor and upload lifetime are behavior

The FFX INT8 shaders use pass-local register meanings.  For example, the same UAV register can name scratch in one dispatch and output in another.  Therefore, the baseline binding contract is dispatch-local:

```text
pass -> exact SRV/UAV/CBV/sampler descriptors for that pass
```

Every backend must preserve descriptor/resource/constant lifetime until the GPU has retired the work that references them.  This is part of correctness, not an optimization detail.

## Acceptable baseline changes

A change may enter the baseline only when it is contract-equivalent or strictly improves backend cost without changing model-visible behavior.  Examples:

- caching a previously rebuilt plan by model-state key
- replacing maps/strings in the release recorder with fixed records
- moving packaging validation out of frame execution
- using a cheaper descriptor-copy path with the same descriptor lifetime

A change may not enter the baseline if it changes ML math, tensor layout, recurrent/history behavior, exposure behavior, reset behavior, external output, or resource lifetime semantics.

## Cross-backend rule

The API-neutral plan in `apusr_fsr4_int8_graph` and `apusr_fsr4_int8_reference_runner` is the portability seam.  DX12 is the first working lowering, not the owner of the model contract.  Vulkan and D3D11 bridge work should reuse the same plan and should add only backend-specific lowering code.
