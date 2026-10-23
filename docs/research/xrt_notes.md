<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# XRT notes

APUSR uses modern XRT through `xrt::hw_context` and packaged MLIR-AIE artifacts. Deprecated `load_axlf` style paths are not product paths.

The runtime shape is one frame workload per exported frame slot. XRT waits on GPU export completion, runs the NPU workload, and signals completion for the GPU import pass. CPU involvement is analogous to submitting a command list; it must not become per-token scheduling.

Rules:

- Use the product kernel ABI argument map for resident parameters, ingress frame slot, and egress frame slot.
- Import frame-slot buffers once per runtime setup where possible.
- Treat fence divergence as a hard runtime error.
- Keep XRT-owned probes out of product CMake targets unless they are resurrected as clearly named standalone tools.
- Do not route product execution through old transport echo kernels.

## Windows `std::any` detection

MSVC does not report a C++17-or-newer `__cplusplus` value unless `/Zc:__cplusplus` is enabled. XRT selects `std::any` or `boost::any` from that macro in `xrt/detail/any.h`. If a Windows target includes XRT headers without `/Zc:__cplusplus`, XRT falls back to `boost::any` and the build fails when Boost headers are not installed:

```text
xrt/detail/any.h: fatal error C1083: Cannot open include file: 'boost/any.hpp'
```

Every MSVC target that includes XRT C++ headers must compile with `/Zc:__cplusplus`. `apusr_core` already sets it publicly; standalone XRT tools that do not link through `apusr_core` must set it on their own target.
