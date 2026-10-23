#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Compare AIE ELF audit packages without pretending static counts are cycles.

The scalar audit is necessary, but scalar removal is not the whole performance
story. This tool compares two ``tools/aie_elf_audit.py`` outputs and reports the
instruction-mix tradeoff: code size, scalar staging, parameter packing, vector
memory traffic, vector data motion, vector math, and total watched instructions.

Inputs can be unpacked audit directories or ``.zip`` files produced by
``tools/aie_elf_audit.py``. Missing instruction columns are treated as zero so
older audits remain comparable to newer ones.
"""

from __future__ import annotations

import argparse
import csv
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


SCALAR_BYTE_MNEMONICS = ("lda.s8", "st.s8", "vextract.8", "vpush.hi.8")
PARAMETER_PACK_MNEMONICS = ("vpush.hi.16", "vpush.hi.32")
VECTOR_MEMORY_MNEMONICS = ("vldb", "vlda", "vst.pack")
VECTOR_DATA_MOTION_MNEMONICS = (
    "vshuffle",
    "vshift",
    "vpack",
    "vsel.8",
    "vsel.16",
    "vsel.32",
    "vbcst.8",
    "vbcst.16",
    "vbcst.32",
    "vmov",
    "vmov.d",
)
VECTOR_MATH_MNEMONICS = ("vmac", "vaddmac", "vmul", "vsrs.4x")

GROUPS = {
    "scalar_byte": SCALAR_BYTE_MNEMONICS,
    "parameter_pack": PARAMETER_PACK_MNEMONICS,
    "vector_memory": VECTOR_MEMORY_MNEMONICS,
    "vector_data_motion": VECTOR_DATA_MOTION_MNEMONICS,
    "vector_math": VECTOR_MATH_MNEMONICS,
}


class CompareError(Exception):
    pass


@dataclass(frozen=True)
class AuditPackage:
    label: str
    root: Path
    core_rows: dict[str, dict[str, int]]
    instruction_rows: dict[str, dict[str, int]]

    @property
    def cores(self) -> tuple[str, ...]:
        return tuple(sorted(set(self.core_rows) | set(self.instruction_rows)))


def _as_int(value: str | int | None) -> int:
    if value is None or value == "":
        return 0
    return int(value)


def _read_csv(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        raise CompareError(f"missing required audit file: {path}")
    with path.open("r", encoding="utf-8", newline="") as handle:
        return list(csv.DictReader(handle))


def _find_audit_root(path: Path) -> Path:
    if (path / "05_core_summary.csv").is_file() and (path / "06_instruction_counts.csv").is_file():
        return path
    matches = [candidate for candidate in path.rglob("05_core_summary.csv") if (candidate.parent / "06_instruction_counts.csv").is_file()]
    if len(matches) == 1:
        return matches[0].parent
    if not matches:
        raise CompareError(f"could not find audit summary files under {path}")
    joined = "\n".join(str(match.parent) for match in matches[:8])
    raise CompareError(f"found multiple audit roots under {path}; pass the exact directory:\n{joined}")


def _extract_if_zip(path: Path, tempdirs: list[tempfile.TemporaryDirectory[str]]) -> Path:
    if path.suffix.lower() != ".zip":
        return path
    tmp = tempfile.TemporaryDirectory(prefix="apusr_aie_audit_compare_")
    tempdirs.append(tmp)
    root = Path(tmp.name)
    with zipfile.ZipFile(path) as archive:
        archive.extractall(root)
    return root


def load_audit(path: Path, *, label: str | None = None, tempdirs: list[tempfile.TemporaryDirectory[str]] | None = None) -> AuditPackage:
    temps = tempdirs if tempdirs is not None else []
    expanded = _extract_if_zip(path.expanduser().resolve(), temps)
    root = _find_audit_root(expanded)

    core_rows: dict[str, dict[str, int]] = {}
    for row in _read_csv(root / "05_core_summary.csv"):
        core = row.get("core") or ""
        if not core:
            continue
        core_rows[core] = {
            "elf_size": _as_int(row.get("elf_size")),
            "text_size": _as_int(row.get("text_size")),
            "scalar_byte_count": _as_int(row.get("scalar_byte_count")),
            "parameter_pack_count": _as_int(row.get("parameter_pack_count")),
        }

    instruction_rows: dict[str, dict[str, int]] = {}
    for row in _read_csv(root / "06_instruction_counts.csv"):
        core = row.get("core") or ""
        if not core:
            continue
        instruction_rows[core] = {key: _as_int(value) for key, value in row.items() if key and key != "core"}

    return AuditPackage(label=label or path.name, root=root, core_rows=core_rows, instruction_rows=instruction_rows)


def _sum_core_metric(package: AuditPackage, metric: str) -> int:
    return sum(package.core_rows.get(core, {}).get(metric, 0) for core in package.cores)


def _sum_mnemonics(package: AuditPackage, mnemonics: Iterable[str]) -> int:
    total = 0
    for core in package.cores:
        row = package.instruction_rows.get(core, {})
        total += sum(row.get(mnemonic, 0) for mnemonic in mnemonics)
    return total


def _all_mnemonics(*packages: AuditPackage) -> tuple[str, ...]:
    names: set[str] = set()
    for package in packages:
        for row in package.instruction_rows.values():
            names.update(row)
    return tuple(sorted(names))


def _sum_total_watched(package: AuditPackage, mnemonics: Sequence[str]) -> int:
    return _sum_mnemonics(package, mnemonics)


def _format_delta(delta: int) -> str:
    if delta > 0:
        return f"+{delta}"
    return str(delta)


def _row(name: str, base: int | None, current: int) -> str:
    if base is None:
        return f"{name:24} {current:8d}"
    return f"{name:24} {base:8d} -> {current:8d}  {_format_delta(current - base):>8}"


def package_summary(package: AuditPackage, *, baseline: AuditPackage | None = None) -> str:
    mnemonics = _all_mnemonics(*(filter(None, [baseline, package])))
    base = baseline
    lines: list[str] = []
    lines.append(f"Current:  {package.label}")
    lines.append(f"Root:     {package.root}")
    if base:
        lines.append(f"Baseline: {base.label}")
        lines.append("")
        lines.append(f"{'metric':24} {'baseline':>8} -> {'current':>8}  {'delta':>8}")
    else:
        lines.append("")
        lines.append(f"{'metric':24} {'current':>8}")

    metrics = [
        ("cores", len(base.cores) if base else None, len(package.cores)),
        (".text bytes", _sum_core_metric(base, "text_size") if base else None, _sum_core_metric(package, "text_size")),
        ("elf bytes", _sum_core_metric(base, "elf_size") if base else None, _sum_core_metric(package, "elf_size")),
        (
            "scalar-byte instrs",
            _sum_core_metric(base, "scalar_byte_count") if base else None,
            _sum_core_metric(package, "scalar_byte_count"),
        ),
        (
            "parameter-pack instrs",
            _sum_core_metric(base, "parameter_pack_count") if base else None,
            _sum_core_metric(package, "parameter_pack_count"),
        ),
    ]
    for group_name, group_mnemonics in GROUPS.items():
        metrics.append(
            (
                group_name,
                _sum_mnemonics(base, group_mnemonics) if base else None,
                _sum_mnemonics(package, group_mnemonics),
            )
        )
    metrics.append(("total watched instrs", _sum_total_watched(base, mnemonics) if base else None, _sum_total_watched(package, mnemonics)))

    for name, base_value, current_value in metrics:
        lines.append(_row(name, base_value, current_value))

    lines.append("")
    lines.append("Instruction totals:")
    if base:
        lines.append(f"{'mnemonic':24} {'baseline':>8} -> {'current':>8}  {'delta':>8}")
    else:
        lines.append(f"{'mnemonic':24} {'current':>8}")
    for mnemonic in mnemonics:
        current = _sum_mnemonics(package, (mnemonic,))
        baseline_value = _sum_mnemonics(base, (mnemonic,)) if base else None
        if base and current == baseline_value == 0:
            continue
        lines.append(_row(mnemonic, baseline_value, current))

    if base:
        lines.append("")
        lines.append("Per-core deltas:")
        lines.append(f"{'core':18} {'.text':>8} {'scalar':>8} {'param':>8} {'vmem':>8} {'vmove':>8} {'vmath':>8}")
        for core in sorted(set(base.cores) | set(package.cores)):
            b_core = base.core_rows.get(core, {})
            c_core = package.core_rows.get(core, {})
            b_instr = base.instruction_rows.get(core, {})
            c_instr = package.instruction_rows.get(core, {})
            def group_delta(group: Sequence[str]) -> int:
                return sum(c_instr.get(name, 0) for name in group) - sum(b_instr.get(name, 0) for name in group)
            lines.append(
                f"{core:18} "
                f"{_format_delta(c_core.get('text_size', 0) - b_core.get('text_size', 0)):>8} "
                f"{_format_delta(c_core.get('scalar_byte_count', 0) - b_core.get('scalar_byte_count', 0)):>8} "
                f"{_format_delta(c_core.get('parameter_pack_count', 0) - b_core.get('parameter_pack_count', 0)):>8} "
                f"{_format_delta(group_delta(VECTOR_MEMORY_MNEMONICS)):>8} "
                f"{_format_delta(group_delta(VECTOR_DATA_MOTION_MNEMONICS)):>8} "
                f"{_format_delta(group_delta(VECTOR_MATH_MNEMONICS)):>8}"
            )

    return "\n".join(lines) + "\n"


def _parse_limit(text: str) -> tuple[str, int]:
    if "=" not in text:
        raise argparse.ArgumentTypeError("limits must be name=value")
    name, value = text.split("=", 1)
    name = name.strip()
    if not name:
        raise argparse.ArgumentTypeError("limit name is empty")
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid limit value: {value!r}") from exc
    return name, parsed


def evaluate_limits(current: AuditPackage, baseline: AuditPackage | None, limits: Sequence[tuple[str, int]], require_zero_scalar: bool, require_zero_parameter_pack: bool) -> list[str]:
    failures: list[str] = []
    values = {
        "scalar_byte": _sum_core_metric(current, "scalar_byte_count"),
        "parameter_pack": _sum_core_metric(current, "parameter_pack_count"),
        "text_bytes": _sum_core_metric(current, "text_size"),
        "vector_memory": _sum_mnemonics(current, VECTOR_MEMORY_MNEMONICS),
        "vector_data_motion": _sum_mnemonics(current, VECTOR_DATA_MOTION_MNEMONICS),
        "vector_math": _sum_mnemonics(current, VECTOR_MATH_MNEMONICS),
    }
    if baseline:
        values.update(
            {
                "delta_text_bytes": values["text_bytes"] - _sum_core_metric(baseline, "text_size"),
                "delta_vector_memory": values["vector_memory"] - _sum_mnemonics(baseline, VECTOR_MEMORY_MNEMONICS),
                "delta_vector_data_motion": values["vector_data_motion"] - _sum_mnemonics(baseline, VECTOR_DATA_MOTION_MNEMONICS),
                "delta_vector_math": values["vector_math"] - _sum_mnemonics(baseline, VECTOR_MATH_MNEMONICS),
            }
        )
    if require_zero_scalar and values["scalar_byte"] != 0:
        failures.append(f"scalar_byte is {values['scalar_byte']}, expected 0")
    if require_zero_parameter_pack and values["parameter_pack"] != 0:
        failures.append(f"parameter_pack is {values['parameter_pack']}, expected 0")
    for name, maximum in limits:
        if name not in values:
            known = ", ".join(sorted(values))
            failures.append(f"unknown limit metric {name!r}; known metrics: {known}")
            continue
        value = values[name]
        if value > maximum:
            failures.append(f"{name} is {value}, limit is {maximum}")
    return failures


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("current", type=Path, help="Current audit directory or .zip package.")
    parser.add_argument("--baseline", type=Path, help="Baseline audit directory or .zip package.")
    parser.add_argument("--label", help="Display label for the current audit.")
    parser.add_argument("--baseline-label", help="Display label for the baseline audit.")
    parser.add_argument("--limit", action="append", type=_parse_limit, default=[], help="Optional gate, for example --limit delta_text_bytes=512. Known metrics are shown in error output.")
    parser.add_argument("--require-zero-scalar", action="store_true", help="Fail if scalar-byte staging remains.")
    parser.add_argument("--require-zero-parameter-pack", action="store_true", help="Fail if hot compact-parameter packing remains.")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    tempdirs: list[tempfile.TemporaryDirectory[str]] = []
    try:
        baseline = load_audit(args.baseline, label=args.baseline_label, tempdirs=tempdirs) if args.baseline else None
        current = load_audit(args.current, label=args.label, tempdirs=tempdirs)
        print(package_summary(current, baseline=baseline), end="")
        failures = evaluate_limits(
            current,
            baseline,
            args.limit,
            require_zero_scalar=args.require_zero_scalar,
            require_zero_parameter_pack=args.require_zero_parameter_pack,
        )
        if failures:
            print("\nAIE audit compare failed:")
            for failure in failures:
                print(f"  - {failure}")
            return 1
        return 0
    finally:
        for tempdir in tempdirs:
            tempdir.cleanup()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except CompareError as exc:
        print(f"error: {exc}")
        raise SystemExit(2) from exc
