<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Reference runtime and future execution placements

APUSR's product upscalers share the FSR 4.0.2-derived INT8 graph. These notes distinguish the reference D3D12 runner from native product execution and prospective accelerator placements.

Execution paths:

```text
frontend         = FFX D3D12 | NGX/DLSS D3D12 | NGX/Streamline Vulkan
reference route  = imported FSR4 INT8 graph through Dx12Fsr4ReferenceRuntime
future variants  = WMMA replacements | NPU islands | D3D11On12 bridge | new model versions
latency policy   = reference fixtures | host-captured same-frame execution
```

The direct FFX and NGX/DLSS compatibility packages are existing product routes. D3D12 and native Vulkan execute the shared logical graph. Experimental GPU/NPU cooperation requires a separate queue contract. Host topology is discovered outside the frame hot path.

The pass schedule is the imported reference graph: optional SPD auto exposure, pre bookend, generated middle passes, post bookend, optional RCAS, and optional debug view.  WMMA and NPU work should replace individual reference dispatches and compare against the same dispatch from the imported graph.


Reference model-state keying is part of the universal contract.  The key includes the selected preset, frame-capacity bucket, backend capability class, motion/depth/auto-exposure/color-space feature bits, and the create-time debug-visualization capability.  Per-frame draw-debug-view and RCAS are dispatch options, not model-state changes.  History reset is required when the model-state key changes, matching FFX's model recreation/reset behavior.

Release frame execution must not perform optional validation or discovery.  Packaged shaders/weights, resident resource plans, and backend pipeline caches are prepared outside the frame-hot path.  Any future WMMA pass or NPU island replaces a named imported reference dispatch and is compared against that dispatch; it does not replace the reference graph itself.
