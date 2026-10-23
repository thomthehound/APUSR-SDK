<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Third-party notices

The Apache-2.0 license in the repository root applies to original APUSR work.
It does **not** relicense third-party code, derived components, generated
payloads, or vendor/reference artifacts. Per-file notices and the component
license files listed below remain authoritative.

## AMD FidelityFX SDK 2.0.0 reference material

APUSR retains a deliberately quarantined reference/provenance snapshot under
`third_party/ffx_sdk/`. The source package from which this snapshot was taken
identifies itself as **AMD FidelityFX SDK 2.0.0**, includes FSR 4.0.2 material,
and distributes that snapshot under the MIT License. The retained license is `third_party/ffx_sdk/LICENSE.md`; a distribution-copy is also kept at `LICENSES/FidelityFX-MIT.txt`.

Copyright (C) 2025 Advanced Micro Devices, Inc.

Active APUSR Frame Generation code under `src/framegen/` includes files derived
from that MIT-licensed FidelityFX material. Those files retain AMD's notices;
APUSR-authored modifications and surrounding integration code are Copyright (c)
2026 thomthehound where indicated.

The FSR4 INT8 reference shaders and initializer payloads under
`models/fsr4_int8/reference/` are derived from the same MIT-licensed SDK 2.0.0
snapshot. The resident parameter arenas under `models/fsr4_int8/resident_params/`
are generated transformations of those MIT-licensed model parameters. See the
local provenance READMEs in those directories.
The checked-in Vulkan shader-pack fixture under `tests/data/` is likewise a
compiled derivative of that reference shader material and carries a local copy
of the same MIT license.

Do not assume that a newer FidelityFX SDK release uses the same licensing
layout. Updating or replacing this snapshot requires a fresh license audit.

## Microsoft Detours 4.0.1

`third_party/detours/` contains Microsoft Detours 4.0.1 headers and the x64
static library used by the Windows Vulkan host-capture bootstrap.

Copyright (c) Microsoft Corporation.
License: MIT. See `third_party/detours/LICENSE.md` and `LICENSES/Detours-MIT.txt`.

## DirectX Shader Compiler (DXC)

`tools/shader_compiler/windows/` and `tools/shader_compiler/linux/` contain
matched DXC runtime payloads built from the source revision recorded in each
`DXC_BUILDINFO.txt` file. Their upstream license and third-party notices are
preserved under `tools/shader_compiler/licenses/dxc/`.

These compiler binaries are not APUSR-owned and are not covered by APUSR's
Apache-2.0 license.

## AMD Radeon Data File (RDF)

`tools/rgp/libamdrdf/` contains AMD's Radeon Data File library used by APUSR's
RGP analysis tooling.

Copyright (c) 2022-2024 Advanced Micro Devices, Inc.
License: MIT. See `tools/rgp/libamdrdf/LICENSE`.

That subtree also carries its own imported dependencies and license files:

- CLI11 — BSD 3-Clause; `tools/rgp/libamdrdf/imported/cli11/LICENSE`
- nlohmann/json — MIT; `tools/rgp/libamdrdf/imported/json/LICENSE`
- Zstandard — BSD 3-Clause; `tools/rgp/libamdrdf/imported/zstd/LICENSE`

## Other compatibility declarations

Small compatibility declarations that were written specifically for APUSR and
do not contain copied third-party implementation code remain APUSR work and use
Apache-2.0. Names, constants, and ABI shapes belonging to external APIs remain
subject to the applicable trademark/API terms of their owners; the APUSR
license grants rights only in APUSR's own copyrightable implementation.

## Optional external build/runtime dependencies

APUSR can be configured against dependencies that are **not vendored by this
repository**. Their licenses do not become APUSR licenses merely because CMake
finds them on the build host.

- **Microsoft WinPixEventRuntime** is used only by opt-in GPU-marker profiling
  builds. Microsoft has distributed the WinPixEventRuntime source and NuGet
  package under the MIT License since March 2024. APUSR keeps the audited
  Microsoft MIT text at `LICENSES/WinPixEventRuntime-MIT.txt`; when a profiling
  build actually redistributes `WinPixEventRuntime.dll`, CMake also requires and
  stages the exact license file from the selected package. Pre-MIT or otherwise
  different packages are not redistribution-cleared by this repository.
- **AMD/Xilinx XRT** is an optional NPU runtime dependency. APUSR does not carry
  `xrt_coreutil.dll` in the source repository. Any future public package that
  redistributes XRT runtime binaries must first record and satisfy the license
  and notice requirements for the exact XRT build being shipped.

Normal GPU-only distribution builds do not require either dependency.
