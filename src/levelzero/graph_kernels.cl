// Kernels of the Level Zero graph runtime (graph_runtime.cpp).  Every
// kernel takes its shapes as arguments: no shape is compiled in, so an input
// of a new size runs at once.  Tensors are dense and row-major, their
// elements `real` (kernels_real.h); weights and biases are float.
#define ACT_NONE 0
#define ACT_HARD_SWISH 1
#define ACT_RELU 2
#define ACT_HARD_SIGMOID 3
#define ACT_SIGMOID 4
#define ACT_SWISH 5

static inline float activate(float x, int act, float alpha, float beta) {
  switch (act) {
    case ACT_HARD_SWISH: return x * fmin(fmax(x + 3.0f, 0.0f), 6.0f) / 6.0f;
    case ACT_RELU: return fmax(x, 0.0f);
    case ACT_HARD_SIGMOID: return fmin(fmax(alpha * x + beta, 0.0f), 1.0f);
    case ACT_SIGMOID: return 1.0f / (1.0f + exp(-x));
    case ACT_SWISH: return x / (1.0f + exp(-x));
    default: return x;
  }
}

// Up to six dimensions; an operand's stride is 0 along a broadcast axis.
typedef struct {
  int rank;
  int dims[6];
  int a[6];
  int b[6];
} nd_t;

static inline void nd_offsets(const nd_t* s, int index, int* oa, int* ob) {
  int a = 0, b = 0;
  for (int d = s->rank - 1; d >= 0; --d) {
    const int i = index % s->dims[d];
    index /= s->dims[d];
    a += i * s->a[d];
    b += i * s->b[d];
  }
  *oa = a;
  *ob = b;
}

// C[g][m][n] = sum_k A[g][m][k] B[g][k][n] (+ bias[m] or bias[n]), then the
// activation; A, B and C each by (group, row, column) strides.  A group of
// 256 work items computes a 64 x 64 tile, 4 x 4 per item, through local
// memory 16 terms at a time.
typedef struct {
  int n, ci, ih, iw, co, oh, ow;
  int kh, kw, sh, sw, ph, pw, groups;
  int act;
  float alpha, beta;
} conv_t;

#define TM 64
#define TN 64
#define TK 16
typedef struct {
  int m, n, k;
  int a_g, a_m, a_k;
  int b_g, b_k, b_n;
  int c_g, c_m, c_n;
  int bias_on;  // 0 none, 1 per row (m), 2 per column (n)
  int act;
  float alpha, beta;
} gemm_t;

