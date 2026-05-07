/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Based on original code by Drew Whitehouse / Houdini Ocean Toolkit
 * OpenMP hints by Christian Schnellhammer
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <cstring>

#include "MEM_guardedalloc.h"

#include "DNA_modifier_types.h"
#include "DNA_scene_types.h"

#include "BLI_math_vector.h"
#include "BLI_path_utils.hh"
#include "BLI_rand.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"

#include "BKE_image.hh"
#include "BKE_image_format.hh"
#include "BKE_ocean.h"
#include "ocean_intern.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RE_texture.h"

#include "BLI_hash.h"

namespace blender {

#ifdef WITH_OCEANSIM

/* Ocean code */

static float nextfr(RNG *rng, float min, float max)
{
  return BLI_rng_get_float(rng) * (min - max) + max;
}

static float gaussRand(RNG *rng)
{
  /* NOTE: to avoid numerical problems with very small numbers, we make these variables
   * single-precision floats, but later we call the double-precision log() and sqrt() functions
   * instead of logf() and sqrtf(). */
  float x;
  float y;
  float length2;

  do {
    x = nextfr(rng, -1, 1);
    y = nextfr(rng, -1, 1);
    length2 = x * x + y * y;
  } while (length2 >= 1 || length2 == 0);

  return x * sqrtf(-2.0f * logf(length2) / length2);
}

/**
 * Some useful functions
 */
MINLINE float catrom(float p0, float p1, float p2, float p3, float f)
{
  return 0.5f * ((2.0f * p1) + (-p0 + p2) * f + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f * f +
                 (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f * f * f);
}

MINLINE float omega(float k, float depth)
{
  return sqrtf(GRAVITY * k * tanhf(k * depth));
}

/* modified Phillips spectrum */
static float Ph(Ocean *o, float kx, float kz)
{
  float tmp;
  float k2 = kx * kx + kz * kz;

  if (k2 == 0.0f) {
    return 0.0f; /* no DC component */
  }

  /* damp out the waves going in the direction opposite the wind */
  tmp = (o->_wx * kx + o->_wz * kz) / sqrtf(k2);
  if (tmp < 0) {
    tmp *= o->_damp_reflections;
  }

  return o->_A * expf(-1.0f / (k2 * (o->_L * o->_L))) * expf(-k2 * (o->_l * o->_l)) *
         powf(fabsf(tmp), o->_wind_alignment) / (k2 * k2);
}

static void compute_eigenstuff(OceanResult *ocr, float jxx, float jzz, float jxz)
{
  float a, b, qplus, qminus;
  a = jxx + jzz;
  b = sqrt((jxx - jzz) * (jxx - jzz) + 4 * jxz * jxz);

  ocr->Jminus = 0.5f * (a - b);
  ocr->Jplus = 0.5f * (a + b);

  qplus = (ocr->Jplus - jxx) / jxz;
  qminus = (ocr->Jminus - jxx) / jxz;

  a = sqrt(1 + qplus * qplus);
  b = sqrt(1 + qminus * qminus);

  ocr->Eplus[0] = 1.0f / a;
  ocr->Eplus[1] = 0.0f;
  ocr->Eplus[2] = qplus / a;

  ocr->Eminus[0] = 1.0f / b;
  ocr->Eminus[1] = 0.0f;
  ocr->Eminus[2] = qminus / b;
}

/*
 * instead of Complex.h
 * in fftw.h "fftw_complex" typedefed as double[2]
 * below you can see functions are needed to work with such complex numbers.
 */
static void init_complex(fftw_complex cmpl, float real, float image)
{
  cmpl[0] = real;
  cmpl[1] = image;
}

static void add_comlex_c(fftw_complex res, const fftw_complex cmpl1, const fftw_complex cmpl2)
{
  res[0] = cmpl1[0] + cmpl2[0];
  res[1] = cmpl1[1] + cmpl2[1];
}

static void mul_complex_f(fftw_complex res, const fftw_complex cmpl, float f)
{
  res[0] = cmpl[0] * double(f);
  res[1] = cmpl[1] * double(f);
}

static void mul_complex_c(fftw_complex res, const fftw_complex cmpl1, const fftw_complex cmpl2)
{
  fftwf_complex temp;
  temp[0] = cmpl1[0] * cmpl2[0] - cmpl1[1] * cmpl2[1];
  temp[1] = cmpl1[0] * cmpl2[1] + cmpl1[1] * cmpl2[0];
  res[0] = temp[0];
  res[1] = temp[1];
}

static float real_c(fftw_complex cmpl)
{
  return cmpl[0];
}

static float image_c(fftw_complex cmpl)
{
  return cmpl[1];
}

static void conj_complex(fftw_complex res, const fftw_complex cmpl1)
{
  res[0] = cmpl1[0];
  res[1] = -cmpl1[1];
}

static void exp_complex(fftw_complex res, fftw_complex cmpl)
{
  float r = expf(cmpl[0]);

  res[0] = cosf(cmpl[1]) * r;
  res[1] = sinf(cmpl[1]) * r;
}

static void ocean_split_level_free(OceanSplitLevel *level)
{
  if (!level) {
    return;
  }

  if (level->fft_plan != nullptr) {
    BLI_thread_lock(LOCK_FFTW);
    fftw_destroy_plan(level->fft_plan);
    BLI_thread_unlock(LOCK_FFTW);
    level->fft_plan = nullptr;
  }

  MEM_SAFE_FREE(level->fft_in);
  MEM_SAFE_FREE(level->fft_out);
  MEM_SAFE_FREE(level->disp_x);
  MEM_SAFE_FREE(level->disp_y);
  MEM_SAFE_FREE(level->disp_z);
  MEM_SAFE_FREE(level->normal_x);
  MEM_SAFE_FREE(level->normal_y);
  MEM_SAFE_FREE(level->normal_z);
  level->size_x = 0;
  level->size_y = 0;
  level->wavelength = 0.0f;
  zero_v3(level->cumulative_disp_variance);
  zero_v3(level->cumulative_slope_moment);
}

static void ocean_free_split_data(Ocean *o)
{
  if (!o || !o->_split_levels) {
    return;
  }

  for (int level_index = 0; level_index < o->_split_levels_num; level_index++) {
    ocean_split_level_free(&o->_split_levels[level_index]);
  }

  MEM_SAFE_FREE(o->_split_levels);
  o->_split_levels_num = 0;
}

static bool ocean_split_level_alloc(OceanSplitLevel *level,
                                    const int size_x,
                                    const int size_y,
                                    const bool needs_fft_plan)
{
  BLI_assert(level != nullptr);

  level->size_x = size_x;
  level->size_y = size_y;
  level->wavelength = 0.0f;
  level->fft_in = nullptr;
  level->fft_out = nullptr;
  level->fft_plan = nullptr;
  zero_v3(level->cumulative_disp_variance);
  zero_v3(level->cumulative_slope_moment);

  const size_t size = size_t(size_x) * size_t(size_y);
  level->disp_x = MEM_calloc_arrayN<float>(size, "ocean_split_disp_x");
  level->disp_y = MEM_calloc_arrayN<float>(size, "ocean_split_disp_y");
  level->disp_z = MEM_calloc_arrayN<float>(size, "ocean_split_disp_z");
  level->normal_x = MEM_calloc_arrayN<float>(size, "ocean_split_normal_x");
  level->normal_y = MEM_calloc_arrayN<float>(size, "ocean_split_normal_y");
  level->normal_z = MEM_calloc_arrayN<float>(size, "ocean_split_normal_z");

  if (!(level->disp_x && level->disp_y && level->disp_z && level->normal_x && level->normal_y &&
        level->normal_z))
  {
    ocean_split_level_free(level);
    return false;
  }

  if (needs_fft_plan) {
    level->fft_in = MEM_malloc_arrayN<fftw_complex>(
        size_t(size_x) * (1 + size_t(size_y) / 2), "ocean_split_fft_in");
    level->fft_out = MEM_malloc_arrayN<double>(size, "ocean_split_fft_out");
    if (!(level->fft_in && level->fft_out)) {
      ocean_split_level_free(level);
      return false;
    }

    BLI_thread_lock(LOCK_FFTW);
    level->fft_plan = fftw_plan_dft_c2r_2d(
        size_x, size_y, level->fft_in, level->fft_out, FFTW_ESTIMATE);
    BLI_thread_unlock(LOCK_FFTW);
    if (level->fft_plan == nullptr) {
      ocean_split_level_free(level);
      return false;
    }
  }

  return true;
}

static int ocean_split_level_count(const int size_x, const int size_y)
{
  int levels = 1;
  int x = size_x;
  int y = size_y;
  while (x > 1 || y > 1) {
    x = std::max(x / 2, 1);
    y = std::max(y / 2, 1);
    levels++;
  }
  return levels;
}

static void ocean_split_level_size(const Ocean *o,
                                   const int level_index,
                                   int *r_size_x,
                                   int *r_size_y)
{
  BLI_assert(o != nullptr);
  BLI_assert(r_size_x != nullptr);
  BLI_assert(r_size_y != nullptr);

  *r_size_x = std::max(o->_M >> level_index, 1);
  *r_size_y = std::max(o->_N >> level_index, 1);
}

static bool ocean_ensure_split_levels(Ocean *o)
{
  if (!o->_do_split) {
    return false;
  }

  if (o->_split_levels) {
    return true;
  }

  const int levels_num = ocean_split_level_count(o->_M, o->_N);
  o->_split_levels = MEM_calloc_arrayN<OceanSplitLevel>(size_t(levels_num), "ocean_split_levels");
  if (!o->_split_levels) {
    return false;
  }

  for (int level_index = 0; level_index < levels_num; level_index++) {
    int level_size_x, level_size_y;
    ocean_split_level_size(o, level_index, &level_size_x, &level_size_y);
    if (!ocean_split_level_alloc(
            &o->_split_levels[level_index], level_size_x, level_size_y, level_index > 0))
    {
      ocean_free_split_data(o);
      return false;
    }
  }

  o->_split_levels_num = levels_num;
  return true;
}

enum OceanSplitField {
  OCEAN_SPLIT_FIELD_DISP_Y = 0,
  OCEAN_SPLIT_FIELD_DISP_X = 1,
  OCEAN_SPLIT_FIELD_DISP_Z = 2,
};

static float ocean_split_min_wavelength(const Ocean *o);
static float ocean_split_level_wavelength(const Ocean *o, const int level_index);

static float ocean_split_mask_weight(const Ocean *o, const int level_index, const float k)
{
  if (level_index <= 0 || k <= 1.0e-12f) {
    return 1.0f;
  }

  const float wavelength = (2.0f * float(M_PI)) / k;
  const float min_wavelength = ocean_split_min_wavelength(o);
  const float log_lambda = log2f(std::max(wavelength / min_wavelength, 1.0f));
  const float edge0 = float(level_index - 1);
  const float edge1 = float(level_index);
  const float t = clamp_f((log_lambda - edge0) / std::max(edge1 - edge0, 1.0e-6f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static int ocean_split_frequency_signed_index(const int index, const int size)
{
  const int half_size = size / 2;
  return (index <= half_size) ? index : (index - size);
}

static int ocean_split_frequency_wrapped_index(const int signed_index, const int size)
{
  return (signed_index >= 0) ? signed_index : (size + signed_index);
}

static void ocean_split_prepare_spectrum_reduced(Ocean *o,
                                                 OceanSplitLevel &level,
                                                 const int level_index,
                                                 const OceanSplitField field,
                                                 const float scale,
                                                 const float chop_amount)
{
  BLI_assert(level.fft_in != nullptr);

  const int reduced_size_y_half = level.size_y / 2;
  for (int i = 0; i < level.size_x; i++) {
    const int signed_i = ocean_split_frequency_signed_index(i, level.size_x);
    const int full_i = ocean_split_frequency_wrapped_index(signed_i, o->_M);

    for (int j = 0; j <= reduced_size_y_half; j++) {
      const size_t full_index = size_t(full_i) * (1 + size_t(o->_N) / 2) + size_t(j);
      const float k = o->_k[full_index];
      const float mask = ocean_split_mask_weight(o, level_index, k);
      const fftw_complex &h = o->_htilda[full_index];

      fftw_complex coeff;
      switch (field) {
        case OCEAN_SPLIT_FIELD_DISP_Y:
          mul_complex_f(coeff, h, scale * mask);
          break;
        case OCEAN_SPLIT_FIELD_DISP_X: {
          fftw_complex minus_i;
          init_complex(minus_i, 0.0f, -1.0f);
          mul_complex_c(coeff, minus_i, h);
          mul_complex_f(
              coeff, coeff, (k == 0.0f) ? 0.0f : (-scale * chop_amount * mask * o->_kx[full_i] / k));
          break;
        }
        case OCEAN_SPLIT_FIELD_DISP_Z: {
          fftw_complex minus_i;
          init_complex(minus_i, 0.0f, -1.0f);
          mul_complex_c(coeff, minus_i, h);
          mul_complex_f(
              coeff, coeff, (k == 0.0f) ? 0.0f : (-scale * chop_amount * mask * o->_kz[j] / k));
          break;
        }
      }

      init_complex(level.fft_in[size_t(i) * (1 + size_t(reduced_size_y_half)) + size_t(j)],
                   real_c(coeff),
                   image_c(coeff));
    }
  }
}

MINLINE int ocean_split_wrap_index(const int index, const int size)
{
  return (index < 0) ? (index + size) : ((index >= size) ? (index - size) : index);
}

MINLINE size_t ocean_split_level_index(const OceanSplitLevel &level, const int x, const int y)
{
  return size_t(x) * size_t(level.size_y) + size_t(y);
}

static void ocean_split_level_update_normals_and_moments(const Ocean *o, OceanSplitLevel &level)
{
  zero_v3(level.cumulative_disp_variance);
  zero_v3(level.cumulative_slope_moment);

  if (level.size_x <= 0 || level.size_y <= 0) {
    return;
  }

  const float cell_x = (level.size_x > 0) ? (o->_Lx / float(level.size_x)) : 0.0f;
  const float cell_z = (level.size_y > 0) ? (o->_Lz / float(level.size_y)) : 0.0f;
  const float inv_dx = (cell_x > 1.0e-12f) ? (0.5f / cell_x) : 0.0f;
  const float inv_dz = (cell_z > 1.0e-12f) ? (0.5f / cell_z) : 0.0f;
  const float inv_sample_count = 1.0f / float(size_t(level.size_x) * size_t(level.size_y));

  for (int x = 0; x < level.size_x; x++) {
    const int xm = ocean_split_wrap_index(x - 1, level.size_x);
    const int xp = ocean_split_wrap_index(x + 1, level.size_x);
    for (int y = 0; y < level.size_y; y++) {
      const int ym = ocean_split_wrap_index(y - 1, level.size_y);
      const int yp = ocean_split_wrap_index(y + 1, level.size_y);

      const size_t index = ocean_split_level_index(level, x, y);
      const float disp_x = level.disp_x[index];
      const float disp_y = level.disp_y[index];
      const float disp_z = level.disp_z[index];

      level.cumulative_disp_variance[0] += disp_x * disp_x * inv_sample_count;
      level.cumulative_disp_variance[1] += disp_y * disp_y * inv_sample_count;
      level.cumulative_disp_variance[2] += disp_z * disp_z * inv_sample_count;

      const float ddx_dx = (level.disp_x[ocean_split_level_index(level, xp, y)] -
                            level.disp_x[ocean_split_level_index(level, xm, y)]) *
                           inv_dx;
      const float ddx_dz = (level.disp_x[ocean_split_level_index(level, x, yp)] -
                            level.disp_x[ocean_split_level_index(level, x, ym)]) *
                           inv_dz;
      const float ddy_dx = (level.disp_y[ocean_split_level_index(level, xp, y)] -
                            level.disp_y[ocean_split_level_index(level, xm, y)]) *
                           inv_dx;
      const float ddy_dz = (level.disp_y[ocean_split_level_index(level, x, yp)] -
                            level.disp_y[ocean_split_level_index(level, x, ym)]) *
                           inv_dz;
      const float ddz_dx = (level.disp_z[ocean_split_level_index(level, xp, y)] -
                            level.disp_z[ocean_split_level_index(level, xm, y)]) *
                           inv_dx;
      const float ddz_dz = (level.disp_z[ocean_split_level_index(level, x, yp)] -
                            level.disp_z[ocean_split_level_index(level, x, ym)]) *
                           inv_dz;

      float tangent_x[3] = {1.0f + ddx_dx, ddy_dx, ddz_dx};
      float tangent_z[3] = {ddx_dz, ddy_dz, 1.0f + ddz_dz};
      float normal[3];
      cross_v3_v3v3(normal, tangent_z, tangent_x);
      if (normalize_v3(normal) == 0.0f) {
        normal[0] = 0.0f;
        normal[1] = 1.0f;
        normal[2] = 0.0f;
      }

      level.normal_x[index] = normal[0];
      level.normal_y[index] = normal[1];
      level.normal_z[index] = normal[2];

      const float safe_normal_y = (fabsf(normal[1]) > 1.0e-6f) ? normal[1] :
                                                              ((normal[1] < 0.0f) ? -1.0e-6f :
                                                                                     1.0e-6f);
      const float slope_x = -normal[0] / safe_normal_y;
      const float slope_z = -normal[2] / safe_normal_y;
      level.cumulative_slope_moment[0] += slope_x * slope_x * inv_sample_count;
      level.cumulative_slope_moment[1] += slope_x * slope_z * inv_sample_count;
      level.cumulative_slope_moment[2] += slope_z * slope_z * inv_sample_count;
    }
  }
}

static void ocean_split_copy_base_level(Ocean *o)
{
  OceanSplitLevel &base_level = o->_split_levels[0];
  base_level.wavelength = ocean_split_min_wavelength(o);
  const size_t size = size_t(o->_M) * size_t(o->_N);

  for (size_t index = 0; index < size; index++) {
    base_level.disp_y[index] = o->_do_disp_y ? float(o->_disp_y[index]) : 0.0f;
    base_level.disp_x[index] = o->_do_chop ? float(o->_disp_x[index]) : 0.0f;
    base_level.disp_z[index] = o->_do_chop ? float(o->_disp_z[index]) : 0.0f;
  }
  ocean_split_level_update_normals_and_moments(o, base_level);
}

static void ocean_split_copy_fft_output(float *dst, const double *src, const size_t size)
{
  for (size_t index = 0; index < size; index++) {
    dst[index] = float(src[index]);
  }
}

static void ocean_split_build_spectral_pyramid(Ocean *o, const float scale, const float chop_amount)
{
  if (!o->_do_split || !o->_do_normals) {
    return;
  }

  if (!ocean_ensure_split_levels(o)) {
    return;
  }

  ocean_split_copy_base_level(o);

  const size_t size = size_t(o->_M) * size_t(o->_N);
  for (int level_index = 1; level_index < o->_split_levels_num; level_index++) {
    OceanSplitLevel &level = o->_split_levels[level_index];
    BLI_assert(level.fft_plan != nullptr);
    level.wavelength = ocean_split_level_wavelength(o, level_index);

    ocean_split_prepare_spectrum_reduced(
        o, level, level_index, OCEAN_SPLIT_FIELD_DISP_Y, scale, chop_amount);
    fftw_execute(level.fft_plan);
    ocean_split_copy_fft_output(
        level.disp_y, level.fft_out, size_t(level.size_x) * size_t(level.size_y));

    if (o->_do_chop) {
      ocean_split_prepare_spectrum_reduced(
          o, level, level_index, OCEAN_SPLIT_FIELD_DISP_X, scale, chop_amount);
      fftw_execute(level.fft_plan);
      ocean_split_copy_fft_output(
          level.disp_x, level.fft_out, size_t(level.size_x) * size_t(level.size_y));

      ocean_split_prepare_spectrum_reduced(
          o, level, level_index, OCEAN_SPLIT_FIELD_DISP_Z, scale, chop_amount);
      fftw_execute(level.fft_plan);
      ocean_split_copy_fft_output(
          level.disp_z, level.fft_out, size_t(level.size_x) * size_t(level.size_y));
    }
    else {
      memset(level.disp_x, 0, sizeof(float) * size_t(level.size_x) * size_t(level.size_y));
      memset(level.disp_z, 0, sizeof(float) * size_t(level.size_x) * size_t(level.size_y));
    }

    ocean_split_level_update_normals_and_moments(o, level);
  }

  const OceanSplitLevel &base_level = o->_split_levels[0];
  for (size_t index = 0; index < size; index++) {
    if (o->_do_disp_y) {
      o->_disp_y[index] = double(base_level.disp_y[index]);
    }
    if (o->_do_chop) {
      o->_disp_x[index] = double(base_level.disp_x[index]);
      o->_disp_z[index] = double(base_level.disp_z[index]);
    }
  }

  o->_split_runtime_revision++;
}

static float ocean_split_sample_bilerp(const float *field,
                                       const int size_x,
                                       const int size_y,
                                       float u,
                                       float v)
{
  if (!field || size_x <= 0 || size_y <= 0) {
    return 0.0f;
  }

  u = fmodf(u, 1.0f);
  v = fmodf(v, 1.0f);
  if (u < 0.0f) {
    u += 1.0f;
  }
  if (v < 0.0f) {
    v += 1.0f;
  }

  const float uu = u * size_x;
  const float vv = v * size_y;
  const int x0 = int(floorf(uu)) % size_x;
  const int y0 = int(floorf(vv)) % size_y;
  const int x1 = (x0 + 1) % size_x;
  const int y1 = (y0 + 1) % size_y;
  const float frac_x = uu - floorf(uu);
  const float frac_y = vv - floorf(vv);

  return interpf(interpf(field[x1 * size_y + y1], field[x0 * size_y + y1], frac_x),
                 interpf(field[x1 * size_y + y0], field[x0 * size_y + y0], frac_x),
                 frac_y);
}

static float ocean_split_min_wavelength(const Ocean *o)
{
  const float cell_x = (o->_M > 0) ? (o->_Lx / float(o->_M)) : 0.0f;
  const float cell_z = (o->_N > 0) ? (o->_Lz / float(o->_N)) : 0.0f;
  return std::max(2.0f * std::max(cell_x, cell_z), 1.0e-6f);
}

static float ocean_split_support_to_level(const Ocean *o, const float support_wavelength)
{
  const float min_wavelength = ocean_split_min_wavelength(o);
  const float clamped_wavelength = std::max(support_wavelength, min_wavelength);
  return std::max(0.0f, log2f(clamped_wavelength / min_wavelength));
}

static float ocean_split_level_wavelength(const Ocean *o, const int level_index)
{
  return ocean_split_min_wavelength(o) * exp2f(float(level_index));
}

static float ocean_split_level_variance(const Ocean *o, const int level_index)
{
  if (level_index <= 0) {
    return 0.0f;
  }

  const float sigma = 0.5f * ocean_split_level_wavelength(o, level_index);
  const float sigma_min = 0.5f * ocean_split_min_wavelength(o);
  return std::max(0.0f, sigma * sigma - sigma_min * sigma_min);
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

static int ocean_split_support_base_level(const Ocean *o,
                                          const OceanSplitSupport &support,
                                          float *r_base_variance)
{
  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(support.covariance, &minor_variance, &major_variance);

  const float minor_wavelength = 2.0f * sqrtf(std::max(minor_variance, 0.0f));
  int level_index = int(floorf(ocean_split_support_to_level(o, minor_wavelength)));
  level_index = std::clamp(level_index, 0, o->_split_levels_num - 1);

  float base_variance = ocean_split_level_variance(o, level_index);
  while (level_index > 0 && base_variance > minor_variance + 1.0e-10f) {
    level_index--;
    base_variance = ocean_split_level_variance(o, level_index);
  }

  *r_base_variance = std::min(base_variance, minor_variance);
  return level_index;
}

static void ocean_split_covariance_subtract_isotropic(float r_residual_covariance[3],
                                                      const float covariance[3],
                                                      const float isotropic_variance)
{
  r_residual_covariance[0] = covariance[0] - isotropic_variance;
  r_residual_covariance[1] = covariance[1];
  r_residual_covariance[2] = covariance[2] - isotropic_variance;
  ocean_split_covariance_project_psd(r_residual_covariance);
}

static float ocean_split_sample_anisotropic(const float *field,
                                            const int size_x,
                                            const int size_y,
                                            const float u,
                                            const float v,
                                            const float residual_covariance[3],
                                            const float cell_x,
                                            const float cell_z)
{
  float texel_covariance[3] = {
      residual_covariance[0] / std::max(cell_x * cell_x, 1.0e-12f),
      residual_covariance[1] / std::max(cell_x * cell_z, 1.0e-12f),
      residual_covariance[2] / std::max(cell_z * cell_z, 1.0e-12f),
  };
  ocean_split_covariance_project_psd(texel_covariance);

  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(texel_covariance, &minor_variance, &major_variance);
  if (major_variance <= 1.0e-8f) {
    return ocean_split_sample_bilerp(field, size_x, size_y, u, v);
  }

  const float determinant = texel_covariance[0] * texel_covariance[2] -
                            texel_covariance[1] * texel_covariance[1];
  if (determinant <= 1.0e-10f) {
    return ocean_split_sample_bilerp(field, size_x, size_y, u, v);
  }

  const float inv_covariance[3] = {
      texel_covariance[2] / determinant,
      -texel_covariance[1] / determinant,
      texel_covariance[0] / determinant,
  };

  constexpr float kernel_sigma_radius = 3.5f;
  constexpr int max_kernel_radius = 16;
  const int radius_x = std::min(
      max_kernel_radius,
      std::max(0, int(ceilf(kernel_sigma_radius * sqrtf(std::max(texel_covariance[0], 0.0f))))));
  const int radius_y = std::min(
      max_kernel_radius,
      std::max(0, int(ceilf(kernel_sigma_radius * sqrtf(std::max(texel_covariance[2], 0.0f))))));

  if (radius_x == 0 && radius_y == 0) {
    return ocean_split_sample_bilerp(field, size_x, size_y, u, v);
  }

  const float q_max = kernel_sigma_radius * kernel_sigma_radius;
  float weight_sum = 0.0f;
  float value_sum = 0.0f;

  for (int offset_x = -radius_x; offset_x <= radius_x; offset_x++) {
    for (int offset_y = -radius_y; offset_y <= radius_y; offset_y++) {
      const float q = inv_covariance[0] * float(offset_x * offset_x) +
                      2.0f * inv_covariance[1] * float(offset_x * offset_y) +
                      inv_covariance[2] * float(offset_y * offset_y);
      if (q > q_max) {
        continue;
      }

      const float weight = expf(-0.5f * q);
      if (weight <= 1.0e-8f) {
        continue;
      }

      const float sample_u = u + (float(offset_x) / float(size_x));
      const float sample_v = v + (float(offset_y) / float(size_y));
      value_sum += weight * ocean_split_sample_bilerp(field, size_x, size_y, sample_u, sample_v);
      weight_sum += weight;
    }
  }

  if (weight_sum <= 1.0e-8f) {
    return ocean_split_sample_bilerp(field, size_x, size_y, u, v);
  }
  return value_sum / weight_sum;
}

static float ocean_split_sample_field_support(const Ocean *o,
                                              float *OceanSplitLevel::*field,
                                              const float u,
                                              const float v,
                                              const OceanSplitSupport &support)
{
  if (!o->_split_levels || o->_split_levels_num == 0) {
    return 0.0f;
  }

  float base_variance = 0.0f;
  const int level_index = ocean_split_support_base_level(o, support, &base_variance);
  const OceanSplitLevel &split_level = o->_split_levels[level_index];

  float residual_covariance[3];
  ocean_split_covariance_subtract_isotropic(residual_covariance, support.covariance, base_variance);

  const float cell_x = (split_level.size_x > 0) ? (o->_Lx / float(split_level.size_x)) : 0.0f;
  const float cell_z = (split_level.size_y > 0) ? (o->_Lz / float(split_level.size_y)) : 0.0f;

  return ocean_split_sample_anisotropic(
      split_level.*field, split_level.size_x, split_level.size_y, u, v, residual_covariance, cell_x, cell_z);
}

static void ocean_split_support_visible_moment(const Ocean *o,
                                               const OceanSplitSupport &support,
                                               float r_moment[3])
{
  zero_v3(r_moment);
  if (!o->_split_levels || o->_split_levels_num == 0) {
    return;
  }

  float minor_variance, major_variance;
  ocean_split_covariance_eigenvalues(support.covariance, &minor_variance, &major_variance);

  float base_variance = 0.0f;
  const int level_index = ocean_split_support_base_level(o, support, &base_variance);
  const int next_level_index = std::min(level_index + 1, o->_split_levels_num - 1);
  const OceanSplitLevel &base_level = o->_split_levels[level_index];
  const OceanSplitLevel &next_level = o->_split_levels[next_level_index];

  if (next_level_index == level_index) {
    copy_v3_v3(r_moment, base_level.cumulative_slope_moment);
    return;
  }

  const float next_variance = ocean_split_level_variance(o, next_level_index);
  const float denom = std::max(next_variance - base_variance, 1.0e-12f);
  const float t = clamp_f((minor_variance - base_variance) / denom, 0.0f, 1.0f);

  r_moment[0] = base_level.cumulative_slope_moment[0] +
                (next_level.cumulative_slope_moment[0] - base_level.cumulative_slope_moment[0]) * t;
  r_moment[1] = base_level.cumulative_slope_moment[1] +
                (next_level.cumulative_slope_moment[1] - base_level.cumulative_slope_moment[1]) * t;
  r_moment[2] = base_level.cumulative_slope_moment[2] +
                (next_level.cumulative_slope_moment[2] - base_level.cumulative_slope_moment[2]) * t;
}

static void ocean_split_slope_from_normal(const float normal[3], float r_slope[2])
{
  const float safe_normal_y = (fabsf(normal[1]) > 1.0e-6f) ? normal[1] :
                                                          ((normal[1] < 0.0f) ? -1.0e-6f :
                                                                                 1.0e-6f);
  r_slope[0] = -normal[0] / safe_normal_y;
  r_slope[1] = -normal[2] / safe_normal_y;
}

static void ocean_split_sample_normal_support(const Ocean *o,
                                              float r_normal[3],
                                              const float u,
                                              const float v,
                                              const OceanSplitSupport &support)
{
  r_normal[0] = ocean_split_sample_field_support(o, &OceanSplitLevel::normal_x, u, v, support);
  r_normal[1] = ocean_split_sample_field_support(o, &OceanSplitLevel::normal_y, u, v, support);
  r_normal[2] = ocean_split_sample_field_support(o, &OceanSplitLevel::normal_z, u, v, support);
  if (normalize_v3(r_normal) == 0.0f) {
    r_normal[0] = 0.0f;
    r_normal[1] = 1.0f;
    r_normal[2] = 0.0f;
  }
}

static OceanSplitSupport ocean_split_isotropic_support(const float wavelength)
{
  OceanSplitSupport support{};
  const float half = 0.5f * wavelength;
  support.wavelength_x = wavelength;
  support.wavelength_z = wavelength;
  support.wavelength_major = wavelength;
  support.covariance[0] = half * half;
  support.covariance[1] = 0.0f;
  support.covariance[2] = half * half;
  return support;
}

static void ocean_split_support_sanitize(const Ocean *o, OceanSplitSupport *support)
{
  const float min_wavelength = ocean_split_min_wavelength(o);

  support->wavelength_x = std::max(support->wavelength_x, min_wavelength);
  support->wavelength_z = std::max(support->wavelength_z, min_wavelength);
  support->wavelength_major = std::max(
      std::max(support->wavelength_major, support->wavelength_x), support->wavelength_z);

  if (support->covariance[0] <= 0.0f) {
    const float sigma_x = 0.5f * support->wavelength_x;
    support->covariance[0] = sigma_x * sigma_x;
  }
  if (support->covariance[2] <= 0.0f) {
    const float sigma_z = 0.5f * support->wavelength_z;
    support->covariance[2] = sigma_z * sigma_z;
  }

  const float sigma_limit = sqrtf(fmaxf(support->covariance[0] * support->covariance[2], 0.0f));
  support->covariance[1] = std::clamp(support->covariance[1], -sigma_limit, sigma_limit);
}

float BKE_ocean_jminus_to_foam(float jminus, float coverage)
{
  float foam = jminus * -0.005f + coverage;
  CLAMP(foam, 0.0f, 1.0f);
  return foam;
}

static void ocean_eval_uv_locked(const Ocean *oc, OceanResult *ocr, float u, float v)
{
  int i0, i1, j0, j1;
  float frac_x, frac_z;
  float uu, vv;

  /* first wrap the texture so 0 <= (u, v) < 1 */
  u = fmodf(u, 1.0f);
  v = fmodf(v, 1.0f);

  if (u < 0) {
    u += 1.0f;
  }
  if (v < 0) {
    v += 1.0f;
  }

  uu = u * oc->_M;
  vv = v * oc->_N;

  i0 = int(floor(uu));
  j0 = int(floor(vv));

  i1 = (i0 + 1);
  j1 = (j0 + 1);

  frac_x = uu - i0;
  frac_z = vv - j0;

  i0 = i0 % oc->_M;
  j0 = j0 % oc->_N;

  i1 = i1 % oc->_M;
  j1 = j1 % oc->_N;

#  define BILERP(m) \
    interpf(interpf(m[i1 * oc->_N + j1], m[i0 * oc->_N + j1], frac_x), \
            interpf(m[i1 * oc->_N + j0], m[i0 * oc->_N + j0], frac_x), \
            frac_z)

  {
    if (oc->_do_disp_y) {
      ocr->disp[1] = BILERP(oc->_disp_y);
    }

    if (oc->_do_normals) {
      ocr->normal[0] = BILERP(oc->_N_x);
      ocr->normal[1] = oc->_N_y /* BILERP(oc->_N_y) (MEM01) */;
      ocr->normal[2] = BILERP(oc->_N_z);
    }

    if (oc->_do_chop) {
      ocr->disp[0] = BILERP(oc->_disp_x);
      ocr->disp[2] = BILERP(oc->_disp_z);
    }
    else {
      ocr->disp[0] = 0.0;
      ocr->disp[2] = 0.0;
    }

    if (oc->_do_jacobian) {
      compute_eigenstuff(ocr, BILERP(oc->_Jxx), BILERP(oc->_Jzz), BILERP(oc->_Jxz));
    }
  }
#  undef BILERP

}

void BKE_ocean_eval_uv(Ocean *oc, OceanResult *ocr, float u, float v)
{
  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);
  ocean_eval_uv_locked(oc, ocr, u, v);
  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

bool BKE_ocean_runtime_read_begin(const Ocean *oc, OceanRuntimeReadScope *r_scope)
{
  if (!oc || !r_scope) {
    return false;
  }

  BLI_rw_mutex_lock(const_cast<ThreadRWMutex *>(&oc->oceanmutex), THREAD_LOCK_READ);
  r_scope->ocean = oc;
  return true;
}

void BKE_ocean_runtime_read_end(OceanRuntimeReadScope *scope)
{
  if (!scope || !scope->ocean) {
    return;
  }

  BLI_rw_mutex_unlock(const_cast<ThreadRWMutex *>(&scope->ocean->oceanmutex));
  scope->ocean = nullptr;
}

bool BKE_ocean_eval_uv_in_scope(const OceanRuntimeReadScope *scope,
                                OceanResult *ocr,
                                const float u,
                                const float v)
{
  if (!scope || !scope->ocean || !ocr) {
    return false;
  }

  ocean_eval_uv_locked(scope->ocean, ocr, u, v);
  return true;
}

void BKE_ocean_eval_uv_catrom(Ocean *oc, OceanResult *ocr, float u, float v)
{
  int i0, i1, i2, i3, j0, j1, j2, j3;
  float frac_x, frac_z;
  float uu, vv;

  /* first wrap the texture so 0 <= (u, v) < 1 */
  u = fmod(u, 1.0f);
  v = fmod(v, 1.0f);

  if (u < 0) {
    u += 1.0f;
  }
  if (v < 0) {
    v += 1.0f;
  }

  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);

  uu = u * oc->_M;
  vv = v * oc->_N;

  i1 = int(floor(uu));
  j1 = int(floor(vv));

  i2 = (i1 + 1);
  j2 = (j1 + 1);

  frac_x = uu - i1;
  frac_z = vv - j1;

  i1 = i1 % oc->_M;
  j1 = j1 % oc->_N;

  i2 = i2 % oc->_M;
  j2 = j2 % oc->_N;

  i0 = (i1 - 1);
  i3 = (i2 + 1);
  i0 = i0 < 0 ? i0 + oc->_M : i0;
  i3 = i3 >= oc->_M ? i3 - oc->_M : i3;

  j0 = (j1 - 1);
  j3 = (j2 + 1);
  j0 = j0 < 0 ? j0 + oc->_N : j0;
  j3 = j3 >= oc->_N ? j3 - oc->_N : j3;

#  define INTERP(m) \
    catrom(catrom(m[i0 * oc->_N + j0], \
                  m[i1 * oc->_N + j0], \
                  m[i2 * oc->_N + j0], \
                  m[i3 * oc->_N + j0], \
                  frac_x), \
           catrom(m[i0 * oc->_N + j1], \
                  m[i1 * oc->_N + j1], \
                  m[i2 * oc->_N + j1], \
                  m[i3 * oc->_N + j1], \
                  frac_x), \
           catrom(m[i0 * oc->_N + j2], \
                  m[i1 * oc->_N + j2], \
                  m[i2 * oc->_N + j2], \
                  m[i3 * oc->_N + j2], \
                  frac_x), \
           catrom(m[i0 * oc->_N + j3], \
                  m[i1 * oc->_N + j3], \
                  m[i2 * oc->_N + j3], \
                  m[i3 * oc->_N + j3], \
                  frac_x), \
           frac_z)

  {
    if (oc->_do_disp_y) {
      ocr->disp[1] = INTERP(oc->_disp_y);
    }
    if (oc->_do_normals) {
      ocr->normal[0] = INTERP(oc->_N_x);
      ocr->normal[1] = oc->_N_y /* INTERP(oc->_N_y) (MEM01) */;
      ocr->normal[2] = INTERP(oc->_N_z);
    }
    if (oc->_do_chop) {
      ocr->disp[0] = INTERP(oc->_disp_x);
      ocr->disp[2] = INTERP(oc->_disp_z);
    }
    else {
      ocr->disp[0] = 0.0;
      ocr->disp[2] = 0.0;
    }

    if (oc->_do_jacobian) {
      compute_eigenstuff(ocr, INTERP(oc->_Jxx), INTERP(oc->_Jzz), INTERP(oc->_Jxz));
    }
  }
#  undef INTERP

  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

void BKE_ocean_eval_xz(Ocean *oc, OceanResult *ocr, float x, float z)
{
  BKE_ocean_eval_uv(oc, ocr, x / oc->_Lx, z / oc->_Lz);
}

void BKE_ocean_eval_xz_catrom(Ocean *oc, OceanResult *ocr, float x, float z)
{
  BKE_ocean_eval_uv_catrom(oc, ocr, x / oc->_Lx, z / oc->_Lz);
}

void BKE_ocean_eval_uv_split_support(Ocean *oc,
                                     OceanSplitResult *osr,
                                     float u,
                                     float v,
                                     const OceanSplitSupport *geometry_support_in,
                                     const OceanSplitSupport *camera_support_in)
{
  memset(osr, 0, sizeof(*osr));
  OceanSplitSupport geometry_support = geometry_support_in ?
                                           *geometry_support_in :
                                           ocean_split_isotropic_support(0.0f);
  OceanSplitSupport camera_support = camera_support_in ? *camera_support_in :
                                                      ocean_split_isotropic_support(0.0f);

  if (!oc) {
    osr->geometry_normal[1] = 1.0f;
    osr->visible_normal[1] = 1.0f;
    osr->geometry_wavelength = geometry_support.wavelength_major;
    osr->camera_wavelength = camera_support.wavelength_major;
    osr->geometry_wavelength_xz[0] = geometry_support.wavelength_x;
    osr->geometry_wavelength_xz[1] = geometry_support.wavelength_z;
    osr->camera_wavelength_xz[0] = camera_support.wavelength_x;
    osr->camera_wavelength_xz[1] = camera_support.wavelength_z;
    copy_v3_v3(osr->geometry_support_covariance, geometry_support.covariance);
    copy_v3_v3(osr->camera_support_covariance, camera_support.covariance);
    return;
  }

  ocean_split_support_sanitize(oc, &geometry_support);
  ocean_split_support_sanitize(oc, &camera_support);

  osr->geometry_wavelength = geometry_support.wavelength_major;
  osr->camera_wavelength = camera_support.wavelength_major;
  osr->geometry_wavelength_xz[0] = geometry_support.wavelength_x;
  osr->geometry_wavelength_xz[1] = geometry_support.wavelength_z;
  osr->camera_wavelength_xz[0] = camera_support.wavelength_x;
  osr->camera_wavelength_xz[1] = camera_support.wavelength_z;
  copy_v3_v3(osr->geometry_support_covariance, geometry_support.covariance);
  copy_v3_v3(osr->camera_support_covariance, camera_support.covariance);

  if (!oc->_split_levels || oc->_split_levels_num == 0) {
    OceanResult full_result{};
    BKE_ocean_eval_uv(oc, &full_result, u, v);
    copy_v3_v3(osr->geometry_disp, full_result.disp);
    copy_v3_v3(osr->geometry_normal, full_result.normal);
    copy_v3_v3(osr->visible_normal, full_result.normal);
    if (is_zero_v3(osr->geometry_normal)) {
      osr->geometry_normal[1] = 1.0f;
    }
    if (is_zero_v3(osr->visible_normal)) {
      osr->visible_normal[1] = 1.0f;
    }
    normalize_v3(osr->geometry_normal);
    normalize_v3(osr->visible_normal);
    return;
  }

  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);

  osr->geometry_disp[0] = ocean_split_sample_field_support(
      oc, &OceanSplitLevel::disp_x, u, v, geometry_support);
  osr->geometry_disp[1] = ocean_split_sample_field_support(
      oc, &OceanSplitLevel::disp_y, u, v, geometry_support);
  osr->geometry_disp[2] = ocean_split_sample_field_support(
      oc, &OceanSplitLevel::disp_z, u, v, geometry_support);

  ocean_split_sample_normal_support(oc, osr->geometry_normal, u, v, geometry_support);
  ocean_split_sample_normal_support(oc, osr->visible_normal, u, v, camera_support);

  float geometry_slope[2];
  float visible_slope[2];
  ocean_split_slope_from_normal(osr->geometry_normal, geometry_slope);
  ocean_split_slope_from_normal(osr->visible_normal, visible_slope);

  osr->visible_residual_slope[0] = visible_slope[0] - geometry_slope[0];
  osr->visible_residual_slope[1] = visible_slope[1] - geometry_slope[1];

  float visible_moment[3];
  ocean_split_support_visible_moment(oc, camera_support, visible_moment);

  osr->unresolved_slope_covariance[0] = oc->_split_levels[0].cumulative_slope_moment[0] -
                                        visible_moment[0];
  osr->unresolved_slope_covariance[1] = oc->_split_levels[0].cumulative_slope_moment[1] -
                                        visible_moment[1];
  osr->unresolved_slope_covariance[2] = oc->_split_levels[0].cumulative_slope_moment[2] -
                                        visible_moment[2];
  ocean_split_covariance_project_psd(osr->unresolved_slope_covariance);

  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

void BKE_ocean_eval_uv_split(Ocean *oc,
                             OceanSplitResult *osr,
                             float u,
                             float v,
                             float geometry_wavelength,
                             float camera_wavelength)
{
  const OceanSplitSupport geometry_support = ocean_split_isotropic_support(geometry_wavelength);
  const OceanSplitSupport camera_support = ocean_split_isotropic_support(camera_wavelength);
  BKE_ocean_eval_uv_split_support(oc, osr, u, v, &geometry_support, &camera_support);
}

void BKE_ocean_eval_xz_split_support(Ocean *oc,
                                     OceanSplitResult *osr,
                                     float x,
                                     float z,
                                     const OceanSplitSupport *geometry_support,
                                     const OceanSplitSupport *camera_support)
{
  if (!oc) {
    memset(osr, 0, sizeof(*osr));
    osr->geometry_normal[1] = 1.0f;
    osr->visible_normal[1] = 1.0f;
    if (geometry_support) {
      osr->geometry_wavelength = geometry_support->wavelength_major;
      osr->geometry_wavelength_xz[0] = geometry_support->wavelength_x;
      osr->geometry_wavelength_xz[1] = geometry_support->wavelength_z;
      copy_v3_v3(osr->geometry_support_covariance, geometry_support->covariance);
    }
    if (camera_support) {
      osr->camera_wavelength = camera_support->wavelength_major;
      osr->camera_wavelength_xz[0] = camera_support->wavelength_x;
      osr->camera_wavelength_xz[1] = camera_support->wavelength_z;
      copy_v3_v3(osr->camera_support_covariance, camera_support->covariance);
    }
    return;
  }

  BKE_ocean_eval_uv_split_support(oc, osr, x / oc->_Lx, z / oc->_Lz, geometry_support, camera_support);
}

void BKE_ocean_eval_xz_split(Ocean *oc,
                             OceanSplitResult *osr,
                             float x,
                             float z,
                             float geometry_wavelength,
                             float camera_wavelength)
{
  const OceanSplitSupport geometry_support = ocean_split_isotropic_support(geometry_wavelength);
  const OceanSplitSupport camera_support = ocean_split_isotropic_support(camera_wavelength);
  BKE_ocean_eval_xz_split_support(oc, osr, x, z, &geometry_support, &camera_support);
}

int BKE_ocean_split_level_count_get(const Ocean *oc)
{
  return (oc && oc->_split_levels) ? oc->_split_levels_num : 0;
}

float BKE_ocean_split_min_wavelength_get(const Ocean *oc)
{
  return oc ? ocean_split_min_wavelength(oc) : 0.0f;
}

uint64_t BKE_ocean_split_runtime_revision_get(const Ocean *oc)
{
  return oc ? oc->_split_runtime_revision : 0;
}

static bool ocean_split_runtime_level_get_locked(const Ocean *oc,
                                                 const int level_index,
                                                 OceanSplitRuntimeLevel *r_level)
{
  if (!oc || !r_level || !oc->_split_levels || level_index < 0 || level_index >= oc->_split_levels_num) {
    return false;
  }

  const OceanSplitLevel &level = oc->_split_levels[level_index];
  r_level->size_x = level.size_x;
  r_level->size_y = level.size_y;
  r_level->wavelength = level.wavelength;
  copy_v3_v3(r_level->cumulative_disp_variance, level.cumulative_disp_variance);
  copy_v3_v3(r_level->cumulative_slope_moment, level.cumulative_slope_moment);
  return true;
}

static bool ocean_split_runtime_sample_level_locked(const Ocean *oc,
                                                    const int level_index,
                                                    const float u,
                                                    const float v,
                                                    float *r_displacement,
                                                    float *r_normal)
{
  if (!oc || !oc->_split_levels || level_index < 0 || level_index >= oc->_split_levels_num ||
      (!r_displacement && !r_normal))
  {
    return false;
  }

  const OceanSplitLevel &level = oc->_split_levels[level_index];
  if (r_displacement) {
    r_displacement[0] = ocean_split_sample_bilerp(level.disp_x, level.size_x, level.size_y, u, v);
    r_displacement[1] = ocean_split_sample_bilerp(level.disp_y, level.size_x, level.size_y, u, v);
    r_displacement[2] = ocean_split_sample_bilerp(level.disp_z, level.size_x, level.size_y, u, v);
  }

  if (r_normal) {
    r_normal[0] = ocean_split_sample_bilerp(level.normal_x, level.size_x, level.size_y, u, v);
    r_normal[1] = ocean_split_sample_bilerp(level.normal_y, level.size_x, level.size_y, u, v);
    r_normal[2] = ocean_split_sample_bilerp(level.normal_z, level.size_x, level.size_y, u, v);
    if (normalize_v3(r_normal) == 0.0f) {
      r_normal[0] = 0.0f;
      r_normal[1] = 1.0f;
      r_normal[2] = 0.0f;
    }
  }

  return true;
}

bool BKE_ocean_split_runtime_level_get(const Ocean *oc,
                                       const int level_index,
                                       OceanSplitRuntimeLevel *r_level)
{
  if (!oc) {
    return false;
  }

  BLI_rw_mutex_lock(const_cast<ThreadRWMutex *>(&oc->oceanmutex), THREAD_LOCK_READ);
  const bool success = ocean_split_runtime_level_get_locked(oc, level_index, r_level);
  BLI_rw_mutex_unlock(const_cast<ThreadRWMutex *>(&oc->oceanmutex));
  return success;
}

bool BKE_ocean_split_runtime_sample_level(const Ocean *oc,
                                          const int level_index,
                                          const float u,
                                          const float v,
                                          float *r_displacement,
                                          float *r_normal)
{
  if (!oc) {
    return false;
  }

  BLI_rw_mutex_lock(const_cast<ThreadRWMutex *>(&oc->oceanmutex), THREAD_LOCK_READ);
  const bool success = ocean_split_runtime_sample_level_locked(
      oc, level_index, u, v, r_displacement, r_normal);
  BLI_rw_mutex_unlock(const_cast<ThreadRWMutex *>(&oc->oceanmutex));
  return success;
}

bool BKE_ocean_split_runtime_read_begin(const Ocean *oc, OceanSplitRuntimeReadScope *r_scope)
{
  if (!oc || !r_scope) {
    return false;
  }

  BLI_rw_mutex_lock(const_cast<ThreadRWMutex *>(&oc->oceanmutex), THREAD_LOCK_READ);
  r_scope->ocean = oc;
  return true;
}

void BKE_ocean_split_runtime_read_end(OceanSplitRuntimeReadScope *scope)
{
  if (!scope || !scope->ocean) {
    return;
  }

  BLI_rw_mutex_unlock(const_cast<ThreadRWMutex *>(&scope->ocean->oceanmutex));
  scope->ocean = nullptr;
}

bool BKE_ocean_split_runtime_level_get_in_scope(const OceanSplitRuntimeReadScope *scope,
                                                const int level_index,
                                                OceanSplitRuntimeLevel *r_level)
{
  return scope ? ocean_split_runtime_level_get_locked(scope->ocean, level_index, r_level) : false;
}

bool BKE_ocean_split_runtime_sample_level_in_scope(const OceanSplitRuntimeReadScope *scope,
                                                   const int level_index,
                                                   const float u,
                                                   const float v,
                                                   float *r_displacement,
                                                   float *r_normal)
{
  return scope ?
             ocean_split_runtime_sample_level_locked(
                 scope->ocean, level_index, u, v, r_displacement, r_normal) :
             false;
}

bool BKE_ocean_split_runtime_level_normal_data_get(const Ocean *oc,
                                                   const int level_index,
                                                   float *r_normal_data,
                                                   const int normal_data_len)
{
  if (!oc || !r_normal_data || !oc->_split_levels || level_index < 0 ||
      level_index >= oc->_split_levels_num)
  {
    return false;
  }

  BLI_rw_mutex_lock(const_cast<ThreadRWMutex *>(&oc->oceanmutex), THREAD_LOCK_READ);

  const OceanSplitLevel &level = oc->_split_levels[level_index];
  const size_t pixel_count = size_t(level.size_x) * size_t(level.size_y);
  const size_t expected_len = pixel_count * 4;
  if (level.size_x <= 0 || level.size_y <= 0 || normal_data_len < 0 ||
      size_t(normal_data_len) < expected_len)
  {
    BLI_rw_mutex_unlock(const_cast<ThreadRWMutex *>(&oc->oceanmutex));
    return false;
  }

  for (size_t index = 0; index < pixel_count; index++) {
    r_normal_data[index * 4 + 0] = level.normal_x[index];
    r_normal_data[index * 4 + 1] = level.normal_y[index];
    r_normal_data[index * 4 + 2] = level.normal_z[index];
    r_normal_data[index * 4 + 3] = 1.0f;
  }

  BLI_rw_mutex_unlock(const_cast<ThreadRWMutex *>(&oc->oceanmutex));
  return true;
}

void BKE_ocean_eval_ij(Ocean *oc, OceanResult *ocr, int i, int j)
{
  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);

  i = abs(i) % oc->_M;
  j = abs(j) % oc->_N;

  ocr->disp[1] = oc->_do_disp_y ? float(oc->_disp_y[i * oc->_N + j]) : 0.0f;

  if (oc->_do_chop) {
    ocr->disp[0] = oc->_disp_x[i * oc->_N + j];
    ocr->disp[2] = oc->_disp_z[i * oc->_N + j];
  }
  else {
    ocr->disp[0] = 0.0f;
    ocr->disp[2] = 0.0f;
  }

  if (oc->_do_normals) {
    ocr->normal[0] = oc->_N_x[i * oc->_N + j];
    ocr->normal[1] = oc->_N_y /* oc->_N_y[i * oc->_N + j] (MEM01) */;
    ocr->normal[2] = oc->_N_z[i * oc->_N + j];

    normalize_v3(ocr->normal);
  }

  if (oc->_do_jacobian) {
    compute_eigenstuff(
        ocr, oc->_Jxx[i * oc->_N + j], oc->_Jzz[i * oc->_N + j], oc->_Jxz[i * oc->_N + j]);
  }

  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

struct OceanSimulateData {
  Ocean *o;
  float t;
  float scale;
  float chop_amount;
};

static void ocean_compute_htilda(void *__restrict userdata,
                                 const int i,
                                 const TaskParallelTLS *__restrict /*tls*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(userdata);
  const Ocean *o = osd->o;
  const float scale = osd->scale;
  const float t = osd->t;

  int j;

  /* Note the <= _N/2 here, see the FFTW documentation
   * about the mechanics of the complex->real fft storage. */
  for (j = 0; j <= o->_N / 2; j++) {
    fftw_complex exp_param1;
    fftw_complex exp_param2;
    fftw_complex conj_param;

    init_complex(exp_param1, 0.0, omega(o->_k[i * (1 + o->_N / 2) + j], o->_depth) * t);
    init_complex(exp_param2, 0.0, -omega(o->_k[i * (1 + o->_N / 2) + j], o->_depth) * t);
    exp_complex(exp_param1, exp_param1);
    exp_complex(exp_param2, exp_param2);
    conj_complex(conj_param, o->_h0_minus[i * o->_N + j]);

    mul_complex_c(exp_param1, o->_h0[i * o->_N + j], exp_param1);
    mul_complex_c(exp_param2, conj_param, exp_param2);

    add_comlex_c(o->_htilda[i * (1 + o->_N / 2) + j], exp_param1, exp_param2);
    mul_complex_f(o->_fft_in[i * (1 + o->_N / 2) + j], o->_htilda[i * (1 + o->_N / 2) + j], scale);
  }
}

static void ocean_compute_displacement_y(TaskPool *__restrict pool, void * /*taskdata*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(BLI_task_pool_user_data(pool));
  const Ocean *o = osd->o;

  fftw_execute(o->_disp_y_plan);
}

static void ocean_compute_displacement_x(TaskPool *__restrict pool, void * /*taskdata*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(BLI_task_pool_user_data(pool));
  const Ocean *o = osd->o;
  const float scale = osd->scale;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;
      fftw_complex minus_i;

      init_complex(minus_i, 0.0, -1.0);
      init_complex(mul_param, -scale, 0);
      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, minus_i);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kx[i] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_x[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_disp_x_plan);
}

static void ocean_compute_displacement_z(TaskPool *__restrict pool, void * /*taskdata*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(BLI_task_pool_user_data(pool));
  const Ocean *o = osd->o;
  const float scale = osd->scale;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;
      fftw_complex minus_i;

      init_complex(minus_i, 0.0, -1.0);
      init_complex(mul_param, -scale, 0);
      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, minus_i);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kz[j] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_z[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_disp_z_plan);
}

static void ocean_compute_jacobian_jxx(TaskPool *__restrict pool, void * /*taskdata*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(BLI_task_pool_user_data(pool));
  const Ocean *o = osd->o;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      // init_complex(mul_param, -scale, 0);
      init_complex(mul_param, -1, 0);

      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kx[i] * o->_kx[i] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_jxx[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_Jxx_plan);

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j < o->_N; j++) {
      o->_Jxx[i * o->_N + j] += 1.0;
    }
  }
}

static void ocean_compute_jacobian_jzz(TaskPool *__restrict pool, void * /*taskdata*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(BLI_task_pool_user_data(pool));
  const Ocean *o = osd->o;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      // init_complex(mul_param, -scale, 0);
      init_complex(mul_param, -1, 0);

      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kz[j] * o->_kz[j] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_jzz[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_Jzz_plan);

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j < o->_N; j++) {
      o->_Jzz[i * o->_N + j] += 1.0;
    }
  }
}

static void ocean_compute_jacobian_jxz(TaskPool *__restrict pool, void * /*taskdata*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(BLI_task_pool_user_data(pool));
  const Ocean *o = osd->o;
  const float chop_amount = osd->chop_amount;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      // init_complex(mul_param, -scale, 0);
      init_complex(mul_param, -1, 0);

      mul_complex_f(mul_param, mul_param, chop_amount);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param,
                    mul_param,
                    ((o->_k[i * (1 + o->_N / 2) + j] == 0.0f) ?
                         0.0f :
                         o->_kx[i] * o->_kz[j] / o->_k[i * (1 + o->_N / 2) + j]));
      init_complex(o->_fft_in_jxz[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_Jxz_plan);
}

static void ocean_compute_normal_x(TaskPool *__restrict pool, void * /*taskdata*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(BLI_task_pool_user_data(pool));
  const Ocean *o = osd->o;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      init_complex(mul_param, 0.0, -1.0);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param, mul_param, o->_kx[i]);
      init_complex(o->_fft_in_nx[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_N_x_plan);
}

static void ocean_compute_normal_z(TaskPool *__restrict pool, void * /*taskdata*/)
{
  OceanSimulateData *osd = static_cast<OceanSimulateData *>(BLI_task_pool_user_data(pool));
  const Ocean *o = osd->o;
  int i, j;

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      fftw_complex mul_param;

      init_complex(mul_param, 0.0, -1.0);
      mul_complex_c(mul_param, mul_param, o->_htilda[i * (1 + o->_N / 2) + j]);
      mul_complex_f(mul_param, mul_param, o->_kz[i]);
      init_complex(o->_fft_in_nz[i * (1 + o->_N / 2) + j], real_c(mul_param), image_c(mul_param));
    }
  }
  fftw_execute(o->_N_z_plan);
}

bool BKE_ocean_is_valid(const Ocean *o)
{
  return o->_k != nullptr;
}

void BKE_ocean_simulate(Ocean *o, float t, float scale, float chop_amount)
{
  TaskPool *pool;

  OceanSimulateData osd;

  scale *= o->normalize_factor;

  osd.o = o;
  osd.t = t;
  osd.scale = scale;
  osd.chop_amount = chop_amount;

  pool = BLI_task_pool_create(&osd, TASK_PRIORITY_HIGH);

  BLI_rw_mutex_lock(&o->oceanmutex, THREAD_LOCK_WRITE);

  /* Note about multi-threading here: we have to run a first set of computations (htilda one)
   * before we can run all others, since they all depend on it.
   * So we make a first parallelized forloop run for htilda,
   * and then pack all other computations into a set of parallel tasks.
   * This is not optimal in all cases,
   * but remains reasonably simple and should be OK most of the time. */

  /* compute a new htilda */
  TaskParallelSettings settings;
  BLI_parallel_range_settings_defaults(&settings);
  settings.use_threading = (o->_M > 16);
  BLI_task_parallel_range(0, o->_M, &osd, ocean_compute_htilda, &settings);

  if (o->_do_disp_y) {
    BLI_task_pool_push(pool, ocean_compute_displacement_y, nullptr, false, nullptr);
  }

  if (o->_do_chop) {
    BLI_task_pool_push(pool, ocean_compute_displacement_x, nullptr, false, nullptr);
    BLI_task_pool_push(pool, ocean_compute_displacement_z, nullptr, false, nullptr);
  }

  if (o->_do_jacobian) {
    BLI_task_pool_push(pool, ocean_compute_jacobian_jxx, nullptr, false, nullptr);
    BLI_task_pool_push(pool, ocean_compute_jacobian_jzz, nullptr, false, nullptr);
    BLI_task_pool_push(pool, ocean_compute_jacobian_jxz, nullptr, false, nullptr);
  }

  if (o->_do_normals) {
    BLI_task_pool_push(pool, ocean_compute_normal_x, nullptr, false, nullptr);
    BLI_task_pool_push(pool, ocean_compute_normal_z, nullptr, false, nullptr);
    o->_N_y = 1.0f / scale;
  }

  BLI_task_pool_work_and_wait(pool);

  if (o->_do_split) {
    ocean_split_build_spectral_pyramid(o, scale, chop_amount);
  }

  BLI_rw_mutex_unlock(&o->oceanmutex);

  BLI_task_pool_free(pool);
}

static void set_height_normalize_factor(Ocean *oc)
{
  float res = 1.0;
  float max_h = 0.0;

  int i, j;

  if (!oc->_do_disp_y) {
    return;
  }

  oc->normalize_factor = 1.0;

  BKE_ocean_simulate(oc, 0.0, 1.0, 0);

  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_READ);

  for (i = 0; i < oc->_M; i++) {
    for (j = 0; j < oc->_N; j++) {
      max_h = std::max<double>(max_h, fabs(oc->_disp_y[i * oc->_N + j]));
    }
  }

  BLI_rw_mutex_unlock(&oc->oceanmutex);

  if (max_h == 0.0f) {
    max_h = 0.00001f; /* just in case ... */
  }

  res = 1.0f / (max_h);

  oc->normalize_factor = res;
}

Ocean *BKE_ocean_add()
{
  Ocean *oc = MEM_new_zeroed<Ocean>("ocean sim data");

  BLI_rw_mutex_init(&oc->oceanmutex);
  oc->_do_split = false;
  oc->_split_levels_num = 0;
  oc->_split_levels = nullptr;
  oc->_split_runtime_revision = 0;

  return oc;
}

bool BKE_ocean_ensure(OceanModifierData *omd, const int resolution)
{
  if (omd->ocean) {
    /* Check that the ocean has the same resolution than we want now. */
    if (omd->ocean->_M == resolution * resolution) {
      return false;
    }

    BKE_ocean_free(omd->ocean);
  }

  omd->ocean = BKE_ocean_add();
  BKE_ocean_init_from_modifier(omd->ocean, omd, resolution);
  return true;
}

bool BKE_ocean_init_from_modifier(Ocean *ocean, OceanModifierData const *omd, const int resolution)
{
  short do_heightfield, do_chop, do_normals, do_jacobian, do_spray;
  const bool do_split = (omd->flag & MOD_OCEAN_USE_CAMERA_LOD) != 0 &&
                        omd->geometry_mode == MOD_OCEAN_GEOM_GENERATE;

  do_heightfield = true;
  do_chop = (omd->chop_amount > 0);
  do_normals = do_split || (omd->flag & MOD_OCEAN_GENERATE_NORMALS);
  do_jacobian = (omd->flag & MOD_OCEAN_GENERATE_FOAM);
  do_spray = do_jacobian && (omd->flag & MOD_OCEAN_GENERATE_SPRAY);

  BKE_ocean_free_data(ocean);

  const bool initialized = BKE_ocean_init(ocean,
                                          resolution * resolution,
                                          resolution * resolution,
                                          omd->spatial_size,
                                          omd->spatial_size,
                                          omd->wind_velocity,
                                          omd->smallest_wave,
                                          1.0,
                                          omd->wave_direction,
                                          omd->damp,
                                          omd->wave_alignment,
                                          omd->depth,
                                          omd->time,
                                          omd->spectrum,
                                          omd->fetch_jonswap,
                                          omd->sharpen_peak_jonswap,
                                          do_heightfield,
                                          do_chop,
                                          do_spray,
                                          do_normals,
                                          do_jacobian,
                                          omd->seed);
  if (initialized) {
    ocean->_do_split = do_split;
  }
  return initialized;
}

bool BKE_ocean_init(Ocean *o,
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
                    int seed)
{
  int i, j, ii;

  BLI_rw_mutex_lock(&o->oceanmutex, THREAD_LOCK_WRITE);

  o->_M = M;
  o->_N = N;
  o->_V = V;
  o->_l = l;
  o->_A = A;
  o->_w = w;
  o->_damp_reflections = 1.0f - damp;
  o->_wind_alignment = alignment * 10.0f;
  o->_depth = depth;
  o->_Lx = Lx;
  o->_Lz = Lz;
  o->_wx = cos(w);
  o->_wz = -sin(w);        /* wave direction */
  o->_L = V * V / GRAVITY; /* largest wave for a given velocity V */
  o->time = time;

  /* Spectrum to use. */
  o->_spectrum = spectrum;

  /* Common JONSWAP parameters. */
  o->_fetch_jonswap = fetch_jonswap;
  o->_sharpen_peak_jonswap = sharpen_peak_jonswap * 10.0f;

  /* NOTE: most modifiers don't account for failure to allocate.
   * In this case however a large resolution can easily perform large allocations that fail,
   * support early exiting in this case. */
  if ((o->_k = MEM_new_array_uninitialized<float>(size_t(M) * (1 + size_t(N) / 2), "ocean_k")) &&
      (o->_h0 = MEM_new_array_uninitialized<fftw_complex>(size_t(M) * size_t(N), "ocean_h0")) &&
      (o->_h0_minus = MEM_new_array_uninitialized<fftw_complex>(size_t(M) * size_t(N),
                                                                "ocean_h0_minus")) &&
      (o->_kx = MEM_new_array_uninitialized<float>(size_t(o->_M), "ocean_kx")) &&
      (o->_kz = MEM_new_array_uninitialized<float>(size_t(o->_N), "ocean_kz")))
  {
    /* Success. */
  }
  else {
    MEM_SAFE_DELETE(o->_k);
    MEM_SAFE_DELETE(o->_h0);
    MEM_SAFE_DELETE(o->_h0_minus);
    MEM_SAFE_DELETE(o->_kx);
    MEM_SAFE_DELETE(o->_kz);

    BLI_rw_mutex_unlock(&o->oceanmutex);
    return false;
  }

  o->_do_disp_y = do_height_field;
  o->_do_normals = do_normals;
  o->_do_spray = do_spray;
  o->_do_chop = do_chop;
  o->_do_jacobian = do_jacobian;

  /* make this robust in the face of erroneous usage */
  if (o->_Lx == 0.0f) {
    o->_Lx = 0.001f;
  }

  if (o->_Lz == 0.0f) {
    o->_Lz = 0.001f;
  }

  /* The +VE components and DC. */
  for (i = 0; i <= o->_M / 2; i++) {
    o->_kx[i] = 2.0f * float(M_PI) * i / o->_Lx;
  }

  /* The -VE components. */
  for (i = o->_M - 1, ii = 0; i > o->_M / 2; i--, ii++) {
    o->_kx[i] = -2.0f * float(M_PI) * ii / o->_Lx;
  }

  /* The +VE components and DC. */
  for (i = 0; i <= o->_N / 2; i++) {
    o->_kz[i] = 2.0f * float(M_PI) * i / o->_Lz;
  }

  /* The -VE components. */
  for (i = o->_N - 1, ii = 0; i > o->_N / 2; i--, ii++) {
    o->_kz[i] = -2.0f * float(M_PI) * ii / o->_Lz;
  }

  /* pre-calculate the k matrix */
  for (i = 0; i < o->_M; i++) {
    for (j = 0; j <= o->_N / 2; j++) {
      o->_k[size_t(i) * (1 + o->_N / 2) + j] = sqrt(o->_kx[i] * o->_kx[i] + o->_kz[j] * o->_kz[j]);
    }
  }

  RNG *rng = BLI_rng_new(seed);

  for (i = 0; i < o->_M; i++) {
    for (j = 0; j < o->_N; j++) {
      /* This ensures we get a value tied to the surface location, avoiding dramatic surface
       * change with changing resolution.
       * Explicitly cast to signed int first to ensure consistent behavior on all processors,
       * since behavior of `float` to `uint` cast is undefined in C. */
      const int hash_x = o->_kx[i] * 360.0f;
      const int hash_z = o->_kz[j] * 360.0f;
      int new_seed = seed + BLI_hash_int_2d(hash_x, hash_z);

      BLI_rng_seed(rng, new_seed);
      float r1 = gaussRand(rng);
      float r2 = gaussRand(rng);

      fftw_complex r1r2;
      init_complex(r1r2, r1, r2);
      switch (o->_spectrum) {
        case MOD_OCEAN_SPECTRUM_JONSWAP:
          mul_complex_f(o->_h0[i * o->_N + j],
                        r1r2,
                        sqrt(BLI_ocean_spectrum_jonswap(o, o->_kx[i], o->_kz[j]) / 2.0f));
          mul_complex_f(o->_h0_minus[i * o->_N + j],
                        r1r2,
                        sqrt(BLI_ocean_spectrum_jonswap(o, -o->_kx[i], -o->_kz[j]) / 2.0f));
          break;
        case MOD_OCEAN_SPECTRUM_TEXEL_MARSEN_ARSLOE:
          mul_complex_f(
              o->_h0[i * o->_N + j],
              r1r2,
              sqrt(BLI_ocean_spectrum_texelmarsenarsloe(o, o->_kx[i], o->_kz[j]) / 2.0f));
          mul_complex_f(
              o->_h0_minus[i * o->_N + j],
              r1r2,
              sqrt(BLI_ocean_spectrum_texelmarsenarsloe(o, -o->_kx[i], -o->_kz[j]) / 2.0f));
          break;
        case MOD_OCEAN_SPECTRUM_PIERSON_MOSKOWITZ:
          mul_complex_f(o->_h0[i * o->_N + j],
                        r1r2,
                        sqrt(BLI_ocean_spectrum_piersonmoskowitz(o, o->_kx[i], o->_kz[j]) / 2.0f));
          mul_complex_f(
              o->_h0_minus[i * o->_N + j],
              r1r2,
              sqrt(BLI_ocean_spectrum_piersonmoskowitz(o, -o->_kx[i], -o->_kz[j]) / 2.0f));
          break;
        default:
          mul_complex_f(o->_h0[i * o->_N + j], r1r2, sqrt(Ph(o, o->_kx[i], o->_kz[j]) / 2.0f));
          mul_complex_f(
              o->_h0_minus[i * o->_N + j], r1r2, sqrt(Ph(o, -o->_kx[i], -o->_kz[j]) / 2.0f));
          break;
      }
    }
  }

  o->_fft_in = MEM_new_array_uninitialized<fftw_complex>(size_t(o->_M) * (1 + size_t(o->_N) / 2),
                                                         "ocean_fft_in");
  o->_htilda = MEM_new_array_uninitialized<fftw_complex>(size_t(o->_M) * (1 + size_t(o->_N) / 2),
                                                         "ocean_htilda");

  BLI_thread_lock(LOCK_FFTW);

  if (o->_do_disp_y) {
    o->_disp_y = MEM_new_array_uninitialized<double>(size_t(o->_M) * size_t(o->_N),
                                                     "ocean_disp_y");
    o->_disp_y_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in, o->_disp_y, FFTW_ESTIMATE);
  }

  if (o->_do_normals) {
    o->_fft_in_nx = MEM_new_array_uninitialized<fftw_complex>(
        size_t(o->_M) * (1 + size_t(o->_N) / 2), "ocean_fft_in_nx");
    o->_fft_in_nz = MEM_new_array_uninitialized<fftw_complex>(
        size_t(o->_M) * (1 + size_t(o->_N) / 2), "ocean_fft_in_nz");

    o->_N_x = MEM_new_array_uninitialized<double>(size_t(o->_M) * size_t(o->_N), "ocean_N_x");
    // o->_N_y = (float *) fftwf_malloc(o->_M * o->_N * sizeof(float)); /* (MEM01) */
    o->_N_z = MEM_new_array_uninitialized<double>(size_t(o->_M) * size_t(o->_N), "ocean_N_z");

    o->_N_x_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_nx, o->_N_x, FFTW_ESTIMATE);
    o->_N_z_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_nz, o->_N_z, FFTW_ESTIMATE);
  }

