# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Audit signed packed-DOT4 call count and direct accumulator-chain depth in DXIL."""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import stat
import subprocess
from dataclasses import asdict, dataclass
from pathlib import Path

DOT4_CALL_RE = re.compile(
    r"^\s*(%\d+) = call i32 @dx\.op\.dot4AddPacked\.i32\(i32 163, i32 ([^,]+),"
)


@dataclass(frozen=True)
class Dp4aAudit:
    path: str
    byte_size: int
    signed_dp4a_calls: int
    max_direct_dp4a_chain: int


def default_dxc(repo_root: Path) -> Path:
    if platform.system().lower() == "windows":
        return repo_root / "tools/shader_compiler/windows/bin/dxc.exe"
    return repo_root / "tools/shader_compiler/linux/bin/dxc"


def prepare_dxc(dxc: Path) -> dict[str, str]:
    if not dxc.is_file():
        raise SystemExit(f"DXC not found: {dxc}")
    env = os.environ.copy()
    if platform.system().lower() == "windows":
        env["PATH"] = str(dxc.parent) + (os.pathsep + env["PATH"] if env.get("PATH") else "")
    else:
        dxc.chmod(dxc.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        env["LD_LIBRARY_PATH"] = str(dxc.parent) + (
            os.pathsep + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else ""
        )
    return env


def dump_dxil(dxc: Path, dxil: Path, env: dict[str, str]) -> str:
    result = subprocess.run(
        [str(dxc), "-dumpbin", str(dxil)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    return result.stdout


def audit_dump(text: str, path: Path, byte_size: int) -> Dp4aAudit:
    depths: dict[str, int] = {}
    calls = 0
    max_depth = 0
    for line in text.splitlines():
        match = DOT4_CALL_RE.match(line)
        if match is None:
            continue
        result_name, accumulator = match.groups()
        accumulator = accumulator.strip()
        depth = depths.get(accumulator, 0) + 1
        depths[result_name] = depth
        calls += 1
        max_depth = max(max_depth, depth)
    return Dp4aAudit(
        path=str(path),
        byte_size=byte_size,
        signed_dp4a_calls=calls,
        max_direct_dp4a_chain=max_depth,
    )


def audit_file(dxc: Path, dxil: Path, env: dict[str, str]) -> Dp4aAudit:
    if not dxil.is_file():
        raise SystemExit(f"DXIL file not found: {dxil}")
    return audit_dump(dump_dxil(dxc, dxil, env), dxil, dxil.stat().st_size)


def parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dxil", nargs="+", type=Path)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--dxc", type=Path, default=None)
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON instead of a compact table.")
    parser.add_argument("--require-calls", type=int, default=None)
    parser.add_argument("--require-max-chain", type=int, default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    repo_root = args.repo_root.resolve()
    dxc = args.dxc.resolve() if args.dxc is not None else default_dxc(repo_root)
    env = prepare_dxc(dxc)
    audits = [audit_file(dxc, path.resolve(), env) for path in args.dxil]

    if args.require_calls is not None:
        bad = [audit for audit in audits if audit.signed_dp4a_calls != args.require_calls]
        if bad:
            raise SystemExit(
                "signed DP4A call-count mismatch: "
                + ", ".join(f"{audit.path}={audit.signed_dp4a_calls}" for audit in bad)
            )
    if args.require_max_chain is not None:
        bad = [audit for audit in audits if audit.max_direct_dp4a_chain != args.require_max_chain]
        if bad:
            raise SystemExit(
                "DP4A chain-depth mismatch: "
                + ", ".join(f"{audit.path}={audit.max_direct_dp4a_chain}" for audit in bad)
            )

    if args.json:
        print(json.dumps([asdict(audit) for audit in audits], indent=2))
        return

    print("path\tbytes\tsigned_dp4a_calls\tmax_direct_dp4a_chain")
    for audit in audits:
        print(
            f"{audit.path}\t{audit.byte_size}\t{audit.signed_dp4a_calls}\t{audit.max_direct_dp4a_chain}"
        )


if __name__ == "__main__":
    main()
