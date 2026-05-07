# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
from pathlib import Path
from typing import Any


VARIANTS = ("dense_baseline", "dense_optimized", "lod_optimized")
COMPARISON_CANDIDATES = ("dense_optimized", "lod_optimized")
CAMERA_SIDES = ("Left", "Right")


def elapsed_to_seconds(value: str) -> float:
    parts = value.strip().split(":")
    if len(parts) == 2:
        return int(parts[0]) * 60.0 + float(parts[1])
    if len(parts) == 3:
        return int(parts[0]) * 3600.0 + int(parts[1]) * 60.0 + float(parts[2])
    return float(value)


def parse_time_file(path: Path) -> dict[str, float]:
    text = path.read_text(errors="replace")
    patterns = {
        "user_s": r"User time \(seconds\):\s*([0-9.]+)",
        "system_s": r"System time \(seconds\):\s*([0-9.]+)",
        "cpu_percent": r"Percent of CPU this job got:\s*([0-9.]+)%",
        "elapsed": r"Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*([^\n]+)",
        "max_rss_kb": r"Maximum resident set size \(kbytes\):\s*(\d+)",
    }
    result: dict[str, float] = {}
    for key, pattern in patterns.items():
        match = re.search(pattern, text)
        if match is None:
            continue
        value = match.group(1).strip()
        if key == "elapsed":
            result["full_process_wall_s"] = elapsed_to_seconds(value)
        elif key == "max_rss_kb":
            result["peak_rss_gb"] = float(value) / 1024.0 / 1024.0
        else:
            result[key] = float(value)
    return result


def parse_render_log(path: Path) -> dict[str, float]:
    text = path.read_text(errors="replace")
    entries: list[tuple[float, float | None]] = []
    for match in re.finditer(
        r"Time: (\d\d):(\d\d)\.(\d\d)(?: \(Saving: (\d\d):(\d\d)\.(\d\d)\))?", text
    ):
        total_s = int(match.group(1)) * 60.0 + int(match.group(2)) + int(match.group(3)) / 100.0
        saving_s = None
        if match.group(4) is not None:
            saving_s = (
                int(match.group(4)) * 60.0 + int(match.group(5)) + int(match.group(6)) / 100.0
            )
        entries.append((total_s, saving_s))
    if not entries:
        return {}
    render_s, saving_s = entries[-1]
    result = {"render_operation_wall_s": render_s}
    if saving_s is not None:
        result["render_saving_s"] = saving_s
    return result


def parse_profile_entries(path: Path) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    for line in path.read_text(errors="replace").splitlines():
        if "OCEAN_CAMERA_LOD_PROFILE" not in line:
            continue
        entry: dict[str, Any] = {}
        for key, value in re.findall(r"(\w+)=([^\s]+)", line):
            value = value.strip("'")
            if re.fullmatch(r"-?\d+", value):
                entry[key] = int(value)
                continue
            try:
                entry[key] = float(value)
            except ValueError:
                entry[key] = value
        entries.append(entry)
    return entries


def find_variant_runs(root: Path, variant: str) -> list[Path]:
    variant_dir = root / variant
    if (variant_dir / "time.txt").exists():
        return [variant_dir]
    return sorted(path for path in variant_dir.glob("run_*") if (path / "time.txt").exists())


def run_id_for_case(case_dir: Path) -> str:
    if case_dir.name.startswith("run_"):
        return case_dir.name
    return "run_01"


def parse_case(case_dir: Path) -> dict[str, Any]:
    metadata = json.loads((case_dir / "output" / "metadata.json").read_text())
    performance = metadata.get("performance", {})
    component_times = performance.get("component_times", {})
    ocean_component = component_times.get("ocean_system", {})

    result: dict[str, Any] = {
        "run_id": run_id_for_case(case_dir),
        "case_dir": str(case_dir),
        "mode": metadata.get("ocean", {}).get("evaluation_mode", ""),
        "frame_wall_s": float(performance.get("total_time", 0.0)),
        "ocean_component_s": float(ocean_component.get("total_time", 0.0)),
        "seed": metadata.get("config", {}).get("random_seed")
        or metadata.get("system", {}).get("random_seed"),
    }
    result.update(parse_time_file(case_dir / "time.txt"))
    result.update(parse_render_log(case_dir / "blender.log"))

    profile_entries = parse_profile_entries(case_dir / "blender.log")
    result["profile_entries"] = profile_entries
    modifier_entries = [entry for entry in profile_entries if entry.get("stage") == "modifier"]
    if modifier_entries:
        result["modifier_eval_count"] = len(modifier_entries)
        result["modifier_total_s"] = sum(float(entry.get("total_s", 0.0)) for entry in modifier_entries)
        result["modifier_mean_s"] = result["modifier_total_s"] / len(modifier_entries)
        last_modifier = modifier_entries[-1]
        result["verts"] = last_modifier.get("verts")
        result["faces"] = last_modifier.get("faces")
    return result