  if (o->_do_chop) {
    o->_fft_in_x = MEM_new_array_uninitialized<fftw_complex>(
        size_t(o->_M) * (1 + size_t(o->_N) / 2), "ocean_fft_in_x");
    o->_fft_in_z = MEM_new_array_uninitialized<fftw_complex>(
        size_t(o->_M) * (1 + size_t(o->_N) / 2), "ocean_fft_in_z");

    o->_disp_x = MEM_new_array_uninitialized<double>(size_t(o->_M) * size_t(o->_N),
                                                     "ocean_disp_x");
    o->_disp_z = MEM_new_array_uninitialized<double>(size_t(o->_M) * size_t(o->_N),
                                                     "ocean_disp_z");

    o->_disp_x_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_x, o->_disp_x, FFTW_ESTIMATE);
    o->_disp_z_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_z, o->_disp_z, FFTW_ESTIMATE);
  }
  if (o->_do_jacobian) {
    o->_fft_in_jxx = MEM_new_array_uninitialized<fftw_complex>(
        size_t(o->_M) * (1 + size_t(o->_N) / 2), "ocean_fft_in_jxx");
    o->_fft_in_jzz = MEM_new_array_uninitialized<fftw_complex>(
        size_t(o->_M) * (1 + size_t(o->_N) / 2), "ocean_fft_in_jzz");
    o->_fft_in_jxz = MEM_new_array_uninitialized<fftw_complex>(
        size_t(o->_M) * (1 + size_t(o->_N) / 2), "ocean_fft_in_jxz");

    o->_Jxx = MEM_new_array_uninitialized<double>(size_t(o->_M) * size_t(o->_N), "ocean_Jxx");
    o->_Jzz = MEM_new_array_uninitialized<double>(size_t(o->_M) * size_t(o->_N), "ocean_Jzz");
    o->_Jxz = MEM_new_array_uninitialized<double>(size_t(o->_M) * size_t(o->_N), "ocean_Jxz");

    o->_Jxx_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_jxx, o->_Jxx, FFTW_ESTIMATE);
    o->_Jzz_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_jzz, o->_Jzz, FFTW_ESTIMATE);
    o->_Jxz_plan = fftw_plan_dft_c2r_2d(o->_M, o->_N, o->_fft_in_jxz, o->_Jxz, FFTW_ESTIMATE);
  }

  BLI_thread_unlock(LOCK_FFTW);

  BLI_rw_mutex_unlock(&o->oceanmutex);

  set_height_normalize_factor(o);

  BLI_rng_free(rng);

  return true;
}