__kernel __attribute__((reqd_work_group_size(256, 1, 1)))
void gemm(__global const real* A, __global const real* B, __global real* C, __global const float* bias, gemm_t p) {
  __local float as[TK][TM + 4];
  __local float bs[TK][TN + 4];
  const int t = get_local_id(0);
  const int m0 = get_group_id(1) * TM, n0 = get_group_id(0) * TN, g = get_group_id(2);
  A += (size_t)g * p.a_g;
  B += (size_t)g * p.b_g;
  C += (size_t)g * p.c_g;
  const int tm = (t / 16) * 4, tn = (t % 16) * 4;
  float acc[4][4];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) acc[i][j] = 0.0f;
  for (int k0 = 0; k0 < p.k; k0 += TK) {
    for (int e = t; e < TK * TM; e += 256) {
      const int kk = e / TM, mm = e % TM;
      const int row = m0 + mm, col = k0 + kk;
      as[kk][mm] = (row < p.m && col < p.k) ? RLOAD(A, (size_t)row * p.a_m + (size_t)col * p.a_k) : 0.0f;
    }
    for (int e = t; e < TK * TN; e += 256) {
      const int kk = e / TN, nn = e % TN;
      const int row = k0 + kk, col = n0 + nn;
      bs[kk][nn] = (row < p.k && col < p.n) ? RLOAD(B, (size_t)row * p.b_k + (size_t)col * p.b_n) : 0.0f;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    __attribute__((opencl_unroll_hint))
    for (int kk = 0; kk < TK; ++kk) {
      float av[4], bv[4];
      for (int i = 0; i < 4; ++i) av[i] = as[kk][tm + i];
      for (int j = 0; j < 4; ++j) bv[j] = bs[kk][tn + j];
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) acc[i][j] = fma(av[i], bv[j], acc[i][j]);
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  for (int i = 0; i < 4; ++i) {
    const int row = m0 + tm + i;
    if (row >= p.m) continue;
    for (int j = 0; j < 4; ++j) {
      const int col = n0 + tn + j;
      if (col >= p.n) continue;
      float v = acc[i][j];
      if (p.bias_on == 1) v += bias[row];
      if (p.bias_on == 2) v += bias[col];
      RSTORE(C, (size_t)row * p.c_m + (size_t)col * p.c_n, activate(v, p.act, p.alpha, p.beta));
    }
  }
}


// A dense convolution (groups 1) as an implicit GEMM: C[co][p] = sum over
// k = (ci, ky, kx) of w[co][k] x[ci][y*s - pad + ky][x*s - pad + kx], tiled
// as gemm is, the input patch gathered as it is staged.
__kernel __attribute__((reqd_work_group_size(256, 1, 1)))
void conv_gemm(__global const real* in, __global real* out, __global const float* w, __global const float* b,
               conv_t p) {
  __local float as[TK][TM + 4];
  __local float bs[TK][TN + 4];
  const int t = get_local_id(0);
  const int m0 = get_group_id(1) * TM, n0 = get_group_id(0) * TN, image = get_group_id(2);
  const int K = p.ci * p.kh * p.kw, P = p.oh * p.ow, taps = p.kh * p.kw;
  in += (size_t)image * p.ci * p.ih * p.iw;
  out += (size_t)image * p.co * P;
  const int tm = (t / 16) * 4, tn = (t % 16) * 4;
  float acc[4][4];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) acc[i][j] = 0.0f;
  for (int k0 = 0; k0 < K; k0 += TK) {
    for (int e = t; e < TK * TM; e += 256) {
      const int kk = e / TM, mm = e % TM;
      const int row = m0 + mm, col = k0 + kk;
      as[kk][mm] = (row < p.co && col < K) ? w[(size_t)row * K + col] : 0.0f;
    }
    for (int e = t; e < TK * TN; e += 256) {
      const int kk = e / TN, nn = e % TN;
      const int k = k0 + kk, q = n0 + nn;
      float v = 0.0f;
      if (k < K && q < P) {
        const int ci = k / taps, tap = k % taps;
        const int y = (q / p.ow) * p.sh - p.ph + tap / p.kw, x = (q % p.ow) * p.sw - p.pw + tap % p.kw;
        if (y >= 0 && y < p.ih && x >= 0 && x < p.iw) v = RLOAD(in, ((size_t)ci * p.ih + y) * p.iw + x);
      }
      bs[kk][nn] = v;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    __attribute__((opencl_unroll_hint))
    for (int kk = 0; kk < TK; ++kk) {
      float av[4], bv[4];
      for (int i = 0; i < 4; ++i) av[i] = as[kk][tm + i];
      for (int j = 0; j < 4; ++j) bv[j] = bs[kk][tn + j];
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) acc[i][j] = fma(av[i], bv[j], acc[i][j]);
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  for (int i = 0; i < 4; ++i) {
    const int row = m0 + tm + i;
    if (row >= p.co) continue;
    for (int j = 0; j < 4; ++j) {
      const int q = n0 + tn + j;
      if (q < P) RSTORE(out, (size_t)row * P + q, activate(acc[i][j] + b[row], p.act, p.alpha, p.beta));
    }
  }
}

// Depthwise: one output of one channel per work item.
__kernel void conv_depthwise(__global const real* in, __global real* out, __global const float* w,
                             __global const float* b, conv_t p) {
  const int x = get_global_id(0), y = get_global_id(1), nc = get_global_id(2);
  if (x >= p.ow || y >= p.oh) return;
  const int c = nc % p.co;
  __global const real* plane = in + (size_t)nc * p.ih * p.iw;
  __global const float* kw = w + (size_t)c * p.kh * p.kw;
  float acc = 0.0f;
  for (int ky = 0; ky < p.kh; ++ky) {
    const int sy = y * p.sh - p.ph + ky;
    if (sy < 0 || sy >= p.ih) continue;
    for (int kx = 0; kx < p.kw; ++kx) {
      const int sx = x * p.sw - p.pw + kx;
      if (sx >= 0 && sx < p.iw) acc = fma(kw[ky * p.kw + kx], RLOAD(plane, sy * p.iw + sx), acc);
    }
  }
  RSTORE(out, ((size_t)nc * p.oh + y) * p.ow + x, activate(acc + b[c], p.act, p.alpha, p.beta));
}

// Dense or grouped: one output pixel per work item, 8 output channels of
// one group at once.
#define CONV_CO 8
__kernel void conv_direct(__global const real* in, __global real* out, __global const float* w,
                          __global const float* b, conv_t p) {
  const int x = get_global_id(0), y = get_global_id(1);
  const int tiles = (p.co + CONV_CO - 1) / CONV_CO;
  const int n = get_global_id(2) / tiles, o0 = (get_global_id(2) % tiles) * CONV_CO;
  if (x >= p.ow || y >= p.oh) return;
  const int cig = p.ci / p.groups, cog = p.co / p.groups;
  const int group = o0 / cog;
  float acc[CONV_CO];
  for (int o = 0; o < CONV_CO; ++o) acc[o] = 0.0f;
  for (int i = 0; i < cig; ++i) {
    __global const real* plane = in + ((size_t)n * p.ci + group * cig + i) * p.ih * p.iw;
    for (int ky = 0; ky < p.kh; ++ky) {
      const int sy = y * p.sh - p.ph + ky;
      if (sy < 0 || sy >= p.ih) continue;
      for (int kx = 0; kx < p.kw; ++kx) {
        const int sx = x * p.sw - p.pw + kx;
        if (sx < 0 || sx >= p.iw) continue;
        const float v = RLOAD(plane, sy * p.iw + sx);
        for (int o = 0; o < CONV_CO; ++o) {
          const int oc = o0 + o;
          if (oc < p.co && oc / cog == group) {
            acc[o] = fma(w[(((size_t)oc * cig + i) * p.kh + ky) * p.kw + kx], v, acc[o]);
          }
        }
      }
    }
  }
  for (int o = 0; o < CONV_CO; ++o) {
    const int oc = o0 + o;
    if (oc < p.co) RSTORE(out, (((size_t)n * p.co + oc) * p.oh + y) * p.ow + x, activate(acc[o] + b[oc], p.act, p.alpha, p.beta));
  }
}

// Transposed, stride == kernel, no padding: each output reads one input pixel.
__kernel void conv_transpose(__global const real* in, __global real* out, __global const float* w,
                             __global const float* b, conv_t p) {
  const int x = get_global_id(0), y = get_global_id(1), nc = get_global_id(2);
  if (x >= p.ow || y >= p.oh) return;
  const int n = nc / p.co, c = nc % p.co;
  const int sy = y / p.sh, sx = x / p.sw, ky = y % p.sh, kx = x % p.sw;
  float acc = b[c];
  for (int i = 0; i < p.ci; ++i) {
    acc = fma(RLOAD(in, (((size_t)n * p.ci + i) * p.ih + sy) * p.iw + sx), w[(((size_t)i * p.co + c) * p.kh + ky) * p.kw + kx], acc);
  }
  RSTORE(out, ((size_t)nc * p.oh + y) * p.ow + x, activate(acc, p.act, p.alpha, p.beta));
}

// pool: 0 average (count_include_pad in flags bit 1), 1 max.
__kernel void pool2d(__global const real* in, __global real* out, conv_t p, int max_pool, int include_pad) {
  const int x = get_global_id(0), y = get_global_id(1), nc = get_global_id(2);
  if (x >= p.ow || y >= p.oh) return;
  __global const real* plane = in + (size_t)nc * p.ih * p.iw;
  float acc = max_pool ? -INFINITY : 0.0f;
  int count = 0;
  for (int ky = 0; ky < p.kh; ++ky) {
    const int sy = y * p.sh - p.ph + ky;
    for (int kx = 0; kx < p.kw; ++kx) {
      const int sx = x * p.sw - p.pw + kx;
      if (sy < 0 || sy >= p.ih || sx < 0 || sx >= p.iw) {
        if (include_pad) ++count;
        continue;
      }
      const float v = RLOAD(plane, sy * p.iw + sx);
      acc = max_pool ? fmax(acc, v) : acc + v;
      ++count;
    }
  }
  RSTORE(out, ((size_t)nc * p.oh + y) * p.ow + x, max_pool ? acc : acc / (float)count);
}

// Mean of each row of `cols` floats: one sub-group per row.
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void row_mean(__global const real* in, __global real* out, int rows, int cols) {
  const int r = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  if (r >= rows) return;
  float acc = 0.0f;
  for (int c = get_sub_group_local_id(); c < cols; c += 16) acc += RLOAD(in, (size_t)r * cols + c);
  acc = sub_group_reduce_add(acc);
  if (get_sub_group_local_id() == 0) RSTORE(out, r, acc / (float)cols);
}

// LayerNorm over each row: one sub-group per row, as Paddle computes it
// (mean, then the mean of squared deviations, then divide by sqrt(var+eps)).
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void layer_norm(__global const real* in, __global real* out, __global const float* gamma,
                __global const float* beta, int rows, int cols, float eps) {
  const int r = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  if (r >= rows) return;
  __global const real* row = in + (size_t)r * cols;
  const int lane = get_sub_group_local_id();
  float s = 0.0f;
  for (int c = lane; c < cols; c += 16) s += RLOAD(row, c);
  const float mean = sub_group_reduce_add(s) / (float)cols;
  float q = 0.0f;
  for (int c = lane; c < cols; c += 16) {
    const float d = RLOAD(row, c) - mean;
    q += d * d;
  }
  const float sd = sqrt(sub_group_reduce_add(q) / (float)cols + eps);
  for (int c = lane; c < cols; c += 16) RSTORE(out, (size_t)r * cols + c, (RLOAD(row, c) - mean) / sd * gamma[c] + beta[c]);
}

// Softmax over each row: one group of 256 per row.
__kernel __attribute__((reqd_work_group_size(256, 1, 1)))
void softmax_rows(__global const real* in, __global real* out, int cols) {
  const size_t r = get_group_id(0);
  __global const real* row = in + r * cols;
  float m = -INFINITY;
  for (int c = get_local_id(0); c < cols; c += 256) m = fmax(m, RLOAD(row, c));
  m = work_group_reduce_max(m);
  float s = 0.0f;
  for (int c = get_local_id(0); c < cols; c += 256) s += exp(RLOAD(row, c) - m);
  s = work_group_reduce_add(s);
  for (int c = get_local_id(0); c < cols; c += 256) RSTORE(out, r * cols + c, exp(RLOAD(row, c) - m) / s);
}

// op: 0 add, 1 sub, 2 mul, 3 div, 4 pow.
__kernel void binary(__global const real* a, __global const real* b, __global real* out, nd_t s, int count, int op) {
  const int index = get_global_id(0);
  if (index >= count) return;
  int ia, ib;
  nd_offsets(&s, index, &ia, &ib);
  const float x = RLOAD(a, ia), y = RLOAD(b, ib);
  float v;
  switch (op) {
    case 0: v = x + y; break;
    case 1: v = x - y; break;
    case 2: v = x * y; break;
    case 3: v = x / y; break;
    default: v = (y == 2.0f) ? x * x : pow(x, y); break;
  }
  RSTORE(out, index, v);
}

// op: 0 relu, 1 sigmoid, 2 hard sigmoid, 3 clip [alpha, beta], 4 sqrt.
__kernel void unary(__global const real* in, __global real* out, int count, int op, float alpha, float beta) {
  const int index = get_global_id(0);
  if (index >= count) return;
  const float x = RLOAD(in, index);
  float v;
  switch (op) {
    case 0: v = fmax(x, 0.0f); break;
    case 1: v = 1.0f / (1.0f + exp(-x)); break;
    case 2: v = fmin(fmax(alpha * x + beta, 0.0f), 1.0f); break;
    case 3: v = fmin(fmax(x, alpha), beta); break;
    default: v = sqrt(x); break;
  }
  RSTORE(out, index, v);
}

// out[i] = in[offset(i)]: transposes and slices; s.a are the input strides
// per output axis and s.b[0] the input start offset.
__kernel void gather_nd(__global const real* in, __global real* out, nd_t s, int count) {
  const int index = get_global_id(0);
  if (index >= count) return;
  int ia, ib;
  nd_offsets(&s, index, &ia, &ib);
  out[index] = in[ia + s.b[0]];
}

// A concat input into its range of the output: blocks of `inner` floats,
// `outer` of them, at output stride `out_stride`.
__kernel void copy_blocks(__global const real* in, __global real* out, int outer, int inner, int out_stride) {
  const int index = get_global_id(0);
  if (index >= outer * inner) return;
  out[(size_t)(index / inner) * out_stride + index % inner] = in[index];
}

// Nearest, asymmetric, floor: source = floor(destination / scale).
__kernel void resize_nearest(__global const real* in, __global real* out, int ih, int iw, int oh, int ow,
                             float scale_h, float scale_w) {
  const int x = get_global_id(0), y = get_global_id(1), nc = get_global_id(2);
  if (x >= ow || y >= oh) return;
  const int sy = min((int)floor((float)y / scale_h), ih - 1), sx = min((int)floor((float)x / scale_w), iw - 1);
  out[((size_t)nc * oh + y) * ow + x] = in[((size_t)nc * ih + sy) * iw + sx];
}

// Mean of each H x W plane: one sub-group per plane.
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void global_average_pool(__global const real* in, __global real* out, int planes, int hw) {
  const int pl = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  if (pl >= planes) return;
  float acc = 0.0f;
  for (int i = get_sub_group_local_id(); i < hw; i += 16) acc += RLOAD(in, (size_t)pl * hw + i);
  acc = sub_group_reduce_add(acc);
  if (get_sub_group_local_id() == 0) RSTORE(out, pl, acc / (float)hw);
}

// The graph's float input into its first buffer, and an output back to float.
__kernel void to_real(__global const float* in, __global real* out, int count) {
  const int index = get_global_id(0);
  if (index < count) RSTORE(out, index, in[index]);
}
__kernel void from_real(__global const real* in, __global float* out, int count) {
  const int index = get_global_id(0);
  if (index < count) out[index] = RLOAD(in, index);
}

#ifdef GRAPH_FP16
// ---- Convolutions and linear layers on the matrix engines (XMX) -----------
// C[m][col] = act(sum_k A[m][k] B[k][col] + bias[m]), F16 operands, F32 sums.
// Both operands are whole-block surfaces read with 2D block reads, nothing
// staged: A is the layer's weights as the runtime uploads them ([mp][kp],
// rows the outputs), B the layer's input repacked by pack_patches or
// pack_tokens ([kp][colsp], rows the terms, a column per output position of
// the whole batch).  One product (matrix_mad_k16) takes 8 x 16 of A (a lane
// per term, eight rows each) by 16 x 16 of B (a lane per column, its sixteen
// terms in pairs, which the read's VNNI transform makes) into 8 x 16 sums.  A
// sub-group computes 32 x 32 of C, a group of 2 x 2 sub-groups 64 x 64, 32
// terms a step.
__attribute__((overloadable)) float8 intel_sub_group_f16_f16_matrix_mad_k16(short8 a, int8 b, float8 acc);
__attribute__((overloadable)) void intel_sub_group_2d_block_read_16b_16r16x2c(__global void* base, int width,
                                                                              int height, int pitch, int2 coord,
                                                                              __private ushort* dst);
__attribute__((overloadable)) void intel_sub_group_2d_block_read_transform_16b_32r16x1c(__global void* base, int width,
                                                                                        int height, int pitch,
                                                                                        int2 coord,
                                                                                        __private uint* dst);
typedef struct {
  int m, cols, k;      // outputs, columns, terms
  int mp, kp, colsp;   // padded: A is [mp][kp], B is [kp][colsp]
  int c_m, c_n, c_image, per_image;  // C[m][col] is at m * c_m + col * c_n + (col / per_image) * c_image
  int act;
  float alpha, beta;
} xmx_t;

__kernel __attribute__((intel_reqd_sub_group_size(16))) __attribute__((reqd_work_group_size(64, 1, 1)))
void gemm_xmx(__global const half* A, __global const half* B, __global half* C, __global const float* bias, xmx_t p) {
  const int sg = get_sub_group_id(), lane = get_sub_group_local_id();
  const int n0 = get_group_id(0) * 64 + (sg % 2) * 32, m0 = get_group_id(1) * 64 + (sg / 2) * 32;
  if (n0 >= p.cols || m0 >= p.m) return;
  float8 acc[4][2];
  for (int i = 0; i < 4; ++i) acc[i][0] = acc[i][1] = (float8)(0.0f);
  for (int k0 = 0; k0 < p.k; k0 += 32) {
    // a[h][i]: rows m0 + 8i.., terms k0 + 16h..; b[h][j]: the same terms, columns n0 + 16j..
    short8 a[2][4];
    for (int r = 0; r < 2; ++r) {
      short8 blk[4];
      intel_sub_group_2d_block_read_16b_16r16x2c((__global void*)A, p.kp * 2, p.mp, p.kp * 2, (int2)(k0, m0 + 16 * r),
                                                 (__private ushort*)blk);
      a[0][2 * r] = blk[0];
      a[0][2 * r + 1] = blk[1];
      a[1][2 * r] = blk[2];
      a[1][2 * r + 1] = blk[3];
    }
    int8 b[2][2];
    for (int j = 0; j < 2; ++j) {
      int8 r[2];
      intel_sub_group_2d_block_read_transform_16b_32r16x1c((__global void*)B, p.colsp * 2, p.kp, p.colsp * 2,
                                                           (int2)(n0 + 16 * j, k0), (__private uint*)r);
      b[0][j] = r[0];
      b[1][j] = r[1];
    }
    for (int h = 0; h < 2; ++h)
      for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j) acc[i][j] = intel_sub_group_f16_f16_matrix_mad_k16(a[h][i], b[h][j], acc[i][j]);
  }
  for (int j = 0; j < 2; ++j) {
    const int col = n0 + 16 * j + lane;
    if (col >= p.cols) continue;
    const size_t at = (size_t)col * p.c_n + (size_t)(col / p.per_image) * p.c_image;
    for (int i = 0; i < 4; ++i) {
      const float vals[8] = {acc[i][j].s0, acc[i][j].s1, acc[i][j].s2, acc[i][j].s3,
                             acc[i][j].s4, acc[i][j].s5, acc[i][j].s6, acc[i][j].s7};
      for (int r = 0; r < 8; ++r) {
        const int row = m0 + 8 * i + r;
        if (row < p.m) vstore_half(activate(vals[r] + bias[row], p.act, p.alpha, p.beta), at + (size_t)row * p.c_m, C);
      }
    }
  }
}

typedef struct {
  int n, ci, ih, iw, oh, ow;
  int kh, kw, sh, sw, ph, pw;
  int k, kp, cols, colsp;  // terms ci * kh * kw, columns n * oh * ow, and both padded
} pack_t;

// A convolution's input as gemm_xmx's B: B[k][col] is the input pixel term k
// (channel, tap) reads for output position col (image, y, x); zero outside
// the image and in the padding.  One element per work item.
__kernel void pack_patches(__global const half* in, __global half* B, pack_t p) {
  const int col = get_global_id(0), k = get_global_id(1);
  if (col >= p.colsp || k >= p.kp) return;
  __global const ushort* src = (__global const ushort*)in;
  ushort v = 0;
  if (k < p.k && col < p.cols) {
    const int taps = p.kh * p.kw, positions = p.oh * p.ow;
    const int image = col / positions, q = col % positions;
    const int ci = k / taps, tap = k % taps;
    const int y = (q / p.ow) * p.sh - p.ph + tap / p.kw, x = (q % p.ow) * p.sw - p.pw + tap % p.kw;
    if (y >= 0 && y < p.ih && x >= 0 && x < p.iw) v = src[(((size_t)image * p.ci + ci) * p.ih + y) * p.iw + x];
  }
  ((__global ushort*)B)[(size_t)k * p.colsp + col] = v;
}

// A token-major tensor X[t][k] as gemm_xmx's B: B[k][t].
__kernel void pack_tokens(__global const half* X, __global half* B, int tokens, int k_terms, int kp, int colsp) {
  const int col = get_global_id(0), k = get_global_id(1);
  if (col >= colsp || k >= kp) return;
  ushort v = 0;
  if (k < k_terms && col < tokens) v = ((__global const ushort*)X)[(size_t)col * k_terms + k];
  ((__global ushort*)B)[(size_t)k * colsp + col] = v;
}
#endif
