# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.

#!/usr/bin/env python3
from __future__ import annotations

import csv
import subprocess
import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: apusr_profiler_compare_contract.py COMPARATOR REPORT_DIR OUTPUT_DIR", file=sys.stderr)
        return 2
    comparator, report_dir, output_dir = map(Path, sys.argv[1:])
    subprocess.run(
        [sys.executable, str(comparator), str(report_dir), str(report_dir), "-o", str(output_dir),
         "--baseline-label", "reference", "--candidate-label", "reference"],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    delta_path = output_dir / "metric_deltas.csv"
    summary_path = output_dir / "summary.md"
    distribution_path = output_dir / "distribution_deltas.csv"
    workload_identity_path = output_dir / "gpu_workload_identity_deltas.csv"
    if not delta_path.exists() or not summary_path.exists() or not distribution_path.exists() or not workload_identity_path.exists():
        print("profiler comparator did not produce expected outputs", file=sys.stderr)
        return 3
    with delta_path.open(newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    wait_rows = [row for row in rows if row["family"] == "wait" and row["name"] == "profiler.contract.wait"]
    if not wait_rows:
        print("profiler comparator did not ingest wait metrics", file=sys.stderr)
        return 4
    for row in wait_rows:
        if row["delta"] not in ("", "0.0", "0"):
            print(f"self-comparison produced a non-zero delta: {row}", file=sys.stderr)
            return 5
    state_rows = [row for row in rows if row["family"] == "state_conditioned" and
                  row["state_name"] == "framegen.enabled" and row["state_value"] == "1" and
                  row["observation_family"] == "wait" and row["name"] == "profiler.contract.wait"]
    if not state_rows or any(row["delta"] not in ("", "0.0", "0") for row in state_rows):
        print("state-conditioned self-comparison was not preserved", file=sys.stderr)
        return 7
    workload_rows = [row for row in rows if row["family"] == "gpu_workload" and
                     row["name"] == "profiler.contract.kernel"]
    if not workload_rows or any(row["delta"] not in ("", "0.0", "0") for row in workload_rows):
        print("GPU workload metadata self-comparison was not preserved", file=sys.stderr)
        return 8
    with workload_identity_path.open(newline="", encoding="utf-8") as handle:
        workload_identities = list(csv.DictReader(handle))
    identity_rows = [row for row in workload_identities if row["name"] == "profiler.contract.kernel"]
    if not identity_rows or any(row["status"] != "same" or
                                row["baseline_code_hashes"] != "0x12345678abcdef00" or
                                row["candidate_code_hashes"] != "0x12345678abcdef00"
                                for row in identity_rows):
        print("GPU workload code-hash identity self-comparison was not preserved", file=sys.stderr)
        return 9
    with distribution_path.open(newline="", encoding="utf-8") as handle:
        distributions = list(csv.DictReader(handle))
    flow_rows = [row for row in distributions if row["family"] == "flow" and row["name"] == "runtime.execute_frame"]
    if not flow_rows or any(float(row["median_delta"]) != 0.0 for row in flow_rows):
        print("distribution comparator did not preserve zero self-delta for flow samples", file=sys.stderr)
        return 6
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