void BKE_ocean_free_data(Ocean *oc)
{
  if (!oc) {
    return;
  }

  BLI_rw_mutex_lock(&oc->oceanmutex, THREAD_LOCK_WRITE);

  BLI_thread_lock(LOCK_FFTW);

  if (oc->_do_disp_y) {
    fftw_destroy_plan(oc->_disp_y_plan);
    MEM_delete(oc->_disp_y);
  }

  if (oc->_do_normals) {
    MEM_delete(oc->_fft_in_nx);
    MEM_delete(oc->_fft_in_nz);
    fftw_destroy_plan(oc->_N_x_plan);
    fftw_destroy_plan(oc->_N_z_plan);
    MEM_delete(oc->_N_x);
    // fftwf_free(oc->_N_y); /* (MEM01) */
    MEM_delete(oc->_N_z);
  }

  if (oc->_do_chop) {
    MEM_delete(oc->_fft_in_x);
    MEM_delete(oc->_fft_in_z);
    fftw_destroy_plan(oc->_disp_x_plan);
    fftw_destroy_plan(oc->_disp_z_plan);
    MEM_delete(oc->_disp_x);
    MEM_delete(oc->_disp_z);
  }

  if (oc->_do_jacobian) {
    MEM_delete(oc->_fft_in_jxx);
    MEM_delete(oc->_fft_in_jzz);
    MEM_delete(oc->_fft_in_jxz);
    fftw_destroy_plan(oc->_Jxx_plan);
    fftw_destroy_plan(oc->_Jzz_plan);
    fftw_destroy_plan(oc->_Jxz_plan);
    MEM_delete(oc->_Jxx);
    MEM_delete(oc->_Jzz);
    MEM_delete(oc->_Jxz);
  }

  BLI_thread_unlock(LOCK_FFTW);

  if (oc->_fft_in) {
    MEM_delete(oc->_fft_in);
  }

  /* check that ocean data has been initialized */
  if (oc->_htilda) {
    MEM_delete(oc->_htilda);
    MEM_delete(oc->_k);
    MEM_delete(oc->_h0);
    MEM_delete(oc->_h0_minus);
    MEM_delete(oc->_kx);
    MEM_delete(oc->_kz);
  }

  ocean_free_split_data(oc);
  oc->_do_split = false;

  BLI_rw_mutex_unlock(&oc->oceanmutex);
}

