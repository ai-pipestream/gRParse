// Kernels for the depthwise-separable blocks of the mobile vision nets
// (the OCR direction classifier, and the backbones of the detector and the
// recognizer).  Built after graph_kernels.cl in the same module: conv_t and
// activate() come from there, `real`, RLOAD and RSTORE from kernels_real.h.
// Activations are `real` (float, or half under GRAPH_FP16); every sum is
// taken in float in a fixed order and rounded once at the store.

#ifdef GRAPH_FP16
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#define RLOAD16(p, at) convert_float16(vload16(0, (p) + (at)))
#define RSTORE8(p, at, v) vstore8(convert_half8(v), 0, (p) + (at))
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

// Eight of the loaded inputs for one tap: every SW-th from START.
#define DW_PICK1(START) shuffle(v0, (uint8)(0, 1, 2, 3, 4, 5, 6, 7) + (uint8)(START))
#define DW_PICK2(START) shuffle2(v0, v1, (uint8)(0, 2, 4, 6, 8, 10, 12, 14) + (uint8)(START))

#define DW_TAPS(KW, SW, SHIFT)                                                           \
  __attribute__((opencl_unroll_hint)) for (int kx = 0; kx < (KW); ++kx)                  \
      acc = fma((float8)(kernel_row[kx]), DW_PICK##SW(kx + (SHIFT)), acc);

// A kernel row at stride SW: 16 * SW inputs loaded from SHIFT before the
// first tap, zeroed outside the row, then the taps for a kernel 3 or 5
// wide.
#define DW_LOAD(V, L)                                                                    \
  float16 V = RLOAD16(in, row + from + 16 * (L));                                        \
  if (from + 16 * (L) < 0 || from + 16 * ((L) + 1) > p.iw) {                             \
    const int16 at = (int16)(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15) + (int16)(from + 16 * (L)); \
    V = select(V, (float16)(0.0f), (at < (int16)(0)) | (at >= (int16)(p.iw)));           \
  }
#define DW_LOADS1 DW_LOAD(v0, 0)
#define DW_LOADS2 DW_LOAD(v0, 0) DW_LOAD(v1, 1)
#define DW_WIDE(SW, SHIFT)                                                               \
  {                                                                                      \
    const int from = sx0 - (SHIFT);                                                      \
    DW_LOADS##SW                                                                         \
    if (p.kw == 3) {                                                                     \
      DW_TAPS(3, SW, SHIFT)                                                              \
    } else {                                                                             \
      DW_TAPS(5, SW, SHIFT)                                                              \
    }                                                                                    \
  }

// Half loads are far slower from an odd element (an address that is not a
// multiple of four bytes), which is where an odd padding or a row of an
// odd-width plane starts; the FP16 build loads from the element before it
// instead.
#ifdef GRAPH_FP16
#define DW_ODD(at) ((int)((at) & 1))
#else
#define DW_ODD(at) 0
#endif

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
  float8 acc = (float8)(0.0f);
  for (int ky = 0; ky < p.kh; ++ky, kernel_row += p.kw) {
    const int sy = y * p.sh - p.ph + ky;
    if (sy < 0 || sy >= p.ih) continue;
    const long row = plane + (long)sy * p.iw;
    const int odd = DW_ODD(row + sx0);
    if (wide && row + sx0 - odd >= 0 && row + sx0 - odd + span <= total) {
      if (p.sw == 1) {
        if (odd) {
          DW_WIDE(1, 1)
        } else {
          DW_WIDE(1, 0)
        }
      } else if (odd) {
        DW_WIDE(2, 1)
      } else {
        DW_WIDE(2, 0)
      }
    } else {
      for (int kx = 0; kx < p.kw; ++kx) {
        float taps[DW_R];
        for (int r = 0; r < DW_R; ++r) {
          const int sx = sx0 + r * p.sw + kx;
          taps[r] = (sx >= 0 && sx < p.iw) ? RLOAD(in, row + sx) : 0.0f;
        }
        acc = fma((float8)(kernel_row[kx]), vload8(0, taps), acc);
      }
    }
  }
  // Through activate() of graph_kernels.cl, value by value: the one place
  // that knows every activation.
  float sums[DW_R];
  vstore8(acc + (float8)(b[c]), 0, sums);
  __attribute__((opencl_unroll_hint)) for (int r = 0; r < DW_R; ++r)
      sums[r] = activate(sums[r], p.act, p.alpha, p.beta);
  const size_t to = ((size_t)nc * p.oh + y) * p.ow + x0;
  if (x0 + DW_R <= p.ow) {
    RSTORE8(out, to, vload8(0, sums));
  } else {
    for (int r = 0; x0 + r < p.ow; ++r) RSTORE(out, to + r, sums[r]);
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
