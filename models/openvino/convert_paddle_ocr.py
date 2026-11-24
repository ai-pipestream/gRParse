"""Converts PaddleOCR's official PP-OCRv3 inference models to OpenVINO IR.

OpenVINO's Paddle frontend reads inference.pdmodel/.pdiparams directly, so no
ONNX step sits between the original weights and the IR.  Weights are stored
as FP16 (compress_to_fp16); shapes stay dynamic in the IR and gRParse
reshapes them to its static buckets at load (src/inference_model.cpp).

  python convert_paddle_ocr.py <dir with the three extracted tarballs> <out dir>

Sources (sha256 of the tarballs as downloaded 2026-11-16):
  https://paddleocr.bj.bcebos.com/PP-OCRv3/chinese/ch_PP-OCRv3_det_infer.tar
    8682b39d9b9edb03cb91cf0b0fa8bc2dabf87bf941a88049efc97e62328f1892
  https://paddleocr.bj.bcebos.com/PP-OCRv3/chinese/ch_PP-OCRv3_rec_infer.tar
    f92b13dc91c92e0f220d095412695c429f7e70907467d2f780f339eeb83bb7b1
  https://paddleocr.bj.bcebos.com/dygraph_v2.0/ch/ch_ppocr_mobile_v2.0_cls_infer.tar
    507352585040d035da3b1e6374694ad679a850acb0a36a8d0d47984176357717
"""
import sys
from pathlib import Path

import openvino as ov

NETS = ["ch_PP-OCRv3_det_infer", "ch_PP-OCRv3_rec_infer", "ch_ppocr_mobile_v2.0_cls_infer"]


def main():
    source, target = Path(sys.argv[1]), Path(sys.argv[2])
    target.mkdir(parents=True, exist_ok=True)
    for net in NETS:
        model = ov.convert_model(str(source / net / "inference.pdmodel"))
        ov.save_model(model, str(target / f"{net}.xml"), compress_to_fp16=True)


if __name__ == "__main__":
    main()
