// Kernels of the Level Zero graph runtime (graph_runtime.cpp).  Every
// kernel takes its shapes as arguments: no shape is compiled in, so an input
// of a new size runs at once.  Tensors are dense, row-major floats.
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
void gemm(__global const float* A, __global const float* B, __global float* C, __global const float* bias, gemm_t p) {
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
      as[kk][mm] = (row < p.m && col < p.k) ? A[(size_t)row * p.a_m + (size_t)col * p.a_k] : 0.0f;
    }
    for (int e = t; e < TK * TN; e += 256) {
      const int kk = e / TN, nn = e % TN;
      const int row = k0 + kk, col = n0 + nn;
      bs[kk][nn] = (row < p.k && col < p.n) ? B[(size_t)row * p.b_k + (size_t)col * p.b_n] : 0.0f;
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
      C[(size_t)row * p.c_m + (size_t)col * p.c_n] = activate(v, p.act, p.alpha, p.beta);
    }
  }
}


// A dense convolution (groups 1) as an implicit GEMM: C[co][p] = sum over
// k = (ci, ky, kx) of w[co][k] x[ci][y*s - pad + ky][x*s - pad + kx], tiled
// as gemm is, the input patch gathered as it is staged.
__kernel __attribute__((reqd_work_group_size(256, 1, 1)))
void conv_gemm(__global const float* in, __global float* out, __global const float* w, __global const float* b,
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
        if (y >= 0 && y < p.ih && x >= 0 && x < p.iw) v = in[((size_t)ci * p.ih + y) * p.iw + x];
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
      if (q < P) out[(size_t)row * P + q] = activate(acc[i][j] + b[row], p.act, p.alpha, p.beta);
    }
  }
}

// Depthwise: one output of one channel per work item.
__kernel void conv_depthwise(__global const float* in, __global float* out, __global const float* w,
                             __global const float* b, conv_t p) {
  const int x = get_global_id(0), y = get_global_id(1), nc = get_global_id(2);
  if (x >= p.ow || y >= p.oh) return;
  const int c = nc % p.co;
  __global const float* plane = in + (size_t)nc * p.ih * p.iw;
  __global const float* kw = w + (size_t)c * p.kh * p.kw;
  float acc = 0.0f;
  for (int ky = 0; ky < p.kh; ++ky) {
    const int sy = y * p.sh - p.ph + ky;
    if (sy < 0 || sy >= p.ih) continue;
    for (int kx = 0; kx < p.kw; ++kx) {
      const int sx = x * p.sw - p.pw + kx;
      if (sx >= 0 && sx < p.iw) acc = fma(kw[ky * p.kw + kx], plane[sy * p.iw + sx], acc);
    }
  }
  out[((size_t)nc * p.oh + y) * p.ow + x] = activate(acc + b[c], p.act, p.alpha, p.beta);
}

// Dense or grouped: one output pixel per work item, 8 output channels of
// one group at once.
#define CONV_CO 8
__kernel void conv_direct(__global const float* in, __global float* out, __global const float* w,
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
    __global const float* plane = in + ((size_t)n * p.ci + group * cig + i) * p.ih * p.iw;
    for (int ky = 0; ky < p.kh; ++ky) {
      const int sy = y * p.sh - p.ph + ky;
      if (sy < 0 || sy >= p.ih) continue;
      for (int kx = 0; kx < p.kw; ++kx) {
        const int sx = x * p.sw - p.pw + kx;
        if (sx < 0 || sx >= p.iw) continue;
        const float v = plane[sy * p.iw + sx];
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
    if (oc < p.co) out[(((size_t)n * p.co + oc) * p.oh + y) * p.ow + x] = activate(acc[o] + b[oc], p.act, p.alpha, p.beta);
  }
}

// Transposed, stride == kernel, no padding: each output reads one input pixel.
__kernel void conv_transpose(__global const float* in, __global float* out, __global const float* w,
                             __global const float* b, conv_t p) {
  const int x = get_global_id(0), y = get_global_id(1), nc = get_global_id(2);
  if (x >= p.ow || y >= p.oh) return;
  const int n = nc / p.co, c = nc % p.co;
  const int sy = y / p.sh, sx = x / p.sw, ky = y % p.sh, kx = x % p.sw;
  float acc = b[c];
  for (int i = 0; i < p.ci; ++i) {
    acc = fma(in[(((size_t)n * p.ci + i) * p.ih + sy) * p.iw + sx], w[(((size_t)i * p.co + c) * p.kh + ky) * p.kw + kx], acc);
  }
  out[((size_t)nc * p.oh + y) * p.ow + x] = activate(acc, p.act, p.alpha, p.beta);
}

// pool: 0 average (count_include_pad in flags bit 1), 1 max.
__kernel void pool2d(__global const float* in, __global float* out, conv_t p, int max_pool, int include_pad) {
  const int x = get_global_id(0), y = get_global_id(1), nc = get_global_id(2);
  if (x >= p.ow || y >= p.oh) return;
  __global const float* plane = in + (size_t)nc * p.ih * p.iw;
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
      const float v = plane[sy * p.iw + sx];
      acc = max_pool ? fmax(acc, v) : acc + v;
      ++count;
    }
  }
  out[((size_t)nc * p.oh + y) * p.ow + x] = max_pool ? acc : acc / (float)count;
}

