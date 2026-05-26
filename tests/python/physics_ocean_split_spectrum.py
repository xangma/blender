# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import math
import os
import sys

import bpy
from mathutils import Vector

sys.path.append(os.path.dirname(os.path.realpath(__file__)))

from modules import ocean_camera_lod_metrics as ocean_metrics


RGB_MAE_TOL = ocean_metrics.RGB_MAE_TOL
RGB_MAX_TOL = ocean_metrics.RGB_MAX_TOL
REPROJ_MEAN_TOL = ocean_metrics.REPROJ_MEAN_TOL
REPROJ_MAX_TOL = ocean_metrics.REPROJ_MAX_TOL
DEPTH_MEAN_TOL = ocean_metrics.DEPTH_MEAN_TOL
DEPTH_MAX_TOL = ocean_metrics.DEPTH_MAX_TOL
NORMAL_MEAN_TOL_DEG = ocean_metrics.NORMAL_MEAN_TOL_DEG
NORMAL_MAX_TOL_DEG = ocean_metrics.NORMAL_MAX_TOL_DEG
POSITION_MEAN_TOL = ocean_metrics.POSITION_MEAN_TOL
POSITION_MAX_TOL = ocean_metrics.POSITION_MAX_TOL

clear_scene = ocean_metrics.clear_scene
look_at = ocean_metrics.look_at
make_ocean_object = ocean_metrics.make_ocean_object
make_camera_and_light = ocean_metrics.make_camera_and_light
set_world_flat = ocean_metrics.set_world_flat
set_world_sky_gradient = ocean_metrics.set_world_sky_gradient
enable_stereo_multiview = ocean_metrics.enable_stereo_multiview
evaluated_mesh_snapshot = ocean_metrics.evaluated_mesh_snapshot
evaluated_mesh_stats = ocean_metrics.evaluated_mesh_stats
dense_mesh_stats = ocean_metrics.dense_mesh_stats
assert_camera_lod_attributes = ocean_metrics.assert_camera_lod_attributes
assert_camera_lod_modifier_profile = ocean_metrics.assert_camera_lod_modifier_profile
camera_lod_reference_report = ocean_metrics.camera_lod_reference_report
camera_lod_level_histogram = ocean_metrics.camera_lod_level_histogram
camera_lod_topology_report = ocean_metrics.camera_lod_topology_report
camera_lod_visible_coverage_report = ocean_metrics.camera_lod_visible_coverage_report
render_rgb_difference_report = ocean_metrics.render_rgb_difference_report


def camera_center_ray_ocean_intersection(cam):
    bpy.context.view_layer.update()
    forward = cam.matrix_world.to_quaternion() @ Vector((0.0, 0.0, -1.0))
    if abs(forward.z) < 1.0e-8:
        raise AssertionError("Camera forward vector is parallel to the ocean plane")
    t = -cam.location.z / forward.z
    point = cam.location + (forward * t)
    return (float(point.x), float(point.y))


def camera_object_xy(obj, cam):
    bpy.context.view_layer.update()
    local = obj.matrix_world.inverted() @ cam.location
    return (float(local.x), float(local.y))


def camera_ocean_domain_anchor_xy(obj, cam):
    x, y = camera_object_xy(obj, cam)
    mod = obj.modifiers["Ocean"]
    half_extent = 0.5 * float(mod.size) * float(mod.spatial_size)
    return (
        max(-half_extent, min(half_extent, x)),
        max(-half_extent, min(half_extent, y)),
    )


def finest_level_min_distance_to_point(obj, point_xy, attr_name="ocean_camera_lod_level"):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        attr = mesh_eval.attributes.get(attr_name)
        assert attr is not None, f"Missing evaluated mesh attribute '{attr_name}'"
        levels = [int(round(item.value)) for item in attr.data]
        finest_level = min(levels)
        distances = [
            math.hypot(vertex.co.x - point_xy[0], vertex.co.y - point_xy[1])
            for vertex, level in zip(mesh_eval.vertices, levels)
            if level == finest_level
        ]
        assert distances, f"No evaluated vertices found for finest level {finest_level}"
        return min(distances)
    finally:
        obj_eval.to_mesh_clear()


def assert_ocean_camera_lod_metrics_module_contract():
    required_scenarios = {
        "calm_reference",
        "high_energy_dense_ceiling",
        "grazing_light_adversarial",
        "temporal_camera_move",
        "stereo_dataset_valid",
        "repeat_tiles",
    }
    assert required_scenarios.issubset(set(ocean_metrics.CURATED_SCENARIOS)), (
        "Shared ocean camera LOD metrics module must publish the curated benchmark scenarios"
    )


