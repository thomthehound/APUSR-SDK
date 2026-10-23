<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Shader ownership policy

The FSR4 4.0.2-derived reference graph defines the model's numerical and resource contracts. The production D3D12 and Vulkan implementations may specialize shader lowering and resource layouts, but neither may silently substitute a different network or an unvalidated approximation.

The current reference baseline is the imported FFX/FSR4 INT8 graph under:

```text
models/fsr4_int8/reference/fsr4_4_0_2_int8/
```

That artifact set supplies the preset-specific pre shader, generated middle passes, post shader, shared include tree, and `initializers.bin`.  DX12 shader cache entries are generated from that imported graph by the `tools/model/compile_fsr4_reference_dx12_shaders.py` flow and consumed by `Dx12Fsr4ReferenceRuntime`.

The Vulkan implementation is already a native product backend. Future WMMA and NPU variants must identify the reference dispatches they replace, keep a comparable baseline, and validate their tensor, quantization, temporal, and resource semantics. Approximate bookends or substituted model-middle computations are not part of the reference path.

Allowed shader families:

1. Imported FSR4 INT8 reference graph artifacts.
2. Generated cache/manifest products derived from that graph.
3. Explicit experimental replacements that are named by the reference dispatch they replace and are not called “reference” until they match it.

## Backend specialization without semantic divergence

One API-neutral model graph does not require identical generated machine-facing shader assets on every backend. DXIL and SPIR-V are already backend-specific products. Different compiler switches, specialization constants, subgroup/wave choices, binding declarations, or generated manifests are acceptable when they preserve the same logical dispatch contract.

Use this escalation order:

1. shared semantic/reference HLSL with backend-specific compiler configuration and generated blobs;
2. backend-specific lowering helpers/macros that preserve resource roles, constants, pass order, dispatch geometry, and numerical contract;
3. a named backend-specific shader source implementation only when measured performance plus compiler/ISA evidence shows that the shared source spelling prevents a material optimization.

Do not force Vulkan-specific codegen tricks into the D3D12 source path merely to maintain textual sameness. Equally, do not create an independent Vulkan model implementation because one backend is slower. A backend-specific source variant must identify the reference dispatch it replaces, retain the reference implementation for parity testing, document why ordinary compiler/lowering controls were insufficient, and pass the same visual/numerical contract.

Backend-specific binary packs are normal packaging, not semantic divergence. Semantic divergence begins when the backends independently own model behavior, resource meaning, temporal history, or pass ordering.
