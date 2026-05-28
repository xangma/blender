/* SPDX-FileCopyrightText: Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <algorithm>
#include <cstdarg>
#include <cfloat>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "BLI_color_types.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_listbase.h"
#include "BLI_listbase_wrapper.hh"
#include "BLI_path_utils.hh"
#include "BLI_task.h"
#include "BLI_time.h"
#include "BLI_utildefines.h"
#include "BLI_array.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_customdata_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_camera_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_camera.h"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_modifier.hh"
#include "BKE_ocean.h"
#include "BKE_scene.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_types.hh" /* For UI free bake operator. */

#include "DEG_depsgraph_build.hh"
#include "DEG_depsgraph_query.hh"

#include "MOD_ui_common.hh"

namespace blender {

#ifdef WITH_OCEANSIM
using blender::Array;
using blender::float2;
using blender::float3;
using blender::Span;
using blender::Vector;

static constexpr const char *OCEAN_ATTR_REF_COORD = "ocean_ref_coord";
static constexpr const char *OCEAN_ATTR_REF_UV = "ocean_ref_uv";
/* Geometry-band normal of the explicit mesh surface. Apparent/full-band residual normals remain
 * renderer-internal because they are view/support dependent. */
static constexpr const char *OCEAN_ATTR_GEOMETRY_NORMAL = "ocean_geometry_normal";
static constexpr const char *OCEAN_ATTR_GEOMETRY_SUPPORT_COV = "ocean_geometry_support_covariance";
static constexpr const char *OCEAN_ATTR_CAMERA_LOD_LEVEL = "ocean_camera_lod_level";
static constexpr const char *OCEAN_ATTR_CAMERA_LOD_SPLIT_LEVEL = "ocean_camera_lod_split_level";
static constexpr const char *OCEAN_ATTR_CAMERA_LOD_MORPH = "ocean_camera_lod_morph";
static constexpr const char *OCEAN_ATTR_CAMERA_LOD_RADIUS = "ocean_camera_lod_radius";
static constexpr const char *OCEAN_ATTR_CAMERA_LOD_LEAF_ID = "ocean_camera_lod_leaf_id";
static constexpr const char *OCEAN_ATTR_CAMERA_LOD_LEAF_LEVEL = "ocean_camera_lod_leaf_level";
static constexpr const char *OCEAN_ATTR_CAMERA_LOD_LEAF_SPLIT_LEVEL =
    "ocean_camera_lod_leaf_split_level";
static constexpr const char *OCEAN_ATTR_CAMERA_LOD_CELL_SIZE = "ocean_camera_lod_cell_size";
static constexpr const char *OCEAN_PROP_CAMERA_LOD_CONTRACT = "ocean_camera_lod_contract";
static constexpr const char *OCEAN_PROP_CAMERA_LOD_LAYOUT = "ocean_camera_lod_layout";
static constexpr const char *OCEAN_CAMERA_LOD_CONTRACT_ADAPTIVE_LEAF = "adaptive_leaf_v1";
static constexpr const char *OCEAN_CAMERA_LOD_LAYOUT_ADAPTIVE_LEAF = "adaptive_leaf";
static constexpr const char *OCEAN_SPLIT_DEBUG_ENV = "BLENDER_OCEAN_SPLIT_DEBUG";
static constexpr const char *OCEAN_CAMERA_LOD_PROFILE_ENV = "BLENDER_OCEAN_CAMERA_LOD_PROFILE";
static constexpr const char *OCEAN_LOD_POSITION_TOL_ENV = "BLENDER_OCEAN_LOD_POSITION_TOL_M";
static constexpr const char *OCEAN_LOD_REPROJ_TOL_ENV = "BLENDER_OCEAN_LOD_REPROJ_TOL_PX";
static constexpr const char *OCEAN_LOD_DEPTH_TOL_ENV = "BLENDER_OCEAN_LOD_DEPTH_TOL_M";
static constexpr const char *OCEAN_LOD_NORMAL_TOL_ENV = "BLENDER_OCEAN_LOD_NORMAL_TOL_DEG";
static constexpr const char *OCEAN_LOD_TEMPORAL_TOL_ENV = "BLENDER_OCEAN_LOD_TEMPORAL_TOL_PX";
static constexpr const char *OCEAN_LOD_GRAZING_TOL_ENV = "BLENDER_OCEAN_LOD_GRAZING_TOL_PX";
static constexpr const char *OCEAN_LOD_HARD_CAP_ENV = "BLENDER_OCEAN_LOD_HARD_CAP_SCALE";
static constexpr const char *OCEAN_LOD_FRAME_DT_ENV = "BLENDER_OCEAN_LOD_TEMPORAL_DT";
static constexpr float OCEAN_GRAVITY = 9.81f;

static float ocean_wave_scale_effective(const OceanModifierData *omd)
{
  return (omd->flag & MOD_OCEAN_USE_WAVE_SCALE) ? omd->wave_scale : 1.0f;
}

static void ocean_camera_lod_set_mesh_string_property(Mesh &mesh,
                                                       const char *name,
                                                       const char *value)
{
  IDProperty *properties = IDP_EnsureProperties(&mesh.id);
  if (properties == nullptr) {
    return;
  }

  IDProperty *property = IDP_GetPropertyTypeFromGroup(properties, name, IDP_STRING);
  if (property != nullptr) {
    IDP_AssignString(property, value);
    return;
  }

  if (IDP_GetPropertyFromGroup(properties, name) == nullptr) {
    IDP_AddToGroup(properties, blender::bke::idprop::create(name, value).release());
  }
}

static void init_cache_data(Object *ob, OceanModifierData *omd, const int resolution)
{
  const char *relbase = BKE_modifier_path_relbase_from_global(ob);
  const float wave_scale = ocean_wave_scale_effective(omd);

  omd->oceancache = BKE_ocean_init_cache(omd->cachepath,
                                         relbase,
                                         omd->bakestart,
                                         omd->bakeend,
                                         wave_scale,
                                         omd->chop_amount,
                                         omd->foam_coverage,
                                         omd->foam_fade,
                                         resolution);
}

static void simulate_ocean_modifier(OceanModifierData *omd)
{
  const float wave_scale = ocean_wave_scale_effective(omd);
  BKE_ocean_simulate(omd->ocean, omd->time, wave_scale, omd->chop_amount);
}

static OceanSplitSupport ocean_support_isotropic(const float wavelength)
{
  OceanSplitSupport support{};
  const float resolved_wavelength = std::max(wavelength, 1.0e-6f);
  const float sigma = 0.5f * resolved_wavelength;
  support.wavelength_x = resolved_wavelength;
  support.wavelength_z = resolved_wavelength;
  support.wavelength_major = resolved_wavelength;
  support.covariance[0] = sigma * sigma;
  support.covariance[1] = 0.0f;
  support.covariance[2] = sigma * sigma;
  return support;
}

static void ocean_support_finalize(OceanSplitSupport &support, const float fallback_wavelength)
{
  const float fallback_sigma = 0.5f * std::max(fallback_wavelength, 1.0e-6f);
  if (support.covariance[0] <= 0.0f) {
    support.covariance[0] = fallback_sigma * fallback_sigma;
  }
  if (support.covariance[2] <= 0.0f) {
    support.covariance[2] = fallback_sigma * fallback_sigma;
  }

  const float sigma_limit = sqrtf(
      std::max(support.covariance[0] * support.covariance[2], 0.0f));
  support.covariance[1] = std::clamp(support.covariance[1], -sigma_limit, sigma_limit);

  support.wavelength_x = 2.0f * sqrtf(std::max(support.covariance[0], 0.0f));
  support.wavelength_z = 2.0f * sqrtf(std::max(support.covariance[2], 0.0f));

  const float trace = support.covariance[0] + support.covariance[2];
  const float diff = support.covariance[0] - support.covariance[2];
  const float discriminant = sqrtf(
      std::max(diff * diff + 4.0f * support.covariance[1] * support.covariance[1], 0.0f));
  const float major_variance = std::max(0.0f, 0.5f * (trace + discriminant));
  support.wavelength_major = 2.0f * sqrtf(major_variance);
}

static OceanSplitSupport ocean_support_from_covariance(const float covariance[3],
                                                       const float fallback_wavelength)
{
  OceanSplitSupport support{};
  copy_v3_v3(support.covariance, covariance);
  ocean_support_finalize(support, fallback_wavelength);
  return support;
}

static float2 ocean_reference_plane_coord(const float3 &reference_co, const OceanModifierData *omd)
{
  const float size_inv = (fabsf(omd->size) > 1.0e-8f) ? (1.0f / omd->size) : 1.0f;
  return float2(reference_co.x * size_inv, reference_co.y * size_inv);
}

struct OceanCameraProjection {
  CameraParams params;
  float camera_to_object[4][4];
  float object_to_camera[4][4];
  int winx = 0;
  int winy = 0;
  bool is_valid = false;
};

struct OceanSplitMomentLevel {
  int split_level_index = 0;
  float wavelength = 0.0f;
  float cumulative_disp_variance[3] = {0.0f, 0.0f, 0.0f};
  float cumulative_slope_moment[3] = {0.0f, 0.0f, 0.0f};
  float omitted_disp_variance[3] = {0.0f, 0.0f, 0.0f};
  float omitted_slope_covariance[3] = {0.0f, 0.0f, 0.0f};
};

struct OceanLODObservableError {
  float position_m = 0.0f;
  float reprojection_px = 0.0f;
  float depth_m = 0.0f;
  float normal_radians = 0.0f;
  float temporal_px = 0.0f;
  float grazing_px = 0.0f;
};

struct OceanLODObservableTolerances {
  float position_m = 0.0f;
  float reprojection_px = 0.35f;
  float depth_m = 0.0f;
  float normal_radians = DEG2RADF(2.0f);
  float temporal_px = 0.25f;
  float grazing_px = 0.50f;
  float position_soft_scale = 1.5f;
  float hard_cap_scale = 2.0f;
  float temporal_dt = 1.0f / 24.0f;
};

struct OceanLODObservableErrorStats {
  OceanLODObservableError sampled_rms;
  OceanLODObservableError sampled_max;
  OceanLODObservableError margin_rms;
  OceanLODObservableError margin_max;
  OceanLODObservableError rms;
  OceanLODObservableError max;
  int sample_count = 0;
  int position_sample_count = 0;
  bool valid = false;
  bool position_gate_applied = false;
};

struct OceanLODRelevantFootprint {
  float2 min = float2(0.0f, 0.0f);
  float2 max = float2(0.0f, 0.0f);
  bool valid = false;
};

struct OceanCameraProjectionSet {
  Vector<OceanCameraProjection> projections;
  OceanLODRelevantFootprint visible_footprint;
  float2 support_center = float2(0.0f, 0.0f);
  float2 camera_anchor = float2(0.0f, 0.0f);
  bool have_support_center = false;
  bool have_camera_anchor = false;
  bool valid = false;
};

struct OceanLODObservableSample {
  OceanLODObservableError sampled;
  OceanLODObservableError margin;
};

struct OceanCameraLODRegionProfile {
  int call_count = 0;
  int sample_count = 0;
  int sample_eval_count = 0;
  double total_s = 0.0;
  double sample_eval_s = 0.0;
};

static thread_local bool g_ocean_camera_lod_region_profile_active = false;
static thread_local OceanCameraLODRegionProfile g_ocean_camera_lod_region_profile;

static void ocean_split_covariance_eigenvalues(const float covariance[3],
                                               float *r_minor_variance,
                                               float *r_major_variance);
static bool ocean_camera_projection_plane_intersection(const OceanCameraProjection &projection,
                                                       const float plane_x,
                                                       const float plane_y,
                                                       float r_point[3]);
static bool ocean_camera_projection_center(const OceanCameraProjection &projection, float2 &r_center);
static float ocean_camera_projection_pixel_sensitivity(const OceanCameraProjection &projection,
                                                       const float3 &point_object,
                                                       const float offset_m);

static float3 ocean_normalize_fallback(const float3 &value, const float3 &fallback)
{
  float3 normalized = value;
  return (normalize_v3(normalized) > 1.0e-8f) ? normalized : fallback;
}

static float ocean_env_float(const char *name, const float fallback)
{
  const char *value = BLI_getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char *end = nullptr;
  const float parsed = std::strtof(value, &end);
  return (end != value) ? parsed : fallback;
}

static bool ocean_camera_lod_profile_enabled()
{
  const char *value = BLI_getenv(OCEAN_CAMERA_LOD_PROFILE_ENV);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

static void ocean_camera_lod_profile_logv(const char *object_name,
                                          const char *stage,
                                          const char *fmt,
                                          va_list args)
{
  if (!ocean_camera_lod_profile_enabled()) {
    return;
  }

  printf("[OCEAN_CAMERA_LOD_PROFILE] object='%s' stage=%s ",
         object_name ? object_name : "<none>",
         stage);
  vprintf(fmt, args);
  printf("\n");
  fflush(stdout);
}

static void ocean_camera_lod_profile_logf(const char *object_name,
                                          const char *stage,
                                          const char *fmt,
                                          ...)
{
  va_list args;
  va_start(args, fmt);
  ocean_camera_lod_profile_logv(object_name, stage, fmt, args);
  va_end(args);
}

static OceanLODObservableTolerances ocean_camera_lod_tolerances(const float min_wavelength)
{
  OceanLODObservableTolerances tolerances;
  tolerances.position_m = 0.10f * std::max(min_wavelength, 1.0e-6f);
  tolerances.depth_m = 0.10f * std::max(min_wavelength, 1.0e-6f);
  tolerances.position_m = ocean_env_float(OCEAN_LOD_POSITION_TOL_ENV, tolerances.position_m);
  tolerances.reprojection_px = ocean_env_float(OCEAN_LOD_REPROJ_TOL_ENV, tolerances.reprojection_px);
  tolerances.depth_m = ocean_env_float(OCEAN_LOD_DEPTH_TOL_ENV, tolerances.depth_m);
  tolerances.normal_radians = DEG2RADF(
      ocean_env_float(OCEAN_LOD_NORMAL_TOL_ENV, RAD2DEGF(tolerances.normal_radians)));
  tolerances.temporal_px = ocean_env_float(OCEAN_LOD_TEMPORAL_TOL_ENV, tolerances.temporal_px);
  tolerances.grazing_px = ocean_env_float(OCEAN_LOD_GRAZING_TOL_ENV, tolerances.grazing_px);
  tolerances.hard_cap_scale = std::max(
      ocean_env_float(OCEAN_LOD_HARD_CAP_ENV, tolerances.hard_cap_scale), 1.0f);
  tolerances.temporal_dt = std::max(
      ocean_env_float(OCEAN_LOD_FRAME_DT_ENV, tolerances.temporal_dt), 1.0e-4f);
  return tolerances;
}

static int ocean_camera_lod_validation_mode(const OceanModifierData *omd)
{
  return (omd != nullptr && omd->lod_validation_mode == MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT) ?
             MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT :
             MOD_OCEAN_LOD_VALIDATE_CAMERA_OBSERVABLE;
}

static int ocean_camera_lod_usage_mode(const OceanModifierData *omd)
{
  return (omd != nullptr && omd->lod_usage_mode == MOD_OCEAN_LOD_USAGE_STEREO_DATASET) ?
             MOD_OCEAN_LOD_USAGE_STEREO_DATASET :
             MOD_OCEAN_LOD_USAGE_GENERAL_RENDER;
}

static bool ocean_camera_projection_pixel(const OceanCameraProjection &projection,
                                          const float3 &point_object,
                                          float2 &r_pixel,
                                          float *r_depth)
{
  float point_camera[3];
  mul_v3_m4v3(point_camera, projection.object_to_camera, point_object);

  float plane_x;
  float plane_y;
  if (projection.params.is_ortho) {
    plane_x = point_camera[0];
    plane_y = point_camera[1];
    if (r_depth != nullptr) {
      *r_depth = -point_camera[2];
    }
  }
  else {
    if (point_camera[2] >= -1.0e-6f) {
      return false;
    }
    const float depth = -point_camera[2];
    plane_x = (projection.params.clip_start * point_camera[0]) / depth;
    plane_y = (projection.params.clip_start * point_camera[1]) / depth;
    if (r_depth != nullptr) {
      *r_depth = depth;
    }
  }

  if (plane_x < projection.params.viewplane.xmin || plane_x > projection.params.viewplane.xmax ||
      plane_y < projection.params.viewplane.ymin || plane_y > projection.params.viewplane.ymax)
  {
    return false;
  }

  const float viewplane_width = projection.params.viewplane.xmax - projection.params.viewplane.xmin;
  const float viewplane_height = projection.params.viewplane.ymax - projection.params.viewplane.ymin;
  if (fabsf(viewplane_width) <= 1.0e-12f || fabsf(viewplane_height) <= 1.0e-12f) {
    return false;
  }

  r_pixel.x = ((plane_x - projection.params.viewplane.xmin) / viewplane_width) * projection.winx;
  r_pixel.y = ((plane_y - projection.params.viewplane.ymin) / viewplane_height) * projection.winy;
  return true;
}

static float3 ocean_camera_projection_origin_object(const OceanCameraProjection &projection)
{
  return float3(
      projection.camera_to_object[3][0], projection.camera_to_object[3][1], projection.camera_to_object[3][2]);
}

static void ocean_split_covariance_project_psd(float covariance[3])
{
  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(covariance, &minor_variance, &major_variance);

  const float angle = 0.5f * atan2f(2.0f * covariance[1], covariance[0] - covariance[2]);
  const float c = cosf(angle);
  const float s = sinf(angle);

  covariance[0] = c * c * major_variance + s * s * minor_variance;
  covariance[1] = c * s * (major_variance - minor_variance);
  covariance[2] = s * s * major_variance + c * c * minor_variance;
}

static Vector<OceanSplitMomentLevel> ocean_split_moment_levels(
    const OceanModifierData *omd, const OceanSplitRuntimeReadScope *read_scope)
{
  Vector<OceanSplitMomentLevel> levels;
  if (omd == nullptr || omd->ocean == nullptr) {
    return levels;
  }

  const int level_count = BKE_ocean_split_level_count_get(omd->ocean);
  levels.reserve(level_count);
  for (int level_index = 0; level_index < level_count; level_index++) {
    OceanSplitRuntimeLevel runtime_level{};
    const bool have_level = (read_scope != nullptr) ?
                                BKE_ocean_split_runtime_level_get_in_scope(
                                    read_scope, level_index, &runtime_level) :
                                BKE_ocean_split_runtime_level_get(omd->ocean, level_index, &runtime_level);
    if (!have_level) {
      break;
    }

    OceanSplitMomentLevel level{};
    level.split_level_index = level_index;
    level.wavelength = runtime_level.wavelength;
    copy_v3_v3(level.cumulative_disp_variance, runtime_level.cumulative_disp_variance);
    copy_v3_v3(level.cumulative_slope_moment, runtime_level.cumulative_slope_moment);
    levels.append(level);
  }

  if (levels.is_empty()) {
    return levels;
  }

  for (const int level_index : levels.index_range()) {
    for (int axis = 0; axis < 3; axis++) {
      levels[level_index].omitted_disp_variance[axis] = std::max(
          0.0f,
          levels[0].cumulative_disp_variance[axis] - levels[level_index].cumulative_disp_variance[axis]);
    }
    levels[level_index].omitted_slope_covariance[0] = levels[0].cumulative_slope_moment[0] -
                                                      levels[level_index].cumulative_slope_moment[0];
    levels[level_index].omitted_slope_covariance[1] = levels[0].cumulative_slope_moment[1] -
                                                      levels[level_index].cumulative_slope_moment[1];
    levels[level_index].omitted_slope_covariance[2] = levels[0].cumulative_slope_moment[2] -
                                                      levels[level_index].cumulative_slope_moment[2];
    ocean_split_covariance_project_psd(levels[level_index].omitted_slope_covariance);
  }

  return levels;
}

static void ocean_split_omitted_disp_variance(const Span<OceanSplitMomentLevel> levels,
                                              const int split_level_index,
                                              float r_variance[3])
{
  zero_v3(r_variance);
  if (levels.is_empty()) {
    return;
  }

  const int clamped_index = std::clamp(split_level_index, 0, int(levels.size()) - 1);
  copy_v3_v3(r_variance, levels[clamped_index].omitted_disp_variance);
}

static void ocean_split_omitted_slope_covariance(const Span<OceanSplitMomentLevel> levels,
                                                 const int split_level_index,
                                                 float r_covariance[3])
{
  zero_v3(r_covariance);
  if (levels.is_empty()) {
    return;
  }

  const int clamped_index = std::clamp(split_level_index, 0, int(levels.size()) - 1);
  copy_v3_v3(r_covariance, levels[clamped_index].omitted_slope_covariance);
}

static float ocean_base_domain_size(const OceanModifierData *omd)
{
  return std::max(omd->size * omd->spatial_size, 1.0e-6f);
}

static int ocean_repeat_x(const OceanModifierData *omd)
{
  return std::max(int(omd->repeat_x), 1);
}

static int ocean_repeat_y(const OceanModifierData *omd)
{
  return std::max(int(omd->repeat_y), 1);
}

static float2 ocean_repeated_domain_min(const OceanModifierData *omd)
{
  const float domain_size = ocean_base_domain_size(omd);
  return float2(-0.5f * domain_size, -0.5f * domain_size);
}

static float2 ocean_repeated_domain_max(const OceanModifierData *omd)
{
  const float domain_size = ocean_base_domain_size(omd);
  const float2 domain_min = ocean_repeated_domain_min(omd);
  return float2(domain_min.x + float(ocean_repeat_x(omd)) * domain_size,
                domain_min.y + float(ocean_repeat_y(omd)) * domain_size);
}

static float2 ocean_reference_uv_from_coord(const float2 &coord, const OceanModifierData *omd)
{
  const float domain_size = ocean_base_domain_size(omd);
  return float2((coord.x / domain_size) + 0.5f, (coord.y / domain_size) + 0.5f);
}

static bool ocean_split_sample_geometry_level_object(
    const OceanModifierData *omd,
    const OceanSplitRuntimeReadScope *read_scope,
    const int split_level_index,
    const float2 &coord,
    float3 &r_position_object,
    float3 &r_normal_object)
{
  const float2 uv = ocean_reference_uv_from_coord(coord, omd);
  float displacement[3] = {0.0f, 0.0f, 0.0f};
  float normal[3] = {0.0f, 1.0f, 0.0f};
  const bool have_sample = (read_scope != nullptr) ?
                               BKE_ocean_split_runtime_sample_level_in_scope(
                                   read_scope, split_level_index, uv.x, uv.y, displacement, normal) :
                               BKE_ocean_split_runtime_sample_level(
                                   omd->ocean, split_level_index, uv.x, uv.y, displacement, normal);
  if (!have_sample)
  {
    return false;
  }

  r_position_object = float3(coord.x + displacement[0], coord.y + displacement[2], displacement[1]);
  r_normal_object = float3(normal[0], normal[2], normal[1]);
  normalize_v3(r_normal_object);
  return true;
}

static OceanLODObservableError ocean_camera_lod_blind_spot_margin(
    const OceanCameraProjectionSet &projection_set,
    const OceanModifierData *omd,
    const Span<OceanSplitMomentLevel> levels,
    const OceanLODObservableTolerances &tolerances,
    const int split_level_index,
    const float blind_spot_radius,
    const float3 &reference_position_object,
    const float3 &reference_normal_object,
    const int usage_mode)
{
  OceanLODObservableError margin{};
  if (blind_spot_radius <= 1.0e-6f || levels.is_empty()) {
    return margin;
  }

  float omitted_slope_covariance[3];
  ocean_split_omitted_slope_covariance(levels, split_level_index, omitted_slope_covariance);
  float omitted_disp_variance[3];
  ocean_split_omitted_disp_variance(levels, split_level_index, omitted_disp_variance);

  const float slope_energy = std::max(omitted_slope_covariance[0] + omitted_slope_covariance[2], 0.0f);
  const float slope_rms = sqrtf(slope_energy);
  const float total_disp_rms = sqrtf(std::max(
      omitted_disp_variance[0] + omitted_disp_variance[1] + omitted_disp_variance[2], 0.0f));
  const float vertical_disp_rms = sqrtf(std::max(omitted_disp_variance[1], 0.0f));
  const float local_position_margin = std::min(total_disp_rms, slope_rms * blind_spot_radius);

  const float min_wavelength = std::max(BKE_ocean_split_min_wavelength_get(omd->ocean), 1.0e-6f);
  const int clamped_index = std::clamp(split_level_index, 0, int(levels.size()) - 1);
  const float k_min = (2.0f * float(M_PI)) / std::max(levels[clamped_index].wavelength, min_wavelength);
  const float k_max = (2.0f * float(M_PI)) / min_wavelength;
  const float k_eff = (vertical_disp_rms > 1.0e-8f) ? (slope_rms / vertical_disp_rms) : k_max;
  const float k_clamped = std::clamp(k_eff, k_min, k_max);
  const float omega_eff = sqrtf(OCEAN_GRAVITY * k_clamped);
  const float curvature_bound = slope_rms * k_clamped;

  margin.position_m = local_position_margin;
  if (usage_mode != MOD_OCEAN_LOD_USAGE_STEREO_DATASET) {
    margin.normal_radians = atanf(curvature_bound * blind_spot_radius);
  }

  const float projection_offset = std::max(0.25f * blind_spot_radius, 0.05f * min_wavelength);
  for (const OceanCameraProjection &projection : projection_set.projections) {
    const float pixel_sensitivity = ocean_camera_projection_pixel_sensitivity(
        projection, reference_position_object, projection_offset);
    const float reprojection_px = pixel_sensitivity * local_position_margin;
    margin.reprojection_px = std::max(margin.reprojection_px, reprojection_px);
    margin.depth_m = std::max(margin.depth_m, local_position_margin);
    if (usage_mode != MOD_OCEAN_LOD_USAGE_STEREO_DATASET) {
      margin.temporal_px = std::max(
          margin.temporal_px, reprojection_px * omega_eff * tolerances.temporal_dt);

      const float3 camera_origin = ocean_camera_projection_origin_object(projection);
      const float3 view_dir = ocean_normalize_fallback(
          camera_origin - reference_position_object, float3(0.0f, 0.0f, 1.0f));
      const float grazing = std::max(
          fabsf((reference_normal_object.x * view_dir.x) + (reference_normal_object.y * view_dir.y) +
                (reference_normal_object.z * view_dir.z)),
          0.15f);
      margin.grazing_px = std::max(margin.grazing_px, reprojection_px / grazing);
    }
  }
  return margin;
}

static OceanLODObservableSample ocean_camera_lod_sample_error_from_reference(
    const OceanCameraProjectionSet &projection_set,
    const OceanModifierData *omd,
    const Span<OceanSplitMomentLevel> levels,
    const OceanLODObservableTolerances &tolerances,
    const int split_level_index,
    const float3 &reference_position_object,
    const float3 &reference_normal_object,
    const float3 &position_object,
    const float3 &normal_object,
    const float blind_spot_radius,
    const int usage_mode)
{
  OceanLODObservableSample sample{};
  const float3 position_delta = reference_position_object - position_object;
  sample.sampled.position_m = len_v3v3(reference_position_object, position_object);
  for (const OceanCameraProjection &projection : projection_set.projections) {
    float2 candidate_pixel;
    float2 reference_pixel;
    float candidate_depth = 0.0f;
    float reference_depth = 0.0f;
    const bool candidate_visible = ocean_camera_projection_pixel(
        projection, position_object, candidate_pixel, &candidate_depth);
    const bool reference_visible = ocean_camera_projection_pixel(
        projection, reference_position_object, reference_pixel, &reference_depth);

    if (candidate_visible && reference_visible) {
      sample.sampled.reprojection_px = std::max(
          sample.sampled.reprojection_px, len_v2v2(candidate_pixel, reference_pixel));
      sample.sampled.depth_m = std::max(
          sample.sampled.depth_m, fabsf(candidate_depth - reference_depth));
    }
    else if (candidate_visible != reference_visible) {
      sample.sampled.reprojection_px = std::max(
          sample.sampled.reprojection_px, float(std::max(projection.winx, projection.winy)));
      sample.sampled.depth_m = std::max(
          sample.sampled.depth_m,
          std::max(sample.sampled.position_m, fabsf(candidate_depth - reference_depth)));
    }
  }

  float omitted_slope_covariance[3];
  ocean_split_omitted_slope_covariance(levels, split_level_index, omitted_slope_covariance);
  sample.sampled.normal_radians = angle_normalized_v3v3(reference_normal_object, normal_object);

  float omitted_disp_variance[3];
  ocean_split_omitted_disp_variance(levels, split_level_index, omitted_disp_variance);
  const float slope_energy = std::max(omitted_slope_covariance[0] + omitted_slope_covariance[2], 0.0f);
  const float slope_rms = sqrtf(slope_energy);
  const float vertical_disp_rms = std::max(fabsf(position_delta.z),
                                           sqrtf(std::max(omitted_disp_variance[1], 0.0f)));
  const float min_wavelength = std::max(BKE_ocean_split_min_wavelength_get(omd->ocean), 1.0e-6f);
  const float k_min = (2.0f * float(M_PI)) / std::max(
                                              levels[std::clamp(split_level_index, 0, int(levels.size()) - 1)]
                                                  .wavelength,
                                              min_wavelength);
  const float k_max = (2.0f * float(M_PI)) / min_wavelength;
  const float k_eff = (vertical_disp_rms > 1.0e-8f) ? (slope_rms / vertical_disp_rms) : k_max;
  const float omega_eff = sqrtf(OCEAN_GRAVITY * std::clamp(k_eff, k_min, k_max));
  if (usage_mode != MOD_OCEAN_LOD_USAGE_STEREO_DATASET) {
    sample.sampled.temporal_px = sample.sampled.reprojection_px * omega_eff * tolerances.temporal_dt;
    for (const OceanCameraProjection &projection : projection_set.projections) {
      const float3 camera_origin = ocean_camera_projection_origin_object(projection);
      const float3 view_dir = ocean_normalize_fallback(
          camera_origin - reference_position_object, float3(0.0f, 0.0f, 1.0f));
      const float grazing = std::max(
          fabsf((reference_normal_object.x * view_dir.x) + (reference_normal_object.y * view_dir.y) +
                (reference_normal_object.z * view_dir.z)),
          0.15f);
      sample.sampled.grazing_px = std::max(
          sample.sampled.grazing_px, sample.sampled.reprojection_px / grazing);
    }
  }
  sample.margin = ocean_camera_lod_blind_spot_margin(projection_set,
                                                     omd,
                                                     levels,
                                                     tolerances,
                                                     split_level_index,
                                                     blind_spot_radius,
                                                     reference_position_object,
                                                     reference_normal_object,
                                                     usage_mode);
  return sample;
}

static bool ocean_camera_lod_error_within_tolerance(const OceanLODObservableErrorStats &stats,
                                                    const OceanLODObservableTolerances &tolerances,
                                                    const int validation_mode,
                                                    const int usage_mode)
{
  if (!stats.valid || stats.sample_count <= 0) {
    return false;
  }

  const auto within_metric = [&](const float rms_value, const float max_value, const float tolerance) {
    return rms_value <= tolerance && max_value <= tolerance * tolerances.hard_cap_scale;
  };

  const bool stereo_dataset = usage_mode == MOD_OCEAN_LOD_USAGE_STEREO_DATASET;
  const bool validate_render_observables = !stereo_dataset;
  bool observables_ok = within_metric(
                            stats.rms.reprojection_px,
                            stats.max.reprojection_px,
                            tolerances.reprojection_px) &&
                        within_metric(stats.rms.depth_m, stats.max.depth_m, tolerances.depth_m);
  observables_ok = observables_ok &&
                   within_metric(stats.rms.normal_radians,
                                 stats.max.normal_radians,
                                 tolerances.normal_radians);
  if (validate_render_observables) {
    observables_ok = observables_ok &&
                     within_metric(stats.rms.temporal_px,
                                   stats.max.temporal_px,
                                   tolerances.temporal_px) &&
                     within_metric(stats.rms.grazing_px, stats.max.grazing_px, tolerances.grazing_px);
  }
  if (!observables_ok) {
    return false;
  }

  if (!stats.position_gate_applied) {
    return true;
  }
  if (stats.position_sample_count <= 0) {
    return false;
  }

  if (validation_mode == MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT) {
    return within_metric(stats.rms.position_m, stats.max.position_m, tolerances.position_m);
  }

  const float soft_position_tol = tolerances.position_m * tolerances.position_soft_scale;
  return within_metric(stats.rms.position_m, stats.max.position_m, soft_position_tol);
}

static void ocean_lod_relevant_footprint_expand(OceanLODRelevantFootprint &io_union,
                                                const OceanLODRelevantFootprint &footprint)
{
  if (!footprint.valid) {
    return;
  }

  if (!io_union.valid) {
    io_union = footprint;
    return;
  }

  io_union.min.x = std::min(io_union.min.x, footprint.min.x);
  io_union.min.y = std::min(io_union.min.y, footprint.min.y);
  io_union.max.x = std::max(io_union.max.x, footprint.max.x);
  io_union.max.y = std::max(io_union.max.y, footprint.max.y);
}

static void ocean_lod_relevant_footprint_grow(OceanLODRelevantFootprint &io_footprint,
                                              const float amount,
                                              const float2 &domain_min,
                                              const float2 &domain_max)
{
  if (!io_footprint.valid || amount <= 0.0f) {
    return;
  }

  io_footprint.min.x = std::max(io_footprint.min.x - amount, domain_min.x);
  io_footprint.min.y = std::max(io_footprint.min.y - amount, domain_min.y);
  io_footprint.max.x = std::min(io_footprint.max.x + amount, domain_max.x);
  io_footprint.max.y = std::min(io_footprint.max.y + amount, domain_max.y);
}

static float ocean_camera_lod_projection_footprint_buffer(const Span<OceanSplitMomentLevel> levels)
{
  if (levels.is_empty()) {
    return 0.0f;
  }

  const OceanSplitMomentLevel &full_spectrum = levels.first();
  const float horizontal_variance = std::max(
      full_spectrum.cumulative_disp_variance[0] + full_spectrum.cumulative_disp_variance[2], 0.0f);
  return 4.0f * sqrtf(horizontal_variance);
}

static bool ocean_camera_projection_init(const Scene *scene,
                                         const Object *object,
                                         const Object *camera,
                                         const char *view_name,
                                         OceanCameraProjection &r_projection)
{
  if (scene == nullptr || object == nullptr || camera == nullptr || camera->data == nullptr) {
    return false;
  }

  BKE_render_resolution(&scene->r, false, &r_projection.winx, &r_projection.winy);
  if (r_projection.winx <= 0 || r_projection.winy <= 0) {
    return false;
  }

  BKE_camera_params_init(&r_projection.params);
  BKE_camera_params_from_object(&r_projection.params, camera);
  BKE_camera_multiview_params(&scene->r, &r_projection.params, camera, view_name);
  BKE_camera_params_compute_viewplane(
      &r_projection.params, r_projection.winx, r_projection.winy, scene->r.xasp, scene->r.yasp);

  float camera_model_matrix[4][4];
  BKE_camera_multiview_model_matrix(&scene->r, camera, view_name, camera_model_matrix);

  float world_to_object[4][4];
  if (!invert_m4_m4(world_to_object, object->object_to_world().ptr())) {
    return false;
  }

  mul_m4_m4m4(r_projection.camera_to_object, world_to_object, camera_model_matrix);
  if (!invert_m4_m4(r_projection.object_to_camera, r_projection.camera_to_object)) {
    return false;
  }

  r_projection.is_valid = true;
  return true;
}

static bool ocean_camera_lod_has_stereo_dataset_views(const Scene *scene)
{
  return scene != nullptr && scene->r.views_format == SCE_VIEWS_FORMAT_STEREO_3D &&
         BKE_scene_multiview_is_stereo3d(&scene->r);
}

static OceanCameraProjectionSet ocean_camera_projection_set_init(const Scene *scene,
                                                                 const Object *object,
                                                                 const Object *camera,
                                                                 const int usage_mode)
{
  OceanCameraProjectionSet projection_set;
  if (scene == nullptr || object == nullptr || camera == nullptr) {
    return projection_set;
  }

  float2 center_sum(0.0f, 0.0f);
  float2 origin_sum(0.0f, 0.0f);
  int center_count = 0;
  int origin_count = 0;

  auto append_projection = [&](const char *view_name) {
    const Object *view_camera = BKE_camera_multiview_render(
        scene, const_cast<Object *>(camera), view_name);
    if (view_camera == nullptr) {
      view_camera = camera;
    }

    OceanCameraProjection projection{};
    if (!ocean_camera_projection_init(scene, object, view_camera, view_name, projection)) {
      return;
    }

    projection_set.projections.append(projection);

    float2 center;
    if (ocean_camera_projection_center(projection, center)) {
      center_sum += center;
      center_count++;
    }

    const float3 origin = ocean_camera_projection_origin_object(projection);
    origin_sum += float2(origin.x, origin.y);
    origin_count++;
  };

  if (usage_mode == MOD_OCEAN_LOD_USAGE_STEREO_DATASET) {
    if (!ocean_camera_lod_has_stereo_dataset_views(scene)) {
      return projection_set;
    }

    projection_set.projections.reserve(2);
    append_projection(STEREO_LEFT_NAME);
    append_projection(STEREO_RIGHT_NAME);
  }
  else {
    const int view_count = std::max(BKE_scene_multiview_num_views_get(&scene->r), 1);
    projection_set.projections.reserve(view_count);
    for (int view_id = 0; view_id < view_count; view_id++) {
      const char *view_name = ((scene->r.scemode & R_MULTIVIEW) != 0) ?
                                  BKE_scene_multiview_render_view_name_get(&scene->r, view_id) :
                                  "";
      append_projection(view_name);
    }
  }

  projection_set.valid = !projection_set.projections.is_empty();
  if (center_count > 0) {
    projection_set.support_center = center_sum / float(center_count);
    projection_set.have_support_center = true;
  }
  if (origin_count > 0) {
    projection_set.camera_anchor = origin_sum / float(origin_count);
    projection_set.have_camera_anchor = true;
  }
  return projection_set;
}

static bool ocean_camera_projection_ray_to_object(const OceanCameraProjection &projection,
                                                  const float plane_x,
                                                  const float plane_y,
                                                  float r_origin[3],
                                                  float r_direction[3])
{
  float ray_origin_camera[3];
  float ray_direction_camera[3];

  if (projection.params.is_ortho) {
    ray_origin_camera[0] = plane_x;
    ray_origin_camera[1] = plane_y;
    ray_origin_camera[2] = 0.0f;
    ray_direction_camera[0] = 0.0f;
    ray_direction_camera[1] = 0.0f;
    ray_direction_camera[2] = -1.0f;
  }
  else {
    zero_v3(ray_origin_camera);
    ray_direction_camera[0] = plane_x;
    ray_direction_camera[1] = plane_y;
    ray_direction_camera[2] = -projection.params.clip_start;
  }

  mul_v3_m4v3(r_origin, projection.camera_to_object, ray_origin_camera);
  copy_v3_v3(r_direction, ray_direction_camera);
  mul_mat3_m4_v3(projection.camera_to_object, r_direction);

  return normalize_v3(r_direction) > 1.0e-8f;
}

static bool ocean_camera_projection_plane_intersection(const OceanCameraProjection &projection,
                                                       const float plane_x,
                                                       const float plane_y,
                                                       float r_point[3])
{
  float ray_origin[3];
  float ray_direction[3];
  if (!ocean_camera_projection_ray_to_object(
          projection, plane_x, plane_y, ray_origin, ray_direction))
  {
    return false;
  }

  if (fabsf(ray_direction[2]) <= 1.0e-8f) {
    return false;
  }

  const float t = -ray_origin[2] / ray_direction[2];
  if (t < 0.0f) {
    return false;
  }

  madd_v3_v3v3fl(r_point, ray_origin, ray_direction, t);
  return true;
}

static bool ocean_camera_projection_center(const OceanCameraProjection &projection, float2 &r_center)
{
  const float plane_x = 0.5f * (projection.params.viewplane.xmin + projection.params.viewplane.xmax);
  const float plane_y = 0.5f * (projection.params.viewplane.ymin + projection.params.viewplane.ymax);
  float intersection[3];
  if (!ocean_camera_projection_plane_intersection(projection, plane_x, plane_y, intersection)) {
    return false;
  }
  r_center = float2(intersection[0], intersection[1]);
  return true;
}

static bool ocean_aabb_intersects(const float2 &min_a,
                                  const float2 &max_a,
                                  const float2 &min_b,
                                  const float2 &max_b)
{
  return !(max_a.x < min_b.x || max_b.x < min_a.x || max_a.y < min_b.y || max_b.y < min_a.y);
}

enum class OceanCameraProjectionClipPlane {
  XMin,
  XMax,
  YMin,
  YMax,
  Front,
};

static float ocean_camera_projection_clip_value(const OceanCameraProjection &projection,
                                                const OceanCameraProjectionClipPlane plane,
                                                const float2 &coord)
{
  float point_camera[3];
  const float point_object[3] = {coord.x, coord.y, 0.0f};
  mul_v3_m4v3(point_camera, projection.object_to_camera, point_object);

  if (plane == OceanCameraProjectionClipPlane::Front) {
    return projection.params.is_ortho ? 1.0f : (-point_camera[2] - 1.0e-6f);
  }

  if (projection.params.is_ortho) {
    switch (plane) {
      case OceanCameraProjectionClipPlane::XMin:
        return point_camera[0] - projection.params.viewplane.xmin;
      case OceanCameraProjectionClipPlane::XMax:
        return projection.params.viewplane.xmax - point_camera[0];
      case OceanCameraProjectionClipPlane::YMin:
        return point_camera[1] - projection.params.viewplane.ymin;
      case OceanCameraProjectionClipPlane::YMax:
        return projection.params.viewplane.ymax - point_camera[1];
      case OceanCameraProjectionClipPlane::Front:
        break;
    }
    return -1.0f;
  }

  const float clip_start = projection.params.clip_start;
  switch (plane) {
    case OceanCameraProjectionClipPlane::XMin:
      return (clip_start * point_camera[0]) + (projection.params.viewplane.xmin * point_camera[2]);
    case OceanCameraProjectionClipPlane::XMax:
      return -((clip_start * point_camera[0]) + (projection.params.viewplane.xmax * point_camera[2]));
    case OceanCameraProjectionClipPlane::YMin:
      return (clip_start * point_camera[1]) + (projection.params.viewplane.ymin * point_camera[2]);
    case OceanCameraProjectionClipPlane::YMax:
      return -((clip_start * point_camera[1]) + (projection.params.viewplane.ymax * point_camera[2]));
    case OceanCameraProjectionClipPlane::Front:
      break;
  }
  return -1.0f;
}

static void ocean_camera_projection_clip_polygon(const OceanCameraProjection &projection,
                                                 const OceanCameraProjectionClipPlane plane,
                                                 Vector<float2> &io_polygon)
{
  if (io_polygon.is_empty()) {
    return;
  }

  Vector<float2> clipped;
  clipped.reserve(io_polygon.size() + 1);

  float2 previous = io_polygon.last();
  float previous_value = ocean_camera_projection_clip_value(projection, plane, previous);
  bool previous_inside = previous_value >= 0.0f;

  for (const float2 &current : io_polygon) {
    const float current_value = ocean_camera_projection_clip_value(projection, plane, current);
    const bool current_inside = current_value >= 0.0f;

    if (current_inside != previous_inside) {
      const float denom = previous_value - current_value;
      const float factor = (fabsf(denom) > 1.0e-12f) ?
                               clamp_f(previous_value / denom, 0.0f, 1.0f) :
                               0.0f;
      clipped.append(previous + ((current - previous) * factor));
    }
    if (current_inside) {
      clipped.append(current);
    }

    previous = current;
    previous_value = current_value;
    previous_inside = current_inside;
  }

  io_polygon = std::move(clipped);
}

static bool ocean_camera_projection_visible_footprint(const OceanCameraProjection &projection,
                                                      const float2 &domain_min,
                                                      const float2 &domain_max,
                                                      OceanLODRelevantFootprint &r_footprint)
{
  r_footprint = OceanLODRelevantFootprint{};

  Vector<float2> polygon;
  polygon.append(domain_min);
  polygon.append(float2(domain_max.x, domain_min.y));
  polygon.append(domain_max);
  polygon.append(float2(domain_min.x, domain_max.y));

  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::Front, polygon);
  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::XMin, polygon);
  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::XMax, polygon);
  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::YMin, polygon);
  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::YMax, polygon);

  if (polygon.is_empty()) {
    return false;
  }

  float2 min_coord = domain_max;
  float2 max_coord = domain_min;
  for (const float2 &coord : polygon) {
    min_coord.x = std::min(min_coord.x, coord.x);
    min_coord.y = std::min(min_coord.y, coord.y);
    max_coord.x = std::max(max_coord.x, coord.x);
    max_coord.y = std::max(max_coord.y, coord.y);
  }

  r_footprint.min = min_coord;
  r_footprint.max = max_coord;
  r_footprint.valid = true;
  return true;
}

