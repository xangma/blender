# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import math
import os
import sys
import tempfile
import time

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
REQUIRE_GPU_TIMING_ENV = "BLENDER_OCEAN_CAMERA_LOD_REQUIRE_GPU_TIMING"

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
render_attribute_difference_report = ocean_metrics.render_attribute_difference_report


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
    domain_size = max(float(mod.size) * float(mod.spatial_size), 1.0e-8)
    domain_min = -0.5 * domain_size
    domain_max_x = domain_min + max(1, int(getattr(mod, "repeat_x", 1))) * domain_size
    domain_max_y = domain_min + max(1, int(getattr(mod, "repeat_y", 1))) * domain_size
    return (
        max(domain_min, min(domain_max_x, x)),
        max(domain_min, min(domain_max_y, y)),
    )


def mesh_data_stats(mesh):
    used_vertices = set()
    for poly in mesh.polygons:
        used_vertices.update(poly.vertices)
    return (
        len(mesh.vertices),
        len(mesh.polygons),
        len(mesh.vertices) - len(used_vertices),
        {attr.name for attr in mesh.attributes},
    )


def assert_camera_lod_dense_fallback(obj, message):
    lod_verts, lod_polys, lod_unused, lod_attrs = evaluated_mesh_stats(obj)
    dense_verts, dense_polys, dense_unused, _ = dense_mesh_stats(obj)
    assert (lod_verts, lod_polys, lod_unused) == (
        dense_verts,
        dense_polys,
        dense_unused,
    ), (
        f"{message}; got {(lod_verts, lod_polys, lod_unused)}, "
        f"expected dense {(dense_verts, dense_polys, dense_unused)}"
    )
    if "ocean_camera_lod_level" in lod_attrs:
        level_histogram = camera_lod_level_histogram(obj)
        assert level_histogram == {0: lod_verts}, (
            f"{message}; expected dense-equivalent split metadata, got {level_histogram}"
        )


def assert_camera_lod_mesh_data_matches_dense(mesh, dense_stats, message):
    mesh_verts, mesh_polys, mesh_unused, mesh_attrs = mesh_data_stats(mesh)
    dense_verts, dense_polys, dense_unused, _ = dense_stats
    assert (mesh_verts, mesh_polys, mesh_unused) == (
        dense_verts,
        dense_polys,
        dense_unused,
    ), (
        f"{message}; got {(mesh_verts, mesh_polys, mesh_unused)}, "
        f"expected dense {(dense_verts, dense_polys, dense_unused)}"
    )
    assert "ocean_camera_lod_level" not in mesh_attrs, (
        f"{message}; baked mesh retained camera LOD attributes {mesh_attrs}"
    )


def dense_corner_tri_count(obj):
    mod = obj.modifiers["Ocean"]
    original = mod.use_camera_lod
    mod.use_camera_lod = False
    try:
        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        obj_eval = obj.evaluated_get(depsgraph)
        mesh_eval = obj_eval.to_mesh()
        try:
            mesh_eval.calc_loop_triangles()
            return len(mesh_eval.loop_triangles)
        finally:
            obj_eval.to_mesh_clear()
    finally:
        mod.use_camera_lod = original
        bpy.context.view_layer.update()


