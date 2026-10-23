#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 thomthehound.
"""Compare two APUSR profiler report directories without repo/game assumptions.

The comparison consumes the stable CSVs emitted by apusr_profile_report.py and
produces one normalized metric-delta table plus a compact Markdown summary.
It deliberately keys measurements by backend/component/stream/configuration
and semantic event name; labels are experiment names supplied by the caller,
not inferred repo versions.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
import math
import random
import statistics
from pathlib import Path


def read_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists() or path.stat().st_size == 0:
        return []
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def number(row: dict[str, str], key: str) -> float | None:
    text = row.get(key, "")
    if text in (None, ""):
        return None
    try:
        value = float(text)
    except ValueError:
        return None
    return value if math.isfinite(value) else None




def percentile(values: list[float], p: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * p
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] * (hi - pos) + ordered[hi] * (pos - lo)


def frame_configuration_map(report: Path) -> dict[tuple[str, str, str, str], str]:
    out: dict[tuple[str, str, str, str], str] = {}
    for row in read_rows(report / "frames.csv"):
        key = (row.get("process_id", ""), row.get("module", ""), row.get("stream_id", ""), row.get("frame_id", ""))
        out[key] = row.get("configuration_hash", "")
    return out


def collect_distributions(report: Path) -> dict[tuple[str, tuple[str, ...], str], list[float]]:
    distributions: dict[tuple[str, tuple[str, ...], str], list[float]] = {}
    frame_cfg = frame_configuration_map(report)

    def add(family: str, key: tuple[str, ...], metric: str, value: float | None) -> None:
        if value is None:
            return
        distributions.setdefault((family, key, metric), []).append(value)

    for row in read_rows(report / "gpu_intervals.csv"):
        cfg = frame_cfg.get((row.get("process_id", ""), row.get("module", ""), row.get("stream_id", ""), row.get("frame_id", "")), "")
        key = (row.get("backend", ""), row.get("component", ""), row.get("stream_name", ""), cfg, row.get("name", ""))
        add("gpu_hotspot", key, "gpu_ms", number(row, "gpu_ms"))
        add("gpu_hotspot", key, "execution_ms", number(row, "execution_ms"))
        add("gpu_hotspot", key, "setup_ms", number(row, "setup_ms"))

    for row in read_rows(report / "cpu_exclusive.csv"):
        key = (row.get("backend", ""), row.get("component", ""), row.get("stream_name", ""), row.get("configuration_hash", ""), row.get("name", ""))
        add("cpu_scope", key, "exclusive_ms", number(row, "exclusive_ms"))
        add("cpu_scope", key, "inclusive_ms", number(row, "inclusive_ms"))

    for row in read_rows(report / "frames.csv"):
        key = (row.get("backend", ""), row.get("component", ""), row.get("stream_name", ""), row.get("configuration_hash", ""))
        add("frame", key, "gpu_ms", number(row, "gpu_ms"))
        add("frame", key, "cpu_total_ms", number(row, "cpu_total_ms"))
        add("frame", key, "cpu_record_ms", number(row, "cpu_record_ms"))

    for row in read_rows(report / "wait_intervals.csv"):
        key = (row.get("backend", ""), row.get("component", ""), row.get("stream_name", ""), row.get("name", ""),
               row.get("kind", ""), row.get("result", ""), row.get("detail", ""))
        add("wait", key, "duration_ms", number(row, "duration_ms"))

    for row in read_rows(report / "flow_stats.csv"):
        key = (row.get("backend", ""), row.get("component", ""), row.get("stream_name", ""), row.get("name", ""),
               row.get("terminal_phase", ""), row.get("terminal_detail", ""))
        add("flow", key, "duration_ms", number(row, "duration_ms"))

    return distributions


def distribution_descriptor(family: str, key: tuple[str, ...]) -> dict[str, str]:
    desc = {"backend": "", "component": "", "stream_name": "", "configuration_hash": "", "name": "",
            "kind": "", "result": "", "detail": "", "terminal_phase": "", "terminal_detail": ""}
    if family in ("gpu_hotspot", "cpu_scope"):
        desc.update(zip(("backend", "component", "stream_name", "configuration_hash", "name"), key))
    elif family == "frame":
        desc.update(zip(("backend", "component", "stream_name", "configuration_hash"), key))
    elif family == "wait":
        desc.update(zip(("backend", "component", "stream_name", "name", "kind", "result", "detail"), key))
    elif family == "flow":
        desc.update(zip(("backend", "component", "stream_name", "name", "terminal_phase", "terminal_detail"), key))
    return desc


def common_language_probability_slower(base: list[float], candidate: list[float]) -> float:
    if not base or not candidate:
        return 0.5
    ordered = sorted(base)
    score = 0.0
    for value in candidate:
        less = bisect.bisect_left(ordered, value)
        less_or_equal = bisect.bisect_right(ordered, value)
        ties = less_or_equal - less
        score += less + 0.5 * ties
    return score / (len(base) * len(candidate))


def bootstrap_median_delta(
    base: list[float], candidate: list[float], iterations: int, seed_text: str
) -> tuple[float, float, float | None, float | None]:
    if not base or not candidate or iterations <= 0:
        return 0.0, 0.0, None, None
    seed = int.from_bytes(hashlib.sha256(seed_text.encode("utf-8")).digest()[:8], "little")
    rng = random.Random(seed)
    deltas: list[float] = []
    pct_deltas: list[float] = []
    for _ in range(iterations):
        b = [base[rng.randrange(len(base))] for _ in range(len(base))]
        c = [candidate[rng.randrange(len(candidate))] for _ in range(len(candidate))]
        bm = statistics.median(b)
        cm = statistics.median(c)
        delta = cm - bm
        deltas.append(delta)
        if bm != 0.0:
            pct_deltas.append(delta * 100.0 / abs(bm))
    return (percentile(deltas, 0.025), percentile(deltas, 0.975),
            percentile(pct_deltas, 0.025) if pct_deltas else None,
            percentile(pct_deltas, 0.975) if pct_deltas else None)


def identity(row: dict[str, str], keys: tuple[str, ...]) -> tuple[str, ...]:
    return tuple(row.get(key, "") for key in keys)


def collect_metric(
    report: Path,
    filename: str,
    keys: tuple[str, ...],
    metrics: tuple[str, ...],
    family: str,
) -> dict[tuple[str, tuple[str, ...], str], float]:
    out: dict[tuple[str, tuple[str, ...], str], float] = {}
    for row in read_rows(report / filename):
        key = identity(row, keys)
        for metric in metrics:
            value = number(row, metric)
            if value is not None:
                out[(family, key, metric)] = value
    return out


def collect(report: Path) -> tuple[dict, dict]:
    values: dict[tuple[str, tuple[str, ...], str], float] = {}
    descriptors: dict[tuple[str, tuple[str, ...]], dict[str, str]] = {}

    specs = [
        ("hotspot_stats.csv", "gpu_hotspot",
         ("backend", "component", "stream_name", "configuration_hash", "name"),
         ("gpu_median_ms", "gpu_p95_ms", "execution_median_ms", "setup_median_ms")),
        ("cpu_exclusive_stats.csv", "cpu_scope",
         ("backend", "component", "stream_name", "configuration_hash", "name"),
         ("exclusive_median_ms", "exclusive_p95_ms", "inclusive_median_ms")),
        ("frame_stats.csv", "frame",
         ("backend", "component", "stream_name", "configuration_hash"),
         ("gpu_median_ms", "gpu_p95_ms", "cpu_total_median_ms", "cpu_record_median_ms")),
        ("wait_reason_stats.csv", "wait",
         ("backend", "component", "stream_name", "name", "kind", "result", "detail"),
         ("median_ms", "p95_ms", "p99_ms", "total_ms")),
        ("flow_summary.csv", "flow",
         ("backend", "component", "stream_name", "name", "terminal_phase", "terminal_detail"),
         ("median_ms", "p95_ms", "p99_ms", "max_ms")),
        ("wsi_stats.csv", "wsi",
         ("backend", "component", "stream_name", "virtualized", "format", "present_mode"),
         ("acquire_mean_ms", "acquire_p95_ms", "present_mean_ms", "present_p95_ms",
          "acquire_interval_mean_ms", "present_interval_mean_ms")),
        ("decision_stats.csv", "decision",
         ("backend", "component", "stream_name", "name", "outcome", "reason"),
         ("percent", "count")),
        ("resource_balance.csv", "resource",
         ("backend", "component", "stream_name", "name"),
         ("peak_active_bytes", "outstanding_bytes", "outstanding_count", "unmatched_close_count")),
        ("gpu_workload_stats.csv", "gpu_workload",
         ("backend", "component", "stream_name", "name"),
         ("code_bytes_median", "code_bytes_max", "shared_memory_bytes_max", "variants")),
        ("state_conditioned_stats.csv", "state_conditioned",
         ("state_name", "state_subject", "state_value", "backend", "component", "stream_name",
          "family", "name", "metric"),
         ("mean", "median", "p95", "p99", "max", "total")),
    ]
    for filename, family, keys, metrics in specs:
        rows = read_rows(report / filename)
        for row in rows:
            key = identity(row, keys)
            descriptors[(family, key)] = {name: row.get(name, "") for name in keys}
            for metric in metrics:
                value = number(row, metric)
                if value is not None:
                    values[(family, key, metric)] = value
    return values, descriptors


def delta_percent(base: float, candidate: float) -> float | None:
    if base == 0.0:
        return None
    return (candidate - base) * 100.0 / abs(base)


def collect_gpu_workload_identities(report: Path) -> dict[tuple[str, ...], dict[str, str]]:
    rows = read_rows(report / "gpu_workload_stats.csv")
    identities: dict[tuple[str, ...], dict[str, str]] = {}
    for row in rows:
        key = identity(row, ("backend", "component", "stream_name", "name"))
        identities[key] = {
            "backend": row.get("backend", ""),
            "component": row.get("component", ""),
            "stream_name": row.get("stream_name", ""),
            "name": row.get("name", ""),
            "code_hashes": row.get("code_hashes", ""),
            "thread_shapes": row.get("thread_shapes", ""),
            "subgroup_sizes": row.get("subgroup_sizes", ""),
            "details": row.get("details", ""),
        }
    return identities


def compare_gpu_workload_identities(baseline: Path, candidate: Path) -> list[dict]:
    base = collect_gpu_workload_identities(baseline)
    cand = collect_gpu_workload_identities(candidate)
    rows: list[dict] = []
    for key in sorted(set(base) | set(cand)):
        b = base.get(key)
        c = cand.get(key)
        desc = c or b or {}
        if b is None:
            status = "candidate_only"
        elif c is None:
            status = "baseline_only"
        else:
            stable_fields = ("code_hashes", "thread_shapes", "subgroup_sizes", "details")
            status = "same" if all(b.get(field, "") == c.get(field, "") for field in stable_fields) else "changed"
        rows.append({
            "backend": desc.get("backend", ""),
            "component": desc.get("component", ""),
            "stream_name": desc.get("stream_name", ""),
            "name": desc.get("name", ""),
            "status": status,
            "baseline_code_hashes": "" if b is None else b.get("code_hashes", ""),
            "candidate_code_hashes": "" if c is None else c.get("code_hashes", ""),
            "baseline_thread_shapes": "" if b is None else b.get("thread_shapes", ""),
            "candidate_thread_shapes": "" if c is None else c.get("thread_shapes", ""),
            "baseline_subgroup_sizes": "" if b is None else b.get("subgroup_sizes", ""),
            "candidate_subgroup_sizes": "" if c is None else c.get("subgroup_sizes", ""),
            "baseline_details": "" if b is None else b.get("details", ""),
            "candidate_details": "" if c is None else c.get("details", ""),
        })
    return rows


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two APUSR profiler report directories.")
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--baseline-label", default="baseline")
    parser.add_argument("--candidate-label", default="candidate")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--bootstrap", type=int, default=1000,
                        help="deterministic bootstrap iterations for raw-distribution median deltas (0 disables)")
    args = parser.parse_args()

    base, base_desc = collect(args.baseline)
    cand, cand_desc = collect(args.candidate)
    all_keys = sorted(set(base) | set(cand))
    rows: list[dict] = []
    for family, entity_key, metric in all_keys:
        b = base.get((family, entity_key, metric))
        c = cand.get((family, entity_key, metric))
        desc = cand_desc.get((family, entity_key)) or base_desc.get((family, entity_key)) or {}
        delta = (c - b) if b is not None and c is not None else None
        pct = delta_percent(b, c) if b is not None and c is not None else None
        row = {
            "family": family,
            "backend": desc.get("backend", ""),
            "component": desc.get("component", ""),
            "stream_name": desc.get("stream_name", ""),
            "configuration_hash": desc.get("configuration_hash", ""),
            "name": desc.get("name", ""),
            "kind": desc.get("kind", ""),
            "result": desc.get("result", ""),
            "outcome": desc.get("outcome", ""),
            "reason": desc.get("reason", ""),
            "detail": desc.get("detail", ""),
            "terminal_phase": desc.get("terminal_phase", ""),
            "terminal_detail": desc.get("terminal_detail", ""),
            "virtualized": desc.get("virtualized", ""),
            "format": desc.get("format", ""),
            "present_mode": desc.get("present_mode", ""),
            "state_name": desc.get("state_name", ""),
            "state_subject": desc.get("state_subject", ""),
            "state_value": desc.get("state_value", ""),
            "observation_family": desc.get("family", ""),
            "observation_metric": desc.get("metric", ""),
            "metric": metric,
            "baseline_label": args.baseline_label,
            "candidate_label": args.candidate_label,
            "baseline_value": "" if b is None else b,
            "candidate_value": "" if c is None else c,
            "delta": "" if delta is None else delta,
            "delta_percent": "" if pct is None else pct,
            "present_in_baseline": b is not None,
            "present_in_candidate": c is not None,
        }
        rows.append(row)

    args.output.mkdir(parents=True, exist_ok=True)
    write_csv(args.output / "metric_deltas.csv", rows)

    workload_identity_rows = compare_gpu_workload_identities(args.baseline, args.candidate)
    write_csv(args.output / "gpu_workload_identity_deltas.csv", workload_identity_rows)

    base_distributions = collect_distributions(args.baseline)
    candidate_distributions = collect_distributions(args.candidate)
    distribution_rows: list[dict] = []
    for family, entity_key, metric in sorted(set(base_distributions) | set(candidate_distributions)):
        base_values = base_distributions.get((family, entity_key, metric), [])
        candidate_values = candidate_distributions.get((family, entity_key, metric), [])
        if not base_values or not candidate_values:
            continue
        desc = distribution_descriptor(family, entity_key)
        base_median = statistics.median(base_values)
        candidate_median = statistics.median(candidate_values)
        delta = candidate_median - base_median
        pct = delta_percent(base_median, candidate_median)
        ci_low, ci_high, pct_low, pct_high = bootstrap_median_delta(
            base_values, candidate_values, max(args.bootstrap, 0), f"{family}|{entity_key}|{metric}")
        if args.bootstrap <= 0:
            direction = "not_computed"
        elif ci_low > 0.0:
            direction = "slower"
        elif ci_high < 0.0:
            direction = "faster"
        else:
            direction = "mixed"
        distribution_rows.append({
            "family": family, **desc, "metric": metric,
            "baseline_label": args.baseline_label, "candidate_label": args.candidate_label,
            "baseline_samples": len(base_values), "candidate_samples": len(candidate_values),
            "baseline_median": base_median, "candidate_median": candidate_median,
            "median_delta": delta, "median_delta_percent": "" if pct is None else pct,
            "bootstrap_iterations": max(args.bootstrap, 0),
            "median_delta_ci95_low": "" if args.bootstrap <= 0 else ci_low,
            "median_delta_ci95_high": "" if args.bootstrap <= 0 else ci_high,
            "median_delta_percent_ci95_low": "" if pct_low is None else pct_low,
            "median_delta_percent_ci95_high": "" if pct_high is None else pct_high,
            "probability_candidate_slower": common_language_probability_slower(base_values, candidate_values),
            "directional_evidence": direction,
        })
    write_csv(args.output / "distribution_deltas.csv", distribution_rows)

    comparable = [row for row in rows if row["delta_percent"] != ""]
    # Timing-oriented metrics use positive delta as regression. Keep count/percent
    # rows visible but do not pretend a higher decision count is universally bad.
    timing = [row for row in comparable if row["family"] in {"gpu_hotspot", "cpu_scope", "frame", "wait", "flow", "wsi", "state_conditioned"}]
    regressions = sorted((row for row in timing if float(row["delta_percent"]) > 0.0),
                         key=lambda row: float(row["delta_percent"]), reverse=True)
    improvements = sorted((row for row in timing if float(row["delta_percent"]) < 0.0),
                          key=lambda row: float(row["delta_percent"]))

    md = [
        "# APUSR profiler comparison",
        "",
        f"Baseline: `{args.baseline_label}`  ",
        f"Candidate: `{args.candidate_label}`",
        "",
        "Comparisons are keyed by semantic profiler identity rather than repository version or application name.",
        "Positive timing deltas are slower; negative timing deltas are faster.",
        "",
    ]

    def section(title: str, selected: list[dict]) -> None:
        md.append(f"## {title}")
        md.append("")
        md.append("| family | backend | component | name | metric | baseline | candidate | delta % |")
        md.append("|---|---|---|---|---:|---:|---:|---:|")
        for row in selected[: max(args.top, 0)]:
            md.append(
                f"| {row['family']} | {row['backend']} | {row['component']} | {row['name']} | "
                f"{row['metric']} | {float(row['baseline_value']):.4f} | {float(row['candidate_value']):.4f} | "
                f"{float(row['delta_percent']):+.2f}% |"
            )
        if not selected:
            md.append("| _none_ | | | | | | | |")
        md.append("")

    section("Largest timing regressions", regressions)
    section("Largest timing improvements", improvements)

    evidence = [row for row in distribution_rows if row["directional_evidence"] in ("slower", "faster")]
    evidence.sort(key=lambda row: abs(float(row["median_delta_percent"])) if row["median_delta_percent"] != "" else abs(float(row["median_delta"])), reverse=True)
    md.append("## Distribution-backed median changes")
    md.append("")
    md.append("The 95% intervals below are deterministic non-parametric bootstrap intervals over the retained samples. "
              "They describe sampling uncertainty in this capture pair; they do not compensate for uncontrolled run-to-run differences.")
    md.append("")
    md.append("| direction | family | backend | component | name | metric | n base/cand | median delta | 95% CI | P(candidate slower) |")
    md.append("|---|---|---|---|---|---|---:|---:|---:|---:|")
    for row in evidence[: max(args.top, 0)]:
        md.append(
            f"| {row['directional_evidence']} | {row['family']} | {row['backend']} | {row['component']} | {row['name']} | "
            f"{row['metric']} | {row['baseline_samples']}/{row['candidate_samples']} | {float(row['median_delta']):+.4f} | "
            f"[{float(row['median_delta_ci95_low']):+.4f}, {float(row['median_delta_ci95_high']):+.4f}] | "
            f"{100.0 * float(row['probability_candidate_slower']):.1f}% |")
    if not evidence:
        md.append("| _none with directional bootstrap evidence_ | | | | | | | | | |")
    md.append("")

    state_rows = [row for row in comparable if row["family"] == "state_conditioned" and float(row["delta_percent"]) != 0.0]
    state_rows.sort(key=lambda row: abs(float(row["delta_percent"])), reverse=True)
    md.append("## State-conditioned timing changes")
    md.append("")
    md.append("These rows compare the same semantic observation while a durable profiler state had the same value in both reports. "
              "State membership is assigned at the host observation timestamp; inspect the timeline for asynchronous work that crosses a transition boundary.")
    md.append("")
    md.append("| state | subject | value | backend | component | observation | measured statistic | delta % |")
    md.append("|---|---|---|---|---|---|---|---:|")
    for row in state_rows[: max(args.top, 0)]:
        observation = "/".join(part for part in (row["observation_family"], row["name"], row["observation_metric"]) if part)
        md.append(
            f"| {row['state_name']} | {row['state_subject']} | {row['state_value']} | {row['backend']} | "
            f"{row['component']} | {observation} | {row['metric']} | {float(row['delta_percent']):+.2f}% |")
    if not state_rows:
        md.append("| _none_ | | | | | | | |")
    md.append("")

    workload_rows = [row for row in comparable if row["family"] == "gpu_workload" and float(row["delta_percent"]) != 0.0]
    workload_rows.sort(key=lambda row: abs(float(row["delta_percent"])), reverse=True)
    md.append("## Static GPU workload changes")
    md.append("")
    md.append("Pipeline/code metadata is reported separately from timing so compiler or shader changes remain visible even when timing samples are sparse.")
    md.append("")
    md.append("| backend | component | stream | workload | metric | baseline | candidate | delta % |")
    md.append("|---|---|---|---|---|---:|---:|---:|")
    for row in workload_rows[: max(args.top, 0)]:
        md.append(
            f"| {row['backend']} | {row['component']} | {row['stream_name']} | {row['name']} | {row['metric']} | "
            f"{float(row['baseline_value']):.4f} | {float(row['candidate_value']):.4f} | {float(row['delta_percent']):+.2f}% |")
    if not workload_rows:
        md.append("| _none_ | | | | | | | |")
    md.append("")

    md.append("## GPU workload identity")
    md.append("")
    md.append("This compares stable shader/pipeline metadata retained by APUSR. Matching code hashes prove that the same recorded shader bytecode was used; they do not expose AMD native ISA, register allocation, or occupancy.")
    md.append("")
    md.append("| status | backend | component | stream | workload | baseline code hash | candidate code hash |")
    md.append("|---|---|---|---|---|---|---|")
    identity_changes = [row for row in workload_identity_rows if row["status"] != "same"]
    if identity_changes:
        for row in identity_changes[: max(args.top, 0)]:
            md.append(
                f"| {row['status']} | {row['backend']} | {row['component']} | {row['stream_name']} | {row['name']} | "
                f"{row['baseline_code_hashes']} | {row['candidate_code_hashes']} |")
    elif workload_identity_rows:
        md.append(f"| same | _all {len(workload_identity_rows)} comparable workloads_ | | | | | |")
    else:
        md.append("| _no GPU workload identity metadata_ | | | | | | |")
    md.append("")
    md.append("Full identity comparison: `gpu_workload_identity_deltas.csv` (code hashes, thread-group shapes, subgroup sizes, and workload details).")
    md.append("")
    (args.output / "summary.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print(f"wrote {len(rows)} metric deltas to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
