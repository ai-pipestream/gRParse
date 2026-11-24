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
  // Without a division where there is one image and where a factor is a power of two.
  const int n = nc < p.c ? 0 : nc / p.c, c = nc - n * p.c;
  const int xs = p.fx & (p.fx - 1) ? -1 : 31 - clz(p.fx), ys = p.fy & (p.fy - 1) ? -1 : 31 - clz(p.fy);
  const int xx = xs < 0 ? ox / p.fx : ox >> xs, xy = xs < 0 ? oy / p.fx : oy >> xs;
  float v = RLOAD(x, ((size_t)nc * p.xh + xy) * p.xw + xx);
  if (p.has_s) v = fma(v, RLOAD(s, nc), v);
  if (p.has_y) {
    const int yx = ys < 0 ? ox / p.fy : ox >> ys, yy = ys < 0 ? oy / p.fy : oy >> ys;
    v += RLOAD(y, ((size_t)nc * p.yh + yy) * p.yw + yx);
  }
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

// An interleaved 3-channel byte image into the graph's first buffer,
// normalized as RapidOCR's substractMeanNormalize does on the host:
// pixel * norm - mean * norm in float, each product rounded before the
// subtraction (no contraction), so the FP32 module stores the host's bits.
typedef struct {
  float mean[3], norm[3];
} image_t;
#pragma OPENCL FP_CONTRACT OFF
__kernel void image_to_real(__global const uchar* in, __global real* out, int pixels, image_t p) {
  const int index = get_global_id(0);
  if (index >= pixels) return;
  for (int c = 0; c < 3; ++c) {
    const float scaled = (float)in[(size_t)index * 3 + c] * p.norm[c], offset = p.mean[c] * p.norm[c];
    RSTORE(out, (size_t)c * pixels + index, scaled - offset);
  }
}
#pragma OPENCL FP_CONTRACT DEFAULT

// A dense convolution with few terms an output (ci * kh * kw up to 32, the
// network's first layer over an image's three channels): a work item takes
// one output pixel and 8 output channels and reads each term once.  A patch
// matrix or a tiled GEMM spends more moving these few terms than the sums
// cost.
__kernel void conv_few_terms(__global const real* in, __global real* out, __global const float* w,
                             __global const float* b, conv_t p) {
  const int x = get_global_id(0), y = get_global_id(1);
  const int tiles = (p.co + 7) / 8;
  const int n = get_global_id(2) / tiles, o0 = (get_global_id(2) % tiles) * 8;
  if (x >= p.ow || y >= p.oh) return;
  const int terms = p.ci * p.kh * p.kw;
  float acc[8];
  for (int o = 0; o < 8; ++o) acc[o] = 0.0f;
  for (int i = 0; i < p.ci; ++i) {
    for (int ky = 0; ky < p.kh; ++ky) {
      const int sy = y * p.sh - p.ph + ky;
      for (int kx = 0; kx < p.kw; ++kx) {
        const int sx = x * p.sw - p.pw + kx;
        const int inside = sy >= 0 && sy < p.ih && sx >= 0 && sx < p.iw;
        const float v = inside ? RLOAD(in, (((size_t)n * p.ci + i) * p.ih + sy) * p.iw + sx) : 0.0f;
        const int k = (i * p.kh + ky) * p.kw + kx;
        for (int o = 0; o < 8; ++o) acc[o] = fma(v, w[(size_t)min(o0 + o, p.co - 1) * terms + k], acc[o]);
      }
    }
  }
  for (int o = 0; o < 8 && o0 + o < p.co; ++o) {
    RSTORE(out, (((size_t)n * p.co + o0 + o) * p.oh + y) * p.ow + x, activate(acc[o] + b[o0 + o], p.act, p.alpha, p.beta));
  }
}

#ifdef GRAPH_FP16
// A dense stride-1 convolution that keeps its input's size, on the matrix
// engines, reading the input planes where they lie: no patch matrix is
// built.  A sub-group computes 16 output channels for a run of 32 pixels in
// row-major order.  For a tap, the pixels a run reads are a run too (the
// same one, shifted by the tap's offset), so 32 input channels at 16 pixels
// are one transform read with the channel planes as its rows; a pixel whose
// tap falls outside the image, where the shifted run wraps to the
// neighbouring row or leaves the plane, contributes zero.  Needs
// ih * iw to be a multiple of 32, so every plane starts on a 64-byte
// boundary and is a whole number of runs.  The block read places a run only
// on an even element (4 bytes), in its base and in its coordinate alike, so
// a tap whose offset is odd reads `in1`, the input moved down one element
// (shift_copy): in1[i] = in[i + 1].  Weights are [tap][co pad 16]
// [ci pad 32] halves, zero filled (GraphModel::build_plane_weights).
__attribute__((overloadable)) void intel_sub_group_2d_block_write_16b_8r16x1c(__global void* base, int width,
                                                                              int height, int pitch, int2 coord,
                                                                              __private ushort* src);