static bool ocean_camera_projection_region_clip_polygon(const OceanCameraProjection &projection,
                                                        const float2 &region_min,
                                                        const float2 &region_max,
                                                        Vector<float2> &r_polygon)
{
  r_polygon.clear();
  if (region_max.x < region_min.x || region_max.y < region_min.y) {
    return false;
  }

  r_polygon.append(region_min);
  r_polygon.append(float2(region_max.x, region_min.y));
  r_polygon.append(region_max);
  r_polygon.append(float2(region_min.x, region_max.y));

  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::Front, r_polygon);
  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::XMin, r_polygon);
  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::XMax, r_polygon);
  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::YMin, r_polygon);
  ocean_camera_projection_clip_polygon(
      projection, OceanCameraProjectionClipPlane::YMax, r_polygon);

  return !r_polygon.is_empty();
}

static bool ocean_camera_projection_region_intersects_fast(const OceanCameraProjection &projection,
                                                           const float2 &region_min,
                                                           const float2 &region_max)
{
  if (region_max.x < region_min.x || region_max.y < region_min.y) {
    return false;
  }

  float2 src[16] = {
      region_min,
      float2(region_max.x, region_min.y),
      region_max,
      float2(region_min.x, region_max.y),
  };
  float2 dst[16];
  int src_len = 4;

  const OceanCameraProjectionClipPlane planes[] = {
      OceanCameraProjectionClipPlane::Front,
      OceanCameraProjectionClipPlane::XMin,
      OceanCameraProjectionClipPlane::XMax,
      OceanCameraProjectionClipPlane::YMin,
      OceanCameraProjectionClipPlane::YMax,
  };

  for (const OceanCameraProjectionClipPlane plane : planes) {
    if (src_len == 0) {
      return false;
    }

    int dst_len = 0;
    float2 previous = src[src_len - 1];
    float previous_value = ocean_camera_projection_clip_value(projection, plane, previous);
    bool previous_inside = previous_value >= 0.0f;

    for (int i = 0; i < src_len; i++) {
      const float2 current = src[i];
      const float current_value = ocean_camera_projection_clip_value(projection, plane, current);
      const bool current_inside = current_value >= 0.0f;

      if (current_inside != previous_inside) {
        if (dst_len == 16) {
          return true;
        }
        const float denom = previous_value - current_value;
        const float factor = (fabsf(denom) > 1.0e-12f) ?
                                 clamp_f(previous_value / denom, 0.0f, 1.0f) :
                                 0.0f;
        dst[dst_len++] = previous + ((current - previous) * factor);
      }
      if (current_inside) {
        if (dst_len == 16) {
          return true;
        }
        dst[dst_len++] = current;
      }

      previous = current;
      previous_value = current_value;
      previous_inside = current_inside;
    }

    for (int i = 0; i < dst_len; i++) {
      src[i] = dst[i];
    }
    src_len = dst_len;
  }

  return src_len > 0;
}

static bool ocean_camera_projection_region_intersects(const OceanCameraProjection &projection,
                                                      const float2 &region_min,
                                                      const float2 &region_max)
{
  return ocean_camera_projection_region_intersects_fast(projection, region_min, region_max);
}

static float ocean_camera_projection_pixel_sensitivity(const OceanCameraProjection &projection,
                                                       const float3 &point_object,
                                                       const float offset_m)
{
  if (offset_m <= 1.0e-6f) {
    return 0.0f;
  }

  float2 base_pixel;
  if (!ocean_camera_projection_pixel(projection, point_object, base_pixel, nullptr)) {
    return 0.0f;
  }

  const float3 offsets[3] = {
      float3(offset_m, 0.0f, 0.0f),
      float3(0.0f, offset_m, 0.0f),
      float3(0.0f, 0.0f, offset_m),
  };

  float sensitivity = 0.0f;
  for (const float3 &offset : offsets) {
    float2 offset_pixel;
    if (!ocean_camera_projection_pixel(projection, point_object + offset, offset_pixel, nullptr)) {
      continue;
    }
    sensitivity = std::max(sensitivity, len_v2v2(base_pixel, offset_pixel) / offset_m);
  }
  return sensitivity;
}

static float ocean_camera_projection_planar_pixel_sensitivity(
    const OceanCameraProjection &projection,
    const float3 &point_object,
    const float offset_m)
{
  if (offset_m <= 1.0e-6f) {
    return 0.0f;
  }

  float2 base_pixel;
  if (!ocean_camera_projection_pixel(projection, point_object, base_pixel, nullptr)) {
    return 0.0f;
  }

  const float3 offsets[2] = {
      float3(offset_m, 0.0f, 0.0f),
      float3(0.0f, offset_m, 0.0f),
  };

  float sensitivity = 0.0f;
  for (const float3 &offset : offsets) {
    float2 offset_pixel;
    if (!ocean_camera_projection_pixel(projection, point_object + offset, offset_pixel, nullptr)) {
      continue;
    }
    sensitivity = std::max(sensitivity, len_v2v2(base_pixel, offset_pixel) / offset_m);
  }
  return sensitivity;
}

