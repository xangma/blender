# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Native stereo Geometry Supervision foam regression and artifact harness.

Run: blender -b --factory-startup --python-exit-code 1 --python THIS_FILE --
     --outdir OUTPUT [--baseline] [--mesh-only]

Requires only Blender's bundled bpy, mathutils and numpy. The same modifier is
evaluated with LOD off/on; time, spectrum, seed and cameras never change.
Baseline asserts the old dense visible carrier; it does not waive geometry or
image bounds. Mesh-only is a preflight, not full native output validation.
The default fixture uses 2 m smallest-wave damping to combine useful visible
coarsening with strict maximum-pixel appearance bounds. Use --smallest-wave
0.02 for the high-frequency stress case, whose rare appearance outliers are
recorded separately; the Ocean modifier's production defaults are unchanged.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time

import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

sys.path.append(str(Path(__file__).resolve().parent))
from modules import ocean_camera_lod_metrics as metrics


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir", required=True)
    parser.add_argument("--baseline", "--expect-dense-carrier", action="store_true")
    parser.add_argument("--mesh-only", action="store_true")
    parser.add_argument("--resolution", type=int, default=32, help="Ocean resolution; mesh cells = resolution squared.")
    parser.add_argument("--width", type=int, default=128)
    parser.add_argument("--height", type=int, default=80)
    parser.add_argument("--samples", type=int, default=4)
    parser.add_argument("--device", default="CPU")
    parser.add_argument("--pixel-error", type=float, default=0.5)
    parser.add_argument("--wave-scale", type=float, default=1.0)
    parser.add_argument("--smallest-wave", type=float, default=2.0)
    parser.add_argument("--foam-coverage", type=float, default=0.0)
    parser.add_argument("--min-visible-reduction", type=float, default=0.15)
    return parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])


def stats(values):
    array = np.asarray(values, dtype=np.float64).reshape(-1)
    if not array.size:
        return {"count": 0, "mean": None, "p95": None, "max": None}
    assert np.isfinite(array).all(), "Metric contains non-finite values"
    return {"count": int(array.size), "mean": float(array.mean()),
            "p95": float(np.percentile(array, 95)), "max": float(array.max())}


def setup(args):
    metrics.clear_scene()
    cam = metrics.make_camera_and_light(cam_location=(0.0, -50.0, 12.0),
                                        cam_target=(0.0, 50.0, 0.0), lens=28.0,
                                        sun_rotation=(0.3, 0.0, 0.8), sun_energy=1.0)
    cam.data.clip_start = 0.1
    cam.data.clip_end = 10000.0
    metrics.enable_stereo_multiview(cam, interocular_distance=0.2,
                                    convergence_distance=80.0)
    cam.data.stereo.pivot = "CENTER"
    scene = bpy.context.scene
    scene.render.resolution_x = args.width
    scene.render.resolution_y = args.height
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.views_format = "INDIVIDUAL"
    scene.render.film_transparent = True
    scene.render.use_motion_blur = False
    scene.cycles.samples = args.samples
    scene.cycles.seed = 0
    scene.cycles.use_denoising = False
    scene.cycles.use_adaptive_sampling = False
    scene.view_settings.view_transform = "Standard"
    metrics.configure_cycles_device(args.device)
    metrics.set_world_sky_gradient(strength=0.5)
    obj = metrics.make_ocean_object(name="SupervisedFoam", resolution=args.resolution,
                                   spatial_size=128, camera_lod=True, lod_levels=5,
                                   lod_pixel_error=getattr(args, "pixel_error", 0.5), lod_usage_mode="STEREO_DATASET",
                                   lod_validation_mode="GEOMETRY_STRICT", time_value=1.0,
                                   choppiness=1.3, wind_velocity=18.0,
                                   wave_scale=args.wave_scale, smallest_wave=args.smallest_wave, seed=424242)
    mod = obj.modifiers["Ocean"]
    mod.use_foam = True
    mod.foam_layer_name = "foam"
    mod.foam_coverage = args.foam_coverage
    mod.use_spray = True
    mod.spray_layer_name = "spray"
    return scene, cam, obj, mod


