# create_model.py

import onnx
from onnx import helper, TensorProto

# Input / output definitions
x = helper.make_tensor_value_info(
    "input",
    TensorProto.FLOAT,
    [None, 1],
)

y = helper.make_tensor_value_info(
    "output",
    TensorProto.FLOAT,
    [None, 1],
)

# Constants
two = helper.make_tensor(
    name="two",
    data_type=TensorProto.FLOAT,
    dims=[1],
    vals=[2.0],
)

one = helper.make_tensor(
    name="one",
    data_type=TensorProto.FLOAT,
    dims=[1],
    vals=[1.0],
)

# y = x * 2
mul_node = helper.make_node(
    "Mul",
    inputs=["input", "two"],
    outputs=["mul_output"],
)

# y = x * 2 + 1
add_node = helper.make_node(
    "Add",
    inputs=["mul_output", "one"],
    outputs=["output"],
)

graph = helper.make_graph(
    nodes=[mul_node, add_node],
    name="SimpleModel",
    inputs=[x],
    outputs=[y],
    initializer=[two, one],
)

model = helper.make_model(
    graph,
    opset_imports=[helper.make_opsetid("", 17)],
)

onnx.save(model, "simple.onnx")

print("saved simple.onnx")