static bool ocean_split_debug_enabled()
{
  const char *value = BLI_getenv(OCEAN_SPLIT_DEBUG_ENV);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

static void ocean_split_covariance_eigenvalues(const float covariance[3],
                                               float *r_minor_variance,
                                               float *r_major_variance)
{
  const float trace = covariance[0] + covariance[2];
  const float diff = covariance[0] - covariance[2];
  const float discriminant = sqrtf(std::max(diff * diff + 4.0f * covariance[1] * covariance[1], 0.0f));
  *r_major_variance = std::max(0.0f, 0.5f * (trace + discriminant));
  *r_minor_variance = std::max(0.0f, 0.5f * (trace - discriminant));
}

struct OceanSplitSupportDebugStats {
  int count = 0;
  int radius_count = 0;
  double radius_min = 0.0;
  double radius_max = 0.0;
  double radius_sum = 0.0;
  double major_min = 0.0;
  double major_max = 0.0;
  double major_sum = 0.0;
  double minor_min = 0.0;
  double minor_max = 0.0;
  double minor_sum = 0.0;
  double wavelength_x_min = 0.0;
  double wavelength_x_max = 0.0;
  double wavelength_x_sum = 0.0;
  double wavelength_z_min = 0.0;
  double wavelength_z_max = 0.0;
  double wavelength_z_sum = 0.0;
  double covariance_trace_min = 0.0;
  double covariance_trace_max = 0.0;
  double covariance_trace_sum = 0.0;
  double normal_y_min = 0.0;
  double normal_y_max = 0.0;
  double normal_y_sum = 0.0;
};

static void ocean_split_support_debug_accumulate(OceanSplitSupportDebugStats &stats,
                                                 const OceanSplitSupport &support,
                                                 const float radius,
                                                 const bool has_radius,
                                                 const float geometry_normal_y)
{
  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(support.covariance, &minor_variance, &major_variance);

  const double wavelength_major = support.wavelength_major;
  const double wavelength_minor = 2.0 * sqrt(std::max(minor_variance, 0.0f));
  const double wavelength_x = support.wavelength_x;
  const double wavelength_z = support.wavelength_z;
  const double covariance_trace = double(support.covariance[0] + support.covariance[2]);
  const double normal_y = geometry_normal_y;

  if (stats.count == 0) {
    stats.major_min = stats.major_max = wavelength_major;
    stats.minor_min = stats.minor_max = wavelength_minor;
    stats.wavelength_x_min = stats.wavelength_x_max = wavelength_x;
    stats.wavelength_z_min = stats.wavelength_z_max = wavelength_z;
    stats.covariance_trace_min = stats.covariance_trace_max = covariance_trace;
    stats.normal_y_min = stats.normal_y_max = normal_y;
  }
  else {
    stats.major_min = std::min(stats.major_min, wavelength_major);
    stats.major_max = std::max(stats.major_max, wavelength_major);
    stats.minor_min = std::min(stats.minor_min, wavelength_minor);
    stats.minor_max = std::max(stats.minor_max, wavelength_minor);
    stats.wavelength_x_min = std::min(stats.wavelength_x_min, wavelength_x);
    stats.wavelength_x_max = std::max(stats.wavelength_x_max, wavelength_x);
    stats.wavelength_z_min = std::min(stats.wavelength_z_min, wavelength_z);
    stats.wavelength_z_max = std::max(stats.wavelength_z_max, wavelength_z);
    stats.covariance_trace_min = std::min(stats.covariance_trace_min, covariance_trace);
    stats.covariance_trace_max = std::max(stats.covariance_trace_max, covariance_trace);
    stats.normal_y_min = std::min(stats.normal_y_min, normal_y);
    stats.normal_y_max = std::max(stats.normal_y_max, normal_y);
  }

  stats.count++;
  stats.major_sum += wavelength_major;
  stats.minor_sum += wavelength_minor;
  stats.wavelength_x_sum += wavelength_x;
  stats.wavelength_z_sum += wavelength_z;
  stats.covariance_trace_sum += covariance_trace;
  stats.normal_y_sum += normal_y;

  if (has_radius) {
    if (stats.radius_count == 0) {
      stats.radius_min = stats.radius_max = radius;
    }
    else {
      stats.radius_min = std::min(stats.radius_min, double(radius));
      stats.radius_max = std::max(stats.radius_max, double(radius));
    }
    stats.radius_count++;
    stats.radius_sum += radius;
  }
}

static void ocean_split_support_debug_log_stats(const char *object_name,
                                                const char *label,
                                                const OceanSplitSupportDebugStats &stats)
{
  if (stats.count == 0) {
    return;
  }

  printf("Ocean split debug: object='%s' %s verts=%d ", object_name, label, stats.count);
  if (stats.radius_count > 0) {
    printf("radius_m[min=%.4f mean=%.4f max=%.4f] ",
           stats.radius_min,
           stats.radius_sum / double(stats.radius_count),
           stats.radius_max);
  }
  printf("support_major_m[min=%.4f mean=%.4f max=%.4f] "
         "support_minor_m[min=%.4f mean=%.4f max=%.4f] "
         "support_x_m[min=%.4f mean=%.4f max=%.4f] "
         "support_z_m[min=%.4f mean=%.4f max=%.4f] "
         "support_trace[min=%.6f mean=%.6f max=%.6f] "
         "geometry_normal_y[min=%.6f mean=%.6f max=%.6f]\n",
         stats.major_min,
         stats.major_sum / double(stats.count),
         stats.major_max,
         stats.minor_min,
         stats.minor_sum / double(stats.count),
         stats.minor_max,
         stats.wavelength_x_min,
         stats.wavelength_x_sum / double(stats.count),
         stats.wavelength_x_max,
         stats.wavelength_z_min,
         stats.wavelength_z_sum / double(stats.count),
         stats.wavelength_z_max,
         stats.covariance_trace_min,
         stats.covariance_trace_sum / double(stats.count),
         stats.covariance_trace_max,
         stats.normal_y_min,
         stats.normal_y_sum / double(stats.count),
         stats.normal_y_max);
}

static Array<OceanSplitSupport> ocean_geometry_supports(const OceanModifierData *omd,
                                                        const Mesh &mesh,
                                                        const blender::Span<float3> reference_positions,
                                                        const int resolution)
{
  Array<OceanSplitSupport> supports(mesh.verts_num);

  const float generated_spacing = float(omd->spatial_size) / float(std::max(resolution * resolution, 1));
  const float generated_wavelength = 2.0f * std::max(generated_spacing, 1.0e-6f);
  OceanSplitSupport generated_support = ocean_support_isotropic(generated_wavelength);
  generated_support.covariance[0] = generated_spacing * generated_spacing;
  generated_support.covariance[1] = 0.0f;
  generated_support.covariance[2] = generated_spacing * generated_spacing;
  ocean_support_finalize(generated_support, generated_wavelength);

  if (omd->geometry_mode == MOD_OCEAN_GEOM_GENERATE) {
    supports.fill(generated_support);
    return supports;
  }

  const blender::Span<blender::int2> edges = mesh.edges();
  if (edges.is_empty()) {
    supports.fill(generated_support);
    return supports;
  }

  Array<int> vert_to_edge_offsets;
  Array<int> vert_to_edge_indices;
  const blender::GroupedSpan<int> vert_to_edge = blender::bke::mesh::build_vert_to_edge_map(
      edges, mesh.verts_num, vert_to_edge_offsets, vert_to_edge_indices);

  for (const int vert : reference_positions.index_range()) {
    float covariance[3] = {0.0f, 0.0f, 0.0f};
    int sample_count = 0;
    const float2 reference_uv = ocean_reference_plane_coord(reference_positions[vert], omd);

    for (const int edge_index : vert_to_edge[vert]) {
      const blender::int2 edge = edges[edge_index];
      const int other_vert = (edge[0] == vert) ? edge[1] : edge[0];
      const float2 other_reference_uv = ocean_reference_plane_coord(reference_positions[other_vert],
                                                                    omd);
      const float dx = other_reference_uv.x - reference_uv.x;
      const float dz = other_reference_uv.y - reference_uv.y;
      const float length_sq = dx * dx + dz * dz;
      if (length_sq > 1.0e-12f) {
        covariance[0] += dx * dx;
        covariance[1] += dx * dz;
        covariance[2] += dz * dz;
        sample_count++;
      }
    }

    if (sample_count == 0) {
      supports[vert] = generated_support;
      continue;
    }

    const float inv_count = 1.0f / float(sample_count);
    mul_v3_fl(covariance, inv_count);
    supports[vert] = ocean_support_from_covariance(covariance, generated_wavelength);
  }

  return supports;
}

static float2 ocean_reference_uv(const float3 &reference_co, const float size_co_inv)
{
  return float2((reference_co.x * size_co_inv) + 0.5f, (reference_co.y * size_co_inv) + 0.5f);
}

static float3 ocean_reference_coord(const float3 &reference_co, const OceanModifierData *omd)
{
  const float2 plane = ocean_reference_plane_coord(reference_co, omd);
  return float3(plane.x, 0.0f, plane.y);
}

static bool ocean_use_camera_lod(const OceanModifierData *omd)
{
  return (omd->flag & MOD_OCEAN_USE_CAMERA_LOD) != 0 &&
         omd->geometry_mode == MOD_OCEAN_GEOM_GENERATE;
}

static bool ocean_modifier_context_is_render(const ModifierEvalContext *ctx)
{
  if (ctx == nullptr) {
    return false;
  }
  if ((ctx->flag & MOD_APPLY_RENDER) != 0) {
    return true;
  }
  return ctx->depsgraph != nullptr && DEG_get_mode(ctx->depsgraph) == DAG_EVAL_RENDER;
}

static bool ocean_camera_lod_uses_cycles_shading(const ModifierEvalContext *ctx)
{
  if (ctx == nullptr || ctx->depsgraph == nullptr) {
    return false;
  }

  const Scene *scene = DEG_get_input_scene(ctx->depsgraph);
  return scene != nullptr && STREQ(scene->r.engine, RE_engine_id_CYCLES);
}

static bool ocean_camera_lod_requires_dense_fallback(const ModifierEvalContext *ctx,
                                                     const OceanModifierData *omd,
                                                     const char **r_reason)
{
  if (r_reason != nullptr) {
    *r_reason = nullptr;
  }
  if (!ocean_use_camera_lod(omd)) {
    return false;
  }

  if (omd->cached) {
    if (r_reason != nullptr) {
      *r_reason = "Camera LOD uses live full-spectrum data; bake/cache renders use dense geometry";
    }
    return true;
  }

  if ((omd->flag & (MOD_OCEAN_GENERATE_FOAM | MOD_OCEAN_GENERATE_SPRAY)) != 0 &&
      !ocean_camera_lod_uses_cycles_shading(ctx))
  {
    if (r_reason != nullptr) {
      *r_reason =
          "Camera LOD foam/spray full-spectrum sampling requires Cycles; using dense geometry";
    }
    return true;
  }

  if (ctx != nullptr && (ctx->flag & MOD_APPLY_TO_ORIGINAL) != 0) {
    if (r_reason != nullptr) {
      *r_reason = "Applying camera LOD would bake the reduced surface; using dense geometry";
    }
    return true;
  }

  if (ocean_modifier_context_is_render(ctx) && ctx->depsgraph != nullptr) {
    const Scene *scene = DEG_get_input_scene(ctx->depsgraph);
    if (scene != nullptr && !STREQ(scene->r.engine, RE_engine_id_CYCLES)) {
      if (r_reason != nullptr) {
        *r_reason = "Camera LOD render equivalence is only available in Cycles; using dense geometry";
      }
      return true;
    }
  }

  return false;
}

#endif /* WITH_OCEANSIM */

/* Modifier Code */

static void free_runtime_data(void *runtime_data_v);

static void init_data(ModifierData *md)
{
#ifdef WITH_OCEANSIM
  OceanModifierData *omd = (OceanModifierData *)md;
  INIT_DEFAULT_STRUCT_AFTER(omd, modifier);

  BKE_modifier_path_init(omd->cachepath, sizeof(omd->cachepath), "cache_ocean");

  omd->ocean = BKE_ocean_add();
  if (BKE_ocean_init_from_modifier(omd->ocean, omd, omd->viewport_resolution)) {
    simulate_ocean_modifier(omd);
  }
#else  /* WITH_OCEANSIM */
  UNUSED_VARS(md);
#endif /* WITH_OCEANSIM */
}

static void free_data(ModifierData *md)
{
#ifdef WITH_OCEANSIM
  OceanModifierData *omd = (OceanModifierData *)md;

  BKE_ocean_free(omd->ocean);
  if (omd->oceancache) {
    BKE_ocean_free_cache(omd->oceancache);
  }
  free_runtime_data(omd->modifier.runtime);
  omd->modifier.runtime = nullptr;
#else  /* WITH_OCEANSIM */
  /* unused */
  (void)md;
#endif /* WITH_OCEANSIM */
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
#ifdef WITH_OCEANSIM
#  if 0
  const OceanModifierData *omd = (const OceanModifierData *)md;
#  endif
  OceanModifierData *tomd = (OceanModifierData *)target;

  BKE_modifier_copydata_generic(md, target, flag);

  /* The oceancache object will be recreated for this copy
   * automatically when cached=true */
  tomd->oceancache = nullptr;

  tomd->ocean = BKE_ocean_add();
  if (BKE_ocean_init_from_modifier(tomd->ocean, tomd, tomd->viewport_resolution)) {
    simulate_ocean_modifier(tomd);
  }
#else  /* WITH_OCEANSIM */
  /* unused */
  (void)md;
  (void)target;
  (void)flag;
#endif /* WITH_OCEANSIM */
}

#ifdef WITH_OCEANSIM
static void required_data_mask(ModifierData *md, CustomData_MeshMasks *r_cddata_masks)
{
  OceanModifierData *omd = (OceanModifierData *)md;

  if (omd->flag & MOD_OCEAN_GENERATE_FOAM) {
    r_cddata_masks->fmask |= CD_MASK_MCOL; /* XXX Should be loop cddata I guess? */
  }
}
#else  /* WITH_OCEANSIM */
static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks * /*r_cddata_masks*/) {}
#endif /* WITH_OCEANSIM */

#ifdef WITH_OCEANSIM

struct OceanCameraLODSettings;
static bool ocean_camera_lod_uses_dense_generate_fast_path(const OceanCameraLODSettings &settings);

struct OceanCameraLODMeshCacheKey {
  int frame = 0;
  int resolution = 0;
  int scene_resolution_x = 0;
  int scene_resolution_y = 0;
  int scene_scemode = 0;
  int scene_views_format = 0;
  int scene_view_count = 0;
  int repeat_x = 0;
  int repeat_y = 0;
  int spatial_size = 0;
  int spectrum = 0;
  int seed = 0;
  int flag = 0;
  int geometry_mode = 0;
  int lod_levels = 0;
  int lod_usage_mode = 0;
  int lod_validation_mode = 0;
  int camera_type = 0;
  int camera_stereo_convergence_mode = 0;
  float wind_velocity = 0.0f;
  float damp = 0.0f;
  float smallest_wave = 0.0f;
  float depth = 0.0f;
  float wave_alignment = 0.0f;
  float wave_direction = 0.0f;
  float wave_scale = 0.0f;
  float chop_amount = 0.0f;
  float foam_coverage = 0.0f;
  float time = 0.0f;
  float fetch_jonswap = 0.0f;
  float sharpen_peak_jonswap = 0.0f;
  float realsea_fmin = 0.0f;
  float realsea_fmax = 0.0f;
  float realsea_dvar = 0.0f;
  float size = 0.0f;
  float foam_fade = 0.0f;
  float lod_pixel_error = 0.0f;
  float lod_camera_full_spectrum_radius = 0.0f;
  float camera_lens = 0.0f;
  float camera_sensor_x = 0.0f;
  float camera_sensor_y = 0.0f;
  float camera_shiftx = 0.0f;
  float camera_shifty = 0.0f;
  float camera_clip_start = 0.0f;
  float camera_clip_end = 0.0f;
  float camera_ortho_scale = 0.0f;
  float camera_interocular_distance = 0.0f;
  float camera_convergence_distance = 0.0f;
  float object_matrix[4][4] = {};
  float camera_matrix[4][4] = {};
};

struct OceanModifierRuntimeData {
  Mesh *camera_lod_dense_template = nullptr;
  Mesh *camera_lod_cached_result = nullptr;
  OceanCameraLODMeshCacheKey camera_lod_cached_key;
  bool camera_lod_cached_key_valid = false;
  Mesh *camera_lod_topology_template = nullptr;
  OceanCameraLODMeshCacheKey camera_lod_topology_key;
  bool camera_lod_topology_key_valid = false;
  Array<int> camera_lod_topology_levels;
  Array<float> camera_lod_topology_morph_factors;
  Array<float> camera_lod_topology_radii;
  int dense_cells_x = 0;
  int dense_cells_y = 0;
  float2 domain_min = float2(0.0f, 0.0f);
  float2 domain_max = float2(0.0f, 0.0f);
};

struct GenerateOceanGeometryData {
  MutableSpan<float3> vert_positions;
  MutableSpan<int> face_offsets;
  MutableSpan<int> corner_verts;
  MutableSpan<float2> uv_map;

  int res_x, res_y;
  int rx, ry;
  float ox, oy;
  float sx, sy;
  float ix, iy;
};

struct GenerateOceanCameraLODDenseGeometryData {
  blender::MutableSpan<blender::float3> vert_positions;
  blender::MutableSpan<int> face_offsets;
  blender::MutableSpan<int> corner_verts;
  blender::MutableSpan<int> point_levels;
  blender::MutableSpan<float> point_morph_factors;
  blender::MutableSpan<float> point_radius;
  int cells_x;
  int cells_y;
  float2 domain_min;
  float cell_size;
  float2 center;
  int split_level_index;
};

static void generate_ocean_geometry_verts(void *__restrict userdata,
                                          const int y,
                                          const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanGeometryData *gogd = static_cast<GenerateOceanGeometryData *>(userdata);
  int x;

  for (x = 0; x <= gogd->res_x; x++) {
    const int i = y * (gogd->res_x + 1) + x;
    float *co = gogd->vert_positions[i];
    co[0] = gogd->ox + (x * gogd->sx);
    co[1] = gogd->oy + (y * gogd->sy);
    co[2] = 0.0f;
  }
}

static void generate_ocean_geometry_faces(void *__restrict userdata,
                                          const int y,
                                          const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanGeometryData *gogd = static_cast<GenerateOceanGeometryData *>(userdata);
  int x;

  for (x = 0; x < gogd->res_x; x++) {
    const int fi = y * gogd->res_x + x;
    const int vi = y * (gogd->res_x + 1) + x;

    gogd->corner_verts[fi * 4 + 0] = vi;
    gogd->corner_verts[fi * 4 + 1] = vi + 1;
    gogd->corner_verts[fi * 4 + 2] = vi + 1 + gogd->res_x + 1;
    gogd->corner_verts[fi * 4 + 3] = vi + gogd->res_x + 1;

    gogd->face_offsets[fi] = fi * 4;
  }
}

static void generate_ocean_geometry_uvs(void *__restrict userdata,
                                        const int y,
                                        const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanGeometryData *gogd = static_cast<GenerateOceanGeometryData *>(userdata);
  int x;

  for (x = 0; x < gogd->res_x; x++) {
    const int i = y * gogd->res_x + x;
    float2 *luv = &gogd->uv_map[i * 4];

    (*luv)[0] = x * gogd->ix;
    (*luv)[1] = y * gogd->iy;
    luv++;

    (*luv)[0] = (x + 1) * gogd->ix;
    (*luv)[1] = y * gogd->iy;
    luv++;

    (*luv)[0] = (x + 1) * gogd->ix;
    (*luv)[1] = (y + 1) * gogd->iy;
    luv++;

    (*luv)[0] = x * gogd->ix;
    (*luv)[1] = (y + 1) * gogd->iy;
    luv++;
  }
}

static Mesh *generate_ocean_geometry(OceanModifierData *omd,
                                     Mesh *mesh_orig,
                                     const int resolution,
                                     const bool minimal_carrier)
{
  Mesh *result;

  GenerateOceanGeometryData gogd;

  int verts_num;
  int faces_num;

  const bool use_threading = !minimal_carrier && resolution > 4;

  gogd.rx = minimal_carrier ? 1 : resolution * resolution;
  gogd.ry = minimal_carrier ? 1 : resolution * resolution;
  gogd.res_x = gogd.rx * omd->repeat_x;
  gogd.res_y = gogd.ry * omd->repeat_y;

  verts_num = (gogd.res_x + 1) * (gogd.res_y + 1);
  faces_num = gogd.res_x * gogd.res_y;

  gogd.sx = omd->size * omd->spatial_size;
  gogd.sy = omd->size * omd->spatial_size;
  gogd.ox = -gogd.sx / 2.0f;
  gogd.oy = -gogd.sy / 2.0f;

  gogd.sx /= gogd.rx;
  gogd.sy /= gogd.ry;

  result = BKE_mesh_new_nomain(verts_num, 0, faces_num, faces_num * 4);
  BKE_mesh_copy_parameters_for_eval(result, mesh_orig);

  gogd.vert_positions = result->vert_positions_for_write();
  gogd.face_offsets = result->face_offsets_for_write();
  gogd.corner_verts = result->corner_verts_for_write();

  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.use_threading = use_threading;

  /* create vertices */
  BLI_task_parallel_range(0, gogd.res_y + 1, &gogd, generate_ocean_geometry_verts, &settings);

  /* create faces */
  BLI_task_parallel_range(0, gogd.res_y, &gogd, generate_ocean_geometry_faces, &settings);

  bke::mesh_calc_edges(*result, false, false);

  /* add uvs */
  if (result->uv_map_names().size() < MAX_MTFACE) {
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    std::string name = BKE_attribute_calc_unique_name(AttributeOwner::from_id(&result->id),
                                                      "UVMap");
    bke::SpanAttributeWriter<float2> uv_map = attributes.lookup_or_add_for_write_span<float2>(
        name, bke::AttrDomain::Corner);

    if (uv_map) { /* unlikely to fail */
      gogd.uv_map = uv_map.span;
      gogd.ix = 1.0 / gogd.rx;
      gogd.iy = 1.0 / gogd.ry;

      BLI_task_parallel_range(0, gogd.res_y, &gogd, generate_ocean_geometry_uvs, &settings);
    }

    uv_map.finish();
  }

  return result;
}

static void ocean_modifier_runtime_free_dense_template(OceanModifierRuntimeData &runtime_data)
{
  if (runtime_data.camera_lod_dense_template != nullptr) {
    BKE_id_free(nullptr, runtime_data.camera_lod_dense_template);
    runtime_data.camera_lod_dense_template = nullptr;
  }
  runtime_data.dense_cells_x = 0;
  runtime_data.dense_cells_y = 0;
  runtime_data.domain_min = float2(0.0f, 0.0f);
  runtime_data.domain_max = float2(0.0f, 0.0f);
}

static void ocean_modifier_runtime_free_camera_lod_cache(OceanModifierRuntimeData &runtime_data)
{
  if (runtime_data.camera_lod_cached_result != nullptr) {
    BKE_id_free(nullptr, runtime_data.camera_lod_cached_result);
    runtime_data.camera_lod_cached_result = nullptr;
  }
  runtime_data.camera_lod_cached_key_valid = false;
}

static void ocean_modifier_runtime_free_camera_lod_topology_cache(
    OceanModifierRuntimeData &runtime_data)
{
  if (runtime_data.camera_lod_topology_template != nullptr) {
    BKE_id_free(nullptr, runtime_data.camera_lod_topology_template);
    runtime_data.camera_lod_topology_template = nullptr;
  }
  runtime_data.camera_lod_topology_key_valid = false;
  runtime_data.camera_lod_topology_levels.reinitialize(0);
  runtime_data.camera_lod_topology_morph_factors.reinitialize(0);
  runtime_data.camera_lod_topology_radii.reinitialize(0);
}

static void free_runtime_data(void *runtime_data_v)
{
  if (runtime_data_v == nullptr) {
    return;
  }
  OceanModifierRuntimeData *runtime_data = static_cast<OceanModifierRuntimeData *>(runtime_data_v);
  ocean_modifier_runtime_free_dense_template(*runtime_data);
  ocean_modifier_runtime_free_camera_lod_cache(*runtime_data);
  ocean_modifier_runtime_free_camera_lod_topology_cache(*runtime_data);
  MEM_delete(runtime_data);
}

static OceanModifierRuntimeData *ocean_ensure_runtime_data(OceanModifierData *omd)
{
  OceanModifierRuntimeData *runtime_data =
      static_cast<OceanModifierRuntimeData *>(omd->modifier.runtime);
  if (runtime_data == nullptr) {
    runtime_data = MEM_new<OceanModifierRuntimeData>(__func__);
    omd->modifier.runtime = runtime_data;
  }
  return runtime_data;
}

static OceanModifierData *ocean_modifier_cache_owner(ModifierData *md, const ModifierEvalContext *ctx)
{
  if (md == nullptr || ctx == nullptr || ctx->object == nullptr) {
    return reinterpret_cast<OceanModifierData *>(md);
  }

  Object *object_orig = DEG_get_original(ctx->object);
  if (object_orig == nullptr) {
    return reinterpret_cast<OceanModifierData *>(md);
  }

  for (ModifierData *orig_md : ListBaseWrapper<ModifierData>(&object_orig->modifiers)) {
    if (orig_md->type == md->type && orig_md->persistent_uid == md->persistent_uid) {
      return reinterpret_cast<OceanModifierData *>(orig_md);
    }
  }

  return reinterpret_cast<OceanModifierData *>(md);
}