def assert_camera_lod_generate_tracks_camera():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(0.0, -35.0, 8.0),
        cam_target=(0.0, 0.0, 0.0),
        lens=50.0,
        sun_energy=0.0,
    )

    obj = make_ocean_object(
        name="OceanCameraLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
    )
    assert_camera_lod_attributes(obj)

    mod = obj.modifiers["Ocean"]
    base_cell = (mod.size * mod.spatial_size) / float(mod.resolution * mod.resolution)
    anchor_distance = finest_level_min_distance_to_point(
        obj, camera_ocean_domain_anchor_xy(obj, cam), attr_name="ocean_camera_lod_split_level"
    )
    assert anchor_distance <= base_cell, (
        "Camera LOD should force full-spectrum split level 0 at the camera's in-domain XY anchor; "
        f"got nearest split-level-0 distance {anchor_distance:.3f}m, base_cell={base_cell:.3f}m"
    )

    snap0 = evaluated_mesh_snapshot(obj)

    cam.location.x = 0.4 * base_cell
    look_at(cam, (0.4 * base_cell, 0.0, 0.0))
    snap_small_move = evaluated_mesh_snapshot(obj)
    assert snap_small_move[2] == snap0[2]
    small_move_anchor_distance = finest_level_min_distance_to_point(
        obj, camera_ocean_domain_anchor_xy(obj, cam), attr_name="ocean_camera_lod_split_level"
    )
    assert small_move_anchor_distance <= base_cell, (
        "Camera LOD should keep split level 0 within one dense cell of the moved camera anchor; "
        f"got {small_move_anchor_distance:.3f}m, base_cell={base_cell:.3f}m"
    )

    cam.location.x = 6.0 * base_cell
    look_at(cam, (6.0 * base_cell, 0.0, 0.0))
    snap_large_move = evaluated_mesh_snapshot(obj)
    assert snap_large_move[2] == snap0[2]
    large_move_anchor_distance = finest_level_min_distance_to_point(
        obj, camera_ocean_domain_anchor_xy(obj, cam), attr_name="ocean_camera_lod_split_level"
    )
    assert large_move_anchor_distance <= base_cell, (
        "Camera LOD should re-anchor split level 0 after a larger camera move; "
        f"got {large_move_anchor_distance:.3f}m, base_cell={base_cell:.3f}m"
    )


def assert_camera_lod_reference_validation():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -26.0, 6.0),
        cam_target=(0.0, 24.0, 0.0),
        lens=40.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanReferenceLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
    )
    assert_camera_lod_attributes(obj)

    cam = bpy.context.scene.camera
    report = camera_lod_reference_report(obj, cam)
    assert report["lod_verts"] < report["dense_verts"], "Camera LOD should reduce geometry on this case"
    assert report["geometry_reduction"] > 0.10, "Geometry reduction should be material on the reference case"
    assert report["visible_sample_count"] > 0, "Need visible samples for reprojection/depth validation"
    assert report["mean_position_error"] < POSITION_MEAN_TOL
    assert report["max_position_error"] < POSITION_MAX_TOL
    assert report["mean_reprojection_error"] < REPROJ_MEAN_TOL
    assert report["max_reprojection_error"] < REPROJ_MAX_TOL
    assert report["mean_depth_error"] < DEPTH_MEAN_TOL
    assert report["max_depth_error"] < DEPTH_MAX_TOL
    assert report["normal_semantics"] == "GEOMETRIC_NORMAL"
    assert not report["apparent_normal_available"]
    assert report["mean_geometric_normal_error_deg"] < NORMAL_MEAN_TOL_DEG
    assert report["max_geometric_normal_error_deg"] < NORMAL_MAX_TOL_DEG


def assert_camera_lod_reference_validation_hits_simulation_ceiling():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -26.0, 6.0),
        cam_target=(0.0, 24.0, 0.0),
        lens=40.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanReferenceDenseFloor",
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
    )
    assert_camera_lod_attributes(obj)

    cam = bpy.context.scene.camera
    report = camera_lod_reference_report(obj, cam)
    assert report["lod_verts"] <= report["dense_verts"], (
        "High-energy strict validation should never exceed dense reference geometry"
    )
    assert report["geometry_reduction"] < 0.20, (
        "High-energy strict validation should stay near the dense reference ceiling; "
        f"got reduction {report['geometry_reduction']:.3f}"
    )
    assert report["visible_sample_count"] > 0
    assert report["mean_position_error"] < 0.01
    assert report["max_position_error"] < 0.25
    assert report["max_reprojection_error"] < REPROJ_MAX_TOL
    assert report["max_depth_error"] < DEPTH_MAX_TOL
    assert report["mean_geometric_normal_error_deg"] < NORMAL_MEAN_TOL_DEG


