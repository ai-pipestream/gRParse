// The SLANet+ encoder's kernels (src/levelzero/encoder_plan.h lowers the
// graph to them).  Activations are channel-major per image, images
// image_stride floats apart; every buffer is padded to 16 floats, so a
// four-pixel load past a row of pixels stays inside its own buffer.
#define ACT_NONE 0
#define ACT_HARD_SWISH 1
#define ACT_RELU 2
#define ACT_HARD_SIGMOID 3

static inline float activate(float x, int act, float alpha, float beta) {
  if (act == ACT_HARD_SWISH) return x * fmin(fmax(x / 6.0f + 0.5f, 0.0f), 1.0f);
  if (act == ACT_RELU) return fmax(x, 0.0f);
  if (act == ACT_HARD_SIGMOID) return fmin(fmax(alpha * x + beta, 0.0f), 1.0f);
  return x;
}

// 1x1 convolution: out[co][p] = b[co] + sum_ci w[co][ci] in[ci][p], as a
// GEMM tiled through local memory.  A group of 64 work items computes a
// PW_TC x PW_TP output tile, each item 4 channels at 4 consecutive pixels;
// the K loop stages PW_TK input channels of weights and pixels at a time.
#define PW_TC 16
#define PW_TP 64
#define PW_TK 32
__kernel __attribute__((reqd_work_group_size(64, 1, 1)))
void pointwise(__global const float* in, __global float* out, __global const float* w,
               __global const float* b, int ci, int co, int hw, uint image_stride, int act, float alpha,
               float beta) {
  __local float xs[PW_TK][PW_TP];
  __local float ws[PW_TK][PW_TC];
  const int t = get_local_id(0);
  const int p0 = get_group_id(0) * PW_TP, c0 = get_group_id(1) * PW_TC;
  const size_t image = get_global_id(2) * (size_t)image_stride;
  in += image;
  out += image;
  const int tp = (t % 16) * 4, tc = (t / 16) * 4;  // this item's pixels and channels in the tile
  float4 acc[4] = {(float4)(0.0f), (float4)(0.0f), (float4)(0.0f), (float4)(0.0f)};
  for (int k0 = 0; k0 < ci; k0 += PW_TK) {
    // Stage PW_TK rows of PW_TP pixels (32 per item) and PW_TK x PW_TC weights (8 per item).
    for (int e = t; e < PW_TK * PW_TP; e += 64) {
      const int k = e / PW_TP, px = e % PW_TP;
      xs[k][px] = (k0 + k < ci && p0 + px < hw) ? in[(size_t)(k0 + k) * hw + p0 + px] : 0.0f;
    }
    for (int e = t; e < PW_TK * PW_TC; e += 64) {
      const int c = e / PW_TK, k = e % PW_TK;
      ws[k][c] = (k0 + k < ci && c0 + c < co) ? w[(size_t)(c0 + c) * ci + k0 + k] : 0.0f;
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    __attribute__((opencl_unroll_hint(8)))
    for (int k = 0; k < PW_TK; ++k) {
      const float4 x = vload4(0, &xs[k][tp]);
      const float4 wc = vload4(0, &ws[k][tc]);
      acc[0] = fma((float4)(wc.s0), x, acc[0]);
      acc[1] = fma((float4)(wc.s1), x, acc[1]);
      acc[2] = fma((float4)(wc.s2), x, acc[2]);
      acc[3] = fma((float4)(wc.s3), x, acc[3]);
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  for (int r = 0; r < 4; ++r) {
    const int c = c0 + tc + r;
    if (c >= co) break;
    const float bias = b[c];
    const float v[4] = {acc[r].s0, acc[r].s1, acc[r].s2, acc[r].s3};
    for (int j = 0; j < 4; ++j) {
      const int p = p0 + tp + j;
      if (p < hw) out[(size_t)c * hw + p] = activate(v[j] + bias, act, alpha, beta);
    }
  }
}

// The same for one pixel per channel (the squeeze-excite layers): one
// sub-group per output channel, lanes across the input channels.
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void pointwise_vector(__global const float* in, __global float* out, __global const float* w,
                      __global const float* b, int ci, int co, uint image_stride, int act, float alpha,
                      float beta) {
  const int c = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  const size_t image = get_global_id(1) * (size_t)image_stride;
  if (c >= co) return;
  float acc = 0.0f;
  for (int i = get_sub_group_local_id(); i < ci; i += 16) acc = fma(w[(size_t)c * ci + i], in[image + i], acc);
  acc = sub_group_reduce_add(acc);
  if (get_sub_group_local_id() == 0) out[image + c] = activate(acc + b[c], act, alpha, beta);
}

// Depthwise k x k: one output pixel of one channel per work item.
__kernel void depthwise(__global const float* in, __global float* out, __global const float* w,
                        __global const float* b, int channels, int ih, int iw, int oh, int ow, int k,
                        int stride, int pad, uint image_stride, int act, float alpha, float beta) {
  const int x = get_global_id(0), y = get_global_id(1);
  const int c = get_global_id(2) % channels;
  const size_t image = (get_global_id(2) / channels) * (size_t)image_stride;
  if (x >= ow || y >= oh) return;
  __global const float* plane = in + image + (size_t)c * ih * iw;
  __global const float* kw = w + (size_t)c * k * k;
  float acc = 0.0f;
  for (int ky = 0; ky < k; ++ky) {
    const int sy = y * stride - pad + ky;
    if (sy < 0 || sy >= ih) continue;
    for (int kx = 0; kx < k; ++kx) {
      const int sx = x * stride - pad + kx;
      if (sx >= 0 && sx < iw) acc = fma(kw[ky * k + kx], plane[sy * iw + sx], acc);
    }
  }
  out[image + ((size_t)c * oh + y) * ow + x] = activate(acc + b[c], act, alpha, beta);
}

// Dense k x k (the stem): one output pixel per work item, CONV_CO output
// channels at once, so each input tap is loaded once for all of them.
#define CONV_CO 16
__kernel void conv(__global const float* in, __global float* out, __global const float* w,
                   __global const float* b, int ci, int co, int ih, int iw, int oh, int ow, int k, int stride,
                   int pad, uint image_stride, int act, float alpha, float beta) {
  const int x = get_global_id(0), y = get_global_id(1);
  const int groups = (co + CONV_CO - 1) / CONV_CO;
  const int o0 = (get_global_id(2) % groups) * CONV_CO;
  const size_t image = (get_global_id(2) / groups) * (size_t)image_stride;
  if (x >= ow || y >= oh) return;
  float acc[CONV_CO];
  for (int o = 0; o < CONV_CO; ++o) acc[o] = 0.0f;
  for (int i = 0; i < ci; ++i) {
    __global const float* plane = in + image + (size_t)i * ih * iw;
    for (int ky = 0; ky < k; ++ky) {
      const int sy = y * stride - pad + ky;
      if (sy < 0 || sy >= ih) continue;
      for (int kx = 0; kx < k; ++kx) {
        const int sx = x * stride - pad + kx;
        if (sx < 0 || sx >= iw) continue;
        const float v = plane[sy * iw + sx];
        __attribute__((opencl_unroll_hint))
        for (int o = 0; o < CONV_CO; ++o) {
          if (o0 + o < co) acc[o] = fma(w[(((size_t)(o0 + o) * ci + i) * k + ky) * k + kx], v, acc[o]);
        }
      }
    }
  }
  for (int o = 0; o < CONV_CO && o0 + o < co; ++o) {
    out[image + ((size_t)(o0 + o) * oh + y) * ow + x] = activate(acc[o] + b[o0 + o], act, alpha, beta);
  }
}

// Mean over each channel's H x W: one sub-group per channel.
__kernel __attribute__((intel_reqd_sub_group_size(16)))
void global_pool(__global const float* in, __global float* out, int channels, int hw, uint image_stride) {
  const int c = get_group_id(0) * get_num_sub_groups() + get_sub_group_id();
  const size_t image = get_global_id(1) * (size_t)image_stride;
  if (c >= channels) return;
  float acc = 0.0f;
  for (int p = get_sub_group_local_id(); p < hw; p += 16) acc += in[image + (size_t)c * hw + p];
  acc = sub_group_reduce_add(acc);
  if (get_sub_group_local_id() == 0) out[image + c] = acc / (float)hw;
}

__kernel void scale(__global const float* in, __global const float* factor, __global float* out, int hw,
                    int count, uint image_stride) {
  const int at = get_global_id(0);
  const size_t image = get_global_id(1) * (size_t)image_stride;
  if (at < count) out[image + at] = in[image + at] * factor[image + at / hw];
}

// Nearest, asymmetric, floor: source = floor(destination / (out / in)).
__kernel void resize_nearest(__global const float* in, __global float* out, int channels, int ih, int iw,
                             int oh, int ow, uint image_stride) {
  const int x = get_global_id(0), y = get_global_id(1);
  const int c = get_global_id(2) % channels;
  const size_t image = (get_global_id(2) / channels) * (size_t)image_stride;
  if (x >= ow || y >= oh) return;
  const float sy = (float)oh / (float)ih, sx = (float)ow / (float)iw;
  const int iy = min((int)floor((float)y / sy), ih - 1), ix = min((int)floor((float)x / sx), iw - 1);
  out[image + ((size_t)c * oh + y) * ow + x] = in[image + ((size_t)c * ih + iy) * iw + ix];
}

// A concat input into its channel range: count floats from in to out.
__kernel void copy(__global const float* in, __global float* out, int count, uint image_stride) {
  const int at = get_global_id(0);
  const size_t image = get_global_id(1) * (size_t)image_stride;
  if (at < count) out[image + at] = in[image + at];
}

// [C][P] -> [P][C].
__kernel void transpose(__global const float* in, __global float* out, int channels, int positions,
                        uint image_stride, uint out_stride) {
  const int c = get_global_id(0), p = get_global_id(1);
  const size_t image = get_global_id(2);
  if (c < channels && p < positions) {
    out[image * out_stride + (size_t)p * channels + c] = in[image * image_stride + (size_t)c * positions + p];
  }
}
