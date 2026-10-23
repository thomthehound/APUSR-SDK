#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Verify DXIL shader source fingerprints across Git checkout line endings."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import compile_dx12_shaders as shaders  # noqa: E402


def main() -> int:
    header = (
        ROOT / "src/frontends/ffx_fsr4_dx12/apusr_fsr4_dx12_shader_blobs.h"
    ).read_text(encoding="utf-8")

    with tempfile.TemporaryDirectory(prefix="apusr_shader_crlf_") as directory:
        alternate = Path(directory).resolve()
        closures = {
            path.relative_to(ROOT)
            for job in shaders.SHADERS
            for path in shaders.shader_source_closure(ROOT, job)
        }
        for relative in closures:
            destination = alternate / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            source = (ROOT / relative).read_bytes().replace(b"\r\n", b"\n")
            destination.write_bytes(source.replace(b"\n", b"\r\n"))

        for job in shaders.SHADERS:
            expected = shaders.metadata_hash(header, "SOURCE", job)
            assert expected is not None, job.name
            assert shaders.shader_source_fingerprint(ROOT, job) == expected, job.name
            assert shaders.shader_source_fingerprint(alternate, job) == expected, job.name

        # Genuine source changes must still invalidate the selected shader.
        job = shaders.SHADERS[0]
        changed = alternate / job.source
        changed.write_bytes(changed.read_bytes() + b"\r\n// fingerprint regression\r\n")
        assert shaders.shader_source_fingerprint(alternate, job) != shaders.metadata_hash(
            header, "SOURCE", job
        )

    print("DXIL shader fingerprint line-ending contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
