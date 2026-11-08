#!/usr/bin/env python3
"""Derive the pinned batch-one SLANet graph used by native OpenVINO tables.

No trained weights or decoder arithmetic change. Loop formal names are made
lexically unique, direct passthrough outputs receive Identity nodes, singleton
zero control constants become scalars, and five provably unread initial states
get their output ranks. The input contract is 1x3x488x488. The exact source and
result hashes prevent applying these model-specific rewrites to other exports.
The tokens output is the original character metadata verbatim; the decoder
owns sos/eos and <td> normalization.
"""
import argparse
import hashlib
from pathlib import Path

import numpy as np
import onnx
from onnx import helper, numpy_helper

SOURCE_SHA256 = "d57a942af6a2f57d6a4a0372573c696a2379bf5857c45e2ac69993f3b334514b"
MODEL_SHA256 = "2b75d7ea35fd6e825a503fb620245fdcb570173697d08090722c3bc64934755e"

def nested_graphs(node):
    for attr in node.attribute:
        if attr.type == onnx.AttributeProto.GRAPH:
            yield attr.name, attr.g
        elif attr.type == onnx.AttributeProto.GRAPHS:
            for graph in attr.graphs:
                yield attr.name, graph


def all_names(graph):
    for value in (*graph.input, *graph.output, *graph.value_info):
        yield value.name
    for tensor in graph.initializer:
        yield tensor.name
    for tensor in graph.sparse_initializer:
        yield tensor.values.name
    for node in graph.node:
        yield from node.input
        yield from node.output
        for _, child in nested_graphs(node):
            yield from all_names(child)


def rename_loop_formals(model):
    reserved = set(all_names(model.graph))
    changes = []

    def fresh(body_number, index):
        name = f"grparse_loop_{body_number}_formal_{index}"
        while name in reserved:
            name += "_"
        reserved.add(name)
        return name

    def visit(graph, inherited, loop_body=False):
        # Inner definitions shadow outer bindings, even when their strings
        # coincide. Only free references inherit a parent-scope rename.
        definitions = {v.name for v in graph.input}
        definitions.update(v.name for v in graph.initializer)
        definitions.update(v.values.name for v in graph.sparse_initializer)
        definitions.update(name for node in graph.node for name in node.output)
        bindings = {old: new for old, new in inherited.items() if old not in definitions}
        own = {}
        if loop_body:
            number = len(changes)
            own = {v.name: fresh(number, i) for i, v in enumerate(graph.input)}
            changes.append({"body": graph.name, "bindings": own.copy()})
            bindings.update(own)
            for value in graph.input:
                value.name = own[value.name]
            for tensor in graph.initializer:
                tensor.name = own.get(tensor.name, tensor.name)
            for tensor in graph.sparse_initializer:
                tensor.values.name = own.get(tensor.values.name, tensor.values.name)
        for node in graph.node:
            for i, name in enumerate(node.input):
                node.input[i] = bindings.get(name, name)
            for attr_name, child in nested_graphs(node):
                visit(child, bindings, node.op_type == "Loop" and attr_name == "body")
        for value in (*graph.output, *graph.value_info):
            value.name = bindings.get(value.name, value.name)

    visit(model.graph, {})
    return changes


def graphs(graph):
    yield graph
    for node in graph.node:
        for attr in node.attribute:
            if attr.type == onnx.AttributeProto.GRAPH:
                yield from graphs(attr.g)
            elif attr.type == onnx.AttributeProto.GRAPHS:
                for child in attr.graphs:
                    yield from graphs(child)


def references(graph):
    names = {name for node in graph.node for name in node.input}
    names.update(value.name for value in graph.output)
    for node in graph.node:
        for attr in node.attribute:
            if attr.type == onnx.AttributeProto.GRAPH:
                names.update(references(attr.g))
            elif attr.type == onnx.AttributeProto.GRAPHS:
                for child in attr.graphs:
                    names.update(references(child))
    return names


