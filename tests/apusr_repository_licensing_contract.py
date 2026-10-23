#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Guard APUSR's public licensing/provenance boundaries against accidental drift."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

REQUIRED_FILES = (
    "LICENSE",
    "NOTICE",
    "DCO",
    "THIRD_PARTY_NOTICES.md",
    "CONTRIBUTING.md",
    "docs/development/licensing.md",
    "third_party/ffx_sdk/LICENSE.md",
    "third_party/detours/LICENSE.md",
    "LICENSES/README.md",
    "LICENSES/Apache-2.0.txt",
    "LICENSES/FidelityFX-MIT.txt",
    "LICENSES/Detours-MIT.txt",
    "LICENSES/WinPixEventRuntime-MIT.txt",
    "tools/shader_compiler/licenses/dxc/LICENSE.TXT",
    "tools/shader_compiler/licenses/dxc/ThirdPartyNotices.txt",
    "tools/rgp/libamdrdf/LICENSE",
    "tools/rgp/libamdrdf/imported/cli11/LICENSE",
    "tools/rgp/libamdrdf/imported/json/LICENSE",
    "tools/rgp/libamdrdf/imported/zstd/LICENSE",
    "models/fsr4_int8/reference/LICENSE.md",
    "models/fsr4_int8/resident_params/LICENSE.md",
    "tests/data/LICENSE-FidelityFX-MIT.md",
    "tests/data/README.md",
)

SOURCE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hlsl", ".py", ".cmake", ".bat", ".rc"
}

# These trees carry their own upstream notices and are not APUSR-authored source.
UPSTREAM_SOURCE_PREFIXES = (
    "third_party/",
    "tools/rgp/libamdrdf/",
    "models/fsr4_int8/reference/",
    "models/fsr4_int8/resident_params/",
    "src/framegen/ffx_compat/abi/",
    "src/framegen/ffx_dx12/antilag2/",
    "src/framegen/ffx_dx12/generated/",
    "src/framegen/ffx_dx12/shaders/",
)

# Binary material is intentionally limited to audited/fenced locations.
ALLOWED_BINARY_PREFIXES = (
    "models/fsr4_int8/reference/",
    "models/fsr4_int8/resident_params/",
    "third_party/detours/lib/x64/",
    "tools/shader_compiler/windows/bin/",
    "tools/shader_compiler/linux/bin/",
    "tests/data/",
    "tools/rgp/libamdrdf/",
)
BINARY_SUFFIXES = {".dll", ".exe", ".lib", ".bin", ".dat", ".onnx", ".xclbin", ".so", ".a", ".spv", ".pack", ".rdf"}


def fail(message: str) -> None:
    raise SystemExit(f"repository licensing contract failed: {message}")


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8", errors="strict")


def is_generated_build_path(rel: str) -> bool:
    first = rel.split("/", 1)[0]
    return first.startswith("build") or first in {"out", ".cache"}


