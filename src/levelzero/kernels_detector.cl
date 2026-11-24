// Kernels for the text detector's neck and head (a feature pyramid with
// squeeze-excite blocks, then transposed convolutions), built into the
// graph module after graph_kernels.cl, whose types and activate() they use.
// Shapes are arguments; activations are `real` (kernels_real.h).

// Global average pool of large planes.  One sub-group sums one chunk of
// GAP_CHUNK pixels of one plane, so a plane's read is spread over many
// threads; gap_finish adds a plane's chunk sums.  With one chunk a plane,
// gap_partial writes the mean itself.
#define GAP_CHUNK 4096
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void gap_partial(__global const real* in, __global float* partial, __global real* out, int planes, int hw,
                 int chunks) {
  const int sg = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  if (sg >= planes * chunks) return;
  const int plane = sg / chunks, begin = (sg % chunks) * GAP_CHUNK, end = min(hw, begin + GAP_CHUNK);
  __global const real* p = in + (size_t)plane * hw;
  float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int i = begin + get_sub_group_local_id();
  for (; i + 48 < end; i += 64) {
    acc[0] += RLOAD(p, i);
    acc[1] += RLOAD(p, i + 16);
    acc[2] += RLOAD(p, i + 32);
    acc[3] += RLOAD(p, i + 48);
  }
  for (; i < end; i += 16) acc[0] += RLOAD(p, i);
  const float sum = sub_group_reduce_add((acc[0] + acc[1]) + (acc[2] + acc[3]));
  if (get_sub_group_local_id() != 0) return;
  if (chunks == 1) {
    RSTORE(out, plane, sum / (float)hw);
  } else {
    partial[sg] = sum;
  }
}

__kernel __attribute__((intel_reqd_sub_group_size(16)))
void gap_finish(__global const float* partial, __global real* out, int planes, int hw, int chunks) {
  const int plane = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  if (plane >= planes) return;
  float acc = 0.0f;
  for (int c = get_sub_group_local_id(); c < chunks; c += 16) acc += partial[(size_t)plane * chunks + c];
  acc = sub_group_reduce_add(acc);
  if (get_sub_group_local_id() == 0) RSTORE(out, plane, acc / (float)hw);
}

// out[c_off + c] = nearest(x[c]) * (1 + s[c]) + nearest(y[c]) over c
// channels of an output of c_total: the squeeze-excite gate and its
// residual, a pyramid level's upsampled addend, and a concat part written in
// place, in one pass.  fx and fy are the whole-number up factors of x and y.
typedef struct {
  int c, c_total, c_off;
  int oh, ow;
  int xh, xw, fx;
  int yh, yw, fy;
  int has_s, has_y;
} merge_t;

__kernel void scale_merge(__global const real* x, __global const real* s, __global const real* y,
                          __global real* out, merge_t p) {
  const int ox = get_global_id(0), oy = get_global_id(1), nc = get_global_id(2);
  if (ox >= p.ow || oy >= p.oh) return;
  const int n = nc / p.c, c = nc % p.c;
  float v = RLOAD(x, ((size_t)nc * p.xh + oy / p.fx) * p.xw + ox / p.fx);
  if (p.has_s) v = fma(v, RLOAD(s, nc), v);
  if (p.has_y) v += RLOAD(y, ((size_t)nc * p.yh + oy / p.fy) * p.yw + ox / p.fy);
  RSTORE(out, (((size_t)n * p.c_total + p.c_off + c) * p.oh + oy) * p.ow + ox, v);
}

// Transposed 2 x 2, stride 2, no padding: a work item takes one input pixel
// and CT output channels, reads the pixel's ci inputs once and writes the
// CT x 4 outputs they make.  conv_transpose (graph_kernels.cl) reads the ci
// inputs once for every output instead.  Weights are [ci][co][2][2].
#define CONV_TRANSPOSE2(NAME, CT)                                                                          \
  __kernel void NAME(__global const real* in, __global real* out, __global const float* w,                 \
                     __global const float* b, conv_t p) {                                                  \
    const int x = get_global_id(0), y = get_global_id(1);                                                  \
    const int tiles = (p.co + (CT) - 1) / (CT);                                                            \
    const int n = get_global_id(2) / tiles, o0 = (get_global_id(2) % tiles) * (CT);                        \
    if (x >= p.iw || y >= p.ih) return;                                                                    \
    float acc[CT][4];                                                                                      \
    for (int o = 0; o < (CT); ++o)                                                                         \
      for (int t = 0; t < 4; ++t) acc[o][t] = 0.0f;                                                        \
    for (int i = 0; i < p.ci; ++i) {                                                                       \
      const float v = RLOAD(in, (((size_t)n * p.ci + i) * p.ih + y) * p.iw + x);                           \
      __global const float* wi = w + (size_t)i * p.co * 4;                                                 \
      for (int o = 0; o < (CT); ++o) {                                                                     \
        const int oc = min(o0 + o, p.co - 1);                                                              \
        for (int t = 0; t < 4; ++t) acc[o][t] = fma(v, wi[oc * 4 + t], acc[o][t]);                         \
      }                                                                                                    \
    }                                                                                                      \
    for (int o = 0; o < (CT); ++o) {                                                                       \
      const int oc = o0 + o;                                                                               \
      if (oc >= p.co) continue;                                                                            \
      for (int t = 0; t < 4; ++t) {                                                                        \
        const size_t at = (((size_t)n * p.co + oc) * p.oh + 2 * y + t / 2) * p.ow + 2 * x + t % 2;         \
        RSTORE(out, at, activate(acc[o][t] + b[oc], p.act, p.alpha, p.beta));                              \
      }                                                                                                    \
    }                                                                                                      \
  }
CONV_TRANSPOSE2(conv_transpose2_8, 8)
CONV_TRANSPOSE2(conv_transpose2_1, 1)
