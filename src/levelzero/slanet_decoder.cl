// SLANet+ structure decoder: every decode step of a table on the GPU in one
// launch.  M cooperating work-groups per table split each phase's outputs and
// exchange them through global memory behind a device-scope barrier; the cheap
// phases run redundantly in every group, so a step costs four grid barriers.
// Layouts: weights output-major ([out][in]), w_ih rows padded to WIS.
#define P 256
#define C 96
#define H 256
#define V 50
#define G 768
#define MAX_STEPS 501
#define L 1024
#define SG 16
#define NSG (L / SG)
#define WIS 160

typedef struct { float q[H], gh[G], e[P], gi[G], s1[H], l1[H]; } exchange;

static inline float logistic(float x) { return 1.0f / (1.0f + exp(-x)); }
// The attention energy takes 65536 tanh a step, the decoder's hottest loop:
// there tanh is built on the hardware exp, within 1e-4 of the precise one in
// the logits (the narrowest top-two margin on acm-table is 2e-3).  The GRU's
// 256 a step stay precise.
static inline float4 fast_tanh4(float4 x) { return 1.0f - 2.0f * native_recip(native_exp(2.0f * x) + 1.0f); }

// Every group arrives, then waits for all of a table's groups: the release on
// the arrival publishes this group's exchange writes, the acquire on the wait
// makes the others' visible, and both happen in one work-item behind
// work-group barriers.
static inline void grid_sync(__global atomic_uint* counter, uint* epoch, uint groups) {
  work_group_barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);
  ++*epoch;
  if (get_local_id(0) == 0) {
    atomic_fetch_add_explicit(counter, 1u, memory_order_release, memory_scope_device);
    while (atomic_load_explicit(counter, memory_order_acquire, memory_scope_device) < *epoch * groups) {}
  }
  work_group_barrier(CLK_LOCAL_MEM_FENCE | CLK_GLOBAL_MEM_FENCE);
}

// Sub-group dot of a local vector with a global row, n a multiple of 64.
static inline float sg_dot(__local const float* v, __global const float* row, int n, int lane) {
  float a = 0.0f;
  for (int i = lane * 4; i < n; i += SG * 4) a += dot(vload4(0, v + i), vload4(0, row + i));
  return sub_group_reduce_add(a);
}