def count_obj_vertices_and_faces(filepath):
    verts = 0
    faces = 0
    with open(filepath, encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            if line.startswith("v "):
                verts += 1
            elif line.startswith("f "):
                faces += 1
    return verts, faces


def count_ply_vertices_and_faces(filepath):
    verts = None
    faces = None
    with open(filepath, encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            if line.startswith("element vertex "):
                verts = int(line.split()[-1])
            elif line.startswith("element face "):
                faces = int(line.split()[-1])
            elif line.strip() == "end_header":
                break
    return verts, faces


def count_ascii_stl_facets(filepath):
    with open(filepath, encoding="utf-8", errors="ignore") as handle:
        return sum(1 for line in handle if line.lstrip().startswith("facet normal"))


def select_active_object(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


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


def camera_lod_split_zero_stats(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        split_attr = mesh_eval.attributes.get("ocean_camera_lod_split_level")
        assert split_attr is not None, "Missing evaluated split-level metadata"
        distances = [
            math.hypot(vertex.co.x, vertex.co.y)
            for vertex, item in zip(mesh_eval.vertices, split_attr.data)
            if int(round(item.value)) == 0
        ]
        assert distances, "Expected at least one split-level-0 vertex"
        return {
            "count": len(distances),
            "min_radius": min(distances),
            "max_radius": max(distances),
        }
    finally:
        obj_eval.to_mesh_clear()


def assert_camera_lod_leaf_layout_balanced(obj, case_name):
    leaf_layout_report = ocean_metrics.camera_lod_leaf_layout_report(obj)
    assert leaf_layout_report["contract"] == "adaptive_leaf_v1", (
        f"{case_name} quadtree metadata must advertise the adaptive leaf layout contract; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["layout"] == "adaptive_leaf", (
        f"{case_name} quadtree metadata must identify the adaptive leaf layout; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["unique_leaf_count"] == leaf_layout_report["face_count"], (
        f"{case_name} must carry one unique leaf id per emitted quadtree face; "
        f"report={leaf_layout_report}"
    )
    assert len(leaf_layout_report["level_histogram"]) > 1, (
        f"{case_name} should expose multiple quadtree LOD levels; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["max_cell_size"] > leaf_layout_report["min_cell_size"], (
        f"{case_name} should expose real geometry reduction across different cell sizes; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["max_adjacent_split_spread"] <= 1, (
        f"{case_name} must preserve the face-domain 2:1 split-level invariant; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["max_adjacent_local_spread"] <= 1, (
        f"{case_name} must preserve the face-domain 2:1 local-level invariant; "
        f"report={leaf_layout_report}"
    )
    assert leaf_layout_report["nonmanifold_edge_count"] == 0, (
        f"{case_name} should not create non-manifold shared-edge records; "
        f"report={leaf_layout_report}"
    )
    return leaf_layout_report


def assert_ocean_camera_lod_metrics_module_contract():
    required_scenarios = {
        "calm_reference",
        "high_energy_dense_ceiling",
        "grazing_light_adversarial",
        "foam_attribute",
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


def assert_camera_lod_high_choppiness_high_wind_validation():
    clear_scene()
    set_world_sky_gradient(strength=1.25)
    cam = make_camera_and_light(
        cam_location=(0.0, -58.0, 3.0),
        cam_target=(0.0, 180.0, 0.0),
        lens=55.0,
        sun_rotation=(0.18, 0.0, 0.75),
        sun_energy=2.0,
    )
    obj = make_ocean_object(
        name="OceanHighChopWindLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        lod_validation_mode="GEOMETRY_STRICT",
        time_value=1.0,
        choppiness=2.2,
        wind_velocity=38.0,
        roughness=0.02,
    )
    assert_camera_lod_attributes(obj)

    geometry_report = camera_lod_reference_report(obj, cam)
    assert_camera_lod_geometry_report_matches_dense(
        geometry_report, "High-choppiness/high-wind geometry")
    assert geometry_report["max_position_error"] < 0.25, geometry_report
    assert geometry_report["geometry_reduction"] < 0.35, (
        "High-choppiness/high-wind strict validation should stay near the dense ceiling; "
        f"report={geometry_report}"
    )

    topology_report = camera_lod_topology_report(obj)
    assert topology_report["max_face_split_spread"] <= 1, (
        f"High-choppiness/high-wind topology should not span more than one split level per face; "
        f"report={topology_report}"
    )
    assert topology_report["max_face_local_spread"] <= 1, (
        f"High-choppiness/high-wind topology should not span more than one local level per face; "
        f"report={topology_report}"
    )
    assert topology_report["max_face_vertex_count"] <= 6, (
        f"High-choppiness/high-wind topology should only add balanced edge vertices; "
        f"report={topology_report}"
    )
    assert_camera_lod_leaf_layout_balanced(obj, "High-choppiness/high-wind")

    render_report = render_rgb_difference_report(obj, samples=2, resolution=48)
    assert_camera_lod_render_report_matches_dense(
        render_report, "High-choppiness/high-wind render")


def assert_camera_lod_full_spectrum_radius_behavior():

    def build_radius_case(name, radius):
        clear_scene()
        set_world_sky_gradient(strength=1.25)
        cam = make_camera_and_light(
            cam_location=(0.0, -35.0, 8.0),
            cam_target=(0.0, 0.0, 0.0),
            lens=50.0,
            sun_rotation=(0.18, 0.0, 0.75),
            sun_energy=2.0,
        )
        obj = make_ocean_object(
            name=name,
            geometry_mode="GENERATE",
            resolution=6,
            spatial_size=64,
            size=1.0,
            camera_lod=True,
            lod_levels=5,
            lod_validation_mode="CAMERA_OBSERVABLE",
            lod_camera_full_spectrum_radius=radius,
            time_value=1.0,
            choppiness=0.0,
            wind_velocity=1.0,
            roughness=0.02,
        )
        assert_camera_lod_attributes(obj)
        geometry_report = camera_lod_reference_report(obj, cam)
        assert_camera_lod_geometry_report_matches_dense(
            geometry_report, f"{name} full-spectrum radius")
        render_report = render_rgb_difference_report(obj, samples=2, resolution=48)
        assert_camera_lod_render_report_matches_dense(render_report, f"{name} render")
        return geometry_report, camera_lod_split_zero_stats(obj)

    default_report, default_stats = build_radius_case("OceanDefaultFullSpectrumRadiusLOD", 0.0)
    expanded_report, expanded_stats = build_radius_case("OceanExpandedFullSpectrumRadiusLOD", 12.0)

    assert expanded_stats["count"] > default_stats["count"], (
        "Increasing the full-spectrum radius should expand the split-level-0 patch; "
        f"default={default_stats}, expanded={expanded_stats}"
    )
    assert expanded_report["lod_verts"] > default_report["lod_verts"], (
        "Increasing the full-spectrum radius should keep more LOD vertices in the finest patch; "
        f"default={default_report}, expanded={expanded_report}"
    )
    assert expanded_report["geometry_reduction"] < default_report["geometry_reduction"], (
        "Increasing the full-spectrum radius should reduce less geometry near the camera anchor; "
        f"default={default_report}, expanded={expanded_report}"
    )


def assert_camera_lod_transition_and_balancing_validation():
    clear_scene()
    set_world_sky_gradient(strength=1.25)
    cam = make_camera_and_light(
        cam_location=(0.0, -42.0, 7.0),
        cam_target=(0.0, 70.0, 0.0),
        lens=32.0,
        sun_rotation=(0.18, 0.0, 0.75),
        sun_energy=2.0,
    )
    obj = make_ocean_object(
        name="OceanLODTransitionBalance",
        geometry_mode="GENERATE",
        resolution=7,
        spatial_size=96,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        lod_validation_mode="CAMERA_OBSERVABLE",
        time_value=1.0,
        choppiness=0.8,
        wind_velocity=12.0,
        roughness=0.02,
    )
    assert_camera_lod_attributes(obj)

    geometry_report = camera_lod_reference_report(obj, cam)
    assert_camera_lod_geometry_report_matches_dense(
        geometry_report, "LOD transition geometry", min_geometry_reduction=0.20)
    assert geometry_report["visible_sample_count"] > 1000, (
        f"LOD transition validation should cover a broad visible sample set; "
        f"report={geometry_report}"
    )

    topology_report = camera_lod_topology_report(obj)
    assert len(topology_report["split_level_histogram"]) > 1, (
        f"LOD transition validation should contain multiple split levels; "
        f"report={topology_report}"
    )
    assert topology_report["max_face_split_spread"] <= 1, (
        f"LOD transition faces should not span more than one split level; "
        f"report={topology_report}"
    )
    assert topology_report["max_face_local_spread"] <= 1, (
        f"LOD transition faces should not span more than one local level; "
        f"report={topology_report}"
    )
    assert topology_report["max_face_vertex_count"] <= 6, (
        f"LOD transition topology should only add balanced edge vertices; "
        f"report={topology_report}"
    )
    assert_camera_lod_leaf_layout_balanced(obj, "LOD transition")

    render_report = render_rgb_difference_report(obj, samples=2, resolution=48)
    assert_camera_lod_render_report_matches_dense(render_report, "LOD transition render")


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


def assert_camera_lod_geometry_report_matches_dense(report, case_name, min_geometry_reduction=0.10):
    assert report["visible_sample_count"] > 0, (
        f"{case_name} must contain visible dense samples; report={report}"
    )
    assert report["lod_verts"] < report["dense_verts"], (
        f"{case_name} must exercise reduced camera LOD geometry; report={report}"
    )
    assert report["geometry_reduction"] > min_geometry_reduction, (
        f"{case_name} must keep a material geometry reduction; report={report}"
    )
    assert report["mean_position_error"] < POSITION_MEAN_TOL, report
    assert report["max_position_error"] < POSITION_MAX_TOL, report
    assert report["mean_reprojection_error"] < REPROJ_MEAN_TOL, report
    assert report["max_reprojection_error"] < REPROJ_MAX_TOL, report
    assert report["mean_depth_error"] < DEPTH_MEAN_TOL, report
    assert report["max_depth_error"] < DEPTH_MAX_TOL, report
    assert report["mean_geometric_normal_error_deg"] < NORMAL_MEAN_TOL_DEG, report
    assert report["max_geometric_normal_error_deg"] < NORMAL_MAX_TOL_DEG, report


def assert_camera_lod_render_report_matches_dense(report, case_name):
    assert report["dense_luminance_range"] > 0.05, (
        f"{case_name} must render non-flat lighting; report={report}"
    )
    assert report["max_dense_luminance_gradient"] > 0.005, (
        f"{case_name} must contain enough dense contrast to catch visible LOD shifts; "
        f"report={report}"
    )
    assert report["mean_absolute_error"] < RGB_MAE_TOL, report
    assert report["p95_absolute_error"] < 0.10, report
    assert report["max_absolute_error"] < RGB_MAX_TOL, report


def ocean_mesh_height_signature(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        heights = [v.co.z for v in mesh_eval.vertices]
        assert heights, "Ocean mesh must have vertices"
        assert all(math.isfinite(height) for height in heights), "Ocean heights must be finite"
        return {
            "vertex_count": len(heights),
            "max_abs_height": max(abs(height) for height in heights),
            "mean_abs_height": sum(abs(height) for height in heights) / len(heights),
            "checksum": round(sum(round(height, 6) * (i + 1) for i, height in enumerate(heights)), 6),
        }
    finally:
        obj_eval.to_mesh_clear()


def assert_realsea_spectrum_options_are_valid():
    clear_scene()
    obj = make_ocean_object(
        name="OceanRealseaSpectrumSmoke",
        geometry_mode="GENERATE",
        resolution=5,
        spatial_size=64,
        wind_velocity=14.0,
        wave_alignment=0.25,
        smallest_wave=0.02,
        time_value=1.2,
        camera_lod=False,
    )
    mod = obj.modifiers["Ocean"]

    checksums = set()
    for spectrum in ("REALSEA_PM", "REALSEA_JONSWAP"):
        for use_spread in (False, True):
            mod.spectrum = spectrum
            mod.realsea_fmin = 0.05
            mod.realsea_fmax = 1.0
            mod.realsea_use_spread = use_spread
            mod.realsea_spread = 1.0
            report = ocean_mesh_height_signature(obj)
            assert report["vertex_count"] > 0, report
            assert report["max_abs_height"] > 1.0e-6, report
            assert report["mean_abs_height"] > 1.0e-7, report
            checksums.add(report["checksum"])

    assert len(checksums) > 1, "Realsea spectrum options should produce distinguishable surfaces"


def render_scene_rgb_difference_report(objects, samples=2, resolution=48, device="CPU"):
    modifiers = [obj.modifiers["Ocean"] for obj in objects]
    original = [mod.use_camera_lod for mod in modifiers]

    with tempfile.TemporaryDirectory(prefix="ocean_camera_lod_scene_rgb_") as tempdir:
        lod_path = os.path.join(tempdir, "lod.png")
        dense_path = os.path.join(tempdir, "dense.png")
        try:
            for mod in modifiers:
                mod.use_camera_lod = True
            bpy.context.view_layer.update()
            start = time.perf_counter()
            ocean_metrics.render_with_cycles(
                lod_path, samples=samples, resolution=resolution, device=device)
            lod_render_s = time.perf_counter() - start

            for mod in modifiers:
                mod.use_camera_lod = False
            bpy.context.view_layer.update()
            start = time.perf_counter()
            ocean_metrics.render_with_cycles(
                dense_path, samples=samples, resolution=resolution, device=device)
            dense_render_s = time.perf_counter() - start
        finally:
            for mod, use_camera_lod in zip(modifiers, original):
                mod.use_camera_lod = use_camera_lod
            bpy.context.view_layer.update()

        width, height, lod_rgb = ocean_metrics.materialize_render_output(
            lod_path, os.path.join(tempdir, "lod_final.png"))
        dense_width, dense_height, dense_rgb = ocean_metrics.materialize_render_output(
            dense_path, os.path.join(tempdir, "dense_final.png"))
        assert (width, height) == (dense_width, dense_height)

        render_report, _rgba_abs, _rgba_gradient = ocean_metrics.compute_render_report(
            lod_rgb, dense_rgb, width, height)

    report_dict_value = ocean_metrics.render_report_dict(render_report)
    report_dict_value["device"] = device
    report_dict_value["samples"] = samples
    report_dict_value["resolution"] = resolution
    report_dict_value["object_count"] = len(objects)
    report_dict_value["lod_render_s"] = lod_render_s
    report_dict_value["dense_render_s"] = dense_render_s
    print("Camera LOD scene RGB report:", report_dict_value)
    return report_dict_value


def render_timing_stats_dict(timings):
    return {
        name: ocean_metrics.stats_dict(stats)
        for name, stats in timings.items()
    }


def render_rgb_difference_report_with_timings(obj, samples=1, resolution=32, device="CPU"):
    with tempfile.TemporaryDirectory(prefix="ocean_camera_lod_rgb_timing_") as tempdir:
        render_report, timings = ocean_metrics.benchmark_render_case(
            obj,
            tempdir,
            device=device,
            samples=samples,
            resolution=resolution,
            repeat_render=1,
            keep_intermediates=False,
        )

    report_dict_value = ocean_metrics.render_report_dict(render_report)
    report_dict_value["timings"] = render_timing_stats_dict(timings)
    print("Camera LOD RGB timing report:", report_dict_value)
    return report_dict_value


def profiled_render_rgb_difference_report(obj, samples=1, resolution=32, device="CPU"):
    profile_env = "BLENDER_OCEAN_CAMERA_LOD_PROFILE"
    previous = os.environ.get(profile_env)
    result = {}

    def run_render():
        result["report"] = render_rgb_difference_report_with_timings(
            obj, samples=samples, resolution=resolution, device=device)

    try:
        os.environ[profile_env] = "1"
        profile_log = ocean_metrics.capture_process_stdout(run_render)
    finally:
        if previous is None:
            os.environ.pop(profile_env, None)
        else:
            os.environ[profile_env] = previous

    result["profile_log"] = profile_log
    result["profile_entries"] = ocean_metrics.parse_ocean_camera_lod_profile(profile_log)
    print("Camera LOD render profile entries:", result["profile_entries"])
    return result


def ocean_profile_entries(entries, stage, mode=None, object_name=None):
    filtered = [entry for entry in entries if entry.get("stage") == stage]
    if mode is not None:
        filtered = [entry for entry in filtered if entry.get("mode") == mode]
    if object_name is not None:
        filtered = [entry for entry in filtered if entry.get("object") == object_name]
    return filtered


def assert_render_timing_pair_recorded(report, case_name):
    for key in ("lod_render_s", "dense_render_s"):
        stats = report["timings"][key]
        assert stats["count"] == 1 and stats["max"] > 0.0, (
            f"{case_name} must record a positive {key} timing; report={report}"
        )


def enable_centered_motion_blur(scene):
    scene.frame_start = 9
    scene.frame_end = 11
    scene.render.use_motion_blur = True
    scene.render.motion_blur_shutter = 1.0
    if hasattr(scene.render, "motion_blur_position"):
        scene.render.motion_blur_position = "CENTER"


def camera_lod_frame_topologies(obj, frames=(9, 10, 11)):
    scene = bpy.context.scene
    topologies = []
    for frame in frames:
        scene.frame_set(frame)
        topologies.append((frame, evaluated_mesh_stats(obj)[:3]))
    return topologies


def assert_motion_blur_topology_changes(topologies, case_name):
    unique_topologies = {tuple(stats) for _frame, stats in topologies}
    assert len(unique_topologies) > 1, (
        f"{case_name} must exercise changing endpoint LOD topology; "
        f"topologies={topologies}"
    )


def assert_stereo_dataset_rgb_validation():
    clear_scene()
    set_world_sky_gradient(strength=1.25)
    cam = make_camera_and_light(
        cam_location=(0.0, -30.0, 6.0),
        cam_target=(0.0, 32.0, 0.0),
        lens=38.0,
        sun_rotation=(0.2, 0.0, 0.8),
        sun_energy=1.0,
    )
    enable_stereo_multiview(cam, interocular_distance=0.18, convergence_distance=36.0)
    obj = make_ocean_object(
        name="OceanStereoRGBLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=96,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        lod_usage_mode="STEREO_DATASET",
        time_value=1.0,
        choppiness=0.45,
        wind_velocity=7.0,
        roughness=0.02,
    )
    assert_camera_lod_attributes(obj)

    geometry_report = camera_lod_reference_report(obj, cam)
    assert_camera_lod_geometry_report_matches_dense(geometry_report, "Stereo multiview render")

    render_report = render_rgb_difference_report(obj, samples=2, resolution=48)
    assert render_report["rgb_absolute_error"]["count"] == 48 * 48 * 2, (
        "Stereo multiview RGB validation must compare both eye renders; "
        f"report={render_report}"
    )
    assert_camera_lod_render_report_matches_dense(render_report, "Stereo multiview render")


def assert_camera_lod_animated_camera_rgb_validation():
    clear_scene()
    set_world_sky_gradient(strength=1.25)
    scene = bpy.context.scene
    scene.frame_start = 1
    scene.frame_end = 12
    cam = make_camera_and_light(
        cam_location=(-4.0, -28.0, 6.0),
        cam_target=(-4.0, 28.0, 0.0),
        lens=36.0,
        sun_rotation=(0.2, 0.0, 0.8),
        sun_energy=1.0,
    )
    cam.keyframe_insert(data_path="location", frame=1)
    cam.keyframe_insert(data_path="rotation_euler", frame=1)
    cam.location = (6.0, -24.0, 5.5)
    look_at(cam, (6.0, 32.0, 0.0))
    cam.keyframe_insert(data_path="location", frame=12)
    cam.keyframe_insert(data_path="rotation_euler", frame=12)

    obj = make_ocean_object(
        name="OceanAnimatedCameraRGBLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=96,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.45,
        wind_velocity=7.0,
        roughness=0.02,
    )

    scene.frame_set(1)
    assert_camera_lod_attributes(obj)
    start_report = camera_lod_reference_report(obj, cam)
    assert_camera_lod_geometry_report_matches_dense(start_report, "Animated camera frame 1")

    scene.frame_set(12)
    assert_camera_lod_attributes(obj)
    end_report = camera_lod_reference_report(obj, cam)
    assert_camera_lod_geometry_report_matches_dense(end_report, "Animated camera frame 12")
    assert abs(end_report["mean_reprojection_error"] - start_report["mean_reprojection_error"]) < 0.75

    render_report = render_rgb_difference_report(obj, samples=2, resolution=48)
    assert_camera_lod_render_report_matches_dense(render_report, "Animated camera render")


def assert_camera_lod_object_motion_blur_rgb_validation():
    clear_scene()
    set_world_sky_gradient(strength=1.25)
    scene = bpy.context.scene
    enable_centered_motion_blur(scene)
    cam = make_camera_and_light(
        cam_location=(0.0, -36.0, 7.0),
        cam_target=(0.0, 60.0, 0.0),
        lens=35.0,
        sun_rotation=(0.2, 0.0, 0.8),
        sun_energy=1.0,
    )
    obj = make_ocean_object(
        name="OceanObjectMotionBlurLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.45,
        wind_velocity=7.0,
        roughness=0.02,
    )

    for frame, x_location in ((9, -5.0), (10, 0.0), (11, 5.0)):
        scene.frame_set(frame)
        obj.location.x = x_location
        obj.keyframe_insert(data_path="location", frame=frame)

    topologies = camera_lod_frame_topologies(obj)
    assert_motion_blur_topology_changes(topologies, "Object transform motion blur")

    scene.frame_set(10)
    geometry_report = camera_lod_reference_report(obj, cam)
    assert_camera_lod_geometry_report_matches_dense(
        geometry_report, "Object transform motion blur")

    render_report = render_rgb_difference_report(obj, samples=2, resolution=48)
    assert_camera_lod_render_report_matches_dense(
        render_report, "Object transform motion blur")


def assert_camera_lod_camera_motion_blur_rgb_validation():
    clear_scene()
    set_world_sky_gradient(strength=1.25)
    scene = bpy.context.scene
    enable_centered_motion_blur(scene)
    cam = make_camera_and_light(
        cam_location=(-6.0, -36.0, 7.0),
        cam_target=(-6.0, 60.0, 0.0),
        lens=35.0,
        sun_rotation=(0.2, 0.0, 0.8),
        sun_energy=1.0,
    )
    cam.keyframe_insert(data_path="location", frame=9)
    cam.keyframe_insert(data_path="rotation_euler", frame=9)

    scene.frame_set(10)
    cam.location = (0.0, -36.0, 7.0)
    look_at(cam, (0.0, 60.0, 0.0))
    cam.keyframe_insert(data_path="location", frame=10)
    cam.keyframe_insert(data_path="rotation_euler", frame=10)

    scene.frame_set(11)
    cam.location = (6.0, -36.0, 7.0)
    look_at(cam, (6.0, 60.0, 0.0))
    cam.keyframe_insert(data_path="location", frame=11)
    cam.keyframe_insert(data_path="rotation_euler", frame=11)

    obj = make_ocean_object(
        name="OceanCameraMotionBlurLOD",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.45,
        wind_velocity=7.0,
        roughness=0.02,
    )

    topologies = camera_lod_frame_topologies(obj)
    assert_motion_blur_topology_changes(topologies, "Camera motion blur")

    scene.frame_set(10)
    geometry_report = camera_lod_reference_report(obj, cam)
    assert_camera_lod_geometry_report_matches_dense(geometry_report, "Camera motion blur")

    render_report = render_rgb_difference_report(obj, samples=2, resolution=48)
    assert_camera_lod_render_report_matches_dense(render_report, "Camera motion blur")


def assert_camera_lod_extreme_view_validation():

    def assert_extreme_case(case_name,
                            cam_location,
                            cam_target,
                            lens,
                            clip_start,
                            resolution,
                            spatial_size,
                            lod_levels,
                            lod_validation_mode):
        clear_scene()
        cam = make_camera_and_light(
            cam_location=cam_location,
            cam_target=cam_target,
            lens=lens,
            sun_energy=0.0,
        )
        cam.data.clip_start = clip_start
        cam.data.clip_end = 20000.0
        obj = make_ocean_object(
            name=case_name,
            geometry_mode="GENERATE",
            resolution=resolution,
            spatial_size=spatial_size,
            size=1.0,
            camera_lod=True,
            lod_levels=lod_levels,
            lod_validation_mode=lod_validation_mode,
            time_value=1.0,
            choppiness=0.0,
            wind_velocity=1.0,
            roughness=0.02,
        )
        assert_camera_lod_attributes(obj)
        report = camera_lod_reference_report(obj, cam)
        assert_camera_lod_geometry_report_matches_dense(report, case_name)
        return report

    wide = assert_extreme_case(
        "OceanWideFOVLOD",
        cam_location=(0.0, -38.0, 7.0),
        cam_target=(0.0, 34.0, 0.0),
        lens=16.0,
        clip_start=0.05,
        resolution=6,
        spatial_size=96,
        lod_levels=5,
        lod_validation_mode="CAMERA_OBSERVABLE",
    )
    assert wide["visible_sample_count"] > 800, (
        f"Wide-FOV validation should cover a broad visible footprint; report={wide}"
    )

    assert_extreme_case(
        "OceanGrazingCameraLOD",
        cam_location=(0.0, -58.0, 2.2),
        cam_target=(0.0, 160.0, 0.0),
        lens=45.0,
        clip_start=0.05,
        resolution=6,
        spatial_size=128,
        lod_levels=5,
        lod_validation_mode="GEOMETRY_STRICT",
    )

    close = assert_extreme_case(
        "OceanCloseNearClipLOD",
        cam_location=(0.0, -6.0, 0.8),
        cam_target=(0.0, 5.0, 0.0),
        lens=26.0,
        clip_start=0.02,
        resolution=6,
        spatial_size=64,
        lod_levels=5,
        lod_validation_mode="GEOMETRY_STRICT",
    )
    assert close["visible_sample_count"] > 250, (
        f"Very-close near-plane validation should include a meaningful visible sample set; "
        f"report={close}"
    )


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
        cam_location=(58.0, 58.0, 7.0),
        cam_target=(62.0, 62.0, 0.0),
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
        repeat_y=2,
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
    base_tile_max_y = 0.5 * float(mod.spatial_size)
    report = camera_lod_visible_coverage_report(obj, cam)
    assert report["visible_sample_count"] > 0, (
        "Repeat visibility validation needs visible dense reference samples"
    )
    assert report["visible_reference_max_x"] > base_tile_max_x, (
        "Repeat visibility validation must exercise the second repeated X tile; "
        f"report={report}"
    )
    assert report["visible_reference_max_y"] > base_tile_max_y, (
        "Repeat visibility validation must exercise the second repeated Y tile; "
        f"report={report}"
    )
    assert report["lod_verts"] < report["dense_verts"], (
        "Repeated-domain camera LOD should remain adaptive rather than falling back to dense"
    )
    assert report["missing_visible_sample_count"] == 0, (
        "Repeated-domain camera LOD must cover visible dense samples outside the base tile; "
        f"report={report}"
    )


def assert_repeat_visibility_case(name,
                                  cam_location,
                                  cam_target,
                                  lens,
                                  resolution,
                                  spatial_size,
                                  size,
                                  repeat_x,
                                  repeat_y,
                                  lod_levels):
    clear_scene()
    cam = make_camera_and_light(
        cam_location=cam_location,
        cam_target=cam_target,
        lens=lens,
        sun_energy=0.0,
    )
    bpy.context.scene.render.resolution_x = 256
    bpy.context.scene.render.resolution_y = 128
    cam.data.clip_start = 0.1
    cam.data.clip_end = 20000.0

    obj = make_ocean_object(
        name=name,
        geometry_mode="GENERATE",
        resolution=resolution,
        spatial_size=spatial_size,
        size=size,
        repeat_x=repeat_x,
        repeat_y=repeat_y,
        camera_lod=True,
        lod_levels=lod_levels,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
        roughness=0.02,
    )
    assert_camera_lod_attributes(obj)

    report = camera_lod_visible_coverage_report(obj, cam)
    assert report["visible_sample_count"] > 0, (
        f"{name} needs visible dense reference samples; report={report}"
    )
    assert report["lod_verts"] < report["dense_verts"], (
        f"{name} must remain adaptive rather than falling back to dense; report={report}"
    )
    assert report["missing_visible_sample_count"] == 0, (
        f"{name} must cover all visible dense reference samples; report={report}"
    )

    topology_report = camera_lod_topology_report(obj)
    assert len(topology_report["split_level_histogram"]) > 1, (
        f"{name} should use multiple split levels; report={topology_report}"
    )
    assert topology_report["max_face_split_spread"] <= 2, (
        f"{name} split-level metadata should remain locally balanced; report={topology_report}"
    )
    assert topology_report["max_face_local_spread"] <= 2, (
        f"{name} local-level metadata should remain locally balanced; report={topology_report}"
    )
    assert topology_report["max_face_vertex_count"] <= 6, (
        f"{name} should only add edge vertices needed for balanced neighbors; report={topology_report}"
    )

    mod = obj.modifiers["Ocean"]
    base_cell = (mod.size * mod.spatial_size) / float(mod.resolution * mod.resolution)
    anchor_distance = finest_level_min_distance_to_point(
        obj, camera_ocean_domain_anchor_xy(obj, cam), attr_name="ocean_camera_lod_split_level"
    )
    assert anchor_distance <= base_cell, (
        f"{name} should keep split level 0 within one dense cell of the camera anchor; "
        f"got {anchor_distance:.3f}m, base_cell={base_cell:.3f}m"
    )

    return report


def assert_camera_lod_repeat_tiles_edge_cases():
    scaled = assert_repeat_visibility_case(
        "OceanRepeatScaledTileLOD",
        cam_location=(145.0, 145.0, 14.0),
        cam_target=(155.0, 155.0, 0.0),
        lens=38.0,
        resolution=6,
        spatial_size=64,
        size=2.5,
        repeat_x=2,
        repeat_y=2,
        lod_levels=4,
    )
    assert scaled["visible_reference_max_x"] > 32.0 and scaled["visible_reference_max_y"] > 32.0, (
        "Scaled repeated-domain test must exercise repeated tiles in reference coordinates; "
        f"report={scaled}"
    )

    seam = assert_repeat_visibility_case(
        "OceanRepeatSeamCrossingLOD",
        cam_location=(26.0, 26.0, 8.0),
        cam_target=(34.0, 34.0, 0.0),
        lens=35.0,
        resolution=6,
        spatial_size=64,
        size=1.0,
        repeat_x=2,
        repeat_y=2,
        lod_levels=4,
    )
    assert seam["visible_reference_min_x"] < 32.0 < seam["visible_reference_max_x"], (
        "Seam-crossing repeat test must straddle the X tile boundary; "
        f"report={seam}"
    )
    assert seam["visible_reference_min_y"] < 32.0 < seam["visible_reference_max_y"], (
        "Seam-crossing repeat test must straddle the Y tile boundary; "
        f"report={seam}"
    )

    awkward = assert_repeat_visibility_case(
        "OceanRepeatAwkwardLOD",
        cam_location=(62.0, 24.0, 8.0),
        cam_target=(66.0, 28.0, 0.0),
        lens=38.0,
        resolution=7,
        spatial_size=48,
        size=1.0,
        repeat_x=3,
        repeat_y=2,
        lod_levels=5,
    )
    assert awkward["visible_reference_max_x"] > 24.0, (
        "Awkward repeat test must exercise repeated X tiles; "
        f"report={awkward}"
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


def assert_foam_spray_attribute_report_matches_dense(report, attribute_name):
    assert report["dense_luminance_range"] > 0.01, (
        f"{attribute_name} parity render must contain a spatially varying dense reference"
    )
    assert report["max_dense_luminance_gradient"] > 0.001, (
        f"{attribute_name} parity render must contain enough dense contrast to catch mapping shifts"
    )
    assert report["mean_absolute_error"] < 0.005, report
    assert report["p95_absolute_error"] < 0.02, report
    assert report["max_absolute_error"] < 0.05, report


def clear_material_nodes(obj, name):
    if not obj.data.materials:
        obj.data.materials.append(bpy.data.materials.new(name))
    mat = obj.data.materials[0]
    mat.name = name
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    nodes.clear()
    output = nodes.new(type="ShaderNodeOutputMaterial")
    return nodes, links, output


def set_attribute_fac_emission_material(obj, attribute_name):
    nodes, links, output = clear_material_nodes(obj, f"{obj.name}{attribute_name}FacMat")
    attr = nodes.new(type="ShaderNodeAttribute")
    attr.attribute_name = attribute_name
    emission = nodes.new(type="ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    links.new(attr.outputs["Fac"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])


def set_foam_spray_mixed_attribute_material(obj):
    nodes, links, output = clear_material_nodes(obj, f"{obj.name}MixedAttributeMat")
    foam = nodes.new(type="ShaderNodeAttribute")
    foam.attribute_name = "foam"
    spray = nodes.new(type="ShaderNodeAttribute")
    spray.attribute_name = "spray"
    add = nodes.new(type="ShaderNodeVectorMath")
    add.operation = "ADD"
    emission = nodes.new(type="ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    links.new(foam.outputs["Color"], add.inputs[0])
    links.new(spray.outputs["Color"], add.inputs[1])
    links.new(add.outputs["Vector"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])


def set_attribute_bump_material(obj, attribute_name):
    nodes, links, output = clear_material_nodes(obj, f"{obj.name}{attribute_name}BumpMat")
    attr = nodes.new(type="ShaderNodeAttribute")
    attr.attribute_name = attribute_name
    bump = nodes.new(type="ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.35
    bump.inputs["Distance"].default_value = 0.25
    bsdf = nodes.new(type="ShaderNodeBsdfPrincipled")
    bsdf.inputs["Base Color"].default_value = (0.02, 0.08, 0.12, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.2
    links.new(attr.outputs["Fac"], bump.inputs["Height"])
    links.new(bump.outputs["Normal"], bsdf.inputs["Normal"])
    links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])


def make_foam_spray_attribute_object(name, use_spray=True):
    obj = make_ocean_object(
        name=name,
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
    mod.use_spray = use_spray
    mod.spray_layer_name = "spray"
    bpy.context.view_layer.update()
    return obj


def assert_foam_spray_lod_geometry_is_adaptive(obj, expected_attrs):
    assert_camera_lod_attributes(obj)

    lod_verts, lod_faces, lod_unused, lod_attrs = evaluated_mesh_stats(obj)
    dense_verts, dense_faces, dense_unused, _dense_attrs = dense_mesh_stats(obj)

    assert "ocean_camera_lod_level" in lod_attrs, (
        "Cycles foam/spray camera LOD should keep adaptive geometry; full-spectrum foam/spray "
        "is sampled by the shader instead of requiring dense geometry"
    )
    assert expected_attrs.issubset(lod_attrs), (
        f"Foam/spray layer names should still be exported; expected {expected_attrs}, got {lod_attrs}"
    )
    assert lod_verts < dense_verts and lod_faces < dense_faces, (
        "Foam/spray camera LOD should remain adaptive; "
        f"lod={(lod_verts, lod_faces, lod_unused)}, dense={(dense_verts, dense_faces, dense_unused)}"
    )


def assert_camera_lod_foam_attribute_matches_dense_reference(use_spray):
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -58.0, 3.0),
        cam_target=(0.0, 180.0, 0.0),
        lens=35.0,
        sun_energy=0.0,
    )
    expected_attrs = {"foam", "spray"} if use_spray else {"foam"}
    obj = make_foam_spray_attribute_object("OceanCameraLODFoamSpray", use_spray)
    mod = obj.modifiers["Ocean"]
    assert_foam_spray_lod_geometry_is_adaptive(obj, expected_attrs)

    for attribute_name in (["foam", "spray"] if use_spray else ["foam"]):
        report = render_attribute_difference_report(obj, attribute_name, samples=1, resolution=64)
        assert_foam_spray_attribute_report_matches_dense(report, attribute_name)

    if use_spray:
        scene = bpy.context.scene
        scene.frame_start = 9
        scene.frame_end = 11
        scene.render.use_motion_blur = True
        scene.render.motion_blur_shutter = 1.0
        if hasattr(scene.render, "motion_blur_position"):
            scene.render.motion_blur_position = "CENTER"
        for frame, time_value in ((9, 0.65), (10, 1.0), (11, 1.35)):
            scene.frame_set(frame)
            mod.time = time_value
            mod.keyframe_insert(data_path="time", frame=frame)
        scene.frame_set(10)

        for attribute_name in ("foam", "spray"):
            report = render_attribute_difference_report(obj, attribute_name, samples=1, resolution=64)
            assert_foam_spray_attribute_report_matches_dense(report, attribute_name)


def assert_camera_lod_foam_and_spray_use_camera_lod_geometry():
    assert_camera_lod_foam_attribute_matches_dense_reference(use_spray=False)
    assert_camera_lod_foam_attribute_matches_dense_reference(use_spray=True)


def assert_ocean_compression_attribute_exports_live_foam_signal():
    clear_scene()
    obj = make_ocean_object(
        name="OceanCompressionAttribute",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=128,
        size=1.0,
        camera_lod=False,
        time_value=1.0,
        choppiness=1.8,
        wind_velocity=37.04,
        wave_scale=18.0,
        smallest_wave=0.02,
        seed=11,
    )
    mod = obj.modifiers["Ocean"]
    mod.use_foam = True
    mod.foam_layer_name = "foam"
    mod.foam_coverage = 0.0
    bpy.context.view_layer.update()

    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()

    try:
        attr = mesh_eval.attributes.get("ocean_compression")
        assert attr is not None, "Live foam evaluation should export ocean_compression"
        assert attr.domain == "CORNER", (
            f"ocean_compression must be corner-domain, got {attr.domain}"
        )
        assert attr.data_type == "FLOAT", (
            f"ocean_compression must be a float attribute, got {attr.data_type}"
        )

        values = [float(item.value) for item in attr.data]
        assert len(values) == len(mesh_eval.loops), (
            f"ocean_compression should have one value per corner; "
            f"got {len(values)} values for {len(mesh_eval.loops)} loops"
        )
        assert values, "ocean_compression should contain samples"
        assert all(math.isfinite(value) and value >= 0.0 for value in values), (
            "ocean_compression should export finite positive compression magnitudes"
        )
        assert max(values) > 0.0, "ocean_compression should preserve non-zero Jminus compression"
    finally:
        obj_eval.to_mesh_clear()


def assert_camera_lod_foam_spray_fac_and_bump_usage_matches_dense():
    for attribute_name in ("foam", "spray"):
        clear_scene()
        make_camera_and_light(
            cam_location=(0.0, -58.0, 3.0),
            cam_target=(0.0, 180.0, 0.0),
            lens=55.0,
            sun_energy=0.0,
        )
        obj = make_foam_spray_attribute_object(
            f"OceanCameraLOD{attribute_name.title()}Fac", use_spray=True)
        assert_foam_spray_lod_geometry_is_adaptive(obj, {"foam", "spray"})
        set_attribute_fac_emission_material(obj, attribute_name)
        report = render_rgb_difference_report(obj, samples=2, resolution=48)
        assert_foam_spray_attribute_report_matches_dense(report, f"{attribute_name} Fac")

    for attribute_name in ("foam", "spray"):
        clear_scene()
        set_world_sky_gradient(strength=1.25)
        make_camera_and_light(
            cam_location=(0.0, -58.0, 3.0),
            cam_target=(0.0, 180.0, 0.0),
            lens=55.0,
            sun_rotation=(0.2, 0.0, 0.8),
            sun_energy=1.0,
        )
        obj = make_foam_spray_attribute_object(
            f"OceanCameraLOD{attribute_name.title()}Bump", use_spray=True)
        assert_foam_spray_lod_geometry_is_adaptive(obj, {"foam", "spray"})
        set_attribute_bump_material(obj, attribute_name)
        report = render_rgb_difference_report(obj, samples=2, resolution=48)
        assert_foam_spray_attribute_report_matches_dense(report, f"{attribute_name} bump")


def assert_camera_lod_multiple_attribute_users_match_dense():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -58.0, 3.0),
        cam_target=(0.0, 180.0, 0.0),
        lens=55.0,
        sun_energy=0.0,
    )
    obj = make_foam_spray_attribute_object("OceanCameraLODMixedAttributeUsers", use_spray=True)
    assert_foam_spray_lod_geometry_is_adaptive(obj, {"foam", "spray"})
    set_foam_spray_mixed_attribute_material(obj)
    report = render_rgb_difference_report(obj, samples=2, resolution=48)
    assert_foam_spray_attribute_report_matches_dense(report, "foam+spray mixed users")

    obj.location.x = -70.0
    companion = make_foam_spray_attribute_object(
        "OceanCameraLODCompanionMaterial", use_spray=True)
    companion.location.x = 70.0
    set_attribute_fac_emission_material(companion, "spray")
    assert_foam_spray_lod_geometry_is_adaptive(companion, {"foam", "spray"})
    scene_report = render_scene_rgb_difference_report([obj, companion], samples=2, resolution=48)
    assert scene_report["object_count"] == 2, scene_report
    assert scene_report["lod_render_s"] > 0.0 and scene_report["dense_render_s"] > 0.0, (
        f"Multi-material attribute scene must record LOD and dense timings; report={scene_report}"
    )
    assert_foam_spray_attribute_report_matches_dense(scene_report, "multi-material attribute scene")


def assert_camera_lod_large_foam_spray_profile_bounds():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -58.0, 3.0),
        cam_target=(0.0, 180.0, 0.0),
        lens=55.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanCameraLODLargeFoamSprayPerf",
        geometry_mode="GENERATE",
        resolution=8,
        spatial_size=160,
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
    mod.use_spray = True
    mod.spray_layer_name = "spray"
    set_foam_spray_mixed_attribute_material(obj)
    bpy.context.view_layer.update()

    assert_foam_spray_lod_geometry_is_adaptive(obj, {"foam", "spray"})
    lod_verts, lod_faces, _lod_unused, _lod_attrs = evaluated_mesh_stats(obj)
    dense_verts, dense_faces, _dense_unused, _dense_attrs = dense_mesh_stats(obj)
    assert dense_verts >= 4000 and dense_faces >= 4000, (
        f"Large foam/spray profile case must exercise a larger dense ocean; "
        f"lod={(lod_verts, lod_faces)}, dense={(dense_verts, dense_faces)}"
    )
    assert lod_verts < dense_verts and lod_faces < dense_faces, (
        f"Large foam/spray profile case must keep some LOD reduction while preserving the "
        f"visible foam/spray carrier; "
        f"lod={(lod_verts, lod_faces)}, dense={(dense_verts, dense_faces)}"
    )

    result = profiled_render_rgb_difference_report(obj, samples=1, resolution=32)
    report = result["report"]
    assert_foam_spray_attribute_report_matches_dense(report, "large foam/spray")
    assert_render_timing_pair_recorded(report, "large foam/spray")

    entries = result["profile_entries"]
    lod_modifier_entries = ocean_profile_entries(
        entries, "modifier", mode="camera_lod", object_name=obj.name)
    dense_modifier_entries = ocean_profile_entries(
        entries, "modifier", mode="dense_reference", object_name=obj.name)
    sync_entries = ocean_profile_entries(
        entries, "cycles_sync_mesh", mode="camera_lod", object_name=obj.name)
    assert lod_modifier_entries and dense_modifier_entries and sync_entries, (
        "Large foam/spray render must emit modifier and Cycles sync profile entries; "
        f"profile was:\n{result['profile_log']}"
    )

    lod_modifier = max(lod_modifier_entries, key=lambda entry: entry.get("total_s", 0.0))
    dense_foam_s = max(entry.get("foam_s", 0.0) for entry in dense_modifier_entries)
    lod_foam_s = lod_modifier.get("foam_s", 0.0)
    assert int(lod_modifier.get("verts", 0)) == lod_verts, (
        f"Profiled LOD modifier vertex count must match evaluated mesh stats; "
        f"profile={lod_modifier}, lod_verts={lod_verts}"
    )
    assert int(lod_modifier.get("runtime_sample_calls", 0)) > 0, (
        f"Large foam/spray case must use runtime full-spectrum foam/spray samples; "
        f"profile={lod_modifier}"
    )
    assert lod_foam_s < 0.05, (
        f"Large foam/spray LOD texture prep should remain bounded; profile={lod_modifier}"
    )
    assert lod_foam_s <= dense_foam_s * 1.25 + 0.005, (
        f"Large foam/spray LOD texture prep should not exceed dense by a material margin; "
        f"lod_foam_s={lod_foam_s}, dense_foam_s={dense_foam_s}"
    )

    sync_entry = max(sync_entries, key=lambda entry: entry.get("sync_split_resources_s", 0.0))
    assert int(sync_entry.get("synced_verts", 0)) < dense_verts, (
        f"Cycles split-resource sync must preserve reduced LOD geometry; sync={sync_entry}"
    )
    assert int(sync_entry.get("synced_tris", 0)) < dense_faces * 2, (
        f"Cycles split-resource sync must remain below dense triangle count; sync={sync_entry}"
    )
    assert int(sync_entry.get("split_levels", 0)) > 1, (
        f"Large foam/spray case should exercise multiple split-resource levels; sync={sync_entry}"
    )
    assert sync_entry.get("sync_split_resources_s", 0.0) < 0.05, (
        f"Cycles split-resource sync time should remain bounded; sync={sync_entry}"
    )


def assert_camera_lod_representative_render_timings():
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
        name="OceanCameraLODRepresentativeTiming",
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

    cpu_report = render_rgb_difference_report_with_timings(obj, samples=2, resolution=32)
    assert_camera_lod_render_report_matches_dense(cpu_report, "CPU representative timing render")
    assert_render_timing_pair_recorded(cpu_report, "CPU representative timing render")

    available_devices = ocean_metrics.get_available_cycles_devices()
    gpu_device = next(
        (
            device
            for device in available_devices
            if device != "CPU" and not device.endswith("-RT") and not device.endswith("-OSL")
        ),
        None,
    )
    require_gpu_timing = os.environ.get(REQUIRE_GPU_TIMING_ENV, "").lower() not in {"", "0", "false"}
    if gpu_device is None:
        message = "No GPU Cycles device available for representative timing smoke"
        if require_gpu_timing:
            raise RuntimeError(message)
        print(message)
        return

    try:
        gpu_report = render_rgb_difference_report_with_timings(
            obj, samples=1, resolution=16, device=gpu_device)
    except Exception as ex:
        if require_gpu_timing:
            raise RuntimeError(
                f"Required {gpu_device} representative timing smoke failed") from ex
        print(f"Optional {gpu_device} representative timing smoke skipped: {ex}")
        return

    assert_render_timing_pair_recorded(gpu_report, f"{gpu_device} representative timing render")
    assert gpu_report["dense_luminance_range"] > 0.01, (
        f"{gpu_device} representative timing render must render non-flat lighting; "
        f"report={gpu_report}"
    )


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
    assert_camera_lod_leaf_layout_balanced(obj, "Wide-footprint stereo dataset")


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


def assert_camera_lod_falls_back_for_non_cycles_foam():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -35.0, 8.0),
        cam_target=(0.0, 0.0, 0.0),
        lens=50.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanNonCyclesFoamFallback",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.4,
        wind_velocity=10.0,
    )
    mod = obj.modifiers["Ocean"]
    mod.use_foam = True
    mod.use_spray = True
    bpy.context.scene.render.engine = "BLENDER_EEVEE"
    assert_camera_lod_dense_fallback(
        obj, "Camera LOD foam/spray must fall back to dense outside Cycles")


def assert_camera_lod_falls_back_for_cached_ocean():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -35.0, 8.0),
        cam_target=(0.0, 0.0, 0.0),
        lens=50.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanCachedDenseFallback",
        geometry_mode="GENERATE",
        resolution=5,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.4,
        wind_velocity=10.0,
    )
    live_lod_stats = evaluated_mesh_stats(obj)
    live_dense_stats = dense_mesh_stats(obj)
    assert live_lod_stats[:3] != live_dense_stats[:3], (
        "Cached fallback test must exercise a live reduced LOD mesh before baking"
    )

    mod = obj.modifiers["Ocean"]
    with tempfile.TemporaryDirectory(prefix="ocean_camera_lod_cached_fallback_") as tempdir:
        mod.frame_start = 1
        mod.frame_end = 1
        mod.filepath = tempdir
        bpy.context.scene.frame_set(1)
        select_active_object(obj)
        assert bpy.ops.object.ocean_bake(modifier="Ocean") == {"FINISHED"}
        assert mod.is_cached, "Ocean bake operator must leave the modifier in cached mode"
        assert mod.use_camera_lod, "Cached fallback test must keep Camera LOD enabled"

        bpy.context.view_layer.update()
        assert_camera_lod_dense_fallback(
            obj, "Cached camera LOD Ocean must evaluate with dense-equivalent topology")


def assert_camera_lod_final_render_falls_back_for_non_cycles():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -35.0, 8.0),
        cam_target=(0.0, 0.0, 0.0),
        lens=50.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanNonCyclesRenderFallback",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.4,
        wind_velocity=10.0,
    )
    lod_stats = evaluated_mesh_stats(obj)
    dense_stats = dense_mesh_stats(obj)
    assert lod_stats[:3] != dense_stats[:3], (
        "Non-Cycles render fallback test must exercise reduced viewport LOD geometry"
    )

    scene = bpy.context.scene
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 16
    scene.render.resolution_y = 16
    scene.render.resolution_percentage = 100
    log = ocean_metrics.capture_process_stdout(lambda: bpy.ops.render.render(write_still=False))
    assert "Camera LOD render equivalence is only available in Cycles; using dense geometry" in log, (
        "Non-Cycles final render must use dense Ocean geometry; render log was:\n" + log
    )


