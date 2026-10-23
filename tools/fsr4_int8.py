#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""APUSR FSR4 product-tooling entrypoint.

Raw FSR4 INT8 extraction, normalization, and schema exploration live in the
ffxnpu/ffxint8 workspace. APUSR only exposes tools that check or package the
product artifacts kept in this repo.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


def _tool(path_from_repo_root: str) -> Path:
    path = (REPO_ROOT / path_from_repo_root).resolve()
    if not path.exists():
        raise FileNotFoundError(f"missing tool: {path}")
    return path


def _strip_double_dash(args: list[str]) -> list[str]:
    return args[1:] if args and args[0] == "--" else args


def _run(tool: Path, args: list[str]) -> int:
    return subprocess.call([sys.executable, str(tool), *args], cwd=REPO_ROOT)


def _forward(tool_path: str, args: list[str]) -> int:
    return _run(_tool(tool_path), args)


def _cmd_resident_param_check(args: list[str]) -> int:
    return _forward("tools/analysis/resident_parameter_set_check.py", args)


def _cmd_inspect_params(args: list[str]) -> int:
    return _forward("tools/model/inspect_fsr4_params.py", args)


def _cmd_validate_resident_params(args: list[str]) -> int:
    return _forward("tools/model/validate_resident_params.py", args)


def _cmd_pack_params(args: list[str]) -> int:
    return _forward("tools/model/pack_apusr_params.py", args)


def _cmd_middle_core_aie_probe(args: list[str]) -> int:
    return _forward("tools/analysis/middle_core_aie_compile_probe.py", args)



def _cmd_quality_check(args: list[str]) -> int:
    return _forward("tools/quality_check.py", args)


def main(argv: list[str] | None = None) -> int:
    commands: dict[str, tuple[str, Callable[[list[str]], int]]] = {
        "resident-param-check": ("validate the checked-in resident parameter arena set", _cmd_resident_param_check),
        "inspect-params": ("summarize FSR4 boundary and resident parameter artifacts", _cmd_inspect_params),
        "validate-resident-params": ("validate resident parameter artifacts and slice hashes", _cmd_validate_resident_params),
        "pack-params": ("stage resident parameter artifacts for package/runtime experiments", _cmd_pack_params),
        "middle-core-aie-probe": ("compile product middle-core sources with Peano AIE2/AIE2P targets", _cmd_middle_core_aie_probe),
        "quality-check": ("run repository quality checks", _cmd_quality_check),
    }

    parser = argparse.ArgumentParser(prog="tools/fsr4_int8.py", description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name, (help_text, _handler) in commands.items():
        command_parser = subparsers.add_parser(name, help=help_text)
        command_parser.add_argument("tool_args", nargs=argparse.REMAINDER)

    parsed = parser.parse_args(argv)
    args = _strip_double_dash(list(parsed.tool_args or []))
    return commands[parsed.command][1](args)


if __name__ == "__main__":
    raise SystemExit(main())
