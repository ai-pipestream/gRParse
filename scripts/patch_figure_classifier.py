#!/usr/bin/env python3
"""Correct the pooling node in the published figure classifier export.

The upstream ONNX exports the final global average pool as an ``AveragePool``
whose ``kernel_shape`` is the channel count (``[1280, 1280]``) rather than the
spatial extent of its input (``[7, 7]``).  ONNX Runtime's CPU provider clamps
that and produces the right answer anyway; the OpenVINO execution provider
rejects the graph outright and the session never builds.

The correction is lossless: an ``AveragePool`` over the whole spatial extent is
exactly ``GlobalAveragePool``, so the node is swapped for one and nothing else
in the graph moves.  The script proves that by scoring both graphs on the same
random input and reporting the maximum absolute difference, which must be 0.

    python scripts/patch_figure_classifier.py \
        models/figure_classifier_upstream.onnx models/figure_classifier.onnx

Requires ``onnx``; the verification pass additionally requires
``onnxruntime`` and is skipped with a warning when it is not installed.
"""

from __future__ import annotations

import argparse
import sys

import onnx

POOL_NODE = "node_avg_pool2d"
PATCHED_NODE = "node_global_avg_pool"


def patch(model: onnx.ModelProto) -> bool:
    """Swap the mis-specified AveragePool for GlobalAveragePool in place."""
    for index, node in enumerate(model.graph.node):
        if node.op_type != "AveragePool" or node.name != POOL_NODE:
            continue
        replacement = onnx.helper.make_node(
            "GlobalAveragePool",
            inputs=list(node.input),
            outputs=list(node.output),
            name=PATCHED_NODE,
        )
        model.graph.node.remove(node)
        model.graph.node.insert(index, replacement)
        return True
    return False


def verify(original_path: str, patched_path: str) -> int:
    """Score both graphs on one random input; 0 delta means nothing changed."""
    try:
        import numpy as np
        import onnxruntime as ort
    except ImportError:
        print("onnxruntime is not installed; skipping the equivalence check",
              file=sys.stderr)
        return 0

    generator = np.random.default_rng(42)
    sample = generator.standard_normal((1, 3, 224, 224), dtype=np.float32)
    outputs = []
    for path in (original_path, patched_path):
        session = ort.InferenceSession(path, providers=["CPUExecutionProvider"])
        outputs.append(session.run(None, {session.get_inputs()[0].name: sample})[0])

    delta = float(np.abs(outputs[0] - outputs[1]).max())
    print(f"outputs {outputs[0].shape}, max abs delta {delta}, "
          f"argmax match {outputs[0].argmax() == outputs[1].argmax()}")
    if delta != 0.0:
        print("the patch changed the model's output; refusing it", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="the published export")
    parser.add_argument("destination", help="where to write the corrected model")
    parser.add_argument("--skip-verify", action="store_true",
                        help="do not score both graphs afterwards")
    arguments = parser.parse_args()

    model = onnx.load(arguments.source)
    if not patch(model):
        print(f"no AveragePool named {POOL_NODE}: nothing to correct "
              "(already patched, or a different export)", file=sys.stderr)
        return 1
    onnx.checker.check_model(model)
    onnx.save(model, arguments.destination)
    print(f"wrote {arguments.destination}")

    if arguments.skip_verify:
        return 0
    return verify(arguments.source, arguments.destination)


if __name__ == "__main__":
    raise SystemExit(main())
