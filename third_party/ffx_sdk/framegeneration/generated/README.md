<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Reference FidelityFX_SC shader output

`ffx_sc_output/` preserves the original FidelityFX_SC-generated Frame Interpolation / Optical Flow permutation headers and watermark payload from the reference SDK snapshot.

These headers are **not** compiled by APUSR. They are retained as a byte/reflection baseline and provenance record. APUSR's active DX12 runtime consumes `src/framegen/algorithm/generated/apusr_framegen_dxil.{h,cpp}`, and normal regeneration is owned by `tools/compile_framegen_dx12_shaders.py` using DXC directly.

The only supported active use of this directory is an explicit migration/comparison operation such as:

```bash
python tools/compile_framegen_dx12_shaders.py --import-reference
```

That operation imports the reference payload into APUSR's own pack format; it is not part of configure, build, packaging, or runtime.
