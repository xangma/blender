# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any


def create_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Compare two ocean_multi_rig_mesh_probe JSON files.")
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    return parser


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = (len(ordered) - 1) * fraction
    lower = int(math.floor(index))
    upper = int(math.ceil(index))
    if lower == upper:
        return ordered[lower]
    blend = index - lower
    return ordered[lower] * (1.0 - blend) + ordered[upper] * blend


def stats(values: list[float]) -> dict[str, float]:
    if not values:
        return {"count": 0, "mean": 0.0, "p95": 0.0, "max": 0.0}
    return {
        "count": len(values),
        "mean": sum(values) / len(values),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


def vector_distance(a: list[float], b: list[float]) -> float:
    return math.sqrt(sum((float(x) - float(y)) ** 2 for x, y in zip(a, b)))


def angle_degrees(a: list[float], b: list[float]) -> float:
    dot = sum(float(x) * float(y) for x, y in zip(a, b))
    len_a = math.sqrt(sum(float(x) * float(x) for x in a))
    len_b = math.sqrt(sum(float(x) * float(x) for x in b))
    if len_a == 0.0 or len_b == 0.0:
        return 0.0
    cosine = max(-1.0, min(1.0, dot / (len_a * len_b)))
    return math.degrees(math.acos(cosine))


def compare_vectors(reference: list[list[float]], candidate: list[list[float]]) -> dict[str, Any]:
    if len(reference) != len(candidate):
        raise RuntimeError(f"Sample lengths differ: {len(reference)} vs {len(candidate)}")
    distances = [vector_distance(a, b) for a, b in zip(reference, candidate)]
    component_abs = [
        abs(float(x) - float(y))
        for a, b in zip(reference, candidate)
        for x, y in zip(a, b)
    ]
    return {
        "distance": stats(distances),
        "component_abs": stats(component_abs),
    }


def compare_meshes(reference: dict[str, Any], candidate: dict[str, Any]) -> dict[str, Any]:
    if reference["sample_indices"] != candidate["sample_indices"]:
        raise RuntimeError("Probe sample indices differ")

    result: dict[str, Any] = {
        "reference": {
            "config": reference.get("config"),
            "verts": reference.get("verts"),
            "polygons": reference.get("polygons"),
            "build_hash": reference.get("blender_build_hash"),
        },
        "candidate": {
            "config": candidate.get("config"),
            "verts": candidate.get("verts"),
            "polygons": candidate.get("polygons"),
            "build_hash": candidate.get("blender_build_hash"),
        },
        "sample_count": len(reference["sample_indices"]),
        "topology_matches": reference.get("verts") == candidate.get("verts")
        and reference.get("edges") == candidate.get("edges")
        and reference.get("polygons") == candidate.get("polygons")
        and reference.get("loops") == candidate.get("loops"),
        "position_hash_matches": reference.get("position_hash") == candidate.get("position_hash"),
        "normal_hash_matches": reference.get("normal_hash") == candidate.get("normal_hash"),
        "positions": compare_vectors(reference["positions"], candidate["positions"]),
        "normals": compare_vectors(reference["normals"], candidate["normals"]),
        "normal_angle_deg": stats(
            [angle_degrees(a, b) for a, b in zip(reference["normals"], candidate["normals"])]
        ),
    }

    ref_attrs = set(reference.get("attributes", {}).keys())
    cand_attrs = set(candidate.get("attributes", {}).keys())
    result["attributes"] = {
        "reference_only": sorted(ref_attrs - cand_attrs),
        "candidate_only": sorted(cand_attrs - ref_attrs),
        "common": {},
    }
    for name in sorted(ref_attrs & cand_attrs):
        result["attributes"]["common"][name] = {
            "hash_matches": reference["attributes"][name].get("hash")
            == candidate["attributes"][name].get("hash"),
            **compare_vectors(
                reference["attributes"][name]["samples"],
                candidate["attributes"][name]["samples"],
            ),
        }
    return result


def main() -> None:
    args = create_argparser().parse_args()
    reference = json.loads(args.reference.read_text())
    candidate = json.loads(args.candidate.read_text())
    result = compare_meshes(reference, candidate)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
