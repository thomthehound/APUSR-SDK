# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Compatibility wrapper for the DX12 FSR4 reference shader-resource manifest.

The implementation lives in generate_fsr4_reference_shader_resource_manifest.py
so the manifest schema is shared with future backends instead of being baked into
DX12-only tooling.
"""

from __future__ import annotations

from generate_fsr4_reference_shader_resource_manifest import main

if __name__ == "__main__":
    raise SystemExit(main())
