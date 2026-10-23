#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Copy a FidelityFX SDK file into APUSR's third-party quarantine.

This helper intentionally does only one thing: preserve the source path under
third_party/ffx_sdk/imported/ so temporary SDK imports stay easy to audit and
remove. It does not edit the ledgers; the human doing the import should record
why the file is needed and what APUSR-owned file is expected to replace it.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-root", required=True, type=pathlib.Path)
    parser.add_argument("--relative-path", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    repo_root = pathlib.Path(__file__).resolve().parents[1]
    source = (args.sdk_root / args.relative_path).resolve()
    destination = repo_root / "third_party" / "ffx_sdk" / "imported" / args.relative_path

    if not source.is_file():
        raise FileNotFoundError(source)

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    print(destination.relative_to(repo_root))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
