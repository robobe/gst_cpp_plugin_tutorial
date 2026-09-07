# Minimal ONNX Runtime C++ Example

This example loads `onnx/minimal.onnx`, fetches its input and output names,
runs `[1, 2, 3]` through the model, and prints `[2, 3, 4]`.

## Install Build Tools

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build curl
```

## Download ONNX Runtime

Run from the repository root. Version 1.29.0 is the default expected beside
the `minimal/` and `yolo/` example folders:

```bash
cd onnx/cpp
curl -fLO \
  https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-linux-x64-1.29.0.tgz
tar -xzf onnxruntime-linux-x64-1.29.0.tgz
rm onnxruntime-linux-x64-1.29.0.tgz
cd ../..
```

The repository already contains this runtime, so downloading it again is not
normally necessary.

## Create the Model

```bash
uv run --with onnx python onnx/create_minimal_model.py
```

## Build and Run

```bash
cmake -S onnx/cpp/minimal -B build/onnx-minimal -G Ninja
cmake --build build/onnx-minimal
./build/onnx-minimal/run_minimal_model
```

Expected output:

```text
input name:  input
output name: output
input:  [1, 2, 3]
output: [2, 3, 4]
```

Pass another model path as the first argument, or configure another runtime:

```bash
./build/onnx-minimal/run_minimal_model /path/to/model.onnx
cmake -S onnx/cpp/minimal -B build/onnx-minimal \
  -DONNXRUNTIME_ROOT=/path/to/onnxruntime
```