void BKE_ocean_free(Ocean *oc)
{
  if (!oc) {
    return;
  }

  BKE_ocean_free_data(oc);
  BLI_rw_mutex_end(&oc->oceanmutex);

  MEM_delete(oc);
}

#  undef GRAVITY

/* ********* Baking/Caching ********* */

#  define CACHE_TYPE_DISPLACE 1
#  define CACHE_TYPE_FOAM 2
#  define CACHE_TYPE_NORMAL 3
#  define CACHE_TYPE_SPRAY 4
#  define CACHE_TYPE_SPRAY_INVERSE 5

static void cache_filepath(
    char *filepath, const char *dirname, const char *relbase, int frame, int type)
{
  char cachepath[FILE_MAX];
  const char *filename;

  switch (type) {
    case CACHE_TYPE_FOAM:
      filename = "foam_";
      break;
    case CACHE_TYPE_NORMAL:
      filename = "normal_";
      break;
    case CACHE_TYPE_SPRAY:
      filename = "spray_";
      break;
    case CACHE_TYPE_SPRAY_INVERSE:
      filename = "spray_inverse_";
      break;
    case CACHE_TYPE_DISPLACE:
    default:
      filename = "disp_";
      break;
  }

  BLI_path_join(cachepath, sizeof(cachepath), dirname, filename);

  const Vector<bke::path_templates::Error> errors = BKE_image_path_from_imtype(
      filepath, cachepath, relbase, nullptr, frame, R_IMF_IMTYPE_OPENEXR, true, true, "");
  BLI_assert_msg(errors.is_empty(),
                 "Path parsing errors should only occur when a variable map is provided.");
  UNUSED_VARS_NDEBUG(errors);
}