def parse_comparison_file(path: Path) -> dict[str, Any]:
    data = json.loads(path.read_text())
    report = data["report"]
    return {
        "rgb_mean": report["rgb_absolute_error"]["mean"],
        "rgb_p95": report["rgb_absolute_error"]["p95"],
        "rgb_max": report["rgb_absolute_error"]["max"],
        "lum_mean": report["luminance_absolute_error"]["mean"],
        "lum_p95": report["luminance_absolute_error"]["p95"],
        "lum_max": report["luminance_absolute_error"]["max"],
        "grad_mean": report["luminance_gradient_error"]["mean"],
        "grad_p95": report["luminance_gradient_error"]["p95"],
        "grad_max": report["luminance_gradient_error"]["max"],
    }


def find_comparison_path(root: Path, run_id: str, candidate: str, side: str) -> Path | None:
    nested = root / "comparisons" / run_id / f"{candidate}_{side}" / f"{candidate}_{side}_render_compare.json"
    if nested.exists():
        return nested
    flat = root / "comparisons" / f"{candidate}_{side}" / f"{candidate}_{side}_render_compare.json"
    if flat.exists():
        return flat
    return None


def parse_comparisons(root: Path, run_ids: list[str]) -> dict[str, list[dict[str, Any]]]:
    comparisons: dict[str, list[dict[str, Any]]] = {candidate: [] for candidate in COMPARISON_CANDIDATES}
    for run_id in run_ids:
        for candidate in COMPARISON_CANDIDATES:
            side_reports = []
            for side in CAMERA_SIDES:
                path = find_comparison_path(root, run_id, candidate, side)
                if path is not None:
                    side_reports.append({"side": side, **parse_comparison_file(path)})
            if side_reports:
                averaged = {"run_id": run_id}
                for key in (
                    "rgb_mean",
                    "rgb_p95",
                    "rgb_max",
                    "lum_mean",
                    "lum_p95",
                    "lum_max",
                    "grad_mean",
                    "grad_p95",
                    "grad_max",
                ):
                    averaged[key] = statistics.mean(float(report[key]) for report in side_reports)
                averaged["sides"] = side_reports
                comparisons[candidate].append(averaged)
    return comparisons


