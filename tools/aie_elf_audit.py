#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Collect AIE ELF disassembly and scalar/vector instruction summaries.

This script packages the files needed for middle-core scalar audits after an
MLIR-AIE compile. It searches a build tree for ``main_core_*.elf`` files,
runs the AIE-aware LLVM tools, emits per-core dumps, and writes compact CSV/TXT
summaries so repeated audits do not require hand-written PowerShell snippets.
"""

from __future__ import annotations

import argparse
import csv
import datetime as _datetime
import os
import re
import shutil
import subprocess
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_ELF_GLOB = "main_core_*.elf"
DEFAULT_SIDECARE_EXTENSIONS = {".ll", ".mlir", ".script", ".map", ".json", ".txt"}
DEFAULT_RELEVANT_SYMBOL_PATTERN = (
    "front|encoder|bottleneck|decoder|c16|c32|c64|halo|srs|cell_copy|"
    "vector|primitive|mmul|mac|contract|expand"
)
DEFAULT_RELEVANT_DISASM_PATTERN = (
    "front|encoder|bottleneck|decoder|cell_copy|primitive|mac|mul|srs|ups|"
    "load|store|vadd|vmul|vmac|vaddmac|vshuffle|vshift|vextract|vpush|st\\.s8|lda\\.s8|nop"
)
WATCHED_MNEMONICS = [
    "lda.s8",
    "st.s8",
    "vextract.8",
    "vpush.hi.8",
    "vpush.hi.16",
    "vpush.hi.32",
    "vldb",
    "vlda",
    "vst.pack",
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
    "vmac",
    "vaddmac",
    "vmul",
    "vsrs.4x",
]
SCALAR_BYTE_MNEMONICS = {"lda.s8", "st.s8", "vextract.8", "vpush.hi.8"}
PARAMETER_PACK_MNEMONICS = {"vpush.hi.32", "vpush.hi.16"}
INSTRUCTION_LINE_RE = re.compile(r"^\s*[0-9a-fA-F]+:\s+(?:[0-9a-fA-F]{2}\s+)+\s*(?P<ops>.*)$")
SECTION_TEXT_RE = re.compile(r"^\s*\d+\s+\.text\s+(?P<size>[0-9a-fA-F]+)\s+")
CORE_NAME_RE = re.compile(r"main_core_(?P<col>\d+)_(?P<row>\d+)\.elf$")


class AuditError(Exception):
    pass


@dataclass(frozen=True)
class LlvmTools:
    objdump: Path
    nm: Path | None
    readelf: Path | None


@dataclass(frozen=True)
class ElfAudit:
    elf: Path
    output_dir: Path
    elf_size: int
    text_size: int | None
    mnemonic_counts: dict[str, int]

    @property
    def core_name(self) -> str:
        return self.elf.stem

    @property
    def scalar_byte_count(self) -> int:
        return sum(self.mnemonic_counts.get(mnemonic, 0) for mnemonic in SCALAR_BYTE_MNEMONICS)

    @property
    def parameter_pack_count(self) -> int:
        return sum(self.mnemonic_counts.get(mnemonic, 0) for mnemonic in PARAMETER_PACK_MNEMONICS)


def _normalize_path(path: str | Path) -> Path:
    return Path(path).expanduser().resolve()


def _path_entries() -> list[Path]:
    entries: list[Path] = []
    for item in os.environ.get("PATH", "").split(os.pathsep):
        if item:
            entries.append(Path(item))
    return entries


def _tool_name(name: str) -> str:
    return f"{name}.exe" if os.name == "nt" else name


def _existing_file_candidates(candidates: Iterable[Path]) -> list[Path]:
    seen: set[Path] = set()
    result: list[Path] = []
    for candidate in candidates:
        if not candidate:
            continue
        try:
            resolved = candidate.expanduser().resolve()
        except OSError:
            continue
        if resolved in seen:
            continue
        if resolved.is_file():
            seen.add(resolved)
            result.append(resolved)
    return result


def _llvm_tool_candidates(name: str, args: argparse.Namespace) -> list[Path]:
    tool = _tool_name(name)
    candidates: list[Path] = []

    if args.llvm_bin:
        llvm_bin = _normalize_path(args.llvm_bin)
        candidates.append(llvm_bin / tool if llvm_bin.is_dir() else llvm_bin)

    if name == "llvm-objdump" and args.objdump:
        candidates.insert(0, _normalize_path(args.objdump))

    env_roots = [
        "PEANO_INSTALL_DIR",
        "LLVM_AIE_INSTALL_DIR",
        "LLVM_INSTALL_DIR",
        "MLIR_AIE_INSTALL_DIR",
    ]
    for env_name in env_roots:
        value = os.environ.get(env_name)
        if value:
            candidates.append(Path(value) / "bin" / tool)

    if args.mlir_aie_root:
        root = _normalize_path(args.mlir_aie_root)
        candidates.extend(
            [
                root / "llvm" / "install" / "bin" / tool,
                root / "llvm-aie" / "install" / "bin" / tool,
                root / "build" / "bin" / tool,
                root / "install" / "bin" / tool,
            ]
        )

    candidates.extend([Path("C:/opt/llvm-aie/bin") / tool, Path("/opt/llvm-aie/bin") / tool])
    candidates.extend(path / tool for path in _path_entries())
    return _existing_file_candidates(candidates)


def _run_capture(command: Sequence[str | Path], output_path: Path, *, cwd: Path | None = None) -> tuple[int, str]:
    printable = " ".join(str(part) for part in command)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    process = subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd) if cwd else None,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    text = process.stdout or ""
    output_path.write_text(text, encoding="utf-8", errors="replace")
    if process.returncode != 0:
        with output_path.open("a", encoding="utf-8") as handle:
            handle.write(f"\n[command exited {process.returncode}: {printable}]\n")
    return process.returncode, text


def _find_llvm_tools(args: argparse.Namespace, out_dir: Path) -> LlvmTools:
    objdump_candidates = _llvm_tool_candidates("llvm-objdump", args)
    if not objdump_candidates:
        raise AuditError(
            "could not find llvm-objdump. Pass --llvm-bin, --objdump, --mlir-aie-root, "
            "or set PEANO_INSTALL_DIR/LLVM_AIE_INSTALL_DIR."
        )

    selected_objdump: Path | None = None
    version_text = ""
    version_path = out_dir / "03_llvm_objdump_version.txt"
    for candidate in objdump_candidates:
        _code, text = _run_capture([candidate, "--version"], version_path)
        version_text = text
        if args.no_require_aie_target or re.search(r"Registered Targets:.*\baie\b|^\s+aie\b", text, re.MULTILINE):
            selected_objdump = candidate
            break
    if selected_objdump is None:
        candidates = "\n".join(str(path) for path in objdump_candidates)
        raise AuditError(
            "found llvm-objdump, but none reported an AIE target. "
            "Pass --no-require-aie-target only for dry host-tool debugging.\n"
            f"Candidates:\n{candidates}\nLast --version output:\n{version_text}"
        )

    tool_dir = selected_objdump.parent
    nm_candidates = _existing_file_candidates([tool_dir / _tool_name("llvm-nm"), *_llvm_tool_candidates("llvm-nm", args)])
    readelf_candidates = _existing_file_candidates(
        [tool_dir / _tool_name("llvm-readelf"), *_llvm_tool_candidates("llvm-readelf", args)]
    )
    return LlvmTools(
        objdump=selected_objdump,
        nm=nm_candidates[0] if nm_candidates else None,
        readelf=readelf_candidates[0] if readelf_candidates else None,
    )


def _write_environment(out_dir: Path) -> None:
    interesting = ["AIE", "PEANO", "LLVM", "XRT", "IRON", "PATH"]
    lines: list[str] = []
    for key, value in sorted(os.environ.items()):
        if any(fragment in key.upper() for fragment in interesting):
            lines.append(f"{key}={value}")
    (out_dir / "01_relevant_env.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _core_sort_key(path: Path) -> tuple[int, int, str]:
    match = CORE_NAME_RE.match(path.name)
    if match:
        return (int(match.group("col")), int(match.group("row")), path.name)
    return (9999, 9999, path.name)


def _discover_build_dirs(args: argparse.Namespace) -> list[Path]:
    if args.build_dir:
        return [_normalize_path(path) for path in args.build_dir]
    default_dirs = [path for path in REPO_ROOT.iterdir() if path.is_dir() and (path.name == "build" or path.name.startswith("build_"))]
    return sorted(default_dirs)


def _discover_elves(args: argparse.Namespace) -> list[Path]:
    if args.elf:
        elves = [_normalize_path(path) for path in args.elf]
    else:
        search_roots: list[Path] = []
        if args.project:
            search_roots.extend(_normalize_path(path) for path in args.project)
        else:
            search_roots.extend(_discover_build_dirs(args))
        elves = []
        for root in search_roots:
            if root.is_file() and root.name.endswith(".elf"):
                elves.append(root)
                continue
            if not root.exists():
                continue
            elves.extend(root.rglob(args.elf_glob))
    unique: dict[Path, None] = {}
    for elf in elves:
        if elf.is_file():
            unique[elf.resolve()] = None
    return sorted(unique.keys(), key=_core_sort_key)


def _timestamp() -> str:
    return _datetime.datetime.now().strftime("%Y%m%d_%H%M%S")


def _default_output_dir(args: argparse.Namespace) -> Path:
    profile = args.label or "aie_elf_audit"
    return REPO_ROOT / f"{profile}_{_timestamp()}"


def _safe_core_dir_name(elf: Path) -> str:
    match = CORE_NAME_RE.match(elf.name)
    if match:
        return f"core_{int(match.group('col')):02d}_{int(match.group('row')):02d}_{elf.stem}"
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", elf.stem)


def _parse_text_size(section_text: str) -> int | None:
    for line in section_text.splitlines():
        match = SECTION_TEXT_RE.match(line)
        if match:
            return int(match.group("size"), 16)
    return None


def _extract_mnemonic(operation: str) -> str | None:
    operation = operation.strip()
    if not operation or operation == "...":
        return None
    token = operation.split(None, 1)[0].strip()
    if token.endswith(":"):
        return None
    return token


def _count_mnemonics(disasm_text: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for line in disasm_text.splitlines():
        match = INSTRUCTION_LINE_RE.match(line)
        if not match:
            continue
        ops = match.group("ops")
        for operation in ops.split(";"):
            mnemonic = _extract_mnemonic(operation)
            if mnemonic:
                counts[mnemonic] = counts.get(mnemonic, 0) + 1
    return counts


def _grep_with_context(path: Path, pattern: str, output_path: Path, *, before: int, after: int) -> None:
    if not path.exists():
        output_path.write_text("", encoding="utf-8")
        return
    regex = re.compile(pattern, re.IGNORECASE)
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    selected: set[int] = set()
    for index, line in enumerate(lines):
        if regex.search(line):
            start = max(0, index - before)
            end = min(len(lines), index + after + 1)
            selected.update(range(start, end))
    output: list[str] = []
    previous = -2
    for index in sorted(selected):
        if previous != -2 and index != previous + 1:
            output.append("--")
        output.append(f"{index + 1}: {lines[index]}")
        previous = index
    output_path.write_text("\n".join(output) + ("\n" if output else ""), encoding="utf-8")


def _copy_sidecars(elf: Path, output_dir: Path) -> None:
    prefix = elf.with_suffix("")
    source_dir = elf.parent
    sidecar_dir = output_dir / "sidecars"
    sidecar_dir.mkdir(exist_ok=True)
    candidates: list[Path] = []
    for path in source_dir.iterdir():
        if path == elf:
            continue
        if path.name.startswith(prefix.name) and path.suffix in DEFAULT_SIDECARE_EXTENSIONS:
            candidates.append(path)
    # Include project-level MLIR/JSON context without copying binary blobs.
    for path in source_dir.glob("*.mlir"):
        if path not in candidates:
            candidates.append(path)
    for path in source_dir.glob("*.json"):
        if path not in candidates:
            candidates.append(path)
    for path in sorted(candidates):
        if path.is_file():
            shutil.copy2(path, sidecar_dir / path.name)


def _audit_one_elf(elf: Path, tools: LlvmTools, out_dir: Path, args: argparse.Namespace) -> ElfAudit:
    core_dir = out_dir / _safe_core_dir_name(elf)
    core_dir.mkdir(parents=True, exist_ok=True)
    (core_dir / "00_manifest.txt").write_text(
        f"ELF: {elf}\n"
        f"ELF size: {elf.stat().st_size}\n"
        f"Date: {_datetime.datetime.now().isoformat()}\n",
        encoding="utf-8",
    )

    _code, sections = _run_capture([tools.objdump, "-h", elf], core_dir / "10_sections_objdump.txt")
    _run_capture([tools.objdump, "-t", "-C", elf], core_dir / "11_symbols_objdump_demangled.txt")
    _code, disasm = _run_capture(
        [tools.objdump, "-d", "-C", "--print-imm-hex", elf], core_dir / "20_disasm_full_demangled.txt"
    )
    _run_capture(
        [tools.objdump, "-d", "-C", "-S", "--line-numbers", "--print-imm-hex", elf],
        core_dir / "21_disasm_source_lines_demangled.txt",
    )

    if tools.nm:
        _run_capture([tools.nm, "-C", "--defined-only", "--numeric-sort", elf], core_dir / "12_nm_defined_numeric_demangled.txt")
        _run_capture([tools.nm, "--defined-only", "--numeric-sort", elf], core_dir / "13_nm_defined_numeric_raw.txt")
    if tools.readelf:
        _run_capture([tools.readelf, "-h", elf], core_dir / "14_readelf_header.txt")
        _run_capture([tools.readelf, "-S", elf], core_dir / "15_readelf_sections.txt")
        _run_capture([tools.readelf, "-s", elf], core_dir / "16_readelf_symbols.txt")
        _run_capture([tools.readelf, "-r", elf], core_dir / "17_readelf_relocs.txt")

    _grep_with_context(
        core_dir / "11_symbols_objdump_demangled.txt",
        args.symbol_pattern,
        core_dir / "30_relevant_symbol_hits.txt",
        before=2,
        after=2,
    )
    _grep_with_context(
        core_dir / "20_disasm_full_demangled.txt",
        args.disasm_pattern,
        core_dir / "31_relevant_disasm_hits.txt",
        before=4,
        after=12,
    )
    if args.copy_sidecars:
        _copy_sidecars(elf, core_dir)

    return ElfAudit(
        elf=elf,
        output_dir=core_dir,
        elf_size=elf.stat().st_size,
        text_size=_parse_text_size(sections),
        mnemonic_counts=_count_mnemonics(disasm),
    )


def _write_csv(path: Path, rows: Iterable[dict[str, object]], fieldnames: Sequence[str]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def _write_summaries(out_dir: Path, audits: Sequence[ElfAudit], tools: LlvmTools, args: argparse.Namespace) -> None:
    summary_fields = ["core", "elf", "elf_size", "text_size", "scalar_byte_count", "parameter_pack_count"]
    _write_csv(
        out_dir / "05_core_summary.csv",
        (
            {
                "core": audit.core_name,
                "elf": str(audit.elf),
                "elf_size": audit.elf_size,
                "text_size": "" if audit.text_size is None else audit.text_size,
                "scalar_byte_count": audit.scalar_byte_count,
                "parameter_pack_count": audit.parameter_pack_count,
            }
            for audit in audits
        ),
        summary_fields,
    )

    count_fields = ["core", *WATCHED_MNEMONICS]
    _write_csv(
        out_dir / "06_instruction_counts.csv",
        (
            {"core": audit.core_name, **{mnemonic: audit.mnemonic_counts.get(mnemonic, 0) for mnemonic in WATCHED_MNEMONICS}}
            for audit in audits
        ),
        count_fields,
    )

    totals = {mnemonic: 0 for mnemonic in WATCHED_MNEMONICS}
    total_scalar_byte = 0
    total_parameter_pack = 0
    for audit in audits:
        total_scalar_byte += audit.scalar_byte_count
        total_parameter_pack += audit.parameter_pack_count
        for mnemonic in WATCHED_MNEMONICS:
            totals[mnemonic] += audit.mnemonic_counts.get(mnemonic, 0)

    lines = [
        "AIE ELF audit summary",
        f"Date: {_datetime.datetime.now().isoformat()}",
        f"ELF count: {len(audits)}",
        f"llvm-objdump: {tools.objdump}",
        f"llvm-nm: {tools.nm or ''}",
        f"llvm-readelf: {tools.readelf or ''}",
        f"Search glob: {args.elf_glob}",
        "",
        "Per-core scalar-byte staging counts:",
    ]
    for audit in audits:
        lines.append(
            f"  {audit.core_name}: scalar-byte={audit.scalar_byte_count}, "
            f"parameter-pack={audit.parameter_pack_count}, .text={audit.text_size if audit.text_size is not None else '?'}"
        )
    lines.extend(["", "Watched instruction totals:"])
    for mnemonic in WATCHED_MNEMONICS:
        lines.append(f"  {mnemonic}: {totals[mnemonic]}")
    lines.extend(
        [
            "",
            f"Total scalar-byte staging instructions: {total_scalar_byte}",
            f"Total compact-parameter pack instructions: {total_parameter_pack}",
            "",
            "Scalar-byte staging watches: " + ", ".join(sorted(SCALAR_BYTE_MNEMONICS)),
            "Parameter-pack watches: " + ", ".join(sorted(PARAMETER_PACK_MNEMONICS)),
        ]
    )
    (out_dir / "50_combined_summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _write_manifest(out_dir: Path, elves: Sequence[Path], tools: LlvmTools | None, args: argparse.Namespace) -> None:
    lines = [
        "AIE ELF audit package",
        f"Date: {_datetime.datetime.now().isoformat()}",
        f"Repo root: {REPO_ROOT}",
        f"Output dir: {out_dir}",
        f"Label: {args.label or ''}",
        f"ELF glob: {args.elf_glob}",
        "",
        "Inputs:",
        *(f"  {elf}" for elf in elves),
    ]
    if tools:
        lines.extend(
            [
                "",
                "Tools:",
                f"  llvm-objdump: {tools.objdump}",
                f"  llvm-nm: {tools.nm or ''}",
                f"  llvm-readelf: {tools.readelf or ''}",
            ]
        )
    (out_dir / "00_manifest.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def _zip_dir(directory: Path, destination: Path) -> None:
    if destination.exists():
        destination.unlink()
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for path in sorted(directory.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(directory.parent))


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", action="append", type=Path, help="Build directory to scan. Repeatable.")
    parser.add_argument("--project", action="append", type=Path, help="Specific .mlir.prj directory to scan. Repeatable.")
    parser.add_argument("--elf", action="append", type=Path, help="Specific ELF to dump. Repeatable.")
    parser.add_argument("--elf-glob", default=DEFAULT_ELF_GLOB, help=f"ELF glob used when scanning directories. Default: {DEFAULT_ELF_GLOB}")
    parser.add_argument("--out", type=Path, help="Output directory. Default: repo/aie_elf_audit_<timestamp>.")
    parser.add_argument("--label", help="Name prefix for the default output directory.")
    parser.add_argument("--llvm-bin", type=Path, help="Directory containing AIE-aware llvm-objdump/llvm-nm/llvm-readelf.")
    parser.add_argument("--objdump", type=Path, help="Exact llvm-objdump executable to use.")
    parser.add_argument("--mlir-aie-root", type=Path, help="MLIR-AIE checkout/install root used for LLVM tool discovery.")
    parser.add_argument("--no-require-aie-target", action="store_true", help="Do not require llvm-objdump --version to list an AIE target.")
    parser.add_argument("--no-sidecars", dest="copy_sidecars", action="store_false", help="Do not copy matching .ll/.mlir/.json sidecar files.")
    parser.add_argument("--no-zip", dest="make_zip", action="store_false", help="Leave the output directory unpacked only.")
    parser.add_argument("--symbol-pattern", default=DEFAULT_RELEVANT_SYMBOL_PATTERN, help="Regex for symbol hit extracts.")
    parser.add_argument("--disasm-pattern", default=DEFAULT_RELEVANT_DISASM_PATTERN, help="Regex for disassembly hit extracts.")
    parser.set_defaults(copy_sidecars=True, make_zip=True)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    out_dir = _normalize_path(args.out) if args.out else _default_output_dir(args).resolve()
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    _write_environment(out_dir)
    elves = _discover_elves(args)
    if not elves:
        build_dirs = ", ".join(str(path) for path in _discover_build_dirs(args)) or "<none>"
        raise AuditError(f"no ELF files matched {args.elf_glob!r}. Build dirs searched: {build_dirs}")

    tools = _find_llvm_tools(args, out_dir)
    (out_dir / "02_tool_paths.txt").write_text(
        f"Using llvm-objdump: {tools.objdump}\n"
        f"Using llvm-nm: {tools.nm or ''}\n"
        f"Using llvm-readelf: {tools.readelf or ''}\n",
        encoding="utf-8",
    )
    _write_manifest(out_dir, elves, tools, args)

    audits = [_audit_one_elf(elf, tools, out_dir, args) for elf in elves]
    _write_summaries(out_dir, audits, tools, args)

    if args.make_zip:
        zip_path = out_dir.with_suffix(".zip")
        _zip_dir(out_dir, zip_path)
        print(f"Created {zip_path}")
    print(f"Wrote {out_dir}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AuditError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(2) from exc
