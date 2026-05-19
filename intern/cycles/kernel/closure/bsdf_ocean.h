/* SPDX-FileCopyrightText: 2026 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/geom/attribute.h"
#include "kernel/geom/object.h"
#include "kernel/geom/primitive.h"
#include "kernel/util/differential.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline bool ocean_split_object_has_data(KernelGlobals kg,
                                                   const ccl_private ShaderData *sd)
{
  return sd->object != OBJECT_NONE &&
         kernel_data_fetch(objects, sd->object).ocean_split_level_count > 0;
}

ccl_device_inline const ccl_global KernelObject *ocean_split_object_data(
    KernelGlobals kg, const ccl_private ShaderData *sd)
{
  return &kernel_data_fetch(objects, sd->object);
}

ccl_device_inline int ocean_split_level_resolution_x(const ccl_global KernelObject *kobject,
                                                     const int level_index)
{
  return max(kobject->ocean_split_resolution_x[level_index], 1);
}

ccl_device_inline int ocean_split_level_resolution_y(const ccl_global KernelObject *kobject,
                                                     const int level_index)
{
  return max(kobject->ocean_split_resolution_y[level_index], 1);
}

ccl_device_inline float ocean_split_level_cell_size_x(const ccl_global KernelObject *kobject,
                                                      const int level_index)
{
  return fmaxf(kobject->ocean_split_cell_size_x[level_index], 1.0e-12f);
}

ccl_device_inline float ocean_split_level_cell_size_z(const ccl_global KernelObject *kobject,
                                                      const int level_index)
{
  return fmaxf(kobject->ocean_split_cell_size_z[level_index], 1.0e-12f);
}

ccl_device_inline void ocean_split_tangent_basis(KernelGlobals kg,
                                                 const ccl_private ShaderData *sd,
                                                 const float3 N,
                                                 ccl_private float3 *r_T,
                                                 ccl_private float3 *r_B)
{
  float3 ocean_x = make_float3(1.0f, 0.0f, 0.0f);
  float3 ocean_z = make_float3(0.0f, 1.0f, 0.0f);
  object_dir_transform(kg, sd, &ocean_x);
  object_dir_transform(kg, sd, &ocean_z);

  ocean_x -= dot(ocean_x, N) * N;
  ocean_z -= dot(ocean_z, N) * N;

  if (dot(ocean_x, ocean_x) <= 1.0e-12f) {
    ocean_x = cross(ocean_z, N);
  }
  if (dot(ocean_x, ocean_x) <= 1.0e-12f) {
    make_orthonormals(N, r_T, r_B);
    return;
  }

  ocean_x = normalize(ocean_x);
  float3 ocean_y = safe_normalize_fallback(cross(N, ocean_x), zero_float3());
  if (is_zero(ocean_y)) {
    make_orthonormals(N, r_T, r_B);
    return;
  }

  if (dot(ocean_y, ocean_z) < 0.0f) {
    ocean_x = -ocean_x;
    ocean_y = -ocean_y;
  }

  *r_T = ocean_x;
  *r_B = ocean_y;
}

ccl_device_inline float3 ocean_split_retarget_material_normal(
    KernelGlobals kg,
    const ccl_private ShaderData *sd,
    const float3 source_base_normal,
    const float3 material_normal,
    const float3 target_base_normal)
{
  const float3 source_base = safe_normalize_fallback(source_base_normal, target_base_normal);
  const float3 material = safe_normalize_fallback(material_normal, source_base);
  const float3 target_base = safe_normalize_fallback(target_base_normal, source_base);

  if (dot(source_base, material) >= 0.9999f) {
    return target_base;
  }

  float3 source_T, source_B;
  float3 target_T, target_B;
  ocean_split_tangent_basis(kg, sd, source_base, &source_T, &source_B);
  ocean_split_tangent_basis(kg, sd, target_base, &target_T, &target_B);

  const float tangent_x = dot(material, source_T);
  const float tangent_y = dot(material, source_B);
  const float normal_component = fmaxf(dot(material, source_base), 1.0e-4f);
  const float3 retargeted = (target_T * tangent_x) + (target_B * tangent_y) +
                            (target_base * normal_component);
  return safe_normalize_fallback(retargeted, target_base);
}

ccl_device_inline void ocean_split_covariance_eigenvalues(const float3 covariance,
                                                          ccl_private float *r_minor_variance,
                                                          ccl_private float *r_major_variance)
{
  const float trace = covariance.x + covariance.z;
  const float diff = covariance.x - covariance.z;
  const float discriminant = sqrtf(fmaxf(diff * diff + 4.0f * covariance.y * covariance.y, 0.0f));
  *r_major_variance = fmaxf(0.0f, 0.5f * (trace + discriminant));
  *r_minor_variance = fmaxf(0.0f, 0.5f * (trace - discriminant));
}

ccl_device_inline float3 ocean_split_covariance_project_psd(const float3 covariance)
{
  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(covariance, &minor_variance, &major_variance);

  const float angle = 0.5f * atan2f(2.0f * covariance.y, covariance.x - covariance.z);
  const float c = cosf(angle);
  const float s = sinf(angle);

  return make_float3(c * c * major_variance + s * s * minor_variance,
                     c * s * (major_variance - minor_variance),
                     s * s * major_variance + c * c * minor_variance);
}

ccl_device_inline float ocean_split_level_variance(const ccl_global KernelObject *kobject,
                                                   const int level_index)
{
  if (level_index <= 0) {
    return 0.0f;
  }

  const float sigma = 0.5f * kobject->ocean_split_min_wavelength * exp2f(float(level_index));
  const float sigma_min = 0.5f * kobject->ocean_split_min_wavelength;
  return fmaxf(0.0f, sigma * sigma - sigma_min * sigma_min);
}

ccl_device_inline int ocean_split_support_base_level(const ccl_global KernelObject *kobject,
                                                     const float3 support_covariance,
                                                     ccl_private float *r_base_variance)
{
  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(support_covariance, &minor_variance, &major_variance);

  const float minor_wavelength = 2.0f * sqrtf(fmaxf(minor_variance, 0.0f));
  const float clamped_wavelength = fmaxf(minor_wavelength, kobject->ocean_split_min_wavelength);
  int level_index = int(
      floorf(log2f(clamped_wavelength / kobject->ocean_split_min_wavelength)));
  level_index = clamp(level_index, 0, kobject->ocean_split_level_count - 1);

  float base_variance = ocean_split_level_variance(kobject, level_index);
  while (level_index > 0 && base_variance > minor_variance + 1.0e-10f) {
    level_index--;
    base_variance = ocean_split_level_variance(kobject, level_index);
  }

  *r_base_variance = fminf(base_variance, minor_variance);
  return level_index;
}

ccl_device_inline float3 ocean_split_covariance_subtract_isotropic(const float3 covariance,
                                                                   const float isotropic_variance)
{
  return ocean_split_covariance_project_psd(
      make_float3(covariance.x - isotropic_variance, covariance.y, covariance.z - isotropic_variance));
}

ccl_device_inline float3 ocean_split_support_visible_moment(
    const ccl_global KernelObject *kobject, const float3 support_covariance)
{
  const packed_float3 *moments = kobject->ocean_split_cumulative_slope_moments;
  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(support_covariance, &minor_variance, &major_variance);

  float base_variance = 0.0f;
  const int level_index = ocean_split_support_base_level(
      kobject, support_covariance, &base_variance);
  const int next_level_index = min(level_index + 1, kobject->ocean_split_level_count - 1);

  const float3 base_moment = moments[level_index];
  if (next_level_index == level_index) {
    return base_moment;
  }

  const float next_variance = ocean_split_level_variance(kobject, next_level_index);
  const float t = clamp(
      (minor_variance - base_variance) / fmaxf(next_variance - base_variance, 1.0e-12f), 0.0f, 1.0f);
  const float3 next_moment = moments[next_level_index];
  return base_moment + t * (next_moment - base_moment);
}

ccl_device_inline float3 ocean_split_support_visible_moment(
    const ccl_global KernelObject *kobject,
    const packed_float3 *moments,
    const float3 support_covariance)
{
  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(support_covariance, &minor_variance, &major_variance);

  float base_variance = 0.0f;
  const int level_index = ocean_split_support_base_level(
      kobject, support_covariance, &base_variance);
  const int next_level_index = min(level_index + 1, kobject->ocean_split_level_count - 1);

  const float3 base_moment = moments[level_index];
  if (next_level_index == level_index) {
    return base_moment;
  }

  const float next_variance = ocean_split_level_variance(kobject, next_level_index);
  const float t = clamp(
      (minor_variance - base_variance) / fmaxf(next_variance - base_variance, 1.0e-12f), 0.0f, 1.0f);
  const float3 next_moment = moments[next_level_index];
  return base_moment + t * (next_moment - base_moment);
}

ccl_device_inline float3 ocean_split_level_moment_at_time(const ccl_global KernelObject *kobject,
                                                          const ccl_private ShaderData *sd,
                                                          const int level_index)
{
  const float3 moment = kobject->ocean_split_cumulative_slope_moments[level_index];
  if (sd->time <= 0.5f && kobject->ocean_split_slope_texture_slots_pre[level_index] >= 0)
  {
    const float3 pre_moment = kobject->ocean_split_cumulative_slope_moments_pre[level_index];
    return interp(pre_moment, moment, clamp(sd->time * 2.0f, 0.0f, 1.0f));
  }
  if (sd->time > 0.5f && kobject->ocean_split_slope_texture_slots_post[level_index] >= 0)
  {
    const float3 post_moment = kobject->ocean_split_cumulative_slope_moments_post[level_index];
    return interp(moment, post_moment, clamp((sd->time - 0.5f) * 2.0f, 0.0f, 1.0f));
  }
  return moment;
}

ccl_device_inline float3 ocean_split_support_visible_moment_at_time(
    const ccl_global KernelObject *kobject,
    const ccl_private ShaderData *sd,
    const float3 support_covariance)
{
  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(support_covariance, &minor_variance, &major_variance);

  float base_variance = 0.0f;
  const int level_index = ocean_split_support_base_level(
      kobject, support_covariance, &base_variance);
  const int next_level_index = min(level_index + 1, kobject->ocean_split_level_count - 1);

  const float3 base_moment = ocean_split_level_moment_at_time(kobject, sd, level_index);
  if (next_level_index == level_index) {
    return base_moment;
  }

  const float next_variance = ocean_split_level_variance(kobject, next_level_index);
  const float t = clamp(
      (minor_variance - base_variance) / fmaxf(next_variance - base_variance, 1.0e-12f),
      0.0f,
      1.0f);
  const float3 next_moment = ocean_split_level_moment_at_time(kobject, sd, next_level_index);
  return base_moment + t * (next_moment - base_moment);
}

ccl_device_inline bool ocean_split_geometry_normal_canonical(
    KernelGlobals kg, const ccl_private ShaderData *sd, ccl_private float3 *r_geometry_normal)
{
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_OCEAN_GEOMETRY_NORMAL);
  if (desc.offset == ATTR_STD_NOT_FOUND || desc.type != NODE_ATTR_FLOAT3) {
    return false;
  }

  const float3 normal = primitive_surface_attribute<float3>(kg, sd, desc).val;
  if (is_zero(normal)) {
    return false;
  }

  *r_geometry_normal = safe_normalize_fallback(normal, make_float3(0.0f, 1.0f, 0.0f));
  return true;
}

ccl_device_inline bool ocean_split_geometry_normal(KernelGlobals kg,
                                                   const ccl_private ShaderData *sd,
                                                   ccl_private float3 *r_geometry_normal_world,
                                                   ccl_private float3 *r_geometry_normal_canonical)
{
  float3 geometry_normal_canonical;
  if (!ocean_split_geometry_normal_canonical(kg, sd, &geometry_normal_canonical)) {
    return false;
  }

  float3 geometry_normal_world = make_float3(
      geometry_normal_canonical.x, geometry_normal_canonical.z, geometry_normal_canonical.y);
  object_normal_transform(kg, sd, &geometry_normal_world);
  if (is_zero(geometry_normal_world)) {
    return false;
  }

  if (r_geometry_normal_canonical != nullptr) {
    *r_geometry_normal_canonical = geometry_normal_canonical;
  }
  *r_geometry_normal_world = safe_normalize_fallback(geometry_normal_world, sd->N);
  return true;
}

ccl_device_inline bool ocean_split_ref_coord(KernelGlobals kg,
                                             const ccl_private ShaderData *sd,
                                             ccl_private float3 *r_ref_coord,
                                             ccl_private float3 *r_drefdx,
                                             ccl_private float3 *r_drefdy)
{
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_OCEAN_REF_COORD);
  if (desc.offset == ATTR_STD_NOT_FOUND || desc.type != NODE_ATTR_FLOAT3) {
    return false;
  }

  const dual3 ref_coord = primitive_surface_attribute<float3>(
      kg, sd, desc, r_drefdx != nullptr, r_drefdy != nullptr);
  *r_ref_coord = ref_coord.val;
  if (r_drefdx != nullptr) {
    *r_drefdx = ref_coord.dx;
  }
  if (r_drefdy != nullptr) {
    *r_drefdy = ref_coord.dy;
  }
  return true;
}

ccl_device_inline bool ocean_split_ref_uv(KernelGlobals kg,
                                          const ccl_private ShaderData *sd,
                                          ccl_private float2 *r_ref_uv)
{
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_OCEAN_REF_UV);
  if (desc.offset == ATTR_STD_NOT_FOUND || desc.type != NODE_ATTR_FLOAT2) {
    return false;
  }

  *r_ref_uv = primitive_surface_attribute<float2>(kg, sd, desc).val;
  return true;
}

ccl_device_inline bool ocean_split_geometry_support_covariance(
    KernelGlobals kg, const ccl_private ShaderData *sd, ccl_private float3 *r_covariance)
{
  const AttributeDescriptor desc = find_attribute(kg, sd, ATTR_STD_OCEAN_GEOMETRY_SUPPORT_COVARIANCE);
  if (desc.offset == ATTR_STD_NOT_FOUND || desc.type != NODE_ATTR_FLOAT3) {
    return false;
  }

  const float3 covariance = primitive_surface_attribute<float3>(kg, sd, desc).val;
  *r_covariance = ocean_split_covariance_project_psd(
      make_float3(fmaxf(covariance.x, 0.0f), covariance.y, fmaxf(covariance.z, 0.0f)));
  return true;
}

ccl_device_inline bool ocean_split_camera_support_covariance(
    KernelGlobals kg, const ccl_private ShaderData *sd, ccl_private float3 *r_covariance)
{
  float3 ref_coord, drefdx, drefdy;
  if (ocean_split_ref_coord(kg, sd, &ref_coord, &drefdx, &drefdy)) {
    (void)ref_coord;
    const float3 covariance = ocean_split_covariance_project_psd(
        make_float3(0.25f * (drefdx.x * drefdx.x + drefdy.x * drefdy.x),
                    0.25f * (drefdx.x * drefdx.z + drefdy.x * drefdy.z),
                    0.25f * (drefdx.z * drefdx.z + drefdy.z * drefdy.z)));
    if (covariance.x + covariance.z > 1.0e-20f) {
      *r_covariance = covariance;
      return true;
    }
  }

  float3 geometry_normal_world;
  if (!ocean_split_geometry_normal(kg, sd, &geometry_normal_world, nullptr)) {
    return false;
  }

  const differential3 dP = differential_from_compact(geometry_normal_world, sd->dP);
  float3 T, B;
  ocean_split_tangent_basis(kg, sd, geometry_normal_world, &T, &B);

  const float dx_t = dot(dP.dx, T);
  const float dx_b = dot(dP.dx, B);
  const float dy_t = dot(dP.dy, T);
  const float dy_b = dot(dP.dy, B);

  *r_covariance = ocean_split_covariance_project_psd(
      make_float3(0.25f * (dx_t * dx_t + dy_t * dy_t),
                  0.25f * (dx_t * dx_b + dy_t * dy_b),
                  0.25f * (dx_b * dx_b + dy_b * dy_b)));
  return true;
}

ccl_device_inline void ocean_split_time_pair(const ccl_global KernelObject *kobject,
                                             const ccl_private ShaderData *sd,
                                             const int level_index,
                                             ccl_private int *r_slot0,
                                             ccl_private int *r_slot1,
                                             ccl_private float *r_t)
{
  *r_slot0 = kobject->ocean_split_slope_texture_slots[level_index];
  *r_slot1 = *r_slot0;
  *r_t = 0.0f;

  if (sd->time <= 0.5f && kobject->ocean_split_slope_texture_slots_pre[level_index] >= 0) {
    *r_slot0 = (kobject->ocean_split_slope_texture_slots_pre[level_index] >= 0) ?
                   kobject->ocean_split_slope_texture_slots_pre[level_index] :
                   kobject->ocean_split_slope_texture_slots[level_index];
    *r_slot1 = kobject->ocean_split_slope_texture_slots[level_index];
    *r_t = clamp(sd->time * 2.0f, 0.0f, 1.0f);
  }
  else if (sd->time > 0.5f && kobject->ocean_split_slope_texture_slots_post[level_index] >= 0) {
    *r_slot0 = kobject->ocean_split_slope_texture_slots[level_index];
    *r_slot1 = (kobject->ocean_split_slope_texture_slots_post[level_index] >= 0) ?
                   kobject->ocean_split_slope_texture_slots_post[level_index] :
                   kobject->ocean_split_slope_texture_slots[level_index];
    *r_t = clamp((sd->time - 0.5f) * 2.0f, 0.0f, 1.0f);
  }
}

ccl_device_inline float2 ocean_split_normal_sample_to_slope(const float4 sample)
{
  const float3 normal = safe_normalize_fallback(
      make_float3(sample.x, sample.y, sample.z), make_float3(0.0f, 1.0f, 0.0f));
  const float safe_normal_y = (fabsf(normal.y) > 1.0e-6f) ? normal.y :
                                                           ((normal.y < 0.0f) ? -1.0e-6f :
                                                                                 1.0e-6f);
  return make_float2(-normal.x / safe_normal_y, -normal.z / safe_normal_y);
}

ccl_device_inline float2 ocean_split_sample_slope_anisotropic(
    KernelGlobals kg,
    const int slot,
    const float2 uv,
    const float3 residual_covariance,
    const int resolution_x,
    const int resolution_y,
    const float cell_x,
    const float cell_z)
{
  if (slot < 0) {
    return zero_float2();
  }

  const float3 texel_covariance = ocean_split_covariance_project_psd(
      make_float3(residual_covariance.x / fmaxf(cell_x * cell_x, 1.0e-12f),
                  residual_covariance.y / fmaxf(cell_x * cell_z, 1.0e-12f),
                  residual_covariance.z / fmaxf(cell_z * cell_z, 1.0e-12f)));

  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(texel_covariance, &minor_variance, &major_variance);
  if (major_variance <= 1.0e-8f) {
    const float4 sample = kernel_image_interp(kg, slot, uv.x, uv.y);
    return ocean_split_normal_sample_to_slope(sample);
  }

  const float determinant = texel_covariance.x * texel_covariance.z -
                            texel_covariance.y * texel_covariance.y;
  if (determinant <= 1.0e-10f) {
    const float4 sample = kernel_image_interp(kg, slot, uv.x, uv.y);
    return ocean_split_normal_sample_to_slope(sample);
  }

  const float3 inv_covariance = make_float3(texel_covariance.z / determinant,
                                            -texel_covariance.y / determinant,
                                            texel_covariance.x / determinant);
  const float kernel_sigma_radius = 3.5f;
  const int max_kernel_radius = 16;
  const int radius_x = min(max_kernel_radius,
                           max(0,
                               int(ceilf(kernel_sigma_radius *
                                         sqrtf(fmaxf(texel_covariance.x, 0.0f))))));
  const int radius_y = min(max_kernel_radius,
                           max(0,
                               int(ceilf(kernel_sigma_radius *
                                         sqrtf(fmaxf(texel_covariance.z, 0.0f))))));

  if (radius_x == 0 && radius_y == 0) {
    const float4 sample = kernel_image_interp(kg, slot, uv.x, uv.y);
    return ocean_split_normal_sample_to_slope(sample);
  }

  const float q_max = kernel_sigma_radius * kernel_sigma_radius;
  float weight_sum = 0.0f;
  float2 value_sum = zero_float2();

  for (int offset_x = -radius_x; offset_x <= radius_x; offset_x++) {
    for (int offset_y = -radius_y; offset_y <= radius_y; offset_y++) {
      const float q = inv_covariance.x * float(offset_x * offset_x) +
                      2.0f * inv_covariance.y * float(offset_x * offset_y) +
                      inv_covariance.z * float(offset_y * offset_y);
      if (q > q_max) {
        continue;
      }

      const float weight = expf(-0.5f * q);
      if (weight <= 1.0e-8f) {
        continue;
      }

      const float sample_u = uv.x + (float(offset_x) / float(max(1, resolution_x)));
      const float sample_v = uv.y + (float(offset_y) / float(max(1, resolution_y)));
      const float4 sample = kernel_image_interp(kg, slot, sample_u, sample_v);
      value_sum += weight * ocean_split_normal_sample_to_slope(sample);
      weight_sum += weight;
    }
  }

  if (weight_sum <= 1.0e-8f) {
    const float4 sample = kernel_image_interp(kg, slot, uv.x, uv.y);
    return ocean_split_normal_sample_to_slope(sample);
  }
  return value_sum / weight_sum;
}

ccl_device_inline bool ocean_split_visible_slope(KernelGlobals kg,
                                                 const ccl_private ShaderData *sd,
                                                 ccl_private float2 *r_visible_slope,
                                                 ccl_private float2 *r_geometry_slope,
                                                 ccl_private float3 *r_geometry_support_covariance,
                                                 ccl_private float3 *r_camera_support_covariance,
                                                 ccl_private int *r_level_index)
{
  if (!ocean_split_object_has_data(kg, sd)) {
    return false;
  }

  float3 geometry_normal;
  if (!ocean_split_geometry_normal_canonical(kg, sd, &geometry_normal)) {
    return false;
  }
  if (fabsf(geometry_normal.y) <= 1.0e-8f) {
    return false;
  }

  float2 ref_uv;
  float3 geometry_support_covariance;
  if (!ocean_split_ref_uv(kg, sd, &ref_uv) ||
      !ocean_split_geometry_support_covariance(kg, sd, &geometry_support_covariance))
  {
    return false;
  }

  const ccl_global KernelObject *kobject = ocean_split_object_data(kg, sd);
  if (kobject->ocean_split_level_count <= 0) {
    return false;
  }

  const bool use_camera_brdf = (kobject->ocean_split_shading_mode ==
                                OCEAN_SPLIT_SHADING_CAMERA_BRDF);
  float3 camera_support_covariance = zero_float3();
  float visible_base_variance = 0.0f;
  int level_index = 0;
  float3 residual_covariance = zero_float3();

  if (use_camera_brdf) {
    if (ocean_split_camera_support_covariance(kg, sd, &camera_support_covariance)) {
      level_index = ocean_split_support_base_level(
          kobject, camera_support_covariance, &visible_base_variance);
      residual_covariance = ocean_split_covariance_subtract_isotropic(
          camera_support_covariance, visible_base_variance);
    }
  }
  else {
    float geometry_base_variance = 0.0f;
    const int geometry_base_level = ocean_split_support_base_level(
        kobject, geometry_support_covariance, &geometry_base_variance);
    (void)geometry_base_variance;
    if (geometry_base_level <= 0) {
      return false;
    }
  }

  int slot0 = -1;
  int slot1 = -1;
  float time_t = 0.0f;
  ocean_split_time_pair(kobject, sd, level_index, &slot0, &slot1, &time_t);
  if (slot0 < 0 && slot1 < 0) {
    return false;
  }

  const float2 slope0 = ocean_split_sample_slope_anisotropic(kg,
                                                             slot0,
                                                             ref_uv,
                                                             residual_covariance,
                                                             ocean_split_level_resolution_x(
                                                                 kobject, level_index),
                                                             ocean_split_level_resolution_y(
                                                                 kobject, level_index),
                                                             ocean_split_level_cell_size_x(
                                                                 kobject, level_index),
                                                             ocean_split_level_cell_size_z(
                                                                 kobject, level_index));
  const float2 slope1 = ocean_split_sample_slope_anisotropic(kg,
                                                             slot1,
                                                             ref_uv,
                                                             residual_covariance,
                                                             ocean_split_level_resolution_x(
                                                                 kobject, level_index),
                                                             ocean_split_level_resolution_y(
                                                                 kobject, level_index),
                                                             ocean_split_level_cell_size_x(
                                                                 kobject, level_index),
                                                             ocean_split_level_cell_size_z(
                                                                 kobject, level_index));
  *r_visible_slope = interp(slope0, slope1, time_t);
  *r_geometry_slope = make_float2(-geometry_normal.x / geometry_normal.y,
                                  -geometry_normal.z / geometry_normal.y);
  *r_geometry_support_covariance = geometry_support_covariance;
  *r_camera_support_covariance = camera_support_covariance;
  *r_level_index = level_index;
  return true;
}

ccl_device_inline bool ocean_split_visible_normal(KernelGlobals kg,
                                                  const ccl_private ShaderData *sd,
                                                  ccl_private float3 *r_visible_normal)
{
  float3 geometry_normal_world;
  if (!ocean_split_geometry_normal(kg, sd, &geometry_normal_world, nullptr)) {
    return false;
  }

  float2 visible_slope, geometry_slope;
  float3 geometry_support_covariance, camera_support_covariance;
  int level_index = 0;
  if (!ocean_split_visible_slope(kg,
                                 sd,
                                 &visible_slope,
                                 &geometry_slope,
                                 &geometry_support_covariance,
                                 &camera_support_covariance,
                                 &level_index))
  {
    return false;
  }
  (void)geometry_slope;
  (void)geometry_support_covariance;
  (void)camera_support_covariance;
  (void)level_index;

  const float3 visible_normal_canonical = safe_normalize_fallback(
      make_float3(-visible_slope.x, 1.0f, -visible_slope.y), make_float3(0.0f, 1.0f, 0.0f));
  float3 visible_normal_world = make_float3(
      visible_normal_canonical.x, visible_normal_canonical.z, visible_normal_canonical.y);
  object_normal_transform(kg, sd, &visible_normal_world);
  *r_visible_normal = safe_normalize_fallback(visible_normal_world, geometry_normal_world);
  return true;
}

ccl_device_inline bool ocean_split_unresolved_covariance(
    KernelGlobals kg, const ccl_private ShaderData *sd, ccl_private float3 *r_covariance)
{
  *r_covariance = zero_float3();
  if (!ocean_split_object_has_data(kg, sd)) {
    return false;
  }

  const ccl_global KernelObject *kobject = ocean_split_object_data(kg, sd);
  if (kobject->ocean_split_shading_mode != OCEAN_SPLIT_SHADING_CAMERA_BRDF ||
      kobject->ocean_split_level_count <= 0)
  {
    return false;
  }

  float3 camera_support_covariance;
  if (!ocean_split_camera_support_covariance(kg, sd, &camera_support_covariance)) {
    return false;
  }

  const float3 full_moment = ocean_split_level_moment_at_time(kobject, sd, 0);
  const float3 visible_moment = ocean_split_support_visible_moment_at_time(
      kobject, sd, camera_support_covariance);
  *r_covariance = ocean_split_covariance_project_psd(full_moment - visible_moment);
  return (r_covariance->x + r_covariance->z) > 1.0e-8f;
}

CCL_NAMESPACE_END
