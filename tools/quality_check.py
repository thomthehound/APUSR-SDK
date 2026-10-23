#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Build and run APUSR's behavior/regression suite as the repository quality gate."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str], *, timeout: int) -> None:
    print("[RUN] " + " ".join(command), flush=True)
    env = os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    subprocess.run(command, cwd=REPO_ROOT, check=True, env=env, timeout=timeout)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=REPO_ROOT / "build_quality")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--reuse-build", action="store_true")
    parser.add_argument("--configure-timeout", type=int, default=120)
    parser.add_argument("--build-timeout", type=int, default=240)
    parser.add_argument("--test-timeout", type=int, default=180)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.reuse_build and args.build_dir.exists():
        shutil.rmtree(args.build_dir)

    run(
        [
            "cmake",
            "-S", str(REPO_ROOT),
            "-B", str(args.build_dir),
            "-G", args.generator,
            f"-DCMAKE_BUILD_TYPE={args.config}",
            "-DAPUSR_BUILD_TESTS=ON",
            "-DAPUSR_BUILD_FRONTENDS=ON",
            "-DAPUSR_ENABLE_XRT_BACKEND=OFF",
        ],
        timeout=args.configure_timeout,
    )
    run(
        ["cmake", "--build", str(args.build_dir), "--target", "apusr_quality_tests"],
        timeout=args.build_timeout,
    )
    run(
        [
            "ctest",
            "--test-dir", str(args.build_dir),
            "--output-on-failure",
            "-L", "quality",
        ],
        timeout=args.test_timeout,
    )

    print("APUSR quality behavior/regression suite passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
