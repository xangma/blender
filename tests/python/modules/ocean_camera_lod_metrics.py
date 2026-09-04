# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import bpy
import ctypes
import json
import math
import os
import platform
import shlex
import shutil
import socket
import sys
import tempfile
import time
import traceback

from dataclasses import asdict, dataclass, field
from pathlib import Path
from mathutils import Vector
from bpy_extras.object_utils import world_to_camera_view


RGB_MAE_TOL = 0.08
RGB_MAX_TOL = 0.35
REPROJ_MEAN_TOL = 0.50
REPROJ_MAX_TOL = 2.00
DEPTH_MEAN_TOL = 0.35
DEPTH_MAX_TOL = 1.20
NORMAL_MEAN_TOL_DEG = 3.0
NORMAL_MAX_TOL_DEG = 12.0
POSITION_MEAN_TOL = 0.35
POSITION_MAX_TOL = 1.50
OCEAN_SPLIT_SHADING_ENV = "BLENDER_OCEAN_SPLIT_SHADING"
OCEAN_CAMERA_LOD_SKIP_SETUP_ASSERTS_ENV = "BLENDER_OCEAN_LOD_SKIP_SETUP_ASSERTS"


def ocean_split_shading_mode():
    return os.environ.get(OCEAN_SPLIT_SHADING_ENV, "level0") or "level0"


@dataclass(frozen=True)
class DistributionStats:
    count: int
    min: float
    mean: float
    p95: float
    max: float


@dataclass(frozen=True)
class MetricThresholds:
    mean_max: float | None = None
    p95_max: float | None = None
    max_max: float | None = None


@dataclass(frozen=True)
class ScenarioThresholds:
    min_geometry_reduction: float | None = None
    max_geometry_reduction: float | None = None
    min_visible_samples: int = 1
    position: MetricThresholds = field(default_factory=MetricThresholds)
    reprojection: MetricThresholds = field(default_factory=MetricThresholds)
    depth: MetricThresholds = field(default_factory=MetricThresholds)
    geometric_normal_deg: MetricThresholds = field(default_factory=MetricThresholds)
    rgb_absolute: MetricThresholds = field(default_factory=MetricThresholds)
    luminance_absolute: MetricThresholds = field(default_factory=MetricThresholds)
    luminance_gradient: MetricThresholds = field(default_factory=MetricThresholds)


@dataclass(frozen=True)
class ScenarioSpec:
    name: str
    description: str
    thresholds: ScenarioThresholds


@dataclass(frozen=True)
class ScenarioContext:
    spec: ScenarioSpec
    obj_name: str
    cam_name: str


@dataclass(frozen=True)
class GeometryReport:
    lod_validation_mode: str
    lod_verts: int
    dense_verts: int
    geometry_reduction: float
    position_error: DistributionStats
    reprojection_error: DistributionStats
    depth_error: DistributionStats
    geometric_normal_error_deg: DistributionStats
    visible_sample_count: int
    support_sum: DistributionStats
    normal_semantics: str
    apparent_normal_available: bool


@dataclass(frozen=True)
class RenderReport:
    device: str
    samples: int
    resolution: int
    rgb_absolute_error: DistributionStats
    luminance_absolute_error: DistributionStats
    luminance_gradient_error: DistributionStats
    dense_luminance: DistributionStats
    dense_luminance_gradient: DistributionStats
    diff_abs_visualization_scale: float
    diff_gradient_visualization_scale: float


@dataclass(frozen=True)
class BenchmarkCaseResult:
    scenario: str
    description: str
    device: str
    mode: str
    ocean_split_shading: str
    status: str
    strict_passed: bool
    runtime_errors: list[str]
    threshold_failures: list[str]
    geometry: GeometryReport | None = None
    render: RenderReport | None = None
    eval_timings: dict[str, DistributionStats] = field(default_factory=dict)
    render_timings: dict[str, DistributionStats] = field(default_factory=dict)
    profile_entries: list[dict[str, object]] = field(default_factory=list)
    artifact_paths: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class BenchmarkRunSummary:
    blender_version: str
    blender_build_hash: str
    hostname: str
    platform: str
    outdir: str
    devices: list[str]
    scenarios: list[str]
    available_devices: list[str]
    mode: str
    ocean_split_shading: str
    samples: int
    resolution: int
    repeat_eval: int
    repeat_render: int
    overall_status: str
    case_count: int
    runtime_error_count: int
    threshold_failure_count: int
    cases: list[BenchmarkCaseResult]


def mean(values):
    return sum(values) / len(values) if values else 0.0


def percentile(values, fraction):
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


def summarize_values(values):
    if not values:
        return DistributionStats(count=0, min=0.0, mean=0.0, p95=0.0, max=0.0)
    return DistributionStats(
        count=len(values),
        min=min(values),
        mean=mean(values),
        p95=percentile(values, 0.95),
        max=max(values),
    )


def stats_dict(stats):
    return asdict(stats)


def geometry_report_dict(report):
    result = asdict(report)
    result["mean_position_error"] = report.position_error.mean
    result["p95_position_error"] = report.position_error.p95
    result["max_position_error"] = report.position_error.max
    result["mean_reprojection_error"] = report.reprojection_error.mean
    result["p95_reprojection_error"] = report.reprojection_error.p95
    result["max_reprojection_error"] = report.reprojection_error.max
    result["mean_depth_error"] = report.depth_error.mean
    result["p95_depth_error"] = report.depth_error.p95
    result["max_depth_error"] = report.depth_error.max
    result["mean_geometric_normal_error_deg"] = report.geometric_normal_error_deg.mean
    result["p95_geometric_normal_error_deg"] = report.geometric_normal_error_deg.p95
    result["max_geometric_normal_error_deg"] = report.geometric_normal_error_deg.max
    result["min_support_sum"] = report.support_sum.min
    result["mean_support_sum"] = report.support_sum.mean
    result["p95_support_sum"] = report.support_sum.p95
    result["max_support_sum"] = report.support_sum.max
    return result


def render_report_dict(report):
    result = asdict(report)
    result["mean_absolute_error"] = report.rgb_absolute_error.mean
    result["p95_absolute_error"] = report.rgb_absolute_error.p95
    result["max_absolute_error"] = report.rgb_absolute_error.max
    result["mean_luminance_error"] = report.luminance_absolute_error.mean
    result["p95_luminance_error"] = report.luminance_absolute_error.p95
    result["max_luminance_error"] = report.luminance_absolute_error.max
    result["mean_luminance_gradient_error"] = report.luminance_gradient_error.mean
    result["p95_luminance_gradient_error"] = report.luminance_gradient_error.p95
    result["max_luminance_gradient_error"] = report.luminance_gradient_error.max
    result["dense_luminance_range"] = report.dense_luminance.max - report.dense_luminance.min
    result["max_dense_luminance_gradient"] = report.dense_luminance_gradient.max
    return result


def clear_scene():
    bpy.ops.wm.read_homefile(use_empty=True, use_factory_startup=True)


def look_at(obj, target):
    direction = Vector(target) - obj.location
    obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()


def make_ocean_object(name="OceanObj",
                      geometry_mode="GENERATE",
                      resolution=6,
                      spatial_size=64,
                      size=1.0,
                      repeat_x=1,
                      repeat_y=1,
                      camera_lod=True,
                      lod_levels=4,
                      lod_pixel_error=0.5,
                      lod_policy="PIXEL_ERROR",
                      lod_min_wave_pixels=4.0,
                      lod_camera_full_spectrum_radius=0.0,
                      lod_usage_mode="GENERAL_RENDER",
                      lod_validation_mode="CAMERA_OBSERVABLE",
                      time_value=1.25,
                      choppiness=1.0,
                      wind_velocity=30.0,
                      roughness=0.04,
                      wave_scale=None,
                      smallest_wave=None,
                      wave_direction=None,
                      wave_alignment=None,
                      seed=None):
    mesh = bpy.data.meshes.new(f"{name}Mesh")
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)

    mod = obj.modifiers.new(name="Ocean", type="OCEAN")
    mod.geometry_mode = geometry_mode
    mod.resolution = resolution
    mod.viewport_resolution = resolution
    mod.spatial_size = spatial_size
    mod.size = size
    mod.repeat_x = repeat_x
    mod.repeat_y = repeat_y
    mod.use_normals = True
    mod.use_camera_lod = camera_lod
    mod.lod_levels = lod_levels
    if hasattr(mod, "lod_pixel_error"):
        mod.lod_pixel_error = lod_pixel_error
    if hasattr(mod, "lod_policy"):
        mod.lod_policy = lod_policy
    if hasattr(mod, "lod_min_wave_pixels"):
        mod.lod_min_wave_pixels = lod_min_wave_pixels
    if hasattr(mod, "lod_camera_full_spectrum_radius"):
        mod.lod_camera_full_spectrum_radius = lod_camera_full_spectrum_radius
    if hasattr(mod, "lod_usage_mode"):
        mod.lod_usage_mode = lod_usage_mode
    if hasattr(mod, "lod_validation_mode"):
        mod.lod_validation_mode = lod_validation_mode
    mod.choppiness = choppiness
    mod.time = time_value
    mod.wind_velocity = wind_velocity
    if wave_scale is not None:
        if hasattr(mod, "use_wave_scale"):
            mod.use_wave_scale = True
        mod.wave_scale = wave_scale
    if smallest_wave is not None:
        if hasattr(mod, "wave_scale_min"):
            mod.wave_scale_min = smallest_wave
        if hasattr(mod, "smallest_wave"):
            mod.smallest_wave = smallest_wave
    if wave_direction is not None:
        mod.wave_direction = wave_direction
    if wave_alignment is not None:
        mod.wave_alignment = wave_alignment
    if seed is not None:
        if hasattr(mod, "random_seed"):
            mod.random_seed = seed
        if hasattr(mod, "seed"):
            mod.seed = seed

    mat = bpy.data.materials.new(f"{name}Mat")
    mat.use_nodes = True
    obj.data.materials.append(mat)
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.02, 0.08, 0.12, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness

    return obj


