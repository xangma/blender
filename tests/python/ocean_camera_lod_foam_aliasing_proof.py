# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reproduce the BF12 Camera LOD foam aliasing issue.

This is a diagnostic/proof script, not a CI regression test. It builds a
high-Beaufort ocean matching the StereoOcean BF12 preview case, renders the
Ocean foam corner attribute as pure emission, and compares:

* default Camera LOD vs dense reference
* Camera LOD with a larger full-spectrum radius vs dense reference

Use ``--expect issue`` on an unfixed build to assert the aliasing path, or
``--expect fixed`` on a fixed build to assert default Camera LOD matches dense
without relying on the larger full-spectrum radius workaround.
"""

import argparse
import json
import math
import sys
from pathlib import Path

import bpy
from bpy_extras.object_utils import world_to_camera_view


BF12 = {
    "resolution": 64,
    "spatial_size": 1000,
    "wind_velocity": 37.04,
    "wave_scale": 18.0,
    "choppiness": 1.88,
    "wave_alignment": 0.3,
    "wave_scale_min": 0.02,
    "water_depth": 200.0,
    "foam_coverage": -4.4437,
    "seed": 424242,
}


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete()


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--outdir",
        default="/tmp/blender-ocean-lod-tests/foam_aliasing_proof",
        help="Directory for JSON and image artifacts.",
    )
    parser.add_argument("--resolution-x", type=int, default=640)
    parser.add_argument("--resolution-y", type=int, default=360)
    parser.add_argument("--samples", type=int, default=1)
    parser.add_argument(
        "--fixed-radius",
        type=float,
        default=80.0,
        help="lod_camera_full_spectrum_radius used for the fixed LOD comparison.",
    )
    parser.add_argument(
        "--expect",
        choices={"none", "issue", "fixed"},
        default="none",
        help="Exit non-zero unless the requested condition is observed.",
    )
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return parser.parse_args(argv)


def configure_scene(args):
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = args.samples
    scene.cycles.seed = 0
    scene.render.resolution_x = args.resolution_x
    scene.render.resolution_y = args.resolution_y
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "Standard"
    scene.display_settings.display_device = "sRGB"
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"

    world = scene.world or bpy.data.worlds.new("World")
    scene.world = world
    world.color = (0.0, 0.0, 0.0)

    cam_data = bpy.data.cameras.new("BF12_Camera")
    cam = bpy.data.objects.new("BF12_Camera", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.location = (400.0, -400.0, 30.0)
    cam.rotation_euler = (0.0, 0.0, 0.0)
    cam_data.lens = 18.0
    cam_data.sensor_width = 36.0
    cam_data.clip_start = 0.1
    cam_data.clip_end = 20000000.0
    return cam


def make_ocean_object():
    mesh = bpy.data.meshes.new("BF12OceanMesh")
    obj = bpy.data.objects.new("BF12Ocean", mesh)
    bpy.context.scene.collection.objects.link(obj)

    mod = obj.modifiers.new(name="Ocean", type="OCEAN")
    mod.geometry_mode = "GENERATE"
    mod.resolution = BF12["resolution"]
    mod.viewport_resolution = BF12["resolution"]
    mod.spatial_size = BF12["spatial_size"]
    mod.size = 1.0
    mod.repeat_x = 1
    mod.repeat_y = 1
    mod.use_normals = True
    mod.use_foam = True
    mod.foam_layer_name = "foam"
    mod.foam_coverage = BF12["foam_coverage"]
    mod.use_camera_lod = True
    mod.lod_levels = 4
    if hasattr(mod, "lod_pixel_error"):
        mod.lod_pixel_error = 0.5
    if hasattr(mod, "lod_camera_full_spectrum_radius"):
        mod.lod_camera_full_spectrum_radius = 0.0
    if hasattr(mod, "lod_usage_mode"):
        mod.lod_usage_mode = "GENERAL_RENDER"
    if hasattr(mod, "lod_validation_mode"):
        mod.lod_validation_mode = "CAMERA_OBSERVABLE"
    mod.wind_velocity = BF12["wind_velocity"]
    mod.wave_scale = BF12["wave_scale"]
    mod.choppiness = BF12["choppiness"]
    mod.wave_alignment = BF12["wave_alignment"]
    mod.wave_scale_min = BF12["wave_scale_min"]
    if hasattr(mod, "spectrum"):
        mod.spectrum = "PHILLIPS"
    if hasattr(mod, "depth"):
        mod.depth = BF12["water_depth"]
    if hasattr(mod, "random_seed"):
        mod.random_seed = BF12["seed"]
    if hasattr(mod, "seed"):
        mod.seed = BF12["seed"]

    mat = bpy.data.materials.new("BF12FoamEmission")
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()
    output = nodes.new(type="ShaderNodeOutputMaterial")
    attr = nodes.new(type="ShaderNodeAttribute")
    attr.attribute_name = "foam"
    emission = nodes.new(type="ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    links.new(attr.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    obj.data.materials.append(mat)

    return obj, mod


def render_case(label, mod, outdir, use_camera_lod, full_radius):
    mod.use_camera_lod = use_camera_lod
    if hasattr(mod, "lod_camera_full_spectrum_radius"):
        mod.lod_camera_full_spectrum_radius = full_radius
    bpy.context.view_layer.update()

    filepath = outdir / f"{label}.png"
    bpy.context.scene.render.filepath = str(filepath)
    bpy.ops.render.render(write_still=True)
    if not filepath.exists():
        raise FileNotFoundError(filepath)
    return filepath


def load_rgb(filepath):
    image = bpy.data.images.load(str(filepath), check_existing=False)
    try:
        width, height = image.size[:]
        pixels = list(image.pixels[:])
    finally:
        bpy.data.images.remove(image)

    rgb = []
    for i in range(width * height):
        offset = i * 4
        rgb.append((pixels[offset], pixels[offset + 1], pixels[offset + 2]))
    return width, height, rgb


def metric_for_indices(a_rgb, b_rgb, indices):
    if not indices:
        return {"count": 0, "rmse": None, "mae": None, "max_abs": None}
    sum_sq = 0.0
    sum_abs = 0.0
    max_abs = 0.0
    count = 0
    for i in indices:
        ar, ag, ab = a_rgb[i]
        br, bg, bb = b_rgb[i]
        for diff in (ar - br, ag - bg, ab - bb):
            adiff = abs(diff)
            sum_sq += diff * diff
            sum_abs += adiff
            max_abs = max(max_abs, adiff)
            count += 1
    return {
        "count": count,
        "rmse": math.sqrt(sum_sq / count),
        "mae": sum_abs / count,
        "max_abs": max_abs,
    }


def compare_images(candidate_path, dense_path):
    width, height, candidate = load_rgb(candidate_path)
    dense_width, dense_height, dense = load_rgb(dense_path)
    if (width, height) != (dense_width, dense_height):
        raise ValueError(f"Image sizes differ: {(width, height)} vs {(dense_width, dense_height)}")

    def indices_for(region):
        indices = []
        for y in range(height):
            for x in range(width):
                lower10 = y < int(height * 0.10)
                lower25 = y < int(height * 0.25)
                right25 = x >= int(width * 0.75)
                if region == "full":
                    keep = True
                elif region == "lower10":
                    keep = lower10
                elif region == "lower25":
                    keep = lower25
                elif region == "right25":
                    keep = right25
                elif region == "lower25_right25":
                    keep = lower25 and right25
                else:
                    raise ValueError(region)
                if keep:
                    indices.append((y * width) + x)
        return indices

    regions = ["full", "lower10", "lower25", "right25", "lower25_right25"]
    return {
        "width": width,
        "height": height,
        "regions": {
            region: metric_for_indices(candidate, dense, indices_for(region)) for region in regions
        },
    }


def save_abs_diff(candidate_path, dense_path, diff_path, scale):
    width, height, candidate = load_rgb(candidate_path)
    dense_width, dense_height, dense = load_rgb(dense_path)
    if (width, height) != (dense_width, dense_height):
        raise ValueError("Cannot diff images with different sizes")
    rgba = []
    for a, b in zip(candidate, dense):
        for channel in range(3):
            rgba.append(min(abs(a[channel] - b[channel]) * scale, 1.0))
        rgba.append(1.0)
    image = bpy.data.images.new(diff_path.stem, width=width, height=height, alpha=True)
    try:
        image.filepath_raw = str(diff_path)
        image.file_format = "PNG"
        image.pixels[:] = rgba
        image.save()
    finally:
        bpy.data.images.remove(image)


def patch_metric_for_bbox(candidate_path, dense_path, bbox_px):
    width, height, candidate = load_rgb(candidate_path)
    dense_width, dense_height, dense = load_rgb(dense_path)
    if (width, height) != (dense_width, dense_height):
        raise ValueError("Cannot compare image patches with different sizes")

    x0 = max(0, min(width - 1, int(math.floor(bbox_px[0]))))
    y0 = max(0, min(height - 1, int(math.floor(bbox_px[1]))))
    x1 = max(x0 + 1, min(width, int(math.ceil(bbox_px[2]))))
    y1 = max(y0 + 1, min(height, int(math.ceil(bbox_px[3]))))
    indices = [(y * width) + x for y in range(y0, y1) for x in range(x0, x1)]
    dense_values = [dense[i][0] for i in indices]
    return {
        "bbox_px": [x0, y0, x1, y1],
        "pixel_count": len(indices),
        "dense_foam_min": min(dense_values) if dense_values else None,
        "dense_foam_max": max(dense_values) if dense_values else None,
        "dense_foam_range": (max(dense_values) - min(dense_values)) if dense_values else None,
        "lod_vs_dense": metric_for_indices(candidate, dense, indices),
    }


def percentile(values, pct):
    if not values:
        return None
    values = sorted(values)
    index = min(len(values) - 1, max(0, round((len(values) - 1) * pct)))
    return values[index]


def summarize_cells(samples):
    cells = [sample["cell"] for sample in samples]
    levels = {}
    for sample in samples:
        key = str(sample["level"])
        levels[key] = levels.get(key, 0) + 1
    return {
        "count": len(samples),
        "min": min(cells) if cells else None,
        "p50": percentile(cells, 0.50),
        "p95": percentile(cells, 0.95),
        "max": max(cells) if cells else None,
        "level_hist": levels,
    }


def mesh_lod_report(obj, mod, camera):
    mod.use_camera_lod = True
    if hasattr(mod, "lod_camera_full_spectrum_radius"):
        mod.lod_camera_full_spectrum_radius = 0.0
    bpy.context.view_layer.update()

    depsgraph = bpy.context.evaluated_depsgraph_get()
    eval_obj = obj.evaluated_get(depsgraph)
    mesh = eval_obj.to_mesh()
    try:
        cell_attr = mesh.attributes["ocean_camera_lod_cell_size"].data
        level_attr = mesh.attributes["ocean_camera_lod_leaf_level"].data
        matrix = eval_obj.matrix_world
        scene = bpy.context.scene
        buckets = {"visible": [], "lower10": [], "lower25": [], "near_camera_5m": []}
        camera_xy = (camera.location.x, camera.location.y)
        for poly in mesh.polygons:
            center = matrix @ poly.center
            cell = float(cell_attr[poly.index].value)
            level = int(level_attr[poly.index].value)
            sample = {"cell": cell, "level": level, "face": poly.index}
            dist = math.hypot(center.x - camera_xy[0], center.y - camera_xy[1])
            if dist <= 5.0:
                buckets["near_camera_5m"].append(sample)
            co = world_to_camera_view(scene, camera, center)
            if co.z > 0.0 and 0.0 <= co.x <= 1.0 and 0.0 <= co.y <= 1.0:
                buckets["visible"].append(sample)
                if co.y < 0.10:
                    buckets["lower10"].append(sample)
                if co.y < 0.25:
                    buckets["lower25"].append(sample)
        return {
            "verts": len(mesh.vertices),
            "faces": len(mesh.polygons),
            "expected_fine_cell_m": BF12["spatial_size"] / float(BF12["resolution"] * BF12["resolution"]),
            "buckets": {name: summarize_cells(samples) for name, samples in buckets.items()},
        }
    finally:
        eval_obj.to_mesh_clear()


def foam_attribute_cause_report(obj, mod, camera, max_examples=32):
    mod.use_camera_lod = True
    if hasattr(mod, "lod_camera_full_spectrum_radius"):
        mod.lod_camera_full_spectrum_radius = 0.0
    bpy.context.view_layer.update()

    depsgraph = bpy.context.evaluated_depsgraph_get()
    eval_obj = obj.evaluated_get(depsgraph)
    mesh = eval_obj.to_mesh()
    try:
        foam_attr = mesh.attributes["foam"]
        cell_attr = mesh.attributes["ocean_camera_lod_cell_size"].data
        level_attr = mesh.attributes["ocean_camera_lod_leaf_level"].data
        matrix = eval_obj.matrix_world
        scene = bpy.context.scene
        width = scene.render.resolution_x
        height = scene.render.resolution_y
        fine_cell = BF12["spatial_size"] / float(BF12["resolution"] * BF12["resolution"])

        coarse_lower10_count = 0
        examples = []
        for poly in mesh.polygons:
            center = matrix @ poly.center
            co = world_to_camera_view(scene, camera, center)
            if not (co.z > 0.0 and 0.0 <= co.x <= 1.0 and 0.0 <= co.y <= 1.0 and co.y < 0.10):
                continue
            cell = float(cell_attr[poly.index].value)
            if cell <= fine_cell:
                continue

            coarse_lower10_count += 1
            if len(examples) >= max_examples:
                continue

            screen_points = []
            object_xy = []
            for vertex_index in poly.vertices:
                world = matrix @ mesh.vertices[vertex_index].co
                point = world_to_camera_view(scene, camera, world)
                screen_points.append([float(point.x), float(point.y), float(point.z)])
                object_xy.append([float(world.x), float(world.y)])

            xs = [p[0] for p in screen_points]
            ys = [p[1] for p in screen_points]
            loop_values = [float(foam_attr.data[loop_index].color[0]) for loop_index in poly.loop_indices]
            dense_cells_per_side = max(1, int(round(cell / fine_cell)))
            examples.append({
                "face": poly.index,
                "level": int(level_attr[poly.index].value),
                "cell_m": cell,
                "fine_cell_m": fine_cell,
                "cell_to_fine_ratio": cell / fine_cell,
                "corner_count": len(poly.loop_indices),
                "foam_corner_values": loop_values,
                "foam_corner_min": min(loop_values),
                "foam_corner_max": max(loop_values),
                "foam_corner_range": max(loop_values) - min(loop_values),
                "estimated_dense_cells_per_side": dense_cells_per_side,
                "estimated_dense_faces_covered": dense_cells_per_side * dense_cells_per_side,
                "estimated_dense_corners_available": (dense_cells_per_side + 1) * (dense_cells_per_side + 1),
                "screen_center": [float(co.x), float(co.y), float(co.z)],
                "screen_bbox_norm": [min(xs), min(ys), max(xs), max(ys)],
                "screen_bbox_px": [min(xs) * width, min(ys) * height, max(xs) * width, max(ys) * height],
                "object_xy": object_xy,
            })

        return {
            "foam_attribute": {
                "name": foam_attr.name,
                "domain": str(foam_attr.domain),
                "data_type": str(foam_attr.data_type),
                "data_count": len(foam_attr.data),
            },
            "carrier": {
                "mesh_faces": len(mesh.polygons),
                "mesh_vertices": len(mesh.vertices),
                "fine_cell_m": fine_cell,
                "coarse_lower10_face_count": coarse_lower10_count,
            },
            "coarse_lower10_examples": examples,
            "cause_chain_supported": (
                str(foam_attr.domain) == "CORNER"
                and bool(examples)
                and all(example["corner_count"] == 4 for example in examples)
                and any(example["estimated_dense_faces_covered"] > 1 for example in examples)
            ),
        }
    finally:
        eval_obj.to_mesh_clear()


def attach_patch_metrics(cause_report, default_lod_path, dense_path, keep_examples=8):
    for example in cause_report["coarse_lower10_examples"]:
        example["render_patch"] = patch_metric_for_bbox(
            default_lod_path, dense_path, example["screen_bbox_px"]
        )
    cause_report["coarse_lower10_examples"].sort(
        key=lambda example: example["render_patch"]["lod_vs_dense"]["rmse"] or 0.0,
        reverse=True,
    )
    del cause_report["coarse_lower10_examples"][keep_examples:]


def main():
    args = parse_args()
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    clear_scene()
    camera = configure_scene(args)
    obj, mod = make_ocean_object()

    dense = render_case("dense_foam_attribute", mod, outdir, use_camera_lod=False, full_radius=0.0)
    default_lod = render_case("default_lod_foam_attribute", mod, outdir, use_camera_lod=True, full_radius=0.0)
    fixed_lod = render_case(
        "radius_lod_foam_attribute", mod, outdir, use_camera_lod=True, full_radius=args.fixed_radius
    )

    default_report = compare_images(default_lod, dense)
    fixed_report = compare_images(fixed_lod, dense)
    save_abs_diff(default_lod, dense, outdir / "default_lod_vs_dense_absdiff_x6.png", scale=6.0)
    save_abs_diff(fixed_lod, dense, outdir / "radius_lod_vs_dense_absdiff_x6.png", scale=6.0)
    mesh_report = mesh_lod_report(obj, mod, camera)
    cause_report = foam_attribute_cause_report(obj, mod, camera)
    attach_patch_metrics(cause_report, default_lod, dense)

    default_lower10 = default_report["regions"]["lower10"]["rmse"]
    fixed_lower10 = fixed_report["regions"]["lower10"]["rmse"]
    near_camera_max = mesh_report["buckets"]["near_camera_5m"]["max"]
    fine_cell = mesh_report["expected_fine_cell_m"]
    issue_reproduced = (
        default_lower10 is not None
        and fixed_lower10 is not None
        and default_lower10 > 0.05
        and fixed_lower10 < default_lower10 * 0.25
        and near_camera_max == fine_cell
        and mesh_report["buckets"]["lower10"]["max"] > fine_cell
        and cause_report["cause_chain_supported"]
    )
    fix_verified = (
        default_lower10 is not None
        and default_lower10 < 0.005
        and default_report["regions"]["lower25_right25"]["rmse"] < 0.005
        and mesh_report["buckets"]["lower10"]["max"] == fine_cell
        and cause_report["carrier"]["coarse_lower10_face_count"] == 0
    )

    proof = {
        "bf12": BF12,
        "render": {
            "resolution": [args.resolution_x, args.resolution_y],
            "samples": args.samples,
            "camera_location": list(camera.location),
        },
        "artifacts": {
            "dense": str(dense),
            "default_lod": str(default_lod),
            "radius_lod": str(fixed_lod),
            "default_absdiff_x6": str(outdir / "default_lod_vs_dense_absdiff_x6.png"),
            "radius_absdiff_x6": str(outdir / "radius_lod_vs_dense_absdiff_x6.png"),
        },
        "comparisons": {
            "default_lod_vs_dense": default_report,
            "radius_lod_vs_dense": fixed_report,
        },
        "mesh_lod": mesh_report,
        "cause": cause_report,
        "issue_reproduced": issue_reproduced,
        "fix_verified": fix_verified,
        "proof_conditions": {
            "default_lower10_rmse_gt": 0.05,
            "fixed_default_lower10_rmse_lt": 0.005,
            "fixed_default_lower25_right25_rmse_lt": 0.005,
            "fixed_no_coarse_lower10_faces": True,
            "radius_lower10_rmse_lt_default_fraction": 0.25,
            "near_camera_5m_all_finest": True,
            "lower10_contains_coarser_than_finest": True,
        },
    }

    proof_path = outdir / "proof.json"
    proof_path.write_text(json.dumps(proof, indent=2, sort_keys=True))
    print(json.dumps(proof, indent=2, sort_keys=True))
    print(f"WROTE {proof_path}")

    if args.expect == "issue" and not issue_reproduced:
        raise SystemExit("BF12 Camera LOD foam aliasing issue was not reproduced")
    if args.expect == "fixed" and not fix_verified:
        raise SystemExit("BF12 Camera LOD foam aliasing fix was not verified")


if __name__ == "__main__":
    main()