def assert_camera_lod_temporal_reference_validation():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(0.0, -24.0, 5.0),
        cam_target=(0.0, 20.0, 0.0),
        lens=35.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanTemporalLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
    )
    assert_camera_lod_attributes(obj)

    report_a = camera_lod_reference_report(obj, cam)

    mod = obj.modifiers["Ocean"]
    base_cell = (mod.size * mod.spatial_size) / float(mod.resolution * mod.resolution)
    cam.location.x += 5.0 * base_cell
    look_at(cam, (5.0 * base_cell, 20.0, 0.0))
    report_b = camera_lod_reference_report(obj, cam)

    assert report_a["visible_sample_count"] > 0
    assert report_b["visible_sample_count"] > 0
    assert report_a["geometry_reduction"] > 0.10
    assert report_b["geometry_reduction"] > 0.10
    assert max(report_a["max_reprojection_error"], report_b["max_reprojection_error"]) < REPROJ_MAX_TOL
    assert max(report_a["max_depth_error"], report_b["max_depth_error"]) < DEPTH_MAX_TOL
    assert abs(report_b["mean_reprojection_error"] - report_a["mean_reprojection_error"]) < 0.75


def assert_camera_lod_strict_mode_adversarial_validation():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(0.0, -58.0, 3.0),
        cam_target=(0.0, 180.0, 0.0),
        lens=55.0,
        sun_rotation=(0.18, 0.0, 0.75),
        sun_energy=2.0,
    )
    obj = make_ocean_object(
        name="OceanStrictAdversarial",
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

    camera_report_a = camera_lod_reference_report(obj, cam)
    camera_rgb_report = render_rgb_difference_report(obj, samples=4, resolution=64)

    mod = obj.modifiers["Ocean"]
    mod.lod_validation_mode = "GEOMETRY_STRICT"
    bpy.context.view_layer.update()
    strict_report_a = camera_lod_reference_report(obj, cam)
    strict_rgb_report = render_rgb_difference_report(obj, samples=4, resolution=64)

    base_cell = (mod.size * mod.spatial_size) / float(mod.resolution * mod.resolution)
    cam.location.x += 6.0 * base_cell
    cam.location.z += 0.5
    look_at(cam, (6.0 * base_cell, 180.0, 0.0))
    strict_report_b = camera_lod_reference_report(obj, cam)

    mod.lod_validation_mode = "CAMERA_OBSERVABLE"
    bpy.context.view_layer.update()
    camera_report_b = camera_lod_reference_report(obj, cam)

    assert camera_report_a["visible_sample_count"] > 0
    assert strict_report_a["visible_sample_count"] > 0
    assert camera_report_b["visible_sample_count"] > 0
    assert strict_report_b["visible_sample_count"] > 0
    assert strict_report_a["lod_verts"] >= camera_report_a["lod_verts"]
    assert strict_report_b["lod_verts"] >= camera_report_b["lod_verts"]
    assert strict_report_a["geometry_reduction"] <= camera_report_a["geometry_reduction"] + 1.0e-6
    assert strict_report_b["geometry_reduction"] <= camera_report_b["geometry_reduction"] + 1.0e-6
    assert strict_report_a["max_position_error"] <= camera_report_a["max_position_error"] + 1.0e-6
    assert strict_report_b["max_position_error"] <= camera_report_b["max_position_error"] + 1.0e-6
    assert max(strict_report_a["max_reprojection_error"], strict_report_b["max_reprojection_error"]) < REPROJ_MAX_TOL
    assert max(strict_report_a["max_depth_error"], strict_report_b["max_depth_error"]) < DEPTH_MAX_TOL
    assert max(
        strict_report_a["max_geometric_normal_error_deg"],
        strict_report_b["max_geometric_normal_error_deg"],
    ) < NORMAL_MAX_TOL_DEG
    assert strict_rgb_report["max_absolute_error"] <= camera_rgb_report["max_absolute_error"] + 0.05


def assert_camera_lod_general_render_validates_relevant_leaf_error():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -36.0, 7.0),
        cam_target=(0.0, 60.0, 0.0),
        lens=35.0,
        sun_rotation=(0.2, 0.0, 0.8),
        sun_energy=1.0,
    )
    obj = make_ocean_object(
        name="OceanGeneralRenderObservableValidation",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        lod_usage_mode="GENERAL_RENDER",
        lod_validation_mode="CAMERA_OBSERVABLE",
        lod_pixel_error=8.0,
        time_value=1.0,
        choppiness=0.8,
        wind_velocity=1.5,
        roughness=0.01,
    )

    profile_log, profile_entries = ocean_metrics.capture_camera_lod_profile(obj)
    assert_camera_lod_attributes(obj)
    settings_entries = [
        entry
        for entry in profile_entries
        if entry.get("stage") == "settings" and entry.get("mode") == "CAMERA_OBSERVABLE"
    ]
    assert settings_entries, f"Expected Camera LOD settings profile entry, got:\n{profile_log}"
    assert any(entry.get("selection") == "sampled" for entry in settings_entries), (
        "General-render camera LOD should validate visible leaves against dense same-state geometry; "
        f"profile was:\n{profile_log}"
    )
    assert any(int(entry.get("region_calls", 0)) > 0 for entry in settings_entries), (
        "General-render camera LOD must validate relevant leaves against dense same-state geometry; "
        f"profile was:\n{profile_log}"
    )
    assert any(int(entry.get("sample_evals", 0)) > 0 for entry in settings_entries), (
        "General-render camera LOD validation must sample dense reference error; "
        f"profile was:\n{profile_log}"
    )


