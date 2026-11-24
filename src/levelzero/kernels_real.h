// The element type of activation buffers, prepended to every kernel file.
// Activations are float, or half when built with -DGRAPH_FP16; arithmetic is
// always in float, rounded once at the store.
#ifdef GRAPH_FP16
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
typedef half real;
#define RLOAD(p, i) vload_half((i), (p))
#define RSTORE(p, i, v) vstore_half((v), (i), (p))
#else
typedef float real;
#define RLOAD(p, i) ((p)[(i)])
#define RSTORE(p, i, v) ((p)[(i)] = (v))
#endif
