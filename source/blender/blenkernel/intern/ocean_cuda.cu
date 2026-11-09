/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "ocean_cuda.hh"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace blender::ocean_cuda {
namespace {

struct Buffer {
  void *data = nullptr;
  size_t capacity = 0;
  bool host = false;

  explicit Buffer(bool host = false) : host(host) {}
  ~Buffer()
  {
    if (data) {
      host ? cudaFreeHost(data) : cudaFree(data);
    }
  }
  bool reserve(size_t bytes)
  {
    if (bytes <= capacity) {
      return true;
    }
    if (data) {
      host ? cudaFreeHost(data) : cudaFree(data);
      data = nullptr;
      capacity = 0;
    }
    const cudaError_t error = host ? cudaMallocHost(&data, bytes) : cudaMalloc(&data, bytes);
    if (error != cudaSuccess) {
      return false;
    }
    capacity = bytes;
    return true;
  }
};

struct Scratch {
  Buffer fields[6];
  Buffer reference;
  Buffer positions;
  Buffer foam;
  Buffer compression;
  Buffer spray;
  Buffer host_positions{true};
  Buffer host_foam{true};
  Buffer host_compression{true};
  Buffer host_spray{true};
  cudaStream_t stream = nullptr;
  cudaEvent_t events[4]{};
  bool disabled = false;

  ~Scratch()
  {
    for (cudaEvent_t event : events) {
      if (event) {
        cudaEventDestroy(event);
      }
    }
    if (stream) {
      cudaStreamDestroy(stream);
    }
  }
};

__device__ float interpolate(float target, float origin, float factor)
{
  /* Match interpf and the CPU build's -ffp-contract=off. */
  return factor * target + (1.0f - factor) * origin;
}

struct Lookup {
  size_t indices[4];
  float x;
  float z;
};

__device__ float bilerp(const double *field, const Lookup &lookup)
{
  /* interpf takes float arguments: round each double field value BEFORE
   * interpolation, just as the existing BILERP macro does. */
  return interpolate(
      interpolate(float(field[lookup.indices[3]]), float(field[lookup.indices[1]]), lookup.x),
      interpolate(float(field[lookup.indices[2]]), float(field[lookup.indices[0]]), lookup.x),
      lookup.z);
}

__global__ void sample_kernel(Input input,
                              size_t count,
                              const float3 *reference,
                              float3 *positions,
                              uchar4 *foam,
                              float *compression,
                              uchar4 *spray)
{
  const size_t index = size_t(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index >= count) {
    return;
  }
  const float3 ref = reference[index];
  float u = fmodf(ref.x * input.size_co_inv + 0.5f, 1.0f);
  float v = fmodf(ref.y * input.size_co_inv + 0.5f, 1.0f);
  if (u < 0) {
    u += 1.0f;
  }
  if (v < 0) {
    v += 1.0f;
  }
  const float uu = u * input.size_x;
  const float vv = v * input.size_y;
  const int i = int(floorf(uu));
  const int j = int(floorf(vv));
  const int i0 = i % input.size_x, i1 = (i + 1) % input.size_x;
  const int j0 = j % input.size_y, j1 = (j + 1) % input.size_y;
  const Lookup lookup{{size_t(i0) * input.size_y + j0,
                       size_t(i0) * input.size_y + j1,
                       size_t(i1) * input.size_y + j0,
                       size_t(i1) * input.size_y + j1},
                      uu - i,
                      vv - j};
  float3 displaced = ref;
  displaced.z += bilerp(input.disp_y, lookup);
  if (input.chop) {
    displaced.x += bilerp(input.disp_x, lookup);
    displaced.y += bilerp(input.disp_z, lookup);
  }
  positions[index] = displaced;
  if (foam) {
    const float jxx = bilerp(input.jxx, lookup);
    const float jzz = bilerp(input.jzz, lookup);
    const float jxz = bilerp(input.jxz, lookup);
    const float a = jxx + jzz;
    const float b = sqrtf((jxx - jzz) * (jxx - jzz) + 4 * jxz * jxz);
    const float jminus = 0.5f * (a - b);
    const float jplus = 0.5f * (a + b);
    float value = jminus * -0.005f + input.foam_coverage;
    value = value < 0 ? 0 : (value > 1 ? 1 : value);
    const unsigned char byte = static_cast<unsigned char>(value * 255);
    foam[index] = make_uchar4(byte, byte, byte, 255);
    const float raw_compression = -jminus;
    compression[index] = raw_compression < 0 ? 0 : raw_compression;
    if (spray) {
      const float eigenvalue = input.inverse_spray ? jminus : jplus;
      const float q = (eigenvalue - jxx) / jxz;
      const float length = sqrtf(1 + q * q);
      spray[index] = make_uchar4(
          static_cast<unsigned char>(static_cast<int>((1.0f / length) * 255)),
          0,
          static_cast<unsigned char>(static_cast<int>((q / length) * 255)),
          255);
    }
  }
}

}  // namespace

bool sample(const Input &input,
            size_t count,
            const float *reference,
            float *positions,
            unsigned char *foam,
            float *compression,
            unsigned char *spray)
{
  /* Thread-local scratch avoids sharing streams or buffers between concurrently
   * evaluated modifiers. Retained buffers amortize context/allocation overhead. */
  static thread_local Scratch scratch;
  if (scratch.disabled || count == 0 || !input.disp_y || (foam && !input.jacobian)) {
    return false;
  }
  const auto start = std::chrono::steady_clock::now();
  const size_t field_bytes = size_t(input.size_x) * input.size_y * sizeof(double);
  const size_t position_bytes = count * 3 * sizeof(float);
  const size_t scalar_bytes = count * sizeof(float);
  const size_t color_bytes = count * 4;
  auto fail = [&]() {
    if (scratch.stream) {
      cudaStreamSynchronize(scratch.stream);
    }
    scratch.disabled = true;
    std::fprintf(stderr, "[OCEAN_CUDA] CUDA sampling failed; using CPU sampling\n");
    return false;
  };
  if (!scratch.stream) {
    if (cudaStreamCreateWithFlags(&scratch.stream, cudaStreamNonBlocking) != cudaSuccess) {
      return fail();
    }
    for (cudaEvent_t &event : scratch.events) {
      if (cudaEventCreate(&event) != cudaSuccess) {
        return fail();
      }
    }
  }
  if (!scratch.reference.reserve(position_bytes) || !scratch.positions.reserve(position_bytes) ||
      !scratch.host_positions.reserve(position_bytes))
  {
    return fail();
  }
  if (foam && (!scratch.foam.reserve(color_bytes) || !scratch.host_foam.reserve(color_bytes) ||
               !scratch.compression.reserve(scalar_bytes) ||
               !scratch.host_compression.reserve(scalar_bytes)))
  {
    return fail();
  }
  if (spray && (!scratch.spray.reserve(color_bytes) || !scratch.host_spray.reserve(color_bytes))) {
    return fail();
  }
  Input device_input = input;
  const double *host_fields[6] = {
      input.disp_y, input.disp_x, input.disp_z, input.jxx, input.jzz, input.jxz};
  const double **device_fields[6] = {&device_input.disp_y,
                                     &device_input.disp_x,
                                     &device_input.disp_z,
                                     &device_input.jxx,
                                     &device_input.jzz,
                                     &device_input.jxz};
  cudaEventRecord(scratch.events[0], scratch.stream);
  for (int field = 0; field < 6; field++) {
    const bool needed = field == 0 || (field < 3 ? input.chop : foam != nullptr);
    if (needed) {
      if (!scratch.fields[field].reserve(field_bytes) || !host_fields[field] ||
          cudaMemcpyAsync(scratch.fields[field].data,
                          host_fields[field],
                          field_bytes,
                          cudaMemcpyHostToDevice,
                          scratch.stream) != cudaSuccess)
      {
        return fail();
      }
      *device_fields[field] = static_cast<const double *>(scratch.fields[field].data);
    }
  }
  if (cudaMemcpyAsync(scratch.reference.data,
                      reference,
                      position_bytes,
                      cudaMemcpyHostToDevice,
                      scratch.stream) != cudaSuccess)
  {
    return fail();
  }
  cudaEventRecord(scratch.events[1], scratch.stream);
  sample_kernel<<<(count + 255) / 256, 256, 0, scratch.stream>>>(
      device_input,
      count,
      static_cast<const float3 *>(scratch.reference.data),
      static_cast<float3 *>(scratch.positions.data),
      foam ? static_cast<uchar4 *>(scratch.foam.data) : nullptr,
      compression ? static_cast<float *>(scratch.compression.data) : nullptr,
      spray ? static_cast<uchar4 *>(scratch.spray.data) : nullptr);
  if (cudaGetLastError() != cudaSuccess) {
    return fail();
  }
  cudaEventRecord(scratch.events[2], scratch.stream);
  auto download = [&](Buffer &host, Buffer &device, size_t bytes) {
    return cudaMemcpyAsync(
               host.data, device.data, bytes, cudaMemcpyDeviceToHost, scratch.stream) ==
           cudaSuccess;
  };
  if (!download(scratch.host_positions, scratch.positions, position_bytes) ||
      (foam && (!download(scratch.host_foam, scratch.foam, color_bytes) ||
                !download(scratch.host_compression, scratch.compression, scalar_bytes))) ||
      (spray && !download(scratch.host_spray, scratch.spray, color_bytes)))
  {
    return fail();
  }
  cudaEventRecord(scratch.events[3], scratch.stream);
  if (cudaStreamSynchronize(scratch.stream) != cudaSuccess) {
    return fail();
  }
  /* Commit only after every GPU operation succeeds, keeping CPU fallback safe. */
  std::memcpy(positions, scratch.host_positions.data, position_bytes);
  if (foam) {
    std::memcpy(foam, scratch.host_foam.data, color_bytes);
    std::memcpy(compression, scratch.host_compression.data, scalar_bytes);
  }
  if (spray) {
    std::memcpy(spray, scratch.host_spray.data, color_bytes);
  }
  const char *profile = std::getenv("BLENDER_OCEAN_CUDA_PROFILE");
  if (profile && profile[0] == '1') {
    float h2d, kernel, d2h;
    cudaEventElapsedTime(&h2d, scratch.events[0], scratch.events[1]);
    cudaEventElapsedTime(&kernel, scratch.events[1], scratch.events[2]);
    cudaEventElapsedTime(&d2h, scratch.events[2], scratch.events[3]);
    const double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf(
        "[OCEAN_CUDA_PROFILE] verts=%zu h2d_ms=%.6f kernel_ms=%.6f d2h_ms=%.6f wall_s=%.6f\n",
        count,
        h2d,
        kernel,
        d2h,
        wall);
  }
  return true;
}

}  // namespace blender::ocean_cuda