def assert_camera_lod_rgb_validation():
    clear_scene()
    set_world_sky_gradient(strength=1.25)
    make_camera_and_light(
        cam_location=(0.0, -36.0, 7.0),
        cam_target=(0.0, 60.0, 0.0),
        lens=35.0,
        sun_rotation=(0.2, 0.0, 0.8),
        sun_energy=1.0,
    )
    obj = make_ocean_object(
        name="OceanRenderLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.8,
        wind_velocity=1.5,
        roughness=0.01,
    )
    assert_camera_lod_attributes(obj)

    geometry_report = camera_lod_reference_report(obj, bpy.context.scene.camera)
    assert geometry_report["geometry_reduction"] > 0.10, (
        "Reflective sky validation must exercise a materially reduced camera LOD mesh"
    )

    report = render_rgb_difference_report(obj, samples=4, resolution=64)
    assert report["dense_luminance_range"] > 0.05, (
        "Reflective sky validation must render non-flat lighting so mirror-like far-field errors are visible"
    )
    assert report["max_dense_luminance_gradient"] > 0.005, (
        "Reflective sky validation must have enough image contrast to catch blocky far-field reflections"
    )
    assert report["mean_absolute_error"] < RGB_MAE_TOL
    assert report["max_absolute_error"] < RGB_MAX_TOL


def assert_camera_lod_conservative_visibility_coverage():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(0.0, -40.0, 3.0),
        cam_target=(0.0, 40.0, 0.0),
        lens=35.0,
        sun_energy=0.0,
    )
    bpy.context.scene.render.resolution_x = 256
    bpy.context.scene.render.resolution_y = 128
    cam.data.clip_start = 0.1
    cam.data.clip_end = 20000.0

    obj = make_ocean_object(
        name="OceanConservativeVisibilityLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
        roughness=0.02,
    )
    assert_camera_lod_attributes(obj)

    report = camera_lod_visible_coverage_report(obj, cam)
    assert report["visible_sample_count"] > 0, (
        "Conservative visibility validation needs visible dense reference samples"
    )
    assert report["lod_verts"] < report["dense_verts"], (
        "Conservative visibility validation must still exercise a reduced LOD mesh"
    )
    assert report["missing_visible_sample_count"] == 0, (
        "Camera LOD quadtree must keep geometry covering every visible dense reference sample; "
        f"report={report}"
    )


