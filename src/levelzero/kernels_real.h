// Prepended to every kernel file of the Level Zero graph runtime at build.
// Activations on the device are `real`: float in the FP32 module, half in
// the FP16 one (-DGRAPH_FP16).  Arithmetic is always float: load with RLOAD,
// compute, round once at RSTORE.  Constants a kernel takes as parameters
// (convolution weights, biases, LayerNorm scales) are float in both.
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#ifdef GRAPH_FP16
typedef half real;
#define RLOAD(p, i) vload_half((i), (p))
#define RSTORE(p, i, v) vstore_half((v), (i), (p))
#else
typedef float real;
#define RLOAD(p, i) ((p)[(i)])
#define RSTORE(p, i, v) ((p)[(i)] = (v))
#endif
