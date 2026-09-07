#!/usr/bin/env python3
"""Create an ONNX model that adds one to its input."""

from pathlib import Path

import onnx
from onnx import TensorProto, helper


model_path = Path(__file__).with_name("minimal.onnx")

input_info = helper.make_tensor_value_info("input", TensorProto.FLOAT, [1, 3])
output_info = helper.make_tensor_value_info("output", TensorProto.FLOAT, [1, 3])
one = helper.make_tensor("one", TensorProto.FLOAT, [1], [1.0])
add = helper.make_node("Add", ["input", "one"], ["output"])

graph = helper.make_graph([add], "add-one", [input_info], [output_info], [one])
model = helper.make_model(
    graph,
    opset_imports=[helper.make_opsetid("", 13)],
    producer_name="minimal-example",
)
model.ir_version = 10

onnx.checker.check_model(model)
onnx.save(model, model_path)
print(f"created: {model_path}")
