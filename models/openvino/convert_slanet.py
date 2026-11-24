"""Splits SLANet+ (table structure) into two static-shape OpenVINO IRs.

The released model decodes up to 501 structure tokens in one dynamic Loop
whose carried tensors grow each step, a dynamic rank the OpenVINO GPU plugin
rejects (#344).  This script turns it into two IRs that the C++ drives
(src/table_structure_engine.cpp):

  slanet_plus_encoder: x f32 [1,3,488,488] -> features f32 [1,256,96]
      (backbone + neck, cut from the graph just before the loop)
  slanet_plus_step:    features [1,256,96], hidden [1,256], previous [1,50]
      (one-hot of the previous token) -> hidden [1,256], structure [1,50]
      (logits), location [1,8] (sigmoid boxes)

The step is PaddleOCR's SLAHead decode (AttentionGRUCell + the structure and
location generators), rebuilt in PyTorch over the graph's own weights
(linear_0..6, gru_cell_0.*; Paddle Linear weights are [in, out], GRU gates
r, z, n).  Checked against ONNX Runtime running the whole model: max abs
difference 5e-6 on the structure probabilities, 7e-7 on the boxes, argmax
identical on every step.

Source: PaddleX's official SLANet_plus_infer.tar (paddle3.0.0, sha256
68526c5c6573a8a17cdc14805e026f11c2545e42d89a1317bb58d3e1e4b58318) ships
PIR inference.json, which OpenVINO 2025.4's Paddle frontend cannot read, so
the encoder comes from its paddle2onnx export, the slanet_plus.onnx in
models/MANIFEST (sha256 d57a942a...514b), as the last resort.

The token vocabulary (the ONNX metadata's "character" entry, one token per
line) is written beside them as slanet_plus.vocab, since the IR carries no
model metadata the C API reads.

  python convert_slanet.py <slanet_plus.onnx> <out dir>
"""
import sys
from pathlib import Path

import numpy as np
import onnx
import openvino as ov
import torch
from onnx import numpy_helper, utils


def constants(path):
    weights = {}
    for node in onnx.load(path).graph.node:
        if node.op_type == "Constant":
            for attribute in node.attribute:
                if attribute.name == "value":
                    weights[node.output[0]] = numpy_helper.to_array(attribute.t)
    return weights


class Step(torch.nn.Module):
    def __init__(self, w):
        super().__init__()
        t = lambda name: torch.nn.Parameter(torch.from_numpy(np.ascontiguousarray(w[name])), requires_grad=False)
        self.i2h, self.h2h_w, self.h2h_b, self.score = t("linear_0.w_0"), t("linear_1.w_0"), t("linear_1.b_0"), t("linear_2.w_0")
        self.w_ih, self.w_hh, self.b_ih, self.b_hh = t("gru_cell_0.w_0"), t("gru_cell_0.w_1"), t("gru_cell_0.b_0"), t("gru_cell_0.b_1")
        self.s1_w, self.s1_b, self.s2_w, self.s2_b = t("linear_3.w_0"), t("linear_3.b_0"), t("linear_4.w_0"), t("linear_4.b_0")
        self.l1_w, self.l1_b, self.l2_w, self.l2_b = t("linear_5.w_0"), t("linear_5.b_0"), t("linear_6.w_0"), t("linear_6.b_0")

    def forward(self, features, hidden, previous):
        energy = torch.tanh(features @ self.i2h + (hidden @ self.h2h_w + self.h2h_b).unsqueeze(1)) @ self.score
        alpha = torch.softmax(energy, dim=1)
        context = (alpha.transpose(1, 2) @ features).squeeze(1)
        x = torch.cat([context, previous], dim=1)
        gi = x @ self.w_ih.T + self.b_ih
        gh = hidden @ self.w_hh.T + self.b_hh
        r = torch.sigmoid(gi[:, :256] + gh[:, :256])
        z = torch.sigmoid(gi[:, 256:512] + gh[:, 256:512])
        n = torch.tanh(gi[:, 512:] + r * gh[:, 512:])
        hidden = (1 - z) * n + z * hidden
        structure = (hidden @ self.s1_w + self.s1_b) @ self.s2_w + self.s2_b
        location = torch.sigmoid((hidden @ self.l1_w + self.l1_b) @ self.l2_w + self.l2_b)
        return hidden, structure, location


def main():
    source, target = sys.argv[1], Path(sys.argv[2])
    target.mkdir(parents=True, exist_ok=True)
    encoder_onnx = target / "slanet_plus_encoder.onnx"
    utils.extract_model(source, str(encoder_onnx), ["x"], ["transpose_0.tmp_0.0"])
    encoder = ov.convert_model(str(encoder_onnx), input=[("x", [1, 3, 488, 488], ov.Type.f32)])
    encoder.outputs[0].get_tensor().set_names({"features"})
    ov.save_model(encoder, str(target / "slanet_plus_encoder.xml"), compress_to_fp16=True)
    encoder_onnx.unlink()

    step = Step(constants(source)).eval()
    example = (torch.zeros(1, 256, 96), torch.zeros(1, 256), torch.zeros(1, 50))
    with torch.no_grad():
        model = ov.convert_model(step, example_input=example,
                                 input=[("features", [1, 256, 96], ov.Type.f32),
                                        ("hidden", [1, 256], ov.Type.f32),
                                        ("previous", [1, 50], ov.Type.f32)])
    for output, name in zip(model.outputs, ["next_hidden", "structure", "location"]):
        output.get_tensor().set_names({name})
    ov.save_model(model, str(target / "slanet_plus_step.xml"), compress_to_fp16=True)

    metadata = {entry.key: entry.value for entry in onnx.load(source).metadata_props}
    (target / "slanet_plus.vocab").write_text(metadata["character"])


if __name__ == "__main__":
    main()
