"""Converts docling's own heron ONNX export to the OpenVINO IR the openvino flavor ships.

The IR keeps the export's dynamic shapes and FP32 weights: on the Arc Pro B70
(OpenVINO 2025.4.1, NEO 26.40) this graph is the one the GPU plugin computes
exactly at FP32 (every region of five ACM pages equal to ONNX Runtime CPU,
18.6 ms per page warm), while a static reshape of it fails to compile and
every FP16 build shifts boxes by up to 0.7 px, which moves table crops far
enough to change the table decoder's cells.  The PyTorch-checkpoint route
(convert_heron.py) is exact on OpenVINO's CPU device but miscomputes at FP32
on the B70, so it stays only for reference.

Source: docling-project/docling-layout-heron-onnx model.onnx, the
layout_heron.onnx in models/MANIFEST (sha256 59c81a3a...4922a).

  python convert_heron_docling.py <layout_heron.onnx> <out.xml>
"""
import sys

import openvino as ov


def main():
    # read_model (the runtime's ONNX frontend), not convert_model: this is
    # the graph that was checked exact on the B70, and it converts
    # byte-identically run to run.
    model = ov.Core().read_model(sys.argv[1])
    ov.save_model(model, sys.argv[2], compress_to_fp16=False)


if __name__ == "__main__":
    main()