/* silly functions but useful to inline when the args do a lot of indirections */
MINLINE void rgb_to_rgba_unit_alpha(float r_rgba[4], const float rgb[3])
{
  r_rgba[0] = rgb[0];
  r_rgba[1] = rgb[1];
  r_rgba[2] = rgb[2];
  r_rgba[3] = 1.0f;
}
MINLINE void value_to_rgba_unit_alpha(float r_rgba[4], const float value)
{
  r_rgba[0] = value;
  r_rgba[1] = value;
  r_rgba[2] = value;
  r_rgba[3] = 1.0f;
}

void BKE_ocean_free_cache(OceanCache *och)
{
  int i, f = 0;

  if (!och) {
    return;
  }

  if (och->ibufs_disp) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_disp[f]) {
        IMB_freeImBuf(och->ibufs_disp[f]);
      }
    }
    MEM_delete(och->ibufs_disp);
  }

  if (och->ibufs_foam) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_foam[f]) {
        IMB_freeImBuf(och->ibufs_foam[f]);
      }
    }
    MEM_delete(och->ibufs_foam);
  }

  if (och->ibufs_spray) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_spray[f]) {
        IMB_freeImBuf(och->ibufs_spray[f]);
      }
    }
    MEM_delete(och->ibufs_spray);
  }

  if (och->ibufs_spray_inverse) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_spray_inverse[f]) {
        IMB_freeImBuf(och->ibufs_spray_inverse[f]);
      }
    }
    MEM_delete(och->ibufs_spray_inverse);
  }

  if (och->ibufs_norm) {
    for (i = och->start, f = 0; i <= och->end; i++, f++) {
      if (och->ibufs_norm[f]) {
        IMB_freeImBuf(och->ibufs_norm[f]);
      }
    }
    MEM_delete(och->ibufs_norm);
  }

  if (och->time) {
    MEM_delete(och->time);
  }
  MEM_delete(och);
}

