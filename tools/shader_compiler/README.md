<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR shader compiler payloads

APUSR's active shader compiler payload is DXC. Nothing in this directory implements or wraps the FidelityFX shader compiler.

The platform payloads are matched runtime trios:

- Windows: `windows/bin/dxc.exe`, `dxcompiler.dll`, `dxil.dll`.
- Linux: `linux/bin/dxc`, `libdxcompiler.so`, `libdxil.so`.

Both current trios were built from the exact DXC revision recorded in each platform's `DXC_BUILDINFO.txt`. Shared DXC license/notice material live in `licenses/dxc/`.

APUSR owns permutation enumeration, compiler recipes, reflection extraction, deduplication, and generated-pack formats. The active entry points are:

```bash
python tools/compile_dx12_shaders.py --verify
python tools/compile_dx12_shaders.py
python tools/compile_framegen_dx12_shaders.py --smoke
python tools/compile_framegen_dx12_shaders.py
python tools/compile_framegen_vulkan_shaders.py
```

The old AMD FidelityFX_SC wrapper and APUSR's former Linux port are retained only as historical reference under `third_party/ffx_sdk/reference_tools/fidelityfx_sc/`. They are not shipped, linked, or required by active shader generation.
