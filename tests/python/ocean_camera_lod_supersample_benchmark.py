# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import argparse
import json
import os
import platform
import socket
import sys
import tempfile
import time
from dataclasses import asdict
from pathlib import Path

sys.path.append(os.path.dirname(os.path.realpath(__file__)))

from modules import ocean_camera_lod_metrics as ocean_metrics


def downsample_rgb_box(rgb, source_width, source_height, target_width, target_height):
    if source_width != target_width and source_width % target_width != 0:
        raise ValueError(f"Source width {source_width} is not an integer multiple of {target_width}")
    if source_height != target_height and source_height % target_height != 0:
        raise ValueError(f"Source height {source_height} is not an integer multiple of {target_height}")

    scale_x = source_width // target_width
    scale_y = source_height // target_height
    if scale_x < 1 or scale_y < 1:
        raise ValueError(
            f"Source image {source_width}x{source_height} cannot downsample to "
            f"{target_width}x{target_height}"
        )

    downsampled = []
    sample_count = scale_x * scale_y
    for y in range(target_height):
        for x in range(target_width):
            r = g = b = 0.0
            for yy in range(scale_y):
                row = ((y * scale_y) + yy) * source_width
                for xx in range(scale_x):
                    pixel = rgb[row + (x * scale_x) + xx]
                    r += pixel[0]
                    g += pixel[1]
                    b += pixel[2]
            downsampled.append((r / sample_count, g / sample_count, b / sample_count))
    return downsampled


def assert_camera_lod_active_for_supersample(obj):
    lod_verts, lod_faces, _lod_unused, lod_attrs = ocean_metrics.evaluated_mesh_stats(obj)
    dense_verts, dense_faces, _dense_unused, _dense_attrs = ocean_metrics.dense_mesh_stats(obj)
    required_attrs = {
        "ocean_ref_coord",
        "ocean_geometry_normal",
        "ocean_geometry_support_covariance",
        "ocean_ref_uv",
        "ocean_camera_lod_level",
        "ocean_camera_lod_split_level",
    }
    missing = required_attrs.difference(lod_attrs)
    if missing:
        raise RuntimeError(
            "Supersample benchmark requires active camera LOD metadata; "
            f"missing={sorted(missing)}"
        )
    if dense_verts <= 0 or lod_verts >= dense_verts or lod_faces >= dense_faces:
        raise RuntimeError(
            "Supersample benchmark would not exercise an adaptive LOD render; "
            f"lod={(lod_verts, lod_faces)}, dense={(dense_verts, dense_faces)}"
        )


