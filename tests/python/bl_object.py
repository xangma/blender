# SPDX-FileCopyrightText: 2025 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later */

import unittest

import bpy
from mathutils import Vector


class ClosestPointOnMeshTest(unittest.TestCase):
    def test_function_finds_closest_point_successfully(self):
        """Test that attempting to find the closest point succeeds and returns the correct location."""

        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.mesh.primitive_cube_add()
        ret_val = bpy.context.scene.objects[0].closest_point_on_mesh(Vector((0.0, 0.0, 2.0)))
        self.assertTrue(ret_val[0])
        self.assertEqual(ret_val[1], Vector((0.0, 0.0, 1.0)))


class RemeshTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.ed.undo_push()

        bpy.ops.mesh.primitive_cube_add()
        bpy.ops.sculpt.sculptmode_toggle()

    def test_operator_remeshes_basic_cube(self):
        """Test that using the operator with default settings creates a mesh with the expected amount of vertices."""
        mesh = bpy.context.object.data
        mesh.remesh_voxel_size = 0.1

        ret_val = bpy.ops.object.voxel_remesh()

        self.assertEqual({'FINISHED'}, ret_val)

        num_vertices = mesh.attributes.domain_size('POINT')
        self.assertEqual(num_vertices, 2648)

    def test_operator_doesnt_run_with_0_voxel_size(self):
        """Test that using the operator returns an error to the user with a voxel size of 0."""
        mesh = bpy.context.object.data
        mesh.remesh_voxel_size = 0

        with self.assertRaises(RuntimeError):
            bpy.ops.object.voxel_remesh()


class CameraStereoMatrixTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.object.camera_add(location=(0.0, 0.0, 10.0))
        self.camera = bpy.context.object
        self.scene = bpy.context.scene
        self.scene.camera = self.camera
        self.scene.render.resolution_x = 640
        self.scene.render.resolution_y = 480
        self.scene.render.resolution_percentage = 100
        self.scene.render.pixel_aspect_x = 1.0
        self.scene.render.pixel_aspect_y = 1.0
        self.scene.render.use_multiview = True
        self.scene.render.views_format = 'STEREO_3D'

        self.camera.data.stereo.interocular_distance = 2.0
        self.camera.data.stereo.convergence_distance = 20.0
        self.camera.data.stereo.convergence_mode = 'OFFAXIS'
        self.camera.data.stereo.pivot = 'CENTER'

        for view in self.scene.render.views:
            view.use = view.name.lower() in {"left", "right"}

        bpy.context.view_layer.update()
        self.depsgraph = bpy.context.evaluated_depsgraph_get()

    def assertMatrixAlmostEqual(self, a, b, places=6):
        for row in range(4):
            for col in range(4):
                self.assertAlmostEqual(a[row][col], b[row][col], places=places)

    def test_calc_matrix_camera_view_name_empty_matches_legacy(self):
        legacy = self.camera.calc_matrix_camera(
            self.depsgraph,
            x=640,
            y=480,
            scale_x=1.0,
            scale_y=1.0,
        )
        explicit = self.camera.calc_matrix_camera(
            self.depsgraph,
            x=640,
            y=480,
            scale_x=1.0,
            scale_y=1.0,
            scene=self.scene,
            view_name="",
        )
        self.assertMatrixAlmostEqual(legacy, explicit)

    def test_calc_matrix_camera_stereo_views_differ(self):
        left = self.camera.calc_matrix_camera(
            self.depsgraph,
            x=640,
            y=480,
            scale_x=1.0,
            scale_y=1.0,
            scene=self.scene,
            view_name="left",
        )
        right = self.camera.calc_matrix_camera(
            self.depsgraph,
            x=640,
            y=480,
            scale_x=1.0,
            scale_y=1.0,
            scene=self.scene,
            view_name="right",
        )
        self.assertNotAlmostEqual(left[0][2], right[0][2], places=6)

    def test_calc_matrix_camera_model_stereo_views_differ(self):
        left = self.camera.calc_matrix_camera_model(
            self.depsgraph,
            scene=self.scene,
            view_name="left",
        )
        right = self.camera.calc_matrix_camera_model(
            self.depsgraph,
            scene=self.scene,
            view_name="right",
        )
        midpoint = (left.to_translation() + right.to_translation()) * 0.5
        baseline = (left.to_translation() - right.to_translation()).length

        self.assertAlmostEqual(midpoint.x, self.camera.matrix_world.translation.x, places=6)
        self.assertAlmostEqual(midpoint.y, self.camera.matrix_world.translation.y, places=6)
        self.assertAlmostEqual(midpoint.z, self.camera.matrix_world.translation.z, places=6)
        self.assertAlmostEqual(
            baseline,
            self.camera.data.stereo.interocular_distance,
            places=6,
        )

    def test_calc_matrix_camera_model_ignores_camera_scale(self):
        mono = self.camera.calc_matrix_camera_model(self.depsgraph)
        left = self.camera.calc_matrix_camera_model(
            self.depsgraph,
            scene=self.scene,
            view_name="left",
        )
        right = self.camera.calc_matrix_camera_model(
            self.depsgraph,
            scene=self.scene,
            view_name="right",
        )

        self.camera.scale = (2.5, 3.0, 4.5)
        bpy.context.view_layer.update()

        mono_scaled = self.camera.calc_matrix_camera_model(self.depsgraph)
        left_scaled = self.camera.calc_matrix_camera_model(
            self.depsgraph,
            scene=self.scene,
            view_name="left",
        )
        right_scaled = self.camera.calc_matrix_camera_model(
            self.depsgraph,
            scene=self.scene,
            view_name="right",
        )

        self.assertMatrixAlmostEqual(mono, mono_scaled)
        self.assertMatrixAlmostEqual(left, left_scaled)
        self.assertMatrixAlmostEqual(right, right_scaled)

    def test_calc_matrix_camera_rejects_unknown_view(self):
        with self.assertRaises(ValueError):
            self.camera.calc_matrix_camera(
                self.depsgraph,
                x=640,
                y=480,
                scale_x=1.0,
                scale_y=1.0,
                scene=self.scene,
                view_name="unknown",
            )

    def test_calc_matrix_camera_rejects_inactive_view(self):
        for view in self.scene.render.views:
            if view.name.lower() == "right":
                view.use = False

        with self.assertRaises(ValueError):
            self.camera.calc_matrix_camera(
                self.depsgraph,
                x=640,
                y=480,
                scale_x=1.0,
                scale_y=1.0,
                scene=self.scene,
                view_name="right",
            )

    def test_calc_matrix_camera_rejects_multiview_disabled_scene(self):
        self.scene.render.use_multiview = False

        with self.assertRaises(ValueError):
            self.camera.calc_matrix_camera(
                self.depsgraph,
                x=640,
                y=480,
                scale_x=1.0,
                scale_y=1.0,
                scene=self.scene,
                view_name="left",
            )


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main()
