<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Contributing to APUSR

APUSR is performance-sensitive systems software. Changes should preserve
observable behavior, backend correctness, and the ability to measure whether an
optimization actually helped.

## Developer Certificate of Origin

APUSR uses the **Developer Certificate of Origin 1.1** and does not require a
Contributor License Agreement.

Every contributed commit must contain a `Signed-off-by:` trailer certifying the
DCO in the repository root. The normal way to add it is:

```text
git commit -s
```

By signing off a commit, you certify that you have the right to submit the work
under the license indicated by the affected files. Do not sign off work whose
provenance or redistribution rights you cannot establish.

## Engineering expectations

- Prefer the real implementation path over scaffolding or compatibility layers
  that do not exercise production behavior.
- Do not silently substitute scalar emulation or a slow fallback for an
  advertised accelerated path.
- Keep API-neutral model/resource contracts independent of D3D12/Vulkan
  implementation details.
- Backend-specific lowering is acceptable when it preserves semantics and fixes
  a measured compiler/backend problem.
- Treat descriptor/resource lifetime and synchronization as correctness
  contracts.
- Keep hot-path logging disabled by default.
- Preserve explicit TODOs for unresolved correctness, performance, or
  architectural debt until the underlying issue is actually solved.

## Licensing and provenance

Original APUSR contributions are accepted under Apache-2.0 unless a file
explicitly states another license. Do not replace or remove an existing
third-party copyright/license notice.

For new original source files, use the short project header appropriate to the
file format, for example:

```text
Copyright (c) 2026 thomthehound and APUSR contributors.
SPDX-License-Identifier: Apache-2.0
```

When modifying a third-party-derived file, keep its existing license and
attribution. If the modification is substantial enough to carry a separate
copyright notice, add that notice without implying ownership of the upstream
material.

New third-party imports require an explicit provenance and redistribution check
before they are committed. See `THIRD_PARTY_NOTICES.md` and
`docs/development/licensing.md`.

## Tests

Run:

```text
python tools/quality_check.py
```

before submitting changes. Add regression coverage for behavior that previously
failed or for non-obvious invariants introduced by an optimization.

Tests should be high-signal. A trivial compile/run test is not a substitute for
checking the contract that motivated a change.

## Performance changes

Performance patches should state:

- the bottleneck being targeted;
- why the transformation is semantically safe;
- which backend/path is affected;
- the measurement used to decide whether to keep it.

Do not infer whole-frame performance from shader size or instruction count
alone. Those are useful clues, not final evidence.