def assert_camera_lod_repeat_tiles_cover_visible_domain():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(58.0, -28.0, 7.0),
        cam_target=(62.0, 24.0, 0.0),
        lens=38.0,
        sun_energy=0.0,
    )
    bpy.context.scene.render.resolution_x = 256
    bpy.context.scene.render.resolution_y = 128
    cam.data.clip_start = 0.1
    cam.data.clip_end = 20000.0

    obj = make_ocean_object(
        name="OceanRepeatVisibilityLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        repeat_x=2,
        repeat_y=1,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
        roughness=0.02,
    )
    assert_camera_lod_attributes(obj)

    mod = obj.modifiers["Ocean"]
    base_tile_max_x = 0.5 * float(mod.spatial_size)
    report = camera_lod_visible_coverage_report(obj, cam)
    assert report["visible_sample_count"] > 0, (
        "Repeat visibility validation needs visible dense reference samples"
    )
    assert report["visible_reference_max_x"] > base_tile_max_x, (
        "Repeat visibility validation must exercise the second repeated X tile; "
        f"report={report}"
    )
    assert report["lod_verts"] < report["dense_verts"], (
        "Repeated-domain camera LOD should remain adaptive rather than falling back to dense"
    )
    assert report["missing_visible_sample_count"] == 0, (
        "Repeated-domain camera LOD must cover visible dense samples outside the base tile; "
        f"report={report}"
    )


def assert_camera_lod_budget_invariant():
    scenarios = (
        ((0.0, -35.0, 8.0), (0.0, 0.0, 0.0), 50.0),
        ((12.0, -22.0, 6.0), (8.0, 10.0, 0.0), 35.0),
        ((0.0, -14.0, 2.5), (0.0, 18.0, 0.0), 20.0),
    )

    for index, (cam_location, cam_target, lens) in enumerate(scenarios):
        clear_scene()
        make_camera_and_light(
            cam_location=cam_location,
            cam_target=cam_target,
            lens=lens,
            sun_energy=0.0,
        )
        obj = make_ocean_object(
            name=f"OceanBudget{index}",
            geometry_mode="GENERATE",
            resolution=6,
            spatial_size=64,
            size=1.0,
            camera_lod=True,
            lod_levels=5,
        )

        lod_verts, _, lod_unused, _ = evaluated_mesh_stats(obj)
        assert lod_unused == 0, "Camera LOD mesh should not contain unused vertices"

        mod = obj.modifiers["Ocean"]
        mod.use_camera_lod = False
        dense_verts, _, dense_unused, _ = evaluated_mesh_stats(obj)
        assert dense_unused == 0, "Dense reference mesh should not contain unused vertices"
        assert lod_verts <= dense_verts, (
            f"Camera LOD should not exceed dense reference vertices "
            f"(lod={lod_verts}, dense={dense_verts})"
        )


def assert_camera_lod_large_footprint_budget_regression():
    clear_scene()
    make_camera_and_light(
        cam_location=(360.0, -360.0, 30.0),
        cam_target=(360.0, 240.0, 0.0),
        lens=35.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanLargeFootprintBudget",
        geometry_mode="GENERATE",
        resolution=16,
        spatial_size=64,
        size=16.0,
        camera_lod=True,
        lod_levels=5,
        lod_pixel_error=0.5,
    )

    lod_verts, _, lod_unused, _ = evaluated_mesh_stats(obj)
    dense_verts, _, dense_unused, _ = dense_mesh_stats(obj)

    assert lod_unused == 0, "Camera LOD large-footprint mesh should not contain unused vertices"
    assert dense_unused == 0, "Dense large-footprint reference mesh should not contain unused vertices"
    assert lod_verts < dense_verts * 0.45, (
        f"Camera LOD should remain a meaningful acceleration path for large footprints "
        f"(lod={lod_verts}, dense={dense_verts})"
    )


def assert_stereo_dataset_reference_validation():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(0.0, -26.0, 6.0),
        cam_target=(0.0, 24.0, 0.0),
        lens=40.0,
        sun_energy=0.0,
    )
    enable_stereo_multiview(cam, interocular_distance=4.0, convergence_distance=50.0)
    obj = make_ocean_object(
        name="OceanStereoDataset",
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
    )
    assert_camera_lod_attributes(obj)

    mod = obj.modifiers["Ocean"]
    assert getattr(mod, "lod_usage_mode", "GENERAL_RENDER") == "STEREO_DATASET"

    cam = bpy.context.scene.camera
    report = camera_lod_reference_report(obj, cam)
    assert report["lod_verts"] < report["dense_verts"], "Stereo dataset mode should still reduce geometry"
    assert report["visible_sample_count"] > 0
    assert report["mean_position_error"] < POSITION_MEAN_TOL
    assert report["max_position_error"] < POSITION_MAX_TOL
    assert report["mean_reprojection_error"] < REPROJ_MEAN_TOL
    assert report["max_reprojection_error"] < REPROJ_MAX_TOL
    assert report["mean_depth_error"] < DEPTH_MEAN_TOL
    assert report["max_depth_error"] < DEPTH_MAX_TOL
    assert report["mean_geometric_normal_error_deg"] < NORMAL_MEAN_TOL_DEG
    assert report["max_geometric_normal_error_deg"] < NORMAL_MAX_TOL_DEG


