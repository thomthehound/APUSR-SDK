<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Universal runtime plan

APUSR is now organized around a single correctness baseline: the imported FSR4 INT8 reference graph.  Frontends may differ, and future execution placements may differ, but the graph being matched is the same.

Execution paths:

```text
frontend         = FFX DX12 drop-in | NGX/DLSS-compatible shim | future D3D11/Vulkan
reference route  = imported FSR4 INT8 graph through Dx12Fsr4ReferenceRuntime
future variants  = WMMA replacements | NPU islands | Vulkan/D3D11 execution replacements
latency policy   = frame-delayed fixture | inline queue capture
```

The direct FFX replacement package remains a correctness fixture.  The NGX/DLSS-compatible facade plus DLSS compatibility package is the product-shaped broad compatibility route. Queue capture remains a later scheduler concern for same-frame GPU/NPU cooperation and must enter through cold-path host-capture registration, not repeated per-frame probing.

The pass schedule is the imported reference graph: optional SPD auto exposure, pre bookend, generated middle passes, post bookend, optional RCAS, and optional debug view.  WMMA and NPU work should replace individual reference dispatches and compare against the same dispatch from the imported graph.


Reference model-state keying is part of the universal contract.  The key includes the selected preset, frame-capacity bucket, backend capability class, motion/depth/auto-exposure/color-space feature bits, and the create-time debug-visualization capability.  Per-frame draw-debug-view and RCAS are dispatch options, not model-state changes.  History reset is required when the model-state key changes, matching FFX's model recreation/reset behavior.

Release frame execution must not perform optional validation or discovery.  Packaged shaders/weights, resident resource plans, and backend pipeline caches are prepared outside the frame-hot path.  Any future WMMA pass or NPU island replaces a named imported reference dispatch and is compared against that dispatch; it does not replace the reference graph itself.