void BKE_ocean_cache_eval_uv(OceanCache *och, OceanResult *ocr, int f, float u, float v)
{
  int res_x = och->resolution_x;
  int res_y = och->resolution_y;
  float result[4];

  u = fmod(u, 1.0);
  v = fmod(v, 1.0);

  if (u < 0) {
    u += 1.0f;
  }
  if (v < 0) {
    v += 1.0f;
  }

  if (och->ibufs_disp[f]) {
    ibuf_sample(och->ibufs_disp[f], u, v, (1.0f / float(res_x)), (1.0f / float(res_y)), result);
    copy_v3_v3(ocr->disp, result);
  }

  if (och->ibufs_foam[f]) {
    ibuf_sample(och->ibufs_foam[f], u, v, (1.0f / float(res_x)), (1.0f / float(res_y)), result);
    ocr->foam = result[0];
  }

  if (och->ibufs_spray[f]) {
    ibuf_sample(och->ibufs_spray[f], u, v, (1.0f / float(res_x)), (1.0f / float(res_y)), result);
    copy_v3_v3(ocr->Eplus, result);
  }

  if (och->ibufs_spray_inverse[f]) {
    ibuf_sample(
        och->ibufs_spray_inverse[f], u, v, (1.0f / float(res_x)), (1.0f / float(res_y)), result);
    copy_v3_v3(ocr->Eminus, result);
  }

  if (och->ibufs_norm[f]) {
    ibuf_sample(och->ibufs_norm[f], u, v, (1.0f / float(res_x)), (1.0f / float(res_y)), result);
    copy_v3_v3(ocr->normal, result);
  }
}

