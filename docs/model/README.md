<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Model and parameter documentation

The product GPU path implements an FSR 4.0.2-derived INT8 model on D3D12 and Vulkan. The runtime selects the model permutation, quality preset, capacity, parameter epoch, passes, and scratch resources. AIE/NPU documents describe experiments, not production execution.

## GPU model and runtime

- [Model and comparative shader analysis](models_fsr4.md)
- [Reference baseline](fsr4_int8_reference_baseline.md), [runner](fsr4_reference_runner.md), [import](fsr4_reference_import.md)
- [Parameter provenance](fsr4_parameter_provenance.md), [usage](fsr4_parameter_usage_map.md), [packing](apusr_parameter_packing_plan.md)
- [D3D12 backend](fsr4_reference_dx12_backend.md), [descriptor plan](fsr4_reference_dx12_descriptor_plan.md)
- [DP4A/tiled fusion research](fsr4_int8_dp4a_tiled_fusion_plan.md)

## Experimental NPU partitioning

- [Middle-core stage ledger](middle_core_stage_ledger.md)
- [Stage placement](middle_core_stage_placement.md)
- [Resident primitives](middle_core_resident_primitives.md)
- [Model epoch contract](model_epoch_runtime_contract.md)

Raw model extraction workspaces are not distributed with APUSR. Redistributable generated model material and licenses are under `models/fsr4_int8/`.