def render_supersample_case(
    *,
    scenario_name,
    device_name,
    outdir,
    samples,
    reference_samples,
    resolution,
    reference_scale,
):
    original_skip_asserts = os.environ.get(ocean_metrics.OCEAN_CAMERA_LOD_SKIP_SETUP_ASSERTS_ENV)
    os.environ[ocean_metrics.OCEAN_CAMERA_LOD_SKIP_SETUP_ASSERTS_ENV] = "1"
    try:
        context = ocean_metrics.setup_curated_scenario(scenario_name)
    finally:
        if original_skip_asserts is None:
            os.environ.pop(ocean_metrics.OCEAN_CAMERA_LOD_SKIP_SETUP_ASSERTS_ENV, None)
        else:
            os.environ[ocean_metrics.OCEAN_CAMERA_LOD_SKIP_SETUP_ASSERTS_ENV] = original_skip_asserts

    obj = ocean_metrics.bpy.data.objects[context.obj_name]
    mod = obj.modifiers["Ocean"]
    assert_camera_lod_active_for_supersample(obj)

    case_dir = Path(outdir) / ocean_metrics.sanitize_path_component(scenario_name) / ocean_metrics.sanitize_path_component(device_name)
    case_dir.mkdir(parents=True, exist_ok=True)

    low_resolution = resolution
    high_resolution = resolution * reference_scale

    original_use_camera_lod = mod.use_camera_lod
    lod_render_s = 0.0
    dense_render_s = 0.0

    with tempfile.TemporaryDirectory(prefix="ocean_lod_supersample_") as tempdir:
        tempdir = Path(tempdir)
        temp_lod = tempdir / "lod.png"
        temp_dense = tempdir / "dense_supersampled.png"

        try:
            mod.use_camera_lod = True
            ocean_metrics.bpy.context.view_layer.update()
            start = time.perf_counter()
            ocean_metrics.render_with_cycles(
                str(temp_lod),
                samples=samples,
                resolution=low_resolution,
                device=device_name,
            )
            lod_render_s = time.perf_counter() - start

            mod.use_camera_lod = False
            ocean_metrics.bpy.context.view_layer.update()
            start = time.perf_counter()
            ocean_metrics.render_with_cycles(
                str(temp_dense),
                samples=reference_samples,
                resolution=high_resolution,
                device=device_name,
            )
            dense_render_s = time.perf_counter() - start
        finally:
            mod.use_camera_lod = original_use_camera_lod
            ocean_metrics.bpy.context.view_layer.update()

        lod_width, lod_height, lod_rgb = ocean_metrics.materialize_render_output(
            temp_lod,
            str(case_dir / "lod.png"),
        )
        dense_width, dense_height, dense_rgb = ocean_metrics.materialize_render_output(
            temp_dense,
            str(case_dir / "dense_supersampled.png"),
        )

    expected_dense_size = (lod_width * reference_scale, lod_height * reference_scale)
    if (dense_width, dense_height) != expected_dense_size:
        raise RuntimeError(
            f"Dense reference size {dense_width}x{dense_height} does not match "
            f"expected {expected_dense_size[0]}x{expected_dense_size[1]}"
        )

    dense_downsampled_rgb = downsample_rgb_box(
        dense_rgb,
        dense_width,
        dense_height,
        lod_width,
        lod_height,
    )
    ocean_metrics.save_rgba_image(
        str(case_dir / "dense_downsampled.png"),
        lod_width,
        lod_height,
        ocean_metrics.rgb_to_rgba_pixels(dense_downsampled_rgb),
    )

    report, rgba_abs, rgba_gradient = ocean_metrics.compute_render_report(
        lod_rgb,
        dense_downsampled_rgb,
        lod_width,
        lod_height,
    )
    ocean_metrics.save_rgba_image(str(case_dir / "diff_abs.png"), lod_width, lod_height, rgba_abs)
    ocean_metrics.save_rgba_image(str(case_dir / "diff_gradient.png"), lod_width, lod_height, rgba_gradient)

    result = {
        "scenario": context.spec.name,
        "description": context.spec.description,
        "device": device_name,
        "ocean_split_shading": ocean_metrics.ocean_split_shading_mode(),
        "samples": samples,
        "reference_samples": reference_samples,
        "resolution": resolution,
        "reference_scale": reference_scale,
        "dense_reference_resolution": high_resolution,
        "lod_render_s": lod_render_s,
        "dense_reference_render_s": dense_render_s,
        "render": ocean_metrics.render_report_dict(report),
        "artifact_paths": {
            "lod": str(case_dir / "lod.png"),
            "dense_supersampled": str(case_dir / "dense_supersampled.png"),
            "dense_downsampled": str(case_dir / "dense_downsampled.png"),
            "diff_abs": str(case_dir / "diff_abs.png"),
            "diff_gradient": str(case_dir / "diff_gradient.png"),
        },
    }
    ocean_metrics.write_json(case_dir / "case.json", result)
    return result


def create_argparser():
    parser = argparse.ArgumentParser(
        description=(
            "Compare Ocean Camera LOD shading against a dense reference rendered "
            "at higher resolution and box-downsampled to the target resolution."
        )
    )
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--scenarios", default="calm_reference,grazing_light_adversarial")
    parser.add_argument("--devices", default="CPU")
    parser.add_argument("--samples", default=16, type=int)
    parser.add_argument("--reference-samples", default=64, type=int)
    parser.add_argument("--resolution", default=128, type=int)
    parser.add_argument("--reference-scale", default=4, type=int)
    return parser


def main():
    parser = create_argparser()
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    args = parser.parse_args(argv)

    if args.reference_scale < 1:
        raise ValueError("--reference-scale must be at least 1")

    outdir = Path(args.outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    scenario_names = ocean_metrics.resolve_scenarios(args.scenarios)
    device_names = ocean_metrics.resolve_devices(args.devices)

    cases = []
    for scenario_name in scenario_names:
        for device_name in device_names:
            cases.append(render_supersample_case(
                scenario_name=scenario_name,
                device_name=device_name,
                outdir=outdir,
                samples=args.samples,
                reference_samples=args.reference_samples,
                resolution=args.resolution,
                reference_scale=args.reference_scale,
            ))

    summary = {
        "blender_version": ocean_metrics.bpy.app.version_string,
        "blender_build_hash": ocean_metrics.blender_build_hash(),
        "hostname": socket.gethostname(),
        "platform": platform.platform(),
        "outdir": str(outdir),
        "scenarios": scenario_names,
        "devices": device_names,
        "available_devices": ocean_metrics.get_available_cycles_devices(),
        "ocean_split_shading": ocean_metrics.ocean_split_shading_mode(),
        "samples": args.samples,
        "reference_samples": args.reference_samples,
        "resolution": args.resolution,
        "reference_scale": args.reference_scale,
        "case_count": len(cases),
        "cases": cases,
    }
    ocean_metrics.write_json(outdir / "summary.json", summary)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
