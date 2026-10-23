<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR DXC payload for Windows

This directory contains the matched Windows x64 DXC runtime trio used by APUSR's shader-generation scripts:

- `bin/dxc.exe`
- `bin/dxcompiler.dll`
- `bin/dxil.dll`

Keep the trio together. Exact source/submodule provenance and hashes are recorded in `DXC_BUILDINFO.txt`; shared license/notice material are in `../licenses/dxc/`.

There is deliberately no FidelityFX_SC wrapper, glslang payload, copied DXC development SDK, or Agility SDK here. APUSR invokes DXC directly and owns its own permutation/build tooling.

For an intentional local refresh from a DXC source checkout, use `tools/refresh_dxc_windows.ps1`; see `README_DXC_REFRESH.md`.