def summarize(values: list[float]) -> dict[str, float]:
    if not values:
        return {"count": 0.0, "mean": 0.0, "min": 0.0, "max": 0.0, "stddev": 0.0}
    return {
        "count": float(len(values)),
        "mean": statistics.mean(values),
        "min": min(values),
        "max": max(values),
        "stddev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def summarize_cases(cases: list[dict[str, Any]]) -> dict[str, Any]:
    keys = (
        "full_process_wall_s",
        "frame_wall_s",
        "render_operation_wall_s",
        "ocean_component_s",
        "peak_rss_gb",
        "modifier_total_s",
        "modifier_mean_s",
    )
    summary: dict[str, Any] = {}
    for key in keys:
        summary[key] = summarize([float(case[key]) for case in cases if key in case])
    summary["runs"] = len(cases)
    summary["mode"] = cases[0].get("mode", "") if cases else ""
    summary["seed"] = cases[0].get("seed") if cases else None
    for key in ("verts", "faces"):
        values = [case.get(key) for case in cases if case.get(key) is not None]
        if values:
            summary[key] = int(round(statistics.mean(values)))
    return summary


def fmt_stat(summary: dict[str, float], digits: int = 2) -> str:
    if summary["count"] == 0:
        return "n/a"
    return (
        f"{summary['mean']:.{digits}f} "
        f"(min {summary['min']:.{digits}f}, max {summary['max']:.{digits}f}, "
        f"sd {summary['stddev']:.{digits}f})"
    )


def fmt_number(value: float, digits: int = 2) -> str:
    if math.isfinite(value):
        return f"{value:.{digits}f}"
    return "n/a"


def build_summary(root: Path) -> dict[str, Any]:
    cases_by_variant = {
        variant: [parse_case(case_dir) for case_dir in find_variant_runs(root, variant)]
        for variant in VARIANTS
    }
    run_ids = sorted({case["run_id"] for cases in cases_by_variant.values() for case in cases})
    comparisons = parse_comparisons(root, run_ids)
    return {
        "artifact_root": str(root),
        "run_ids": run_ids,
        "variants": cases_by_variant,
        "variant_summaries": {
            variant: summarize_cases(cases) for variant, cases in cases_by_variant.items()
        },
        "comparisons": comparisons,
    }


def markdown_for_summary(summary: dict[str, Any]) -> str:
    variant_summaries = summary["variant_summaries"]
    lines = [
        "## Generated Repeat Summary",
        "",
        f"Artifact root: `{summary['artifact_root']}`",
        "",
        "| Variant | Runs | Mode | Full process wall | Generator frame wall | Render operation wall | Ocean component | Peak RSS |",
        "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for variant in VARIANTS:
        item = variant_summaries.get(variant, {})
        lines.append(
            "| "
            + " | ".join(
                [
                    variant,
                    str(item.get("runs", 0)),
                    f"`{item.get('mode', '')}`",
                    fmt_stat(item.get("full_process_wall_s", summarize([]))),
                    fmt_stat(item.get("frame_wall_s", summarize([]))),
                    fmt_stat(item.get("render_operation_wall_s", summarize([]))),
                    fmt_stat(item.get("ocean_component_s", summarize([]))),
                    fmt_stat(item.get("peak_rss_gb", summarize([]))),
                ]
            )
            + " |"
        )

    baseline = variant_summaries["dense_baseline"]["full_process_wall_s"]["mean"]
    dense = variant_summaries["dense_optimized"]["full_process_wall_s"]["mean"]
    lod = variant_summaries["lod_optimized"]["full_process_wall_s"]["mean"]
    lines.extend(
        [
            "",
            "| Comparison | Full wall delta | Full wall speedup |",
            "| --- | ---: | ---: |",
            f"| Dense optimized vs dense baseline | {dense - baseline:.2f} s | {baseline / dense:.2f}x |",
            f"| LOD optimized vs dense baseline | {lod - baseline:.2f} s | {baseline / lod:.2f}x |",
            f"| LOD optimized vs dense optimized | {lod - dense:.2f} s | {dense / lod:.2f}x |",
            "",
            "| Candidate | Runs | RGB mean / p95 / max | Luminance mean / p95 / max | Luminance-gradient mean / p95 / max |",
            "| --- | ---: | --- | --- | --- |",
        ]
    )
    for candidate, reports in summary["comparisons"].items():
        rgb_mean = summarize([report["rgb_mean"] for report in reports])["mean"]
        rgb_p95 = summarize([report["rgb_p95"] for report in reports])["mean"]
        rgb_max = summarize([report["rgb_max"] for report in reports])["mean"]
        lum_mean = summarize([report["lum_mean"] for report in reports])["mean"]
        lum_p95 = summarize([report["lum_p95"] for report in reports])["mean"]
        lum_max = summarize([report["lum_max"] for report in reports])["mean"]
        grad_mean = summarize([report["grad_mean"] for report in reports])["mean"]
        grad_p95 = summarize([report["grad_p95"] for report in reports])["mean"]
        grad_max = summarize([report["grad_max"] for report in reports])["mean"]
        lines.append(
            "| "
            + " | ".join(
                [
                    candidate,
                    str(len(reports)),
                    f"{rgb_mean:.8f} / {rgb_p95:.6f} / {rgb_max:.6f}",
                    f"{lum_mean:.8f} / {lum_p95:.6f} / {lum_max:.6f}",
                    f"{grad_mean:.8f} / {grad_p95:.6f} / {grad_max:.6f}",
                ]
            )
            + " |"
        )
    lines.append("")
    return "\n".join(lines)


def create_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Summarize StereoOcean multi-rig benchmark artifacts.")
    parser.add_argument("--artifact-root", required=True, type=Path)
    parser.add_argument("--out-md", type=Path, help="Optional Markdown output path.")
    parser.add_argument("--out-json", type=Path, help="Optional JSON summary output path.")
    return parser


def main() -> None:
    args = create_argparser().parse_args()
    summary = build_summary(args.artifact_root)
    markdown = markdown_for_summary(summary)
    if args.out_md:
        args.out_md.write_text(markdown)
    else:
        print(markdown)
    if args.out_json:
        args.out_json.write_text(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
