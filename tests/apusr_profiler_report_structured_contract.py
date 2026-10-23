# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
from __future__ import annotations

import csv
import subprocess
import sys
from pathlib import Path


def rows(path: Path) -> list[dict[str, str]]:
    if not path.exists() or path.stat().st_size == 0:
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: apusr_profiler_report_structured_contract.py REPORTER TRACE_DIR OUTPUT_DIR", file=sys.stderr)
        return 2
    reporter, trace_dir, output_dir = map(Path, sys.argv[1:])
    subprocess.run(
        [sys.executable, str(reporter), str(trace_dir), "-o", str(output_dir), "--timeline"],
        check=True,
        stdout=subprocess.DEVNULL,
    )

    decisions = rows(output_dir / "decisions.csv")
    if not any(row["name"] == "framegen.source_disposition" and row["outcome"] == "warmup" and
               row["reason"] == "history_gap" and row["triggered_burst"] == "True" for row in decisions):
        print("structured decision was not decoded correctly", file=sys.stderr)
        return 3

    states = rows(output_dir / "state_transitions.csv")
    if not any(row["name"] == "framegen.enabled" and row["previous_value"] == "0" and
               row["next_value"] == "1" for row in states):
        print("state transition was not decoded correctly", file=sys.stderr)
        return 4

    balances = rows(output_dir / "resource_balance.csv")
    contract_balances = [row for row in balances if row["name"] == "framegen.effect_resource"]
    if len(contract_balances) != 2 or len({row["stream_id"] for row in contract_balances}) != 2 or any(
            row["opens"] != "1" or row["closes"] != "1" or row["outstanding_count"] != "0" or
            row["unmatched_close_count"] != "0" for row in contract_balances):
        print("resource lifetime ledgers were not isolated by stream identity", file=sys.stderr)
        return 5

    display = rows(output_dir / "display_timing.csv")
    sync_row = next((row for row in display if row["frame_id"] == "18"), None)
    present_row = next((row for row in display if row["frame_id"] == "19"), None)
    if sync_row is None or sync_row.get("actual_sync_qpc") != "123456" or present_row is None or \
            present_row.get("present_begin_to_sync_ms") != "2.0":
        print("heterogeneous display timing rows were not written with a union schema", file=sys.stderr)
        return 24

    flows = rows(output_dir / "flow_stats.csv")
    if not any(row["name"] == "runtime.execute_frame" and row["terminal_phase"] == "end" and
               row["occurrence_id"] == "1701" and row["complete"] == "True" for row in flows):
        print("logical flow was not reconstructed correctly", file=sys.stderr)
        return 6

    threads = rows(output_dir / "threads.csv")
    if not any(row["name"] == "Profiler Contract Worker" and row["role"] == "test.worker" for row in threads):
        print("thread metadata was not decoded correctly", file=sys.stderr)
        return 7

    waits = rows(output_dir / "wait_intervals.csv")
    if not any(row["name"] == "profiler.contract.wait" and row["kind"] == "gpu_fence" and
               row["result"] == "satisfied" and row["value"] == "9" for row in waits):
        print("wait interval was not decoded correctly", file=sys.stderr)
        return 8

    wait_stats = rows(output_dir / "wait_reason_stats.csv")
    if not any(row["name"] == "profiler.contract.wait" and row["samples"] == "1" for row in wait_stats):
        print("wait statistics were not generated", file=sys.stderr)
        return 9

    wait_dependencies = rows(output_dir / "wait_dependencies.csv")
    if not any(row["name"] == "profiler.contract.wait" and row["signal_found"] == "True" and
               row["signal_name"] == "profiler.contract.signal" for row in wait_dependencies):
        print("wait-to-queue dependency was not reconstructed", file=sys.stderr)
        return 10

    metadata = rows(output_dir / "metadata.csv")
    if not any(row["name"] == "profiler.contract.metadata" and row["detail"] == "contract-value" and
               row["value0"] == "42" for row in metadata):
        print("metadata record was not decoded correctly", file=sys.stderr)
        return 11

    workloads = rows(output_dir / "gpu_workloads.csv")
    if not any(row["name"] == "profiler.contract.kernel" and row["code_bytes"] == "8192" and
               row["subgroup_size"] == "64" and row["threads_x"] == "8" and row["threads_y"] == "8"
               for row in workloads):
        print("GPU workload metadata was not decoded correctly", file=sys.stderr)
        return 12

    frames = rows(output_dir / "frames.csv")
    if not any(row["stream_name"] == "profiler.contract" and row["gpu_duration_valid"] == "False" and
               row["gpu_timing_status"] == "raw_ticks_without_frequency" for row in frames):
        print("zero-frequency frame GPU timing was not marked invalid", file=sys.stderr)
        return 19

    gpu_intervals = rows(output_dir / "gpu_intervals.csv")
    if not any(row["name"] == "profiler.contract.invalid_frequency" and row["gpu_duration_valid"] == "False" and
               row["gpu_timing_status"] == "raw_ticks_without_frequency" for row in gpu_intervals):
        print("zero-frequency GPU interval was not marked invalid", file=sys.stderr)
        return 20

    hotspots = rows(output_dir / "hotspot_stats.csv")
    if any(row["name"] == "profiler.contract.invalid_frequency" for row in hotspots):
        print("invalid zero-frequency GPU timing leaked into hotspot statistics", file=sys.stderr)
        return 21

    workload_stats = rows(output_dir / "gpu_workload_stats.csv")
    if not any(row["name"] == "profiler.contract.kernel" and row["code_bytes_max"] == "8192" and
               row["subgroup_sizes"] == "64" and "8x8x1" in row["thread_shapes"] for row in workload_stats):
        print("GPU workload statistics were not generated", file=sys.stderr)
        return 13

    lifetimes = rows(output_dir / "resource_lifetimes.csv")
    if not any(row["name"] == "framegen.effect_resource" and row["closed"] == "True" and
               row["problem"] == "" for row in lifetimes):
        print("resource lifetime report was not generated", file=sys.stderr)
        return 12

    outcomes = rows(output_dir / "frame_outcomes.csv")
    if not any(row["frame_id"] == "17" and row["terminal_outcome"] == "warmup" and
               row["terminal_reason"] == "history_gap" and row["explicit_wait_count"] == "1" and
               row["flow_terminal_count"] == "1" for row in outcomes):
        print("per-frame structured outcome index was not generated", file=sys.stderr)
        return 13

    counts = rows(output_dir / "record_counts.csv")
    seen = {row["record_type"] for row in counts}
    required = {"decision", "state_transition", "resource_event", "flow_event",
                "thread_info", "wait_interval", "metadata", "gpu_workload_info"}
    if not required.issubset(seen):
        print(f"record count report missing {sorted(required - seen)}", file=sys.stderr)
        return 14

    coverage = rows(output_dir / "stream_coverage.csv")
    if not any(row["stream_name"] == "profiler.contract" and row["decision_records"] == "1" and
               row["wait_interval_records"] == "1" and row["resource_event_records"] == "2" and
               row["gpu_workload_info_records"] == "1" and row["capability_manifest_present"] == "True"
               for row in coverage):
        print("per-stream instrumentation coverage was not generated", file=sys.stderr)
        return 15

    capabilities = rows(output_dir / "capability_coverage.csv")
    contract_caps = [row for row in capabilities if row["stream_name"] == "profiler.contract"]
    if not contract_caps or any(row["status"] == "observed_undeclared" for row in contract_caps) or not any(
            row["capability"] == "gpu_workload_info" and row["status"] == "observed" for row in contract_caps):
        print("stream capability manifest/coverage was not reconstructed", file=sys.stderr)
        return 18

    conditioned = rows(output_dir / "state_conditioned_stats.csv")
    if not any(row["state_name"] == "framegen.enabled" and row["state_value"] == "1" and
               row["family"] == "wait" and row["name"] == "profiler.contract.wait" for row in conditioned):
        print("state-conditioned performance statistics were not generated", file=sys.stderr)
        return 16

    quality = rows(output_dir / "capture_quality.csv")
    if not quality or not (output_dir / "report_index.md").exists():
        print("capture quality/report index was not generated", file=sys.stderr)
        return 17
    if not any(row["stream_name"] == "profiler.contract" and row["check"] == "gpu_clock_calibration" and
               row["status"] == "warning" and "raw ticks but no timestamp frequency" in row["detail"]
               for row in quality):
        print("zero-frequency GPU timing was not surfaced as a capture-quality warning", file=sys.stderr)
        return 22
    manifest_rows = [row for row in quality if row["stream_name"] == "profiler.contract" and row["check"] == "capability_manifest"]
    if len(manifest_rows) != 2 or len({row.get("stream_id", "") for row in manifest_rows}) != 2:
        print("capture-quality rows collapsed same-name streams instead of preserving stream identity", file=sys.stderr)
        return 25

    pipeline_activity = rows(output_dir / "vulkan_pipeline_activity.csv")
    compute = next((row for row in pipeline_activity if row.get("operation") == "vk.create_compute_pipelines"), None)
    shader = next((row for row in pipeline_activity if row.get("operation") == "vk.create_shader_module"), None)
    cache = next((row for row in pipeline_activity if row.get("operation") == "vk.create_pipeline_cache"), None)
    if compute is None or compute.get("create_count") != "2" or compute.get("pipeline_cache") != "" or \
            compute.get("caller_module") != "contract-host.exe":
        print("Vulkan pipeline activity did not preserve no-cache creation/callsite attribution", file=sys.stderr)
        return 26
    if shader is None or shader.get("bytes") != "4096" or shader.get("code_hash") != "0x12345678abcdef00":
        print("Vulkan shader-module activity/hash attribution was not generated", file=sys.stderr)
        return 27
    if cache is None or cache.get("bytes") != "1024" or cache.get("flags") != "1":
        print("Vulkan pipeline-cache activity was not generated", file=sys.stderr)
        return 28

    conditioned = rows(output_dir / "state_conditioned_stats.csv")
    if not any(row.get("state_name") == "streamline.reflex.mode" and row.get("state_value") == "1" and
               row.get("name") == "reflex.d3d12.update" and row.get("family") == "cpu" for row in conditioned):
        print("Reflex state-conditioned CPU profiling was not generated", file=sys.stderr)
        return 29

    summary = (output_dir / "summary.txt").read_text(encoding="utf-8")
    if "Vulkan shader/pipeline/cache activity" not in summary or \
            "Reflex / Anti-Lag conditioned CPU wall time (mode=on)" not in summary:
        print("optimization-focused profiler summary sections were not generated", file=sys.stderr)
        return 30
    if "profiler.contract.compute" not in summary or "profiler.contract.prepare" not in summary:
        print("frame-phase streams were collapsed in the human profiler summary", file=sys.stderr)
        return 31
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
