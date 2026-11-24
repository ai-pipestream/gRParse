// A pointwise (1 x 1) convolution in float that stages nothing: no patch
// matrix, no local memory, no barrier.  Built after graph_kernels.cl:
// conv_t and activate() come from there.
//
// A sub-group takes 16 neighbouring pixels of one image, a lane each, and a
// tile of output channels.  The input is read where it lies, plane by
// plane: one block read gives each lane its pixel of input channel ci.  The
// weights come transposed, wt[ci][co padded to the tile], so the weights
// that term needs for the tile are one run the whole sub-group shares.
// Every output is summed over ci in order, whatever the batch or the
// pixel's place in it.
//
// The work is bound by how often the input planes are read, once per tile:
// a wider tile reads them less, until its sums no longer fit the registers
// (128 outputs ran at less than half the speed of 64 on an Arc B70).  The
// runtime picks the narrowest of the three tiles that holds the layer's
// outputs, 64 for all larger layers.
//
// The last sub-group of a plane reads up to 15 elements past it (the next
// plane, the next image, or the arena's slack) into lanes that store
// nothing.
__attribute__((overloadable)) uint intel_sub_group_block_read(const __global uint* p);
__attribute__((overloadable)) void intel_sub_group_block_write(__global uint* p, uint v);

// Group ids, not global ids, place a sub-group: a group is one sub-group,
// and what all 16 lanes share has to be known as shared to be loaded once.
#define CONV_POINTWISE(NAME, TILE)                                                               \
  __kernel __attribute__((intel_reqd_sub_group_size(16)))                                        \
  void NAME(__global const float* in, __global float* out, __global const float* wt,             \
            __global const float* bias, conv_t p, int cop) {                                     \
    const int lane = get_sub_group_local_id();                                                   \
    const int first = get_group_id(0) * 16, pixel = first + lane;                                \
    const int o0 = get_group_id(1) * (TILE), image = get_group_id(2);                            \
    const int pixels = p.ih * p.iw;                                                              \
    in += (size_t)image * p.ci * pixels + first;                                                 \
    out += (size_t)image * p.co * pixels + first;                                                \
    wt += o0;                                                                                    \
    float16 acc[(TILE) / 16];                                                                    \
    __attribute__((opencl_unroll_hint)) for (int j = 0; j < (TILE) / 16; ++j) acc[j] = (float16)(0.0f); \
    for (int ci = 0; ci < p.ci; ++ci) {                                                          \
      const float x = as_float(intel_sub_group_block_read((__global const uint*)(in + (size_t)ci * pixels))); \
      __attribute__((opencl_unroll_hint)) for (int j = 0; j < (TILE) / 16; ++j)                  \
          acc[j] = fma(vload16(j, wt + (size_t)ci * cop), (float16)(x), acc[j]);                 \
    }                                                                                            \
    float sums[TILE];                                                                            \
    __attribute__((opencl_unroll_hint)) for (int j = 0; j < (TILE) / 16; ++j) vstore16(acc[j], j, sums); \
    const bool whole = first + 16 <= pixels;                                                     \
    for (int j = 0; j < (TILE) && o0 + j < p.co; ++j) {                                          \
      const float v = activate(sums[j] + bias[o0 + j], p.act, p.alpha, p.beta);                  \
      __global float* to = out + (size_t)(o0 + j) * pixels;                                      \
      if (whole) {                                                                               \
        intel_sub_group_block_write((__global uint*)to, as_uint(v));                             \
      } else if (pixel < pixels) {                                                               \
        to[lane] = v;                                                                            \
      }                                                                                          \
    }                                                                                            \
  }

CONV_POINTWISE(conv_pointwise_16, 16)
CONV_POINTWISE(conv_pointwise_32, 32)
CONV_POINTWISE(conv_pointwise_64, 64)
