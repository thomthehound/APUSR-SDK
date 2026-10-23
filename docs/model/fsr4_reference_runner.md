<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 INT8 reference runner

APUSR's FSR4 INT8 reference path is intentionally split into two layers:

1. An API-neutral model runner plan in `src/models/fsr4_int8/`.
2. Concrete backend recorders, starting with DX12, that compile shaders, allocate resources, upload constants, bind descriptors, and dispatch the plan.

The API-neutral runner does not expose `ID3D12Resource`, `VkImage`, NGX parameter bags, or FFX backend structs. It describes the same work the FFX FSR4 DX12 provider schedules, but in APUSR-owned terms so the same graph can later be recorded through DX12, Vulkan, WMMA-specialized passes, or NPU islands.

## Current reference-runner layer

`apusr_fsr4_int8_reference_runner` currently provides:

- aligned upscale-size selection;
- model preset selection from render/display scale;
- FFX-compatible permutation description and keying;
- FFX-compatible optimized constants, SPD auto-exposure constants, RCAS constants, and pass-0 weight-buffer layout;
- shader compile descriptors for `pre.hlsl`, `passes_*.hlsl`, `post.hlsl`, SPD auto exposure, RCAS, and debug view;
- dispatch grids for the DP4a reference pass sequence;
- FFX-compatible shader binding names mapped to APUSR `ResourceId` / constant-buffer roles;
- initializer loading for each imported model preset.

This layer is not yet the in-game DX12 recorder. It is the plan the DX12 recorder must consume.

## Important entry-point detail

The imported FSR4 INT8 graph uses:

- `main` for `pre.hlsl`;
- `fsr4_model_v07_i8_pass1` through `fsr4_model_v07_i8_pass12` for the middle model shader;
- `main` for `post.hlsl`;
- `main` for SPD auto exposure, RCAS, and debug view.

Do not treat the pre/post shaders as generated pass-0/pass-13 entry points.  The imported generated middle shader keeps each entry point behind an `MLSR_PASS_N` preprocessor gate, so a DX12 compile for pass `N` must define `MLSR_PASS_N=1` or DXC will report a missing entry point.

## Backend work next

The DX12 backend should consume `ReferenceFramePlan` and `ResourcePlan` in this order:

1. Compile the `ReferenceShaderCompileDesc` list with the imported include roots and defines.  The baseline DX12 reference contract is `cs_6_6`, `-HV 2021`, `-enable-16bit-types`, `FFX_HLSL=1`, `FFX_HALF=1`, and `FFX_HLSL_SM=66`.
2. Create persistent resources from `ResourcePlan`.
3. Upload `initializers.bin` into `InitializerBuffer`.
4. Copy pass-0 weights from the initializer data into `Pass0WeightsConstants`.
5. Upload per-frame constants from `ReferenceFramePlan`.
6. Resolve aliases such as `MlsrOutput`, `RcasOutput`, and `ResultColor` based on whether RCAS is enabled.
7. Lower each dispatch's exact `ReferenceDispatchDesc::bindings` list to backend descriptors and access transitions.
8. Record dispatches in `ReferenceFramePlan::dispatches`.

`ReferenceDispatchDesc::bindings` is authoritative for that dispatch.  Backends must not reconstruct resource usage from pass kind, stage-wide binding tables, or descriptor class.  In particular, read/write intent is explicit because a UAV binding is not necessarily a write in every pass.

The direct FFX replacement path uses this DX12 recorder as the correctness fixture. The NGX/DLSS facade uses the same recorder for DLSS-shaped game integrations.
