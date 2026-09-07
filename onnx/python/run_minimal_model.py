#!/usr/bin/env python3
"""Run the minimal ONNX model with ONNX Runtime."""

from pathlib import Path

import numpy as np
import onnxruntime as ort


model_path = Path(__file__).with_name("minimal.onnx")
session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
input_name = session.get_inputs()[0].name
output_name = session.get_outputs()[0].name

input_data = np.array([[1.0, 2.0, 3.0]], dtype=np.float32)
output_data = session.run([output_name], {input_name: input_data})[0]

print(f"input name:  {input_name}")
print(f"output name: {output_name}")
print(f"input:  {input_data}")
print(f"output: {output_data}")
