#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Offline AMD_RDF/RGP/RMV inspection helper.

The runtime does not use this tool.  It understands the public RDF container
layout, AMDGPU ELF note metadata, and a small amount of RGP-specific chunk
structure that is useful for APUSR shader audits:

* RDF chunk inventory.
* CodeObject AMDGPU metadata: stage, workgroup size, wave size, SGPR/VGPR,
  LDS, scratch, and pipeline hashes.
* PsoCorrelation and COLoadEvent hash tables, enough to connect a code object
  to its API PSO hash and load-event size/timestamp records.

The tool intentionally avoids depending on the RGP GUI.  It accepts raw .rgp /
.rmv files, directories, or zip bundles containing captures.
"""

from __future__ import annotations

import argparse
import collections
import csv
import dataclasses
import json
import pathlib
import shutil
import struct
import sys
import tempfile
import zipfile
from typing import Any, Iterable, Iterator, Sequence

RDF_MAGIC = b"AMD_RDF "
RDF_HEADER = struct.Struct("<8sIIqq")
RDF_INDEX_ENTRY = struct.Struct("<16sB3xIqqqqq")
ELF64_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
ELF64_SECTION = struct.Struct("<IIQQQQIIQQ")
ELF_NOTE_HEADER = struct.Struct("<III")
PSO_CORRELATION_HEADER = struct.Struct("<I")
CO_LOAD_EVENT_HEADER = struct.Struct("<I")
PSO_CORRELATION_RECORD_SIZE = 96
CO_LOAD_EVENT_RECORD_SIZE = 40
CODE_OBJECT_HEADER = struct.Struct("<IIQQ")

CAPTURE_SUFFIXES = (".rgp", ".rmv")


@dataclasses.dataclass(frozen=True)
class RdfEntry:
    ordinal: int
    identifier: str
    compression: int
    version: int
    header_offset: int
    header_size: int
    data_offset: int
    data_size: int
    uncompressed_size: int
    identifier_index: int


class RdfFile:
    def __init__(self, path: pathlib.Path) -> None:
        self.path = path
        self._data = path.read_bytes()
        if len(self._data) < RDF_HEADER.size:
            raise ValueError(f"{path} is too small to be an RDF file")
        magic, version, reserved, index_offset, index_size = RDF_HEADER.unpack_from(self._data, 0)
        if magic != RDF_MAGIC:
            raise ValueError(f"{path} is not an AMD_RDF file")
        if reserved != 0:
            raise ValueError(f"{path} has a non-zero RDF reserved header field")
        if index_offset < 0 or index_size < 0 or index_offset + index_size > len(self._data):
            raise ValueError(f"{path} has an invalid RDF index range")
        if index_size % RDF_INDEX_ENTRY.size != 0:
            raise ValueError(f"{path} has an RDF index size that is not entry-aligned")
        self.version = version
        self.index_offset = index_offset
        self.index_size = index_size
        self.entries = self._read_entries()

    def _read_entries(self) -> list[RdfEntry]:
        counts: dict[str, int] = collections.defaultdict(int)
        entries: list[RdfEntry] = []
        entry_count = self.index_size // RDF_INDEX_ENTRY.size
        for ordinal in range(entry_count):
            off = self.index_offset + ordinal * RDF_INDEX_ENTRY.size
            raw_id, comp, version, ho, hs, do, ds, us = RDF_INDEX_ENTRY.unpack_from(self._data, off)
            identifier = raw_id.split(b"\0", 1)[0].decode("utf-8", "replace")
            identifier_index = counts[identifier]
            counts[identifier] += 1
            if ho < 0 or hs < 0 or do < 0 or ds < 0:
                raise ValueError(f"chunk {ordinal} has a negative offset/size")
            if ho + hs > len(self._data) or do + ds > len(self._data):
                raise ValueError(f"chunk {ordinal} points outside the file")
            entries.append(
                RdfEntry(
                    ordinal=ordinal,
                    identifier=identifier,
                    compression=comp,
                    version=version,
                    header_offset=ho,
                    header_size=hs,
                    data_offset=do,
                    data_size=ds,
                    uncompressed_size=us,
                    identifier_index=identifier_index,
                )
            )
        return entries

    def chunk_data(self, entry: RdfEntry) -> bytes:
        data = self._data[entry.data_offset : entry.data_offset + entry.data_size]
        if entry.compression == 0:
            return data
        if entry.compression == 1:
            try:
                import zstandard  # type: ignore
            except Exception as exc:  # pragma: no cover - optional local dependency
                raise RuntimeError("zstd-compressed RDF chunk requires the zstandard Python package") from exc
            return zstandard.ZstdDecompressor().decompress(data, max_output_size=entry.uncompressed_size)
        raise RuntimeError(f"unsupported RDF chunk compression mode {entry.compression}")

    def chunk_header(self, entry: RdfEntry) -> bytes:
        return self._data[entry.header_offset : entry.header_offset + entry.header_size]


def _cstr(blob: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(blob):
        return ""
    end = blob.find(b"\0", offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode("utf-8", "replace")


def _u64_hex(value: int | None) -> str | None:
    return None if value is None else f"0x{value:016x}"


def _hash_pair_hex(pair: Sequence[int] | None) -> str | None:
    if not pair or len(pair) < 2:
        return None
    return f"0x{int(pair[0]):016x}:0x{int(pair[1]):016x}"


def _parse_elf_sections(data: bytes) -> list[dict[str, Any]]:
    if len(data) < ELF64_HEADER.size or data[:4] != b"\x7fELF":
        return []
    ident, _etype, _machine, _version, _entry, _phoff, shoff, _flags, _ehsize, _phentsize, _phnum, shentsize, shnum, shstrndx = ELF64_HEADER.unpack_from(data, 0)
    if ident[4] != 2 or ident[5] != 1:  # ELFCLASS64, little-endian
        return []
    if shoff <= 0 or shnum <= 0 or shentsize < ELF64_SECTION.size:
        return []
    if shoff + shnum * shentsize > len(data):
        return []
    raw_sections = []
    for i in range(shnum):
        raw_sections.append(ELF64_SECTION.unpack_from(data, shoff + i * shentsize))
    names = b""
    if 0 <= shstrndx < len(raw_sections):
        sh = raw_sections[shstrndx]
        names = data[sh[4] : sh[4] + sh[5]]
    sections = []
    for i, sh in enumerate(raw_sections):
        name = _cstr(names, sh[0]) if names else ""
        sections.append(
            {
                "index": i,
                "name": name,
                "type": sh[1],
                "flags": sh[2],
                "addr": sh[3],
                "offset": sh[4],
                "size": sh[5],
            }
        )
    return sections


def _iter_elf_notes(data: bytes) -> Iterable[tuple[str, int, bytes]]:
    for sec in _parse_elf_sections(data):
        if sec["type"] != 7:  # SHT_NOTE
            continue
        pos = int(sec["offset"])
        end = pos + int(sec["size"])
        while pos + ELF_NOTE_HEADER.size <= end:
            namesz, descsz, note_type = ELF_NOTE_HEADER.unpack_from(data, pos)
            pos += ELF_NOTE_HEADER.size
            if namesz == 0 and descsz == 0:
                break
            name = data[pos : pos + namesz]
            pos += (namesz + 3) & ~3
            desc = data[pos : pos + descsz]
            pos += (descsz + 3) & ~3
            yield name.rstrip(b"\0").decode("utf-8", "replace"), note_type, desc


def parse_amdgpu_metadata(data: bytes) -> dict[str, Any] | None:
    for name, _note_type, desc in _iter_elf_notes(data):
        if name != "AMDGPU":
            continue
        try:
            import msgpack  # type: ignore
        except Exception as exc:
            raise RuntimeError("AMDGPU metadata notes require the msgpack Python package") from exc
        try:
            return msgpack.unpackb(desc, raw=False, strict_map_key=False)
        except Exception:
            continue
    return None


def parse_code_object_header(header: bytes) -> dict[str, Any]:
    if len(header) < CODE_OBJECT_HEADER.size:
        return {}
    process_id, api_id, hash0, hash1 = CODE_OBJECT_HEADER.unpack_from(header, 0)
    return {
        "process_id": process_id,
        "api_id": api_id,
        "internal_pipeline_hash": [hash0, hash1],
        "internal_pipeline_hash_hex": _hash_pair_hex([hash0, hash1]),
    }


def parse_pso_correlations(rdf: RdfFile) -> dict[tuple[int, int], dict[str, Any]]:
    """Return PsoCorrelation rows keyed by internal pipeline hash.

    RGP's PsoCorrelation record layout is not documented by this project, but in
    current RGP captures the useful leading fields are stable enough for audits:
    u32 process_id, u32 api_id, u64 api_pso_hash, u64[2] internal_pipeline_hash.
    The remaining bytes are reserved/zero for the DX12 captures inspected here.
    """
    out: dict[tuple[int, int], dict[str, Any]] = {}
    for entry in rdf.entries:
        if entry.identifier != "PsoCorrelation":
            continue
        header = rdf.chunk_header(entry)
        data = rdf.chunk_data(entry)
        count = PSO_CORRELATION_HEADER.unpack_from(header, 0)[0] if len(header) >= 4 else len(data) // PSO_CORRELATION_RECORD_SIZE
        usable = min(count, len(data) // PSO_CORRELATION_RECORD_SIZE)
        for i in range(usable):
            off = i * PSO_CORRELATION_RECORD_SIZE
            process_id, api_id, api_pso_hash, hash0, hash1 = struct.unpack_from("<IIQQQ", data, off)
            key = (hash0, hash1)
            out[key] = {
                "record_index": i,
                "process_id": process_id,
                "api_id": api_id,
                "api_pso_hash": api_pso_hash,
                "api_pso_hash_hex": _u64_hex(api_pso_hash),
                "internal_pipeline_hash": [hash0, hash1],
                "internal_pipeline_hash_hex": _hash_pair_hex([hash0, hash1]),
            }
    return out


def parse_code_load_events(rdf: RdfFile) -> dict[tuple[int, int], list[dict[str, Any]]]:
    """Return COLoadEvent rows keyed by internal pipeline hash.

    The exact non-hash fields are RGP-private.  For APUSR diagnostics we expose
    them under conservative names: code_object_bytes appears to match loaded ELF
    byte size, and raw_time/raw_tail are useful for checking duplicate loads.
    """
    out: dict[tuple[int, int], list[dict[str, Any]]] = collections.defaultdict(list)
    for entry in rdf.entries:
        if entry.identifier != "COLoadEvent":
            continue
        header = rdf.chunk_header(entry)
        data = rdf.chunk_data(entry)
        count = CO_LOAD_EVENT_HEADER.unpack_from(header, 0)[0] if len(header) >= 4 else len(data) // CO_LOAD_EVENT_RECORD_SIZE
        usable = min(count, len(data) // CO_LOAD_EVENT_RECORD_SIZE)
        for i in range(usable):
            off = i * CO_LOAD_EVENT_RECORD_SIZE
            process_id, reserved0, code_object_bytes, event_kind, hash0, hash1, raw_time, raw_tail = struct.unpack_from("<IIIIQQII", data, off)
            out[(hash0, hash1)].append(
                {
                    "record_index": i,
                    "process_id": process_id,
                    "reserved0": reserved0,
                    "code_object_bytes": code_object_bytes,
                    "event_kind": event_kind,
                    "internal_pipeline_hash": [hash0, hash1],
                    "internal_pipeline_hash_hex": _hash_pair_hex([hash0, hash1]),
                    "raw_time": raw_time,
                    "raw_tail": raw_tail,
                }
            )
    return out


def summarize_code_object(entry: RdfEntry, data: bytes, *, rdf: RdfFile | None = None, pso: dict[tuple[int, int], dict[str, Any]] | None = None, loads: dict[tuple[int, int], list[dict[str, Any]]] | None = None) -> list[dict[str, Any]]:
    meta = parse_amdgpu_metadata(data)
    if not meta:
        return []
    header = parse_code_object_header(rdf.chunk_header(entry)) if rdf is not None else {}
    header_hash = header.get("internal_pipeline_hash")
    header_key = tuple(header_hash) if isinstance(header_hash, list) and len(header_hash) == 2 else None
    pso_row = pso.get(header_key, {}) if pso and header_key else {}
    load_rows = loads.get(header_key, []) if loads and header_key else []
    sections = _parse_elf_sections(data)
    text_size = sum(int(s["size"]) for s in sections if ".text" in str(s["name"]))
    out: list[dict[str, Any]] = []
    for pipe_index, pipe in enumerate(meta.get("amdpal.pipelines", [])):
        stages = pipe.get(".hardware_stages", {})
        pipe_hash = pipe.get(".internal_pipeline_hash") or header_hash
        pipe_key = tuple(pipe_hash) if isinstance(pipe_hash, list) and len(pipe_hash) == 2 else header_key
        pipe_pso = pso.get(pipe_key, pso_row) if pso and pipe_key else pso_row
        pipe_loads = loads.get(pipe_key, load_rows) if loads and pipe_key else load_rows
        for stage_name, stage in stages.items():
            if not isinstance(stage, dict):
                continue
            out.append(
                {
                    "chunk_ordinal": entry.ordinal,
                    "code_object_index": entry.identifier_index,
                    "code_object_bytes": entry.data_size,
                    "loaded_code_object_bytes": max((int(x.get("code_object_bytes", 0)) for x in pipe_loads), default=None),
                    "elf_text_bytes": text_size,
                    "pipeline_index": pipe_index,
                    "pipeline_type": pipe.get(".type"),
                    "stage": stage_name.lstrip("."),
                    "entry": stage.get(".entry_point_symbol"),
                    "threadgroup_dimensions": stage.get(".threadgroup_dimensions"),
                    "orig_threadgroup_dimensions": stage.get(".orig_threadgroup_dimensions"),
                    "wavefront_size": stage.get(".wavefront_size"),
                    "sgprs": stage.get(".sgpr_count"),
                    "vgprs": stage.get(".vgpr_count"),
                    "lds_size": stage.get(".lds_size"),
                    "scratch_memory_size": stage.get(".scratch_memory_size"),
                    "scratch_en": stage.get(".scratch_en"),
                    "user_sgprs": stage.get(".user_sgprs"),
                    "checksum": stage.get(".checksum_value"),
                    "internal_pipeline_hash": pipe_hash,
                    "internal_pipeline_hash_hex": _hash_pair_hex(pipe_hash),
                    "api_pso_hash": pipe_pso.get("api_pso_hash"),
                    "api_pso_hash_hex": pipe_pso.get("api_pso_hash_hex"),
                    "api_shader_hash": pipe.get(".shaders", {}).get(".compute", {}).get(".api_shader_hash"),
                    "co_load_event_count": len(pipe_loads),
                }
            )
    return out


def parse_workgroup(text: str | None) -> list[int] | None:
    if not text:
        return None
    parts = [p.strip() for p in text.split(",")]
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("workgroup must be X,Y,Z")
    return [int(p, 0) for p in parts]


def _sort_key(row: dict[str, Any], key: str) -> Any:
    value = row.get(key)
    if value is None:
        return -1
    if isinstance(value, list):
        return tuple(value)
    return value


def _capture_paths(inputs: Sequence[pathlib.Path], temp_root: pathlib.Path) -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for item in inputs:
        if item.is_dir():
            found.extend(sorted(p for p in item.rglob("*") if p.suffix.lower() in CAPTURE_SUFFIXES))
        elif item.suffix.lower() == ".zip":
            dst = temp_root / item.stem
            dst.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(item, "r") as zf:
                for member in zf.infolist():
                    if pathlib.PurePosixPath(member.filename).suffix.lower() in CAPTURE_SUFFIXES:
                        zf.extract(member, dst)
            found.extend(sorted(p for p in dst.rglob("*") if p.suffix.lower() in CAPTURE_SUFFIXES))
        elif item.suffix.lower() in CAPTURE_SUFFIXES:
            found.append(item)
        else:
            raise ValueError(f"unsupported input path: {item}")
    return found


def _row_matches(row: dict[str, Any], ns: argparse.Namespace) -> bool:
    stage_filter = ns.stage.lower().lstrip(".") if ns.stage else None
    if stage_filter and str(row.get("stage", "")).lower() != stage_filter:
        return False
    if ns.workgroup and row.get("threadgroup_dimensions") != ns.workgroup:
        return False
    for attr, field in (("min_lds", "lds_size"), ("min_vgprs", "vgprs"), ("min_sgprs", "sgprs")):
        v = getattr(ns, attr)
        if v is not None and (row.get(field) is None or int(row[field]) < v):
            return False
    for attr, field in (("max_lds", "lds_size"), ("max_vgprs", "vgprs"), ("max_sgprs", "sgprs")):
        v = getattr(ns, attr)
        if v is not None and (row.get(field) is None or int(row[field]) > v):
            return False
    if ns.pipeline_hash:
        needle = ns.pipeline_hash.lower().replace("-", ":")
        hay = str(row.get("internal_pipeline_hash_hex") or "").lower()
        if needle not in hay:
            return False
    if ns.api_pso_hash:
        needle = ns.api_pso_hash.lower()
        hay = str(row.get("api_pso_hash_hex") or "").lower()
        if needle not in hay:
            return False
    return True


def _csv_write(rows: list[dict[str, Any]], path: pathlib.Path | None) -> None:
    fields = [
        "capture",
        "chunk_ordinal",
        "code_object_index",
        "stage",
        "threadgroup_dimensions",
        "wavefront_size",
        "sgprs",
        "vgprs",
        "lds_size",
        "scratch_memory_size",
        "user_sgprs",
        "code_object_bytes",
        "loaded_code_object_bytes",
        "elf_text_bytes",
        "entry",
        "pipeline_type",
        "api_pso_hash_hex",
        "internal_pipeline_hash_hex",
        "co_load_event_count",
    ]
    out = sys.stdout if path is None else path.open("w", newline="")
    try:
        writer = csv.DictWriter(out, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            flat = dict(row)
            if isinstance(flat.get("threadgroup_dimensions"), list):
                flat["threadgroup_dimensions"] = "x".join(str(x) for x in flat["threadgroup_dimensions"])
            writer.writerow(flat)
    finally:
        if path is not None:
            out.close()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Summarize AMD_RDF RGP/RMV files and AMDGPU code-object metadata.")
    ap.add_argument("capture", type=pathlib.Path, nargs="+", help="Path(s) to .rgp/.rmv, directories, or zip bundles")
    ap.add_argument("--chunks", action="store_true", help="print RDF chunk counts and a short chunk table")
    ap.add_argument("--code-objects", action="store_true", help="print parsed CodeObject shader metadata")
    ap.add_argument("--pso-correlations", action="store_true", help="print decoded PsoCorrelation hash records")
    ap.add_argument("--code-loads", action="store_true", help="print decoded COLoadEvent hash/load records")
    ap.add_argument("--stage", help="filter code objects by hardware stage, e.g. CS, PS, VS")
    ap.add_argument("--workgroup", type=parse_workgroup, help="filter compute code objects by threadgroup dimensions X,Y,Z")
    ap.add_argument("--min-lds", type=int, default=None, help="filter code objects by minimum LDS bytes")
    ap.add_argument("--max-lds", type=int, default=None, help="filter code objects by maximum LDS bytes")
    ap.add_argument("--min-vgprs", type=int, default=None, help="filter code objects by minimum VGPR count")
    ap.add_argument("--max-vgprs", type=int, default=None, help="filter code objects by maximum VGPR count")
    ap.add_argument("--min-sgprs", type=int, default=None, help="filter code objects by minimum SGPR count")
    ap.add_argument("--max-sgprs", type=int, default=None, help="filter code objects by maximum SGPR count")
    ap.add_argument("--pipeline-hash", help="filter by internal pipeline hash substring")
    ap.add_argument("--api-pso-hash", help="filter by API PSO hash substring")
    ap.add_argument("--sort-by", default="chunk_ordinal", choices=("chunk_ordinal", "code_object_index", "lds_size", "vgprs", "sgprs", "code_object_bytes", "elf_text_bytes"))
    ap.add_argument("--descending", action="store_true", help="sort descending")
    ap.add_argument("--limit", type=int, default=80, help="maximum rows to print for tables")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of text")
    ap.add_argument("--csv", nargs="?", const="-", help="emit code-object table as CSV, optionally to a file")
    ns = ap.parse_args(argv)

    with tempfile.TemporaryDirectory(prefix="apusr_rgp_rdf_") as tmp:
        captures = _capture_paths(ns.capture, pathlib.Path(tmp))
        if not captures:
            raise SystemExit("no .rgp/.rmv files found")
        all_summaries: list[dict[str, Any]] = []
        all_code_rows: list[dict[str, Any]] = []
        pso_tables: list[dict[str, Any]] = []
        load_tables: list[dict[str, Any]] = []

        for cap in captures:
            rdf = RdfFile(cap)
            chunk_counts = dict(collections.Counter(e.identifier for e in rdf.entries))
            pso = parse_pso_correlations(rdf)
            loads = parse_code_load_events(rdf)
            summary = {
                "path": str(cap),
                "rdf_version": rdf.version,
                "chunk_count": len(rdf.entries),
                "chunk_counts": chunk_counts,
            }
            code_rows: list[dict[str, Any]] = []
            if ns.code_objects or ns.csv:
                for entry in rdf.entries:
                    if entry.identifier != "CodeObject":
                        continue
                    for row in summarize_code_object(entry, rdf.chunk_data(entry), rdf=rdf, pso=pso, loads=loads):
                        row["capture"] = str(cap)
                        if _row_matches(row, ns):
                            code_rows.append(row)
                code_rows.sort(key=lambda row: _sort_key(row, ns.sort_by), reverse=ns.descending)
                summary["code_objects"] = code_rows
                all_code_rows.extend(code_rows)

            if ns.pso_correlations:
                rows = list(pso.values())[: ns.limit]
                pso_tables.append({"capture": str(cap), "rows": rows, "count": len(pso)})
                summary["pso_correlation_count"] = len(pso)
            if ns.code_loads:
                flat_loads = []
                for values in loads.values():
                    flat_loads.extend(values)
                load_tables.append({"capture": str(cap), "rows": flat_loads[: ns.limit], "count": len(flat_loads)})
                summary["code_load_event_count"] = len(flat_loads)
            all_summaries.append(summary)

        if ns.csv is not None:
            all_code_rows.sort(key=lambda row: (row.get("capture", ""), _sort_key(row, ns.sort_by)), reverse=False)
            _csv_write(all_code_rows, None if ns.csv == "-" else pathlib.Path(ns.csv))
            return 0

        if ns.json:
            print(json.dumps({"captures": all_summaries, "pso_correlations": pso_tables, "code_loads": load_tables}, indent=2, sort_keys=True))
            return 0

        for summary in all_summaries:
            print(f"RDF: {summary['path']}")
            print(f"  version: {summary['rdf_version']}")
            print(f"  chunks : {summary['chunk_count']}")
            print("  chunk counts:")
            for ident, count in sorted(summary["chunk_counts"].items(), key=lambda kv: (-kv[1], kv[0])):
                print(f"    {ident:16s} {count}")

            if ns.chunks:
                rdf = RdfFile(pathlib.Path(summary["path"]))
                print("\n  chunk table:")
                for e in rdf.entries[: ns.limit]:
                    comp = "none" if e.compression == 0 else "zstd" if e.compression == 1 else str(e.compression)
                    print(
                        f"    #{e.ordinal:5d} {e.identifier:16s}[{e.identifier_index:4d}] "
                        f"v{e.version} comp={comp:4s} header={e.header_size:7d} data={e.data_size:9d}"
                    )
                if len(rdf.entries) > ns.limit:
                    print(f"    ... {len(rdf.entries) - ns.limit} more chunks")

            if ns.code_objects:
                code_rows = summary.get("code_objects", [])
                print("\n  code objects:")
                for row in code_rows[: ns.limit]:
                    tg = row.get("threadgroup_dimensions")
                    print(
                        f"    chunk=#{row['chunk_ordinal']:5d} co[{row['code_object_index']:4d}] "
                        f"stage={str(row.get('stage')):2s} tg={tg} wave={row.get('wavefront_size')} "
                        f"sgpr={row.get('sgprs')} vgpr={row.get('vgprs')} "
                        f"lds={row.get('lds_size')} scratch={row.get('scratch_memory_size')} "
                        f"pso={row.get('api_pso_hash_hex')} hash={row.get('internal_pipeline_hash_hex')}"
                    )
                if len(code_rows) > ns.limit:
                    print(f"    ... {len(code_rows) - ns.limit} more code-object stage records")
            print()

        if ns.pso_correlations:
            print("PsoCorrelation records:")
            for table in pso_tables:
                print(f"  {table['capture']} count={table['count']}")
                for row in table["rows"]:
                    print(f"    pso={row['api_pso_hash_hex']} hash={row['internal_pipeline_hash_hex']} api_id={row['api_id']}")
        if ns.code_loads:
            print("COLoadEvent records:")
            for table in load_tables:
                print(f"  {table['capture']} count={table['count']}")
                for row in table["rows"]:
                    print(f"    bytes={row['code_object_bytes']} hash={row['internal_pipeline_hash_hex']} raw_time={row['raw_time']} tail={row['raw_tail']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