def cameras(scene, cam):
    depsgraph = bpy.context.evaluated_depsgraph_get()
    result = {}
    for eye in ("left", "right"):
        model = cam.calc_matrix_camera_model(depsgraph, scene=scene, view_name=eye)
        projection = cam.calc_matrix_camera(depsgraph, x=scene.render.resolution_x,
                                             y=scene.render.resolution_y,
                                             scale_x=scene.render.pixel_aspect_x,
                                             scale_y=scene.render.pixel_aspect_y,
                                             scene=scene, view_name=eye)
        result[eye] = {"model": model, "view": model.inverted(),
                       "projection": projection, "world_to_clip": projection @ model.inverted(),
                       "origin": model.translation.copy()}
    return result


def snapshot(obj):
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        mesh.calc_loop_triangles()
        vertices = [obj.matrix_world @ vertex.co for vertex in mesh.vertices]
        faces = [tuple(poly.vertices) for poly in mesh.polygons]
        triangles = [tuple(tri.vertices) for tri in mesh.loop_triangles]
        digest = hashlib.sha256(np.asarray(vertices, dtype=np.float32).tobytes() +
                                np.asarray(triangles, dtype=np.int32).tobytes()).hexdigest()
        attrs = sorted(attr.name for attr in mesh.attributes)
        levels = mesh.attributes.get("ocean_camera_lod_level")
        histogram = {}
        if levels:
            for item in levels.data:
                level = int(round(item.value))
                histogram[level] = histogram.get(level, 0) + 1
        return {"vertices": vertices, "faces": faces, "triangles": triangles,
                "bvh": BVHTree.FromPolygons(vertices, triangles, all_triangles=True),
                "sha256": digest, "attrs": attrs, "levels": histogram}
    finally:
        evaluated.to_mesh_clear()


def project(camera, point, width, height):
    clip = camera["world_to_clip"] @ Vector((*point, 1.0))
    if clip.w <= 0.0:
        return None
    return np.asarray(((clip.x / clip.w + 1.0) * width / 2.0,
                       (clip.y / clip.w + 1.0) * height / 2.0))


def mesh_report(mesh, views, args):
    visible = {}
    vertices = np.asarray(mesh["vertices"], dtype=np.float64)
    homogeneous = np.column_stack((vertices, np.ones(len(vertices))))
    face_groups = {}
    for face in mesh["faces"]:
        face_groups.setdefault(len(face), []).append(face)
    for eye, camera in views.items():
        count = 0
        clip = homogeneous @ np.asarray(camera["world_to_clip"]).T
        positive_w = clip[:, 3] > 0.0
        denominator = np.where(positive_w, clip[:, 3], 1.0)
        projected = (clip[:, :2] / denominator[:, None] + 1.0) * (args.width / 2.0, args.height / 2.0)
        for faces in face_groups.values():
            indices = np.asarray(faces, dtype=np.int32)
            mask = positive_w[indices]
            points = projected[indices]
            # Count polygons whose projected bounding box meets the image. This
            # includes boundary-crossing faces, not just their centroids.
            lower = np.where(mask[:, :, None], points, np.inf).min(axis=1)
            upper = np.where(mask[:, :, None], points, -np.inf).max(axis=1)
            count += int((mask.any(axis=1) & (upper[:, 0] >= 0) & (lower[:, 0] <= args.width) &
                          (upper[:, 1] >= 0) & (lower[:, 1] <= args.height)).sum())
        visible[eye] = count
    return {"vertices": len(mesh["vertices"]), "faces": len(mesh["faces"]),
            "triangles": len(mesh["triangles"]), "visible_faces": visible,
            "mesh_sha256": mesh["sha256"], "attributes": mesh["attrs"],
            "local_level_histogram": mesh["levels"]}


def set_material(obj, kind):
    mat = obj.data.materials[0]
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    attr = nodes.new("ShaderNodeAttribute")
    attr.attribute_name = "spray" if kind == "spray" else "foam"
    if kind in {"foam", "spray"}:
        emission = nodes.new("ShaderNodeEmission")
        links.new(attr.outputs["Color"], emission.inputs["Color"])
        links.new(emission.outputs["Emission"], output.inputs["Surface"])
    else:
        mix = nodes.new("ShaderNodeMixRGB")
        mix.inputs[1].default_value = (0.015, 0.055, 0.09, 1.0)
        mix.inputs[2].default_value = (0.9, 0.9, 0.9, 1.0)
        links.new(attr.outputs["Fac"], mix.inputs[0])
        bsdf = nodes.new("ShaderNodeBsdfPrincipled")
        bsdf.inputs["Roughness"].default_value = 0.25
        links.new(mix.outputs["Color"], bsdf.inputs["Base Color"])
        links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])


