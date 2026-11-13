"""Pin the dynamic RapidOCR inputs to the fixed shapes a Hailo HEF needs.

Usage: python fix_shapes.py MODELS_DIR OUT_DIR

Each variant is the upstream ONNX (verified by models/MANIFEST hash) with its
input overwritten to a static shape and simplified by onnxsim, so the Hailo
parser sees no dynamic dimension.
"""
import hashlib
import sys
from pathlib import Path

import onnx
from onnxsim import simplify

VARIANTS = {
    "det_960": ("ch_PP-OCRv3_det_infer.onnx", [1, 3, 960, 960]),
    "det_640": ("ch_PP-OCRv3_det_infer.onnx", [1, 3, 640, 640]),
    "rec_48x320": ("ch_PP-OCRv3_rec_infer.onnx", [1, 3, 48, 320]),
    "rec_48x640": ("ch_PP-OCRv3_rec_infer.onnx", [1, 3, 48, 640]),
    "cls_48x192": ("ch_ppocr_mobile_v2.0_cls_infer.onnx", [1, 3, 48, 192]),
}


def main() -> None:
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for name, (file, shape) in VARIANTS.items():
        model = onnx.load(src / file)
        inp = model.graph.input[0].name
        simplified, ok = simplify(model, overwrite_input_shapes={inp: shape})
        if not ok:
            raise SystemExit(f"{name}: onnxsim check failed")
        path = out / f"{name}.onnx"
        onnx.save(simplified, path)
        outs = [(o.name, [d.dim_value for d in o.type.tensor_type.shape.dim]) for o in simplified.graph.output]
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        print(f"{name}: input {inp} {shape} -> outputs {outs} nodes={len(simplified.graph.node)} sha256={digest}")


if __name__ == "__main__":
    main()
