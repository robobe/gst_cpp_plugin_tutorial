# inference.py

import numpy as np
import onnxruntime as ort

session = ort.InferenceSession("simple.onnx")

#region get input and output names
for inp in session.get_inputs():
    print(
        "name:", inp.name,
        "shape:", inp.shape,
        "type:", inp.type
    )

for out in session.get_outputs():
    print(
        "name:", out.name,
        "shape:", out.shape,
        "type:", out.type
    )
#endregion

x = np.array([
    [1.0],
    [2.0],
    [3.0],
], dtype=np.float32)

input_name = session.get_inputs()[0].name

outputs = session.run(
    None,
    {input_name: x}
)

print(outputs[0])