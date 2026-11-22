#!/usr/bin/env python3
"""Split SLANet+ (table structure) into an encoder and one decoder step.

The released model decodes up to 501 structure tokens in one ONNX Loop whose
carried tensors grow each step.  TensorRT's ONNX parser rejects that loop
(a dynamic-rank Concat inside it), and so does the OpenVINO GPU plugin.  This
script writes the same network as two static-shape graphs that the server
drives itself (src/table_structure_engine.cpp), one decoder step per token:

  slanet_plus_encoder.onnx  x f32 [1,3,488,488] -> features f32 [1,256,96]
      the backbone and neck, cut from the released graph just before the loop
  slanet_plus_step.onnx     features [1,256,96], hidden [1,256],
                            previous [1,50] (one-hot of the previous token)
                            -> next_hidden [1,256], structure [1,50] (logits),
                               location [1,8] (sigmoid corner boxes)
      PaddleOCR's SLAHead step (attention, GRU cell, the structure and
      location heads) rebuilt node by node over the released graph's own
      weights (linear_0..6, gru_cell_0.*; Paddle Linear weights are
      [in, out], GRU gates are ordered r, z, n)
  slanet_plus.vocab         the token vocabulary, one token per line, taken
                            from the released model's "character" metadata
                            (the two graphs carry no metadata a native
                            runtime reads)

Unless --no-verify is given, the split is then proved against the released
model on ONNX Runtime's CPU provider: the same random input must produce the
same token at every step and probabilities and boxes within 1e-4.

    python scripts/split_slanet.py models/slanet_plus.onnx models/

Requires onnx and numpy; the verification also needs onnxruntime.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

INPUT_SIDE = 488
POSITIONS = 256
CHANNELS = 96
HIDDEN = 256
MAX_STEPS = 501
ENCODER_OUTPUT = "transpose_0.tmp_0.0"
WEIGHTS = (
    "linear_0.w_0", "linear_1.w_0", "linear_1.b_0", "linear_2.w_0",
    "gru_cell_0.w_0", "gru_cell_0.w_1", "gru_cell_0.b_0", "gru_cell_0.b_1",
    "linear_3.w_0", "linear_3.b_0", "linear_4.w_0", "linear_4.b_0",
    "linear_5.w_0", "linear_5.b_0", "linear_6.w_0", "linear_6.b_0",
)


def released_weights(model: onnx.ModelProto) -> dict[str, np.ndarray]:
    """The decoder's weights, wherever the export put them."""
    found: dict[str, np.ndarray] = {}

    def visit(graph: onnx.GraphProto) -> None:
        for tensor in graph.initializer:
            if tensor.name in WEIGHTS:
                found[tensor.name] = numpy_helper.to_array(tensor)
        for node in graph.node:
            for attribute in node.attribute:
                if node.op_type == "Constant" and attribute.name == "value":
                    if node.output[0] in WEIGHTS:
                        found[node.output[0]] = numpy_helper.to_array(attribute.t)
                if attribute.type == onnx.AttributeProto.GRAPH:
                    visit(attribute.g)

    visit(model.graph)
    missing = [name for name in WEIGHTS if name not in found]
    if missing:
        raise SystemExit(f"split_slanet: the model lacks decoder weights {missing}")
    return found


def write_encoder(source: Path, target: Path) -> None:
    onnx.utils.extract_model(str(source), str(target), ["x"], [ENCODER_OUTPUT])
    encoder = onnx.load(str(target))
    for value, shape in ((encoder.graph.input[0], (1, 3, INPUT_SIDE, INPUT_SIDE)),
                         (encoder.graph.output[0], (1, POSITIONS, CHANNELS))):
        dims = value.type.tensor_type.shape.dim
        del dims[:]
        for size in shape:
            dims.add().dim_value = size
    del encoder.graph.value_info[:]
    del encoder.metadata_props[:]
    onnx.save(encoder, str(target))