def assert_camera_lod_foam_and_spray_use_camera_lod_geometry():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -58.0, 3.0),
        cam_target=(0.0, 180.0, 0.0),
        lens=55.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanCameraLODFoamSpray",
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
    mod.use_spray = True
    mod.spray_layer_name = "spray"
    bpy.context.view_layer.update()
    assert_camera_lod_attributes(obj)

    lod_verts, lod_faces, lod_unused, lod_attrs = evaluated_mesh_stats(obj)
    dense_verts, dense_faces, dense_unused, _dense_attrs = dense_mesh_stats(obj)

    assert "ocean_camera_lod_level" in lod_attrs, (
        "Cycles foam/spray camera LOD should keep adaptive geometry; full-spectrum foam/spray "
        "is sampled by the shader instead of requiring dense geometry"
    )
    assert {"foam", "spray"}.issubset(lod_attrs), "Foam/spray layer names should still be exported"
    assert lod_verts < dense_verts and lod_faces < dense_faces, (
        "Foam/spray camera LOD should remain adaptive; "
        f"lod={(lod_verts, lod_faces, lod_unused)}, dense={(dense_verts, dense_faces, dense_unused)}"
    )

    mat = obj.data.materials[0]
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    for node in list(nodes):
        if node.name != "Material Output":
            nodes.remove(node)
    output = nodes["Material Output"]
    attr = nodes.new("ShaderNodeAttribute")
    attr.attribute_name = "foam"
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    links.new(attr.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])

    report = render_rgb_difference_report(obj, samples=4, resolution=64)
    assert report["mean_absolute_error"] < RGB_MAE_TOL, report
    assert report["p95_absolute_error"] < 0.30, report
    assert report["max_absolute_error"] < 0.50, report


def assert_stereo_dataset_wide_footprint_uses_multiple_levels():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(1200.0, -1200.0, 30.0),
        cam_target=(1163.3, -1163.3, 0.0),
        lens=18.0,
        sun_energy=0.0,
    )
    bpy.context.scene.render.resolution_x = 960
    bpy.context.scene.render.resolution_y = 540
    cam.data.clip_start = 0.1
    cam.data.clip_end = 20_000_000.0
    enable_stereo_multiview(cam, interocular_distance=0.08, convergence_distance=36.0)

    obj = make_ocean_object(
        name="OceanStereoDatasetWideFootprint",
        geometry_mode="GENERATE",
        resolution=64,
        spatial_size=3000,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        lod_usage_mode="STEREO_DATASET",
        lod_validation_mode="GEOMETRY_STRICT",
        time_value=0.417,
        choppiness=1.6,
        wind_velocity=10.6,
        roughness=0.035,
        wave_scale=1.2,
        smallest_wave=0.02,
        wave_direction=0.0,
        wave_alignment=0.0,
        seed=12345,
    )
    assert_camera_lod_attributes(obj)

    lod_verts, _lod_faces, _lod_loose, _lod_attrs = evaluated_mesh_stats(obj)
    dense_verts, _dense_faces, _dense_loose, _dense_attrs = dense_mesh_stats(obj)
    split_level_histogram = camera_lod_level_histogram(
        obj, attr_name="ocean_camera_lod_split_level"
    )
    local_level_histogram = camera_lod_level_histogram(obj)

    assert split_level_histogram, "Expected evaluated split-level metadata on the wide-footprint case"
    assert min(split_level_histogram) == 0, (
        "Camera-anchored LOD must keep full-spectrum split level 0 at the camera anchor; "
        f"got split-level histogram {split_level_histogram}"
    )
    assert len(local_level_histogram) > 1, (
        "Wide-footprint stereo dataset case should emit multiple evaluated LOD levels; "
        f"got local-level histogram {local_level_histogram} and split-level histogram {split_level_histogram}"
    )
    assert lod_verts < (dense_verts * 0.45), (
        "Wide-footprint stereo dataset case should materially reduce geometry; "
        f"got lod_verts={lod_verts}, dense_verts={dense_verts}"
    )

    topology_report = camera_lod_topology_report(obj)
    assert topology_report["max_face_split_spread"] <= 2, (
        "Camera LOD quadtree faces may include diagonal corner spread, but must stay bounded; "
        f"report={topology_report}"
    )
    assert topology_report["max_face_local_spread"] <= 2, (
        "Camera LOD local quadtree face-level spread may include diagonal corners, but must stay bounded; "
        f"report={topology_report}"
    )
    leaf_layout_report = ocean_metrics.camera_lod_leaf_layout_report(obj)
    assert leaf_layout_report["contract"] == "adaptive_leaf_v1", (
        "Camera LOD quadtree metadata must advertise the adaptive leaf layout contract; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["layout"] == "adaptive_leaf", (
        "Camera LOD quadtree metadata must identify the adaptive leaf layout; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["unique_leaf_count"] == leaf_layout_report["face_count"], (
        "Each emitted quadtree face must carry a unique leaf id for top-down debug rendering; "
        f"report={leaf_layout_report}"
    )
    assert len(leaf_layout_report["level_histogram"]) > 1, (
        "Adaptive leaf metadata should expose multiple quadtree LOD levels; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["max_cell_size"] > leaf_layout_report["min_cell_size"], (
        "Adaptive leaf metadata should expose real geometry reduction across different cell sizes; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["max_adjacent_split_spread"] <= 1, (
        "Face-domain quadtree metadata must preserve the 2:1 balancing invariant; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["max_adjacent_local_spread"] <= 1, (
        "Face-domain local quadtree metadata must not jump by more than one level across neighbors; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["nonmanifold_edge_count"] == 0, (
        "Adaptive quadtree wireframe metadata should not create non-manifold shared-edge records; "
        f"report={leaf_layout_report}"
    )


