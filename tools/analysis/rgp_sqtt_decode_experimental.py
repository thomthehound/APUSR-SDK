#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Experimental SQTT/thread-trace decode harness for RGP SqttData chunks.

The ROCprof Trace Decoder is meant for ROCprof ATT/thread-trace payloads.  RGP
captures contain related SQTT payloads in AMD_RDF ``SqttData`` chunks.  This
script extracts those chunks and can attempt to decode them with
librocprof-trace-decoder.  Treat decoded output as provisional until it is
cross-checked against RGP's Instruction Timing pane.

Practical status: the decoder library builds cleanly from rocm-systems, but full
raw RGP SqttData chunks can be very slow or hang in current experiments.  Use
``--decode-timeout-sec`` and ``--max-bytes`` while probing.
"""
from __future__ import annotations

import argparse
import ctypes as C
import csv
import json
import multiprocessing as mp
import pathlib
import subprocess
import sys
import tempfile
import zipfile
from collections import defaultdict
from typing import Any

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import rgp_rdf_summary as rdfmod  # noqa: E402

SUCCESS = 0
ERR_OOM = 2
ERR_INVALID_ARGUMENT = 3

RECORD_NAMES = {
    0: "GFXIP",
    1: "OCCUPANCY",
    2: "PERFEVENT",
    3: "WAVE",
    4: "INFO",
    5: "EVENT",
    6: "SHADERDATA",
    7: "REALTIME",
    8: "RT_FREQUENCY",
    9: "INST_OTHER_SIMD",
    10: "DISPATCH",
}

INST_CATEGORIES = {
    0: "NONE",
    1: "SMEM",
    2: "SALU",
    3: "VMEM",
    4: "FLAT",
    5: "LDS",
    6: "VALU",
    7: "JUMP",
    8: "NEXT",
    9: "IMMED",
    10: "CONTEXT",
    11: "MESSAGE",
    12: "BVH",
}


class Handle(C.Structure):
    _fields_ = [("handle", C.c_uint64)]


class Pc(C.Structure):
    _fields_ = [("address", C.c_uint64), ("code_object_id", C.c_uint64)]


class Inst(C.Structure):
    _fields_ = [("category_stall", C.c_uint32), ("duration", C.c_int32), ("time", C.c_int64), ("pc", Pc)]

    @property
    def category(self) -> int:
        return int(self.category_stall & 0xFF)

    @property
    def stall(self) -> int:
        return int((self.category_stall >> 8) & 0xFFFFFF)


class WaveState(C.Structure):
    _fields_ = [("type", C.c_int32), ("duration", C.c_int32)]


class Wave(C.Structure):
    _fields_ = [
        ("cu", C.c_uint8),
        ("simd", C.c_uint8),
        ("wave_id", C.c_uint8),
        ("contexts", C.c_uint8),
        ("dispatcher", C.c_uint8),
        ("workgroup_id", C.c_uint8),
        ("reserved", C.c_uint16),
        ("size", C.c_uint64),
        ("begin_time", C.c_int64),
        ("end_time", C.c_int64),
        ("timeline_size", C.c_uint64),
        ("instructions_size", C.c_uint64),
        ("timeline_array", C.POINTER(WaveState)),
        ("instructions_array", C.POINTER(Inst)),
    ]


class Occupancy(C.Structure):
    _fields_ = [("pc", Pc), ("time", C.c_uint64), ("reserved", C.c_uint8), ("cu", C.c_uint8), ("simd", C.c_uint8), ("wave_id", C.c_uint8), ("bits", C.c_uint32)]


class DecodeState:
    def __init__(self) -> None:
        self.wave_rows: list[dict[str, Any]] = []
        self.inst_rows: list[dict[str, Any]] = []
        self.occ_rows: list[dict[str, Any]] = []
        self.info_rows: list[dict[str, Any]] = []
        self.record_counts: dict[str, int] = defaultdict(int)


def _capture_paths(inputs: list[pathlib.Path], temp_root: pathlib.Path) -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for item in inputs:
        if item.is_dir():
            found.extend(sorted(p for p in item.rglob("*") if p.suffix.lower() == ".rgp"))
        elif item.suffix.lower() == ".zip":
            dst = temp_root / item.stem
            dst.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(item, "r") as zf:
                for member in zf.infolist():
                    if pathlib.PurePosixPath(member.filename).suffix.lower() == ".rgp":
                        zf.extract(member, dst)
            found.extend(sorted(p for p in dst.rglob("*.rgp")))
        elif item.suffix.lower() == ".rgp":
            found.append(item)
        else:
            raise ValueError(f"unsupported input path: {item}")
    return found


def _sqtt_chunks(capture: pathlib.Path) -> list[tuple[int, int, bytes]]:
    rdf = rdfmod.RdfFile(capture)
    return [(e.ordinal, e.identifier_index, rdf.chunk_data(e)) for e in rdf.entries if e.identifier == "SqttData"]


def _csv(path: pathlib.Path, rows: list[dict[str, Any]]) -> None:
    fields = sorted({k for r in rows for k in r.keys()})
    with path.open("w", newline="", encoding="utf-8") as f:
        if not fields:
            f.write("")
            return
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader(); w.writerows(rows)


def _configure_lib(lib_path: pathlib.Path):
    lib = C.CDLL(str(lib_path))
    TRACE_CB = C.CFUNCTYPE(C.c_int, C.c_int, C.c_void_p, C.c_uint64, C.c_void_p)
    ISA_CB = C.CFUNCTYPE(C.c_int, C.c_char_p, C.POINTER(C.c_uint64), C.POINTER(C.c_uint64), Pc, C.c_void_p)
    lib.rocprof_trace_decoder_create_handle.argtypes = [C.POINTER(Handle)]
    lib.rocprof_trace_decoder_create_handle.restype = C.c_int
    lib.rocprof_trace_decoder_destroy_handle.argtypes = [Handle]
    lib.rocprof_trace_decoder_destroy_handle.restype = C.c_int
    lib.rocprof_trace_decoder_set_isa_callback.argtypes = [Handle, ISA_CB, C.c_void_p]
    lib.rocprof_trace_decoder_set_isa_callback.restype = C.c_int
    lib.rocprof_trace_decoder_parse.argtypes = [Handle, C.c_void_p, C.c_uint64, TRACE_CB, C.c_void_p]
    lib.rocprof_trace_decoder_parse.restype = C.c_int
    if hasattr(lib, "rocprof_trace_decoder_get_status_string"):
        lib.rocprof_trace_decoder_get_status_string.argtypes = [C.c_int]
        lib.rocprof_trace_decoder_get_status_string.restype = C.c_char_p
    return lib, TRACE_CB, ISA_CB


def _decode_buffer(lib_path: pathlib.Path, data: bytes, out_dir: pathlib.Path, *, prefix: str) -> dict[str, Any]:
    lib, TRACE_CB, ISA_CB = _configure_lib(lib_path)
    state = DecodeState()

    handle = Handle()
    status = int(lib.rocprof_trace_decoder_create_handle(C.byref(handle)))
    if status != SUCCESS:
        raise RuntimeError(f"rocprof_trace_decoder_create_handle failed: {status}")

    @ISA_CB
    def isa_cb(instruction: C.c_char_p, memory_size_p: C.POINTER(C.c_uint64), size_p: C.POINTER(C.c_uint64), _address: Pc, _userdata: C.c_void_p) -> int:
        # Minimal fixed-width placeholder.  Accurate disassembly/liveness comes
        # from RGA over the extracted code object; this callback exists so the
        # trace decoder can emit PC-indexed records without COMGR/LLVM.
        text = b"s_nop 0"
        need = len(text) + 1
        if size_p[0] < need:
            size_p[0] = need
            return ERR_OOM
        C.memmove(instruction, text, need)
        size_p[0] = len(text)
        memory_size_p[0] = 4
        return SUCCESS

    status = int(lib.rocprof_trace_decoder_set_isa_callback(handle, isa_cb, None))
    if status != SUCCESS:
        raise RuntimeError(f"rocprof_trace_decoder_set_isa_callback failed: {status}")

    @TRACE_CB
    def trace_cb(record_type: int, trace_events: C.c_void_p, trace_size: int, _userdata: C.c_void_p) -> int:
        name = RECORD_NAMES.get(int(record_type), f"TYPE_{record_type}")
        state.record_counts[name] += int(trace_size)
        if trace_events is None or trace_size == 0:
            return SUCCESS
        if record_type == 3:  # WAVE
            waves = C.cast(trace_events, C.POINTER(Wave))
            for wi in range(int(trace_size)):
                w = waves[wi]
                wave_index = len(state.wave_rows)
                state.wave_rows.append({
                    "wave_index": wave_index,
                    "cu": int(w.cu), "simd": int(w.simd), "wave_id": int(w.wave_id),
                    "dispatcher": int(w.dispatcher), "workgroup_id": int(w.workgroup_id),
                    "begin_time": int(w.begin_time), "end_time": int(w.end_time),
                    "instructions": int(w.instructions_size), "timeline": int(w.timeline_size),
                })
                prev_time = int(w.begin_time)
                for ii in range(int(w.instructions_size)):
                    inst = w.instructions_array[ii]
                    if inst.pc.code_object_id == 0 and inst.pc.address == 0:
                        continue
                    idle = max(int(inst.time) - prev_time, 0)
                    prev_time = max(prev_time, int(inst.time) + int(inst.duration))
                    state.inst_rows.append({
                        "wave_index": wave_index,
                        "instruction_index": ii,
                        "cu": int(w.cu), "simd": int(w.simd), "wave_id": int(w.wave_id),
                        "time": int(inst.time),
                        "duration": int(inst.duration),
                        "stall": int(inst.stall),
                        "active": int(inst.duration) - int(inst.stall),
                        "idle": idle,
                        "category_id": int(inst.category),
                        "category": INST_CATEGORIES.get(int(inst.category), str(int(inst.category))),
                        "pc_address": int(inst.pc.address),
                        "code_object_id": int(inst.pc.code_object_id),
                    })
        elif record_type == 1:  # OCCUPANCY
            occs = C.cast(trace_events, C.POINTER(Occupancy))
            for i in range(int(trace_size)):
                o = occs[i]
                state.occ_rows.append({
                    "time": int(o.time), "cu": int(o.cu), "simd": int(o.simd), "wave_id": int(o.wave_id),
                    "start": int(o.bits & 1), "pc_address": int(o.pc.address), "code_object_id": int(o.pc.code_object_id),
                })
        elif record_type == 4:  # INFO
            vals = C.cast(trace_events, C.POINTER(C.c_int32))
            for i in range(int(trace_size)):
                state.info_rows.append({"info": int(vals[i])})
        return SUCCESS

    data_buf = (C.c_uint8 * len(data)).from_buffer_copy(data)
    status = int(lib.rocprof_trace_decoder_parse(handle, data_buf, len(data), trace_cb, None))
    desc = ""
    if hasattr(lib, "rocprof_trace_decoder_get_status_string"):
        raw = lib.rocprof_trace_decoder_get_status_string(status)
        desc = raw.decode("utf-8", "replace") if raw else ""
    lib.rocprof_trace_decoder_destroy_handle(handle)

    by_pc: dict[tuple[int, int], dict[str, Any]] = {}
    for r in state.inst_rows:
        key = (int(r["code_object_id"]), int(r["pc_address"]))
        row = by_pc.setdefault(key, {
            "code_object_id": key[0], "pc_address": key[1], "hitcount": 0,
            "latency": 0, "stall": 0, "active": 0, "idle": 0,
        })
        row["hitcount"] += 1
        row["latency"] += int(r["duration"])
        row["stall"] += int(r["stall"])
        row["active"] += int(r["active"])
        row["idle"] += int(r["idle"])
        row["category"] = r["category"]
    agg = sorted(by_pc.values(), key=lambda x: int(x["latency"]), reverse=True)

    _csv(out_dir / f"{prefix}_decoded_waves.csv", state.wave_rows)
    _csv(out_dir / f"{prefix}_decoded_instructions_raw.csv", state.inst_rows)
    _csv(out_dir / f"{prefix}_decoded_occupancy.csv", state.occ_rows)
    _csv(out_dir / f"{prefix}_decoded_info.csv", state.info_rows)
    _csv(out_dir / f"{prefix}_decoded_instruction_latency_by_pc.csv", agg)

    return {
        "status": status,
        "status_string": desc,
        "record_counts": dict(state.record_counts),
        "waves": len(state.wave_rows),
        "instructions": len(state.inst_rows),
        "occupancy_records": len(state.occ_rows),
        "unique_pcs": len(agg),
    }


def _decode_worker(lib_path: str, data_path: str, out_dir: str, prefix: str, q: mp.Queue) -> None:
    try:
        data = pathlib.Path(data_path).read_bytes()
        summary = _decode_buffer(pathlib.Path(lib_path), data, pathlib.Path(out_dir), prefix=prefix)
        q.put({"ok": True, "summary": summary})
    except BaseException as exc:  # child boundary: report everything
        q.put({"ok": False, "error": repr(exc)})


def _maybe_build_decoder(ns: argparse.Namespace) -> pathlib.Path | None:
    if ns.decoder_lib:
        return ns.decoder_lib.resolve()
    if not ns.rocm_systems_root and not ns.rocm_systems_archive:
        return None
    out = ns.decoder_build_dir
    cmd = [sys.executable, str(SCRIPT_DIR / "rocprof_trace_decoder_build.py"), "--out-dir", str(out)]
    if ns.rocm_systems_root:
        cmd += ["--rocm-systems-root", str(ns.rocm_systems_root)]
    else:
        cmd += ["--rocm-systems-archive", str(ns.rocm_systems_archive)]
    subprocess.run(cmd, check=True)
    report = json.loads((out / "rocprof_trace_decoder_build.json").read_text(encoding="utf-8"))
    return pathlib.Path(report["library"]).resolve()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Experimental decode of RGP SqttData chunks via ROCprof Trace Decoder.")
    ap.add_argument("capture", type=pathlib.Path, nargs="+", help=".rgp files, directories, or zip bundles")
    ap.add_argument("--decoder-lib", type=pathlib.Path, help="path to librocprof-trace-decoder.so/.dll")
    ap.add_argument("--rocm-systems-root", type=pathlib.Path, help="build decoder from a rocm-systems checkout")
    ap.add_argument("--rocm-systems-archive", type=pathlib.Path, help="build decoder from a rocm-systems zip/tar archive")
    ap.add_argument("--decoder-build-dir", type=pathlib.Path, default=pathlib.Path("build/rocprof_trace_decoder"))
    ap.add_argument("--out-dir", type=pathlib.Path, default=pathlib.Path("rgp_sqtt_decode"))
    ap.add_argument("--chunk-index", type=int, action="append", help="decode only these SqttData indices; default writes all raw chunks but decodes all if a decoder is provided")
    ap.add_argument("--max-bytes", type=int, help="truncate each SqttData chunk before decode; useful for probing")
    ap.add_argument("--decode-timeout-sec", type=float, default=0.0, help="kill per-chunk decode after this many seconds; 0 disables timeout")
    ns = ap.parse_args(argv)
    ns.out_dir.mkdir(parents=True, exist_ok=True)
    decoder_lib = _maybe_build_decoder(ns)

    with tempfile.TemporaryDirectory(prefix="apusr_sqtt_decode_") as tmp:
        captures = _capture_paths(ns.capture, pathlib.Path(tmp))
        if not captures:
            raise SystemExit("no .rgp files found")
        reports = []
        for cap in captures:
            chunks = _sqtt_chunks(cap)
            cap_dir = ns.out_dir / cap.stem
            cap_dir.mkdir(parents=True, exist_ok=True)
            for ordinal, idx, data in chunks:
                raw = data[: ns.max_bytes] if ns.max_bytes else data
                raw_path = cap_dir / f"sqtt_{idx:03d}_chunk_{ordinal:05d}.bin"
                raw_path.write_bytes(raw)
                selected = ns.chunk_index is None or idx in set(ns.chunk_index)
                base = {
                    "capture": str(cap), "chunk_ordinal": ordinal, "sqtt_index": idx,
                    "sqtt_bytes_full": len(data), "sqtt_bytes_used": len(raw), "raw_path": str(raw_path),
                }
                if not decoder_lib or not selected:
                    reports.append({**base, "status": "extracted_only" if not decoder_lib else "skipped"})
                    continue
                prefix = f"sqtt_{idx:03d}_chunk_{ordinal:05d}"
                if ns.decode_timeout_sec and ns.decode_timeout_sec > 0:
                    q: mp.Queue = mp.Queue()
                    p = mp.Process(target=_decode_worker, args=(str(decoder_lib), str(raw_path), str(cap_dir), prefix, q))
                    p.start(); p.join(ns.decode_timeout_sec)
                    if p.is_alive():
                        p.terminate(); p.join(5)
                        reports.append({**base, "status": "decode_timeout", "timeout_sec": ns.decode_timeout_sec})
                        continue
                    if not q.empty():
                        result = q.get()
                    else:
                        result = {"ok": False, "error": f"child exited with code {p.exitcode}"}
                    if result.get("ok"):
                        reports.append({**base, **result["summary"], "status": "decoded"})
                    else:
                        reports.append({**base, "status": "decode_exception", "error": result.get("error", "unknown")})
                else:
                    try:
                        summary = _decode_buffer(decoder_lib, raw, cap_dir, prefix=prefix)
                        reports.append({**base, **summary, "status": "decoded"})
                    except Exception as exc:
                        reports.append({**base, "status": "decode_exception", "error": repr(exc)})
    (ns.out_dir / "decode_reports.json").write_text(json.dumps(reports, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(reports, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