def step_model(weights: dict[str, np.ndarray], vocab_size: int) -> onnx.ModelProto:
    """One SLAHead decode step as a static graph."""
    constants = {
        "i2h": weights["linear_0.w_0"], "h2h_w": weights["linear_1.w_0"],
        "h2h_b": weights["linear_1.b_0"], "score": weights["linear_2.w_0"],
        "w_ih": weights["gru_cell_0.w_0"].T, "w_hh": weights["gru_cell_0.w_1"].T,
        "b_ih": weights["gru_cell_0.b_0"], "b_hh": weights["gru_cell_0.b_1"],
        "s1_w": weights["linear_3.w_0"], "s1_b": weights["linear_3.b_0"],
        "s2_w": weights["linear_4.w_0"], "s2_b": weights["linear_4.b_0"],
        "l1_w": weights["linear_5.w_0"], "l1_b": weights["linear_5.b_0"],
        "l2_w": weights["linear_6.w_0"], "l2_b": weights["linear_6.b_0"],
        "one": np.array(1.0, dtype=np.float32),
        "query_shape": np.array([1, 1, -1], dtype=np.int64),
        "context_shape": np.array([1, CHANNELS], dtype=np.int64),
        "gates": np.array([HIDDEN, HIDDEN, HIDDEN], dtype=np.int64),
    }
    initializers = [
        numpy_helper.from_array(np.ascontiguousarray(value), name)
        for name, value in constants.items()
    ]
    node = helper.make_node
    nodes = [
        # Attention over the 256 positions, queried by the hidden state.
        node("MatMul", ["features", "i2h"], ["keys"]),
        node("MatMul", ["hidden", "h2h_w"], ["query_raw"]),
        node("Add", ["query_raw", "h2h_b"], ["query_flat"]),
        node("Reshape", ["query_flat", "query_shape"], ["query"]),
        node("Add", ["keys", "query"], ["energy_in"]),
        node("Tanh", ["energy_in"], ["energy_tanh"]),
        node("MatMul", ["energy_tanh", "score"], ["energy"]),
        node("Softmax", ["energy"], ["alpha"], axis=1),
        node("Transpose", ["alpha"], ["alpha_row"], perm=[0, 2, 1]),
        node("MatMul", ["alpha_row", "features"], ["context_row"]),
        node("Reshape", ["context_row", "context_shape"], ["context"]),
        node("Concat", ["context", "previous"], ["cell_in"], axis=1),
        # GRU cell, gates r, z, n.
        node("MatMul", ["cell_in", "w_ih"], ["gi_raw"]),
        node("Add", ["gi_raw", "b_ih"], ["gi"]),
        node("MatMul", ["hidden", "w_hh"], ["gh_raw"]),
        node("Add", ["gh_raw", "b_hh"], ["gh"]),
        node("Split", ["gi", "gates"], ["gi_r", "gi_z", "gi_n"], axis=1),
        node("Split", ["gh", "gates"], ["gh_r", "gh_z", "gh_n"], axis=1),
        node("Add", ["gi_r", "gh_r"], ["r_in"]),
        node("Sigmoid", ["r_in"], ["r"]),
        node("Add", ["gi_z", "gh_z"], ["z_in"]),
        node("Sigmoid", ["z_in"], ["z"]),
        node("Mul", ["r", "gh_n"], ["n_gate"]),
        node("Add", ["gi_n", "n_gate"], ["n_in"]),
        node("Tanh", ["n_in"], ["n"]),
        node("Sub", ["one", "z"], ["keep"]),
        node("Mul", ["keep", "n"], ["fresh"]),
        node("Mul", ["z", "hidden"], ["carried"]),
        node("Add", ["fresh", "carried"], ["next_hidden"]),
        # Structure and location heads.
        node("MatMul", ["next_hidden", "s1_w"], ["s1_raw"]),
        node("Add", ["s1_raw", "s1_b"], ["s1"]),
        node("MatMul", ["s1", "s2_w"], ["s2_raw"]),
        node("Add", ["s2_raw", "s2_b"], ["structure"]),
        node("MatMul", ["next_hidden", "l1_w"], ["l1_raw"]),
        node("Add", ["l1_raw", "l1_b"], ["l1"]),
        node("MatMul", ["l1", "l2_w"], ["l2_raw"]),
        node("Add", ["l2_raw", "l2_b"], ["l2"]),
        node("Sigmoid", ["l2"], ["location"]),
    ]
    value = helper.make_tensor_value_info
    graph = helper.make_graph(
        nodes, "slanet_plus_step",
        [value("features", TensorProto.FLOAT, [1, POSITIONS, CHANNELS]),
         value("hidden", TensorProto.FLOAT, [1, HIDDEN]),
         value("previous", TensorProto.FLOAT, [1, vocab_size])],
        [value("next_hidden", TensorProto.FLOAT, [1, HIDDEN]),
         value("structure", TensorProto.FLOAT, [1, vocab_size]),
         value("location", TensorProto.FLOAT, [1, 8])],
        initializers)
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 13)],
                              producer_name="grparse-split-slanet")
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def decode(encoder, step, image: np.ndarray, vocab_size: int):
    """Drives the split the way the server does; returns (probs, boxes)."""
    features = encoder.run(None, {"x": image})[0]
    hidden = np.zeros((1, HIDDEN), np.float32)
    previous = np.zeros((1, vocab_size), np.float32)
    previous[0, 0] = 1.0  # sos
    probs, boxes = [], []
    for index in range(MAX_STEPS):
        hidden, structure, location = step.run(
            None, {"features": features, "hidden": hidden, "previous": previous})
        shifted = np.exp(structure[0] - structure[0].max())
        probs.append(shifted / shifted.sum())
        boxes.append(location[0])
        best = int(structure[0].argmax())
        previous = np.zeros((1, vocab_size), np.float32)
        previous[0, best] = 1.0
        if index > 0 and best == vocab_size - 1:  # eos
            break
    return np.stack(probs), np.stack(boxes)


