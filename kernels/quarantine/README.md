<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Quarantined kernel experiments

This directory keeps failed or retired AIE graph attempts as reference material only.
Nothing under this directory is a selectable product profile, a build target, or a
runtime contract.

The old full-arena two-column Stage1->12 graph is preserved here because it captured
useful FSR4 stage wiring and resident-parameter offsets, but its internal package-sized
object FIFOs and overloaded bridge/worker cores made it the wrong implementation shape
for the product middle-core.