__kernel __attribute__((reqd_work_group_size(L, 1, 1))) __attribute__((intel_reqd_sub_group_size(SG)))
void slanet_decode(__global const float* features, __global const float* i2hT, __global const float* h2hT,
                   __global const float* h2h_b, __global const float* score, __global const float* wihT,
                   __global const float* whhT, __global const float* b_ih, __global const float* b_hh,
                   __global const float* s1T, __global const float* s1_b, __global const float* s2T,
                   __global const float* s2_b, __global const float* l1T, __global const float* l1_b,
                   __global const float* l2T, __global const float* l2_b, __global float* keys,
                   __global exchange* xch, __global atomic_uint* barriers, __global float* logits,
                   __global float* location, __global int* steps, uint M) {
  __local float hid[H], qv[H], al[P], ctx[C], gi[G], gh[G], s1[H], l1[H], lg[V];
  __local int prev;
  const int grp = get_group_id(0), t = grp / M, m = grp % M, x = get_local_id(0);
  const int lane = get_sub_group_local_id(), sgi = get_sub_group_id();
  const int gsg = m * NSG + sgi, NG = M * NSG;
  __global const float* F = features + (size_t)t * P * C;
  __global float* K = keys + (size_t)t * (P * H + C * P);
  __global float* FT = K + P * H;
  __global exchange* xc = xch + t;
  __global atomic_uint* counter = barriers + t * 32;
  uint epoch = 0;
  for (int e = m * L + x; e < P * H; e += M * L) {
    const int p = e / H, j = e % H;
    float a = 0.0f;
    for (int c = 0; c < C; c += 4) a += dot(vload4(0, F + p * C + c), vload4(0, i2hT + j * C + c));
    K[e] = a;
  }
  for (int e = m * L + x; e < C * P; e += M * L) FT[e] = F[(e % P) * C + e / P];
  if (x < H) hid[x] = 0.0f;
  if (x == 0) prev = 0;
  grid_sync(counter, &epoch, M);
  int n = 0;
  for (; n < MAX_STEPS; ++n) {
    for (int o = gsg; o < H + G; o += NG) {  // q and gh: both read only hid
      const bool isq = o < H;
      const int oo = isq ? o : o - H;
      const float a = sg_dot(hid, (isq ? h2hT : whhT) + oo * H, H, lane);
      if (lane == 0) { if (isq) xc->q[oo] = a + h2h_b[oo]; else xc->gh[oo] = a + b_hh[oo]; }
    }
    grid_sync(counter, &epoch, M);
    if (x < H) qv[x] = xc->q[x];
    for (int i = x; i < G; i += L) gh[i] = xc->gh[i];
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    for (int p = gsg; p < P; p += NG) {  // attention energy
      float a = 0.0f;
      for (int j = lane * 4; j < H; j += SG * 4) {
        const float4 k = vload4(0, K + p * H + j), q4 = vload4(0, qv + j), s4 = vload4(0, score + j);
        a += dot(fast_tanh4(k + q4), s4);
      }
      a = sub_group_reduce_add(a);
      if (lane == 0) xc->e[p] = a;
    }
    grid_sync(counter, &epoch, M);
    {
      const float e = x < P ? xc->e[x] : -INFINITY;
      const float mx = work_group_reduce_max(e);
      const float ex = x < P ? exp(e - mx) : 0.0f;
      const float s = work_group_reduce_add(ex);
      if (x < P) al[x] = ex / s;
    }
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    for (int c = sgi; c < C; c += NSG) {
      const float a = sg_dot(al, FT + c * P, P, lane);
      if (lane == 0) ctx[c] = a;
    }
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    const int pv = prev;
    for (int o = gsg; o < G; o += NG) {
      __global const float* wi = wihT + o * WIS;
      float a = 0.0f;
      for (int i = lane * 4; i < C; i += SG * 4) a += dot(vload4(0, ctx + i), vload4(0, wi + i));
      a = sub_group_reduce_add(a);
      if (lane == 0) xc->gi[o] = a + wi[C + pv] + b_ih[o];
    }
    grid_sync(counter, &epoch, M);
    for (int i = x; i < G; i += L) gi[i] = xc->gi[i];
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    float hn = 0.0f;
    if (x < H) {
      const float r = logistic(gi[x] + gh[x]), z = logistic(gi[H + x] + gh[H + x]);
      const float nn = tanh(gi[2 * H + x] + r * gh[2 * H + x]);
      hn = (1.0f - z) * nn + z * hid[x];
    }
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    if (x < H) hid[x] = hn;
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    for (int o = gsg; o < 2 * H; o += NG) {
      const bool iss = o < H;
      const int oo = iss ? o : o - H;
      const float a = sg_dot(hid, (iss ? s1T : l1T) + oo * H, H, lane);
      if (lane == 0) { if (iss) xc->s1[oo] = a + s1_b[oo]; else xc->l1[oo] = a + l1_b[oo]; }
    }
    grid_sync(counter, &epoch, M);
    if (x < H) { s1[x] = xc->s1[x]; l1[x] = xc->l1[x]; }
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    for (int o = sgi; o < V + 8; o += NSG) {
      const bool iss = o < V;
      const int oo = iss ? o : o - V;
      const float a = sg_dot(iss ? s1 : l1, (iss ? s2T : l2T) + oo * H, H, lane);
      if (lane == 0) {
        if (iss) {
          lg[oo] = a + s2_b[oo];
          if (m == 0) logits[((size_t)t * MAX_STEPS + n) * V + oo] = lg[oo];
        } else if (m == 0) {
          location[((size_t)t * MAX_STEPS + n) * 8 + oo] = logistic(a + l2_b[oo]);
        }
      }
    }
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    if (sgi == 0) {  // first maximum, as the host decode takes it
      float bv = -INFINITY;
      int bi = V;
      for (int i = lane; i < V; i += SG) if (lg[i] > bv) { bv = lg[i]; bi = i; }
      const float mx = sub_group_reduce_max(bv);
      const int first = sub_group_reduce_min(bv == mx ? bi : V);
      if (lane == 0) prev = first;
    }
    work_group_barrier(CLK_LOCAL_MEM_FENCE);
    if (n > 0 && prev == V - 1) { ++n; break; }
  }
  if (x == 0 && m == 0) steps[t] = n;
}
