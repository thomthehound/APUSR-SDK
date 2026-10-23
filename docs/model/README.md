<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# FSR4 model/parameter bridge

This folder records the FSR4 INT8 model knowledge APUSR needs after the
`ffxnpu` cleanup. It is intentionally not a copy of the raw extraction workspace.

Read these in order:

1. `fsr4_parameter_provenance.md` — where the model data comes from and which
   artifacts APUSR keeps.
2. `fsr4_parameter_usage_map.md` — how the extracted parameters map onto the
   GPU bookends and the NPU middle core.
3. `middle_core_stage_ledger.md` — the canonical logical-stage to physical-row
   and resident-parameter planning contract.
4. `middle_core_stage_placement.md` — the first-order fusion/fission and
   2col/4col connectivity plan.
5. `middle_core_resident_primitives.md` — the reusable resident-primitive record
   workflow and current proof-instance notes.
6. `apusr_parameter_packing_plan.md` — the current resident arena contract and
   the rules for future APUSR-native packing.
7. `model_epoch_runtime_contract.md` — how resident parameters are programmed
   outside the frame-hot path.

The shader files stay in APUSR as active reference material. The raw/logical
extraction bundles stay out of APUSR.