def verify(source: Path, encoder_path: Path, step_path: Path, vocab_size: int) -> float:
    import onnxruntime as ort

    options = ort.SessionOptions()
    options.log_severity_level = 3
    providers = ["CPUExecutionProvider"]
    released = ort.InferenceSession(str(source), options, providers=providers)
    encoder = ort.InferenceSession(str(encoder_path), options, providers=providers)
    step = ort.InferenceSession(str(step_path), options, providers=providers)
    image = np.random.default_rng(364).standard_normal(
        (1, 3, INPUT_SIDE, INPUT_SIDE)).astype(np.float32)
    first, second = released.run(None, {"x": image})
    want_boxes, want_probs = (first, second) if first.shape[-1] == 8 else (second, first)
    probs, boxes = decode(encoder, step, image, vocab_size)
    steps = probs.shape[0]
    want_tokens = want_probs[0, :steps].argmax(-1)
    if not np.array_equal(probs.argmax(-1), want_tokens):
        raise SystemExit("split_slanet: the split decodes different tokens than the model")
    worst = max(float(np.abs(probs - want_probs[0, :steps]).max()),
                float(np.abs(boxes - want_boxes[0, :steps]).max()))
    if worst > 1e-4:
        raise SystemExit(f"split_slanet: the split differs from the model by {worst:.2e}")
    return worst


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("source", type=Path, help="the released slanet_plus.onnx")
    parser.add_argument("target", type=Path, help="directory the three files are written to")
    parser.add_argument("--no-verify", action="store_true",
                        help="skip the ONNX Runtime comparison with the released model")
    arguments = parser.parse_args()
    arguments.target.mkdir(parents=True, exist_ok=True)
    model = onnx.load(str(arguments.source))
    metadata = {entry.key: entry.value for entry in model.metadata_props}
    if "character" not in metadata:
        raise SystemExit("split_slanet: the model metadata lacks its token vocabulary")
    weights = released_weights(model)
    vocab_size = int(weights["linear_4.b_0"].shape[0])  # the structure head's width
    encoder_path = arguments.target / "slanet_plus_encoder.onnx"
    step_path = arguments.target / "slanet_plus_step.onnx"
    write_encoder(arguments.source, encoder_path)
    onnx.save(step_model(weights, vocab_size), str(step_path))
    (arguments.target / "slanet_plus.vocab").write_text(metadata["character"], encoding="utf-8")
    if arguments.no_verify:
        print("split_slanet: written, not verified")
        return 0
    try:
        worst = verify(arguments.source, encoder_path, step_path, vocab_size)
    except ImportError:
        print("split_slanet: onnxruntime is not installed; split written, not verified",
              file=sys.stderr)
        return 0
    print(f"split_slanet: same tokens as the released model, max difference {worst:.2e}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
