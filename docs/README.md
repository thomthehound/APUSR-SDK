<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# APUSR documentation

APUSR documentation is organized around current architecture, supported behavior, reproducible research, and unresolved engineering work.

## Start here

- [Project status](status.md) — what works, what is experimental, and what is planned.
- [Architecture](architecture.md) — runtime layering and ownership boundaries.
- [Building](building.md) — toolchain, targets, generated shader products, and optional components.
- [Backend portability](runtime/backend_portability.md) — how one model graph maps to D3D12, Vulkan, and future bridges.
- [Host integration strategy](integration/host_integration_strategy.md) — FidelityFX, NGX/DLSS, Streamline, host capture, and queue ownership.
- [Performance](performance.md) — optimization principles and active performance directions.
- [Profiling](profiling.md) — structured CPU/GPU instrumentation and analysis tools.
- [Testing](testing.md) — regression philosophy and quality suite.
- [Licensing and provenance](development/licensing.md) — ownership, third-party imports, redistribution, and contribution policy.
- [Engineering backlog](engineering_backlog.md) — unresolved technical work.
- [Roadmap](roadmap.md) — project-level priorities.

## Runtime and integration

- [Interface](runtime/interface.md)
- [Shader ownership](runtime/shader_ownership.md)
- [Streamline feature compatibility](integration/streamline_feature_compatibility.md)
- [NGX/DLSS frontend](integration/ngx_dlss_frontend.md)
- [DLSS/Streamline frontend](integration/dlss_streamline_frontend.md)
- [Vulkan host/layer strategy](integration/vulkan_layer_strategy.md)
- [Vulkan frame-generation architecture](runtime/vulkan_framegen_architecture.md)
- [Frame generation](runtime/frame_generation.md)

## FSR4 INT8 model/runtime

- [FSR4 model overview](model/models_fsr4.md)
- [Reference baseline](model/fsr4_int8_reference_baseline.md)
- [Reference import](model/fsr4_reference_import.md)
- [Reference runner](model/fsr4_reference_runner.md)
- [D3D12 reference recorder](model/fsr4_reference_dx12_backend.md)
- [DX12 descriptor plan](model/fsr4_reference_dx12_descriptor_plan.md)
- [DX12 root-signature plan](model/fsr4_reference_dx12_root_signature_plan.md)
- [DX12 shader provider](model/fsr4_reference_dx12_shader_provider.md)
- [DX12 shader/PSO plan](model/fsr4_reference_dx12_shader_pso_plan.md)
- [DP4A tuned plan](model/fsr4_int8_dp4a_tuned_plan.md)
- [Tiled/fusion research](model/fsr4_int8_dp4a_tiled_fusion_plan.md)

## NPU and future acceleration research

These documents describe experimental directions and do not represent the current GPU execution path. Their NPU model-epoch and frame-slot contracts apply to the experimental XRT/MLIR-AIE backend, not to D3D12/Vulkan inference.

- [NPU frame-slot transport](runtime/transport.md)
- [Experimental model-epoch contract](model/model_epoch_runtime_contract.md)

- [XRT notes](research/xrt_notes.md)
- [MLIR-AIE notes](research/mlir_aie_notes.md)
- [AIE programming references](references/aie_ml_programming_references.md)
- [IRON/AIR lessons](references/iron_air_lessons.md)
- [Middle-core stage ledger](model/middle_core_stage_ledger.md)
- [Middle-core stage placement](model/middle_core_stage_placement.md)
- [Middle-core resident primitives](model/middle_core_resident_primitives.md)

## Documentation rules

Public documentation should describe durable facts that help a user or contributor understand, build, measure, or extend APUSR. Avoid:

- private build identifiers or unpublished release numbering;
- narrative accounts of development sessions or debugging chronology;
- application-specific anecdotes unless they define a supported integration contract;
- benchmark claims without a reproducible workload definition;
- abandoned hypotheses presented as current design rationale;
- resolved TODOs retained as archaeology.

Reproducible experiments may remain under `experiments/` when the setup, result, and engineering conclusion are still useful. The surrounding documentation should state the conclusion directly rather than requiring readers to reconstruct the project's development path.
