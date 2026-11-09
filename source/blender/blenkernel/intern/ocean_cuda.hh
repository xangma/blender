/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstddef>

namespace blender::ocean_cuda {

/* Independent of Blender's mesh and runtime layouts so the CUDA translation unit
 * can be compiled separately. The caller holds the Ocean read lock throughout. */
struct Input {
  int size_x;
  int size_y;
  bool chop;
  bool jacobian;
  float size_co_inv;
  float foam_coverage;
  bool inverse_spray;
  const double *disp_y;
  const double *disp_x;
  const double *disp_z;
  const double *jxx;
  const double *jzz;
  const double *jxz;
};

/* Returns false without changing host outputs on allocation/runtime failure. */
bool sample(const Input &input,
            size_t vertex_count,
            const float *reference_positions,
            float *positions,
            unsigned char *foam,
            float *compression,
            unsigned char *spray);

}  // namespace blender::ocean_cuda
