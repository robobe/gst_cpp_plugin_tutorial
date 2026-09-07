# YOLOv8 ONNX Runtime C++ Pipeline

This independent example runs `models/yolov8n.onnx` on `assets/bus.jpg`. It
performs image loading, letterboxing, tensor conversion, synchronous inference,
YOLO decoding, confidence filtering, coordinate restoration, and class-aware
NMS. The source file documents each pipeline stage from top to bottom.

## Dependencies

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build libopencv-dev curl
```

Download ONNX Runtime 1.29.0 into `onnx/cpp` if it is not already present:

```bash
cd onnx/cpp
curl -fLO \
  https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-linux-x64-1.29.0.tgz
tar -xzf onnxruntime-linux-x64-1.29.0.tgz
rm onnxruntime-linux-x64-1.29.0.tgz
cd ../..
```

## Build and Run

Run from the repository root:

```bash
cmake -S onnx/cpp/yolo -B build/onnx-yolo -G Ninja
cmake --build build/onnx-yolo
./build/onnx-yolo/yolo_inference
```

Pass another compatible raw YOLOv8 model and image if needed:

```bash
./build/onnx-yolo/yolo_inference /path/to/yolov8.onnx /path/to/image.jpg
```

The default image should produce five detections, including COCO class `0`
(person) and class `5` (bus). This example prints boxes but does not draw them.
