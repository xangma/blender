# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import struct
import sys
from pathlib import Path
from typing import Any

import bpy


def create_argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Build a StereoOcean scene with --dry-run and record deterministic samples from the "
            "evaluated Ocean mesh."
        )
    )
    parser.add_argument("--stereoocean-root", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--sample-count", default=65536, type=int)
    return parser


def sample_indices(size: int, sample_count: int) -> list[int]:
    if size <= 0:
        return []
    if sample_count <= 0 or size <= sample_count:
        return list(range(size))
    return sorted(
        {
            min(size - 1, max(0, round(i * (size - 1) / float(sample_count - 1))))
            for i in range(sample_count)
        }
    )


def find_ocean_object() -> bpy.types.Object:
    for obj in bpy.context.scene.objects:
        if any(mod.type == "OCEAN" for mod in obj.modifiers):
            return obj
    raise RuntimeError("No object with an Ocean modifier was found")


def vector_to_list(value: Any) -> list[float]:
    return [float(component) for component in value[:]]


def sampled_vector_attribute(mesh: bpy.types.Mesh, name: str, indices: list[int]) -> list[list[float]]:
    attr = mesh.attributes.get(name)
    if attr is None:
        return []
    data = attr.data
    values: list[list[float]] = []
    for index in indices:
        item = data[index]
        if hasattr(item, "vector"):
            values.append(vector_to_list(item.vector))
        elif hasattr(item, "color"):
            values.append(vector_to_list(item.color))
        elif hasattr(item, "value"):
            values.append([float(item.value)])
    return values


def hash_sampled_vectors(*vectors: list[list[float]]) -> str:
    hasher = hashlib.sha256()
    for vector_list in vectors:
        for vector in vector_list:
            hasher.update(struct.pack("<" + "f" * len(vector), *vector))
    return hasher.hexdigest()


def bounds_for_vectors(vectors: list[list[float]]) -> dict[str, list[float]]:
    if not vectors:
        return {"min": [], "max": []}
    width = len(vectors[0])
    mins = [math.inf] * width
    maxs = [-math.inf] * width
    for vector in vectors:
        for axis, value in enumerate(vector):
            mins[axis] = min(mins[axis], value)
            maxs[axis] = max(maxs[axis], value)
    return {"min": mins, "max": maxs}


def run_stereoocean_dry_run(stereoocean_root: Path, config_path: Path) -> None:
    if str(stereoocean_root) not in sys.path:
        sys.path.insert(0, str(stereoocean_root))

    old_argv = sys.argv[:]
    try:
        sys.argv = [
            old_argv[0],
            "--",
            "--config",
            str(config_path),
            "--dry-run",
        ]
        from stereoocean.generator import main as stereoocean_main

        stereoocean_main()
    finally:
        sys.argv = old_argv


def main() -> None:
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = create_argparser().parse_args(argv)

    run_stereoocean_dry_run(args.stereoocean_root, args.config)

    scene = bpy.context.scene
    scene.frame_set(1)
    depsgraph = bpy.context.evaluated_depsgraph_get()
    depsgraph.update()

    ocean_obj = find_ocean_object()
    evaluated = ocean_obj.evaluated_get(depsgraph)
    mesh = evaluated.to_mesh()
    try:
        indices = sample_indices(len(mesh.vertices), args.sample_count)
        positions = [vector_to_list(mesh.vertices[index].co) for index in indices]
        normals = [vector_to_list(mesh.vertices[index].normal) for index in indices]

        attribute_names = sorted(mesh.attributes.keys())
        sampled_attributes = {}
        for name in attribute_names:
            if name in {"position", ".corner_vert", ".corner_edge"}:
                continue
            attr = mesh.attributes.get(name)
            if attr is None or attr.domain != "POINT":
                continue
            if attr.data_type not in {"FLOAT", "FLOAT_VECTOR", "FLOAT_COLOR", "BYTE_COLOR"}:
                continue
            values = sampled_vector_attribute(mesh, name, indices)
            if values:
                sampled_attributes[name] = {
                    "domain": attr.domain,
                    "data_type": attr.data_type,
                    "samples": values,
                    "hash": hash_sampled_vectors(values),
                    "bounds": bounds_for_vectors(values),
                }

        result = {
            "config": str(args.config),
            "sample_count_requested": args.sample_count,
            "sample_count": len(indices),
            "object": ocean_obj.name,
            "verts": len(mesh.vertices),
            "edges": len(mesh.edges),
            "polygons": len(mesh.polygons),
            "loops": len(mesh.loops),
            "sample_indices": indices,
            "positions": positions,
            "normals": normals,
            "position_hash": hash_sampled_vectors(positions),
            "normal_hash": hash_sampled_vectors(normals),
            "position_bounds": bounds_for_vectors(positions),
            "normal_bounds": bounds_for_vectors(normals),
            "attributes": sampled_attributes,
            "attribute_names": attribute_names,
            "blender_version": bpy.app.version_string,
            "blender_build_hash": bpy.app.build_hash.decode("utf-8")
            if isinstance(bpy.app.build_hash, bytes)
            else str(bpy.app.build_hash),
            "pid": os.getpid(),
        }
    finally:
        evaluated.to_mesh_clear()

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
