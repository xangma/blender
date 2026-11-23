# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Animated Ocean fields must agree between persistent and fresh stereo renders."""

from pathlib import Path
import sys

import bpy
import numpy as np

sys.path.append(str(Path(__file__).resolve().parent))
from ocean_camera_lod_supervised_foam import (
    arguments, find_view_file, read_image, set_material, setup, strip_legacy_attributes,
)


def render_frame(scene, frame, directory, label):
    scene.frame_set(frame)
    bpy.context.view_layer.update()
    scene.render.filepath = str(directory / (label + ".png"))
    result = bpy.ops.render.render(write_still=True)
    assert result == {'FINISHED'}, label
    return {eye: read_image(find_view_file(directory, label, eye, ".png"))[..., :3]
            for eye in ("left", "right")}


def main():
    args = arguments()
    directory = Path(args.outdir).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    scene, _cam, obj, mod = setup(args)
    mod.driver_add("time").driver.expression = "1.0 + frame / 15.0"

    for kind in ("foam", "spray"):
        set_material(obj, kind)
        scene.render.use_persistent_data = False
        fresh = render_frame(scene, 12, directory, kind + "_fresh")
        scene.render.use_persistent_data = True
        initial = render_frame(scene, 1, directory, kind + "_initial")
        sequence = render_frame(scene, 12, directory, kind + "_sequence")
        rewind = render_frame(scene, 1, directory, kind + "_rewind")
        for eye in fresh:
            assert np.abs(fresh[eye] - initial[eye]).mean() > 0.001, (kind, eye, "static fixture")
            np.testing.assert_array_equal(sequence[eye], fresh[eye], err_msg=f"{kind}/{eye} forward")
            np.testing.assert_array_equal(rewind[eye], initial[eye], err_msg=f"{kind}/{eye} rewind")

        # Force a render-resolution simulation and eliminate the corner-attribute fallback.
        scene.render.use_persistent_data = False
        mod.viewport_resolution = max(1, mod.resolution // 2)
        removal, tree = strip_legacy_attributes(obj)
        resized = render_frame(scene, 12, directory, kind + "_resized")
        for eye in fresh:
            np.testing.assert_array_equal(resized[eye], fresh[eye], err_msg=f"{kind}/{eye} resolution")
        obj.modifiers.remove(removal)
        bpy.data.node_groups.remove(tree)
        mod.viewport_resolution = mod.resolution


if __name__ == "__main__":
    main()
