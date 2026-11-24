// Kernels for the depthwise-separable blocks of the mobile vision nets
// (the OCR direction classifier, and the backbones of the detector and the
// recognizer).  Built after graph_kernels.cl in the same module: conv_t and
// activate() come from there, `real`, RLOAD and RSTORE from kernels_real.h.
// Activations are `real` (float, or half under GRAPH_FP16); every sum is
// taken in float in a fixed order and rounded once at the store.

#ifdef GRAPH_FP16
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#define RLOAD16(p, at) vload_half16(0, (p) + (at))
#define RSTORE8(p, at, v) vstore_half8(v, 0, (p) + (at))
#else
#define RLOAD16(p, at) vload16(0, (p) + (at))
#define RSTORE8(p, at, v) vstore8(v, 0, (p) + (at))
#endif

// Depthwise convolution.  A work item computes DW_R neighbouring outputs of
// one row of one channel.  With a kernel 3 or 5 wide at a horizontal stride
// of 1 or 2, the taps of a kernel row for all of them lie in 16 (stride 1)
// or 32 (stride 2) consecutive inputs: one or two loads, the values outside
// the row zeroed.  Any other geometry, and a load that would leave the
// tensor, reads tap by tap.  Taps are added in (ky, kx) order either way,
// a tap in the padding adding zero.
#define DW_R 8

#define DW_TAPS(KW, SW)                                                                  \
  __attribute__((opencl_unroll_hint)) for (int kx = 0; kx < (KW); ++kx) {                \
    const float wk = kernel_row[kx];                                                     \
    __attribute__((opencl_unroll_hint)) for (int r = 0; r < DW_R; ++r)                   \
        acc[r] = fma(wk, v[r * (SW) + kx], acc[r]);                                      \
  }

// A kernel row at stride SW: 16 * SW inputs loaded, masked at the row's
// edges, then the taps for a kernel 3 or 5 wide.
#define DW_WIDE(SW)                                                                      \
  {                                                                                      \
    float v[16 * (SW)];                                                                  \
    __attribute__((opencl_unroll_hint)) for (int l = 0; l < (SW); ++l)                   \
        vstore16(RLOAD16(in, row + sx0 + 16 * l), 0, v + 16 * l);                        \
    if (sx0 < 0 || sx0 + 16 * (SW) > p.iw) {                                             \
      __attribute__((opencl_unroll_hint)) for (int i = 0; i < 16 * (SW); ++i)            \
          v[i] = (sx0 + i >= 0 && sx0 + i < p.iw) ? v[i] : 0.0f;                         \
    }                                                                                    \
    if (p.kw == 3) {                                                                     \
      DW_TAPS(3, SW)                                                                     \
    } else {                                                                             \
      DW_TAPS(5, SW)                                                                     \
    }                                                                                    \
  }

__kernel void conv_depthwise(__global const real* in, __global real* out, __global const float* w,
                             __global const float* b, conv_t p) {
  const int blocks = (p.ow + DW_R - 1) / DW_R;
  const int at = get_global_id(0), nc = get_global_id(1);
  const int y = at / blocks, x0 = (at - y * blocks) * DW_R;
  if (y >= p.oh) return;
  const int c = nc % p.co;
  const long plane = (long)nc * p.ih * p.iw;
  const long total = (long)p.n * p.ci * p.ih * p.iw;
  __global const float* kernel_row = w + (size_t)c * p.kh * p.kw;
  const int sx0 = x0 * p.sw - p.pw;
  const bool wide = (p.kw == 3 || p.kw == 5) && (p.sw == 1 || p.sw == 2);
  const int span = 16 * p.sw;
  float acc[DW_R];
  __attribute__((opencl_unroll_hint)) for (int r = 0; r < DW_R; ++r) acc[r] = 0.0f;
  for (int ky = 0; ky < p.kh; ++ky, kernel_row += p.kw) {
    const int sy = y * p.sh - p.ph + ky;
    if (sy < 0 || sy >= p.ih) continue;
    const long row = plane + (long)sy * p.iw;
    if (wide && row + sx0 >= 0 && row + sx0 + span <= total) {
      if (p.sw == 1) {
        DW_WIDE(1)
      } else {
        DW_WIDE(2)
      }
    } else {
      for (int kx = 0; kx < p.kw; ++kx) {
        const float wk = kernel_row[kx];
        for (int r = 0; r < DW_R; ++r) {
          const int sx = sx0 + r * p.sw + kx;
          acc[r] = fma(wk, (sx >= 0 && sx < p.iw) ? RLOAD(in, row + sx) : 0.0f, acc[r]);
        }
      }
    }
  }
  const float bias = b[c];
  const size_t to = ((size_t)nc * p.oh + y) * p.ow + x0;
  if (x0 + DW_R <= p.ow) {
    float8 o;
    o.s0 = activate(acc[0] + bias, p.act, p.alpha, p.beta);
    o.s1 = activate(acc[1] + bias, p.act, p.alpha, p.beta);
    o.s2 = activate(acc[2] + bias, p.act, p.alpha, p.beta);
    o.s3 = activate(acc[3] + bias, p.act, p.alpha, p.beta);
    o.s4 = activate(acc[4] + bias, p.act, p.alpha, p.beta);
    o.s5 = activate(acc[5] + bias, p.act, p.alpha, p.beta);
    o.s6 = activate(acc[6] + bias, p.act, p.alpha, p.beta);
    o.s7 = activate(acc[7] + bias, p.act, p.alpha, p.beta);
    RSTORE8(out, to, o);
  } else {
    for (int r = 0; x0 + r < p.ow; ++r) RSTORE(out, to + r, activate(acc[r] + bias, p.act, p.alpha, p.beta));
  }
}

// out[plane][i] = x[plane][i] * s[plane]: a squeeze-and-excitation gate
// applied to its feature map ([N, C, H, W] times [N, C, 1, 1]).
__kernel void channel_scale(__global const real* x, __global const real* s, __global real* out, int planes, int hw) {
  const int i = get_global_id(0), plane = get_global_id(1);
  if (i >= hw || plane >= planes) return;
  const size_t at = (size_t)plane * hw + i;
  RSTORE(out, at, RLOAD(x, at) * RLOAD(s, plane));
}

// Two tensors of one shape, element by element.  op: 0 add, 2 mul.
__kernel void binary_same(__global const real* a, __global const real* b, __global real* out, int count, int op) {
  const int i = get_global_id(0);
  if (i >= count) return;
  const float x = RLOAD(a, i), y = RLOAD(b, i);
  RSTORE(out, i, op == 0 ? x + y : x * y);
}
