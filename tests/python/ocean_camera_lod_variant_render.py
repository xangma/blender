# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import os
import sys
import time
from dataclasses import asdict
from pathlib import Path

sys.path.append(os.path.dirname(os.path.realpath(__file__)))

import bpy

from modules import ocean_camera_lod_metrics as ocean_metrics


def create_argparser():
    parser = argparse.ArgumentParser(
        description="Render or evaluate one ocean benchmark scenario as dense or camera LOD."
    )
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--scenario", default="calm_reference")
    parser.add_argument("--variant", choices={"dense", "lod"}, required=True)
    parser.add_argument("--device", default="OPTIX")
    parser.add_argument("--samples", default=4, type=int)
    parser.add_argument("--resolution", default=64, type=int)
    parser.add_argument("--ocean-resolution", default=64, type=int)
    parser.add_argument("--repeat-eval", default=3, type=int)
    parser.add_argument("--repeat-render", default=1, type=int)
    return parser


def configure_variant(obj, variant):
    mod = obj.modifiers["Ocean"]
    if variant == "lod":
        if not hasattr(mod, "use_camera_lod"):
            raise RuntimeError("This Blender build does not expose Ocean camera LOD")
        mod.use_camera_lod = True
    elif hasattr(mod, "use_camera_lod"):
        mod.use_camera_lod = False


def evaluated_mesh_counts(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()
    try:
        return {
            "verts": len(mesh_eval.vertices),
            "faces": len(mesh_eval.polygons),
            "attributes": sorted(attr.name for attr in mesh_eval.attributes),
        }
    finally:
        obj_eval.to_mesh_clear()


def summarize_values(values):
    return asdict(ocean_metrics.summarize_values(values))


def render_variant(obj, image_path, samples, resolution, device):
    temp_path = image_path.with_name(f"{image_path.stem}_tmp{image_path.suffix}")
    ocean_metrics.render_with_cycles(str(temp_path), samples=samples, resolution=resolution, device=device)
    width, height, _rgb = ocean_metrics.materialize_render_output(str(temp_path), str(image_path))
    for path in ocean_metrics.render_output_paths(str(temp_path)):
        path.unlink(missing_ok=True)
    return {"path": str(image_path), "width": width, "height": height}


def main():
    parser = create_argparser()
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = parser.parse_args(argv)

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    previous_ocean_resolution = ocean_metrics.OCEAN_RESOLUTION_OVERRIDE
    ocean_metrics.OCEAN_RESOLUTION_OVERRIDE = args.ocean_resolution
    previous_assert_camera_lod_attributes = ocean_metrics.assert_camera_lod_attributes
    ocean_metrics.assert_camera_lod_attributes = lambda _obj: None
    try:
        context = ocean_metrics.setup_curated_scenario(args.scenario)
    finally:
        ocean_metrics.OCEAN_RESOLUTION_OVERRIDE = previous_ocean_resolution
        ocean_metrics.assert_camera_lod_attributes = previous_assert_camera_lod_attributes

    obj = bpy.data.objects[context.obj_name]
    configure_variant(obj, args.variant)

    eval_times = []
    mesh_counts = None
    for _index in range(args.repeat_eval):
        start = time.perf_counter()
        mesh_counts = evaluated_mesh_counts(obj)
        eval_times.append(time.perf_counter() - start)

    render_times = []
    render_artifact = None
    if args.repeat_render > 0:
        for render_index in range(args.repeat_render):
            image_path = outdir / (
                f"{args.scenario}_{args.variant}.png"
                if render_index == args.repeat_render - 1 else
                f"{args.scenario}_{args.variant}_repeat_{render_index + 1}.png"
            )
            start = time.perf_counter()
            render_artifact = render_variant(
                obj,
                image_path,
                samples=args.samples,
                resolution=args.resolution,
                device=args.device,
            )
            render_times.append(time.perf_counter() - start)

    result = {
        "blender_version": bpy.app.version_string,
        "blender_build_hash": ocean_metrics.blender_build_hash(),
        "hostname": ocean_metrics.socket.gethostname(),
        "scenario": args.scenario,
        "variant": args.variant,
        "device": args.device,
        "samples": args.samples,
        "render_resolution": args.resolution,
        "ocean_resolution": args.ocean_resolution,
        "mesh": mesh_counts,
        "eval_s": summarize_values(eval_times),
        "render_s": summarize_values(render_times),
        "render": render_artifact,
    }
    ocean_metrics.write_json(outdir / f"{args.scenario}_{args.variant}.json", result)


if __name__ == "__main__":
    main()