def assert_camera_lod_apply_and_convert_bake_dense_mesh():
    def build_apply_case(name):
        clear_scene()
        make_camera_and_light(
            cam_location=(0.0, -35.0, 8.0),
            cam_target=(0.0, 0.0, 0.0),
            lens=50.0,
            sun_energy=0.0,
        )
        return make_ocean_object(
            name=name,
            geometry_mode="GENERATE",
            resolution=6,
            spatial_size=64,
            size=1.0,
            camera_lod=True,
            lod_levels=4,
            time_value=1.0,
            choppiness=0.4,
            wind_velocity=10.0,
        )

    obj = build_apply_case("OceanApplyDenseFallback")
    lod_stats = evaluated_mesh_stats(obj)
    dense_stats = dense_mesh_stats(obj)
    assert lod_stats[:3] != dense_stats[:3], "Apply test must exercise reduced LOD geometry"
    select_active_object(obj)
    assert bpy.ops.object.modifier_apply(modifier="Ocean") == {"FINISHED"}
    assert len(obj.modifiers) == 0, "Ocean modifier must be removed after apply"
    assert_camera_lod_mesh_data_matches_dense(
        obj.data, dense_stats, "Applying camera LOD Ocean must bake dense geometry")

    obj = build_apply_case("OceanConvertDenseFallback")
    lod_stats = evaluated_mesh_stats(obj)
    dense_stats = dense_mesh_stats(obj)
    assert lod_stats[:3] != dense_stats[:3], "Convert test must exercise reduced LOD geometry"
    select_active_object(obj)
    assert bpy.ops.object.convert(target="MESH") == {"FINISHED"}
    converted = bpy.context.object
    assert converted is not None, "Object conversion must leave an active converted object"
    assert len(converted.modifiers) == 0, "Ocean modifier must be removed after convert"
    assert_camera_lod_mesh_data_matches_dense(
        converted.data, dense_stats, "Converting camera LOD Ocean must bake dense geometry")


