/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "bvh/bvh.h"

#include "device/device.h"

#include "scene/attribute.h"
#include "scene/camera.h"
#include "scene/geometry.h"
#include "scene/hair.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/osl.h"
#include "scene/pointcloud.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_nodes.h"

#include "util/log.h"
#include "util/progress.h"
#include "util/time.h"

CCL_NAMESPACE_BEGIN

static bool ocean_camera_lod_profile_enabled()
{
  const char *value = std::getenv("BLENDER_OCEAN_CAMERA_LOD_PROFILE");
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

static bool mesh_is_ocean_profile_target(const Mesh *mesh)
{
  return mesh != nullptr && mesh->ocean_modifier_active;
}

void GeometryManager::device_update_mesh(Device * /*unused*/,
                                         DeviceScene *dscene,
                                         Scene *scene,
                                         Progress &progress)
{
  const bool profile_enabled = ocean_camera_lod_profile_enabled();
  const double stage_start = profile_enabled ? time_dt() : 0.0;
  /* Count. */
  size_t tri_size = 0;

  size_t curve_size = 0;
  size_t curve_segment_size = 0;

  size_t point_size = 0;
  size_t ocean_mesh_count = 0;
  size_t ocean_vert_size = 0;
  size_t ocean_tri_size = 0;

  for (Geometry *geom : scene->geometry) {
    if (geom->is_mesh() || geom->is_volume()) {
      Mesh *mesh = static_cast<Mesh *>(geom);

      tri_size += mesh->num_triangles();
      if (mesh_is_ocean_profile_target(mesh)) {
        ocean_mesh_count++;
        ocean_vert_size += mesh->verts.size();
        ocean_tri_size += mesh->num_triangles();
      }
    }
    else if (geom->is_hair()) {
      Hair *hair = static_cast<Hair *>(geom);

      curve_size += hair->num_curves();
      curve_segment_size += hair->num_segments();
    }
    else if (geom->is_pointcloud()) {
      PointCloud *pointcloud = static_cast<PointCloud *>(geom);
      point_size += pointcloud->num_points();
    }
  }

  /* Fill in all the arrays. */
  if (tri_size != 0) {
    progress.set_status("Updating Mesh", "Computing normals");
    const double pack_start = profile_enabled ? time_dt() : 0.0;

    uint *tri_shader = dscene->tri_shader.alloc(tri_size);
    packed_uint3 *tri_vindex = dscene->tri_vindex.alloc(tri_size);

    const bool copy_all_data = dscene->tri_shader.need_realloc() ||
                               dscene->tri_vindex.need_realloc();

    for (Geometry *geom : scene->geometry) {
      if (geom->is_mesh() || geom->is_volume()) {
        Mesh *mesh = static_cast<Mesh *>(geom);
        const bool profile_mesh = profile_enabled && mesh_is_ocean_profile_target(mesh);
        const double mesh_pack_start = profile_mesh ? time_dt() : 0.0;

        if (mesh->shader_is_modified() || mesh->smooth_is_modified() ||
            mesh->triangles_is_modified() || copy_all_data)
        {
          mesh->pack_shaders(scene, &tri_shader[mesh->prim_offset]);
        }

        if (mesh->triangles_is_modified() || copy_all_data) {
          mesh->pack_triangles(&tri_vindex[mesh->prim_offset]);
        }

        if (progress.get_cancel()) {
          return;
        }

        if (profile_mesh) {
          LOG_INFO << "[OCEAN_CAMERA_LOD_PROFILE] object='" << mesh->name
                   << "' stage=cycles_device_update_mesh_pack mode="
                   << (mesh->ocean_camera_lod_active ? "camera_lod" : "dense_reference")
                   << " pack_s=" << (time_dt() - mesh_pack_start) << " verts="
                   << mesh->verts.size() << " tris=" << mesh->num_triangles() << " copy_all="
                   << int(copy_all_data) << " verts_modified=" << int(mesh->verts_is_modified())
                   << " triangles_modified=" << int(mesh->triangles_is_modified())
                   << " shader_modified=" << int(mesh->shader_is_modified());
        }
      }
    }

    /* vertex coordinates */
    progress.set_status("Updating Mesh", "Copying Mesh to device");
    const double copy_to_device_start = profile_enabled ? time_dt() : 0.0;

    dscene->tri_shader.copy_to_device_if_modified();
    dscene->tri_vindex.copy_to_device_if_modified();

    if (profile_enabled && ocean_mesh_count > 0) {
      LOG_INFO << "[OCEAN_CAMERA_LOD_PROFILE] object='scene' stage=cycles_device_update_mesh total_s="
               << (time_dt() - stage_start) << " pack_s=" << (time_dt() - pack_start)
               << " copy_to_device_s=" << (time_dt() - copy_to_device_start)
               << " ocean_meshes=" << ocean_mesh_count << " ocean_verts=" << ocean_vert_size
               << " ocean_tris=" << ocean_tri_size << " scene_verts=" << vert_size
               << " scene_tris=" << tri_size;
    }
  }

  if (curve_segment_size != 0) {
    progress.set_status("Updating Mesh", "Copying Curves to device");

    KernelCurve *curves = dscene->curves.alloc(curve_size);
    KernelCurveSegment *curve_segments = dscene->curve_segments.alloc(curve_segment_size);

    const bool copy_all_data = dscene->curves.need_realloc() ||
                               dscene->curve_segments.need_realloc();

    for (Geometry *geom : scene->geometry) {
      if (geom->is_hair()) {
        Hair *hair = static_cast<Hair *>(geom);

        const bool curve_data_modified = hair->curve_shader_is_modified() ||
                                         hair->curve_first_key_is_modified();

        if (!curve_data_modified && !copy_all_data) {
          continue;
        }

        hair->pack_curves(
            scene, &curves[hair->prim_offset], &curve_segments[hair->curve_segment_offset]);
        if (progress.get_cancel()) {
          return;
        }
      }
    }

    dscene->curves.copy_to_device_if_modified();
    dscene->curve_segments.copy_to_device_if_modified();
  }

  if (point_size != 0) {
    progress.set_status("Updating Mesh", "Copying Point clouds to device");

    uint *points_shader = dscene->points_shader.alloc(point_size);

    for (Geometry *geom : scene->geometry) {
      if (geom->is_pointcloud()) {
        PointCloud *pointcloud = static_cast<PointCloud *>(geom);
        pointcloud->pack(scene, &points_shader[pointcloud->prim_offset]);
        if (progress.get_cancel()) {
          return;
        }
      }
    }

    dscene->points_shader.copy_to_device();
  }
}

CCL_NAMESPACE_END