def configure_outputs(scene, directory):
    scene.view_layers[0].use_pass_z = True
    scene.view_layers[0].use_pass_position = True
    scene.view_layers[0].use_pass_normal = True
    tree = bpy.data.node_groups.new("SupervisedFoamPasses", "CompositorNodeTree")
    scene.compositing_node_group = tree
    source = tree.nodes.new("CompositorNodeRLayers")
    tree.interface.new_socket(name="Image", in_out="OUTPUT", socket_type="NodeSocketColor")
    composite = tree.nodes.new("NodeGroupOutput")
    tree.links.new(source.outputs["Image"], composite.inputs["Image"])
    output = tree.nodes.new("CompositorNodeOutputFile")
    output.directory = str(directory)
    output.file_name = ""
    # Blender 5.2 defaults File Output to MULTI_LAYER_IMAGE, whose enum admits
    # only OPEN_EXR_MULTILAYER. Select image media before individual EXR format.
    output.format.media_type = "IMAGE"
    output.format.file_format = "OPEN_EXR"
    output.format.color_depth = "32"
    output.format.color_mode = "RGBA"
    output.format.views_format = "INDIVIDUAL"
    output.save_as_render = False
    output.file_output_items.clear()
    for name, socket in (("depth", "Depth"), ("position", "Position"), ("normal", "Normal")):
        output.file_output_items.new("RGBA", name)
        tree.links.new(source.outputs[socket], output.inputs[name])
    return tree


