<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Repository hygiene

Documentation and distributable source describe the current implementation, reproducible results, and outstanding engineering work.

## Keep

- Active runtime/backend/frontend interfaces and behavior.
- Tests that detect current regressions and tools required to build or profile the product.
- Measured performance findings with workload, hardware, and configuration context.
- Reproducible experiments under `experiments/` with an explicit outcome.
- Imported source and generated products accompanied by provenance and redistribution notices.

## Remove or isolate

- Private archive/build identifiers, local captures, and generated build output.
- Retired code paths and duplicate extraction or generator workspaces.
- Debugging chronology that adds no durable contract or reproducible result.
- Hypotheses presented as implemented runtime behavior.

When findings affect production invariants, update the owning architecture, backend, or performance document. Diagnostics are opt-in and must not alter the disabled product path.
