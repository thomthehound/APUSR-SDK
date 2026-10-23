<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Repository hygiene policy

The repository should describe and build the current project, not preserve a chronological transcript of how it was discovered.

## Keep

- active runtime/backend/frontend source;
- tests that protect current behavior or non-obvious invariants;
- tools required to generate, inspect, package, or profile current products;
- durable architecture and performance documentation;
- reproducible experiments that remain technically relevant;
- third-party source/reference material only when its provenance and redistribution terms are clear.

## Remove or quarantine

- private build/archive numbering;
- chronological development notes after their durable lessons are documented;
- game-specific debugging narratives after their durable lesson is extracted;
- stale hypotheses and superseded decision logs;
- generated profiler output and local capture artifacts;
- retired implementation modes and their dedicated tests;
- duplicate third-party working trees or normalization/export workspaces;
- binary/reference documentation that does not need to be redistributed with the project.

## Documentation rule

When an experiment produces a durable result, update the owning architecture/performance document. Do not add a new permanent document whose only organizing principle is "what changed in this build."

## Diagnostics rule

A diagnostic tool should exist because it answers a current engineering question and should be named for that function. Diagnostics remain opt-in and must not become a product execution mode by accident.