def make_camera_and_light(cam_location=(0.0, -35.0, 8.0),
                          cam_target=(0.0, 0.0, 0.0),
                          lens=50.0,
                          sun_rotation=(0.8, 0.0, 0.6),
                          sun_energy=2.0):
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.render.resolution_x = 128
    scene.render.resolution_y = 128
    scene.render.resolution_percentage = 100

    cam_data = bpy.data.cameras.new("Cam")
    cam = bpy.data.objects.new("Cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    cam.location = cam_location
    look_at(cam, cam_target)
    cam.data.lens = lens

    sun_data = bpy.data.lights.new("Sun", type="SUN")
    sun = bpy.data.objects.new("Sun", sun_data)
    scene.collection.objects.link(sun)
    sun.rotation_euler = sun_rotation
    sun.data.energy = sun_energy

    return cam


def set_world_flat(color=(0.02, 0.02, 0.02, 1.0), strength=0.05):
    scene = bpy.context.scene
    if scene.world is None:
        scene.world = bpy.data.worlds.new("World")

    scene.world.use_nodes = True
    background = scene.world.node_tree.nodes["Background"]
    background.inputs["Color"].default_value = color
    background.inputs["Strength"].default_value = strength


def set_world_sky_gradient(strength=1.0):
    scene = bpy.context.scene
    if scene.world is None:
        scene.world = bpy.data.worlds.new("World")

    scene.world.use_nodes = True
    nodes = scene.world.node_tree.nodes
    links = scene.world.node_tree.links
    nodes.clear()

    output = nodes.new(type="ShaderNodeOutputWorld")
    background = nodes.new(type="ShaderNodeBackground")
    sky = nodes.new(type="ShaderNodeTexSky")
    sky_type_items = sky.bl_rna.properties["sky_type"].enum_items
    sky_types = {item.identifier for item in sky_type_items}
    for sky_type in ("NISHITA", "SINGLE_SCATTERING", "MULTIPLE_SCATTERING", "HOSEK_WILKIE", "PREETHAM"):
        if sky_type in sky_types:
            sky.sky_type = sky_type
            break
    if hasattr(sky, "sun_elevation"):
        sky.sun_elevation = 0.10
    if hasattr(sky, "sun_rotation"):
        sky.sun_rotation = 0.85
    if hasattr(sky, "air_density"):
        sky.air_density = 1.5
    if hasattr(sky, "dust_density"):
        sky.dust_density = 2.0
    if hasattr(sky, "ozone_density"):
        sky.ozone_density = 1.0
    background.inputs["Strength"].default_value = strength

    links.new(sky.outputs["Color"], background.inputs["Color"])
    links.new(background.outputs["Background"], output.inputs["Surface"])


def enable_stereo_multiview(cam,
                            interocular_distance=0.065,
                            convergence_distance=30.0,
                            convergence_mode="OFFAXIS"):
    scene = bpy.context.scene
    scene.render.use_multiview = True
    scene.render.views_format = "STEREO_3D"
    for view in scene.render.views:
        view.use = view.name.lower() in {"left", "right"}
    cam.data.stereo.interocular_distance = interocular_distance
    cam.data.stereo.convergence_distance = convergence_distance
    cam.data.stereo.convergence_mode = convergence_mode


def get_available_cycles_devices():
    devices = ["CPU"]
    prefs = bpy.context.preferences
    if "cycles" not in prefs.addons:
        return devices

    cprefs = prefs.addons["cycles"].preferences
    try:
        device_types = cprefs.get_device_types(bpy.context)
    except Exception:
        return devices

    for device_type, _, _, _ in device_types:
        try:
            type_devices = cprefs.get_devices_for_type(device_type)
        except Exception:
            continue
        if any(device.type == device_type for device in type_devices):
            devices.append(device_type)
            if device_type in {"HIP", "METAL", "ONEAPI"}:
                devices.append(f"{device_type}-RT")
            if device_type == "OPTIX":
                devices.append(f"{device_type}-OSL")

    return devices


def configure_cycles_device(device_name):
    requested = device_name.strip().upper()
    if not requested:
        raise ValueError("Empty device name is not valid")

    tokens = requested.split("-")
    device_type = tokens[0]
    suffixes = set(tokens[1:])

    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU" if device_type == "CPU" else "GPU"
    scene.cycles.shading_system = "OSL" in suffixes

    if device_type == "CPU":
        if suffixes.difference({"OSL"}):
            raise ValueError(f"Unsupported CPU device suffixes: {sorted(suffixes)}")
        return requested

    prefs = bpy.context.preferences
    if "cycles" not in prefs.addons:
        raise RuntimeError("Cycles preferences are not available")

    cprefs = prefs.addons["cycles"].preferences
    available_types = {item[0] for item in cprefs.get_device_types(bpy.context)}
    if device_type not in available_types:
        raise RuntimeError(
            f"Cycles device '{device_type}' is not available on this build. "
            f"Available devices: {get_available_cycles_devices()}"
        )

    cprefs.compute_device_type = device_type
    devices = cprefs.get_devices_for_type(device_type)
    if not any(device.type == device_type for device in devices):
        raise RuntimeError(
            f"Cycles device '{device_type}' has no enabled hardware entries. "
            f"Available devices: {get_available_cycles_devices()}"
        )

    for device in devices:
        device.use = device.type == device_type

    use_hwrt = "RT" in suffixes
    cprefs.use_hiprt = use_hwrt
    cprefs.use_oneapirt = use_hwrt
    cprefs.metalrt = "ON" if use_hwrt else "OFF"

    unsupported_suffixes = suffixes.difference({"RT", "OSL"})
    if unsupported_suffixes:
        raise ValueError(f"Unsupported Cycles device suffixes: {sorted(unsupported_suffixes)}")

    return requested


def render_with_cycles(filepath, samples=4, resolution=64, device="CPU"):
    scene = bpy.context.scene
    configure_cycles_device(device)
    scene.cycles.samples = samples
    scene.cycles.seed = 0
    scene.render.resolution_x = resolution
    scene.render.resolution_y = resolution
    scene.render.resolution_percentage = 100
    scene.view_settings.view_transform = "Standard"
    scene.display_settings.display_device = "sRGB"
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.filepath = filepath
    bpy.ops.render.render(write_still=True)


def load_rgb_pixels(filepath):
    image = bpy.data.images.load(filepath, check_existing=False)
    try:
        pixels = list(image.pixels[:])
        width, height = image.size[:]
    finally:
        bpy.data.images.remove(image)

    rgb = []
    for i in range(width * height):
        offset = i * 4
        rgb.append((pixels[offset], pixels[offset + 1], pixels[offset + 2]))
    return width, height, rgb


def rgb_to_rgba_pixels(rgb_pixels):
    rgba = []
    for r, g, b in rgb_pixels:
        rgba.extend((r, g, b, 1.0))
    return rgba


def combine_rgb_images(images):
    if not images:
        raise ValueError("At least one image is required to combine render outputs")

    heights = {height for _width, height, _pixels in images}
    if len(heights) != 1:
        raise ValueError(f"Render outputs must have matching heights, got {sorted(heights)}")

    combined_width = sum(width for width, _height, _pixels in images)
    height = images[0][1]
    combined = []
    row_starts = []
    for width, _height, pixels in images:
        row_starts.append((width, pixels))

    for y in range(height):
        for width, pixels in row_starts:
            row_offset = y * width
            combined.extend(pixels[row_offset:row_offset + width])

    return combined_width, height, combined


def render_output_paths(filepath):
    base = Path(filepath)
    if base.exists():
        return [base]

    multiview_paths = sorted(base.parent.glob(f"{base.stem}_*{base.suffix}"))
    if multiview_paths:
        return multiview_paths

    raise FileNotFoundError(f"Rendered output '{filepath}' was not produced")


def materialize_render_output(source_filepath, dest_filepath):
    paths = render_output_paths(source_filepath)
    if len(paths) == 1 and paths[0] == Path(source_filepath):
        shutil.copyfile(paths[0], dest_filepath)
        return load_rgb_pixels(str(dest_filepath))

    images = [load_rgb_pixels(str(path)) for path in paths]
    width, height, rgb = combine_rgb_images(images)
    save_rgba_image(dest_filepath, width, height, rgb_to_rgba_pixels(rgb))
    return width, height, rgb


def save_rgba_image(filepath, width, height, pixels):
    image = bpy.data.images.new(Path(filepath).stem, width=width, height=height, alpha=True)
    try:
        image.filepath_raw = filepath
        image.file_format = "PNG"
        image.pixels[:] = pixels
        image.save()
    finally:
        bpy.data.images.remove(image)


def angle_degrees(a, b):
    if a.length == 0.0 or b.length == 0.0:
        return 0.0
    dot = max(-1.0, min(1.0, a.normalized().dot(b.normalized())))
    return math.degrees(math.acos(dot))


def project_local_point(obj, cam, point_local):
    scene = bpy.context.scene
    point_world = obj.matrix_world @ point_local
    coord = world_to_camera_view(scene, cam, point_world)
    if coord.z <= 0.0:
        return None
    pixel = Vector((coord.x * scene.render.resolution_x, coord.y * scene.render.resolution_y))
    return pixel, coord.z


class DenseReferenceSampler:
    def __init__(self, mod, positions, normals=None):
        self.repeat_x = max(1, getattr(mod, "repeat_x", 1))
        self.repeat_y = max(1, getattr(mod, "repeat_y", 1))
        self.res_x = mod.resolution * mod.resolution * self.repeat_x
        self.res_y = mod.resolution * mod.resolution * self.repeat_y
        self.positions = positions
        self.normals = normals
        self.step_u = 1.0 / self.res_x
        self.step_v = 1.0 / self.res_y

    def _index(self, x, y):
        return y * (self.res_x + 1) + x

    def sample_grid(self, values, u, v):
        u = max(0.0, min(1.0, u))
        v = max(0.0, min(1.0, v))

        x = u * self.res_x
        y = v * self.res_y
        if x >= self.res_x:
            x0 = self.res_x - 1
            x1 = self.res_x
            fx = 1.0
        else:
            x0 = int(math.floor(x))
            x1 = x0 + 1
            fx = x - x0

        if y >= self.res_y:
            y0 = self.res_y - 1
            y1 = self.res_y
            fy = 1.0
        else:
            y0 = int(math.floor(y))
            y1 = y0 + 1
            fy = y - y0

        p00 = values[self._index(x0, y0)]
        p10 = values[self._index(x1, y0)]
        p01 = values[self._index(x0, y1)]
        p11 = values[self._index(x1, y1)]

        p0 = p00.lerp(p10, fx)
        p1 = p01.lerp(p11, fx)
        return p0.lerp(p1, fy)

    def sample_position(self, u, v):
        return self.sample_grid(self.positions, u, v)

    def sample(self, u, v):
        grid_u = u / self.repeat_x
        grid_v = v / self.repeat_y
        position = self.sample_position(grid_u, grid_v)

        if self.normals is not None:
            normal = self.sample_grid(self.normals, grid_u, grid_v)
            if normal.length != 0.0:
                normal.normalize()
            else:
                normal = Vector((0.0, 0.0, 1.0))
        else:
            u0 = max(0.0, grid_u - self.step_u)
            u1 = min(1.0, grid_u + self.step_u)
            v0 = max(0.0, grid_v - self.step_v)
            v1 = min(1.0, grid_v + self.step_v)

            tangent_u = self.sample_position(u1, grid_v) - self.sample_position(u0, grid_v)
            tangent_v = self.sample_position(grid_u, v1) - self.sample_position(grid_u, v0)
            normal = tangent_u.cross(tangent_v)
            if normal.length != 0.0:
                normal.normalize()
            else:
                normal = Vector((0.0, 0.0, 1.0))

        return position, normal


def evaluated_mesh_snapshot(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        attr_names = {attr.name for attr in mesh_eval.attributes}
        ref_coord_attr = mesh_eval.attributes.get("ocean_ref_coord")
        if ref_coord_attr is not None:
            x_coords = tuple(round(ref_coord_attr.data[i].vector[0], 6) for i in range(len(mesh_eval.vertices)))
            y_coords = tuple(round(ref_coord_attr.data[i].vector[2], 6) for i in range(len(mesh_eval.vertices)))
        else:
            x_coords = tuple(round(v.co.x, 6) for v in mesh_eval.vertices)
            y_coords = tuple(round(v.co.y, 6) for v in mesh_eval.vertices)
        finest_support_centroid = None
        support_attr = mesh_eval.attributes.get("ocean_geometry_support_covariance")
        if support_attr is not None and len(mesh_eval.vertices) > 0:
            support_values = [
                support_attr.data[i].vector[0] + support_attr.data[i].vector[2]
                for i in range(len(mesh_eval.vertices))
            ]
            min_support = min(support_values)
            finest_points = [
                mesh_eval.vertices[i].co
                for i, value in enumerate(support_values)
                if abs(value - min_support) < 1.0e-6
            ]
            if finest_points:
                centroid = sum((Vector((co.x, co.y)) for co in finest_points), Vector((0.0, 0.0)))
                centroid /= len(finest_points)
                finest_support_centroid = (round(centroid.x, 6), round(centroid.y, 6))
        return (
            len(mesh_eval.vertices),
            len(mesh_eval.polygons),
            attr_names,
            x_coords,
            y_coords,
            finest_support_centroid,
        )
    finally:
        obj_eval.to_mesh_clear()


def flush_process_stdout():
    sys.stdout.flush()
    try:
        ctypes.CDLL(None).fflush(None)
    except Exception:
        pass


def capture_process_stdout(callback):
    stdout_fd = sys.stdout.fileno()
    saved_stdout_fd = os.dup(stdout_fd)
    try:
        with tempfile.TemporaryFile(mode="w+b") as capture:
            flush_process_stdout()
            os.dup2(capture.fileno(), stdout_fd)
            try:
                callback()
                flush_process_stdout()
            finally:
                os.dup2(saved_stdout_fd, stdout_fd)
            capture.seek(0)
            return capture.read().decode("utf-8", errors="replace")
    finally:
        os.close(saved_stdout_fd)
        flush_process_stdout()


def parse_profile_value(text):
    if text in {"True", "False"}:
        return text == "True"
    try:
        if any(marker in text for marker in (".", "e", "E")):
            return float(text)
        return int(text)
    except ValueError:
        return text


def parse_ocean_camera_lod_profile(profile_log):
    entries = []
    for line in profile_log.splitlines():
        if not line.startswith("[OCEAN_CAMERA_LOD_PROFILE] "):
            continue
        payload = line.split("] ", 1)[1]
        entry = {}
        for token in shlex.split(payload):
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            entry[key] = parse_profile_value(value)
        entries.append(entry)
    return entries


def capture_camera_lod_profile(obj):
    profile_env = "BLENDER_OCEAN_CAMERA_LOD_PROFILE"
    previous = os.environ.get(profile_env)
    try:
        os.environ[profile_env] = "1"
        profile_log = capture_process_stdout(lambda: evaluated_mesh_snapshot(obj))
    finally:
        if previous is None:
            os.environ.pop(profile_env, None)
        else:
            os.environ[profile_env] = previous
    return profile_log, parse_ocean_camera_lod_profile(profile_log)


def assert_camera_lod_modifier_profile(obj, message):
    profile_log, _profile_entries = capture_camera_lod_profile(obj)
    assert "stage=modifier" in profile_log, message


def evaluated_mesh_stats(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        used_vertices = set()
        for poly in mesh_eval.polygons:
            used_vertices.update(poly.vertices)
        attr_names = {attr.name for attr in mesh_eval.attributes}
        return (
            len(mesh_eval.vertices),
            len(mesh_eval.polygons),
            len(mesh_eval.vertices) - len(used_vertices),
            attr_names,
        )
    finally:
        obj_eval.to_mesh_clear()


def dense_mesh_stats(obj):
    mod = obj.modifiers["Ocean"]
    original = mod.use_camera_lod
    mod.use_camera_lod = False
    try:
        return evaluated_mesh_stats(obj)
    finally:
        mod.use_camera_lod = original
        bpy.context.view_layer.update()


def assert_camera_lod_attributes(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        attr_names = {attr.name for attr in mesh_eval.attributes}
        expected = {
            "ocean_ref_coord",
            "ocean_geometry_normal",
            "ocean_geometry_support_covariance",
            "ocean_ref_uv",
            "ocean_camera_lod_level",
            "ocean_camera_lod_split_level",
            "ocean_camera_lod_morph",
            "ocean_camera_lod_radius",
        }
        missing = expected.difference(attr_names)
        if missing and os.environ.get(OCEAN_CAMERA_LOD_SKIP_SETUP_ASSERTS_ENV):
            return
        assert not missing, f"Missing camera LOD attributes: {sorted(missing)}"
    finally:
        obj_eval.to_mesh_clear()


def camera_lod_leaf_layout_report(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        expected_face_attrs = {
            "ocean_camera_lod_leaf_id": "leaf id",
            "ocean_camera_lod_leaf_level": "leaf local level",
            "ocean_camera_lod_leaf_split_level": "leaf split level",
            "ocean_camera_lod_cell_size": "leaf cell size",
        }
        attrs = {}
        for attr_name, label in expected_face_attrs.items():
            attr = mesh_eval.attributes.get(attr_name)
            assert attr is not None, f"Missing evaluated camera LOD {label} face attribute '{attr_name}'"
            assert attr.domain == "FACE", (
                f"Camera LOD {label} attribute '{attr_name}' must be FACE domain, got {attr.domain}"
            )
            assert len(attr.data) == len(mesh_eval.polygons), (
                f"Camera LOD {label} attribute '{attr_name}' length must match polygon count"
            )
            attrs[attr_name] = attr

        leaf_ids = [int(round(item.value)) for item in attrs["ocean_camera_lod_leaf_id"].data]
        local_levels = [
            int(round(item.value)) for item in attrs["ocean_camera_lod_leaf_level"].data
        ]
        split_levels = [
            int(round(item.value)) for item in attrs["ocean_camera_lod_leaf_split_level"].data
        ]
        cell_sizes = [float(item.value) for item in attrs["ocean_camera_lod_cell_size"].data]

        edge_faces = {}
        for face_index, poly in enumerate(mesh_eval.polygons):
            vertices = [int(index) for index in poly.vertices]
            for edge_index, vertex_a in enumerate(vertices):
                vertex_b = vertices[(edge_index + 1) % len(vertices)]
                key = tuple(sorted((vertex_a, vertex_b)))
                edge_faces.setdefault(key, []).append(face_index)

        max_adjacent_local_spread = 0
        max_adjacent_split_spread = 0
        nonmanifold_edge_count = 0
        for face_indices in edge_faces.values():
            if len(face_indices) > 2:
                nonmanifold_edge_count += 1
            if len(face_indices) != 2:
                continue
            face_a, face_b = face_indices
            max_adjacent_local_spread = max(
                max_adjacent_local_spread, abs(local_levels[face_a] - local_levels[face_b])
            )
            max_adjacent_split_spread = max(
                max_adjacent_split_spread, abs(split_levels[face_a] - split_levels[face_b])
            )

        def histogram(values):
            counts = {}
            for value in values:
                counts[value] = counts.get(value, 0) + 1
            return counts

        return {
            "contract": mesh_eval.get("ocean_camera_lod_contract"),
            "layout": mesh_eval.get("ocean_camera_lod_layout"),
            "face_count": len(mesh_eval.polygons),
            "unique_leaf_count": len(set(leaf_ids)),
            "level_histogram": histogram(local_levels),
            "split_level_histogram": histogram(split_levels),
            "min_cell_size": min(cell_sizes) if cell_sizes else 0.0,
            "max_cell_size": max(cell_sizes) if cell_sizes else 0.0,
            "max_adjacent_local_spread": max_adjacent_local_spread,
            "max_adjacent_split_spread": max_adjacent_split_spread,
            "nonmanifold_edge_count": nonmanifold_edge_count,
        }
    finally:
        obj_eval.to_mesh_clear()


def camera_lod_level_histogram(obj, attr_name="ocean_camera_lod_level"):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        attr = mesh_eval.attributes.get(attr_name)
        assert attr is not None, f"Missing evaluated mesh attribute '{attr_name}'"
        counts = {}
        for item in attr.data:
            level = int(round(item.value))
            counts[level] = counts.get(level, 0) + 1
        return counts
    finally:
        obj_eval.to_mesh_clear()


def camera_lod_topology_report(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        split_attr = mesh_eval.attributes.get("ocean_camera_lod_split_level")
        local_attr = mesh_eval.attributes.get("ocean_camera_lod_level")
        assert split_attr is not None, "Missing evaluated split-level metadata"
        assert local_attr is not None, "Missing evaluated local-level metadata"

        split_levels = [int(round(item.value)) for item in split_attr.data]
        local_levels = [int(round(item.value)) for item in local_attr.data]
        split_histogram = {}
        for level in split_levels:
            split_histogram[level] = split_histogram.get(level, 0) + 1

        max_face_split_spread = 0
        max_face_local_spread = 0
        max_face_vertex_count = 0
        for poly in mesh_eval.polygons:
            face_split_levels = [split_levels[index] for index in poly.vertices]
            face_local_levels = [local_levels[index] for index in poly.vertices]
            max_face_split_spread = max(
                max_face_split_spread, max(face_split_levels) - min(face_split_levels)
            )
            max_face_local_spread = max(
                max_face_local_spread, max(face_local_levels) - min(face_local_levels)
            )
            max_face_vertex_count = max(max_face_vertex_count, len(poly.vertices))

        return {
            "verts": len(mesh_eval.vertices),
            "faces": len(mesh_eval.polygons),
            "split_level_histogram": split_histogram,
            "max_face_split_spread": max_face_split_spread,
            "max_face_local_spread": max_face_local_spread,
            "max_face_vertex_count": max_face_vertex_count,
        }
    finally:
        obj_eval.to_mesh_clear()


def _camera_lod_reference_bounds(obj):
    mod = obj.modifiers["Ocean"]
    original = mod.use_camera_lod
    mod.use_camera_lod = True
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        ref_coord_attr = mesh_eval.attributes.get("ocean_ref_coord")
        assert ref_coord_attr is not None, "Missing evaluated reference coordinate metadata"

        bounds = []
        for poly in mesh_eval.polygons:
            coords = [ref_coord_attr.data[index].vector for index in poly.vertices]
            bounds.append((
                min(coord.x for coord in coords),
                max(coord.x for coord in coords),
                min(coord.z for coord in coords),
                max(coord.z for coord in coords),
            ))
        return bounds, len(mesh_eval.vertices), len(mesh_eval.polygons)
    finally:
        obj_eval.to_mesh_clear()
        mod.use_camera_lod = original
        bpy.context.view_layer.update()


def camera_lod_visible_coverage_report(obj, cam, max_dense_samples=12000):
    bounds, lod_verts, lod_faces = _camera_lod_reference_bounds(obj)
    mod = obj.modifiers["Ocean"]
    original = mod.use_camera_lod
    mod.use_camera_lod = False
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        dense_count = len(mesh_eval.vertices)
        sample_step = max(1, math.ceil(dense_count / max_dense_samples))
        domain_size = mod.spatial_size
        coverage_epsilon = max(1.0e-5, domain_size * 1.0e-6)
        visible_count = 0
        missing_visible = []
        visible_reference_x = []
        visible_reference_y = []

        def covered(reference_x, reference_y):
            for min_x, max_x, min_y, max_y in bounds:
                if (min_x - coverage_epsilon <= reference_x <= max_x + coverage_epsilon and
                        min_y - coverage_epsilon <= reference_y <= max_y + coverage_epsilon):
                    return True
            return False

        for index, vertex in enumerate(mesh_eval.vertices):
            if index % sample_step != 0:
                continue

            projected = project_local_point(obj, cam, vertex.co)
            if projected is None:
                continue
            pixel, _depth = projected
            if not (0.0 <= pixel.x <= bpy.context.scene.render.resolution_x and
                    0.0 <= pixel.y <= bpy.context.scene.render.resolution_y):
                continue

            visible_count += 1
            reference_x = vertex.co.x / max(abs(mod.size), 1.0e-8)
            reference_y = vertex.co.y / max(abs(mod.size), 1.0e-8)
            visible_reference_x.append(reference_x)
            visible_reference_y.append(reference_y)
            if not covered(reference_x, reference_y):
                missing_visible.append((round(reference_x, 6), round(reference_y, 6)))

        coverage_ratio = 1.0 if visible_count == 0 else 1.0 - (len(missing_visible) / visible_count)
        return {
            "lod_verts": lod_verts,
            "lod_faces": lod_faces,
            "dense_verts": dense_count,
            "visible_sample_count": visible_count,
            "missing_visible_sample_count": len(missing_visible),
            "coverage_ratio": coverage_ratio,
            "visible_reference_min_x": min(visible_reference_x) if visible_reference_x else 0.0,
            "visible_reference_max_x": max(visible_reference_x) if visible_reference_x else 0.0,
            "visible_reference_min_y": min(visible_reference_y) if visible_reference_y else 0.0,
            "visible_reference_max_y": max(visible_reference_y) if visible_reference_y else 0.0,
            "missing_visible_examples": missing_visible[:8],
        }
    finally:
        obj_eval.to_mesh_clear()
        mod.use_camera_lod = original
        bpy.context.view_layer.update()


def build_dense_reference(obj):
    mod = obj.modifiers["Ocean"]
    original_camera_lod = mod.use_camera_lod
    original_lod_levels = getattr(mod, "lod_levels", None)

    mod.use_camera_lod = True
    if original_lod_levels is not None:
        mod.lod_levels = 1
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        positions = [v.co.copy() for v in mesh_eval.vertices]
        geometry_normal_attr = mesh_eval.attributes.get("ocean_geometry_normal")
        if geometry_normal_attr is not None and len(geometry_normal_attr.data) == len(mesh_eval.vertices):
            normals = []
            for item in geometry_normal_attr.data:
                normal = Vector(item.vector)
                normal = Vector((normal.x, normal.z, normal.y))
                if normal.length != 0.0:
                    normal.normalize()
                else:
                    normal = Vector((0.0, 0.0, 1.0))
                normals.append(normal)
        else:
            normals = [v.normal.copy() for v in mesh_eval.vertices]
        sampler = DenseReferenceSampler(mod, positions, normals)
        dense_verts = len(mesh_eval.vertices)
    finally:
        obj_eval.to_mesh_clear()
        mod.use_camera_lod = original_camera_lod
        if original_lod_levels is not None:
            mod.lod_levels = original_lod_levels
        bpy.context.view_layer.update()

    return sampler, dense_verts


def build_camera_lod_samples(obj):
    mod = obj.modifiers["Ocean"]
    original = mod.use_camera_lod
    mod.use_camera_lod = True
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        ref_coord_attr = mesh_eval.attributes.get("ocean_ref_coord")
        geometry_normal_attr = mesh_eval.attributes.get("ocean_geometry_normal")
        support_attr = mesh_eval.attributes.get("ocean_geometry_support_covariance")
        assert ref_coord_attr is not None
        assert geometry_normal_attr is not None
        assert support_attr is not None

        samples = []
        for i, vert in enumerate(mesh_eval.vertices):
            ref_coord = Vector(ref_coord_attr.data[i].vector)
            geometry_normal_canonical = Vector(geometry_normal_attr.data[i].vector)
            geometry_normal_object = Vector(
                (geometry_normal_canonical.x, geometry_normal_canonical.z, geometry_normal_canonical.y)
            )
            if geometry_normal_object.length != 0.0:
                geometry_normal_object.normalize()
            support_cov = support_attr.data[i].vector
            samples.append({
                "position": vert.co.copy(),
                "geometry_normal": geometry_normal_object,
                "u": (ref_coord.x / mod.spatial_size) + 0.5,
                "v": (ref_coord.z / mod.spatial_size) + 0.5,
                "support_sum": support_cov[0] + support_cov[2],
            })
        return samples
    finally:
        obj_eval.to_mesh_clear()
        mod.use_camera_lod = original
        bpy.context.view_layer.update()


def _build_geometry_report(obj, cam, sampler, dense_verts, lod_samples):
    mod = obj.modifiers["Ocean"]

    position_errors = []
    reprojection_errors = []
    depth_errors = []
    geometric_normal_errors = []
    support_values = []
    visible_count = 0

    for sample in lod_samples:
        dense_position, dense_normal = sampler.sample(sample["u"], sample["v"])
        position_errors.append((sample["position"] - dense_position).length)
        support_values.append(sample["support_sum"])

        lod_proj = project_local_point(obj, cam, sample["position"])
        dense_proj = project_local_point(obj, cam, dense_position)
        if lod_proj is None or dense_proj is None:
            continue

        lod_pixel, lod_depth = lod_proj
        dense_pixel, dense_depth = dense_proj
        if not (0.0 <= dense_pixel.x <= bpy.context.scene.render.resolution_x and
                0.0 <= dense_pixel.y <= bpy.context.scene.render.resolution_y):
            continue
        reprojection_errors.append((lod_pixel - dense_pixel).length)
        depth_errors.append(abs(lod_depth - dense_depth))
        geometric_normal_errors.append(angle_degrees(sample["geometry_normal"], dense_normal))
        visible_count += 1

    return GeometryReport(
        lod_validation_mode=getattr(mod, "lod_validation_mode", "CAMERA_OBSERVABLE"),
        lod_verts=len(lod_samples),
        dense_verts=dense_verts,
        geometry_reduction=1.0 - (len(lod_samples) / dense_verts),
        position_error=summarize_values(position_errors),
        reprojection_error=summarize_values(reprojection_errors),
        depth_error=summarize_values(depth_errors),
        geometric_normal_error_deg=summarize_values(geometric_normal_errors),
        visible_sample_count=visible_count,
        support_sum=summarize_values(support_values),
        normal_semantics="GEOMETRIC_NORMAL",
        apparent_normal_available=False,
    )


def camera_lod_reference_report(obj, cam):
    sampler, dense_verts = build_dense_reference(obj)
    lod_samples = build_camera_lod_samples(obj)
    report = _build_geometry_report(obj, cam, sampler, dense_verts, lod_samples)
    report_dict_value = geometry_report_dict(report)
    print("Camera LOD reference report:", report_dict_value)
    return report_dict_value


def benchmark_geometry_case(obj, cam, repeat_eval):
    dense_times = []
    lod_times = []
    sampler = None
    dense_verts = 0
    lod_samples = None

    profile_log, profile_entries = capture_camera_lod_profile(obj)

    for _index in range(repeat_eval):
        start = time.perf_counter()
        sampler, dense_verts = build_dense_reference(obj)
        dense_times.append(time.perf_counter() - start)

        start = time.perf_counter()
        lod_samples = build_camera_lod_samples(obj)
        lod_times.append(time.perf_counter() - start)

    assert sampler is not None
    assert lod_samples is not None

    geometry = _build_geometry_report(obj, cam, sampler, dense_verts, lod_samples)
    return geometry, {
        "dense_eval_s": summarize_values(dense_times),
        "lod_eval_s": summarize_values(lod_times),
    }, profile_log, profile_entries


def luminance(pixel):
    return (0.2126 * pixel[0]) + (0.7152 * pixel[1]) + (0.0722 * pixel[2])


def luminance_gradient_map(values, width, height):
    def sample(x, y):
        x = max(0, min(width - 1, x))
        y = max(0, min(height - 1, y))
        return values[(y * width) + x]

    gradients = []
    for y in range(height):
        for x in range(width):
            dx = sample(x + 1, y) - sample(x - 1, y)
            dy = sample(x, y + 1) - sample(x, y - 1)
            gradients.append(0.5 * math.sqrt((dx * dx) + (dy * dy)))
    return gradients


def compute_render_report(lod_rgb, dense_rgb, width, height):
    rgb_errors = []
    diff_abs_pixels = []
    diff_channel_max = 0.0
    for lod_pixel, dense_pixel in zip(lod_rgb, dense_rgb):
        dr = abs(lod_pixel[0] - dense_pixel[0])
        dg = abs(lod_pixel[1] - dense_pixel[1])
        db = abs(lod_pixel[2] - dense_pixel[2])
        diff_channel_max = max(diff_channel_max, dr, dg, db)
        rgb_errors.append((dr + dg + db) / 3.0)
        diff_abs_pixels.append((dr, dg, db))

    lod_luminance = [luminance(pixel) for pixel in lod_rgb]
    dense_luminance = [luminance(pixel) for pixel in dense_rgb]
    luminance_errors = [abs(a - b) for a, b in zip(lod_luminance, dense_luminance)]

    lod_gradients = luminance_gradient_map(lod_luminance, width, height)
    dense_gradients = luminance_gradient_map(dense_luminance, width, height)
    gradient_errors = [abs(a - b) for a, b in zip(lod_gradients, dense_gradients)]

    diff_abs_scale = 1.0 / max(diff_channel_max, 1.0e-6)
    diff_gradient_scale = 1.0 / max(max(gradient_errors) if gradient_errors else 0.0, 1.0e-6)

    rgba_abs = []
    for dr, dg, db in diff_abs_pixels:
        rgba_abs.extend((
            min(dr * diff_abs_scale, 1.0),
            min(dg * diff_abs_scale, 1.0),
            min(db * diff_abs_scale, 1.0),
            1.0,
        ))

    rgba_gradient = []
    for value in gradient_errors:
        display = min(value * diff_gradient_scale, 1.0)
        rgba_gradient.extend((display, display, display, 1.0))

    report = RenderReport(
        device="",
        samples=0,
        resolution=width,
        rgb_absolute_error=summarize_values(rgb_errors),
        luminance_absolute_error=summarize_values(luminance_errors),
        luminance_gradient_error=summarize_values(gradient_errors),
        dense_luminance=summarize_values(dense_luminance),
        dense_luminance_gradient=summarize_values(dense_gradients),
        diff_abs_visualization_scale=diff_abs_scale,
        diff_gradient_visualization_scale=diff_gradient_scale,
    )
    return report, rgba_abs, rgba_gradient


def _render_pair(obj, lod_path, dense_path, samples, resolution, device):
    mod = obj.modifiers["Ocean"]
    original = mod.use_camera_lod
    try:
        mod.use_camera_lod = True
        bpy.context.view_layer.update()
        start = time.perf_counter()
        render_with_cycles(lod_path, samples=samples, resolution=resolution, device=device)
        lod_time = time.perf_counter() - start

        mod.use_camera_lod = False
        bpy.context.view_layer.update()
        start = time.perf_counter()
        render_with_cycles(dense_path, samples=samples, resolution=resolution, device=device)
        dense_time = time.perf_counter() - start
    finally:
        mod.use_camera_lod = original
        bpy.context.view_layer.update()

    return lod_time, dense_time


def set_attribute_emission_material(obj, attribute_name, strength=1.0):
    if not obj.data.materials:
        obj.data.materials.append(bpy.data.materials.new(f"{obj.name}AttributeMat"))

    mat = obj.data.materials[0]
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()

    output = nodes.new(type="ShaderNodeOutputMaterial")
    attr = nodes.new(type="ShaderNodeAttribute")
    attr.attribute_name = attribute_name
    emission = nodes.new(type="ShaderNodeEmission")
    emission.inputs["Strength"].default_value = strength

    links.new(attr.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])


def benchmark_render_case(obj,
                          case_dir,
                          device,
                          samples,
                          resolution,
                          repeat_render,
                          keep_intermediates):
    case_dir = Path(case_dir)
    tempdir = Path(tempfile.mkdtemp(prefix="ocean_camera_lod_render_"))
    lod_times = []
    dense_times = []
    try:
        temp_lod = tempdir / "lod.png"
        temp_dense = tempdir / "dense.png"
        final_lod = case_dir / "lod.png"
        final_dense = case_dir / "dense.png"

        for repeat_index in range(repeat_render):
            lod_time, dense_time = _render_pair(
                obj,
                str(temp_lod),
                str(temp_dense),
                samples=samples,
                resolution=resolution,
                device=device,
            )
            lod_times.append(lod_time)
            dense_times.append(dense_time)

            if keep_intermediates and repeat_render > 1:
                materialize_render_output(temp_lod, str(case_dir / f"lod_repeat_{repeat_index + 1}.png"))
                materialize_render_output(temp_dense, str(case_dir / f"dense_repeat_{repeat_index + 1}.png"))

        width, height, lod_rgb = materialize_render_output(temp_lod, str(final_lod))
        dense_width, dense_height, dense_rgb = materialize_render_output(temp_dense, str(final_dense))
        assert (width, height) == (dense_width, dense_height)

        render_report, rgba_abs, rgba_gradient = compute_render_report(lod_rgb, dense_rgb, width, height)
        render_report = RenderReport(
            device=device,
            samples=samples,
            resolution=resolution,
            rgb_absolute_error=render_report.rgb_absolute_error,
            luminance_absolute_error=render_report.luminance_absolute_error,
            luminance_gradient_error=render_report.luminance_gradient_error,
            dense_luminance=render_report.dense_luminance,
            dense_luminance_gradient=render_report.dense_luminance_gradient,
            diff_abs_visualization_scale=render_report.diff_abs_visualization_scale,
            diff_gradient_visualization_scale=render_report.diff_gradient_visualization_scale,
        )

        save_rgba_image(str(case_dir / "diff_abs.png"), width, height, rgba_abs)
        save_rgba_image(str(case_dir / "diff_gradient.png"), width, height, rgba_gradient)

        return render_report, {
            "lod_render_s": summarize_values(lod_times),
            "dense_render_s": summarize_values(dense_times),
        }
    finally:
        shutil.rmtree(tempdir, ignore_errors=True)


def render_rgb_difference_report(obj, samples=4, resolution=64, device="CPU"):
    with tempfile.TemporaryDirectory(prefix="ocean_camera_lod_rgb_") as tempdir:
        case_dir = Path(tempdir)
        render_report, _timings = benchmark_render_case(
            obj,
            case_dir,
            device=device,
            samples=samples,
            resolution=resolution,
            repeat_render=1,
            keep_intermediates=False,
        )
    report_dict_value = render_report_dict(render_report)
    print("Camera LOD RGB report:", report_dict_value)
    return report_dict_value


def render_attribute_difference_report(obj, attribute_name, samples=1, resolution=64, device="CPU"):
    set_attribute_emission_material(obj, attribute_name)
    report_dict_value = render_rgb_difference_report(
        obj,
        samples=samples,
        resolution=resolution,
        device=device,
    )
    report_dict_value["attribute_name"] = attribute_name
    print("Camera LOD attribute RGB report:", report_dict_value)
    return report_dict_value


def write_json(filepath, data):
    with open(filepath, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2, sort_keys=True)
        handle.write("\n")


def write_text(filepath, text):
    with open(filepath, "w", encoding="utf-8") as handle:
        handle.write(text)


def sanitize_path_component(name):
    return "".join(ch if ch.isalnum() or ch in {"-", "_", "."} else "_" for ch in name)


def metric_threshold_failures(label, stats, thresholds):
    failures = []
    if thresholds.mean_max is not None and stats.mean > thresholds.mean_max:
        failures.append(f"{label}.mean {stats.mean:.6f} > {thresholds.mean_max:.6f}")
    if thresholds.p95_max is not None and stats.p95 > thresholds.p95_max:
        failures.append(f"{label}.p95 {stats.p95:.6f} > {thresholds.p95_max:.6f}")
    if thresholds.max_max is not None and stats.max > thresholds.max_max:
        failures.append(f"{label}.max {stats.max:.6f} > {thresholds.max_max:.6f}")
    return failures


def evaluate_case_thresholds(spec, geometry, render):
    thresholds = spec.thresholds
    failures = []

    if thresholds.min_geometry_reduction is not None and geometry.geometry_reduction < thresholds.min_geometry_reduction:
        failures.append(
            f"geometry_reduction {geometry.geometry_reduction:.6f} < {thresholds.min_geometry_reduction:.6f}"
        )
    if thresholds.max_geometry_reduction is not None and geometry.geometry_reduction > thresholds.max_geometry_reduction:
        failures.append(
            f"geometry_reduction {geometry.geometry_reduction:.6f} > {thresholds.max_geometry_reduction:.6f}"
        )
    if geometry.visible_sample_count < thresholds.min_visible_samples:
        failures.append(
            f"visible_sample_count {geometry.visible_sample_count} < {thresholds.min_visible_samples}"
        )

    failures.extend(metric_threshold_failures("position_error", geometry.position_error, thresholds.position))
    failures.extend(metric_threshold_failures("reprojection_error", geometry.reprojection_error, thresholds.reprojection))
    failures.extend(metric_threshold_failures("depth_error", geometry.depth_error, thresholds.depth))
    failures.extend(metric_threshold_failures(
        "geometric_normal_error_deg", geometry.geometric_normal_error_deg, thresholds.geometric_normal_deg))
    failures.extend(metric_threshold_failures("rgb_absolute_error", render.rgb_absolute_error, thresholds.rgb_absolute))
    failures.extend(metric_threshold_failures(
        "luminance_absolute_error", render.luminance_absolute_error, thresholds.luminance_absolute))
    failures.extend(metric_threshold_failures(
        "luminance_gradient_error", render.luminance_gradient_error, thresholds.luminance_gradient))

    return failures


def _scenario_calm_reference():
    clear_scene()
    set_world_flat()
    cam = make_camera_and_light(
        cam_location=(0.0, -26.0, 6.0),
        cam_target=(0.0, 24.0, 0.0),
        lens=40.0,
        sun_rotation=(0.48, 0.0, 0.82),
        sun_energy=2.0,
    )
    obj = make_ocean_object(
        name="OceanBenchmarkCalm",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
        roughness=0.04,
    )
    assert_camera_lod_attributes(obj)
    return ScenarioContext(CURATED_SCENARIOS["calm_reference"], obj.name, cam.name)


def _scenario_high_energy_dense_ceiling():
    clear_scene()
    set_world_flat()
    cam = make_camera_and_light(
        cam_location=(0.0, -26.0, 6.0),
        cam_target=(0.0, 24.0, 0.0),
        lens=40.0,
        sun_rotation=(0.35, 0.0, 0.72),
        sun_energy=2.0,
    )
    obj = make_ocean_object(
        name="OceanBenchmarkDenseCeiling",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        lod_validation_mode="GEOMETRY_STRICT",
        time_value=1.0,
        choppiness=1.5,
        wind_velocity=30.0,
        roughness=0.025,
    )
    assert_camera_lod_attributes(obj)
    return ScenarioContext(CURATED_SCENARIOS["high_energy_dense_ceiling"], obj.name, cam.name)


def _scenario_grazing_light_adversarial():
    clear_scene()
    set_world_flat()
    cam = make_camera_and_light(
        cam_location=(0.0, -58.0, 3.0),
        cam_target=(0.0, 180.0, 0.0),
        lens=55.0,
        sun_rotation=(0.18, 0.0, 0.75),
        sun_energy=2.0,
    )
    obj = make_ocean_object(
        name="OceanBenchmarkGrazing",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        lod_validation_mode="CAMERA_OBSERVABLE",
        time_value=1.0,
        choppiness=1.3,
        wind_velocity=18.0,
        roughness=0.02,
    )
    assert_camera_lod_attributes(obj)
    return ScenarioContext(CURATED_SCENARIOS["grazing_light_adversarial"], obj.name, cam.name)


def _scenario_foam_attribute():
    clear_scene()
    set_world_flat(strength=0.0)
    cam = make_camera_and_light(
        cam_location=(0.0, -58.0, 3.0),
        cam_target=(0.0, 180.0, 0.0),
        lens=55.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanBenchmarkFoamAttribute",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        lod_validation_mode="CAMERA_OBSERVABLE",
        time_value=1.0,
        choppiness=1.3,
        wind_velocity=18.0,
        roughness=0.02,
    )
    mod = obj.modifiers["Ocean"]
    mod.use_foam = True
    mod.foam_layer_name = "foam"
    mod.foam_coverage = 0.0
    set_attribute_emission_material(obj, "foam")
    assert_camera_lod_attributes(obj)
    return ScenarioContext(CURATED_SCENARIOS["foam_attribute"], obj.name, cam.name)


def _scenario_temporal_camera_move():
    clear_scene()
    set_world_flat()
    cam = make_camera_and_light(
        cam_location=(0.0, -24.0, 5.0),
        cam_target=(0.0, 20.0, 0.0),
        lens=35.0,
        sun_rotation=(0.42, 0.0, 0.74),
        sun_energy=2.0,
    )
    obj = make_ocean_object(
        name="OceanBenchmarkTemporal",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
        roughness=0.035,
    )
    mod = obj.modifiers["Ocean"]
    base_cell = (mod.size * mod.spatial_size) / float(mod.resolution * mod.resolution)
    cam.location.x += 5.0 * base_cell
    look_at(cam, (5.0 * base_cell, 20.0, 0.0))
    assert_camera_lod_attributes(obj)
    return ScenarioContext(CURATED_SCENARIOS["temporal_camera_move"], obj.name, cam.name)


def _scenario_stereo_dataset_valid():
    clear_scene()
    set_world_flat()
    cam = make_camera_and_light(
        cam_location=(0.0, -26.0, 6.0),
        cam_target=(0.0, 24.0, 0.0),
        lens=40.0,
        sun_rotation=(0.44, 0.0, 0.68),
        sun_energy=2.0,
    )
    enable_stereo_multiview(cam, interocular_distance=0.08, convergence_distance=36.0)
    obj = make_ocean_object(
        name="OceanBenchmarkStereoDataset",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        lod_usage_mode="STEREO_DATASET",
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
        roughness=0.035,
    )
    assert_camera_lod_attributes(obj)
    return ScenarioContext(CURATED_SCENARIOS["stereo_dataset_valid"], obj.name, cam.name)


def _scenario_repeat_tiles():
    clear_scene()
    set_world_flat()
    cam = make_camera_and_light(
        cam_location=(58.0, 58.0, 7.0),
        cam_target=(62.0, 62.0, 0.0),
        lens=38.0,
        sun_rotation=(0.44, 0.0, 0.68),
        sun_energy=2.0,
    )
    obj = make_ocean_object(
        name="OceanBenchmarkRepeatTiles",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        repeat_x=2,
        repeat_y=2,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
        roughness=0.035,
    )
    assert_camera_lod_attributes(obj)
    return ScenarioContext(CURATED_SCENARIOS["repeat_tiles"], obj.name, cam.name)


CURATED_SCENARIOS = {
    "calm_reference": ScenarioSpec(
        name="calm_reference",
        description="Forward-view calm water with visible shading to measure baseline geometry and render loss.",
        thresholds=ScenarioThresholds(
            min_geometry_reduction=0.10,
            min_visible_samples=1,
            position=MetricThresholds(mean_max=POSITION_MEAN_TOL, p95_max=0.90, max_max=POSITION_MAX_TOL),
            reprojection=MetricThresholds(mean_max=REPROJ_MEAN_TOL, p95_max=1.20, max_max=REPROJ_MAX_TOL),
            depth=MetricThresholds(mean_max=DEPTH_MEAN_TOL, p95_max=0.80, max_max=DEPTH_MAX_TOL),
            geometric_normal_deg=MetricThresholds(mean_max=NORMAL_MEAN_TOL_DEG, p95_max=8.0),
            rgb_absolute=MetricThresholds(mean_max=RGB_MAE_TOL, p95_max=0.20),
            luminance_absolute=MetricThresholds(mean_max=0.08, p95_max=0.18),
            luminance_gradient=MetricThresholds(mean_max=0.10, p95_max=0.24),
        ),
    ),
    "high_energy_dense_ceiling": ScenarioSpec(
        name="high_energy_dense_ceiling",
        description="High-energy geometry-strict case that should stay near the dense reference ceiling.",
        thresholds=ScenarioThresholds(
            max_geometry_reduction=0.20,
            min_visible_samples=1,
            position=MetricThresholds(mean_max=0.01, p95_max=0.02, max_max=0.25),
            reprojection=MetricThresholds(mean_max=REPROJ_MEAN_TOL, p95_max=1.20, max_max=REPROJ_MAX_TOL),
            depth=MetricThresholds(mean_max=DEPTH_MEAN_TOL, p95_max=0.80, max_max=DEPTH_MAX_TOL),
            geometric_normal_deg=MetricThresholds(mean_max=NORMAL_MEAN_TOL_DEG, p95_max=8.0),
            rgb_absolute=MetricThresholds(mean_max=0.02, p95_max=0.05),
            luminance_absolute=MetricThresholds(mean_max=0.02, p95_max=0.05),
            luminance_gradient=MetricThresholds(mean_max=0.03, p95_max=0.08),
        ),
    ),
    "grazing_light_adversarial": ScenarioSpec(
        name="grazing_light_adversarial",
        description=(
            "Grazing-light stress case where small geometry changes noticeably perturb highlights and "
            "the safest outcome may be to stay dense."
        ),
        thresholds=ScenarioThresholds(
            min_visible_samples=1,
            position=MetricThresholds(mean_max=POSITION_MEAN_TOL, p95_max=1.00, max_max=POSITION_MAX_TOL),
            reprojection=MetricThresholds(mean_max=REPROJ_MEAN_TOL, p95_max=1.40, max_max=REPROJ_MAX_TOL),
            depth=MetricThresholds(mean_max=DEPTH_MEAN_TOL, p95_max=0.90, max_max=DEPTH_MAX_TOL),
            geometric_normal_deg=MetricThresholds(mean_max=NORMAL_MEAN_TOL_DEG, p95_max=8.0),
            rgb_absolute=MetricThresholds(mean_max=0.12, p95_max=0.26),
            luminance_absolute=MetricThresholds(mean_max=0.10, p95_max=0.22),
            luminance_gradient=MetricThresholds(mean_max=0.14, p95_max=0.30),
        ),
    ),
    "temporal_camera_move": ScenarioSpec(
        name="temporal_camera_move",
        description="Camera moved laterally by multiple dense cells to benchmark the post-move mesh selection.",
        thresholds=ScenarioThresholds(
            min_geometry_reduction=0.10,
            min_visible_samples=1,
            position=MetricThresholds(mean_max=POSITION_MEAN_TOL, p95_max=0.90, max_max=POSITION_MAX_TOL),
            reprojection=MetricThresholds(mean_max=REPROJ_MEAN_TOL, p95_max=1.20, max_max=REPROJ_MAX_TOL),
            depth=MetricThresholds(mean_max=DEPTH_MEAN_TOL, p95_max=0.80, max_max=DEPTH_MAX_TOL),
            geometric_normal_deg=MetricThresholds(mean_max=NORMAL_MEAN_TOL_DEG, p95_max=8.0),
            rgb_absolute=MetricThresholds(mean_max=0.09, p95_max=0.20),
            luminance_absolute=MetricThresholds(mean_max=0.08, p95_max=0.18),
            luminance_gradient=MetricThresholds(mean_max=0.11, p95_max=0.24),
        ),
    ),
    "foam_attribute": ScenarioSpec(
        name="foam_attribute",
        description="Foam attribute rendered as pure emission to compare dense and camera LOD sampling.",
        thresholds=ScenarioThresholds(
            min_geometry_reduction=0.10,
            min_visible_samples=1,
            position=MetricThresholds(mean_max=POSITION_MEAN_TOL, p95_max=1.00, max_max=POSITION_MAX_TOL),
            reprojection=MetricThresholds(mean_max=REPROJ_MEAN_TOL, p95_max=1.40, max_max=REPROJ_MAX_TOL),
            depth=MetricThresholds(mean_max=DEPTH_MEAN_TOL, p95_max=0.90, max_max=DEPTH_MAX_TOL),
            geometric_normal_deg=MetricThresholds(mean_max=NORMAL_MEAN_TOL_DEG, p95_max=8.0),
            rgb_absolute=MetricThresholds(mean_max=0.005, p95_max=0.02, max_max=0.05),
            luminance_absolute=MetricThresholds(mean_max=0.005, p95_max=0.02, max_max=0.05),
            luminance_gradient=MetricThresholds(mean_max=0.005, p95_max=0.02, max_max=0.05),
        ),
    ),
    "stereo_dataset_valid": ScenarioSpec(
        name="stereo_dataset_valid",
        description="Valid stereo dataset case using a true stereo-3D left/right view pair.",
        thresholds=ScenarioThresholds(
            min_geometry_reduction=0.05,
            min_visible_samples=1,
            position=MetricThresholds(mean_max=POSITION_MEAN_TOL, p95_max=0.90, max_max=POSITION_MAX_TOL),
            reprojection=MetricThresholds(mean_max=REPROJ_MEAN_TOL, p95_max=1.20, max_max=REPROJ_MAX_TOL),
            depth=MetricThresholds(mean_max=DEPTH_MEAN_TOL, p95_max=0.80, max_max=DEPTH_MAX_TOL),
            geometric_normal_deg=MetricThresholds(mean_max=NORMAL_MEAN_TOL_DEG, p95_max=8.0),
            rgb_absolute=MetricThresholds(mean_max=0.10, p95_max=0.22),
            luminance_absolute=MetricThresholds(mean_max=0.09, p95_max=0.20),
            luminance_gradient=MetricThresholds(mean_max=0.11, p95_max=0.26),
        ),
    ),
    "repeat_tiles": ScenarioSpec(
        name="repeat_tiles",
        description="Forward-view calm water looking into the second repeated tile.",
        thresholds=ScenarioThresholds(
            min_geometry_reduction=0.10,
            min_visible_samples=1,
            position=MetricThresholds(mean_max=POSITION_MEAN_TOL, p95_max=1.00, max_max=POSITION_MAX_TOL),
            reprojection=MetricThresholds(mean_max=REPROJ_MEAN_TOL, p95_max=1.20, max_max=REPROJ_MAX_TOL),
            depth=MetricThresholds(mean_max=DEPTH_MEAN_TOL, p95_max=0.80, max_max=DEPTH_MAX_TOL),
            geometric_normal_deg=MetricThresholds(mean_max=NORMAL_MEAN_TOL_DEG, p95_max=8.0),
            rgb_absolute=MetricThresholds(mean_max=0.10, p95_max=0.22),
            luminance_absolute=MetricThresholds(mean_max=0.09, p95_max=0.20),
            luminance_gradient=MetricThresholds(mean_max=0.11, p95_max=0.26),
        ),
    ),
}


SCENARIO_BUILDERS = {
    "calm_reference": _scenario_calm_reference,
    "high_energy_dense_ceiling": _scenario_high_energy_dense_ceiling,
    "grazing_light_adversarial": _scenario_grazing_light_adversarial,
    "foam_attribute": _scenario_foam_attribute,
    "temporal_camera_move": _scenario_temporal_camera_move,
    "stereo_dataset_valid": _scenario_stereo_dataset_valid,
    "repeat_tiles": _scenario_repeat_tiles,
}


def setup_curated_scenario(name):
    if name not in SCENARIO_BUILDERS:
        raise ValueError(f"Unknown benchmark scenario '{name}'. Choices: {sorted(CURATED_SCENARIOS)}")
    return SCENARIO_BUILDERS[name]()


def get_benchmark_case_paths(outdir, scenario_name, device_name):
    case_dir = Path(outdir) / sanitize_path_component(scenario_name) / sanitize_path_component(device_name)
    return {
        "case_dir": case_dir,
        "lod": case_dir / "lod.png",
        "dense": case_dir / "dense.png",
        "diff_abs": case_dir / "diff_abs.png",
        "diff_gradient": case_dir / "diff_gradient.png",
        "profile": case_dir / "profile.log",
        "case": case_dir / "case.json",
    }


def build_case_artifact_paths(paths):
    return {key: str(value) for key, value in paths.items() if key != "case_dir"}


def benchmark_case(scenario_name,
                   device_name,
                   mode,
                   outdir,
                   samples,
                   resolution,
                   repeat_eval,
                   repeat_render,
                   keep_intermediates):
    paths = get_benchmark_case_paths(outdir, scenario_name, device_name)
    paths["case_dir"].mkdir(parents=True, exist_ok=True)

    context = setup_curated_scenario(scenario_name)
    obj = bpy.data.objects[context.obj_name]
    cam = bpy.data.objects[context.cam_name]

    geometry, eval_timings, profile_log, profile_entries = benchmark_geometry_case(
        obj,
        cam,
        repeat_eval=repeat_eval,
    )
    render, render_timings = benchmark_render_case(
        obj,
        case_dir=paths["case_dir"],
        device=device_name,
        samples=samples,
        resolution=resolution,
        repeat_render=repeat_render,
        keep_intermediates=keep_intermediates,
    )

    write_text(paths["profile"], profile_log)

    threshold_failures = evaluate_case_thresholds(context.spec, geometry, render)
    strict_passed = not threshold_failures
    status = "pass"
    if threshold_failures and mode == "strict":
        status = "threshold_fail"
    elif threshold_failures:
        status = "warning"

    case_result = BenchmarkCaseResult(
        scenario=context.spec.name,
        description=context.spec.description,
        device=device_name,
        mode=mode,
        ocean_split_shading=ocean_split_shading_mode(),
        status=status,
        strict_passed=strict_passed,
        runtime_errors=[],
        threshold_failures=threshold_failures,
        geometry=geometry,
        render=render,
        eval_timings=eval_timings,
        render_timings=render_timings,
        profile_entries=profile_entries,
        artifact_paths=build_case_artifact_paths(paths),
    )
    write_json(paths["case"], asdict(case_result))
    return case_result


def benchmark_case_with_error_capture(scenario_name,
                                      device_name,
                                      mode,
                                      outdir,
                                      samples,
                                      resolution,
                                      repeat_eval,
                                      repeat_render,
                                      keep_intermediates):
    paths = get_benchmark_case_paths(outdir, scenario_name, device_name)
    paths["case_dir"].mkdir(parents=True, exist_ok=True)
    try:
        return benchmark_case(
            scenario_name=scenario_name,
            device_name=device_name,
            mode=mode,
            outdir=outdir,
            samples=samples,
            resolution=resolution,
            repeat_eval=repeat_eval,
            repeat_render=repeat_render,
            keep_intermediates=keep_intermediates,
        )
    except Exception as exc:
        error_text = traceback.format_exc()
        write_text(paths["profile"], error_text)
        runtime_errors = [f"{type(exc).__name__}: {exc}"]
        case_result = BenchmarkCaseResult(
            scenario=scenario_name,
            description=CURATED_SCENARIOS.get(
                scenario_name,
                ScenarioSpec(scenario_name, "", ScenarioThresholds()),
            ).description,
            device=device_name,
            mode=mode,
            ocean_split_shading=ocean_split_shading_mode(),
            status="error",
            strict_passed=False,
            runtime_errors=runtime_errors,
            threshold_failures=[],
            artifact_paths=build_case_artifact_paths(paths),
        )
        write_json(paths["case"], asdict(case_result))
        print(error_text)
        return case_result


def resolve_scenarios(spec):
    if spec == "all":
        return list(CURATED_SCENARIOS.keys())
    scenario_names = [item.strip() for item in spec.split(",") if item.strip()]
    invalid = [name for name in scenario_names if name not in CURATED_SCENARIOS]
    if invalid:
        raise ValueError(f"Unknown benchmark scenarios: {invalid}. Choices: {sorted(CURATED_SCENARIOS)}")
    return scenario_names


def resolve_devices(spec):
    devices = [item.strip().upper() for item in spec.split(",") if item.strip()]
    if not devices:
        raise ValueError("At least one device must be requested")
    return devices


def blender_build_hash():
    build_hash = bpy.app.build_hash
    if isinstance(build_hash, bytes):
        return build_hash.decode("utf-8", "ignore")
    return str(build_hash)


def run_benchmark_suite(*,
                        outdir,
                        scenario_names,
                        device_names,
                        mode,
                        samples,
                        resolution,
                        repeat_eval,
                        repeat_render,
                        keep_intermediates):
    outdir = Path(outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)

    cases = []
    for scenario_name in scenario_names:
        for device_name in device_names:
            cases.append(benchmark_case_with_error_capture(
                scenario_name=scenario_name,
                device_name=device_name,
                mode=mode,
                outdir=outdir,
                samples=samples,
                resolution=resolution,
                repeat_eval=repeat_eval,
                repeat_render=repeat_render,
                keep_intermediates=keep_intermediates,
            ))

    runtime_error_count = sum(1 for case in cases if case.status == "error")
    threshold_failure_count = sum(1 for case in cases if case.threshold_failures)
    if runtime_error_count:
        overall_status = "error"
    elif mode == "strict" and threshold_failure_count:
        overall_status = "threshold_fail"
    elif threshold_failure_count:
        overall_status = "warning"
    else:
        overall_status = "pass"

    summary = BenchmarkRunSummary(
        blender_version=bpy.app.version_string,
        blender_build_hash=blender_build_hash(),
        hostname=socket.gethostname(),
        platform=platform.platform(),
        outdir=str(outdir),
        devices=device_names,
        scenarios=scenario_names,
        available_devices=get_available_cycles_devices(),
        mode=mode,
        ocean_split_shading=ocean_split_shading_mode(),
        samples=samples,
        resolution=resolution,
        repeat_eval=repeat_eval,
        repeat_render=repeat_render,
        overall_status=overall_status,
        case_count=len(cases),
        runtime_error_count=runtime_error_count,
        threshold_failure_count=threshold_failure_count,
        cases=cases,
    )
    write_json(outdir / "summary.json", asdict(summary))
    return summary


def benchmark_exit_code(summary):
    if summary.runtime_error_count:
        return 1
    if summary.mode == "strict" and summary.threshold_failure_count:
        return 1
    return 0
