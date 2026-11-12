# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Check physical Ocean controls against saved legacy storage and evaluated geometry.

Run with: blender -b --factory-startup --python-exit-code 1 --python physics_ocean_controls.py
"""

import json
import math
from pathlib import Path
import tempfile

import bpy
import numpy as np


def make_ocean(name):
    bpy.ops.mesh.primitive_plane_add()
    obj = bpy.context.object
    obj.name = name
    modifier = obj.modifiers.new("Ocean", "OCEAN")
    modifier.spectrum = "REALSEA_JONSWAP"
    modifier.geometry_mode = "GENERATE"
    modifier.use_camera_lod = False
    modifier.resolution = modifier.viewport_resolution = 8
    modifier.spatial_size = 500
    modifier.size = 1.0
    modifier.repeat_x = modifier.repeat_y = 1
    modifier.wind_velocity = 15.0
    modifier.depth = 1000.0
    modifier.choppiness = 0.0
    modifier.use_wave_scale = False
    modifier.wave_scale = 1.0
    modifier.realsea_fmin = 0.05
    modifier.realsea_fmax = 1.0
    modifier.realsea_use_spread = False
    modifier.fetch_jonswap = 100000.0
    modifier.random_seed = 0
    modifier.time = 0.0
    return obj, modifier


def close(actual, expected):
    assert math.isclose(actual, expected, rel_tol=1e-6, abs_tol=1e-6), (actual, expected)


def height_field(obj, modifier):
    bpy.context.view_layer.update()
    evaluated = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
    mesh = evaluated.to_mesh()
    try:
        n = modifier.viewport_resolution ** 2
        assert len(mesh.vertices) == (n + 1) ** 2
        coordinates = np.empty(3 * len(mesh.vertices), dtype=np.float32)
        mesh.vertices.foreach_get("co", coordinates)
        # Exclude duplicate periodic endpoints when measuring variance.
        heights = coordinates.reshape(n + 1, n + 1, 3)[:-1, :-1, 2].astype(np.float64)
        assert np.isfinite(heights).all()
        return heights
    finally:
        evaluated.to_mesh_clear()


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    obj, modifier = make_ocean("PhysicalGamma")
    prop = modifier.bl_rna.properties["realsea_peak_enhancement"]
    close(prop.hard_min, 1.0)
    close(prop.hard_max, 6.0)
    close(prop.default, 3.3)
    assert not prop.is_animatable

    # Reading an old zero must preserve storage and the exact default sea.
    assert modifier.sharpen_peak_jonswap == 0.0
    close(modifier.realsea_peak_enhancement, 3.3)
    assert modifier.sharpen_peak_jonswap == 0.0
    default_heights = height_field(obj, modifier)
    modifier.realsea_peak_enhancement = 3.3
    close(modifier.sharpen_peak_jonswap, 0.33)
    assert np.allclose(height_field(obj, modifier), default_heights, rtol=1e-5, atol=1e-5)

    # This is an effective geometry check: moving the physical slider upwards
    # must increase peak energy instead of the old zero-to-positive discontinuity.
    gammas = (1.0, 1.1, 2.0, 3.3, 5.9, 6.0)
    variances = []
    for gamma in gammas:
        modifier.realsea_peak_enhancement = gamma
        close(modifier.realsea_peak_enhancement, gamma)
        variances.append(float(np.var(height_field(obj, modifier))))
    assert all(b > a for a, b in zip(variances, variances[1:])), variances

    # Old raw values retain their existing effective meanings without migration.
    saved = []
    for name, spectrum, raw, effective in (
            ("SavedZero", "REALSEA_JONSWAP", 0.0, 3.3),
            ("SavedLow", "REALSEA_JONSWAP", 0.001, 1.0),
            ("SavedHigh", "REALSEA_JONSWAP", 0.9, 6.0),
            ("SavedLegacy", "JONSWAP", 0.9, 6.0)):
        _, old_modifier = make_ocean(name)
        old_modifier.resolution = old_modifier.viewport_resolution = 1
        old_modifier.spectrum = spectrum
        old_modifier.sharpen_peak_jonswap = raw
        close(old_modifier.realsea_peak_enhancement, effective)
        close(old_modifier.sharpen_peak_jonswap, raw)
        saved.append((name, spectrum, raw, effective))

    modifier.realsea_peak_enhancement = 4.2
    saved.append((obj.name, modifier.spectrum, 0.42, 4.2))
    with tempfile.TemporaryDirectory(prefix="ocean-controls-") as directory:
        path = str(Path(directory) / "ocean-controls.blend")
        bpy.ops.wm.save_as_mainfile(filepath=path)
        bpy.ops.wm.open_mainfile(filepath=path)
        for name, spectrum, raw, effective in saved:
            reopened = bpy.data.objects[name].modifiers["Ocean"]
            assert reopened.spectrum == spectrum
            close(reopened.sharpen_peak_jonswap, raw)
            close(reopened.realsea_peak_enhancement, effective)

    print(json.dumps({"gamma": gammas, "height_variance": variances, "saved_cases": len(saved)}))
    print("Ocean physical controls and legacy file compatibility: PASS")


if __name__ == "__main__":
    main()
