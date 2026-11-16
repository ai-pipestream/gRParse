"""Converts docling-layout-heron (RT-DETRv2, PyTorch/HF checkpoint) to OpenVINO IR.

The IR keeps the interface of docling-layout-heron-onnx, which
src/layout_engine.cpp decodes: images u8 [1,3,640,640] (RGB, resized by the
caller), orig_target_sizes i64 [1,2] (width, height), and the top 300
(labels i64, boxes f32 xyxy in original pixels, scores f32) over sigmoid class
scores, as RT-DETR's own postprocessor emits them.  The rescale to [0,1] runs
inside the graph, so the GPU does it.

  python convert_heron.py <hf checkpoint dir> <out.xml> [--fp32]
"""
import sys

import openvino as ov
import torch
from transformers import RTDetrV2ForObjectDetection


class HeronWithPostprocess(torch.nn.Module):
    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, images, orig_target_sizes):
        pixels = images.to(torch.float32) * (1.0 / 255.0)
        out = self.model(pixel_values=pixels)
        logits, boxes = out.logits, out.pred_boxes  # [B,300,C], [B,300,4] cxcywh in [0,1]
        num_classes = logits.shape[-1]
        scores = torch.sigmoid(logits).flatten(1)
        top_scores, index = torch.topk(scores, 300, dim=-1)
        labels = index % num_classes
        query = index // num_classes
        cx, cy, w, h = boxes.unbind(-1)
        xyxy = torch.stack([cx - 0.5 * w, cy - 0.5 * h, cx + 0.5 * w, cy + 0.5 * h], dim=-1)
        size = orig_target_sizes.to(torch.float32).repeat(1, 2).unsqueeze(1)
        xyxy = xyxy * size
        picked = torch.gather(xyxy, 1, query.unsqueeze(-1).expand(-1, -1, 4))
        return labels, picked, top_scores


def main():
    source, target = sys.argv[1], sys.argv[2]
    fp16 = "--fp32" not in sys.argv[3:]
    model = RTDetrV2ForObjectDetection.from_pretrained(source).eval()
    wrapped = HeronWithPostprocess(model).eval()
    example = (torch.zeros(1, 3, 640, 640, dtype=torch.uint8), torch.tensor([[640, 640]], dtype=torch.int64))
    with torch.no_grad():
        ov_model = ov.convert_model(wrapped, example_input=example,
                                    input=[("images", [1, 3, 640, 640], ov.Type.u8),
                                           ("orig_target_sizes", [1, 2], ov.Type.i64)])
    for output, name in zip(ov_model.outputs, ["labels", "boxes", "scores"]):
        output.get_tensor().set_names({name})
    ov.save_model(ov_model, target, compress_to_fp16=fp16)


if __name__ == "__main__":
    main()