def assert_stereo_dataset_wide_footprint_finest_patch_tracks_camera_anchor():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(1200.0, -1200.0, 30.0),
        cam_target=(1163.3, -1163.3, 0.0),
        lens=18.0,
        sun_energy=0.0,
    )
    bpy.context.scene.render.resolution_x = 960
    bpy.context.scene.render.resolution_y = 540
    cam.data.clip_start = 0.1
    cam.data.clip_end = 20_000_000.0
    enable_stereo_multiview(cam, interocular_distance=0.08, convergence_distance=36.0)

    obj = make_ocean_object(
        name="OceanStereoDatasetWideFootprintSupport",
        geometry_mode="GENERATE",
        resolution=64,
        spatial_size=3000,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        lod_usage_mode="STEREO_DATASET",
        lod_validation_mode="GEOMETRY_STRICT",
        time_value=0.417,
        choppiness=1.6,
        wind_velocity=10.6,
        roughness=0.035,
        wave_scale=1.2,
        smallest_wave=0.02,
        wave_direction=0.0,
        wave_alignment=0.0,
        seed=12345,
    )
    assert_camera_lod_attributes(obj)

    camera_xy = camera_ocean_domain_anchor_xy(obj, cam)
    split_zero_distance = finest_level_min_distance_to_point(
        obj, camera_xy, attr_name="ocean_camera_lod_split_level"
    )
    mod = obj.modifiers["Ocean"]
    dense_cell_size = (mod.size * mod.spatial_size) / float(mod.resolution * mod.resolution)

    assert split_zero_distance <= dense_cell_size, (
        "The finest emitted stereo-dataset geometry should track the camera in-domain XY anchor, "
        f"not the center-ray ocean support point; got nearest split-level-0 distance "
        f"{split_zero_distance:.3f}m to camera anchor {camera_xy}, dense_cell={dense_cell_size:.3f}m"
    )