def main() -> int:
    for rel in REQUIRED_FILES:
        if not (ROOT / rel).is_file():
            fail(f"missing required licensing/provenance file: {rel}")

    license_text = read("LICENSE")
    if "Apache License" not in license_text or "Version 2.0" not in license_text:
        fail("root LICENSE is not Apache License 2.0")

    dco = read("DCO")
    if "Developer Certificate of Origin" not in dco or "Version 1.1" not in dco:
        fail("root DCO is not DCO 1.1")
    if "changing it is not allowed" not in dco:
        fail("DCO verbatim-copy notice is missing")

    contributing = read("CONTRIBUTING.md")
    if "git commit -s" not in contributing or "does not require a\nContributor License Agreement" not in contributing:
        fail("CONTRIBUTING.md no longer states the DCO/no-CLA policy")

    notices = read("THIRD_PARTY_NOTICES.md")
    for required_phrase in (
        "FidelityFX SDK 2.0.0",
        "Microsoft Detours 4.0.1",
        "DirectX Shader Compiler (DXC)",
        "AMD Radeon Data File (RDF)",
    ):
        if required_phrase not in notices:
            fail(f"THIRD_PARTY_NOTICES.md lost component notice: {required_phrase}")

    ffx_license = (ROOT / "third_party/ffx_sdk/LICENSE.md").read_bytes()
    if b"Permission is hereby granted, free of charge" not in ffx_license:
        fail("FidelityFX snapshot license is not recorded as MIT")
    for rel in ("models/fsr4_int8/reference/LICENSE.md", "models/fsr4_int8/resident_params/LICENSE.md", "tests/data/LICENSE-FidelityFX-MIT.md", "LICENSES/FidelityFX-MIT.txt"):
        if (ROOT / rel).read_bytes() != ffx_license:
            fail(f"{rel} must remain an exact copy of the audited FidelityFX snapshot license")
    if (ROOT / "LICENSES/Detours-MIT.txt").read_bytes() != (ROOT / "third_party/detours/LICENSE.md").read_bytes():
        fail("LICENSES/Detours-MIT.txt must remain an exact copy of the vendored Detours license")
    if (ROOT / "LICENSES/Apache-2.0.txt").read_bytes() != (ROOT / "LICENSE").read_bytes():
        fail("LICENSES/Apache-2.0.txt must remain an exact copy of the root Apache-2.0 license")
    winpix_license = read("LICENSES/WinPixEventRuntime-MIT.txt")
    if "Copyright (c) Microsoft Corporation." not in winpix_license or "MIT License" not in winpix_license:
        fail("WinPixEventRuntime MIT license copy is missing or malformed")
    cmake = read("CMakeLists.txt")
    for required_cmake_phrase in (
        "APUSR_WINPIX_EVENT_RUNTIME_LICENSE_FILE",
        "LICENSE-WinPixEventRuntime-MIT.txt",
        "APUSR_ENABLE_GPU_DEBUG_MARKERS AND APUSR_WINPIX_EVENT_RUNTIME_DLL",
        "APUSR will not redistribute an externally supplied runtime without its provenance notice",
    ):
        if required_cmake_phrase not in cmake:
            fail(f"WinPixEventRuntime redistribution guard lost CMake contract: {required_cmake_phrase}")

    forbidden = []
    for p in ROOT.rglob("*"):
        if not p.is_file() or p.stat().st_size > 4_000_000:
            continue
        rel = p.relative_to(ROOT).as_posix()
        if is_generated_build_path(rel):
            continue
        try:
            text = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        if ("Apache-2.0 WITH " + "LLVM-exception") in text:
            forbidden.append(rel)
    if forbidden:
        fail("obsolete LLVM exception remains in APUSR licensing: " + ", ".join(forbidden))

    missing_source_notice = []
    for p in ROOT.rglob("*"):
        if not p.is_file() or p.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        rel = p.relative_to(ROOT).as_posix()
        if is_generated_build_path(rel):
            continue
        if any(rel.startswith(prefix) for prefix in UPSTREAM_SOURCE_PREFIXES):
            continue
        try:
            text = p.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        if "SPDX-License-Identifier:" not in text and "Permission is hereby granted" not in text:
            missing_source_notice.append(rel)
    if missing_source_notice:
        fail("source files without a license marker: " + ", ".join(missing_source_notice))

    unexpected_binaries = []
    for p in ROOT.rglob("*"):
        if not p.is_file() or p.suffix.lower() not in BINARY_SUFFIXES:
            continue
        rel = p.relative_to(ROOT).as_posix()
        if is_generated_build_path(rel):
            continue
        if not any(rel.startswith(prefix) for prefix in ALLOWED_BINARY_PREFIXES):
            unexpected_binaries.append(rel)
    if unexpected_binaries:
        fail("binary artifacts appeared outside audited locations: " + ", ".join(unexpected_binaries))

    if (ROOT / "models/fsr4_int8/reference/ffx_v07_i8").exists():
        fail("obsolete private reference-version path ffx_v07_i8 returned")
    if not (ROOT / "models/fsr4_int8/reference/fsr4_4_0_2_int8").is_dir():
        fail("versioned upstream FSR4 reference directory is missing")

    print("APUSR repository licensing/provenance contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
