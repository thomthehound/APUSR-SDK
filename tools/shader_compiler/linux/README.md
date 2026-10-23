<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR DXC payload for Linux

This directory contains the Linux DXC runtime trio used by APUSR's shader-generation scripts:

- `bin/dxc`
- `bin/libdxcompiler.so`
- `bin/libdxil.so`

The shared libraries stay beside `dxc` so the checked-in compiler can run without a system DXC installation. Exact source/submodule provenance and hashes are recorded in `DXC_BUILDINFO.txt`; shared license/notice material are in `../licenses/dxc/`.

There is deliberately no FidelityFX_SC wrapper, glslang payload, copied DXC development SDK, or Agility SDK here. APUSR invokes DXC directly and owns its own permutation/build tooling.
