"""Converts DocumentFigureClassifier-v2.5 (EfficientNet, PyTorch/HF checkpoint) to OpenVINO IR.

Interface as the patched model.onnx src/figure_classifier.cpp feeds: input
f32 [N,3,224,224] normalized by the caller, output f32 [N,26] logits
(the caller applies the softmax).  The checkpoint's pooler is
AvgPool2d(kernel_size=1280, ceil_mode=True) over a 7x7 map, the same bug the
ONNX needed scripts/patch_figure_classifier.py for: PyTorch reads it as a
global average, OpenVINO's shape inference rejects it.  It is swapped for
AdaptiveAvgPool2d(1), the global average it computes, before conversion.

  python convert_figure_classifier.py <hf checkpoint dir> <out.xml>
"""
import sys

import openvino as ov
import torch
from transformers import EfficientNetForImageClassification


class Logits(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, pixel_values):
        return self.model(pixel_values=pixel_values).logits


def main():
    source, target = sys.argv[1], sys.argv[2]
    network = EfficientNetForImageClassification.from_pretrained(source).eval()
    probe = torch.rand(2, 3, 224, 224)
    reference = network(pixel_values=probe).logits
    network.efficientnet.pooler = torch.nn.AdaptiveAvgPool2d(1)
    if not torch.allclose(reference, network(pixel_values=probe).logits, atol=1e-5):
        raise SystemExit("the pooler swap changed the logits")
    model = Logits(network).eval()
    with torch.no_grad():
        ov_model = ov.convert_model(model, example_input=torch.zeros(1, 3, 224, 224),
                                    input=[([-1, 3, 224, 224], ov.Type.f32)])
    ov_model.inputs[0].get_tensor().set_names({"input"})
    ov_model.outputs[0].get_tensor().set_names({"output"})
    ov.save_model(ov_model, target, compress_to_fp16=True)


if __name__ == "__main__":
    main()
