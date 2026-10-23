<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FidelityFX-SC reference material

This directory is reference/provenance material only. It is not part of the APUSR build or shader-generation path.

- `windows/` preserves the FidelityFX-SC source/project snapshot that accompanied the original SDK-derived tool payload.
- `linux_port/` preserves APUSR's earlier Linux portability port for historical comparison.

APUSR no longer builds or ships either wrapper. Active APUSR shader generation invokes DXC directly through `tools/compile_dx12_shaders.py`, `tools/compile_framegen_dx12_shaders.py`, and `tools/compile_framegen_vulkan_shaders.py`.

The retained sources keep their original AMD/MIT notices. The Linux port is retained to document earlier portability work, not as an active dependency or compatibility promise.
