# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
"""Operator schemas for the normalized FSR4 INT8 logical model.

The schemas are deliberately narrow. They describe the SDK INT8 fused-op
surface we have extracted, not a general CNN framework. Keeping this layer
small lets the reference executor, case emitter, and future AIE packers agree
on exactly what a kernel must consume and produce.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import FrozenSet, Tuple


@dataclass(frozen=True)
class OperatorSchema:
    op: str
    parameter_args: FrozenSet[str]
    activation_args: FrozenSet[str]
    boundary_args: FrozenSet[str]
    required_scalar_args: FrozenSet[str]
    optional_scalar_args: FrozenSet[str]
    spatial_effect: str
    notes: str
    primitive_sequence: Tuple[str, ...]
    kernel_priority: int

    def allowed_scalar_args(self) -> FrozenSet[str]:
        return self.required_scalar_args | self.optional_scalar_args


@dataclass(frozen=True)
class KernelTarget:
    op: str
    first_kernel_slice: str
    first_correctness_test: str
    packing_needed: Tuple[str, ...]
    deferred_work: Tuple[str, ...]


SCHEMAS: dict[str, OperatorSchema] = {
    "Conv2D_k2s2b": OperatorSchema(
        op="Conv2D_k2s2b",
        parameter_args=frozenset({"weights", "bias"}),
        activation_args=frozenset({"output"}),
        boundary_args=frozenset({"input"}),
        required_scalar_args=frozenset(),
        optional_scalar_args=frozenset(),
        spatial_effect="down2x2_boundary_to_internal",
        notes="Boundary FP16 input to first INT8 internal activation stream.",
        primitive_sequence=("2x2 stride-2 convolution", "bias", "boundary quantize"),
        kernel_priority=3,
    ),
    "ConvNextBlock": OperatorSchema(
        op="ConvNextBlock",
        parameter_args=frozenset({"weights0", "weights1", "weights2", "bias0", "bias1", "bias2"}),
        activation_args=frozenset({"input", "output"}),
        boundary_args=frozenset(),
        required_scalar_args=frozenset({"rcprOutputScale0", "inputScale1", "rcprOutputScale1", "inputScale2"}),
        optional_scalar_args=frozenset(),
        spatial_effect="same_scale",
        notes="ConvNext-style same-scale block with explicit intermediate scale args.",
        primitive_sequence=("depthwise/spatial convolution", "pointwise expand", "activation", "pointwise contract"),
        kernel_priority=1,
    ),
    "FusedConv2D_k2s2b_QuantizedOutput": OperatorSchema(
        op="FusedConv2D_k2s2b_QuantizedOutput",
        parameter_args=frozenset({"weights", "bias"}),
        activation_args=frozenset({"input", "output"}),
        boundary_args=frozenset(),
        required_scalar_args=frozenset({"quantFactor0", "quantFactor1"}),
        optional_scalar_args=frozenset(),
        spatial_effect="down2x2_internal",
        notes="Internal stride-2 downsample block with quantized output.",
        primitive_sequence=("2x2 stride-2 convolution", "bias", "requantize"),
        kernel_priority=2,
    ),
    "FasterNetBlock": OperatorSchema(
        op="FasterNetBlock",
        parameter_args=frozenset({"weights0", "weights1", "weights2", "bias0", "bias1", "bias2"}),
        activation_args=frozenset({"input", "output"}),
        boundary_args=frozenset(),
        required_scalar_args=frozenset({"activationScaleFactor", "quantScale0", "quantScale1"}),
        optional_scalar_args=frozenset({"quantOutputScale0", "quantOutputScale1"}),
        spatial_effect="same_scale",
        notes="Partial spatial-mixing block. Some instances expose output scale args and some do not.",
        primitive_sequence=("partial 3x3 convolution", "pointwise expand", "activation", "pointwise contract", "residual add"),
        kernel_priority=1,
    ),
    "FNB_CT2D_ADD": OperatorSchema(
        op="FNB_CT2D_ADD",
        parameter_args=frozenset(
            {"weights0", "weights1", "weights2", "bias0", "bias1", "bias2", "ct2d_weights", "ct2d_bias"}
        ),
        activation_args=frozenset({"input", "inputAdd", "output"}),
        boundary_args=frozenset(),
        required_scalar_args=frozenset({"activationScaleFactor", "quantScale0", "quantScale1", "fnbOutputScale"}),
        optional_scalar_args=frozenset({"quantOutputScale0", "quantOutputScale1"}),
        spatial_effect="up2x2_with_skip_add",
        notes="FasterNet block plus CT2D/upscale and skip-add epilogue.",
        primitive_sequence=("FasterNet block", "2x2 CT2D", "skip add", "requantize"),
        kernel_priority=4,
    ),
    "CNB_CT2D": OperatorSchema(
        op="CNB_CT2D",
        parameter_args=frozenset(
            {"weights0", "weights1", "weights2", "bias0", "bias1", "bias2", "ct2d_weights", "ct2d_bias"}
        ),
        activation_args=frozenset({"input"}),
        boundary_args=frozenset({"output"}),
        required_scalar_args=frozenset(
            {"rcprOutputScale0", "inputScale1", "rcprOutputScale1", "inputScale2", "cnbOutputScale"}
        ),
        optional_scalar_args=frozenset(),
        spatial_effect="up2x2_internal_to_boundary",
        notes="Final ConvNext-style block plus CT2D/upscale to the boundary output.",
        primitive_sequence=("ConvNext block", "2x2 CT2D", "boundary dequantize/output"),
        kernel_priority=5,
    ),
    "pre_shader": OperatorSchema(
        op="pre_shader",
        parameter_args=frozenset(),
        activation_args=frozenset(),
        boundary_args=frozenset(),
        required_scalar_args=frozenset(),
        optional_scalar_args=frozenset(),
        spatial_effect="boundary_preprocess",
        notes="SDK pre shader is preserved in extraction but is not yet modeled as one fused operator.",
        primitive_sequence=("external preprocess",),
        kernel_priority=99,
    ),
    "post_shader": OperatorSchema(
        op="post_shader",
        parameter_args=frozenset(),
        activation_args=frozenset(),
        boundary_args=frozenset(),
        required_scalar_args=frozenset(),
        optional_scalar_args=frozenset(),
        spatial_effect="boundary_postprocess",
        notes="SDK post shader is preserved in extraction but is not yet modeled as one fused operator.",
        primitive_sequence=("external postprocess",),
        kernel_priority=99,
    ),
}


KERNEL_TARGETS: dict[str, KernelTarget] = {
    "ConvNextBlock": KernelTarget(
        op="ConvNextBlock",
        first_kernel_slice="pointwise expand/contract on one NHWC row tile, then add the spatial/depthwise leg",
        first_correctness_test="stage-local INT8 output compared against the CPU reference for passes 1, 2, and 12",
        packing_needed=("weights0 spatial/depthwise layout", "weights1 1x1 expand layout", "weights2 1x1 contract layout"),
        deferred_work=("full-frame streaming placement", "cross-stage fusion", "hardware-specific vector reordering"),
    ),
    "FasterNetBlock": KernelTarget(
        op="FasterNetBlock",
        first_kernel_slice="partial 3x3 spatial mix plus pointwise expand/contract for one row tile",
        first_correctness_test="stage-local INT8 output compared against the CPU reference for passes 4, 5, 7, 8, and 10",
        packing_needed=("partial-conv weights0 layout", "weights1 1x1 expand layout", "weights2 1x1 contract layout"),
        deferred_work=("residual epilogue tuning", "scale-domain fusion", "line-buffer scheduling"),
    ),
    "FusedConv2D_k2s2b_QuantizedOutput": KernelTarget(
        op="FusedConv2D_k2s2b_QuantizedOutput",
        first_kernel_slice="2x2 stride-2 internal downsample for one pair of input rows",
        first_correctness_test="stage-local INT8 output compared against the CPU reference for passes 3 and 6",
        packing_needed=("2x2 weights layout", "bias layout"),
        deferred_work=("full cadence adapter", "DMA row-pair staging"),
    ),
    "Conv2D_k2s2b": KernelTarget(
        op="Conv2D_k2s2b",
        first_kernel_slice="boundary FP16 to INT8 2x2 stride-2 for one pair of input rows",
        first_correctness_test="stage-local output compared against the CPU reference for pass 0",
        packing_needed=("2x2 FP16 weights layout", "FP16 bias layout"),
        deferred_work=("boundary format ownership", "GPU/NPU handoff contract"),
    ),
    "FNB_CT2D_ADD": KernelTarget(
        op="FNB_CT2D_ADD",
        first_kernel_slice="CT2D plus skip-add epilogue after FasterNetBlock semantics are stable",
        first_correctness_test="stage-local output and skip alignment compared against passes 9 and 11",
        packing_needed=("FasterNet weights", "CT2D weights", "skip-add scale metadata"),
        deferred_work=("memtile skip FIFO integration", "producer/consumer row skew validation"),
    ),
    "CNB_CT2D": KernelTarget(
        op="CNB_CT2D",
        first_kernel_slice="final CT2D boundary output after ConvNextBlock semantics are stable",
        first_correctness_test="stage-local boundary output compared against pass 13 reference",
        packing_needed=("ConvNext weights", "CT2D weights", "boundary output scale metadata"),
        deferred_work=("final output handoff", "post shader policy"),
    ),
}


def get_schema(op: str) -> OperatorSchema | None:
    return SCHEMAS.get(op)