def assert_camera_lod_exports_dense_mesh():
    clear_scene()
    make_camera_and_light(
        cam_location=(0.0, -35.0, 8.0),
        cam_target=(0.0, 0.0, 0.0),
        lens=50.0,
        sun_energy=0.0,
    )
    obj = make_ocean_object(
        name="OceanExportDenseFallback",
        geometry_mode="GENERATE",
        resolution=6,
        spatial_size=64,
        size=1.0,
        camera_lod=True,
        lod_levels=4,
        time_value=1.0,
        choppiness=0.4,
        wind_velocity=10.0,
    )
    lod_stats = evaluated_mesh_stats(obj)
    dense_stats = dense_mesh_stats(obj)
    dense_tris = dense_corner_tri_count(obj)
    assert lod_stats[:3] != dense_stats[:3], "Export test must exercise reduced LOD geometry"
    select_active_object(obj)

    with tempfile.TemporaryDirectory() as tmpdir:
        obj_path = os.path.join(tmpdir, "ocean.obj")
        assert bpy.ops.wm.obj_export(
            filepath=obj_path,
            export_selected_objects=True,
            apply_modifiers=True,
            export_materials=False,
            export_uv=False,
            export_normals=False,
            export_colors=False,
        ) == {"FINISHED"}
        assert count_obj_vertices_and_faces(obj_path) == dense_stats[:2], (
            "OBJ export must bake dense Ocean geometry"
        )

        ply_path = os.path.join(tmpdir, "ocean.ply")
        assert bpy.ops.wm.ply_export(
            filepath=ply_path,
            export_selected_objects=True,
            apply_modifiers=True,
            ascii_format=True,
            export_uv=False,
            export_normals=False,
            export_colors="NONE",
            export_attributes=False,
        ) == {"FINISHED"}
        assert count_ply_vertices_and_faces(ply_path) == dense_stats[:2], (
            "PLY export must bake dense Ocean geometry"
        )

        stl_path = os.path.join(tmpdir, "ocean.stl")
        assert bpy.ops.wm.stl_export(
            filepath=stl_path,
            export_selected_objects=True,
            apply_modifiers=True,
            ascii_format=True,
        ) == {"FINISHED"}
        assert count_ascii_stl_facets(stl_path) == dense_tris, (
            "STL export must bake dense Ocean geometry"
        )

    assert obj.modifiers["Ocean"].use_camera_lod, "Export must restore Camera LOD after writing"
    assert evaluated_mesh_stats(obj)[:3] == lod_stats[:3], (
        "Export must restore reduced viewport LOD evaluation after writing"
    )


