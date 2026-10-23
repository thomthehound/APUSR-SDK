#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Stage APUSR resident parameter artifacts for package/runtime experiments."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE = REPO_ROOT / "models" / "fsr4_int8" / "resident_params"
DEFAULT_INDEX_NAME = "stage1_12_full_arena__am020_4x4_base.json"
VALIDATOR = REPO_ROOT / "tools" / "model" / "validate_resident_params.py"


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as file:
        return json.load(file)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")


def copy_resident_tree(source: Path, output: Path, *, overwrite: bool) -> None:
    if output.exists():
        if not overwrite:
            raise SystemExit(f"output exists: {output}")
        shutil.rmtree(output)
    ignore = shutil.ignore_patterns("__pycache__", ".pytest_cache", ".ruff_cache")
    shutil.copytree(source, output, ignore=ignore)


def write_package_manifest(source: Path, output: Path, index_name: str) -> None:
    index = load_json(output / index_name)
    arenas = index.get("arenas") or []
    first_arena = arenas[0] if arenas else {}
    manifest = {
        "schema": "com.apusr.staged_resident_parameter_package.v1",
        "source": str(source),
        "index": index_name,
        "parameter_profile": index.get("parameter_profile"),
        "arena_count": index.get("arena_count"),
        "arena_bytes": index.get("arena_bytes", first_arena.get("arena_bytes")),
        "payload_bytes": index.get("payload_bytes", first_arena.get("payload_bytes")),
        "rule": "This is a staged copy of APUSR product resident artifacts, not a raw/logical FSR4 extraction bundle.",
    }
    write_json(output / "apusr_parameter_package.json", manifest)


def validate_output(output: Path, index_name: str) -> None:
    subprocess.run(
        [sys.executable, str(VALIDATOR), "--index", str(output / index_name), "--allow-boundary-mismatch"],
        cwd=REPO_ROOT,
        check=True,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE, help="resident_params source directory")
    parser.add_argument("--out", type=Path, required=True, help="output directory")
    parser.add_argument("--index-name", default=DEFAULT_INDEX_NAME)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--no-validate", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source = args.source.resolve()
    output = args.out.resolve()
    index_path = source / args.index_name
    if not index_path.is_file():
        raise SystemExit(f"missing resident index: {index_path}")

    copy_resident_tree(source, output, overwrite=args.overwrite)
    write_package_manifest(source, output, args.index_name)
    if not args.no_validate:
        validate_output(output, args.index_name)
    print(f"staged resident parameter package: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
