#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Validate the generated APUSR FSR4 real-boundary manifest."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any


EXPECTED_MODES = {"native", "quality", "balanced", "performance", "drs", "ultraperf"}
EXPECTED_RESOLUTIONS = {"1080", "2160", "4320"}


def fail(errors: list[str], message: str) -> None:
    errors.append(message)


def tensor_shape(tensor: dict[str, Any]) -> list[int]:
    value = tensor.get("logical_size")
    return value if isinstance(value, list) else []


def validate_manifest(path: pathlib.Path) -> list[str]:
    errors: list[str] = []
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("format") != "apusr_fsr4_real_boundary_manifest_v0":
        fail(errors, "unexpected FSR4 real-boundary manifest format")
    boundary = manifest.get("boundary", {})
    if boundary.get("layout") != "NHWC" or boundary.get("dtype") != "int8" or boundary.get("channels") != 16:
        fail(errors, "boundary contract must stay NHWC int8 C16")
    if boundary.get("token_bytes") != 256:
        fail(errors, "boundary token size must stay 256 bytes")

    contracts = manifest.get("contracts", [])
    if len(contracts) != len(EXPECTED_MODES) * len(EXPECTED_RESOLUTIONS):
        fail(errors, "manifest must contain all mode/resolution contracts")
        return errors

    seen = set()
    for contract in contracts:
        mode = contract.get("mode")
        resolution = contract.get("resolution")
        seen.add((mode, resolution))
        if mode not in EXPECTED_MODES:
            fail(errors, f"unexpected mode {mode!r}")
        if resolution not in EXPECTED_RESOLUTIONS:
            fail(errors, f"unexpected resolution {resolution!r}")

        ingress = contract.get("ingress", {})
        egress = contract.get("egress", {})
        ingress_producer = ingress.get("producer", {})
        ingress_consumer = ingress.get("consumer", {})
        egress_producer = egress.get("producer", {})
        egress_consumer = egress.get("consumer", {})

        if ingress.get("producer_pass") != 0 or ingress.get("consumer_pass") != 1:
            fail(errors, f"{mode}__{resolution} ingress must be pass0 -> pass1")
        if egress.get("producer_pass") != 12 or egress.get("consumer_pass") != 13:
            fail(errors, f"{mode}__{resolution} egress must be pass12 -> pass13")
        if tensor_shape(ingress_producer) != tensor_shape(ingress_consumer):
            fail(errors, f"{mode}__{resolution} ingress producer/consumer shapes differ")
        if tensor_shape(egress_producer) != tensor_shape(egress_consumer):
            fail(errors, f"{mode}__{resolution} egress producer/consumer shapes differ")
        if ingress_producer.get("scale_domain") != ingress_consumer.get("scale_domain"):
            fail(errors, f"{mode}__{resolution} ingress scale domains differ")
        if egress_producer.get("scale_domain") != egress_consumer.get("scale_domain"):
            fail(errors, f"{mode}__{resolution} egress scale domains differ")
        if tensor_shape(ingress_producer)[-1:] != [16] or tensor_shape(egress_producer)[-1:] != [16]:
            fail(errors, f"{mode}__{resolution} real seam is not C16")
        if ingress_producer.get("dtype") != "int8" or egress_producer.get("dtype") != "int8":
            fail(errors, f"{mode}__{resolution} seam dtype must be int8")
        if ingress_producer.get("layout") != "NHWC" or egress_producer.get("layout") != "NHWC":
            fail(errors, f"{mode}__{resolution} seam layout must be NHWC")

    expected = {(mode, resolution) for mode in EXPECTED_MODES for resolution in EXPECTED_RESOLUTIONS}
    missing = expected - seen
    if missing:
        fail(errors, f"manifest is missing contracts: {sorted(missing)}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=pathlib.Path)
    args = parser.parse_args()

    errors = validate_manifest(args.manifest)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print(f"FSR4 real-boundary manifest validated: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