void BKE_ocean_cache_eval_ij(OceanCache *och, OceanResult *ocr, int f, int i, int j)
{
  const int res_x = och->resolution_x;
  const int res_y = och->resolution_y;

  if (i < 0) {
    i = -i;
  }
  if (j < 0) {
    j = -j;
  }

  i = i % res_x;
  j = j % res_y;

  if (och->ibufs_disp[f]) {
    copy_v3_v3(ocr->disp, &och->ibufs_disp[f]->float_data()[4 * (res_x * j + i)]);
  }

  if (och->ibufs_foam[f]) {
    ocr->foam = och->ibufs_foam[f]->float_data()[4 * (res_x * j + i)];
  }

  if (och->ibufs_spray[f]) {
    copy_v3_v3(ocr->Eplus, &och->ibufs_spray[f]->float_data()[4 * (res_x * j + i)]);
  }

  if (och->ibufs_spray_inverse[f]) {
    copy_v3_v3(ocr->Eminus, &och->ibufs_spray_inverse[f]->float_data()[4 * (res_x * j + i)]);
  }

  if (och->ibufs_norm[f]) {
    copy_v3_v3(ocr->normal, &och->ibufs_norm[f]->float_data()[4 * (res_x * j + i)]);
  }
}

OceanCache *BKE_ocean_init_cache(const char *bakepath,
                                 const char *relbase,
                                 int start,
                                 int end,
                                 float wave_scale,
                                 float chop_amount,
                                 float foam_coverage,
                                 float foam_fade,
                                 int resolution)
{
  OceanCache *och = MEM_new_zeroed<OceanCache>("ocean cache data");

  och->bakepath = bakepath;
  och->relbase = relbase;

  och->start = start;
  och->end = end;
  och->duration = (end - start) + 1;
  och->wave_scale = wave_scale;
  och->chop_amount = chop_amount;
  och->foam_coverage = foam_coverage;
  och->foam_fade = foam_fade;
  och->resolution_x = resolution * resolution;
  och->resolution_y = resolution * resolution;

  och->ibufs_disp = MEM_new_array_zeroed<ImBuf *>(och->duration,
                                                  "displacement imbuf pointer array");
  och->ibufs_foam = MEM_new_array_zeroed<ImBuf *>(och->duration, "foam imbuf pointer array");
  och->ibufs_spray = MEM_new_array_zeroed<ImBuf *>(och->duration, "spray imbuf pointer array");
  och->ibufs_spray_inverse = MEM_new_array_zeroed<ImBuf *>(och->duration,
                                                           "spray_inverse imbuf pointer array");
  och->ibufs_norm = MEM_new_array_zeroed<ImBuf *>(och->duration, "normal imbuf pointer array");

  och->time = nullptr;

  return och;
}

void BKE_ocean_simulate_cache(OceanCache *och, int frame)
{
  char filepath[FILE_MAX];

  /* ibufs array is zero based, but filenames are based on frame numbers */
  /* still need to clamp frame numbers to valid range of images on disk though */
  CLAMP(frame, och->start, och->end);
  const int f = frame - och->start; /* shift to 0 based */

  /* if image is already loaded in mem, return */
  if (och->ibufs_disp[f] != nullptr) {
    return;
  }

  /* Use default color spaces since we know for sure cache
   * files were saved with default settings too. */

  cache_filepath(filepath, och->bakepath, och->relbase, frame, CACHE_TYPE_DISPLACE);
  och->ibufs_disp[f] = IMB_load_image_from_filepath(filepath, 0);

  cache_filepath(filepath, och->bakepath, och->relbase, frame, CACHE_TYPE_FOAM);
  och->ibufs_foam[f] = IMB_load_image_from_filepath(filepath, 0);

  cache_filepath(filepath, och->bakepath, och->relbase, frame, CACHE_TYPE_SPRAY);
  och->ibufs_spray[f] = IMB_load_image_from_filepath(filepath, 0);

  cache_filepath(filepath, och->bakepath, och->relbase, frame, CACHE_TYPE_SPRAY_INVERSE);
  och->ibufs_spray_inverse[f] = IMB_load_image_from_filepath(filepath, 0);

  cache_filepath(filepath, och->bakepath, och->relbase, frame, CACHE_TYPE_NORMAL);
  och->ibufs_norm[f] = IMB_load_image_from_filepath(filepath, 0);
}

