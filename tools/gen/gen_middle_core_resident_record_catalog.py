#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the middle-core resident primitive record catalog."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.model.resident_arena_layout import resident_record_catalog_payload  # noqa: E402

DEFAULT_OUTPUT = REPO_ROOT / "models/fsr4_middle_core_resident_records_v0.json"


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as file:
        json.dump(payload, file, indent=2, sort_keys=True)
        file.write("\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true", help="fail if the checked-in catalog is not current")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    payload = resident_record_catalog_payload()
    if args.check:
        current = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
        expected = json.dumps(payload, indent=2, sort_keys=True) + "\n"
        if current != expected:
            print(f"resident record catalog is stale: {args.output}")
            return 1
        print(f"resident record catalog is current: {args.output}")
        return 0
    write_json(args.output, payload)
    print(f"generated resident record catalog: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
