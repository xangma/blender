# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

import math
import os
import sys

import bpy

sys.path.append(os.path.dirname(os.path.realpath(__file__)))

from modules import ocean_camera_lod_metrics as ocean_metrics


def evaluated_lod_snapshot(obj):
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(depsgraph)
    mesh_eval = obj_eval.to_mesh()
    try:
        ref_coord = mesh_eval.attributes.get("ocean_ref_coord")
        if ref_coord is not None:
            coordinates = tuple(
                (round(item.vector[0], 5), round(item.vector[2], 5))
                for item in ref_coord.data
            )
        else:
            coordinates = tuple(
                (round(vertex.co.x, 5), round(vertex.co.y, 5))
                for vertex in mesh_eval.vertices
            )
        return {
            "vertices": len(mesh_eval.vertices),
            "faces": len(mesh_eval.polygons),
            "coordinates": coordinates,
            "policy": mesh_eval.get("ocean_camera_lod_policy"),
            "min_wave_pixels": mesh_eval.get("ocean_camera_lod_min_wave_pixels"),
        }
    finally:
        obj_eval.to_mesh_clear()


def topology_signature(snapshot):
    return (snapshot["vertices"], snapshot["faces"], snapshot["coordinates"])


def main():
    ocean_metrics.clear_scene()
    camera = ocean_metrics.make_camera_and_light(
        cam_location=(0.0, 0.0, 1600.0),
        cam_target=(0.0, 0.0, 0.0),
        lens=70.0,
        sun_energy=0.0,
    )
    obj = ocean_metrics.make_ocean_object(
        name="OceanRecoverableWaveLOD",
        geometry_mode="GENERATE",
        resolution=8,
        spatial_size=512,
        size=1.0,
        camera_lod=True,
        lod_levels=5,
        lod_pixel_error=0.5,
        lod_policy="RECOVERABLE_WAVES",
        lod_min_wave_pixels=4.0,
        lod_camera_full_spectrum_radius=0.001,
        time_value=1.0,
        choppiness=0.0,
        wind_velocity=1.0,
        wave_scale=0.0,
    )
    modifier = obj.modifiers["Ocean"]

    policy_rna = modifier.bl_rna.properties["lod_policy"]
    wave_pixels_rna = modifier.bl_rna.properties["lod_min_wave_pixels"]
    assert policy_rna.default == "PIXEL_ERROR"
    assert math.isclose(wave_pixels_rna.default, 4.0)
    assert math.isclose(wave_pixels_rna.hard_min, 2.0)
    assert math.isclose(wave_pixels_rna.hard_max, 64.0)
    assert math.isclose(wave_pixels_rna.soft_min, 2.0)
    assert math.isclose(wave_pixels_rna.soft_max, 16.0)

    modifier.lod_min_wave_pixels = 2.0
    low_threshold = evaluated_lod_snapshot(obj)
    assert low_threshold["policy"] == "RECOVERABLE_WAVES"
    assert math.isclose(low_threshold["min_wave_pixels"], 2.0)

    modifier.lod_min_wave_pixels = 8.0
    high_threshold = evaluated_lod_snapshot(obj)
    assert high_threshold["policy"] == "RECOVERABLE_WAVES"
    assert math.isclose(high_threshold["min_wave_pixels"], 8.0)
    assert high_threshold["vertices"] < low_threshold["vertices"], (
        "A larger projected-pixels-per-wavelength threshold should permit a strictly coarser "
        f"mesh in this fixture: P=2 {low_threshold['vertices']} verts, "
        f"P=8 {high_threshold['vertices']} verts"
    )

    modifier.lod_min_wave_pixels = 4.0
    camera.location = (0.0, 0.0, 800.0)
    ocean_metrics.look_at(camera, (0.0, 0.0, 0.0))
    low_camera = evaluated_lod_snapshot(obj)

    camera.location = (0.0, 0.0, 3200.0)
    ocean_metrics.look_at(camera, (0.0, 0.0, 0.0))
    high_camera = evaluated_lod_snapshot(obj)
    assert high_camera["vertices"] < low_camera["vertices"], (
        "Raising the camera should automatically permit a strictly coarser mesh in this fixture: "
        f"800m {low_camera['vertices']} verts, 3200m {high_camera['vertices']} verts"
    )

    modifier.lod_min_wave_pixels = 8.0
    camera.location = (0.0, 0.0, 1600.0)
    ocean_metrics.look_at(camera, (0.0, 0.0, 0.0))
    level_camera = evaluated_lod_snapshot(obj)
    ocean_metrics.look_at(camera, (400.0, 0.0, 0.0))
    pitched_camera = evaluated_lod_snapshot(obj)
    assert topology_signature(pitched_camera) != topology_signature(level_camera), (
        "Changing camera pitch must invalidate cached camera-LOD topology"
    )

    modifier.lod_min_wave_pixels = 4.0
    camera.location = (0.0, 0.0, 800.0)
    ocean_metrics.look_at(camera, (0.0, 0.0, 0.0))
    bpy.context.scene.render.resolution_percentage = 100
    full_resolution = evaluated_lod_snapshot(obj)
    bpy.context.scene.render.resolution_percentage = 25
    half_resolution = evaluated_lod_snapshot(obj)
    assert half_resolution["vertices"] < full_resolution["vertices"], (
        "Effective render resolution must automatically invalidate camera-LOD topology: "
        f"100% {full_resolution['vertices']} verts, 25% {half_resolution['vertices']} verts"
    )


if __name__ == "__main__":
    main()