def assert_stereo_dataset_reacts_to_camera_parameter_changes():
    clear_scene()
    cam = make_camera_and_light(
        cam_location=(0.0, -32.0, 7.0),
        cam_target=(0.0, 32.0, 0.0),
        lens=55.0,
        sun_energy=0.0,
    )
    enable_stereo_multiview(cam, interocular_distance=0.08, convergence_distance=42.0)

    obj = make_ocean_object(
        name="OceanStereoDepsgraph",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=96,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        lod_usage_mode="STEREO_DATASET",
        time_value=1.0,
        choppiness=0.6,
        wind_velocity=8.0,
    )

    assert_camera_lod_modifier_profile(obj, "Initial stereo dataset mesh must evaluate camera LOD")

    cam.data.lens = 28.0
    assert_camera_lod_modifier_profile(obj, "Lens changes must invalidate stereo dataset camera LOD")

    cam.data.shift_x = 0.18
    assert_camera_lod_modifier_profile(obj, "Lens shift changes must invalidate stereo dataset camera LOD")

    cam.data.stereo.interocular_distance = 0.18
    assert_camera_lod_modifier_profile(
        obj, "Interocular distance must invalidate stereo dataset camera LOD")

    cam.data.stereo.convergence_distance = 12.0
    assert_camera_lod_modifier_profile(
        obj, "Convergence distance must invalidate stereo dataset camera LOD")

    bpy.context.scene.render.use_multiview = False
    assert_camera_lod_modifier_profile(
        obj, "Multiview enable state must invalidate stereo dataset camera LOD")


def assert_stereo_dataset_requires_true_stereo_pair():
    def assert_dense_fallback(message):
        invalid_verts, invalid_polys, invalid_unused, invalid_attrs = evaluated_mesh_stats(obj)
        dense_verts, dense_polys, dense_unused, _ = dense_mesh_stats(obj)
        assert (invalid_verts, invalid_polys, invalid_unused) == (
            dense_verts,
            dense_polys,
            dense_unused,
        ), message
        if "ocean_camera_lod_level" in invalid_attrs:
            level_histogram = camera_lod_level_histogram(obj)
            assert level_histogram == {0: invalid_verts}, (
                f"{message}; expected dense-equivalent split metadata, got {level_histogram}"
            )

    def build_invalid_case(case_name, configure_scene):
        clear_scene()
        cam = make_camera_and_light(
            cam_location=(0.0, -28.0, 6.0),
            cam_target=(0.0, 24.0, 0.0),
            lens=45.0,
            sun_energy=0.0,
        )
        enable_stereo_multiview(cam, interocular_distance=0.08, convergence_distance=36.0)
        configure_scene(bpy.context.scene)
        return make_ocean_object(
            name=case_name,
            geometry_mode="GENERATE",
            resolution=6,
            spatial_size=64,
            size=1.0,
            camera_lod=True,
            lod_levels=4,
            lod_usage_mode="STEREO_DATASET",
            time_value=1.0,
            choppiness=0.4,
            wind_velocity=10.0,
        )

    obj = build_invalid_case(
        "OceanStereoDatasetInvalidNoMultiview",
        lambda scene: setattr(scene.render, "use_multiview", False),
    )
    bpy.context.scene.render.use_multiview = False
    assert_dense_fallback("Stereo dataset mode must fail closed when multiview is disabled")

    def disable_right_eye(scene):
        for view in scene.render.views:
            view.use = view.name.lower() in {"left", "right"}
        for view in scene.render.views:
            if view.name.lower() == "right":
                view.use = False

    obj = build_invalid_case("OceanStereoDatasetInvalidRightOnly", disable_right_eye)
    assert_dense_fallback("Stereo dataset mode must fail closed when one stereo eye is disabled")
    bpy.context.scene.render.views_format = "MULTIVIEW"
    assert_dense_fallback("Stereo dataset mode must fail closed outside stereo-3D render mode")


def main():
    assert_ocean_camera_lod_metrics_module_contract()

    clear_scene()
    obj = make_ocean_object()
    make_camera_and_light()
    assert_camera_lod_attributes(obj)

    assert_camera_lod_generate_tracks_camera()
    assert_camera_lod_budget_invariant()
    assert_camera_lod_large_footprint_budget_regression()
    assert_camera_lod_reference_validation()
    assert_camera_lod_reference_validation_hits_simulation_ceiling()
    assert_camera_lod_temporal_reference_validation()
    assert_camera_lod_strict_mode_adversarial_validation()
    assert_camera_lod_general_render_validates_relevant_leaf_error()
    assert_camera_lod_conservative_visibility_coverage()
    assert_camera_lod_repeat_tiles_cover_visible_domain()
    assert_camera_lod_foam_and_spray_use_camera_lod_geometry()
    assert_stereo_dataset_reference_validation()
    assert_stereo_dataset_wide_footprint_uses_multiple_levels()
    assert_stereo_dataset_wide_footprint_finest_patch_tracks_camera_anchor()
    assert_stereo_dataset_reacts_to_camera_parameter_changes()
    assert_stereo_dataset_requires_true_stereo_pair()
    assert_camera_lod_rgb_validation()


if __name__ == "__main__":
    main()
