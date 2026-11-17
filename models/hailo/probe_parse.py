"""Feasibility probe: pin a model's inputs and record what the DFC parser says for an arch.

Usage: python probe_parse.py ONNX NAME OUT_DIR INPUT=D0xD1x... [INPUT=...] [--arch hailo10h]

Writes OUT_DIR/NAME.probe.json with the full-graph verdict. When the parser
refuses the graph but recommends end nodes, the probe parses again up to
those nodes, saves OUT_DIR/NAME.har, and counts the operators left on the
host side of the cut, so "the backbone fits, the rest does not" is a number
and not a guess.
"""
import collections
import hashlib
import json
import sys
import traceback
from pathlib import Path

import onnx
from hailo_sdk_client import ClientRunner
from onnxsim import simplify


def pinned(path: Path, shapes: dict, out: Path) -> tuple[Path, str]:
    model = onnx.load(path)
    try:
        model, ok = simplify(model, overwrite_input_shapes=shapes)
        note = "onnxsim ok" if ok else "onnxsim check failed"
    except Exception as err:  # noqa: BLE001 - the probe reports, it does not fix
        note = f"onnxsim failed: {type(err).__name__}: {str(err)[:300]}"
        for graph_input in model.graph.input:
            if graph_input.name in shapes:
                dims = graph_input.type.tensor_type.shape.dim
                for dim, value in zip(dims, shapes[graph_input.name]):
                    dim.ClearField("dim_param")
                    dim.dim_value = value
    target = out / f"{path.stem}.pinned.onnx"
    onnx.save(model, target)
    return target, note


def layer_count(hn) -> int:
    graph = json.loads(hn) if isinstance(hn, str) else hn
    return len(graph.get("layers", {}))


def failure(err: Exception) -> dict:
    """The parser's own words: type, message and where in the DFC it was raised."""
    frames = traceback.extract_tb(err.__traceback__)
    where = [f"{Path(f.filename).name}:{f.lineno} {f.name}" for f in frames[-4:]]
    return {"parsed": False, "error_type": type(err).__name__, "error": str(err)[:4000], "raised_in": where}


def host_side(model: onnx.ModelProto, end_nodes: list[str]) -> dict:
    """Operators not upstream of the end nodes: what the cut leaves to the host."""
    by_output = {o: n for n in model.graph.node for o in n.output}
    by_name = {n.name: n for n in model.graph.node}
    seen, stack = set(), [by_name[n] for n in end_nodes if n in by_name]
    while stack:
        node = stack.pop()
        if node.name in seen:
            continue
        seen.add(node.name)
        stack.extend(by_output[i] for i in node.input if i in by_output)
    rest = [n for n in model.graph.node if n.name not in seen]
    return {"device_nodes": len(seen), "host_nodes": len(rest),
            "host_ops": dict(collections.Counter(n.op_type for n in rest).most_common())}


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    arch = sys.argv[sys.argv.index("--arch") + 1] if "--arch" in sys.argv else "hailo10h"
    if "--arch" in sys.argv:
        args.remove(arch)
    source, name, out = Path(args[0]), args[1], Path(args[2])
    shapes = {k: [int(d) for d in v.split("x")] for k, v in (a.split("=") for a in args[3:])}
    out.mkdir(parents=True, exist_ok=True)
    onnx_path, note = pinned(source, shapes, out)
    model = onnx.load(onnx_path)
    report = {
        "model": source.name,
        "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "arch": arch,
        "inputs": shapes,
        "pinning": note,
        "onnx_nodes": len(model.graph.node),
        "onnx_ops": dict(collections.Counter(n.op_type for n in model.graph.node).most_common()),
    }
    runner = ClientRunner(hw_arch=arch)
    try:
        hn, _ = runner.translate_onnx_model(str(onnx_path), name)
        runner.save_har(str(out / f"{name}.har"))
        report["full_graph"] = {"parsed": True, "layers": layer_count(hn)}
    except Exception as err:  # noqa: BLE001
        report["full_graph"] = failure(err)
        ends = list(getattr(err, "recommended_end_node_names", None) or [])
        starts = list(getattr(err, "recommended_start_node_names", None) or [])
        report["recommended_end_nodes"] = ends
        report["recommended_start_nodes"] = starts
        if ends:
            try:
                runner = ClientRunner(hw_arch=arch)
                hn, _ = runner.translate_onnx_model(str(onnx_path), name, end_node_names=ends,
                                                    start_node_names=starts or None)
                runner.save_har(str(out / f"{name}.har"))
                report["cut_graph"] = {"parsed": True, "layers": layer_count(hn), **host_side(model, ends)}
            except Exception as cut_err:  # noqa: BLE001
                report["cut_graph"] = failure(cut_err)
    (out / f"{name}.probe.json").write_text(json.dumps(report, indent=1))
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