static bool ocean_camera_lod_cache_keys_equal(const OceanCameraLODMeshCacheKey &a,
                                              const OceanCameraLODMeshCacheKey &b)
{
  return a.frame == b.frame && a.resolution == b.resolution &&
         a.scene_resolution_x == b.scene_resolution_x &&
         a.scene_resolution_y == b.scene_resolution_y && a.scene_scemode == b.scene_scemode &&
         a.scene_views_format == b.scene_views_format &&
         a.scene_view_count == b.scene_view_count && a.repeat_x == b.repeat_x &&
         a.repeat_y == b.repeat_y && a.spatial_size == b.spatial_size &&
         a.spectrum == b.spectrum && a.seed == b.seed && a.flag == b.flag &&
         a.geometry_mode == b.geometry_mode && a.lod_levels == b.lod_levels &&
         a.lod_usage_mode == b.lod_usage_mode && a.lod_validation_mode == b.lod_validation_mode &&
         a.camera_type == b.camera_type &&
         a.camera_stereo_convergence_mode == b.camera_stereo_convergence_mode &&
         a.wind_velocity == b.wind_velocity && a.damp == b.damp &&
         a.smallest_wave == b.smallest_wave && a.depth == b.depth &&
         a.wave_alignment == b.wave_alignment && a.wave_direction == b.wave_direction &&
         a.wave_scale == b.wave_scale && a.chop_amount == b.chop_amount &&
         a.foam_coverage == b.foam_coverage && a.time == b.time &&
         a.fetch_jonswap == b.fetch_jonswap && a.sharpen_peak_jonswap == b.sharpen_peak_jonswap &&
         a.realsea_fmin == b.realsea_fmin && a.realsea_fmax == b.realsea_fmax &&
         a.realsea_dvar == b.realsea_dvar && a.size == b.size &&
         a.foam_fade == b.foam_fade && a.lod_pixel_error == b.lod_pixel_error &&
         a.lod_camera_full_spectrum_radius == b.lod_camera_full_spectrum_radius &&
         a.camera_lens == b.camera_lens && a.camera_sensor_x == b.camera_sensor_x &&
         a.camera_sensor_y == b.camera_sensor_y && a.camera_shiftx == b.camera_shiftx &&
         a.camera_shifty == b.camera_shifty && a.camera_clip_start == b.camera_clip_start &&
         a.camera_clip_end == b.camera_clip_end && a.camera_ortho_scale == b.camera_ortho_scale &&
         a.camera_interocular_distance == b.camera_interocular_distance &&
         a.camera_convergence_distance == b.camera_convergence_distance &&
         std::memcmp(a.object_matrix, b.object_matrix, sizeof(a.object_matrix)) == 0 &&
         std::memcmp(a.camera_matrix, b.camera_matrix, sizeof(a.camera_matrix)) == 0;
}

static bool ocean_camera_lod_cache_key_init(const ModifierEvalContext *ctx,
                                            const OceanModifierData *omd,
                                            const int resolution,
                                            const int frame,
                                            OceanCameraLODMeshCacheKey &r_key)
{
  if (ctx == nullptr || omd == nullptr || ctx->object == nullptr || ctx->depsgraph == nullptr) {
    return false;
  }

  Scene *scene = DEG_get_input_scene(ctx->depsgraph);
  Object *camera = (scene != nullptr && scene->camera != nullptr) ?
                       DEG_get_evaluated(ctx->depsgraph, scene->camera) :
                       nullptr;
  if (scene == nullptr || camera == nullptr) {
    return false;
  }

  r_key = {};
  r_key.frame = frame;
  r_key.resolution = resolution;
  r_key.scene_resolution_x = scene->r.xsch;
  r_key.scene_resolution_y = scene->r.ysch;
  r_key.scene_scemode = scene->r.scemode;
  r_key.scene_views_format = scene->r.views_format;
  r_key.scene_view_count = BKE_scene_multiview_num_views_get(&scene->r);
  r_key.repeat_x = omd->repeat_x;
  r_key.repeat_y = omd->repeat_y;
  r_key.spatial_size = omd->spatial_size;
  r_key.spectrum = omd->spectrum;
  r_key.seed = omd->seed;
  r_key.flag = omd->flag;
  r_key.geometry_mode = omd->geometry_mode;
  r_key.lod_levels = omd->lod_levels;
  r_key.lod_usage_mode = omd->lod_usage_mode;
  r_key.lod_validation_mode = omd->lod_validation_mode;
  r_key.wind_velocity = omd->wind_velocity;
  r_key.damp = omd->damp;
  r_key.smallest_wave = omd->smallest_wave;
  r_key.depth = omd->depth;
  r_key.wave_alignment = omd->wave_alignment;
  r_key.wave_direction = omd->wave_direction;
  r_key.wave_scale = ocean_wave_scale_effective(omd);
  r_key.chop_amount = omd->chop_amount;
  r_key.foam_coverage = omd->foam_coverage;
  r_key.time = omd->time;
  r_key.fetch_jonswap = omd->fetch_jonswap;
  r_key.sharpen_peak_jonswap = omd->sharpen_peak_jonswap;
  r_key.realsea_fmin = omd->realsea_fmin;
  r_key.realsea_fmax = omd->realsea_fmax;
  r_key.realsea_dvar = omd->realsea_dvar;
  r_key.size = omd->size;
  r_key.foam_fade = omd->foam_fade;
  r_key.lod_pixel_error = omd->lod_pixel_error;
  r_key.lod_camera_full_spectrum_radius = omd->lod_camera_full_spectrum_radius;
  copy_m4_m4(r_key.object_matrix, ctx->object->object_to_world().ptr());
  copy_m4_m4(r_key.camera_matrix, camera->object_to_world().ptr());

  if (camera->data != nullptr && camera->type == OB_CAMERA) {
    const Camera *camera_data = reinterpret_cast<const Camera *>(camera->data);
    r_key.camera_type = camera_data->type;
    r_key.camera_lens = camera_data->lens;
    r_key.camera_sensor_x = camera_data->sensor_x;
    r_key.camera_sensor_y = camera_data->sensor_y;
    r_key.camera_shiftx = camera_data->shiftx;
    r_key.camera_shifty = camera_data->shifty;
    r_key.camera_clip_start = camera_data->clip_start;
    r_key.camera_clip_end = camera_data->clip_end;
    r_key.camera_ortho_scale = camera_data->ortho_scale;
    r_key.camera_interocular_distance = camera_data->stereo.interocular_distance;
    r_key.camera_convergence_distance = camera_data->stereo.convergence_distance;
    r_key.camera_stereo_convergence_mode = camera_data->stereo.convergence_mode;
  }

  return true;
}

static bool ocean_camera_lod_topology_cache_key_init(const ModifierEvalContext *ctx,
                                                     const OceanModifierData *omd,
                                                     const int resolution,
                                                     OceanCameraLODMeshCacheKey &r_key)
{
  if (!ocean_camera_lod_cache_key_init(ctx, omd, resolution, 0, r_key)) {
    return false;
  }

  r_key.frame = 0;
  r_key.time = 0.0f;
  return true;
}

static bool ocean_camera_lod_topology_cache_keys_equal(OceanCameraLODMeshCacheKey a,
                                                       OceanCameraLODMeshCacheKey b)
{
  a.frame = 0;
  b.frame = 0;
  a.time = 0.0f;
  b.time = 0.0f;
  return ocean_camera_lod_cache_keys_equal(a, b);
}

static void ocean_camera_lod_array_copy(const Span<int> src, Array<int> &r_dst)
{
  r_dst.reinitialize(src.size());
  r_dst.as_mutable_span().copy_from(src);
}

static void ocean_camera_lod_array_copy(const Span<float> src, Array<float> &r_dst)
{
  r_dst.reinitialize(src.size());
  r_dst.as_mutable_span().copy_from(src);
}

static void generate_ocean_geometry_camera_lod_dense_verts(
    void *__restrict userdata,
    const int y,
    const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanCameraLODDenseGeometryData *gogd =
      static_cast<GenerateOceanCameraLODDenseGeometryData *>(userdata);
  const int stride = gogd->cells_x + 1;
  const float y_coord = gogd->domain_min.y + (float(y) * gogd->cell_size);

  for (int x = 0; x <= gogd->cells_x; x++) {
    const int i = y * stride + x;
    const float x_coord = gogd->domain_min.x + (float(x) * gogd->cell_size);
    float *co = gogd->vert_positions[i];
    co[0] = x_coord;
    co[1] = y_coord;
    co[2] = 0.0f;

    if (!gogd->point_levels.is_empty()) {
      gogd->point_levels[i] = gogd->split_level_index;
    }
    if (!gogd->point_morph_factors.is_empty()) {
      gogd->point_morph_factors[i] = 1.0f;
    }
    if (!gogd->point_radius.is_empty()) {
      const float dx = x_coord - gogd->center.x;
      const float dy = y_coord - gogd->center.y;
      gogd->point_radius[i] = sqrtf((dx * dx) + (dy * dy));
    }
  }
}

static void generate_ocean_geometry_camera_lod_dense_faces(
    void *__restrict userdata,
    const int y,
    const TaskParallelTLS *__restrict /*tls*/)
{
  GenerateOceanCameraLODDenseGeometryData *gogd =
      static_cast<GenerateOceanCameraLODDenseGeometryData *>(userdata);
  const int stride = gogd->cells_x + 1;

  for (int x = 0; x < gogd->cells_x; x++) {
    const int fi = y * gogd->cells_x + x;
    const int vi = y * stride + x;

    gogd->corner_verts[fi * 4 + 0] = vi;
    gogd->corner_verts[fi * 4 + 1] = vi + 1;
    gogd->corner_verts[fi * 4 + 2] = vi + 1 + stride;
    gogd->corner_verts[fi * 4 + 3] = vi + stride;

    gogd->face_offsets[fi] = fi * 4;
  }
}

struct OceanCameraLODLevel {
  int split_level_index = 0;
  int stride = 1;
  float cell_size = 0.0f;
};

struct OceanCameraLODLeaf {
  int local_level_index = 0;
  int split_level_index = 0;
  int stride = 1;
  float cell_size = 0.0f;
  int min_x = 0;
  int max_x = 0;
  int min_y = 0;
  int max_y = 0;
  OceanLODObservableErrorStats error_stats;
};

struct OceanCameraLODSettings {
  int quadtree_levels = 0;
  int dense_cells_x = 0;
  int dense_cells_y = 0;
  int dense_vert_budget = 0;
  int usage_mode = MOD_OCEAN_LOD_USAGE_GENERAL_RENDER;
  int validation_mode = MOD_OCEAN_LOD_VALIDATE_CAMERA_OBSERVABLE;
  float finest_cell_size = 0.0f;
  float2 domain_min = float2(0.0f, 0.0f);
  float2 domain_max = float2(0.0f, 0.0f);
  float full_spectrum_radius = 0.0f;
  float visible_footprint_guard = 0.0f;
  float2 center = float2(0.0f, 0.0f);
  OceanLODObservableTolerances tolerances;
  bool full_domain_dense = false;
  const char *error_message = nullptr;
  OceanLODRelevantFootprint visible_footprint;
  OceanCameraProjectionSet projection_set;
  Vector<OceanCameraLODLevel> levels;
  Vector<OceanCameraLODLeaf> leaves;
};

static bool ocean_camera_lod_uses_dense_generate_fast_path(const OceanCameraLODSettings &settings)
{
  return settings.projection_set.valid && settings.full_domain_dense && !settings.levels.is_empty();
}

static void ocean_camera_lod_init_dense_mesh_metadata(const Mesh &mesh,
                                                      const OceanCameraLODSettings &lod_settings,
                                                      Array<int> &r_point_levels,
                                                      Array<float> &r_point_morph_factors,
                                                      Array<float> &r_point_radius)
{
  BLI_assert(!lod_settings.levels.is_empty());

  const blender::Span<float3> positions = mesh.vert_positions();
  const int split_level_index = lod_settings.levels.first().split_level_index;
  r_point_levels.reinitialize(positions.size());
  r_point_morph_factors.reinitialize(positions.size());
  r_point_radius.reinitialize(positions.size());

  r_point_levels.as_mutable_span().fill(split_level_index);
  r_point_morph_factors.as_mutable_span().fill(1.0f);

  for (const int vert : positions.index_range()) {
    const float2 delta = float2(positions[vert].x, positions[vert].y) - lod_settings.center;
    r_point_radius[vert] = sqrtf((delta.x * delta.x) + (delta.y * delta.y));
  }
}

static bool ocean_modifier_runtime_dense_template_matches(
    const OceanModifierRuntimeData &runtime_data, const OceanCameraLODSettings &lod_settings)
{
  return runtime_data.camera_lod_dense_template != nullptr &&
         runtime_data.dense_cells_x == lod_settings.dense_cells_x &&
         runtime_data.dense_cells_y == lod_settings.dense_cells_y &&
         runtime_data.domain_min.x == lod_settings.domain_min.x &&
         runtime_data.domain_min.y == lod_settings.domain_min.y &&
         runtime_data.domain_max.x == lod_settings.domain_max.x &&
         runtime_data.domain_max.y == lod_settings.domain_max.y;
}

static Mesh *generate_ocean_geometry_camera_lod_dense_template(
    Mesh *mesh_orig, const OceanCameraLODSettings &lod_settings)
{
  const int cells_x = lod_settings.dense_cells_x;
  const int cells_y = lod_settings.dense_cells_y;
  const int verts_num = lod_settings.dense_vert_budget;
  const int faces_num = cells_x * cells_y;
  const float cell_size = lod_settings.finest_cell_size;

  Mesh *result = BKE_mesh_new_nomain(verts_num, 0, faces_num, faces_num * 4);
  BKE_mesh_copy_parameters_for_eval(result, mesh_orig);

  GenerateOceanCameraLODDenseGeometryData gogd{};
  gogd.vert_positions = result->vert_positions_for_write();
  gogd.face_offsets = result->face_offsets_for_write();
  gogd.corner_verts = result->corner_verts_for_write();
  gogd.cells_x = cells_x;
  gogd.cells_y = cells_y;
  gogd.domain_min = lod_settings.domain_min;
  gogd.cell_size = cell_size;
  gogd.center = lod_settings.center;
  gogd.split_level_index = lod_settings.levels.first().split_level_index;

  TaskParallelSettings parallel_settings;
  BLI_parallel_range_settings_defaults(&parallel_settings);
  parallel_settings.use_threading = std::max(cells_x, cells_y) >= 128;

  BLI_task_parallel_range(0,
                          cells_y + 1,
                          &gogd,
                          generate_ocean_geometry_camera_lod_dense_verts,
                          &parallel_settings);
  BLI_task_parallel_range(0,
                          cells_y,
                          &gogd,
                          generate_ocean_geometry_camera_lod_dense_faces,
                          &parallel_settings);

  blender::bke::mesh_calc_edges(*result, false, false);
  return result;
}

static Mesh *generate_ocean_geometry_camera_lod_dense(
    OceanModifierData *omd,
    Mesh *mesh_orig,
    const OceanCameraLODSettings &lod_settings,
    Array<int> &r_point_levels,
    Array<float> &r_point_morph_factors,
    Array<float> &r_point_radius)
{
  BLI_assert(ocean_camera_lod_uses_dense_generate_fast_path(lod_settings));
  BLI_assert(!lod_settings.levels.is_empty());

  OceanModifierRuntimeData *runtime_data = ocean_ensure_runtime_data(omd);
  if (!ocean_modifier_runtime_dense_template_matches(*runtime_data, lod_settings)) {
    ocean_modifier_runtime_free_dense_template(*runtime_data);
    runtime_data->camera_lod_dense_template = generate_ocean_geometry_camera_lod_dense_template(
        mesh_orig, lod_settings);
    runtime_data->dense_cells_x = lod_settings.dense_cells_x;
    runtime_data->dense_cells_y = lod_settings.dense_cells_y;
    runtime_data->domain_min = lod_settings.domain_min;
    runtime_data->domain_max = lod_settings.domain_max;
  }

  Mesh *result = BKE_mesh_copy_for_eval(*runtime_data->camera_lod_dense_template);
  BKE_mesh_copy_parameters_for_eval(result, mesh_orig);
  ocean_camera_lod_init_dense_mesh_metadata(
      *result, lod_settings, r_point_levels, r_point_morph_factors, r_point_radius);
  return result;
}

static void ocean_camera_lod_debug_log_settings(const OceanModifierData *omd,
                                                const OceanCameraLODSettings &settings)
{
  if (!ocean_split_debug_enabled()) {
    return;
  }

  printf(
      "[OCEAN_CAMERA_LOD_DEBUG] Modifier camera_lod mode=%s usage=%s center=(%.6f,%.6f) "
      "domain_min=(%.6f,%.6f) domain_max=(%.6f,%.6f) "
      "finest_cell=%.6f full_spectrum_radius=%.6f visible_guard=%.6f "
      "quadtree_levels=%d modifier_lod_levels=%d dense_cells=(%d,%d) "
      "dense_budget=%d visible_footprint_valid=%s footprint_min=(%.6f,%.6f) footprint_max=(%.6f,%.6f)\n",
      settings.validation_mode == MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT ? "GEOMETRY_STRICT" :
                                                                            "CAMERA_OBSERVABLE",
      settings.usage_mode == MOD_OCEAN_LOD_USAGE_STEREO_DATASET ? "STEREO_DATASET" :
                                                                   "GENERAL_RENDER",
      double(settings.center.x),
      double(settings.center.y),
      double(settings.domain_min.x),
      double(settings.domain_min.y),
      double(settings.domain_max.x),
      double(settings.domain_max.y),
      double(settings.finest_cell_size),
      double(settings.full_spectrum_radius),
      double(settings.visible_footprint_guard),
      settings.quadtree_levels,
      int(omd->lod_levels),
      settings.dense_cells_x,
      settings.dense_cells_y,
      settings.dense_vert_budget,
      settings.visible_footprint.valid ? "yes" : "no",
      double(settings.visible_footprint.min.x),
      double(settings.visible_footprint.min.y),
      double(settings.visible_footprint.max.x),
      double(settings.visible_footprint.max.y));

  for (const int level_index : settings.levels.index_range()) {
    const OceanCameraLODLevel &level = settings.levels[level_index];
    printf(
        "[OCEAN_CAMERA_LOD_DEBUG] Modifier camera_lod level=%d split_level=%d stride=%d "
        "cell=%.6f\n",
        level_index,
        level.split_level_index,
        level.stride,
        double(level.cell_size));
  }
  fflush(stdout);
}

static int ocean_camera_lod_clamp_level_count(const int dense_cells_x,
                                              const int dense_cells_y,
                                              const int base_stride,
                                              const int requested_levels)
{
  int level_count = 1;
  int stride = std::max(base_stride, 1);
  while (level_count < requested_levels && (dense_cells_x % (stride << 1)) == 0 &&
         (dense_cells_y % (stride << 1)) == 0 && (dense_cells_x / (stride << 1)) >= 4 &&
         (dense_cells_y / (stride << 1)) >= 4)
  {
    stride <<= 1;
    level_count++;
  }
  return level_count;
}

enum class OceanCameraLODEdge {
  South = 0,
  East = 1,
  North = 2,
  West = 3,
};

static float ocean_camera_lod_dense_cell_size(const OceanCameraLODSettings &settings)
{
  return settings.finest_cell_size;
}

static float ocean_camera_lod_dense_coord_x(const OceanCameraLODSettings &settings,
                                            const int dense_index)
{
  return settings.domain_min.x + (float(dense_index) * ocean_camera_lod_dense_cell_size(settings));
}

static float ocean_camera_lod_dense_coord_y(const OceanCameraLODSettings &settings,
                                            const int dense_index)
{
  return settings.domain_min.y + (float(dense_index) * ocean_camera_lod_dense_cell_size(settings));
}

static float2 ocean_camera_lod_leaf_region_min(const OceanCameraLODSettings &settings,
                                               const OceanCameraLODLeaf &leaf)
{
  return float2(
      ocean_camera_lod_dense_coord_x(settings, leaf.min_x),
      ocean_camera_lod_dense_coord_y(settings, leaf.min_y));
}

static float2 ocean_camera_lod_leaf_region_max(const OceanCameraLODSettings &settings,
                                               const OceanCameraLODLeaf &leaf)
{
  return float2(
      ocean_camera_lod_dense_coord_x(settings, leaf.max_x),
      ocean_camera_lod_dense_coord_y(settings, leaf.max_y));
}

static bool ocean_camera_lod_region_intersects_full_spectrum_anchor(
    const OceanCameraLODSettings &settings,
    const float2 &region_min,
    const float2 &region_max)
{
  const float radius = std::max(settings.full_spectrum_radius, 0.0f);
  if (radius <= 0.0f) {
    return false;
  }

  const float closest_x = clamp_f(settings.center.x, region_min.x, region_max.x);
  const float closest_y = clamp_f(settings.center.y, region_min.y, region_max.y);
  const float dx = settings.center.x - closest_x;
  const float dy = settings.center.y - closest_y;
  return (dx * dx) + (dy * dy) <= radius * radius;
}

static bool ocean_camera_lod_region_intersects_projection_set(
    const OceanCameraLODSettings &settings,
    const float2 &region_min,
    const float2 &region_max,
    const float guard)
{
  if (!settings.projection_set.valid) {
    return false;
  }

  const float safe_guard = std::max(guard, 0.0f);
  const float2 guarded_min(std::max(region_min.x - safe_guard, settings.domain_min.x),
                           std::max(region_min.y - safe_guard, settings.domain_min.y));
  const float2 guarded_max(std::min(region_max.x + safe_guard, settings.domain_max.x),
                           std::min(region_max.y + safe_guard, settings.domain_max.y));

  for (const OceanCameraProjection &projection : settings.projection_set.projections) {
    if (ocean_camera_projection_region_intersects(projection, guarded_min, guarded_max)) {
      return true;
    }
  }
  return false;
}

static bool ocean_camera_lod_region_is_relevant(const OceanCameraLODSettings &settings,
                                                const float2 &region_min,
                                                const float2 &region_max)
{
  if (ocean_camera_lod_region_intersects_full_spectrum_anchor(settings, region_min, region_max)) {
    return true;
  }
  if (!settings.visible_footprint.valid) {
    return false;
  }
  if (!ocean_aabb_intersects(
          region_min, region_max, settings.visible_footprint.min, settings.visible_footprint.max))
  {
    return false;
  }
  return ocean_camera_lod_region_intersects_projection_set(
      settings, region_min, region_max, settings.visible_footprint_guard);
}

static bool ocean_camera_lod_point_visible(const OceanCameraProjectionSet &projection_set,
                                           const float3 &point_object)
{
  float2 pixel;
  for (const OceanCameraProjection &projection : projection_set.projections) {
    if (ocean_camera_projection_pixel(projection, point_object, pixel, nullptr)) {
      return true;
    }
  }
  return false;
}

static float ocean_camera_lod_region_pixel_sensitivity(const OceanCameraLODSettings &settings,
                                                       const float2 &region_min,
                                                       const float2 &region_max,
                                                       const float offset_m)
{
  if (!settings.projection_set.valid || offset_m <= 1.0e-6f) {
    return 0.0f;
  }

  const float2 center = 0.5f * (region_min + region_max);
  const float2 coords[5] = {
      region_min,
      float2(region_max.x, region_min.y),
      region_max,
      float2(region_min.x, region_max.y),
      center,
  };

  float sensitivity = 0.0f;
  for (const float2 &coord : coords) {
    const float3 point(coord.x, coord.y, 0.0f);
    for (const OceanCameraProjection &projection : settings.projection_set.projections) {
      sensitivity = std::max(
          sensitivity, ocean_camera_projection_planar_pixel_sensitivity(projection, point, offset_m));
    }
  }
  return sensitivity;
}

struct OceanCameraLODResolvableBound {
  int split_level = 0;
  float resolvable_wavelength = 0.0f;
  float max_cell_size = FLT_MAX;
};

static OceanCameraLODResolvableBound ocean_camera_lod_resolvable_bound(
    const Span<OceanSplitMomentLevel> moment_levels,
    const OceanCameraLODSettings &settings,
    const float2 &region_min,
    const float2 &region_max,
    const float cell_size)
{
  OceanCameraLODResolvableBound bound{};
  if (moment_levels.is_empty()) {
    bound.max_cell_size = ocean_camera_lod_dense_cell_size(settings);
    return bound;
  }

  const int coarsest_split_level = std::min(settings.quadtree_levels - 1,
                                           int(moment_levels.size()) - 1);
  if (ocean_camera_lod_region_intersects_full_spectrum_anchor(settings, region_min, region_max)) {
    bound.split_level = 0;
    bound.resolvable_wavelength = moment_levels.first().wavelength;
    bound.max_cell_size = ocean_camera_lod_dense_cell_size(settings);
    return bound;
  }

  const float dense_cell_size = ocean_camera_lod_dense_cell_size(settings);
  const float offset_m = std::max(0.25f * std::max(cell_size, dense_cell_size), 1.0e-4f);
  const float sensitivity = ocean_camera_lod_region_pixel_sensitivity(
      settings, region_min, region_max, offset_m);
  if (sensitivity <= 1.0e-8f) {
    bound.split_level = coarsest_split_level;
    bound.resolvable_wavelength = FLT_MAX;
    bound.max_cell_size = FLT_MAX;
    return bound;
  }

  const float meters_for_pixel_error = std::max(settings.tolerances.reprojection_px, 1.0e-4f) /
                                       sensitivity;
  const float resolvable_wavelength = std::max(2.0f * meters_for_pixel_error, 1.0e-6f);

  int allowed_split_level = 0;
  for (const int level_index : moment_levels.index_range()) {
    if (moment_levels[level_index].wavelength <= resolvable_wavelength) {
      allowed_split_level = level_index;
    }
  }

  bound.split_level = std::min(allowed_split_level, coarsest_split_level);
  bound.resolvable_wavelength = resolvable_wavelength;
  /* A mesh needs at least two vertices per resolvable wavelength. The old test only checked
   * the procedural spectrum band, so split-level 0 could still be emitted as a large polygon and
   * blur the carrier surface in the camera footprint. */
  bound.max_cell_size = std::max(0.5f * resolvable_wavelength, dense_cell_size);
  return bound;
}