def read_image(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        width, height = image.size[:]
        pixels = np.empty(width * height * 4, dtype=np.float32)
        image.pixels.foreach_get(pixels)
        return pixels.reshape((height, width, 4))
    finally:
        bpy.data.images.remove(image)


def find_view_file(directory, prefix, eye, extension):
    suffix = next(view.file_suffix for view in bpy.context.scene.render.views if view.name == eye)
    found = [path for path in directory.glob(prefix + "*" + extension) if suffix in path.stem]
    assert len(found) == 1, f"Expected one {prefix}/{eye} output, found {found}"
    return found[0]


def render(scene, obj, mod, directory, label, kind, lod):
    path = directory / label
    path.mkdir(parents=True, exist_ok=True)
    mod.use_camera_lod = lod
    set_material(obj, kind)
    bpy.context.view_layer.update()
    mesh = snapshot(obj)
    tree = configure_outputs(scene, path)
    scene.render.filepath = str(path / "rgb.png")
    start = time.monotonic()
    profile = metrics.capture_process_stdout(lambda: bpy.ops.render.render(write_still=True))
    elapsed = time.monotonic() - start
    (path / "profile.log").write_text(profile, encoding="utf-8")
    print(f"RENDER {label} {elapsed:.3f}s", flush=True)
    scene.compositing_node_group = None
    bpy.data.node_groups.remove(tree)
    images = {}
    for eye in ("left", "right"):
        images[eye] = {name: read_image(find_view_file(path, name, eye, extension))
                       for name, extension in (("rgb", ".png"), ("depth", ".exr"),
                                               ("position", ".exr"), ("normal", ".exr"))}
    return images, mesh, {"seconds": elapsed, "directory": str(path),
                           "profile": metrics.parse_ocean_camera_lod_profile(profile)}


def output_consistency(images, explicit, dense, views, args, directory):
    result = {}
    all_points = []
    for eye, camera in views.items():
        other = views["right" if eye == "left" else "left"]
        arrays = images[eye]
        disparity = np.full((args.height, args.width), np.nan, dtype=np.float32)
        correspondence = np.full((args.height, args.width, 2), np.nan, dtype=np.float32)
        errors = {name: [] for name in ("render_to_mesh_m", "depth_to_position_m",
                                       "point_cloud_backprojection_m", "dense_first_hit_depth_m",
                                       "dense_first_hit_position_m", "dense_disparity_px")}
        count = missing = 0
        for y in range(args.height):
            for x in range(args.width):
                depth = float(arrays["depth"][y, x, 0])
                if not (0.0 < depth < 9999.0):
                    continue
                point = Vector(arrays["position"][y, x, :3])
                if point.length == 0.0:
                    continue
                count += 1
                direction = (point - camera["origin"]).normalized()
                mesh_hit = explicit["bvh"].ray_cast(camera["origin"], direction)[0]
                assert mesh_hit is not None, "Rendered ocean position has no explicit-mesh first hit"
                errors["render_to_mesh_m"].append((point - mesh_hit).length)
                axial_depth = -(camera["view"] @ point).z
                errors["depth_to_position_m"].append(abs(depth - axial_depth))
                camera_ray = camera["view"].to_3x3() @ direction
                reconstructed = camera["origin"] + direction * (depth / -camera_ray.z)
                errors["point_cloud_backprojection_m"].append((reconstructed - point).length)
                all_points.append(tuple(point))
                dense_hit = dense["bvh"].ray_cast(camera["origin"], direction)[0]
                if dense_hit is None:
                    missing += 1
                    continue
                errors["dense_first_hit_depth_m"].append(abs(axial_depth + (camera["view"] @ dense_hit).z))
                errors["dense_first_hit_position_m"].append((point - dense_hit).length)
                candidate_pixel = project(other, point, args.width, args.height)
                dense_pixel = project(other, dense_hit, args.width, args.height)
                if candidate_pixel is not None and dense_pixel is not None:
                    own_pixel = project(camera, point, args.width, args.height)
                    disparity[y, x] = own_pixel[0] - candidate_pixel[0]
                    correspondence[y, x] = candidate_pixel
                    errors["dense_disparity_px"].append(abs(float(candidate_pixel[0] - dense_pixel[0])))
        result[eye] = {name: stats(values) for name, values in errors.items()}
        result[eye].update({"valid_rendered_samples": count, "dense_missing_first_hits": missing})
        np.savez_compressed(directory / (eye + "_geometry_outputs.npz"),
                            depth_m=arrays["depth"][:, :, 0], position_m=arrays["position"][:, :, :3],
                            disparity_px=disparity, other_eye_pixel=correspondence,
                            normal=arrays["normal"][:, :, :3])
    with (directory / "rendered_point_cloud.ply").open("w", encoding="utf-8") as handle:
        handle.write("ply\nformat ascii 1.0\nelement vertex %d\nproperty float x\nproperty float y\nproperty float z\nend_header\n" % len(all_points))
        for point in all_points:
            handle.write("%.9g %.9g %.9g\n" % point)
    np.savez_compressed(directory / "explicit_mesh.npz",
                        vertices=np.asarray(explicit["vertices"]), triangles=np.asarray(explicit["triangles"]))
    return result


def image_comparison(candidate, dense, kind):
    report = {}
    for eye in ("left", "right"):
        a, b = candidate[eye]["rgb"], dense[eye]["rgb"]
        mask = (a[:, :, 3] > 0.99) & (b[:, :, 3] > 0.99)
        values = b[:, :, :3][mask]
        item = {"absolute_error": stats(np.abs(a[:, :, :3][mask] - values)),
                "common_ocean_pixels": int(mask.sum())}
        if kind == "foam":
            signal_a, signal_b = a[:, :, :3].mean(axis=2), b[:, :, :3].mean(axis=2)
            item.update({"dense_signal_range": float(values.max() - values.min()),
                         "coverage_threshold": 0.02,
                         "candidate_coverage": float((signal_a[mask] > 0.02).mean()),
                         "dense_coverage": float((signal_b[mask] > 0.02).mean()),
                         "coverage_disagreement": float(((signal_a[mask] > 0.02) != (signal_b[mask] > 0.02)).mean())})
        report[eye] = item
    return report


def strip_legacy_attributes(obj):
    tree = bpy.data.node_groups.new("RemoveLegacyFoamSpray", "GeometryNodeTree")
    tree.interface.new_socket(name="Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    tree.interface.new_socket(name="Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    source = tree.nodes.new("NodeGroupInput").outputs["Geometry"]
    target = tree.nodes.new("NodeGroupOutput").inputs["Geometry"]
    for name in ("foam", "spray"):
        node = tree.nodes.new("GeometryNodeRemoveAttribute")
        node.inputs["Name"].default_value = name
        tree.links.new(source, node.inputs["Geometry"])
        source = node.outputs["Geometry"]
    tree.links.new(source, target)
    modifier = obj.modifiers.new("RemoveLegacyFoamSpray", "NODES")
    modifier.node_group = tree
    return modifier, tree


def main():
    args = arguments()
    outdir = Path(args.outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)
    report = {"blender_version": bpy.app.version_string,
              "build_hash": bpy.app.build_hash.decode(), "arguments": vars(args),
              "stage": "mesh_preflight" if args.mesh_only else "native_validation", "failures": [],
              "bounds": {"minimum_visible_face_reduction": args.min_visible_reduction,
                         "attribute_image_mae": 0.005, "attribute_image_p95": 0.02,
                         "attribute_image_max": 0.05, "foam_coverage_disagreement": 0.03,
                         "beauty_image_mae": 0.08, "beauty_image_p95": 0.2,
                         "beauty_image_max": 0.35, "render_mesh_depth_backprojection_max_m": 0.002,
                         "dense_first_hit_depth_mean_m": 0.35, "dense_first_hit_depth_max_m": 1.2,
                         "dense_first_hit_position_max_m": 1.5, "dense_disparity_max_px": 0.5},
              "scope": "Focused same-state native scene; does not validate the prior 25.28m depth discrepancy or the StereoOcean exporter."}
    failures = report["failures"]
    def check(condition, message):
        if not condition:
            failures.append(message)

    try:
        os.environ["BLENDER_OCEAN_CAMERA_LOD_PROFILE"] = "1"
        scene, cam, obj, mod = setup(args)
        report["ocean_state"] = {name: getattr(mod, name) for name in
                                 ("resolution", "spatial_size", "size", "time", "wave_scale", "wave_scale_min",
                                  "wind_velocity", "choppiness", "foam_coverage", "lod_usage_mode",
                                  "lod_validation_mode", "lod_pixel_error", "lod_levels", "lod_policy",
                                  "random_seed", "use_foam", "use_spray")}
        views = cameras(scene, cam)
        report["cameras"] = {eye: {name: [list(row) for row in data[name]] for name in ("model", "projection")}
                             for eye, data in views.items()}
        report["meshes"] = {}
        snapshots = {}
        for name, lod, foam in (("dense", False, True), ("lod_foam_off", True, False), ("lod_foam_on", True, True)):
            mod.use_camera_lod = lod
            mod.use_foam = foam
            mod.use_spray = foam
            mesh = snapshot(obj)
            snapshots[name] = mesh
            report["meshes"][name] = mesh_report(mesh, views, args)
        dense = snapshots["dense"]
        for name in ("lod_foam_off", "lod_foam_on"):
            item = report["meshes"][name]
            item["whole_domain_face_reduction"] = 1.0 - item["faces"] / report["meshes"]["dense"]["faces"]
            item["visible_face_reduction"] = {eye: 1.0 - item["visible_faces"][eye] / report["meshes"]["dense"]["visible_faces"][eye]
                                               for eye in views}
            for eye, reduction in item["visible_face_reduction"].items():
                if name == "lod_foam_on" and args.baseline:
                    check(reduction < 0.03, f"Baseline must reproduce dense visible foam carrier: {eye} reduction={reduction}")
                else:
                    check(reduction >= args.min_visible_reduction, f"{name}/{eye}: visible face reduction {reduction} below {args.min_visible_reduction}")
        if not args.baseline:
            check(snapshots["lod_foam_off"]["sha256"] == snapshots["lod_foam_on"]["sha256"], "Foam/spray must not change supervised explicit geometry")
        check(mod.lod_usage_mode == "STEREO_DATASET" and mod.lod_validation_mode == "GEOMETRY_STRICT", "Must retain supervision and strict error controls")
        if not args.mesh_only:
            report["renders"] = {}
            for kind in ("foam", "spray", "beauty"):
                lod_images, lod_mesh, lod_info = render(scene, obj, mod, outdir, "lod_" + kind, kind, True)
                dense_images, _dense_mesh, dense_info = render(scene, obj, mod, outdir, "dense_" + kind, kind, False)
                comparison = image_comparison(lod_images, dense_images, kind)
                report["renders"][kind] = {"lod": lod_info, "dense": dense_info, "comparison": comparison}
                for eye, item in comparison.items():
                    error = item["absolute_error"]
                    check(item["common_ocean_pixels"] >= 100, f"{kind}/{eye}: insufficient visible ocean")
                    mean_bound, p95_bound, max_bound = (0.005, 0.02, 0.05) if kind != "beauty" else (0.08, 0.2, 0.35)
                    for key, bound in (("mean", mean_bound), ("p95", p95_bound), ("max", max_bound)):
                        check(error[key] is not None and error[key] <= bound, f"{kind}/{eye}: image {key} {error[key]} exceeds {bound}")
                    if kind == "foam":
                        check(item["dense_signal_range"] > 0.01 and item["dense_coverage"] > 0.005, f"{eye}: dense foam must have nontrivial visible signal")
                        check(item["coverage_disagreement"] < 0.03, f"{eye}: foam coverage disagreement >= 3%")
                if kind == "beauty":
                    consistency = output_consistency(lod_images, lod_mesh, dense, views, args, outdir)
                    report["geometry_outputs"] = consistency
                    for eye, item in consistency.items():
                        check(item["valid_rendered_samples"] >= 100, f"{eye}: insufficient rendered geometry samples")
                        check(item["dense_missing_first_hits"] == 0, f"{eye}: dense first-hit reference missing")
                        for name, bound in (("render_to_mesh_m", 0.002), ("depth_to_position_m", 0.002),
                                            ("point_cloud_backprojection_m", 0.002), ("dense_first_hit_depth_m", 1.2),
                                            ("dense_first_hit_position_m", 1.5), ("dense_disparity_px", 0.5)):
                            check(item[name]["max"] is not None and item[name]["max"] <= bound, f"{eye}: {name} max {item[name]['max']} exceeds {bound}")
                        check(item["dense_first_hit_depth_m"]["mean"] <= 0.35, f"{eye}: mean dense first-hit depth error exceeds 0.35m")
                if not args.baseline:
                    sync = [entry for entry in lod_info["profile"] if entry.get("stage") == "cycles_sync_mesh"]
                    check(bool(sync), f"{kind}: no Cycles resource profile evidence")
                    for entry in sync:
                        check(entry.get("split_levels") == 0, f"{kind}: Geometry Supervision created residual slope images: {entry}")
                        check(entry.get("foam_field") == 1 and entry.get("spray_field") == 1, f"{kind}: full-spectrum foam/spray field resources absent: {entry}")
                        check(entry.get("ocean_ref_uv_std") == 1, f"{kind}: standard reference UV lookup unavailable; shader could fall back to corner attributes: {entry}")
                    if kind == "foam":
                        removal, tree = strip_legacy_attributes(obj)
                        try:
                            stripped, stripped_mesh, info = render(scene, obj, mod, outdir, "lod_foam_without_corner_attributes", kind, True)
                            proof = image_comparison(stripped, lod_images, kind)
                            report["field_lookup_proof"] = {"comparison": proof, "render": info,
                                                            "attributes": stripped_mesh["attrs"],
                                                            "geometry_equal": stripped_mesh["sha256"] == lod_mesh["sha256"]}
                            check(stripped_mesh["sha256"] == lod_mesh["sha256"], "Removing foam corner attributes changed the explicit surface")
                            check(not {"foam", "spray"}.intersection(stripped_mesh["attrs"]), "Field proof did not remove legacy attributes")
                            for eye, item in proof.items():
                                check(item["absolute_error"]["mean"] <= 0.005 and item["absolute_error"]["max"] <= 0.05,
                                      f"{eye}: foam changed after removing corner attributes; field shader lookup may be bypassed: {item}")
                        finally:
                            obj.modifiers.remove(removal)
                            bpy.data.node_groups.remove(tree)
            mod.use_camera_lod = True
            mod.use_foam = True
            mod.use_spray = True
            scene.render.engine = "BLENDER_EEVEE"
            fallback = snapshot(obj)
            report["non_cycles_fallback"] = mesh_report(fallback, views, args)
            check(fallback["sha256"] == dense["sha256"], "Non-Cycles foam/spray fallback must preserve dense geometry")
            scene.render.engine = "CYCLES"
            bpy.ops.wm.save_as_mainfile(filepath=str(outdir / "supervised_foam.blend"))
        report["status"] = "failed" if failures else "preflight_passed" if args.mesh_only else "passed"
    except Exception as exc:
        report["status"] = "error"
        report["exception"] = repr(exc)
        raise
    finally:
        (outdir / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("SUPERVISED_FOAM_REPORT", str(outdir / "report.json"), report.get("status"), flush=True)
    assert not failures, "\n".join(failures)


if __name__ == "__main__":
    main()
