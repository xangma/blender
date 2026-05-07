/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace blender {

/** \file
 * \ingroup bke
 */

struct OceanModifierData;
struct Ocean;

struct OceanResult {
  float disp[3];
  float normal[3];
  float foam;

  /* raw eigenvalues/vectors */
  float Jminus;
  float Jplus;
  float Eminus[3];
  float Eplus[3];
};

/**
 * Support descriptor for the split-spectrum ocean field.
 *
 * The current implementation carries a 2D support estimate in the canonical ocean X/Z frame.
 * The packed covariance tensor is the primary anisotropic support descriptor used by
 * `ocean.cc`; the directional/major-axis wavelengths are retained for compatibility,
 * diagnostics, and UI/debug export.
 */
struct OceanSplitSupport {
  /** Directional support wavelengths in meters along ocean X/Z. */
  float wavelength_x;
  float wavelength_z;
  /** Conservative scalar support, typically the footprint major-axis wavelength. */
  float wavelength_major;
  /** Footprint covariance tensor packed as (xx, xz, zz) in ocean X/Z coordinates. */
  float covariance[3];
};

/**
 * Support-aware split of the shared ocean field.
 *
 * The split is evaluated from the shared ocean field using geometry and camera support
 * descriptors in canonical ocean coordinates. Phase 4 uses the full support covariance as a
 * smooth quadratic-form filter on top of the shared split hierarchy, so LOD transitions can
 * move spectrum between geometry, visible residual, and unresolved statistical channels without
 * reverting to scalar or per-axis cutoffs.
 */
struct OceanSplitResult {
  /** Geometry-resolved displacement/chop only. */
  float geometry_disp[3];
  /**
   * Geometry-resolved normal of the explicitly intersected surface.
   * This is the only normal that is consistent with geometry-band depth/position.
   */
  float geometry_normal[3];
  /**
   * Camera-visible apparent/full-band normal (geometry + deterministic visible residual).
   * This is valid for shading, but not a claim that explicit geometry intersection carries the
   * residual band.
   */
  float visible_normal[3];
  /** Camera-visible but geometry-unresolved residual slope in X/Z. */
  float visible_residual_slope[2];
  /** Unresolved subpixel slope covariance tensor packed as (xx, xz, zz). */
  float unresolved_slope_covariance[3];
  /** Echoed scalar support wavelengths used for compatibility/debugging. */
  float geometry_wavelength;
  float camera_wavelength;
  /** Echoed directional support wavelengths in ocean X/Z. */
  float geometry_wavelength_xz[2];
  float camera_wavelength_xz[2];
  /** Echoed support covariance tensors in ocean X/Z. */
  float geometry_support_covariance[3];
  float camera_support_covariance[3];
};

struct OceanSplitRuntimeLevel {
  int size_x;
  int size_y;
  float wavelength;
  float cumulative_disp_variance[3];
  float cumulative_slope_moment[3];
};

struct OceanSplitRuntimeReadScope {
  const struct Ocean *ocean;
};

struct OceanCache {
  struct ImBuf **ibufs_disp;
  struct ImBuf **ibufs_foam;
  struct ImBuf **ibufs_norm;
  /* spray is Eplus */
  struct ImBuf **ibufs_spray;
  /* spray_inverse is Eminus */
  struct ImBuf **ibufs_spray_inverse;

  const char *bakepath;
  const char *relbase;

  /* precalculated for time range */
  float *time;

  /* constant for time range */
  float wave_scale;
  float chop_amount;
  float foam_coverage;
  float foam_fade;

  int start;
  int end;
  int duration;
  int resolution_x;
  int resolution_y;

  int baked;
};

struct Ocean *BKE_ocean_add();
void BKE_ocean_free_data(struct Ocean *oc);
void BKE_ocean_free(struct Ocean *oc);
bool BKE_ocean_ensure(struct OceanModifierData *omd, int resolution);
/**
 * Return true if the ocean data is valid and can be used.
 */
bool BKE_ocean_init_from_modifier(struct Ocean *ocean,
                                  struct OceanModifierData const *omd,
                                  int resolution);

/**
 * Return true if the ocean is valid and can be used.
 */
bool BKE_ocean_is_valid(const struct Ocean *o);

/**
 * Return true if the ocean data is valid and can be used.
 */
bool BKE_ocean_init(struct Ocean *o,
                    int M,
                    int N,
                    float Lx,
                    float Lz,
                    float V,
                    float l,
                    float A,
                    float w,
                    float damp,
                    float alignment,
                    float depth,
                    float time,
                    int spectrum,
                    float fetch_jonswap,
                    float sharpen_peak_jonswap,
                    short do_height_field,
                    short do_chop,
                    short do_spray,
                    short do_normals,
                    short do_jacobian,
                    int seed);
void BKE_ocean_simulate(struct Ocean *o, float t, float scale, float chop_amount);

float BKE_ocean_jminus_to_foam(float jminus, float coverage);
/**
 * Sampling the ocean surface.
 */
void BKE_ocean_eval_uv(struct Ocean *oc, struct OceanResult *ocr, float u, float v);
/**
 * Use catmullrom interpolation rather than linear.
 */
void BKE_ocean_eval_uv_catrom(struct Ocean *oc, struct OceanResult *ocr, float u, float v);
void BKE_ocean_eval_xz(struct Ocean *oc, struct OceanResult *ocr, float x, float z);
void BKE_ocean_eval_xz_catrom(struct Ocean *oc, struct OceanResult *ocr, float x, float z);
void BKE_ocean_eval_uv_split_support(struct Ocean *oc,
                                     struct OceanSplitResult *osr,
                                     float u,
                                     float v,
                                     const struct OceanSplitSupport *geometry_support,
                                     const struct OceanSplitSupport *camera_support);
void BKE_ocean_eval_uv_split(struct Ocean *oc,
                             struct OceanSplitResult *osr,
                             float u,
                             float v,
                             float geometry_wavelength,
                             float camera_wavelength);
void BKE_ocean_eval_xz_split_support(struct Ocean *oc,
                                     struct OceanSplitResult *osr,
                                     float x,
                                     float z,
                                     const struct OceanSplitSupport *geometry_support,
                                     const struct OceanSplitSupport *camera_support);
void BKE_ocean_eval_xz_split(struct Ocean *oc,
                             struct OceanSplitResult *osr,
                             float x,
                             float z,
                             float geometry_wavelength,
                             float camera_wavelength);
int BKE_ocean_split_level_count_get(const struct Ocean *oc);
float BKE_ocean_split_min_wavelength_get(const struct Ocean *oc);
uint64_t BKE_ocean_split_runtime_revision_get(const struct Ocean *oc);
bool BKE_ocean_split_runtime_level_get(const struct Ocean *oc,
                                       int level_index,
                                       struct OceanSplitRuntimeLevel *r_level);
bool BKE_ocean_split_runtime_sample_level(const struct Ocean *oc,
                                          int level_index,
                                          float u,
                                          float v,
                                          float *r_displacement,
                                          float *r_normal);
bool BKE_ocean_split_runtime_read_begin(const struct Ocean *oc,
                                        struct OceanSplitRuntimeReadScope *r_scope);
void BKE_ocean_split_runtime_read_end(struct OceanSplitRuntimeReadScope *scope);
bool BKE_ocean_split_runtime_level_get_in_scope(const struct OceanSplitRuntimeReadScope *scope,
                                                int level_index,
                                                struct OceanSplitRuntimeLevel *r_level);
bool BKE_ocean_split_runtime_sample_level_in_scope(const struct OceanSplitRuntimeReadScope *scope,
                                                   int level_index,
                                                   float u,
                                                   float v,
                                                   float *r_displacement,
                                                   float *r_normal);
bool BKE_ocean_split_runtime_level_normal_data_get(const struct Ocean *oc,
                                                   int level_index,
                                                   float *r_normal_data,
                                                   int normal_data_len);
/**
 * Note that this doesn't wrap properly for i, j < 0, but its not really meant for that being
 * just a way to get the raw data out to save in some image format.
 */
void BKE_ocean_eval_ij(struct Ocean *oc, struct OceanResult *ocr, int i, int j);

/**
 * Ocean cache handling.
 */
struct OceanCache *BKE_ocean_init_cache(const char *bakepath,
                                        const char *relbase,
                                        int start,
                                        int end,
                                        float wave_scale,
                                        float chop_amount,
                                        float foam_coverage,
                                        float foam_fade,
                                        int resolution);
void BKE_ocean_simulate_cache(struct OceanCache *och, int frame);

void BKE_ocean_bake(struct Ocean *o,
                    struct OceanCache *och,
                    void (*update_cb)(void *, float progress, int *cancel),
                    void *update_cb_data);
void BKE_ocean_cache_eval_uv(
    struct OceanCache *och, struct OceanResult *ocr, int f, float u, float v);
void BKE_ocean_cache_eval_ij(struct OceanCache *och, struct OceanResult *ocr, int f, int i, int j);

void BKE_ocean_free_cache(struct OceanCache *och);
void BKE_ocean_free_modifier_cache(struct OceanModifierData *omd);

/* `ocean_spectrum.cc` */

/**
 * Pierson-Moskowitz model, 1964, assumes waves reach equilibrium with wind.
 * Model is intended for large area 'fully developed' sea, where winds have been steadily blowing
 * for days over an area that includes hundreds of wavelengths on a side.
 */
float BLI_ocean_spectrum_piersonmoskowitz(const struct Ocean *oc, float kx, float kz);
/**
 * TMA extends the JONSWAP spectrum.
 * This spectral model is best suited to shallow water.
 */
float BLI_ocean_spectrum_texelmarsenarsloe(const struct Ocean *oc, float kx, float kz);
/**
 * Hasselmann et al, 1973. This model extends the Pierson-Moskowitz model with a peak sharpening
 * function This enhancement is an artificial construct to address the problem that the wave
 * spectrum is never fully developed.
 *
 * The fetch parameter represents the distance from a lee shore,
 * called the fetch, or the distance over which the wind blows with constant velocity.
 */
float BLI_ocean_spectrum_jonswap(const struct Ocean *oc, float kx, float kz);

}  // namespace blender
