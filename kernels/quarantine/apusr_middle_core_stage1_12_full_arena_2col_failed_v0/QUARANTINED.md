<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Quarantined: full-arena two-column Stage1→12 graph

This graph is not a product target.

It was rejected because the public ABI was correctly frame-shaped, but the internal
AIE graph moved full macro-package objects through compute-tile FIFOs. Several FIFO
elements were 32-50 KB and used depth 2, which made individual edges reserve more
memory than a 64 KB compute tile can afford before stack, scratch, compiler overhead,
and neighboring traffic are considered. The graph also concentrated unrelated stage
work into bridge/worker cores, increasing program-memory and routing pressure.

Useful material to salvage:

- resident-parameter arena offsets;
- stage arithmetic kernels that still make sense after fission;
- the frame-conveyor public ABI and capacity-class metadata.

Do not revive this graph by patching compiler errors. The replacement design uses
microtile/cell FIFOs, resource-budget checks, and separate two-column and four-column
spatial profiles.