def assert_stereo_dataset_requires_true_stereo_pair():

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
    assert_camera_lod_dense_fallback(
        obj, "Stereo dataset mode must fail closed when multiview is disabled")

    def disable_right_eye(scene):
        for view in scene.render.views:
            view.use = view.name.lower() in {"left", "right"}
        for view in scene.render.views:
            if view.name.lower() == "right":
                view.use = False

    obj = build_invalid_case("OceanStereoDatasetInvalidRightOnly", disable_right_eye)
    assert_camera_lod_dense_fallback(
        obj, "Stereo dataset mode must fail closed when one stereo eye is disabled")
    bpy.context.scene.render.views_format = "MULTIVIEW"
    assert_camera_lod_dense_fallback(
        obj, "Stereo dataset mode must fail closed outside stereo-3D render mode")


def main():
    assert_ocean_camera_lod_metrics_module_contract()

    clear_scene()
    obj = make_ocean_object()
    make_camera_and_light()
    assert_camera_lod_attributes(obj)

    assert_realsea_spectrum_options_are_valid()
    assert_camera_lod_generate_tracks_camera()
    assert_camera_lod_budget_invariant()
    assert_camera_lod_large_footprint_budget_regression()
    assert_camera_lod_reference_validation()
    assert_camera_lod_reference_validation_hits_simulation_ceiling()
    assert_camera_lod_temporal_reference_validation()
    assert_camera_lod_animated_camera_rgb_validation()
    assert_camera_lod_object_motion_blur_rgb_validation()
    assert_camera_lod_camera_motion_blur_rgb_validation()
    assert_camera_lod_strict_mode_adversarial_validation()
    assert_camera_lod_high_choppiness_high_wind_validation()
    assert_camera_lod_full_spectrum_radius_behavior()
    assert_camera_lod_transition_and_balancing_validation()
    assert_camera_lod_extreme_view_validation()
    assert_camera_lod_general_render_validates_relevant_leaf_error()
    assert_camera_lod_conservative_visibility_coverage()
    assert_camera_lod_repeat_tiles_cover_visible_domain()
    assert_camera_lod_repeat_tiles_edge_cases()
    assert_ocean_compression_attribute_exports_live_foam_signal()
    assert_camera_lod_foam_and_spray_use_camera_lod_geometry()
    assert_camera_lod_foam_spray_fac_and_bump_usage_matches_dense()
    assert_camera_lod_multiple_attribute_users_match_dense()
    assert_camera_lod_large_foam_spray_profile_bounds()
    assert_camera_lod_representative_render_timings()
    assert_stereo_dataset_reference_validation()
    assert_stereo_dataset_wide_footprint_uses_multiple_levels()
    assert_stereo_dataset_wide_footprint_finest_patch_tracks_camera_anchor()
    assert_stereo_dataset_reacts_to_camera_parameter_changes()
    assert_stereo_dataset_rgb_validation()
    assert_camera_lod_falls_back_for_cached_ocean()
    assert_camera_lod_falls_back_for_non_cycles_foam()
    assert_camera_lod_final_render_falls_back_for_non_cycles()
    assert_camera_lod_apply_and_convert_bake_dense_mesh()
    assert_camera_lod_exports_dense_mesh()
    assert_stereo_dataset_requires_true_stereo_pair()
    assert_camera_lod_rgb_validation()


if __name__ == "__main__":
    main()
