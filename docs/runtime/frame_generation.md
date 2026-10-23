<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Frame generation

APUSR implements frame interpolation (FI), optical flow (OF), and backend-specific presentation. Frame generation has a separate runtime lifecycle; disabled frame generation does not schedule FI/OF.

## Ownership model

The subsystem is divided into three layers:

```text
host/frontend feature contract
        |
        v
API-neutral frame-generation scheduling policy
        |
        +-- D3D12 resources/presentation
        +-- Vulkan resources/presentation
        |
        v
FI / OF algorithm and shader products
```

Host compatibility surfaces translate feature options, frame tokens, resources, and pacing markers. The scheduler decides whether a source frame can produce a generated frame. Backend-specific code owns command recording, resource state, queues, WSI/swapchain behavior, and retirement.

## D3D12

The D3D12 frame-generation provider is built from source maintained in the repository. It includes:

- FidelityFX-compatible host ABI/provider transport;
- D3D12 FI/OF backend execution;
- swapchain/presentation/pacing integration;
- offline-generated FI/OF shader permutations;
- source-built resource/state management.

The package stages APUSR-built provider code and shader products.

## Vulkan

Vulkan frame generation is native Vulkan, not D3D12 translation. It uses:

- captured Vulkan device and queue topology;
- Vulkan FI/OF shader products generated as SPIR-V;
- a retained-source virtual-swapchain architecture;
- explicit timeline/fence ownership for source replay, generation, and presentation;
- accelerated unsigned packed-dot operations in optical flow;
- Vulkan latency-control integration where supported.

The detailed WSI/resource/queue contract lives in [vulkan_framegen_architecture.md](vulkan_framegen_architecture.md).

## Retained-source presentation model

When Vulkan frame generation is active, APUSR can keep the loader's real WSI images private and expose ordinary APUSR-owned images as the application's logical swapchain.

A presented virtual backbuffer is retained as the immutable source frame. Source replay copies it to a real WSI image while FI/OF can consume the same retained image directly. This avoids a separate steady-state full-resolution source snapshot.

Virtual-slot reuse is gated by the actual consumers of that source image. The design does not rely on empty release submissions or a global queue drain to make reuse appear safe.

## Queue policy

Heavy generation work should use an independent compute queue when the captured topology provides one without stealing the application's active queue. If no suitable spare compute queue exists, generation uses a safe graphics-queue configuration.

Only one heavy generation is allowed in flight. New source frames remain independently presentable and obsolete generation work is not allowed to build an unbounded queue behind the application.

A generated frame that cannot satisfy its real-time presentation decision is discarded rather than delaying source presentation.

## Pacing and back-pressure

Frame generation must remain asynchronous without allowing presentation debt to accumulate indefinitely.

The Vulkan virtual presentation path therefore keeps a small bound on outstanding physical source presentation. This preserves ordinary WSI-style back-pressure while avoiding the large latency spikes that appear when several source PRESENT markers are allowed to lag behind corresponding input markers.

The pacing path must not regress into:

- application-thread midpoint sleeps;
- CPU spin loops;
- full presenter drains;
- `QueueWaitIdle`/device-idle synchronization;
- unbounded source/generation queues.

## Shader products

FI/OF shader permutations are generated offline. The current D3D12 and Vulkan products cover the supported precision, subgroup/wave, depth, and motion-vector configuration space without runtime compilation.

Optical flow uses exact packed-byte difference arithmetic followed by accelerated unsigned packed-dot accumulation on Vulkan. This is preferred over a large scalar expansion of specialized masked-SAD operations when the Vulkan compiler/driver cannot prove a native equivalent.

## FidelityFX compatibility

APUSR preserves the external ABI shapes needed by supported FidelityFX-era hosts while keeping internal scheduling/backend state APUSR-owned.

Compatibility with newer descriptor structures does not imply support for every newer provider ABI behavior. APUSR should advertise only the feature/version contract it actually implements.

The detailed external-SDK delta audit is retained under [../research/ffx_sdk_2_3_audit.md](../research/ffx_sdk_2_3_audit.md).

## Performance priorities

Frame-generation profiling separates at least:

- prepare/setup;
- FI/OF compute;
- source readiness/replay;
- generated-frame readiness;
- physical presentation;
- application-present back-pressure.

The current optimization goal is to reduce real FI/OF GPU work and unnecessary full-resolution traffic while keeping pacing stable. Queue tricks that merely move a wait without improving throughput are not performance wins.

See [../performance.md](../performance.md) and [../profiling.md](../profiling.md).

## Invariants

- Generate frames only from the implemented FI/OF pipeline.
- Do not route super resolution through the frame-generation provider.
- Do not add frame-generation work to the SR hot path when FG is disabled.
- Preserve AMD/MIT provenance on FidelityFX-derived algorithm/shader source.
- Do not advertise unsupported FidelityFX/Streamline/DLSS feature generations.
- Prefer source-frame continuity over completing a stale generated frame.
- Treat teardown/re-enable behavior as a resource/presentation correctness problem, not a reason to add global idle waits.

## Future work

- reduce FI/OF GPU cost further;
- broaden HDR/UI-composition coverage;
- continue Vulkan pacing/teardown compatibility hardening;
- add a D3D11On12-facing adapter without duplicating FI/OF algorithms;
- evaluate WMMA only for measured FI/OF hotspots where the math maps cleanly.
