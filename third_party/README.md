<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Third-party material

External source and binary material is quarantined here so provenance and
product ownership remain explicit.

Current areas:

- `ffx_sdk/` — FidelityFX SDK 2.0.0 reference/provenance snapshot retained under
  the MIT license carried with that snapshot. Normal APUSR configure, build,
  runtime, and shader generation do not compile or include this tree.
- `detours/` — Microsoft Detours 4.0.1 headers and x64 static library used by
  the Windows Vulkan host-capture bootstrap, under MIT.
- `streamline_compat/` — APUSR-authored minimal compatibility declarations used
  at the Streamline/NVAPI boundary; no vendored Streamline implementation is
  carried here.

Third-party material keeps its own copyright and license terms. APUSR-maintained
derivatives preserve upstream notices where applicable. See
`../THIRD_PARTY_NOTICES.md` and the README/license files inside each subtree.

## Product boundary

APUSR's production super-resolution implementation lives under `include/`,
`src/`, `shaders/`, `models/`, and `tools/`. Reference imports must not become
hidden runtime or build dependencies.

When behavior is learned from a reference implementation, maintain the required
product contract in the active APUSR tree and preserve the original provenance
of any derived code.
