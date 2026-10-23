<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Backend portability

APUSR uses one API-neutral model/runtime contract and multiple native execution backends. D3D12 is the most mature backend, but Vulkan is a first-class product path rather than a compatibility afterthought.

## Core rule

```text
one model graph
  -> one logical resource/pass contract
      -> D3D12 lowering
      -> Vulkan lowering
      -> future D3D11On12 bridge
      -> experimental accelerator lanes
```

A backend may choose different descriptors, barriers, shader products, resource layouts, queue topology, and compiler-specific spelling. It may not quietly invent a different model graph or different frame semantics.

## API-neutral ownership

The shared graph owns:

- model/pass ordering;
- logical resources and lifetimes;
- preset/capacity/permutation identity;
- constants and dispatch geometry;
- initializer/parameter data;
- SRV/UAV-style access intent.

Backends own API-specific:

- resource objects and allocation;
- descriptor/binding infrastructure;
- command recording;
- barriers/layout transitions;
- pipeline/shader creation;
- queue synchronization and retirement.

## D3D12

D3D12 remains the strongest correctness reference because the FidelityFX-compatible reference fixtures are D3D12-shaped. Product shaders are generated offline as DXIL and embedded/staged through the relevant package.

## Vulkan

Vulkan lowers the same graph directly to native Vulkan compute. Product shaders are generated offline as SPIR-V and collected into a self-indexed shader pack.

The Vulkan backend may use backend-specific HLSL/SPIR-V lowering where the shared source produces a material code-generation defect, provided that:

1. the model/resource contract is unchanged;
2. the transformation is proven safe for the affected layout/geometry;
3. a reference/parity test remains available;
4. hardware or native-code evidence justifies keeping it.

This policy has been important for packed integer dot products and aligned vector storage accesses, where equivalent high-level HLSL can otherwise lower very differently between DXIL and SPIR-V.

## Vulkan capability contract

The shader product declares the operations it requires. Host capture/device negotiation must enable and verify those features before the runtime creates pipelines.

Current native Vulkan FSR4 requirements include accelerated signed packed 4x8 integer dot products and the subgroup/compute-derivative capabilities used by the generated shaders. Frame generation additionally requires the unsigned packed-dot path used by optical flow.

Do not substitute a materially slower scalar implementation merely to make a capability check pass.

## Shader identity

Runtime shader identity is structured data, not a filesystem path. Logical identity is derived from:

- pipeline/model variant;
- preset;
- capacity class;
- product permutation;
- pass kind/logical stage.

Compiler profile strings, source paths, and reflection files are build-time metadata, not runtime ABI.

## Resource lifetime parity

Every backend must guarantee that descriptors/bindings and resources referenced by in-flight GPU work remain valid until retirement.

Examples:

- D3D12 uses shader-visible descriptor lifetime rules appropriate to recorded command lists.
- Vulkan descriptor sets/bindings cannot be overwritten while referenced by in-flight command buffers.
- Cross-queue Vulkan resources require explicit sharing/ownership and synchronization.
- A future D3D11On12 bridge must establish equivalent lifetime/state guarantees before entering the runtime.

## Synchronization parity

Backend parity is semantic, not textual. A D3D12 UAV barrier and a Vulkan pipeline barrier need not look alike; they must enforce the same producer/consumer relationship and no more than necessary.

Avoid blanket post-dispatch barriers when the next operation does not require them. Conversely, do not narrow synchronization beyond what host/resource provenance proves.

## D3D11 direction

D3D11 support should be implemented as a bridge into the established runtime, most likely through D3D11On12. The bridge owns D3D11 resource/view translation and command/queue ownership; the model graph remains unchanged.

## Linux direction

Linux support is expected to center on the native Vulkan backend. The compute backend and the deployment/injection mechanism are separate concerns: Linux packaging should reuse the same Vulkan model/runtime path rather than creating a second implementation.

## Experimental acceleration

WMMA/cooperative-matrix and NPU/AIE work are additional execution lanes. They should consume the same logical model/parameter contract and prove a measured benefit against the DP4A production baseline.

## Related documents

- [Architecture](../architecture.md)
- [Shader ownership](shader_ownership.md)
- [Performance](../performance.md)
- [Host integration strategy](../integration/host_integration_strategy.md)