// out[i] = in[i + 1] for i from -32 up to `count`, 32 elements a work item
// as eight 64-bit words, each the word above it moved down one element, so
// every load and store is on a word boundary (a vector load of halves from
// an odd element is several times slower).  out[-1] is in[0], which the run
// starting one element before a plane reads.
__kernel void shift_copy(__global const half* in, __global half* out, int count) {
  const int at = (int)get_global_id(0) * 32 - 32;
  if (at >= count) return;
  const ulong8 low = vload8(0, (__global const ulong*)(in + at)), high = vload8(0, (__global const ulong*)(in + at + 4));
  vstore8((low >> 16) | (high << 48), 0, (__global ulong*)(out + at));
}

__kernel __attribute__((intel_reqd_sub_group_size(16))) __attribute__((reqd_work_group_size(64, 1, 1)))
void conv_planes_xmx(__global const half* in, __global const half* in1, __global half* out, __global const half* w,
                     __global const float* bias, conv_t p, int cip, int cop) {
  const int lane = get_sub_group_local_id();
  const int hw = p.ih * p.iw;
  const int q0 = (get_group_id(0) * 4 + get_sub_group_id()) * 32, m0 = get_group_id(1) * 16;
  if (q0 >= hw) return;
  in += (size_t)get_group_id(2) * p.ci * hw;
  in1 += (size_t)get_group_id(2) * p.ci * hw;
  out += (size_t)get_group_id(2) * p.co * hw;
  float8 acc[2][2];
  for (int i = 0; i < 2; ++i) acc[i][0] = acc[i][1] = (float8)(0.0f);
  int px[2], py[2];
  for (int j = 0; j < 2; ++j) {
    px[j] = (q0 + 16 * j + lane) % p.iw;
    py[j] = (q0 + 16 * j + lane) / p.iw;
  }
  for (int tap = 0; tap < p.kh * p.kw; ++tap) {
    const int dy = tap / p.kw - p.ph, dx = tap % p.kw - p.pw;
    int valid[2];
    for (int j = 0; j < 2; ++j) {
      valid[j] = py[j] + dy >= 0 && py[j] + dy < p.ih && px[j] + dx >= 0 && px[j] + dx < p.iw;
    }
    if (!sub_group_any(valid[0] | valid[1])) continue;
    __global const half* wt = w + (size_t)tap * cop * cip;
    // in1 at an even element is in at the odd one after it.
    const int odd = (dy * p.iw + dx) & 1;
    __global const half* src = odd ? in1 : in;
    for (int c0 = 0; c0 < p.ci; c0 += 32) {
      short8 blk[4];
      intel_sub_group_2d_block_read_16b_16r16x2c((__global void*)wt, cip * 2, cop, cip * 2, (int2)(c0, m0),
                                                 (__private ushort*)blk);
      for (int j = 0; j < 2; ++j) {
        // The shifted run's first element, split into a 32-element (64-byte) base and the rest.
        const int s = q0 + 16 * j + dy * p.iw + dx - odd, base = (s >> 5) << 5;
        int8 r[2];
        intel_sub_group_2d_block_read_transform_16b_32r16x1c(
            (__global void*)(src + (ptrdiff_t)c0 * hw + base), 128, min(32, p.ci - c0), hw * 2, (int2)(s - base, 0),
            (__private uint*)r);
        if (!valid[j]) r[0] = r[1] = (int8)(0);
        acc[0][j] = intel_sub_group_f16_f16_matrix_mad_k16(blk[0], r[0], acc[0][j]);
        acc[1][j] = intel_sub_group_f16_f16_matrix_mad_k16(blk[1], r[0], acc[1][j]);
        acc[0][j] = intel_sub_group_f16_f16_matrix_mad_k16(blk[2], r[1], acc[0][j]);
        acc[1][j] = intel_sub_group_f16_f16_matrix_mad_k16(blk[3], r[1], acc[1][j]);
      }
    }
  }
  for (int i = 0; i < 2; ++i) {
    const int row = m0 + 8 * i;
    if (row >= p.co) continue;
    for (int j = 0; j < 2; ++j) {
      float8 v = acc[i][j];
      for (int m = 0; m < 8; ++m) v[m] = activate(v[m] + bias[min(row + m, p.co - 1)], p.act, p.alpha, p.beta);
      if (row + 8 <= p.co) {
        ushort8 h = as_ushort8(convert_half8(v));
        intel_sub_group_2d_block_write_16b_8r16x1c((__global void*)(out + (size_t)row * hw + q0), 128, 8, hw * 2,
                                                   (int2)(16 * j, 0), (__private ushort*)&h);
      } else {
        for (int m = 0; row + m < p.co; ++m) vstore_half(v[m], (size_t)(row + m) * hw + q0 + 16 * j + lane, out);
      }
    }
  }
}
#endif
