#!/usr/bin/env python3
"""Export ds4sd/DocumentFigureClassifier (MIT) to ONNX for gRParse.

The upstream repository publishes safetensors only; gRParse runs ONNX
Runtime natively, so the classifier is exported once with this script and
the result is placed at models/figure_classifier.onnx. Requires:

    pip install torch torchvision --index-url https://download.pytorch.org/whl/cpu
    pip install transformers onnx onnxscript

The transformers EfficientNet pooler is nn.AvgPool2d(kernel_size=hidden_dim)
whose oversized window only works through eager-mode clipping; exported to
ONNX AveragePool it divides by the full window and collapses the features.
The wrapper below therefore composes the layers directly and pools with an
explicit spatial mean, which matches the eager model to float precision.

Preprocessing contract (per docling-ibm-models): RGB, resize 224x224,
scale 1/255, mean [0.485, 0.456, 0.406], std [0.47853944, 0.4732864,
0.47434163]. Class order is the model's id2label order, pinned in
src/figure_classifier.cpp.
"""

import sys

import torch
from transformers import AutoConfig, AutoModelForImageClassification

MODEL_ID = "ds4sd/DocumentFigureClassifier"


class Plain(torch.nn.Module):
    def __init__(self, model: torch.nn.Module):
        super().__init__()
        self.embeddings = model.efficientnet.embeddings
        self.blocks = model.efficientnet.encoder.blocks
        self.top_conv = model.efficientnet.encoder.top_conv
        self.top_bn = model.efficientnet.encoder.top_bn
        self.top_activation = model.efficientnet.encoder.top_activation
        self.classifier = model.classifier

    def forward(self, pixel_values: torch.Tensor) -> torch.Tensor:
        hidden = self.embeddings(pixel_values)
        for block in self.blocks:
            hidden = block(hidden)
        hidden = self.top_activation(self.top_bn(self.top_conv(hidden)))
        return self.classifier(hidden.mean(dim=(2, 3)))


def main() -> int:
    output = sys.argv[1] if len(sys.argv) > 1 else "figure_classifier.onnx"
    config = AutoConfig.from_pretrained(MODEL_ID)
    print("class order:", [config.id2label[i] for i in sorted(config.id2label)])

    model = AutoModelForImageClassification.from_pretrained(MODEL_ID).eval()
    wrapper = Plain(model).eval()
    dummy = torch.zeros(1, 3, 224, 224)
    with torch.no_grad():
        reference = model(pixel_values=dummy).logits
        composed = wrapper(dummy)
        assert torch.allclose(reference, composed, atol=1e-5), "wrapper diverged from model"

    exported = torch.onnx.export(
        wrapper, (dummy,), dynamo=True,
        input_names=["pixel_values"], output_names=["logits"],
    )
    exported.save(output)
    print("wrote", output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