def normalize(model):
    original_weights = [t.SerializeToString() for t in model.graph.initializer]
    rename_loop_formals(model)
    identities = []
    scalar_flags = []
    for graph in list(graphs(model.graph)):
        for node in list(graph.node):
            if node.op_type == "Loop":
                body = next(a.g for a in node.attribute if a.name == "body")
                input_names = {v.name for v in body.input}
                for output in body.output:
                    if output.name in input_names:
                        original = output.name
                        output.name = f"grparse_loop_identity_output_{len(identities)}"
                        body.node.append(helper.make_node(
                            "Identity", [original], [output.name], name=output.name))
                        identities.append(output.name)
            if node.op_type == "Constant" and node.output[0] in {
                    "helper.constant.122", "helper.constant.125", "helper.constant.150"}:
                tensor = next(a.t for a in node.attribute if a.type == onnx.AttributeProto.TENSOR)
                value = numpy_helper.to_array(tensor)
                assert value.shape == (1,) and value.item() == 0
                tensor.CopyFrom(numpy_helper.from_array(value.reshape(()), name=tensor.name))
                scalar_flags.append(node.output[0])
    assert len(identities) == 18
    assert len(scalar_flags) == 3
    graph = model.graph
    loops = [n for n in graph.node if n.op_type == "Loop"]
    assert len(loops) == 1
    loop = loops[0]
    assert len(loop.input) == 32 and len(loop.output) == 30
    body = next(a.g for a in loop.attribute if a.name == "body")
    assert len(body.input) == 32 and len(body.output) == 31
    unchanged_body = body.SerializeToString()
    unchanged_outputs = list(loop.output)
    producers = {name: n for n in graph.node for name in n.output}

    def scalar_expression(name):
        node = producers[name]
        if node.op_type == "Constant":
            return numpy_helper.to_array(next(a.t for a in node.attribute
                                              if a.type == onnx.AttributeProto.TENSOR))
        values = [scalar_expression(x) for x in node.input]
        if node.op_type == "Cast":
            dtype = next(a.i for a in node.attribute if a.name == "to")
            return values[0].astype(helper.tensor_dtype_to_np_dtype(dtype))
        if node.op_type == "Add":
            return values[0] + values[1]
        if node.op_type == "Less":
            return values[0] < values[1]
        if node.op_type == "Not":
            return np.logical_not(values[0])
        if node.op_type == "And":
            return np.logical_and(*values)
        raise ValueError(f"nonconstant loop control: {node.op_type} {name}")

    trip_count = scalar_expression(loop.input[0])
    initial_condition = scalar_expression(loop.input[1])
    assert trip_count.size == 1 and trip_count.item() == 1024
    assert initial_condition.size == 1 and bool(initial_condition.item())
    refnames = references(body)
    specs = {
        10: (1, 501, 8),
        12: (1, 50),
        17: (1, 501, 50),
        20: (1, 8),
        30: (1, 501),
    }
    occupied = set(producers) | {i.name for i in graph.initializer}
    replacements = []
    for index, shape in specs.items():
        formal = body.input[index]
        assert formal.name not in refnames, f"state {index} is referenced"
        dtype = formal.type.tensor_type.elem_type
        expected_dtype = onnx.TensorProto.INT32 if index == 30 else onnx.TensorProto.FLOAT
        assert dtype == expected_dtype
        name = f"grparse_b70_initial_state_{index}"
        assert name not in occupied
        old = loop.input[index]
        value = np.zeros(shape, dtype=helper.tensor_dtype_to_np_dtype(dtype))
        graph.initializer.append(numpy_helper.from_array(value, name=name))
        loop.input[index] = name
        replacements.append({"index":index,"old":old,"new":name,
                             "shape":shape,"element_type":dtype})
    assert len(graph.input) == 1 and graph.input[0].name == "x"
    shape = graph.input[0].type.tensor_type.shape
    shape.ClearField("dim")
    for size in (1, 3, 488, 488):
        shape.dim.add().dim_value = size
    assert body.SerializeToString() == unchanged_body
    assert list(loop.output) == unchanged_outputs
    onnx.checker.check_model(model)
    onnx.shape_inference.infer_shapes(model, strict_mode=True, data_prop=True)

    assert [t.SerializeToString() for t in graph.initializer[:len(original_weights)]] == original_weights
    return model


def derive(source):
    original = source.read_bytes()
    if hashlib.sha256(original).hexdigest() != SOURCE_SHA256:
        raise ValueError("source SHA256 differs from pinned slanet_plus.onnx; refusing rewrite")
    model = onnx.load_model_from_string(original)
    onnx.checker.check_model(model)
    characters = [entry.value for entry in model.metadata_props if entry.key == "character"]
    if len(characters) != 1 or not characters[0]:
        raise ValueError("expected one nonempty character metadata entry")
    tokens = characters[0].encode("utf-8")
    result = normalize(model).SerializeToString()
    if hashlib.sha256(result).hexdigest() != MODEL_SHA256:
        raise ValueError("derived model SHA256 differs from validated graph; refusing output")
    return result, tokens


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--tokens-only", action="store_true",
                        help="write the original character metadata instead of the graph")
    args = parser.parse_args()
    if args.source.resolve() == args.output.resolve():
        parser.error("output must differ from source")
    model, tokens = derive(args.source)
    data = tokens if args.tokens_only else model
    args.output.write_bytes(data)
    print(f"{hashlib.sha256(data).hexdigest()}  {args.output}")


if __name__ == "__main__":
    main()
