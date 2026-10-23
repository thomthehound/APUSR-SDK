<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FidelityFX SDK 2.0.0 reference snapshot

This directory deliberately preserves a reduced FidelityFX SDK 2.0.0
reference/provenance snapshot used during APUSR development. The source package
identifies its FSR ML upscaler as FSR 4.0.2 and distributes the carried snapshot
under the MIT License in `LICENSE.md`.

This is historical/reference/provenance material, not an active product source
tree.

Useful reference areas include:

- `framegeneration/` — the FidelityFX Frame Generation / Frame Interpolation /
  Optical Flow implementation and its original DX12 build material;
- `framegeneration/generated/ffx_sc_output/` — original FidelityFX_SC-generated
  permutation headers retained as a migration/byte-comparison baseline;
- `reference_tools/fidelityfx_sc/` — the original FidelityFX_SC tool snapshot
  plus APUSR-maintained portability work;
- retained SDK API/backend/upscaler material used for ABI and implementation
  comparison.

## Boundary

Normal APUSR configure, build, runtime, and shader regeneration must not compile
or include files from this tree. APUSR-maintained active code lives under
`include/`, `src/`, `shaders/`, and `tools/`.

An explicit migration/comparison command may read the reference snapshot. In
particular, `python tools/compile_framegen_dx12_shaders.py --import-reference`
can import original generated DXIL/metadata into APUSR's pack format. That is an
explicit reference operation, not a normal build dependency.

FidelityFX-derived active files retain AMD's MIT notices where applicable.
Moving an active derivative outside this directory does not erase its
provenance.

Do not refresh this directory from a newer FidelityFX SDK without re-auditing
the exact newer license; FidelityFX licensing is version-specific.