static bool ocean_camera_lod_leaf_spectrum_within_resolvable_bound(
    const Span<OceanSplitMomentLevel> moment_levels,
    const OceanCameraLODSettings &settings,
    const OceanCameraLODLeaf &leaf,
    const float2 &region_min,
    const float2 &region_max)
{
  const OceanCameraLODResolvableBound bound = ocean_camera_lod_resolvable_bound(
      moment_levels, settings, region_min, region_max, leaf.cell_size);
  const bool split_level_ok = leaf.split_level_index <= bound.split_level;
  const bool density_ok = leaf.cell_size <= bound.max_cell_size * (1.0f + 1.0e-4f);
  return split_level_ok && density_ok;
}

static OceanLODObservableErrorStats ocean_camera_lod_cell_error_stats(
    const OceanModifierData *omd,
    const Span<OceanSplitMomentLevel> moment_levels,
    const OceanCameraLODSettings &settings,
    const OceanSplitRuntimeReadScope *read_scope,
    const OceanCameraLODLeaf &leaf,
    const float2 &region_min,
    const float2 &region_max)
{
  OceanLODObservableErrorStats stats{};
  const bool profile_enabled = g_ocean_camera_lod_region_profile_active;
  const double profile_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  if (!settings.projection_set.valid) {
    return stats;
  }

  double sampled_sum_sq[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double margin_sum_sq[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  double combined_sum_sq[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  OceanLODObservableError sampled_max{};
  OceanLODObservableError margin_max{};
  OceanLODObservableError combined_max{};
  int sample_count = 0;
  const float sample_extent = std::max(region_max.x - region_min.x, region_max.y - region_min.y);
  const float blind_spot_radius = 0.25f * std::max(sample_extent, 1.0e-6f);

  float3 corner_positions[4];
  float3 corner_normals[4];
  const float2 corner_coords[4] = {
      region_min,
      float2(region_max.x, region_min.y),
      region_max,
      float2(region_min.x, region_max.y),
  };
  for (int corner = 0; corner < 4; corner++) {
    if (!ocean_split_sample_geometry_level_object(omd,
                                                  read_scope,
                                                  leaf.split_level_index,
                                                  corner_coords[corner],
                                                  corner_positions[corner],
                                                  corner_normals[corner]))
    {
      return stats;
    }
  }

  const auto interpolate_triangle = [](const float3 &a,
                                       const float3 &b,
                                       const float3 &c,
                                       const float wa,
                                       const float wb,
                                       const float wc) {
    return (a * wa) + (b * wb) + (c * wc);
  };

  const auto sample_leaf_carrier = [&](const float2 &coord,
                                       float3 &r_position_object,
                                       float3 &r_normal_object) {
    const float u = (region_max.x > region_min.x + 1.0e-8f) ?
                        clamp_f((coord.x - region_min.x) / (region_max.x - region_min.x), 0.0f, 1.0f) :
                        0.0f;
    const float v = (region_max.y > region_min.y + 1.0e-8f) ?
                        clamp_f((coord.y - region_min.y) / (region_max.y - region_min.y), 0.0f, 1.0f) :
                        0.0f;

    /* Camera LOD emits one polygon per leaf. Most leaves are quads, triangulated along the
     * lower-left to upper-right diagonal by the render mesh. Validate against that rendered
     * carrier surface so full-spectrum-but-under-tessellated leaves cannot pass as zero error. */
    if (u >= v) {
      r_position_object = interpolate_triangle(
          corner_positions[0], corner_positions[1], corner_positions[2], 1.0f - u, u - v, v);
      r_normal_object = interpolate_triangle(
          corner_normals[0], corner_normals[1], corner_normals[2], 1.0f - u, u - v, v);
    }
    else {
      r_position_object = interpolate_triangle(
          corner_positions[0], corner_positions[2], corner_positions[3], 1.0f - v, u, v - u);
      r_normal_object = interpolate_triangle(
          corner_normals[0], corner_normals[2], corner_normals[3], 1.0f - v, u, v - u);
    }
    normalize_v3(r_normal_object);
  };

  const auto accumulate_metrics = [&](const OceanLODObservableSample &sample) {
    sampled_sum_sq[0] += double(sample.sampled.position_m) * double(sample.sampled.position_m);
    sampled_sum_sq[1] += double(sample.sampled.reprojection_px) * double(sample.sampled.reprojection_px);
    sampled_sum_sq[2] += double(sample.sampled.depth_m) * double(sample.sampled.depth_m);
    sampled_sum_sq[3] += double(sample.sampled.normal_radians) * double(sample.sampled.normal_radians);
    sampled_sum_sq[4] += double(sample.sampled.temporal_px) * double(sample.sampled.temporal_px);
    sampled_sum_sq[5] += double(sample.sampled.grazing_px) * double(sample.sampled.grazing_px);
    margin_sum_sq[0] += double(sample.margin.position_m) * double(sample.margin.position_m);
    margin_sum_sq[1] += double(sample.margin.reprojection_px) * double(sample.margin.reprojection_px);
    margin_sum_sq[2] += double(sample.margin.depth_m) * double(sample.margin.depth_m);
    margin_sum_sq[3] += double(sample.margin.normal_radians) * double(sample.margin.normal_radians);
    margin_sum_sq[4] += double(sample.margin.temporal_px) * double(sample.margin.temporal_px);
    margin_sum_sq[5] += double(sample.margin.grazing_px) * double(sample.margin.grazing_px);
    combined_sum_sq[0] += double(sample.sampled.position_m + sample.margin.position_m) *
                          double(sample.sampled.position_m + sample.margin.position_m);
    combined_sum_sq[1] += double(sample.sampled.reprojection_px + sample.margin.reprojection_px) *
                          double(sample.sampled.reprojection_px + sample.margin.reprojection_px);
    combined_sum_sq[2] += double(sample.sampled.depth_m + sample.margin.depth_m) *
                          double(sample.sampled.depth_m + sample.margin.depth_m);
    combined_sum_sq[3] += double(sample.sampled.normal_radians + sample.margin.normal_radians) *
                          double(sample.sampled.normal_radians + sample.margin.normal_radians);
    combined_sum_sq[4] += double(sample.sampled.temporal_px + sample.margin.temporal_px) *
                          double(sample.sampled.temporal_px + sample.margin.temporal_px);
    combined_sum_sq[5] += double(sample.sampled.grazing_px + sample.margin.grazing_px) *
                          double(sample.sampled.grazing_px + sample.margin.grazing_px);

    sampled_max.position_m = std::max(sampled_max.position_m, sample.sampled.position_m);
    sampled_max.reprojection_px = std::max(sampled_max.reprojection_px, sample.sampled.reprojection_px);
    sampled_max.depth_m = std::max(sampled_max.depth_m, sample.sampled.depth_m);
    sampled_max.normal_radians = std::max(sampled_max.normal_radians, sample.sampled.normal_radians);
    sampled_max.temporal_px = std::max(sampled_max.temporal_px, sample.sampled.temporal_px);
    sampled_max.grazing_px = std::max(sampled_max.grazing_px, sample.sampled.grazing_px);
    margin_max.position_m = std::max(margin_max.position_m, sample.margin.position_m);
    margin_max.reprojection_px = std::max(margin_max.reprojection_px, sample.margin.reprojection_px);
    margin_max.depth_m = std::max(margin_max.depth_m, sample.margin.depth_m);
    margin_max.normal_radians = std::max(margin_max.normal_radians, sample.margin.normal_radians);
    margin_max.temporal_px = std::max(margin_max.temporal_px, sample.margin.temporal_px);
    margin_max.grazing_px = std::max(margin_max.grazing_px, sample.margin.grazing_px);
    combined_max.position_m = std::max(
        combined_max.position_m, sample.sampled.position_m + sample.margin.position_m);
    combined_max.reprojection_px = std::max(
        combined_max.reprojection_px, sample.sampled.reprojection_px + sample.margin.reprojection_px);
    combined_max.depth_m = std::max(combined_max.depth_m, sample.sampled.depth_m + sample.margin.depth_m);
    combined_max.normal_radians = std::max(
        combined_max.normal_radians, sample.sampled.normal_radians + sample.margin.normal_radians);
    combined_max.temporal_px = std::max(
        combined_max.temporal_px, sample.sampled.temporal_px + sample.margin.temporal_px);
    combined_max.grazing_px = std::max(
        combined_max.grazing_px, sample.sampled.grazing_px + sample.margin.grazing_px);
    sample_count++;
  };

  const bool require_visible_sample = !(settings.validation_mode ==
                                            MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT &&
                                        settings.usage_mode != MOD_OCEAN_LOD_USAGE_STEREO_DATASET);

  const auto accumulate_coord = [&](const float2 &coord) {
    const bool sample_profile_enabled = g_ocean_camera_lod_region_profile_active;
    const double sample_profile_start = sample_profile_enabled ? BLI_time_now_seconds() : 0.0;
    float3 reference_position_object;
    float3 reference_normal_object;
    if (!ocean_split_sample_geometry_level_object(
            omd, read_scope, 0, coord, reference_position_object, reference_normal_object))
    {
      return;
    }
    if (require_visible_sample &&
        !ocean_camera_lod_point_visible(settings.projection_set, reference_position_object))
    {
      return;
    }

    float3 position_object;
    float3 normal_object;
    sample_leaf_carrier(coord, position_object, normal_object);

    const OceanLODObservableSample sample = ocean_camera_lod_sample_error_from_reference(
        settings.projection_set,
        omd,
        moment_levels,
        settings.tolerances,
        leaf.split_level_index,
        reference_position_object,
        reference_normal_object,
        position_object,
        normal_object,
        blind_spot_radius,
        settings.usage_mode);
    accumulate_metrics(sample);
    if (sample_profile_enabled) {
      g_ocean_camera_lod_region_profile.sample_eval_count++;
      g_ocean_camera_lod_region_profile.sample_eval_s += BLI_time_now_seconds() - sample_profile_start;
    }
  };

  static const float sample_coords[] = {0.0f, 0.5f, 1.0f};
  for (const float u : sample_coords) {
    for (const float v : sample_coords) {
      const float2 coord = float2(
          region_min.x + (u * (region_max.x - region_min.x)),
          region_min.y + (v * (region_max.y - region_min.y)));
      accumulate_coord(coord);
    }
  }

  if (sample_count == 0) {
    for (const OceanCameraProjection &projection : settings.projection_set.projections) {
      Vector<float2> polygon;
      if (!ocean_camera_projection_region_clip_polygon(projection, region_min, region_max, polygon)) {
        continue;
      }

      float2 centroid(0.0f, 0.0f);
      for (const float2 &coord : polygon) {
        centroid += coord;
      }
      centroid /= float(polygon.size());
      accumulate_coord(centroid);

      for (const float2 &coord : polygon) {
        accumulate_coord(coord);
      }

      if (sample_count > 0) {
        break;
      }
    }
  }

  stats.sample_count = sample_count;
  stats.position_sample_count = sample_count;
  stats.valid = sample_count > 0;
  stats.position_gate_applied = sample_count > 0;
  stats.sampled_max = sampled_max;
  stats.margin_max = margin_max;
  stats.max = combined_max;

  if (sample_count > 0) {
    const double inv_count = 1.0 / double(sample_count);
    stats.sampled_rms.position_m = sqrtf(float(sampled_sum_sq[0] * inv_count));
    stats.sampled_rms.reprojection_px = sqrtf(float(sampled_sum_sq[1] * inv_count));
    stats.sampled_rms.depth_m = sqrtf(float(sampled_sum_sq[2] * inv_count));
    stats.sampled_rms.normal_radians = sqrtf(float(sampled_sum_sq[3] * inv_count));
    stats.sampled_rms.temporal_px = sqrtf(float(sampled_sum_sq[4] * inv_count));
    stats.sampled_rms.grazing_px = sqrtf(float(sampled_sum_sq[5] * inv_count));
    stats.margin_rms.position_m = sqrtf(float(margin_sum_sq[0] * inv_count));
    stats.margin_rms.reprojection_px = sqrtf(float(margin_sum_sq[1] * inv_count));
    stats.margin_rms.depth_m = sqrtf(float(margin_sum_sq[2] * inv_count));
    stats.margin_rms.normal_radians = sqrtf(float(margin_sum_sq[3] * inv_count));
    stats.margin_rms.temporal_px = sqrtf(float(margin_sum_sq[4] * inv_count));
    stats.margin_rms.grazing_px = sqrtf(float(margin_sum_sq[5] * inv_count));
    stats.rms.position_m = sqrtf(float(combined_sum_sq[0] * inv_count));
    stats.rms.reprojection_px = sqrtf(float(combined_sum_sq[1] * inv_count));
    stats.rms.depth_m = sqrtf(float(combined_sum_sq[2] * inv_count));
    stats.rms.normal_radians = sqrtf(float(combined_sum_sq[3] * inv_count));
    stats.rms.temporal_px = sqrtf(float(combined_sum_sq[4] * inv_count));
    stats.rms.grazing_px = sqrtf(float(combined_sum_sq[5] * inv_count));
  }

  if (profile_enabled) {
    g_ocean_camera_lod_region_profile.call_count++;
    g_ocean_camera_lod_region_profile.sample_count += sample_count;
    g_ocean_camera_lod_region_profile.total_s += BLI_time_now_seconds() - profile_start;
  }
  return stats;
}

static void ocean_camera_lod_append_leaf_children(const OceanCameraLODLeaf &leaf,
                                                  Vector<OceanCameraLODLeaf> &r_children)
{
  BLI_assert(leaf.local_level_index > 0);
  BLI_assert((leaf.max_x - leaf.min_x) >= 2);
  BLI_assert((leaf.max_y - leaf.min_y) >= 2);

  const int mid_x = (leaf.min_x + leaf.max_x) / 2;
  const int mid_y = (leaf.min_y + leaf.max_y) / 2;
  const int child_local_level = leaf.local_level_index - 1;
  const int child_split_level = std::max(leaf.split_level_index - 1, 0);
  const int child_stride = std::max(leaf.stride / 2, 1);
  const float child_cell_size = 0.5f * leaf.cell_size;

  const int child_bounds[4][4] = {
      {leaf.min_x, mid_x, leaf.min_y, mid_y},
      {mid_x, leaf.max_x, leaf.min_y, mid_y},
      {leaf.min_x, mid_x, mid_y, leaf.max_y},
      {mid_x, leaf.max_x, mid_y, leaf.max_y},
  };

  for (int child_index = 0; child_index < 4; child_index++) {
    OceanCameraLODLeaf child{};
    child.local_level_index = child_local_level;
    child.split_level_index = child_split_level;
    child.stride = child_stride;
    child.cell_size = child_cell_size;
    child.min_x = child_bounds[child_index][0];
    child.max_x = child_bounds[child_index][1];
    child.min_y = child_bounds[child_index][2];
    child.max_y = child_bounds[child_index][3];
    r_children.append(child);
  }
}

static bool ocean_camera_lod_leafs_overlap_open_interval(const int min_a,
                                                         const int max_a,
                                                         const int min_b,
                                                         const int max_b)
{
  return std::min(max_a, max_b) > std::max(min_a, min_b);
}

static bool ocean_camera_lod_leafs_share_edge(const OceanCameraLODLeaf &a,
                                              const OceanCameraLODLeaf &b,
                                              const OceanCameraLODEdge edge)
{
  switch (edge) {
    case OceanCameraLODEdge::South:
      return a.min_y == b.max_y &&
             ocean_camera_lod_leafs_overlap_open_interval(a.min_x, a.max_x, b.min_x, b.max_x);
    case OceanCameraLODEdge::East:
      return a.max_x == b.min_x &&
             ocean_camera_lod_leafs_overlap_open_interval(a.min_y, a.max_y, b.min_y, b.max_y);
    case OceanCameraLODEdge::North:
      return a.max_y == b.min_y &&
             ocean_camera_lod_leafs_overlap_open_interval(a.min_x, a.max_x, b.min_x, b.max_x);
    case OceanCameraLODEdge::West:
      return a.min_x == b.max_x &&
             ocean_camera_lod_leafs_overlap_open_interval(a.min_y, a.max_y, b.min_y, b.max_y);
  }
  return false;
}

static bool ocean_camera_lod_leaf_can_split(const OceanCameraLODLeaf &leaf)
{
  return leaf.local_level_index > 0 && leaf.stride > 1 && (leaf.max_x - leaf.min_x) >= 2 &&
         (leaf.max_y - leaf.min_y) >= 2;
}

static bool ocean_camera_lod_balance_leaves_once(Vector<OceanCameraLODLeaf> &io_leaves,
                                                 const int dense_cells_x,
                                                 const int dense_cells_y)
{
  if (io_leaves.is_empty()) {
    return false;
  }

  Array<int> leaf_owner_cell_map(size_t(dense_cells_x) * size_t(dense_cells_y));
  leaf_owner_cell_map.as_mutable_span().fill(-1);
  auto dense_cell_index = [&](const int dense_x, const int dense_y) {
    return size_t(dense_y) * size_t(dense_cells_x) + size_t(dense_x);
  };

  for (const int leaf_index : io_leaves.index_range()) {
    const OceanCameraLODLeaf &leaf = io_leaves[leaf_index];
    for (int y = leaf.min_y; y < leaf.max_y; y++) {
      for (int x = leaf.min_x; x < leaf.max_x; x++) {
        leaf_owner_cell_map[dense_cell_index(x, y)] = leaf_index;
      }
    }
  }

  Array<bool> split_leaf(io_leaves.size(), false);
  bool needs_split = false;

  auto has_unbalanced_neighbor = [&](const int leaf_index, const OceanCameraLODEdge edge) {
    const OceanCameraLODLeaf &leaf = io_leaves[leaf_index];
    const bool horizontal = ELEM(edge, OceanCameraLODEdge::South, OceanCameraLODEdge::North);

    auto other_is_too_fine = [&](const int other_index) {
      if (other_index < 0 || other_index == leaf_index) {
        return false;
      }
      const OceanCameraLODLeaf &other = io_leaves[other_index];
      return other.local_level_index < leaf.local_level_index - 1 &&
             ocean_camera_lod_leafs_share_edge(leaf, other, edge);
    };

    if (horizontal) {
      const int neighbor_y = (edge == OceanCameraLODEdge::South) ? leaf.min_y - 1 : leaf.max_y;
      if (neighbor_y < 0 || neighbor_y >= dense_cells_y) {
        return false;
      }
      int x = leaf.min_x;
      while (x < leaf.max_x) {
        const int other_index = leaf_owner_cell_map[dense_cell_index(x, neighbor_y)];
        if (other_is_too_fine(other_index)) {
          return true;
        }
        if (other_index >= 0) {
          const OceanCameraLODLeaf &other = io_leaves[other_index];
          x = std::max(x + 1, std::min(leaf.max_x, other.max_x));
        }
        else {
          x++;
        }
      }
      return false;
    }

    const int neighbor_x = (edge == OceanCameraLODEdge::West) ? leaf.min_x - 1 : leaf.max_x;
    if (neighbor_x < 0 || neighbor_x >= dense_cells_x) {
      return false;
    }
    int y = leaf.min_y;
    while (y < leaf.max_y) {
      const int other_index = leaf_owner_cell_map[dense_cell_index(neighbor_x, y)];
      if (other_is_too_fine(other_index)) {
        return true;
      }
      if (other_index >= 0) {
        const OceanCameraLODLeaf &other = io_leaves[other_index];
        y = std::max(y + 1, std::min(leaf.max_y, other.max_y));
      }
      else {
        y++;
      }
    }
    return false;
  };

  for (const int leaf_index : io_leaves.index_range()) {
    const OceanCameraLODLeaf &leaf = io_leaves[leaf_index];
    if (!ocean_camera_lod_leaf_can_split(leaf)) {
      continue;
    }

    if (has_unbalanced_neighbor(leaf_index, OceanCameraLODEdge::South) ||
        has_unbalanced_neighbor(leaf_index, OceanCameraLODEdge::East) ||
        has_unbalanced_neighbor(leaf_index, OceanCameraLODEdge::North) ||
        has_unbalanced_neighbor(leaf_index, OceanCameraLODEdge::West)) {
      split_leaf[leaf_index] = true;
      needs_split = true;
    }
  }

  if (!needs_split) {
    return false;
  }

  Vector<OceanCameraLODLeaf> balanced;
  balanced.reserve(io_leaves.size());
  for (const int leaf_index : io_leaves.index_range()) {
    const OceanCameraLODLeaf &leaf = io_leaves[leaf_index];
    if (split_leaf[leaf_index]) {
      ocean_camera_lod_append_leaf_children(leaf, balanced);
    }
    else {
      balanced.append(leaf);
    }
  }
  io_leaves = std::move(balanced);
  return true;
}

static void ocean_camera_lod_balance_leaves(Vector<OceanCameraLODLeaf> &io_leaves,
                                            const int dense_cells_x,
                                            const int dense_cells_y)
{
  for (int pass = 0; pass < 32; pass++) {
    if (!ocean_camera_lod_balance_leaves_once(io_leaves, dense_cells_x, dense_cells_y)) {
      return;
    }
  }
}

static void ocean_camera_lod_build_leaves(const OceanModifierData *omd,
                                          const Span<OceanSplitMomentLevel> moment_levels,
                                          const OceanCameraLODSettings &settings,
                                          const OceanSplitRuntimeReadScope *read_scope,
                                          Vector<OceanCameraLODLeaf> &r_leaves)
{
  const bool profile_enabled = ocean_camera_lod_profile_enabled();
  const double profile_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  double select_s = 0.0;
  double balance_s = 0.0;

  r_leaves.clear();
  if (settings.full_domain_dense || settings.levels.is_empty()) {
    return;
  }

  const OceanCameraLODLevel &coarsest_level = settings.levels.last();
  const int root_stride = coarsest_level.stride;
  const float dense_cell_size = ocean_camera_lod_dense_cell_size(settings);
  const float root_cell_size = dense_cell_size * float(root_stride);
  const float region_min_x = settings.domain_min.x;
  const float region_max_x = settings.domain_max.x;
  const float region_min_y = settings.domain_min.y;
  const float region_max_y = settings.domain_max.y;

  auto root_index_min = [&](const float coord, const float domain_min, const int dense_cells) {
    return std::clamp(
        int(floorf((coord - domain_min) / std::max(root_cell_size, 1.0e-6f))) * root_stride,
        0,
        std::max(dense_cells - root_stride, 0));
  };
  auto root_index_max = [&](const float coord,
                            const float domain_min,
                            const int dense_cells,
                            const int min_index) {
    return std::clamp(
        int(ceilf((coord - domain_min) / std::max(root_cell_size, 1.0e-6f))) * root_stride,
        min_index + root_stride,
        dense_cells);
  };

  const int min_x = root_index_min(region_min_x, settings.domain_min.x, settings.dense_cells_x);
  const int max_x = root_index_max(
      region_max_x, settings.domain_min.x, settings.dense_cells_x, min_x);
  const int min_y = root_index_min(region_min_y, settings.domain_min.y, settings.dense_cells_y);
  const int max_y = root_index_max(
      region_max_y, settings.domain_min.y, settings.dense_cells_y, min_y);

  Vector<OceanCameraLODLeaf> pending;
  for (int x = min_x; x < max_x; x += root_stride) {
    for (int y = min_y; y < max_y; y += root_stride) {
      OceanCameraLODLeaf root{};
      root.local_level_index = settings.quadtree_levels - 1;
      root.split_level_index = coarsest_level.split_level_index;
      root.stride = coarsest_level.stride;
      root.cell_size = coarsest_level.cell_size;
      root.min_x = x;
      root.max_x = std::min(x + root_stride, settings.dense_cells_x);
      root.min_y = y;
      root.max_y = std::min(y + root_stride, settings.dense_cells_y);
      pending.append(root);
    }
  }

  while (!pending.is_empty()) {
    OceanCameraLODLeaf leaf = pending.pop_last();
    const float2 leaf_region_min = ocean_camera_lod_leaf_region_min(settings, leaf);
    const float2 leaf_region_max = ocean_camera_lod_leaf_region_max(settings, leaf);
    const bool can_split = ocean_camera_lod_leaf_can_split(leaf);
    const bool protected_anchor = ocean_camera_lod_region_intersects_full_spectrum_anchor(
        settings, leaf_region_min, leaf_region_max);

    if (protected_anchor && can_split) {
      ocean_camera_lod_append_leaf_children(leaf, pending);
      continue;
    }

    if (!can_split) {
      r_leaves.append(leaf);
      continue;
    }

    const bool relevant = ocean_camera_lod_region_is_relevant(
        settings, leaf_region_min, leaf_region_max);
    if (!relevant) {
      r_leaves.append(leaf);
      continue;
    }

    const bool spectrum_ok = ocean_camera_lod_leaf_spectrum_within_resolvable_bound(
        moment_levels, settings, leaf, leaf_region_min, leaf_region_max);
    if (!spectrum_ok) {
      ocean_camera_lod_append_leaf_children(leaf, pending);
      continue;
    }

    const bool directly_visible = ocean_camera_lod_region_intersects_projection_set(
        settings, leaf_region_min, leaf_region_max, 0.0f);
    if (!directly_visible && !protected_anchor &&
        settings.validation_mode != MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT)
    {
      r_leaves.append(leaf);
      continue;
    }

    const OceanLODObservableErrorStats stats = ocean_camera_lod_cell_error_stats(
        omd, moment_levels, settings, read_scope, leaf, leaf_region_min, leaf_region_max);
    if (!stats.valid) {
      leaf.error_stats = stats;
      if (settings.validation_mode == MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT && directly_visible) {
        ocean_camera_lod_append_leaf_children(leaf, pending);
        continue;
      }
      r_leaves.append(leaf);
      continue;
    }

    const bool accepts = spectrum_ok &&
                         ocean_camera_lod_error_within_tolerance(stats,
                                                                 settings.tolerances,
                                                                 settings.validation_mode,
                                                                 settings.usage_mode);
    if (accepts || !can_split) {
      leaf.error_stats = stats;
      r_leaves.append(leaf);
    }
    else {
      ocean_camera_lod_append_leaf_children(leaf, pending);
    }
  }

  if (r_leaves.is_empty()) {
    OceanCameraLODLeaf fallback{};
    fallback.local_level_index = settings.quadtree_levels - 1;
    fallback.split_level_index = coarsest_level.split_level_index;
    fallback.stride = coarsest_level.stride;
    fallback.cell_size = coarsest_level.cell_size;
    fallback.min_x = min_x;
    fallback.max_x = max_x;
    fallback.min_y = min_y;
    fallback.max_y = max_y;
    r_leaves.append(fallback);
  }

  if (profile_enabled) {
    select_s = BLI_time_now_seconds() - profile_start;
  }

  const double balance_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  ocean_camera_lod_balance_leaves(r_leaves, settings.dense_cells_x, settings.dense_cells_y);
  if (profile_enabled) {
    balance_s = BLI_time_now_seconds() - balance_start;
    ocean_camera_lod_profile_logf("<settings>",
                                  "leaf_build",
                                  "selection=sampled total_s=%.6f select_s=%.6f balance_s=%.6f "
                                  "leaves=%d dense_cells=%d",
                                  BLI_time_now_seconds() - profile_start,
                                  select_s,
                                  balance_s,
                                  int(r_leaves.size()),
                                  std::max(settings.dense_cells_x, settings.dense_cells_y));
  }
}

static OceanCameraLODSettings ocean_camera_lod_settings(const ModifierEvalContext *ctx,
                                                        const OceanModifierData *omd,
                                                        const int resolution,
                                                        const char *profile_stage,
                                                        const bool build_leaves)
{
  const bool profile_enabled = ocean_camera_lod_profile_enabled();
  const char *object_name = (ctx != nullptr && ctx->object != nullptr) ? ctx->object->id.name + 2 :
                                                                     "<none>";
  const double profile_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  double projection_s = 0.0;
  double leaf_build_s = 0.0;
  if (profile_enabled) {
    g_ocean_camera_lod_region_profile_active = true;
    g_ocean_camera_lod_region_profile = OceanCameraLODRegionProfile{};
  }

  OceanCameraLODSettings settings{};
  settings.usage_mode = ocean_camera_lod_usage_mode(omd);

  OceanSplitRuntimeReadScope read_scope{};
  const OceanSplitRuntimeReadScope *read_scope_ptr = nullptr;
  if (omd != nullptr && omd->ocean != nullptr &&
      BKE_ocean_split_runtime_read_begin(omd->ocean, &read_scope))
  {
    read_scope_ptr = &read_scope;
  }

  const Vector<OceanSplitMomentLevel> moment_levels = ocean_split_moment_levels(omd, read_scope_ptr);
  const int available_levels = std::max(1, int(moment_levels.size()));
  const int requested_quadtree_levels = std::clamp(int(omd->lod_levels), 1, available_levels);
  const int base_tile_cells = std::max(resolution * resolution, 4);
  const int dense_cells_x = base_tile_cells * ocean_repeat_x(omd);
  const int dense_cells_y = base_tile_cells * ocean_repeat_y(omd);
  const float domain_size = ocean_base_domain_size(omd);
  const float2 domain_min = ocean_repeated_domain_min(omd);
  const float2 domain_max = ocean_repeated_domain_max(omd);
  const float dense_cell_size = domain_size / float(base_tile_cells);

  const float min_wavelength = std::max(BKE_ocean_split_min_wavelength_get(omd->ocean), 1.0e-6f);
  settings.validation_mode = (settings.usage_mode == MOD_OCEAN_LOD_USAGE_STEREO_DATASET) ?
                                 MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT :
                                 ocean_camera_lod_validation_mode(omd);
  settings.tolerances = ocean_camera_lod_tolerances(min_wavelength);
  settings.tolerances.reprojection_px = std::max(omd->lod_pixel_error, 1.0e-4f);

  settings.dense_cells_x = dense_cells_x;
  settings.dense_cells_y = dense_cells_y;
  settings.dense_vert_budget = (dense_cells_x + 1) * (dense_cells_y + 1);
  settings.domain_min = domain_min;
  settings.domain_max = domain_max;
  settings.full_spectrum_radius = (omd->lod_camera_full_spectrum_radius > 0.0f) ?
                                      omd->lod_camera_full_spectrum_radius :
                                      (2.0f * dense_cell_size);

  Scene *scene = (ctx != nullptr) ? DEG_get_input_scene(ctx->depsgraph) : nullptr;
  Object *camera = (scene != nullptr && scene->camera != nullptr) ?
                       DEG_get_evaluated(ctx->depsgraph, scene->camera) :
                       nullptr;
  if (camera == nullptr) {
    settings.error_message = "Camera LOD requires a scene camera; falling back to uniform mesh";
  }
  else if (settings.usage_mode == MOD_OCEAN_LOD_USAGE_STEREO_DATASET &&
           !ocean_camera_lod_has_stereo_dataset_views(scene))
  {
    settings.error_message =
        "Stereo Dataset mode requires Stereo 3D multiview with enabled left and right views; "
        "falling back to uniform mesh";
  }

  const double projection_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  if (ctx != nullptr) {
    settings.projection_set = ocean_camera_projection_set_init(
        scene, ctx->object, camera, settings.usage_mode);
    for (const OceanCameraProjection &projection : settings.projection_set.projections) {
      OceanLODRelevantFootprint footprint;
      if (ocean_camera_projection_visible_footprint(projection, domain_min, domain_max, footprint)) {
        ocean_lod_relevant_footprint_expand(settings.visible_footprint, footprint);
      }
    }
    settings.visible_footprint_guard = ocean_camera_lod_projection_footprint_buffer(moment_levels);
    ocean_lod_relevant_footprint_grow(
        settings.visible_footprint, settings.visible_footprint_guard, domain_min, domain_max);
    settings.projection_set.visible_footprint = settings.visible_footprint;
  }
  if (profile_enabled) {
    projection_s = BLI_time_now_seconds() - projection_start;
  }

  if (settings.projection_set.have_camera_anchor) {
    settings.center = settings.projection_set.camera_anchor;
  }
  else if (ctx != nullptr && camera != nullptr) {
    float world_to_object[4][4];
    invert_m4_m4(world_to_object, ctx->object->object_to_world().ptr());
    float camera_local[3];
    mul_v3_m4v3(camera_local, world_to_object, camera->object_to_world().location());
    settings.center = float2(camera_local[0], camera_local[1]);
  }
  else {
    settings.center = float2(0.0f, 0.0f);
  }

  settings.quadtree_levels = ocean_camera_lod_clamp_level_count(
      dense_cells_x, dense_cells_y, 1, requested_quadtree_levels);

  settings.finest_cell_size = dense_cell_size;
  settings.full_domain_dense = settings.quadtree_levels == 1;
  settings.levels.reserve(settings.quadtree_levels);

  for (int level_index = 0; level_index < settings.quadtree_levels; level_index++) {
    OceanCameraLODLevel level{};
    level.split_level_index = std::min(level_index, available_levels - 1);
    level.stride = 1 << level_index;
    level.cell_size = dense_cell_size * float(level.stride);
    settings.levels.append(level);
  }

  if (!settings.full_domain_dense && settings.visible_footprint.valid && !settings.levels.is_empty()) {
    settings.visible_footprint_guard += settings.levels.last().cell_size;
    ocean_lod_relevant_footprint_grow(
        settings.visible_footprint,
        settings.levels.last().cell_size,
        settings.domain_min,
        settings.domain_max);
    settings.projection_set.visible_footprint = settings.visible_footprint;
  }

  if (build_leaves && !settings.full_domain_dense && settings.projection_set.valid &&
      !moment_levels.is_empty())
  {
    const double leaf_build_begin = profile_enabled ? BLI_time_now_seconds() : 0.0;
    ocean_camera_lod_build_leaves(
        omd, moment_levels.as_span(), settings, read_scope_ptr, settings.leaves);
    if (profile_enabled) {
      leaf_build_s = BLI_time_now_seconds() - leaf_build_begin;
    }
  }

  if (read_scope_ptr != nullptr) {
    BKE_ocean_split_runtime_read_end(&read_scope);
  }

  if (profile_enabled) {
    const OceanCameraLODRegionProfile region_profile = g_ocean_camera_lod_region_profile;
    g_ocean_camera_lod_region_profile_active = false;
    ocean_camera_lod_profile_logf(
        object_name,
        profile_stage ? profile_stage : "settings",
        "mode=%s selection=%s resolution=%d total_s=%.6f projection_s=%.6f leaf_build_s=%.6f "
        "region_stats_s=%.6f region_calls=%d region_samples=%d sample_eval_s=%.6f sample_evals=%d "
        "quadtree_levels=%d dense_budget=%d",
        settings.validation_mode == MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT ? "GEOMETRY_STRICT" :
                                                                              "CAMERA_OBSERVABLE",
        build_leaves ? "sampled" : "cached_topology",
        resolution,
        BLI_time_now_seconds() - profile_start,
        projection_s,
        leaf_build_s,
        region_profile.total_s,
        region_profile.call_count,
        region_profile.sample_count,
        region_profile.sample_eval_s,
        region_profile.sample_eval_count,
        settings.quadtree_levels,
        settings.dense_vert_budget);
  }

  if (ocean_split_debug_enabled()) {
    printf("[OCEAN_CAMERA_LOD_DEBUG] Modifier camera_lod mode=%s usage=%s support_center=(%.6f,%.6f) "
           "camera_anchor=(%.6f,%.6f) settings_center=(%.6f,%.6f) min_wavelength=%.6f dense_cell=%.6f "
           "requested_quadtree_levels=%d available_levels=%d "
           "tolerances=(position_m=%.4f reproj_px=%.4f depth_m=%.4f normal_deg=%.4f "
           "temporal_px=%.4f grazing_px=%.4f)\n",
           settings.validation_mode == MOD_OCEAN_LOD_VALIDATE_GEOMETRY_STRICT ? "GEOMETRY_STRICT" :
                                                                             "CAMERA_OBSERVABLE",
           settings.usage_mode == MOD_OCEAN_LOD_USAGE_STEREO_DATASET ? "STEREO_DATASET" :
                                                                        "GENERAL_RENDER",
           double(settings.projection_set.support_center.x),
           double(settings.projection_set.support_center.y),
           double(settings.projection_set.camera_anchor.x),
           double(settings.projection_set.camera_anchor.y),
           double(settings.center.x),
           double(settings.center.y),
           double(min_wavelength),
           double(dense_cell_size),
           requested_quadtree_levels,
           available_levels,
           double(settings.tolerances.position_m),
           double(settings.tolerances.reprojection_px),
           double(settings.tolerances.depth_m),
           double(RAD2DEGF(settings.tolerances.normal_radians)),
           double(settings.tolerances.temporal_px),
           double(settings.tolerances.grazing_px));
    fflush(stdout);
  }

  ocean_camera_lod_debug_log_settings(omd, settings);
  return settings;
}

static Mesh *generate_ocean_geometry_camera_lod(const ModifierEvalContext *ctx,
                                                OceanModifierData * /*omd*/,
                                                Mesh *mesh_orig,
                                                const int resolution,
                                                const OceanCameraLODSettings &lod_settings,
                                                Array<int> &r_point_levels,
                                                Array<float> &r_point_morph_factors,
                                                Array<float> &r_point_radius)
{
  const bool profile_enabled = ocean_camera_lod_profile_enabled();
  const char *object_name = (ctx != nullptr && ctx->object != nullptr) ? ctx->object->id.name + 2 :
                                                                     "<none>";
  const double profile_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  if (!lod_settings.projection_set.valid) {
    return nullptr;
  }

  const double settings_s = 0.0;
  const int quadtree_levels = lod_settings.quadtree_levels;
  const float2 center = lod_settings.center;
  const int dense_cells_x = lod_settings.dense_cells_x;
  const int dense_cells_y = lod_settings.dense_cells_y;
  const float dense_cell_size = ocean_camera_lod_dense_cell_size(lod_settings);

  Vector<float3> positions;
  Vector<int> face_offsets;
  Vector<int> corner_verts;
  Vector<int> point_levels;
  Vector<float> point_morph_factors;
  Vector<float> point_radius;
  Vector<float> face_leaf_ids;
  Vector<float> face_leaf_levels;
  Vector<float> face_leaf_split_levels;
  Vector<float> face_cell_sizes;
  face_offsets.reserve(lod_settings.leaves.size());
  corner_verts.reserve(lod_settings.leaves.size() * 4);
  face_leaf_ids.reserve(lod_settings.leaves.size());
  face_leaf_levels.reserve(lod_settings.leaves.size());
  face_leaf_split_levels.reserve(lod_settings.leaves.size());
  face_cell_sizes.reserve(lod_settings.leaves.size());
  Array<int> level_face_counts(quadtree_levels, 0);
  Array<int> dense_vertex_map(size_t(dense_cells_x + 1) * size_t(dense_cells_y + 1));
  dense_vertex_map.as_mutable_span().fill(-1);
  Array<int> leaf_owner_cell_map(size_t(dense_cells_x) * size_t(dense_cells_y));
  leaf_owner_cell_map.as_mutable_span().fill(-1);

  auto dense_coord = [&](const int dense_x, const int dense_y) {
    return float2(lod_settings.domain_min.x + (float(dense_x) * dense_cell_size),
                  lod_settings.domain_min.y + (float(dense_y) * dense_cell_size));
  };

  auto dense_index = [&](const int dense_x, const int dense_y) {
    return size_t(dense_y) * size_t(dense_cells_x + 1) + size_t(dense_x);
  };

  auto dense_cell_index = [&](const int dense_x, const int dense_y) {
    return size_t(dense_y) * size_t(dense_cells_x) + size_t(dense_x);
  };

  auto ensure_vertex = [&](const int dense_x,
                           const int dense_y,
                           const int level_index,
                           const float morph_factor) {
    BLI_assert(dense_x >= 0 && dense_x <= dense_cells_x);
    BLI_assert(dense_y >= 0 && dense_y <= dense_cells_y);
    const size_t key = dense_index(dense_x, dense_y);
    int vertex_index = dense_vertex_map[key];
    const float2 coord = dense_coord(dense_x, dense_y);

    if (vertex_index == -1) {
      const float2 delta = coord - center;
      positions.append(float3(coord.x, coord.y, 0.0f));
      point_levels.append(level_index);
      point_morph_factors.append(morph_factor);
      point_radius.append(sqrtf((delta.x * delta.x) + (delta.y * delta.y)));
      vertex_index = positions.index_range().last();
      dense_vertex_map[key] = vertex_index;
      return vertex_index;
    }

    if (level_index < point_levels[vertex_index]) {
      point_levels[vertex_index] = level_index;
      point_morph_factors[vertex_index] = morph_factor;
    }
    else if (level_index == point_levels[vertex_index]) {
      point_morph_factors[vertex_index] = std::min(point_morph_factors[vertex_index], morph_factor);
    }
    return vertex_index;
  };

  auto morph_factor_for_coord = [&](const OceanCameraLODLeaf &leaf,
                                    const float2 &leaf_region_min,
                                    const float2 &leaf_region_max,
                                    const bool south_finer,
                                    const bool east_finer,
                                    const bool north_finer,
                                    const bool west_finer,
                                    const float2 &coord) {
    if (leaf.local_level_index == 0) {
      return 1.0f;
    }

    float distance = FLT_MAX;
    if (south_finer) {
      distance = std::min(distance, coord.y - leaf_region_min.y);
    }
    if (east_finer) {
      distance = std::min(distance, leaf_region_max.x - coord.x);
    }
    if (north_finer) {
      distance = std::min(distance, leaf_region_max.y - coord.y);
    }
    if (west_finer) {
      distance = std::min(distance, coord.x - leaf_region_min.x);
    }

    if (distance == FLT_MAX) {
      return 1.0f;
    }

    const float morph_width = std::max(2.0f * leaf.cell_size, 1.0e-6f);
    return clamp_f(distance / morph_width, 0.0f, 1.0f);
  };

  auto add_face = [&](const Span<int> verts,
                      const int leaf_id,
                      const int local_level_index,
                      const int split_level_index,
                      const float cell_size) {
    BLI_assert(!verts.is_empty());
    face_offsets.append(corner_verts.size());
    corner_verts.extend(verts);
    face_leaf_ids.append(float(leaf_id));
    face_leaf_levels.append(float(local_level_index));
    face_leaf_split_levels.append(float(split_level_index));
    face_cell_sizes.append(cell_size);
  };

  for (const int leaf_index : lod_settings.leaves.index_range()) {
    const OceanCameraLODLeaf &leaf = lod_settings.leaves[leaf_index];
    for (int y = leaf.min_y; y < leaf.max_y; y++) {
      for (int x = leaf.min_x; x < leaf.max_x; x++) {
        leaf_owner_cell_map[dense_cell_index(x, y)] = leaf_index;
      }
    }
  }

  auto edge_split_positions = [&](const int leaf_index, const OceanCameraLODEdge edge) {
    const OceanCameraLODLeaf &leaf = lod_settings.leaves[leaf_index];
    Vector<int> positions_along_edge;
    const bool horizontal = ELEM(edge, OceanCameraLODEdge::South, OceanCameraLODEdge::North);
    positions_along_edge.append(horizontal ? leaf.min_x : leaf.min_y);
    positions_along_edge.append(horizontal ? leaf.max_x : leaf.max_y);

    auto append_neighbor_split = [&](const int other_index) {
      if (other_index < 0) {
        return;
      }
      if (other_index == leaf_index) {
        return;
      }
      const OceanCameraLODLeaf &other = lod_settings.leaves[other_index];
      if (other.local_level_index >= leaf.local_level_index ||
          !ocean_camera_lod_leafs_share_edge(leaf, other, edge))
      {
        return;
      }

      if (horizontal) {
        positions_along_edge.append(std::max(leaf.min_x, other.min_x));
        positions_along_edge.append(std::min(leaf.max_x, other.max_x));
      }
      else {
        positions_along_edge.append(std::max(leaf.min_y, other.min_y));
        positions_along_edge.append(std::min(leaf.max_y, other.max_y));
      }
    };

    if (horizontal) {
      const int neighbor_y = (edge == OceanCameraLODEdge::South) ? leaf.min_y - 1 : leaf.max_y;
      if (neighbor_y >= 0 && neighbor_y < dense_cells_y) {
        int x = leaf.min_x;
        while (x < leaf.max_x) {
          const int other_index = leaf_owner_cell_map[dense_cell_index(x, neighbor_y)];
          append_neighbor_split(other_index);
          if (other_index >= 0) {
            const OceanCameraLODLeaf &other = lod_settings.leaves[other_index];
            x = std::max(x + 1, std::min(leaf.max_x, other.max_x));
          }
          else {
            x++;
          }
        }
      }
    }
    else {
      const int neighbor_x = (edge == OceanCameraLODEdge::West) ? leaf.min_x - 1 : leaf.max_x;
      if (neighbor_x >= 0 && neighbor_x < dense_cells_x) {
        int y = leaf.min_y;
        while (y < leaf.max_y) {
          const int other_index = leaf_owner_cell_map[dense_cell_index(neighbor_x, y)];
          append_neighbor_split(other_index);
          if (other_index >= 0) {
            const OceanCameraLODLeaf &other = lod_settings.leaves[other_index];
            y = std::max(y + 1, std::min(leaf.max_y, other.max_y));
          }
          else {
            y++;
          }
        }
      }
    }

    std::sort(positions_along_edge.begin(), positions_along_edge.end());
    Vector<int> unique_positions;
    int last_position = INT_MIN;
    for (const int position : positions_along_edge) {
      if (unique_positions.is_empty() || position != last_position) {
        unique_positions.append(position);
        last_position = position;
      }
    }
    return unique_positions;
  };

  const double topology_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  for (const int leaf_index : lod_settings.leaves.index_range()) {
    const OceanCameraLODLeaf &leaf = lod_settings.leaves[leaf_index];
    const int split_level_index = leaf.split_level_index;
    const int dense_x0 = leaf.min_x;
    const int dense_x1 = leaf.max_x;
    const int dense_y0 = leaf.min_y;
    const int dense_y1 = leaf.max_y;
    if (dense_x1 <= dense_x0 || dense_y1 <= dense_y0) {
      continue;
    }
    const float2 leaf_region_min = ocean_camera_lod_leaf_region_min(lod_settings, leaf);
    const float2 leaf_region_max = ocean_camera_lod_leaf_region_max(lod_settings, leaf);
    const Vector<int> south_x = edge_split_positions(leaf_index, OceanCameraLODEdge::South);
    const Vector<int> east_y = edge_split_positions(leaf_index, OceanCameraLODEdge::East);
    const Vector<int> north_x = edge_split_positions(leaf_index, OceanCameraLODEdge::North);
    const Vector<int> west_y = edge_split_positions(leaf_index, OceanCameraLODEdge::West);
    const bool south_finer = south_x.size() > 2;
    const bool east_finer = east_y.size() > 2;
    const bool north_finer = north_x.size() > 2;
    const bool west_finer = west_y.size() > 2;

    Vector<int> face_verts;
    auto append_face_vertex = [&](const int dense_x, const int dense_y, const float morph_factor) {
      face_verts.append(ensure_vertex(dense_x, dense_y, split_level_index, morph_factor));
    };

    for (const int x_coord : south_x) {
      const float2 coord = dense_coord(x_coord, dense_y0);
      const float morph_factor = (x_coord == dense_x0 || x_coord == dense_x1) ?
                                     morph_factor_for_coord(leaf,
                                                            leaf_region_min,
                                                            leaf_region_max,
                                                            south_finer,
                                                            east_finer,
                                                            north_finer,
                                                            west_finer,
                                                            coord) :
                                     0.0f;
      append_face_vertex(x_coord, dense_y0, morph_factor);
    }
    for (int edge_index = 1; edge_index < east_y.size(); edge_index++) {
      const int y_coord = east_y[edge_index];
      const float2 coord = dense_coord(dense_x1, y_coord);
      const float morph_factor = (y_coord == dense_y1) ?
                                     morph_factor_for_coord(leaf,
                                                            leaf_region_min,
                                                            leaf_region_max,
                                                            south_finer,
                                                            east_finer,
                                                            north_finer,
                                                            west_finer,
                                                            coord) :
                                     0.0f;
      append_face_vertex(dense_x1, y_coord, morph_factor);
    }
    for (int edge_index = north_x.size() - 2; edge_index >= 0; edge_index--) {
      const int x_coord = north_x[edge_index];
      const float2 coord = dense_coord(x_coord, dense_y1);
      const float morph_factor = (x_coord == dense_x0) ?
                                     morph_factor_for_coord(leaf,
                                                            leaf_region_min,
                                                            leaf_region_max,
                                                            south_finer,
                                                            east_finer,
                                                            north_finer,
                                                            west_finer,
                                                            coord) :
                                     0.0f;
      append_face_vertex(x_coord, dense_y1, morph_factor);
    }
    for (int edge_index = west_y.size() - 2; edge_index > 0; edge_index--) {
      append_face_vertex(dense_x0, west_y[edge_index], 0.0f);
    }

    add_face(face_verts.as_span(),
             leaf_index,
             leaf.local_level_index,
             leaf.split_level_index,
             leaf.cell_size);
    level_face_counts[leaf.local_level_index]++;
  }
  const double topology_s = profile_enabled ? (BLI_time_now_seconds() - topology_start) : 0.0;

  BLI_assert(int(positions.size()) <= lod_settings.dense_vert_budget);
  if (ocean_split_debug_enabled()) {
    const int owner_levels = point_levels.is_empty() ?
                                 0 :
                                 (*std::max_element(point_levels.begin(), point_levels.end()) + 1);
    Array<int> owner_vertex_counts(owner_levels, 0);
    Array<int> owner_face_counts(owner_levels, 0);
    for (const int vertex : point_levels.index_range()) {
      const int owner_level = std::clamp(point_levels[vertex], 0, std::max(owner_levels - 1, 0));
      owner_vertex_counts[owner_level]++;
    }
    for (const int local_level : lod_settings.levels.index_range()) {
      const int split_level = lod_settings.levels[local_level].split_level_index;
      if (split_level >= 0 && split_level < owner_face_counts.size()) {
        owner_face_counts[split_level] += level_face_counts[local_level];
      }
    }

    printf("[OCEAN_CAMERA_LOD_DEBUG] Modifier camera_lod budget dense_reference_verts=%d "
           "predicted_max_verts=%d actual_verts=%d actual_faces=%d invariant=%s\n",
           lod_settings.dense_vert_budget,
           lod_settings.dense_vert_budget,
           int(positions.size()),
           int(face_offsets.size()),
           (int(positions.size()) <= lod_settings.dense_vert_budget) ? "ok" : "violated");
    for (const int level_index : owner_vertex_counts.index_range()) {
      printf("[OCEAN_CAMERA_LOD_DEBUG] Modifier camera_lod level=%d emitted_owner_verts=%d "
             "emitted_faces=%d\n",
             level_index,
             owner_vertex_counts[level_index],
             owner_face_counts[level_index]);
    }
    fflush(stdout);
  }

  const double finalize_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  Mesh *result = BKE_mesh_new_nomain(
      positions.size(), 0, face_offsets.size(), corner_verts.size());
  BKE_mesh_copy_parameters_for_eval(result, mesh_orig);
  result->vert_positions_for_write().copy_from(positions.as_span());
  auto result_face_offsets = result->face_offsets_for_write();
  result_face_offsets.drop_back(1).copy_from(face_offsets.as_span());
  result_face_offsets.last() = corner_verts.size();
  result->corner_verts_for_write().copy_from(corner_verts.as_span());
  blender::bke::mesh_calc_edges(*result, false, false);

  if (!face_offsets.is_empty()) {
    BLI_assert(face_leaf_ids.size() == face_offsets.size());
    BLI_assert(face_leaf_levels.size() == face_offsets.size());
    BLI_assert(face_leaf_split_levels.size() == face_offsets.size());
    BLI_assert(face_cell_sizes.size() == face_offsets.size());

    blender::bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    auto write_face_attribute = [&](const char *name, const Span<float> values) {
      blender::bke::SpanAttributeWriter<float> attr =
          attributes.lookup_or_add_for_write_only_span<float>(name, blender::bke::AttrDomain::Face);
      if (!attr) {
        return;
      }
      attr.span.copy_from(values);
      attr.finish();
    };
    write_face_attribute(OCEAN_ATTR_CAMERA_LOD_LEAF_ID, face_leaf_ids.as_span());
    write_face_attribute(OCEAN_ATTR_CAMERA_LOD_LEAF_LEVEL, face_leaf_levels.as_span());
    write_face_attribute(OCEAN_ATTR_CAMERA_LOD_LEAF_SPLIT_LEVEL, face_leaf_split_levels.as_span());
    write_face_attribute(OCEAN_ATTR_CAMERA_LOD_CELL_SIZE, face_cell_sizes.as_span());

    ocean_camera_lod_set_mesh_string_property(
        *result, OCEAN_PROP_CAMERA_LOD_CONTRACT, OCEAN_CAMERA_LOD_CONTRACT_ADAPTIVE_LEAF);
    ocean_camera_lod_set_mesh_string_property(
        *result, OCEAN_PROP_CAMERA_LOD_LAYOUT, OCEAN_CAMERA_LOD_LAYOUT_ADAPTIVE_LEAF);
  }

  r_point_levels.reinitialize(positions.size());
  r_point_morph_factors.reinitialize(positions.size());
  r_point_radius.reinitialize(positions.size());
  r_point_levels.as_mutable_span().copy_from(point_levels.as_span());
  r_point_morph_factors.as_mutable_span().copy_from(point_morph_factors.as_span());
  r_point_radius.as_mutable_span().copy_from(point_radius.as_span());

  if (profile_enabled) {
    ocean_camera_lod_profile_logf(
        object_name,
        "geometry_generate",
        "resolution=%d total_s=%.6f settings_s=%.6f topology_s=%.6f finalize_s=%.6f "
        "quadtree_levels=%d verts=%d faces=%d dense_budget=%d",
        resolution,
        BLI_time_now_seconds() - profile_start,
        settings_s,
        topology_s,
        BLI_time_now_seconds() - finalize_start,
        quadtree_levels,
        int(positions.size()),
        int(face_offsets.size()),
        lod_settings.dense_vert_budget);
  }

  return result;
}

static bool ocean_modifier_runtime_camera_lod_topology_cache_matches(
    const OceanModifierRuntimeData &runtime_data, const OceanCameraLODMeshCacheKey &key)
{
  return runtime_data.camera_lod_topology_key_valid &&
         runtime_data.camera_lod_topology_template != nullptr &&
         ocean_camera_lod_topology_cache_keys_equal(runtime_data.camera_lod_topology_key, key) &&
         runtime_data.camera_lod_topology_levels.size() ==
             runtime_data.camera_lod_topology_template->verts_num &&
         runtime_data.camera_lod_topology_morph_factors.size() ==
             runtime_data.camera_lod_topology_template->verts_num &&
         runtime_data.camera_lod_topology_radii.size() ==
             runtime_data.camera_lod_topology_template->verts_num;
}

static Mesh *generate_ocean_geometry_camera_lod_cached_topology(
    const ModifierEvalContext *ctx,
    Mesh *mesh_orig,
    const OceanModifierRuntimeData &runtime_data,
    const int resolution,
    Array<int> &r_point_levels,
    Array<float> &r_point_morph_factors,
    Array<float> &r_point_radius)
{
  const bool profile_enabled = ocean_camera_lod_profile_enabled();
  const char *object_name = (ctx != nullptr && ctx->object != nullptr) ? ctx->object->id.name + 2 :
                                                                     "<none>";
  const double profile_start = profile_enabled ? BLI_time_now_seconds() : 0.0;

  Mesh *result = BKE_mesh_copy_for_eval(*runtime_data.camera_lod_topology_template);
  BKE_mesh_copy_parameters_for_eval(result, mesh_orig);
  ocean_camera_lod_array_copy(runtime_data.camera_lod_topology_levels.as_span(), r_point_levels);
  ocean_camera_lod_array_copy(runtime_data.camera_lod_topology_morph_factors.as_span(),
                              r_point_morph_factors);
  ocean_camera_lod_array_copy(runtime_data.camera_lod_topology_radii.as_span(), r_point_radius);

  if (profile_enabled) {
    ocean_camera_lod_profile_logf(
        object_name,
        "geometry_generate_cache_hit",
        "resolution=%d total_s=%.6f verts=%d faces=%d",
        resolution,
        BLI_time_now_seconds() - profile_start,
        result ? result->verts_num : 0,
        result ? result->faces_num : 0);
  }
  return result;
}

static void ocean_modifier_runtime_store_camera_lod_topology_cache(
    const ModifierEvalContext *ctx,
    OceanModifierRuntimeData &runtime_data,
    const OceanCameraLODMeshCacheKey &key,
    const Mesh &topology_mesh,
    const Span<int> point_levels,
    const Span<float> point_morph_factors,
    const Span<float> point_radius)
{
  const bool profile_enabled = ocean_camera_lod_profile_enabled();
  const char *object_name = (ctx != nullptr && ctx->object != nullptr) ? ctx->object->id.name + 2 :
                                                                     "<none>";
  const double profile_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  ocean_modifier_runtime_free_camera_lod_topology_cache(runtime_data);
  runtime_data.camera_lod_topology_template = BKE_mesh_copy_for_eval(topology_mesh);
  if (runtime_data.camera_lod_topology_template == nullptr) {
    return;
  }
  runtime_data.camera_lod_topology_key = key;
  runtime_data.camera_lod_topology_key_valid = true;
  ocean_camera_lod_array_copy(point_levels, runtime_data.camera_lod_topology_levels);
  ocean_camera_lod_array_copy(point_morph_factors,
                              runtime_data.camera_lod_topology_morph_factors);
  ocean_camera_lod_array_copy(point_radius, runtime_data.camera_lod_topology_radii);
  if (profile_enabled) {
    ocean_camera_lod_profile_logf(
        object_name,
        "topology_cache_store",
        "total_s=%.6f verts=%d faces=%d",
        BLI_time_now_seconds() - profile_start,
        topology_mesh.verts_num,
        topology_mesh.faces_num);
  }
}

static Mesh *ocean_modifier_runtime_camera_lod_cache_lookup(
    const OceanModifierRuntimeData &runtime_data,
    const OceanCameraLODMeshCacheKey &key,
    Mesh *mesh_orig)
{
  if (!runtime_data.camera_lod_cached_key_valid ||
      runtime_data.camera_lod_cached_result == nullptr ||
      !ocean_camera_lod_cache_keys_equal(runtime_data.camera_lod_cached_key, key))
  {
    return nullptr;
  }

  Mesh *cached_result = BKE_mesh_copy_for_eval(*runtime_data.camera_lod_cached_result);
  BKE_mesh_copy_parameters_for_eval(cached_result, mesh_orig);
  return cached_result;
}

static Mesh *doOcean(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  OceanModifierData *omd = (OceanModifierData *)md;
  if (omd->ocean && !BKE_ocean_is_valid(omd->ocean)) {
    BKE_modifier_set_error(ctx->object, md, "Failed to allocate memory");
    return mesh;
  }
  int cfra_scene = int(DEG_get_ctime(ctx->depsgraph));
  Object *ob = ctx->object;
  const bool profile_enabled = ocean_camera_lod_profile_enabled();
  const char *object_name = (ob != nullptr) ? ob->id.name + 2 : "<none>";
  const double profile_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  double simulation_s = 0.0;
  double geometry_generate_s = 0.0;
  double foam_s = 0.0;
  double settings_requery_s = 0.0;
  double support_prepare_s = 0.0;
  double displacement_s = 0.0;
  double finish_s = 0.0;
  double cleanup_s = 0.0;
  int runtime_sample_calls = 0;
  int runtime_morph_blend_calls = 0;
  int split_support_eval_calls = 0;
  bool camera_lod_dense_fast_path = false;
  bool allocated_ocean = false;
  const bool requested_camera_lod = ocean_use_camera_lod(omd);
  const char *camera_lod_dense_fallback_reason = nullptr;
  const bool use_camera_lod =
      requested_camera_lod &&
      !ocean_camera_lod_requires_dense_fallback(ctx, omd, &camera_lod_dense_fallback_reason);
  const bool camera_lod_cycles_shading = use_camera_lod &&
                                         ocean_camera_lod_uses_cycles_shading(ctx);
  if (requested_camera_lod && !use_camera_lod && camera_lod_dense_fallback_reason != nullptr) {
    BKE_modifier_set_error(ctx->object, md, "%s", camera_lod_dense_fallback_reason);
  }

  Mesh *result = nullptr;
  OceanResult ocr;

  const int resolution = (ctx->flag & MOD_APPLY_RENDER) ? omd->resolution :
                                                          omd->viewport_resolution;

  int cfra_for_cache;
  int i, j;
  Array<int> camera_lod_levels;
  Array<float> camera_lod_morph_factors;
  Array<float> camera_lod_radii;
  OceanCameraLODSettings camera_lod_settings;
  OceanModifierRuntimeData *camera_lod_runtime_data = nullptr;
  OceanCameraLODMeshCacheKey camera_lod_cache_key{};
  bool camera_lod_cache_enabled = false;
  OceanCameraLODMeshCacheKey camera_lod_topology_cache_key{};
  bool camera_lod_topology_cache_enabled = false;
  bool camera_lod_topology_cache_hit = false;

  /* use cached & inverted value for speed
   * expanded this would read...
   *
   * (axis / (omd->size * omd->spatial_size)) + 0.5f) */
#  define OCEAN_CO(_size_co_inv, _v) ((_v * _size_co_inv) + 0.5f)

  const float size_co_inv = 1.0f / (omd->size * omd->spatial_size);

  /* can happen in when size is small, avoid bad array lookups later and quit now */
  if (!isfinite(size_co_inv)) {
    return mesh;
  }

  if (use_camera_lod && omd->geometry_mode == MOD_OCEAN_GEOM_GENERATE) {
    camera_lod_runtime_data = ocean_ensure_runtime_data(ocean_modifier_cache_owner(md, ctx));
    camera_lod_cache_enabled = ocean_camera_lod_cache_key_init(
        ctx, omd, resolution, cfra_scene, camera_lod_cache_key);
    camera_lod_topology_cache_enabled = ocean_camera_lod_topology_cache_key_init(
        ctx, omd, resolution, camera_lod_topology_cache_key);
    if (camera_lod_cache_enabled) {
      Mesh *cached_result = ocean_modifier_runtime_camera_lod_cache_lookup(
          *camera_lod_runtime_data, camera_lod_cache_key, mesh);
      if (cached_result != nullptr) {
        if (profile_enabled) {
          ocean_camera_lod_profile_logf(
              object_name,
              "modifier_cache_hit",
              "mode=camera_lod total_s=%.6f simulation_s=%.6f cleanup_s=%.6f "
              "verts=%d faces=%d",
              BLI_time_now_seconds() - profile_start,
              simulation_s,
              cleanup_s,
              cached_result->verts_num,
              cached_result->faces_num);
        }
        return cached_result;
      }
      ocean_modifier_runtime_free_camera_lod_cache(*camera_lod_runtime_data);
    }
  }

  /* do ocean simulation */
  const double simulation_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
  if (omd->cached && !use_camera_lod) {
    if (!omd->oceancache) {
      init_cache_data(ob, omd, resolution);
    }
    BKE_ocean_simulate_cache(omd->oceancache, cfra_scene);
  }
  else {
    /* omd->ocean is nullptr on an original object (in contrast to an evaluated one).
     * We can create a new one, but we have to free it as well once we're done.
     * This function is only called on an original object when applying the modifier
     * using the 'Apply Modifier' button, and thus it is not called frequently for
     * simulation. */
    allocated_ocean |= BKE_ocean_ensure(omd, resolution);
    simulate_ocean_modifier(omd);
  }
  if (profile_enabled) {
    simulation_s = BLI_time_now_seconds() - simulation_start;
  }

  if (use_camera_lod && omd->geometry_mode == MOD_OCEAN_GEOM_GENERATE) {
    if (camera_lod_topology_cache_enabled) {
      camera_lod_topology_cache_hit = ocean_modifier_runtime_camera_lod_topology_cache_matches(
          *camera_lod_runtime_data, camera_lod_topology_cache_key);
      if (!camera_lod_topology_cache_hit) {
        ocean_modifier_runtime_free_camera_lod_topology_cache(*camera_lod_runtime_data);
      }
    }
  }

  if (use_camera_lod) {
    const double settings_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    camera_lod_settings = ocean_camera_lod_settings(ctx,
                                                    omd,
                                                    resolution,
                                                    camera_lod_topology_cache_hit ?
                                                        "settings_topology_cache_hit" :
                                                        "settings",
                                                    !camera_lod_topology_cache_hit);
    if (profile_enabled) {
      settings_requery_s = BLI_time_now_seconds() - settings_start;
    }
  }

  if (omd->geometry_mode == MOD_OCEAN_GEOM_GENERATE) {
    const double geometry_generate_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    if (use_camera_lod) {
      if (!camera_lod_settings.projection_set.valid) {
        BKE_modifier_set_error(
            ctx->object,
            md,
            "%s",
            camera_lod_settings.error_message ?
                camera_lod_settings.error_message :
                "Camera LOD requires valid camera projections; falling back to uniform mesh");
        result = generate_ocean_geometry(omd, mesh, resolution, false);
      }
      else {
        camera_lod_dense_fast_path = ocean_camera_lod_uses_dense_generate_fast_path(
            camera_lod_settings);
        if (camera_lod_dense_fast_path) {
          result = generate_ocean_geometry_camera_lod_dense(
              omd,
              mesh,
              camera_lod_settings,
              camera_lod_levels,
              camera_lod_morph_factors,
              camera_lod_radii);
        }
        else if (camera_lod_topology_cache_hit && camera_lod_runtime_data != nullptr) {
          result = generate_ocean_geometry_camera_lod_cached_topology(ctx,
                                                                      mesh,
                                                                      *camera_lod_runtime_data,
                                                                      resolution,
                                                                      camera_lod_levels,
                                                                      camera_lod_morph_factors,
                                                                      camera_lod_radii);
        }
        else {
          result = generate_ocean_geometry_camera_lod(
              ctx,
              omd,
              mesh,
              resolution,
              camera_lod_settings,
              camera_lod_levels,
              camera_lod_morph_factors,
              camera_lod_radii);
          if (result == nullptr) {
            BKE_modifier_set_error(ctx->object,
                                   md,
                                   "%s",
                                   camera_lod_settings.error_message ?
                                       camera_lod_settings.error_message :
                                       "Camera LOD requires a scene camera; falling back to "
                                       "uniform mesh");
            result = generate_ocean_geometry(omd, mesh, resolution, false);
          }
        }
        if (result != nullptr && !camera_lod_dense_fast_path && !camera_lod_topology_cache_hit &&
            camera_lod_topology_cache_enabled && camera_lod_runtime_data != nullptr &&
            camera_lod_levels.size() == result->verts_num &&
            camera_lod_morph_factors.size() == result->verts_num &&
            camera_lod_radii.size() == result->verts_num)
        {
          ocean_modifier_runtime_store_camera_lod_topology_cache(
              ctx,
              *camera_lod_runtime_data,
              camera_lod_topology_cache_key,
              *result,
              camera_lod_levels.as_span(),
              camera_lod_morph_factors.as_span(),
              camera_lod_radii.as_span());
        }
      }
    }
    else {
      result = generate_ocean_geometry(omd, mesh, resolution, false);
    }
    if (profile_enabled) {
      geometry_generate_s = BLI_time_now_seconds() - geometry_generate_start;
    }
  }
  else if (omd->geometry_mode == MOD_OCEAN_GEOM_DISPLACE) {
    const double geometry_generate_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    result = (Mesh *)BKE_id_copy_ex(nullptr, &mesh->id, nullptr, LIB_ID_COPY_LOCALIZE);
    if (profile_enabled) {
      geometry_generate_s = BLI_time_now_seconds() - geometry_generate_start;
    }
  }

  cfra_for_cache = cfra_scene;
  CLAMP(cfra_for_cache, omd->bakestart, omd->bakeend);
  cfra_for_cache -= omd->bakestart; /* shift to 0 based */

  MutableSpan<float3> positions = result->vert_positions_for_write();
  Array<float3> reference_positions(result->verts_num);
  reference_positions.as_mutable_span().copy_from(positions);
  const OffsetIndices faces = result->faces();
  const Span<int> corner_verts = result->corner_verts();
  const bool use_camera_lod_mesh = use_camera_lod && camera_lod_levels.size() == result->verts_num &&
                                   camera_lod_morph_factors.size() == result->verts_num;

  Array<OceanSplitSupport> geometry_supports;
  Array<float3> custom_normals;
  Array<float> split_debug_level_values;
  Array<float> split_debug_radius_values;
  Array<OceanSplitSupportDebugStats> split_debug_level_stats;
  OceanSplitSupportDebugStats split_debug_all_stats;
  const bool split_debug_enabled = use_camera_lod && ocean_split_debug_enabled();
  bool split_debug_has_level_attr = false;
  bool split_debug_has_radius_attr = false;

  bke::SpanAttributeWriter<float3> ref_coord_attr;
  bke::SpanAttributeWriter<float3> geometry_normal_attr;
  bke::SpanAttributeWriter<float3> geometry_support_cov_attr;
  bke::SpanAttributeWriter<float2> ref_uv_attr;
  bke::SpanAttributeWriter<float> camera_lod_level_attr;
  bke::SpanAttributeWriter<float> camera_lod_split_level_attr;
  bke::SpanAttributeWriter<float> camera_lod_morph_attr;
  bke::SpanAttributeWriter<float> camera_lod_radius_attr;

  if (use_camera_lod) {
    const double support_prepare_local_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    geometry_supports = use_camera_lod_mesh ?
                           Array<OceanSplitSupport>(result->verts_num) :
                           ocean_geometry_supports(omd, *result, reference_positions, resolution);
    if (!camera_lod_cycles_shading) {
      custom_normals.reinitialize(result->verts_num);
    }

    if (split_debug_enabled && use_camera_lod_mesh) {
      split_debug_level_values.reinitialize(result->verts_num);
      split_debug_radius_values.reinitialize(result->verts_num);
      for (const int vert : split_debug_level_values.index_range()) {
        split_debug_level_values[vert] = float(camera_lod_levels[vert]);
        split_debug_radius_values[vert] = camera_lod_radii[vert];
      }
      split_debug_has_level_attr = true;
      split_debug_has_radius_attr = true;
      split_debug_level_stats.reinitialize(*std::max_element(camera_lod_levels.begin(),
                                                             camera_lod_levels.end()) +
                                           1);
    }
    if (profile_enabled) {
      support_prepare_s += BLI_time_now_seconds() - support_prepare_local_start;
    }
  }

  /* Add vertex-colors before displacement: allows lookup based on position. */

  if (omd->flag & MOD_OCEAN_GENERATE_FOAM) {
    const double foam_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    AttributeOwner owner = AttributeOwner::from_id(&result->id);
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    bke::SpanAttributeWriter mloopcols = attributes.lookup_or_add_for_write_span<ColorGeometry4b>(
        BKE_attribute_calc_unique_name(owner, omd->foamlayername), bke::AttrDomain::Corner);

    bke::SpanAttributeWriter<ColorGeometry4b> mloopcols_spray;
    if (omd->flag & MOD_OCEAN_GENERATE_SPRAY) {
      mloopcols_spray = attributes.lookup_or_add_for_write_span<ColorGeometry4b>(
          BKE_attribute_calc_unique_name(owner, omd->spraylayername), bke::AttrDomain::Corner);
    }

    if (mloopcols) { /* unlikely to fail */

      for (const int i : faces.index_range()) {
        const IndexRange face = faces[i];
        const int *corner_vert = &corner_verts[face.start()];
        ColorGeometry4b *mlcol = &mloopcols.span[face.start()];

        ColorGeometry4b *mlcolspray = nullptr;
        if ((omd->flag & MOD_OCEAN_GENERATE_SPRAY) && mloopcols_spray) {
          mlcolspray = &mloopcols_spray.span[face.start()];
        }

        for (j = face.size(); j--; corner_vert++, mlcol++) {
          const float *vco = reference_positions[*corner_vert];
          const float u = OCEAN_CO(size_co_inv, vco[0]);
          const float v = OCEAN_CO(size_co_inv, vco[1]);
          float foam;

          if (omd->oceancache && omd->cached && !use_camera_lod) {
            BKE_ocean_cache_eval_uv(omd->oceancache, &ocr, cfra_for_cache, u, v);
            foam = ocr.foam;
            CLAMP(foam, 0.0f, 1.0f);
          }
          else {
            BKE_ocean_eval_uv(omd->ocean, &ocr, u, v);
            foam = BKE_ocean_jminus_to_foam(ocr.Jminus, omd->foam_coverage);
          }

          mlcol->r = mlcol->g = mlcol->b = char(foam * 255);
          /* This needs to be set (render engine uses) */
          mlcol->a = 255;

          if (mlcolspray != nullptr) {
            if (omd->flag & MOD_OCEAN_INVERT_SPRAY) {
              mlcolspray->r = ocr.Eminus[0] * 255;
            }
            else {
              mlcolspray->r = ocr.Eplus[0] * 255;
            }
            mlcolspray->g = 0;
            if (omd->flag & MOD_OCEAN_INVERT_SPRAY) {
              mlcolspray->b = ocr.Eminus[2] * 255;
            }
            else {
              mlcolspray->b = ocr.Eplus[2] * 255;
            }
            mlcolspray->a = 255;
            mlcolspray++;
          }
        }
      }
    }

    mloopcols.finish();
    mloopcols_spray.finish();
    if (profile_enabled) {
      foam_s = BLI_time_now_seconds() - foam_start;
    }
  }

  if (use_camera_lod) {
    const double support_prepare_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    bke::MutableAttributeAccessor attributes = result->attributes_for_write();
    ref_coord_attr = attributes.lookup_or_add_for_write_only_span<float3>(OCEAN_ATTR_REF_COORD,
                                                                          bke::AttrDomain::Point);
    geometry_normal_attr = attributes.lookup_or_add_for_write_only_span<float3>(
        OCEAN_ATTR_GEOMETRY_NORMAL, bke::AttrDomain::Point);
    geometry_support_cov_attr = attributes.lookup_or_add_for_write_only_span<float3>(
        OCEAN_ATTR_GEOMETRY_SUPPORT_COV, bke::AttrDomain::Point);
    ref_uv_attr = attributes.lookup_or_add_for_write_only_span<float2>(
        OCEAN_ATTR_REF_UV, bke::AttrDomain::Point);
    if (use_camera_lod_mesh) {
      camera_lod_level_attr = attributes.lookup_or_add_for_write_only_span<float>(
          OCEAN_ATTR_CAMERA_LOD_LEVEL, bke::AttrDomain::Point);
      camera_lod_split_level_attr = attributes.lookup_or_add_for_write_only_span<float>(
          OCEAN_ATTR_CAMERA_LOD_SPLIT_LEVEL, bke::AttrDomain::Point);
      camera_lod_morph_attr = attributes.lookup_or_add_for_write_only_span<float>(
          OCEAN_ATTR_CAMERA_LOD_MORPH, bke::AttrDomain::Point);
      camera_lod_radius_attr = attributes.lookup_or_add_for_write_only_span<float>(
          OCEAN_ATTR_CAMERA_LOD_RADIUS, bke::AttrDomain::Point);
    }
    if (profile_enabled) {
      support_prepare_s += BLI_time_now_seconds() - support_prepare_start;
    }
  }

  /* displace the geometry */

  /* NOTE: tried to parallelized that one and previous foam loop,
   * but gives 20% slower results... odd. */
  {
    const double displacement_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    const int verts_num = result->verts_num;
    OceanSplitRuntimeReadScope split_read_scope{};
    const OceanSplitRuntimeReadScope *split_read_scope_ptr = nullptr;
    if (use_camera_lod_mesh && BKE_ocean_split_runtime_read_begin(omd->ocean, &split_read_scope)) {
      split_read_scope_ptr = &split_read_scope;
    }

    for (i = 0; i < verts_num; i++) {
      float *vco = positions[i];
      const float3 reference_co = reference_positions[i];
      const float2 ref_uv = ocean_reference_uv(reference_co, size_co_inv);

      if (use_camera_lod) {
        if (ref_coord_attr) {
          ref_coord_attr.span[i] = ocean_reference_coord(reference_co, omd);
        }
        if (ref_uv_attr) {
          ref_uv_attr.span[i] = ref_uv;
        }
        OceanSplitSupport geometry_support = geometry_supports[i];
        float geometry_disp[3] = {0.0f, 0.0f, 0.0f};
        float geometry_normal[3] = {0.0f, 1.0f, 0.0f};
        int split_level_index = 0;
        int local_level_index = 0;
        float morph_factor = 1.0f;

        if (use_camera_lod_mesh) {
          runtime_sample_calls++;
          split_level_index = std::clamp(
              camera_lod_levels[i], 0, std::max(BKE_ocean_split_level_count_get(omd->ocean) - 1, 0));
          local_level_index = split_level_index;
          const float cell_size = (local_level_index < camera_lod_settings.levels.size()) ?
                                      camera_lod_settings.levels[local_level_index].cell_size :
                                      (camera_lod_settings.finest_cell_size *
                                       exp2f(float(local_level_index)));
          const float support_wavelength = std::max(
              2.0f * std::max(cell_size, 1.0e-6f),
              BKE_ocean_split_min_wavelength_get(omd->ocean) * exp2f(float(split_level_index)));
          geometry_support = ocean_support_isotropic(support_wavelength);

          if (split_read_scope_ptr != nullptr) {
            BKE_ocean_split_runtime_sample_level_in_scope(
                split_read_scope_ptr, split_level_index, ref_uv.x, ref_uv.y, geometry_disp, geometry_normal);
          }
          else {
            BKE_ocean_split_runtime_sample_level(
                omd->ocean, split_level_index, ref_uv.x, ref_uv.y, geometry_disp, geometry_normal);
          }

          morph_factor = camera_lod_morph_factors[i];
          if (split_level_index > 0 && morph_factor < 1.0f) {
            runtime_sample_calls++;
            runtime_morph_blend_calls++;
            float finer_disp[3] = {0.0f, 0.0f, 0.0f};
            float finer_normal[3] = {0.0f, 1.0f, 0.0f};
            if (split_read_scope_ptr != nullptr) {
              BKE_ocean_split_runtime_sample_level_in_scope(
                  split_read_scope_ptr, split_level_index - 1, ref_uv.x, ref_uv.y, finer_disp, finer_normal);
            }
            else {
              BKE_ocean_split_runtime_sample_level(
                  omd->ocean, split_level_index - 1, ref_uv.x, ref_uv.y, finer_disp, finer_normal);
            }
            interp_v3_v3v3(geometry_disp, finer_disp, geometry_disp, morph_factor);
            interp_v3_v3v3(geometry_normal, finer_normal, geometry_normal, morph_factor);
            normalize_v3(geometry_normal);
          }
        }
        else {
          split_support_eval_calls++;
          OceanSplitResult split_result;
          BKE_ocean_eval_uv_split_support(
              omd->ocean, &split_result, ref_uv.x, ref_uv.y, &geometry_support, &geometry_support);
          copy_v3_v3(geometry_disp, split_result.geometry_disp);
          copy_v3_v3(geometry_normal, split_result.geometry_normal);
        }

        geometry_supports[i] = geometry_support;

        vco[2] += geometry_disp[1];

        if (omd->chop_amount > 0.0f) {
          vco[0] += geometry_disp[0];
          vco[1] += geometry_disp[2];
        }

        if (geometry_normal_attr) {
          geometry_normal_attr.span[i] = float3(
              geometry_normal[0], geometry_normal[1], geometry_normal[2]);
        }
        if (geometry_support_cov_attr) {
          geometry_support_cov_attr.span[i] = float3(
              geometry_support.covariance[0], geometry_support.covariance[1], geometry_support.covariance[2]);
        }
        if (camera_lod_level_attr) {
          camera_lod_level_attr.span[i] = float(local_level_index);
        }
        if (camera_lod_split_level_attr) {
          camera_lod_split_level_attr.span[i] = float(split_level_index);
        }
        if (camera_lod_morph_attr) {
          camera_lod_morph_attr.span[i] = morph_factor;
        }
        if (camera_lod_radius_attr && i < camera_lod_radii.size()) {
          camera_lod_radius_attr.span[i] = camera_lod_radii[i];
        }
        if (!custom_normals.is_empty()) {
          custom_normals[i] = float3(geometry_normal[0], geometry_normal[2], geometry_normal[1]);
        }

        if (split_debug_enabled) {
          const float radius = (split_debug_has_radius_attr &&
                                i < split_debug_radius_values.size()) ?
                                   split_debug_radius_values[i] :
                                   0.0f;
          ocean_split_support_debug_accumulate(split_debug_all_stats,
                                               geometry_support,
                                               radius,
                                               split_debug_has_radius_attr,
                                               geometry_normal[1]);

          if (split_debug_has_level_attr && i < split_debug_level_values.size()) {
            const float level_value = split_debug_level_values[i];
            if (isfinite(level_value)) {
              const int debug_level_index = std::max(0, int(floorf(level_value + 0.5f)));
              if (debug_level_index < split_debug_level_stats.size()) {
                ocean_split_support_debug_accumulate(split_debug_level_stats[debug_level_index],
                                                     geometry_support,
                                                     radius,
                                                     split_debug_has_radius_attr,
                                                     geometry_normal[1]);
              }
            }
          }
        }
      }
      else {
        const float u = ref_uv.x;
        const float v = ref_uv.y;

        if (omd->oceancache && omd->cached) {
          BKE_ocean_cache_eval_uv(omd->oceancache, &ocr, cfra_for_cache, u, v);
        }
        else {
          BKE_ocean_eval_uv(omd->ocean, &ocr, u, v);
        }

        vco[2] += ocr.disp[1];

        if (omd->chop_amount > 0.0f) {
          vco[0] += ocr.disp[0];
          vco[1] += ocr.disp[2];
        }
      }
    }
    if (split_read_scope_ptr != nullptr) {
      BKE_ocean_split_runtime_read_end(&split_read_scope);
    }
    if (profile_enabled) {
      displacement_s = BLI_time_now_seconds() - displacement_start;
    }
  }

  result->tag_positions_changed();

  if (use_camera_lod) {
    const double finish_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    double finish_debug_s = 0.0;
    double finish_attr_s = 0.0;
    double finish_normal_override_s = 0.0;
    double finish_custom_normal_s = 0.0;
    if (split_debug_enabled) {
      const double debug_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
      ocean_split_support_debug_log_stats(ob->id.name + 2, "all", split_debug_all_stats);
      for (const int debug_level_index : split_debug_level_stats.index_range()) {
        if (split_debug_level_stats[debug_level_index].count == 0) {
          continue;
        }
        char label[32];
        std::snprintf(label, sizeof(label), "level=%d", debug_level_index);
        ocean_split_support_debug_log_stats(
            ob->id.name + 2, label, split_debug_level_stats[debug_level_index]);
      }
      fflush(stdout);
      if (profile_enabled) {
        finish_debug_s = BLI_time_now_seconds() - debug_start;
      }
    }

    const double attr_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    if (ref_coord_attr) {
      ref_coord_attr.finish();
    }
    if (geometry_normal_attr) {
      geometry_normal_attr.finish();
    }
    if (geometry_support_cov_attr) {
      geometry_support_cov_attr.finish();
    }
    if (camera_lod_level_attr) {
      camera_lod_level_attr.finish();
    }
    if (camera_lod_split_level_attr) {
      camera_lod_split_level_attr.finish();
    }
    if (camera_lod_morph_attr) {
      camera_lod_morph_attr.finish();
    }
    if (camera_lod_radius_attr) {
      camera_lod_radius_attr.finish();
    }
    if (ref_uv_attr) {
      ref_uv_attr.finish();
    }
    if (profile_enabled) {
      finish_attr_s = BLI_time_now_seconds() - attr_start;
    }

    if (use_camera_lod_mesh && !custom_normals.is_empty()) {
      const double normal_override_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
      const blender::Span<float3> mesh_normals = result->vert_normals();
      for (const int vert : custom_normals.index_range()) {
        if (vert < camera_lod_levels.size() &&
            camera_lod_levels[vert] == 0)
        {
          custom_normals[vert] = mesh_normals[vert];
        }
      }
      if (profile_enabled) {
        finish_normal_override_s = BLI_time_now_seconds() - normal_override_start;
      }
    }

    if (!custom_normals.is_empty()) {
      /* Non-Cycles renderers fall back to explicit geometry-band displacement plus geometry-band
       * normals. Cycles uses the exported ocean normal attributes directly and avoids building a
       * large custom-normal layer. */
      const double custom_normal_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
      blender::bke::mesh_set_custom_normals_from_verts(*result, custom_normals);
      if (profile_enabled) {
        finish_custom_normal_s = BLI_time_now_seconds() - custom_normal_start;
      }
    }
    if (profile_enabled) {
      finish_s = BLI_time_now_seconds() - finish_start;
      ocean_camera_lod_profile_logf(
          object_name,
          "finish",
          "total_s=%.6f debug_s=%.6f attr_s=%.6f normal_override_s=%.6f "
          "custom_normal_s=%.6f verts=%d corners=%d",
          finish_s,
          finish_debug_s,
          finish_attr_s,
          finish_normal_override_s,
          finish_custom_normal_s,
          result->verts_num,
          result->corners_num);
    }
  }

  if (camera_lod_cache_enabled && camera_lod_runtime_data != nullptr && result != nullptr) {
    const double cache_store_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    ocean_modifier_runtime_free_camera_lod_cache(*camera_lod_runtime_data);
    camera_lod_runtime_data->camera_lod_cached_result = BKE_mesh_copy_for_eval(*result);
    camera_lod_runtime_data->camera_lod_cached_key = camera_lod_cache_key;
    camera_lod_runtime_data->camera_lod_cached_key_valid =
        camera_lod_runtime_data->camera_lod_cached_result != nullptr;
    if (profile_enabled) {
      ocean_camera_lod_profile_logf(
          object_name,
          "modifier_cache_store",
          "mode=camera_lod total_s=%.6f store_s=%.6f verts=%d faces=%d",
          BLI_time_now_seconds() - profile_start,
          BLI_time_now_seconds() - cache_store_start,
          result->verts_num,
          result->faces_num);
    }
  }

  if (allocated_ocean) {
    const double cleanup_start = profile_enabled ? BLI_time_now_seconds() : 0.0;
    BKE_ocean_free(omd->ocean);
    omd->ocean = nullptr;
    if (profile_enabled) {
      cleanup_s = BLI_time_now_seconds() - cleanup_start;
    }
  }

  if (profile_enabled) {
    ocean_camera_lod_profile_logf(
        object_name,
        "modifier",
        "mode=%s geometry_mode=%d resolution=%d total_s=%.6f simulation_s=%.6f "
        "geometry_generate_s=%.6f foam_s=%.6f settings_requery_s=%.6f support_prepare_s=%.6f "
        "displacement_s=%.6f finish_s=%.6f cleanup_s=%.6f verts=%d faces=%d "
        "use_camera_lod_mesh=%d dense_fast_path=%d topology_cache_hit=%d "
        "runtime_sample_calls=%d runtime_morph_blend_calls=%d "
        "split_support_eval_calls=%d",
        use_camera_lod ? "camera_lod" : "dense_reference",
        int(omd->geometry_mode),
        resolution,
        BLI_time_now_seconds() - profile_start,
        simulation_s,
        geometry_generate_s,
        foam_s,
        settings_requery_s,
        support_prepare_s,
        displacement_s,
        finish_s,
        cleanup_s,
        result ? result->verts_num : 0,
        result ? result->faces_num : 0,
        int(use_camera_lod_mesh),
        int(camera_lod_dense_fast_path),
        int(camera_lod_topology_cache_hit),
        runtime_sample_calls,
        runtime_morph_blend_calls,
        split_support_eval_calls);
  }

#  undef OCEAN_CO

  return result;
}
#else  /* WITH_OCEANSIM */
static Mesh *doOcean(ModifierData * /*md*/, const ModifierEvalContext * /*ctx*/, Mesh *mesh)
{
  return mesh;
}
#endif /* WITH_OCEANSIM */

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  return doOcean(md, ctx, mesh);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
#ifdef WITH_OCEANSIM
  OceanModifierData *omd = (OceanModifierData *)md;
  if (ocean_use_camera_lod(omd) && ctx->scene != nullptr) {
    DEG_add_depends_on_transform_relation(ctx->node, "Ocean Modifier");
    DEG_add_scene_camera_relation(ctx->node, ctx->scene, DEG_OB_COMP_TRANSFORM, "Ocean Modifier");
    DEG_add_scene_camera_relation(ctx->node, ctx->scene, DEG_OB_COMP_PARAMETERS, "Ocean Modifier");
    DEG_add_scene_relation(ctx->node, ctx->scene, DEG_SCENE_COMP_PARAMETERS, "Ocean Modifier");
  }
#else
  UNUSED_VARS(md, ctx);
#endif
}
// #define WITH_OCEANSIM
static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
#ifdef WITH_OCEANSIM

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.prop(ptr, "geometry_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const bool generate_mode = RNA_enum_get(ptr, "geometry_mode") == MOD_OCEAN_GEOM_GENERATE;
  const bool use_camera_lod = RNA_boolean_get(ptr, "use_camera_lod");
  const bool stereo_dataset_mode = RNA_enum_get(ptr, "lod_usage_mode") ==
                                   MOD_OCEAN_LOD_USAGE_STEREO_DATASET;
  if (generate_mode && !use_camera_lod) {
    ui::Layout &sub = col.column(true);
    sub.prop(ptr, "repeat_x", UI_ITEM_NONE, IFACE_("Repeat X"), ICON_NONE);
    sub.prop(ptr, "repeat_y", UI_ITEM_NONE, IFACE_("Y"), ICON_NONE);
  }

  ui::Layout &sub = col.column(true);
  sub.prop(ptr, "viewport_resolution", UI_ITEM_NONE, IFACE_("Resolution Viewport"), ICON_NONE);
  sub.prop(ptr, "resolution", UI_ITEM_NONE, IFACE_("Render"), ICON_NONE);
  if (generate_mode) {
    sub.prop(ptr, "lod_levels", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (use_camera_lod) {
      sub.prop(ptr, "lod_pixel_error", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      sub.prop(ptr, "lod_camera_full_spectrum_radius", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      sub.prop(ptr, "lod_usage_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      if (!stereo_dataset_mode) {
        sub.prop(ptr, "lod_validation_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
      }
    }
  }

  col.prop(ptr, "time", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  col.prop(ptr, "depth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col.prop(ptr, "spatial_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  col.prop(ptr, "random_seed", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  col.prop(ptr, "use_normals", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (generate_mode) {
    col.prop(ptr, "use_camera_lod", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    if (use_camera_lod) {
      ui::Layout &warning = col.column(false);
      warning.label(
          RPT_("Camera LOD is Cycles-only for final renders; unsupported paths use dense geometry"),
          ICON_INFO);
      if (stereo_dataset_mode) {
        warning.label(
            RPT_("Stereo Dataset mode keeps geometry-strict validation over the stereo-eye union"),
            ICON_NONE);
      }
    }
  }

  modifier_error_message_draw(layout, ptr);

#else  /* WITH_OCEANSIM */
  layout.label(RPT_("Built without Ocean modifier"), ICON_NONE);
#endif /* WITH_OCEANSIM */
}

#ifdef WITH_OCEANSIM
static void waves_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  ui::Layout *col = &layout.column(false);
  col->prop(ptr, "use_wave_scale", UI_ITEM_NONE, IFACE_("Use Scale"), ICON_NONE);
  ui::Layout *sub = &col->column(false);
  sub->active_set(RNA_boolean_get(ptr, "use_wave_scale"));
  sub->prop(ptr, "wave_scale", UI_ITEM_NONE, IFACE_("Scale"), ICON_NONE);
  col->prop(ptr, "wave_scale_min", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "choppiness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "wind_velocity", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  layout.separator();

  col = &layout.column(false);
  col->prop(ptr, "wave_alignment", ui::ITEM_R_SLIDER, IFACE_("Alignment"), ICON_NONE);
  ui::Layout &alignment_sub = col->column(false);
  alignment_sub.active_set(RNA_float_get(ptr, "wave_alignment") > 0.0f);
  alignment_sub.prop(ptr, "wave_direction", UI_ITEM_NONE, IFACE_("Direction"), ICON_NONE);
  alignment_sub.prop(ptr, "damping", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

static void foam_panel_draw_header(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.prop(ptr, "use_foam", UI_ITEM_NONE, IFACE_("Foam"), ICON_NONE);
}

static void foam_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  bool use_foam = RNA_boolean_get(ptr, "use_foam");

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.active_set(use_foam);
  col.prop(ptr, "foam_layer_name", UI_ITEM_NONE, IFACE_("Data Layer"), ICON_NONE);
  col.prop(ptr, "foam_coverage", UI_ITEM_NONE, IFACE_("Coverage"), ICON_NONE);
}

static void spray_panel_draw_header(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  bool use_foam = RNA_boolean_get(ptr, "use_foam");

  ui::Layout &row = layout.row(false);
  row.active_set(use_foam);
  row.prop(
      ptr, "use_spray", UI_ITEM_NONE, CTX_IFACE_(BLT_I18NCONTEXT_ID_MESH, "Spray"), ICON_NONE);
}

static void spray_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  bool use_foam = RNA_boolean_get(ptr, "use_foam");
  bool use_spray = RNA_boolean_get(ptr, "use_spray");

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.active_set(use_foam && use_spray);
  col.prop(ptr, "spray_layer_name", UI_ITEM_NONE, IFACE_("Data Layer"), ICON_NONE);
  col.prop(ptr, "invert_spray", UI_ITEM_NONE, IFACE_("Invert"), ICON_NONE);
}

static void spectrum_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  int spectrum = RNA_enum_get(ptr, "spectrum");

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.prop(ptr, "spectrum", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (ELEM(spectrum, MOD_OCEAN_SPECTRUM_TEXEL_MARSEN_ARSLOE, MOD_OCEAN_SPECTRUM_JONSWAP)) {
    col.prop(ptr, "sharpen_peak_jonswap", ui::ITEM_R_SLIDER, std::nullopt, ICON_NONE);
    col.prop(ptr, "fetch_jonswap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (spectrum == MOD_OCEAN_SPECTRUM_REALSEA_JONSWAP) {
    col.prop(ptr, "fetch_jonswap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  if (ELEM(spectrum, MOD_OCEAN_SPECTRUM_REALSEA_PM, MOD_OCEAN_SPECTRUM_REALSEA_JONSWAP)) {
    col.separator();
    col.prop(ptr, "realsea_fmin", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "realsea_fmax", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "realsea_use_spread", UI_ITEM_NONE, IFACE_("Use Directional Spread"), ICON_NONE);
    ui::Layout &realsea_sub = col.column(false);
    realsea_sub.active_set(RNA_boolean_get(ptr, "realsea_use_spread"));
    realsea_sub.prop(ptr, "realsea_spread", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

static void split_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  const bool use_camera_lod = RNA_boolean_get(ptr, "use_camera_lod");
  const bool stereo_dataset_mode = RNA_enum_get(ptr, "lod_usage_mode") ==
                                   MOD_OCEAN_LOD_USAGE_STEREO_DATASET;

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  col.active_set(use_camera_lod);
  if (stereo_dataset_mode) {
    col.label(IFACE_("Stereo Dataset mode uses geometry-only shading for explicit stereo depth"),
              ICON_NONE);
    col.label(IFACE_("The ocean_geometry_normal attribute stores the explicit mesh normal"),
              ICON_NONE);
    col.label(IFACE_("Cycles skips residual split shading detail in this mode"), ICON_NONE);
  }
  else {
    col.label(IFACE_("Cycles reconstructs visible residual detail from the shared ocean hierarchy"),
              ICON_NONE);
    col.label(IFACE_("The ocean_geometry_normal attribute stores the explicit geometry-band normal"),
              ICON_NONE);
    col.label(IFACE_("Cycles keeps the apparent residual normal internal to shading"),
              ICON_NONE);
    col.label(
        IFACE_("Other renderers use geometry-band displacement and geometry custom normals"),
        ICON_NONE);
  }
}

static void bake_panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;

  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);

  bool is_cached = RNA_boolean_get(ptr, "is_cached");
  bool use_foam = RNA_boolean_get(ptr, "use_foam");
  bool use_camera_lod = RNA_boolean_get(ptr, "use_camera_lod");

  if (use_camera_lod) {
    layout.label(IFACE_("Camera LOD uses live simulation only"), ICON_INFO);
    layout.label(IFACE_("Bake/cache is unavailable while Camera LOD is enabled"), ICON_NONE);
    return;
  }

  if (is_cached) {
    PointerRNA op_ptr = layout.op("OBJECT_OT_ocean_bake",
                                  IFACE_("Delete Bake"),
                                  ICON_NONE,
                                  wm::OpCallContext::InvokeDefault,
                                  UI_ITEM_NONE);
    RNA_boolean_set(&op_ptr, "free", true);
  }
  else {
    PointerRNA op_ptr = layout.op("OBJECT_OT_ocean_bake",
                                  IFACE_("Bake"),
                                  ICON_NONE,
                                  wm::OpCallContext::InvokeDefault,
                                  UI_ITEM_NONE);
    RNA_boolean_set(&op_ptr, "free", false);
  }

  layout.prop(ptr, "filepath", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout *col = &layout.column(true);
  col->enabled_set(!is_cached);
  col->prop(ptr, "frame_start", UI_ITEM_NONE, IFACE_("Frame Start"), ICON_NONE);
  col->prop(ptr, "frame_end", UI_ITEM_NONE, IFACE_("End"), ICON_NONE);

  col = &layout.column(false);
  col->active_set(use_foam);
  col->prop(ptr, "bake_foam_fade", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}
#endif /* WITH_OCEANSIM */

static void panel_register(ARegionType *region_type)
{
  PanelType *panel_type = modifier_panel_register(region_type, eModifierType_Ocean, panel_draw);
#ifdef WITH_OCEANSIM
  modifier_subpanel_register(region_type, "waves", "Waves", nullptr, waves_panel_draw, panel_type);
  PanelType *foam_panel = modifier_subpanel_register(
      region_type, "foam", "", foam_panel_draw_header, foam_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "spray", "", spray_panel_draw_header, spray_panel_draw, foam_panel);
  modifier_subpanel_register(
      region_type, "spectrum", "Spectrum", nullptr, spectrum_panel_draw, panel_type);
  modifier_subpanel_register(
      region_type, "split", "Camera LOD", nullptr, split_panel_draw, panel_type);
  modifier_subpanel_register(region_type, "bake", "Bake", nullptr, bake_panel_draw, panel_type);
#else
  UNUSED_VARS(panel_type);
#endif /* WITH_OCEANSIM */
}

static void blend_read(BlendDataReader * /*reader*/, ModifierData *md)
{
  OceanModifierData *omd = reinterpret_cast<OceanModifierData *>(md);
  omd->oceancache = nullptr;
  omd->ocean = nullptr;
}

ModifierTypeInfo modifierType_Ocean = {
    /*idname*/ "Ocean",
    /*name*/ N_("Ocean"),
    /*struct_name*/ "OceanModifierData",
    /*struct_size*/ sizeof(OceanModifierData),
    /*srna*/ &RNA_OceanModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
        eModifierTypeFlag_EnableInEditmode,
    /*icon*/ ICON_MOD_OCEAN,

    /*copy_data*/ copy_data,
    /*deform_verts*/ nullptr,

    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
    /*free_data*/ free_data,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ free_runtime_data,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ blend_read,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