// Mean of each row of `cols` floats: one sub-group per row.
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void row_mean(__global const float* in, __global float* out, int rows, int cols) {
  const int r = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  if (r >= rows) return;
  float acc = 0.0f;
  for (int c = get_sub_group_local_id(); c < cols; c += 16) acc += in[(size_t)r * cols + c];
  acc = sub_group_reduce_add(acc);
  if (get_sub_group_local_id() == 0) out[r] = acc / (float)cols;
}

// LayerNorm over each row: one sub-group per row, as Paddle computes it
// (mean, then the mean of squared deviations, then divide by sqrt(var+eps)).
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void layer_norm(__global const float* in, __global float* out, __global const float* gamma,
                __global const float* beta, int rows, int cols, float eps) {
  const int r = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  if (r >= rows) return;
  __global const float* row = in + (size_t)r * cols;
  const int lane = get_sub_group_local_id();
  float s = 0.0f;
  for (int c = lane; c < cols; c += 16) s += row[c];
  const float mean = sub_group_reduce_add(s) / (float)cols;
  float q = 0.0f;
  for (int c = lane; c < cols; c += 16) {
    const float d = row[c] - mean;
    q += d * d;
  }
  const float sd = sqrt(sub_group_reduce_add(q) / (float)cols + eps);
  for (int c = lane; c < cols; c += 16) out[(size_t)r * cols + c] = (row[c] - mean) / sd * gamma[c] + beta[c];
}

// Softmax over each row: one group of 256 per row.
__kernel __attribute__((reqd_work_group_size(256, 1, 1)))
void softmax_rows(__global const float* in, __global float* out, int cols) {
  const size_t r = get_group_id(0);
  __global const float* row = in + r * cols;
  float m = -INFINITY;
  for (int c = get_local_id(0); c < cols; c += 256) m = fmax(m, row[c]);
  m = work_group_reduce_max(m);
  float s = 0.0f;
  for (int c = get_local_id(0); c < cols; c += 256) s += exp(row[c] - m);
  s = work_group_reduce_add(s);
  for (int c = get_local_id(0); c < cols; c += 256) out[r * cols + c] = exp(row[c] - m) / s;
}

// op: 0 add, 1 sub, 2 mul, 3 div, 4 pow.
__kernel void binary(__global const float* a, __global const float* b, __global float* out, nd_t s, int count, int op) {
  const int index = get_global_id(0);
  if (index >= count) return;
  int ia, ib;
  nd_offsets(&s, index, &ia, &ib);
  const float x = a[ia], y = b[ib];
  float v;
  switch (op) {
    case 0: v = x + y; break;
    case 1: v = x - y; break;
    case 2: v = x * y; break;
    case 3: v = x / y; break;
    default: v = (y == 2.0f) ? x * x : pow(x, y); break;
  }
  out[index] = v;
}

// op: 0 relu, 1 sigmoid, 2 hard sigmoid, 3 clip [alpha, beta], 4 sqrt.
__kernel void unary(__global const float* in, __global float* out, int count, int op, float alpha, float beta) {
  const int index = get_global_id(0);
  if (index >= count) return;
  const float x = in[index];
  float v;
  switch (op) {
    case 0: v = fmax(x, 0.0f); break;
    case 1: v = 1.0f / (1.0f + exp(-x)); break;
    case 2: v = fmin(fmax(alpha * x + beta, 0.0f), 1.0f); break;
    case 3: v = fmin(fmax(x, alpha), beta); break;
    default: v = sqrt(x); break;
  }
  out[index] = v;
}

// out[i] = in[offset(i)]: transposes and slices; s.a are the input strides
// per output axis and s.b[0] the input start offset.
__kernel void gather_nd(__global const float* in, __global float* out, nd_t s, int count) {
  const int index = get_global_id(0);
  if (index >= count) return;
  int ia, ib;
  nd_offsets(&s, index, &ia, &ib);
  out[index] = in[ia + s.b[0]];
}

// A concat input into its range of the output: blocks of `inner` floats,
// `outer` of them, at output stride `out_stride`.
__kernel void copy_blocks(__global const float* in, __global float* out, int outer, int inner, int out_stride) {
  const int index = get_global_id(0);
  if (index >= outer * inner) return;
  out[(size_t)(index / inner) * out_stride + index % inner] = in[index];
}

// Nearest, asymmetric, floor: source = floor(destination / scale).
__kernel void resize_nearest(__global const float* in, __global float* out, int ih, int iw, int oh, int ow,
                             float scale_h, float scale_w) {
  const int x = get_global_id(0), y = get_global_id(1), nc = get_global_id(2);
  if (x >= ow || y >= oh) return;
  const int sy = min((int)floor((float)y / scale_h), ih - 1), sx = min((int)floor((float)x / scale_w), iw - 1);
  out[((size_t)nc * oh + y) * ow + x] = in[((size_t)nc * ih + sy) * iw + sx];
}

// Mean of each H x W plane: one sub-group per plane.
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void global_average_pool(__global const float* in, __global float* out, int planes, int hw) {
  const int pl = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  if (pl >= planes) return;
  float acc = 0.0f;
  for (int i = get_sub_group_local_id(); i < hw; i += 16) acc += in[(size_t)pl * hw + i];
  acc = sub_group_reduce_add(acc);
  if (get_sub_group_local_id() == 0) out[pl] = acc / (float)hw;
}
