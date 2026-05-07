# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path
from typing import Any

from ocean_multi_rig_benchmark_report import parse_case, parse_comparison_file


VARIANTS = ("dense_optimized", "lod_optimized")
CAMERA_SIDES = ("Left", "Right")


def create_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Summarize a multi-rig per-commit benchmark root.")
    parser.add_argument("--artifact-root", required=True, type=Path)
    parser.add_argument("--out-md", type=Path, help="Optional Markdown output path.")
    parser.add_argument("--out-json", type=Path, help="Optional JSON output path.")
    return parser


def read_commit_plan(root: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for line in (root / "commit_plan.tsv").read_text().splitlines():
        if not line or line.startswith("short\t"):
            continue
        short, commit, note = line.split("\t")
        rows.append({"short": short, "commit": commit, "note": note})
    return rows


def summarize_comparison(commit_dir: Path, variant: str) -> dict[str, float] | None:
    reports = []
    for side in CAMERA_SIDES:
        path = (
            commit_dir
            / "comparisons"
            / f"{variant}_{side}"
            / f"{variant}_{side}_render_compare.json"
        )
        if path.exists():
            reports.append(parse_comparison_file(path))
    if not reports:
        return None
    keys = (
        "rgb_mean",
        "rgb_p95",
        "rgb_max",
        "lum_mean",
        "lum_p95",
        "lum_max",
        "grad_mean",
        "grad_p95",
        "grad_max",
    )
    return {key: statistics.mean(float(report[key]) for report in reports) for key in keys}


def build_summary(root: Path) -> dict[str, Any]:
    baseline = parse_case(root / "baseline_dense")
    rows = []
    for item in read_commit_plan(root):
        commit_dir = root / "commits" / item["short"]
        row: dict[str, Any] = {
            **item,
            "commit_dir": str(commit_dir),
            "build_exit_code": int((commit_dir / "build_exit_code.txt").read_text().strip()),
            "dense_optimized": parse_case(commit_dir / "dense_optimized"),
            "lod_optimized": parse_case(commit_dir / "lod_optimized"),
            "dense_diff": summarize_comparison(commit_dir, "dense_optimized"),
            "lod_diff": summarize_comparison(commit_dir, "lod_optimized"),
        }
        rows.append(row)
    return {"artifact_root": str(root), "baseline": baseline, "rows": rows}


def fmt_seconds(value: float | None) -> str:
    if value is None:
        return "n/a"
    return f"{value:.2f}"


def fmt_diff(diff: dict[str, float] | None) -> str:
    if diff is None:
        return "n/a"
    return f"{diff['rgb_mean']:.8f} / {diff['rgb_p95']:.6f} / {diff['rgb_max']:.6f}"


def markdown_for_summary(summary: dict[str, Any]) -> str:
    baseline = summary["baseline"]
    lines = [
        "## Generated Per-Commit Summary",
        "",
        f"Artifact root: `{summary['artifact_root']}`",
        "",
        (
            "Baseline dense reference: "
            f"{fmt_seconds(baseline.get('full_process_wall_s'))} s full wall, "
            f"{fmt_seconds(baseline.get('render_operation_wall_s'))} s render wall, "
            f"{fmt_seconds(baseline.get('peak_rss_gb'))} GB peak RSS."
        ),
        "",
        "| Commit | Change | Dense full | Dense render | LOD full | LOD render | LOD verts | LOD RGB mean / p95 / max | Dense RGB mean / p95 / max |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | --- | --- |",
    ]
    for row in summary["rows"]:
        dense = row["dense_optimized"]
        lod = row["lod_optimized"]
        lines.append(
            "| "
            + " | ".join(
                [
                    f"`{row['short']}`",
                    row["note"].replace("_", " "),
                    fmt_seconds(dense.get("full_process_wall_s")),
                    fmt_seconds(dense.get("render_operation_wall_s")),
                    fmt_seconds(lod.get("full_process_wall_s")),
                    fmt_seconds(lod.get("render_operation_wall_s")),
                    f"{int(lod.get('verts', 0)):,}" if lod.get("verts") else "n/a",
                    fmt_diff(row["lod_diff"]),
                    fmt_diff(row["dense_diff"]),
                ]
            )
            + " |"
        )

    lines.extend(
        [
            "",
            "| Commit | Dense modifier total | LOD modifier total | LOD faces | Build status |",
            "| --- | ---: | ---: | ---: | --- |",
        ]
    )
    for row in summary["rows"]:
        dense = row["dense_optimized"]
        lod = row["lod_optimized"]
        lines.append(
            "| "
            + " | ".join(
                [
                    f"`{row['short']}`",
                    fmt_seconds(dense.get("modifier_total_s")),
                    fmt_seconds(lod.get("modifier_total_s")),
                    f"{int(lod.get('faces', 0)):,}" if lod.get("faces") else "n/a",
                    str(row["build_exit_code"]),
                ]
            )
            + " |"
        )
    lines.append("")
    return "\n".join(lines)


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
