<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FidelityFX SDK reference/import manifest

## Source identity

- Upstream family: AMD FidelityFX SDK
- Source snapshot: FidelityFX SDK 2.0.0
- FSR ML upscaler identified by that snapshot: FSR 4.0.2
- Snapshot license: MIT (`third_party/ffx_sdk/LICENSE.md`)

The retained tree is a reduced historical/reference snapshot rather than an
active implementation dependency.

## Retained reference material

- `api/`, `backend/`, and `upscalers/`
  - API/backend/resource material useful for ABI and implementation comparison.
- `framegeneration/`
  - Original Frame Generation, Frame Interpolation, Optical Flow, DX12
    swapchain/provider code, HLSL, and build recipes from the reference baseline.
- `framegeneration/generated/ffx_sc_output/`
  - Original FidelityFX_SC-generated DXIL/permutation metadata retained as a
    byte-comparison baseline.
- `reference_tools/fidelityfx_sc/`
  - FidelityFX_SC source/project material and APUSR-maintained portability work.

The narrow FidelityFX-compatible public/host ABI material that APUSR actually
maintains lives outside this snapshot under the active `include/` and
`src/framegen/` trees and retains its applicable provenance notices.

## Why it remains

The snapshot is useful for checking ABI behavior and algorithm ancestry,
comparing future upstream changes deliberately, explaining the provenance of
AMD-derived active code, and verifying original shader permutation
bytes/reflection during migrations.

It is not a source of normal build inputs. CMake, product source, and normal
shader generation must not include or compile this tree.

## Binary policy

No prebuilt FidelityFX Frame Generation provider DLL is carried or supported by
the product package path. APUSR builds its compatible provider from the
maintained active source tree.

## Ownership policy

When APUSR needs behavior learned from the reference, implement or maintain the
required contract in APUSR-owned active code rather than reconnecting the
product to this tree. Preserve AMD/MIT provenance on derived files; do not
confuse engineering ownership with copyright authorship.