void BKE_ocean_bake(Ocean *o,
                    OceanCache *och,
                    void (*update_cb)(void *, float progress, int *cancel),
                    void *update_cb_data)
{
  /* NOTE(@ideasman42): some of these values remain uninitialized unless certain options
   * are enabled, take care that #BKE_ocean_eval_ij() initializes a member before use. */
  OceanResult ocr;

  ImageFormatData imf = {};

  int f, i = 0, x, y, cancel = 0;
  float progress;

  ImBuf *ibuf_foam, *ibuf_disp, *ibuf_normal, *ibuf_spray, *ibuf_spray_inverse;
  float *prev_foam;
  int res_x = och->resolution_x;
  int res_y = och->resolution_y;
  char filepath[FILE_MAX];
  // RNG *rng;

  if (!o) {
    return;
  }

  if (o->_do_jacobian) {
    prev_foam = MEM_new_array_zeroed<float>(size_t(res_x) * size_t(res_y),
                                            "previous frame foam bake data");
  }
  else {
    prev_foam = nullptr;
  }

  // rng = BLI_rng_new(0);

  /* setup image format */
  imf.imtype = R_IMF_IMTYPE_OPENEXR;
  imf.depth = R_IMF_CHAN_DEPTH_16;
  imf.exr_codec = R_IMF_EXR_CODEC_ZIP;

  for (f = och->start, i = 0; f <= och->end; f++, i++) {

    /* create a new imbuf to store image for this frame */
    ibuf_foam = IMB_allocImBuf(res_x, res_y, 32, IB_float_data);
    ibuf_disp = IMB_allocImBuf(res_x, res_y, 32, IB_float_data);
    ibuf_normal = IMB_allocImBuf(res_x, res_y, 32, IB_float_data);
    ibuf_spray = IMB_allocImBuf(res_x, res_y, 32, IB_float_data);
    ibuf_spray_inverse = IMB_allocImBuf(res_x, res_y, 32, IB_float_data);

    BKE_ocean_simulate(o, och->time[i], och->wave_scale, och->chop_amount);

    /* add new foam */
    float *ibuf_disp_data = ibuf_disp->float_data_for_write();
    float *ibuf_foam_data = ibuf_foam->float_data_for_write();
    float *ibuf_spray_data = ibuf_spray->float_data_for_write();
    float *ibuf_spray_inverse_data = ibuf_spray_inverse->float_data_for_write();
    float *ibuf_normal_data = ibuf_normal->float_data_for_write();
    for (y = 0; y < res_y; y++) {
      for (x = 0; x < res_x; x++) {

        BKE_ocean_eval_ij(o, &ocr, x, y);

        /* add to the image */
        rgb_to_rgba_unit_alpha(&ibuf_disp_data[4 * (res_x * y + x)], ocr.disp);

        if (o->_do_jacobian) {
          /* TODO(@ideasman42): cleanup unused code. */

          float /* r, */ /* UNUSED */ pr = 0.0f, foam_result;
          float neg_disp, neg_eplus;

          ocr.foam = BKE_ocean_jminus_to_foam(ocr.Jminus, och->foam_coverage);

          /* accumulate previous value for this cell */
          if (i > 0) {
            pr = prev_foam[res_x * y + x];
          }

          // r = BLI_rng_get_float(rng); /* UNUSED */ /* randomly reduce foam */

          // pr = pr * och->foam_fade; /* overall fade */

          /* Remember ocean coord system is Y up!
           * break up the foam where height (Y) is low (wave valley),
           * and X and Z displacement is greatest. */

          neg_disp = ocr.disp[1] < 0.0f ? 1.0f + ocr.disp[1] : 1.0f;
          neg_disp = neg_disp < 0.0f ? 0.0f : neg_disp;

          /* foam, 'ocr.Eplus' only initialized with do_jacobian */
          neg_eplus = ocr.Eplus[2] < 0.0f ? 1.0f + ocr.Eplus[2] : 1.0f;
          neg_eplus = neg_eplus < 0.0f ? 0.0f : neg_eplus;

          if (pr < 1.0f) {
            pr *= pr;
          }

          pr *= och->foam_fade * (0.75f + neg_eplus * 0.25f);

          /* A full clamping should not be needed! */
          foam_result = min_ff(pr + ocr.foam, 1.0f);

          prev_foam[res_x * y + x] = foam_result;

          // foam_result = min_ff(foam_result, 1.0f);

          value_to_rgba_unit_alpha(&ibuf_foam_data[4 * (res_x * y + x)], foam_result);

          /* spray map baking */
          if (o->_do_spray) {
            rgb_to_rgba_unit_alpha(&ibuf_spray_data[4 * (res_x * y + x)], ocr.Eplus);
            rgb_to_rgba_unit_alpha(&ibuf_spray_inverse_data[4 * (res_x * y + x)], ocr.Eminus);
          }
        }

        if (o->_do_normals) {
          rgb_to_rgba_unit_alpha(&ibuf_normal_data[4 * (res_x * y + x)], ocr.normal);
        }
      }
    }

    /* write the images */
    cache_filepath(filepath, och->bakepath, och->relbase, f, CACHE_TYPE_DISPLACE);
    if (false == BKE_imbuf_write(ibuf_disp, filepath, &imf)) {
      printf("Cannot save Displacement File Output to %s\n", filepath);
    }

    if (o->_do_jacobian) {
      cache_filepath(filepath, och->bakepath, och->relbase, f, CACHE_TYPE_FOAM);
      if (false == BKE_imbuf_write(ibuf_foam, filepath, &imf)) {
        printf("Cannot save Foam File Output to %s\n", filepath);
      }

      if (o->_do_spray) {
        cache_filepath(filepath, och->bakepath, och->relbase, f, CACHE_TYPE_SPRAY);
        if (false == BKE_imbuf_write(ibuf_spray, filepath, &imf)) {
          printf("Cannot save Spray File Output to %s\n", filepath);
        }

        cache_filepath(filepath, och->bakepath, och->relbase, f, CACHE_TYPE_SPRAY_INVERSE);
        if (false == BKE_imbuf_write(ibuf_spray_inverse, filepath, &imf)) {
          printf("Cannot save Spray Inverse File Output to %s\n", filepath);
        }
      }
    }

    if (o->_do_normals) {
      cache_filepath(filepath, och->bakepath, och->relbase, f, CACHE_TYPE_NORMAL);
      if (false == BKE_imbuf_write(ibuf_normal, filepath, &imf)) {
        printf("Cannot save Normal File Output to %s\n", filepath);
      }
    }

    IMB_freeImBuf(ibuf_disp);
    IMB_freeImBuf(ibuf_foam);
    IMB_freeImBuf(ibuf_normal);
    IMB_freeImBuf(ibuf_spray);
    IMB_freeImBuf(ibuf_spray_inverse);

    progress = (f - och->start) / float(och->duration);

    update_cb(update_cb_data, progress, &cancel);

    if (cancel) {
      if (prev_foam) {
        MEM_delete(prev_foam);
      }
      // BLI_rng_free(rng);
      return;
    }
  }

  // BLI_rng_free(rng);
  if (prev_foam) {
    MEM_delete(prev_foam);
  }
  och->baked = 1;
}

#else /* WITH_OCEANSIM */

float BKE_ocean_jminus_to_foam(float /*jminus*/, float /*coverage*/)
{
  return 0.0f;
}

void BKE_ocean_eval_uv(Ocean * /*oc*/, OceanResult * /*ocr*/, float /*u*/, float /*v*/) {}

bool BKE_ocean_runtime_read_begin(const Ocean * /*oc*/, OceanRuntimeReadScope * /*r_scope*/)
{
  return false;
}

void BKE_ocean_runtime_read_end(OceanRuntimeReadScope * /*scope*/) {}

bool BKE_ocean_eval_uv_in_scope(const OceanRuntimeReadScope * /*scope*/,
                                OceanResult * /*ocr*/,
                                const float /*u*/,
                                const float /*v*/)
{
  return false;
}

/* use catmullrom interpolation rather than linear */
void BKE_ocean_eval_uv_catrom(Ocean * /*oc*/, OceanResult * /*ocr*/, float /*u*/, float /*v*/) {}

void BKE_ocean_eval_xz(Ocean * /*oc*/, OceanResult * /*ocr*/, float /*x*/, float /*z*/) {}

void BKE_ocean_eval_xz_catrom(Ocean * /*oc*/, OceanResult * /*ocr*/, float /*x*/, float /*z*/) {}

void BKE_ocean_eval_uv_split_support(Ocean * /*oc*/,
                                     OceanSplitResult *osr,
                                     float /*u*/,
                                     float /*v*/,
                                     const OceanSplitSupport *geometry_support,
                                     const OceanSplitSupport *camera_support)
{
  memset(osr, 0, sizeof(*osr));
  osr->geometry_normal[1] = 1.0f;
  osr->visible_normal[1] = 1.0f;

  if (geometry_support) {
    osr->geometry_wavelength = geometry_support->wavelength_major;
    osr->geometry_wavelength_xz[0] = geometry_support->wavelength_x;
    osr->geometry_wavelength_xz[1] = geometry_support->wavelength_z;
    copy_v3_v3(osr->geometry_support_covariance, geometry_support->covariance);
  }
  if (camera_support) {
    osr->camera_wavelength = camera_support->wavelength_major;
    osr->camera_wavelength_xz[0] = camera_support->wavelength_x;
    osr->camera_wavelength_xz[1] = camera_support->wavelength_z;
    copy_v3_v3(osr->camera_support_covariance, camera_support->covariance);
  }
}

void BKE_ocean_eval_uv_split(Ocean * /*oc*/,
                             OceanSplitResult *osr,
                             float /*u*/,
                             float /*v*/,
                             float geometry_wavelength,
                             float camera_wavelength)
{
  memset(osr, 0, sizeof(*osr));
  osr->geometry_normal[1] = 1.0f;
  osr->visible_normal[1] = 1.0f;
  osr->geometry_wavelength = geometry_wavelength;
  osr->camera_wavelength = camera_wavelength;
}

void BKE_ocean_eval_xz_split_support(Ocean * /*oc*/,
                                     OceanSplitResult *osr,
                                     float /*x*/,
                                     float /*z*/,
                                     const OceanSplitSupport *geometry_support,
                                     const OceanSplitSupport *camera_support)
{
  BKE_ocean_eval_uv_split_support(nullptr, osr, 0.0f, 0.0f, geometry_support, camera_support);
}

void BKE_ocean_eval_xz_split(Ocean * /*oc*/,
                             OceanSplitResult *osr,
                             float /*x*/,
                             float /*z*/,
                             float geometry_wavelength,
                             float camera_wavelength)
{
  BKE_ocean_eval_uv_split(nullptr, osr, 0.0f, 0.0f, geometry_wavelength, camera_wavelength);
}

int BKE_ocean_split_level_count_get(const Ocean * /*oc*/)
{
  return 0;
}

float BKE_ocean_split_min_wavelength_get(const Ocean * /*oc*/)
{
  return 0.0f;
}

uint64_t BKE_ocean_split_runtime_revision_get(const Ocean * /*oc*/)
{
  return 0;
}

bool BKE_ocean_split_runtime_level_get(const Ocean * /*oc*/,
                                       const int /*level_index*/,
                                       OceanSplitRuntimeLevel * /*r_level*/)
{
  return false;
}

bool BKE_ocean_split_runtime_sample_level(const Ocean * /*oc*/,
                                          const int /*level_index*/,
                                          const float /*u*/,
                                          const float /*v*/,
                                          float * /*r_displacement*/,
                                          float * /*r_normal*/)
{
  return false;
}

bool BKE_ocean_split_runtime_level_normal_data_get(const Ocean * /*oc*/,
                                                   const int /*level_index*/,
                                                   float * /*r_normal_data*/,
                                                   const int /*normal_data_len*/)
{
  return false;
}

void BKE_ocean_eval_ij(Ocean * /*oc*/, OceanResult * /*ocr*/, int /*i*/, int /*j*/) {}

void BKE_ocean_simulate(Ocean * /*o*/, float /*t*/, float /*scale*/, float /*chop_amount*/) {}

Ocean *BKE_ocean_add()
{
  Ocean *oc = MEM_new_zeroed<Ocean>("ocean sim data");

  return oc;
}

bool BKE_ocean_init(Ocean * /*o*/,
                    int /*M*/,
                    int /*N*/,
                    float /*Lx*/,
                    float /*Lz*/,
                    float /*V*/,
                    float /*l*/,
                    float /*A*/,
                    float /*w*/,
                    float /*damp*/,
                    float /*alignment*/,
                    float /*depth*/,
                    float /*time*/,
                    int /*spectrum*/,
                    float /*fetch_jonswap*/,
                    float /*sharpen_peak_jonswap*/,
                    short /*do_height_field*/,
                    short /*do_chop*/,
                    short /*do_spray*/,
                    short /*do_normals*/,
                    short /*do_jacobian*/,
                    int /*seed*/)
{
  return false;
}

void BKE_ocean_free_data(Ocean * /*oc*/) {}

void BKE_ocean_free(Ocean *oc)
{
  if (!oc) {
    return;
  }
  MEM_delete(oc);
}

/* ********* Baking/Caching ********* */

void BKE_ocean_free_cache(OceanCache *och)
{
  if (!och) {
    return;
  }

  MEM_delete(och);
}

void BKE_ocean_cache_eval_uv(
    OceanCache * /*och*/, OceanResult * /*ocr*/, int /*f*/, float /*u*/, float /*v*/)
{
}

void BKE_ocean_cache_eval_ij(
    OceanCache * /*och*/, OceanResult * /*ocr*/, int /*f*/, int /*i*/, int /*j*/)
{
}

OceanCache *BKE_ocean_init_cache(const char * /*bakepath*/,
                                 const char * /*relbase*/,
                                 int /*start*/,
                                 int /*end*/,
                                 float /*wave_scale*/,
                                 float /*chop_amount*/,
                                 float /*foam_coverage*/,
                                 float /*foam_fade*/,
                                 int /*resolution*/)
{
  OceanCache *och = MEM_new_zeroed<OceanCache>("ocean cache data");

  return och;
}

void BKE_ocean_simulate_cache(OceanCache * /*och*/, int /*frame*/) {}

void BKE_ocean_bake(Ocean * /*o*/,
                    OceanCache * /*och*/,
                    void (*update_cb)(void *, float progress, int *cancel),
                    void * /*update_cb_data*/)
{
  /* unused */
  (void)update_cb;
}

bool BKE_ocean_init_from_modifier(Ocean * /*ocean*/,
                                  OceanModifierData const * /*omd*/,
                                  int /*resolution*/)
{
  return true;
}

#endif /* WITH_OCEANSIM */

void BKE_ocean_free_modifier_cache(OceanModifierData *omd)
{
  BKE_ocean_free_cache(omd->oceancache);
  omd->oceancache = nullptr;
  omd->cached = false;
}

}  // namespace blender
