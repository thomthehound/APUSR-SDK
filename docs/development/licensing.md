<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (c) 2026 thomthehound. -->

# Licensing and provenance policy

Original APUSR work is licensed under the Apache License 2.0. Third-party and
derived material remains under its original terms; the repository-level APUSR
license does not overwrite those terms.

`LICENSES/` carries distribution-ready copies of license texts that APUSR packages need to ship without depending on a vendored source-tree path.

## Original APUSR files

Use:

```text
Copyright (c) 2026 thomthehound and APUSR contributors.
SPDX-License-Identifier: Apache-2.0
```

Existing files whose original work is solely attributable to thomthehound may
retain the narrower `Copyright (c) 2026 thomthehound.` notice.

Generated APUSR wrappers, manifests, or lookup tables may use Apache-2.0 when
the generated payload is itself APUSR-owned. Generated payloads derived from
third-party shaders, model parameters, or other copyrighted input retain the
source material's license and provenance.

## Third-party-derived files

Do not replace upstream copyright notices. Keep the upstream license, add a
clear APUSR modification note when useful, and add an APUSR copyright notice
only for copyrightable APUSR modifications.

A copied subset must carry the license text required by its upstream license;
placing a subset outside `third_party/` does not turn it into original APUSR
work.

## New imports

Before committing a new external source tree, binary, model, shader payload,
manual, or generated artifact, record:

- upstream project and version/revision;
- exact source location;
- license applying to the exact imported version;
- required notices;
- whether source and binary redistribution are both permitted;
- whether generated/derived forms have different terms;
- local paths that contain the imported or derived material.

If those facts are not clear, do not commit the artifact. Prefer reproducible
fetch/build instructions until redistribution rights are established.

## Contributions

APUSR uses DCO 1.1 sign-off for contributions and does not require a CLA. See
`CONTRIBUTING.md` and the root `DCO` file.

## Optional external dependencies

Finding a dependency on the developer machine is not redistribution clearance.
For WinPixEventRuntime profiling builds, CMake therefore requires the license
file belonging to the selected MIT-licensed package and stages that file beside
the redistributed DLL. XRT remains different: its runtime is externally supplied
and APUSR does not clear or publish an XRT runtime binary until the exact build,
license, notices, and redistribution terms have been recorded.
