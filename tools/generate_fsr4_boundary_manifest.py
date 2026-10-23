#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Generate the APUSR FSR4 real-boundary metadata from FSR4 logical models."""

from __future__ import annotations

import argparse
import json
import pathlib
from typing import Any


MODE_ORDER = {
    "native": 0,
    "quality": 1,
    "balanced": 2,
    "performance": 3,
    "drs": 4,
    "ultraperf": 5,
}


def read_json(path: pathlib.Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def stage_by_pass(stages: list[dict[str, Any]], pass_index: int) -> dict[str, Any]:
    for stage in stages:
        if stage.get("pass_index") == pass_index:
            return stage
    raise ValueError(f"missing pass_index {pass_index}")


def tensor_ref(stage: dict[str, Any], group: str, name: str) -> dict[str, Any]:
    ref = stage[group][name]
    return {
        "tensor_id": ref["tensor_id"],
        "dtype": ref["dtype"],
        "layout": ref["layout"],
        "logical_size": ref["logical_size"],
        "quantization_scale": ref.get("quantization_scale"),
        "scale_domain": ref.get("scale_domain"),
    }


def source_model_path(path: pathlib.Path) -> str:
    root = repo_root()
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def make_contract(path: pathlib.Path) -> dict[str, Any]:
    model = read_json(path)
    stages = model["stages"]
    pass0 = stage_by_pass(stages, 0)
    pass1 = stage_by_pass(stages, 1)
    pass12 = stage_by_pass(stages, 12)
    pass13 = stage_by_pass(stages, 13)

    ingress_producer = tensor_ref(pass0, "activations", "output")
    ingress_consumer = tensor_ref(pass1, "activations", "input")
    egress_producer = tensor_ref(pass12, "activations", "output")
    egress_consumer = tensor_ref(pass13, "activations", "input")
    model_input = tensor_ref(pass0, "boundaries", "input")
    model_output = tensor_ref(pass13, "boundaries", "output")

    if ingress_producer["logical_size"] != ingress_consumer["logical_size"]:
        raise ValueError(f"ingress producer/consumer shape mismatch in {path}")
    if ingress_producer["scale_domain"] != ingress_consumer["scale_domain"]:
        raise ValueError(f"ingress producer/consumer scale mismatch in {path}")
    if egress_producer["logical_size"] != egress_consumer["logical_size"]:
        raise ValueError(f"egress producer/consumer shape mismatch in {path}")
    if egress_producer["scale_domain"] != egress_consumer["scale_domain"]:
        raise ValueError(f"egress producer/consumer scale mismatch in {path}")
    if ingress_producer["logical_size"][2] != 16 or egress_producer["logical_size"][2] != 16:
        raise ValueError(f"real seam is not C16 in {path}")

    return {
        "mode": model["mode"],
        "resolution": model["resolution"],
        "bundle_name": f"{model['mode']}__{model['resolution']}",
        "source_logical_model": source_model_path(path),
        "model_input": model_input,
        "model_output": model_output,
        "ingress": {
            "producer_pass": 0,
            "producer": ingress_producer,
            "consumer_pass": 1,
            "consumer": ingress_consumer,
        },
        "egress": {
            "producer_pass": 12,
            "producer": egress_producer,
            "consumer_pass": 13,
            "consumer": egress_consumer,
        },
    }


def contract_sort_key(contract: dict[str, Any]) -> tuple[int, int]:
    return (MODE_ORDER.get(contract["mode"], 99), int(contract["resolution"]))


def repo_root() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-data-dir",
        type=pathlib.Path,
        default=None,
        help="FSR4 INT8 model data root. Defaults to models/fsr4_int8 for legacy local checkouts.",
    )
    parser.add_argument("--output", type=pathlib.Path, default=repo_root() / "models/fsr4_int8/real_boundary_manifest_v0.json")
    args = parser.parse_args()

    data_root = args.model_data_dir if args.model_data_dir is not None else (repo_root() / "models/fsr4_int8")
    logical_root = data_root / "int8" / "logical" if (data_root / "int8" / "logical").is_dir() else data_root / "logical"
    logical_models = sorted(logical_root.glob("*__/logical_model.json"))
    if not logical_models:
        logical_models = sorted(logical_root.glob("*/logical_model.json"))
    if not logical_models:
        raise SystemExit(f"no logical_model.json files under {logical_root}")

    contracts = sorted((make_contract(path) for path in logical_models), key=contract_sort_key)
    output = {
        "schema_version": 1,
        "format": "apusr_fsr4_real_boundary_manifest_v0",
        "source_note": "Generated from external FSR4 logical_model.json files. This records tensor identity and scale-domain metadata for the real C16 GPU/NPU seam; generated shader headers and resident parameter arenas are the repo-local product artifacts.",
        "rule": "FFX defines the semantics. APUSR defines the physical contract. GPU shaders serve that contract. NPU kernels consume that contract.",
        "boundary": {
            "layout": "NHWC",
            "dtype": "int8",
            "channels": 16,
            "token_bytes": 256,
            "physical_row_rule": "padded_width=alignUp(logical_width,16); row_pitch_bytes=padded_width*16",
        },
        "contracts": contracts,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(output, indent=2, sort_keys=False) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
