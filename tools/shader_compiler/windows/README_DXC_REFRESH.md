<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Refreshing APUSR's vendored Windows DXC

APUSR treats `dxc.exe`, `dxcompiler.dll`, and `dxil.dll` as one matched compiler payload. Do not refresh only one member of the trio.

Use a recursive DXC checkout. The refresh script accepts `-DxcSource <path>`, then falls back to `DXC_SOURCE_DIR`, then to a sibling `DirectXShaderCompiler` checkout beside the APUSR repository. From the APUSR repository root, run:

```powershell
.\tools\refresh_dxc_windows.ps1 -DxcSource C:\src\DirectXShaderCompiler
```

The script uses the existing checkout directly; it does not clone another DXC tree and does not rewrite the source checkout. It synchronizes/initializes the recorded submodules, builds a dedicated `build-apusr-release` Ninja/Release tree with SPIR-V enabled and LLVM/HLSL/Clang/SPIR-V tests disabled, and builds only `dxc`, `dxcompiler`, and `dxildll`.

The script intentionally does not reject the build merely because a matching WDK is absent. The Windows test trees that use the WDK/TAEF infrastructure are disabled for this toolchain build. If a real compiler target requires an unavailable Windows component, CMake or the compiler itself is allowed to report the actual missing dependency.

Before replacing APUSR's compiler, the freshly built `dxc.exe` must compile both a DXIL compute shader and a Vulkan 1.2 SPIR-V compute shader. The matched trio is then staged and replaced together under `tools\shader_compiler\windows\bin`, followed by the same two smoke tests using the vendored path. A failure during replacement or post-vendor testing restores the old trio.

Exact source/submodule revisions, DXC version text, and SHA-256 hashes are recorded in `tools\shader_compiler\windows\DXC_BUILDINFO.txt`.

The exact source `LICENSE.TXT` and `ThirdPartyNotices.txt` are refreshed into the shared `tools\shader_compiler\licenses\dxc\` notice directory; they are not treated as compiler build dependencies.

To use another local DXC checkout, pass `-DxcSource <path>`. To discard the dedicated incremental DXC build tree first, pass `-CleanBuild`.
