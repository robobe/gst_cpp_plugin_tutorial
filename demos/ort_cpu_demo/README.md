# ONNX Runtime YOLOv8 Demo

This demo loads a raw Ultralytics YOLOv8 detection model with the ONNX Runtime
C++ API, preprocesses one image, runs CPU inference, applies confidence
filtering and class-aware NMS, and prints the detected boxes.

## Dependencies

Install the compiler, CMake, and OpenCV development files:

```bash
sudo apt update
sudo apt install build-essential cmake libopencv-dev
```

The repository already contains the ONNX Runtime 1.29.0 Linux x64 CPU package
at `demos/ort_cpu_demo/onnxruntime-linux-x64-1.29.0`.

To download and extract it again:

```bash
cd demos/ort_cpu_demo
curl -fLO https://github.com/microsoft/onnxruntime/releases/download/v1.29.0/onnxruntime-linux-x64-1.29.0.tgz
tar -xzf onnxruntime-linux-x64-1.29.0.tgz
rm onnxruntime-linux-x64-1.29.0.tgz
cd ../..
```

## Project Structure

```text
gst_cpp_plugin_tutorial/
├── CMakeLists.txt                  # Includes the demos directory
└── demos/
    ├── CMakeLists.txt              # Includes the CPU demo
    └── ort_cpu_demo/
        ├── CMakeLists.txt          # Builds and links the ort executable
        ├── README.md
        ├── ort.cpp                 # Preprocess, inference, and postprocess
        ├── yolov8n.pt              # Original Ultralytics model
        ├── yolov8n.onnx            # Model loaded by the C++ demo
        └── onnxruntime-linux-x64-1.29.0/
            ├── include/             # ONNX Runtime C and C++ headers
            └── lib/                 # libonnxruntime.so
```

## CMake Configuration

The root `CMakeLists.txt` enters the demos directory with:

```cmake
add_subdirectory(demos)
```

`demos/CMakeLists.txt` then enters the CPU demo:

```cmake
add_subdirectory(ort_cpu_demo)
```

The local `demos/ort_cpu_demo/CMakeLists.txt` performs four jobs:

1. Finds the OpenCV components used for loading and resizing images.
2. Uses the bundled ONNX Runtime directory by default, while allowing
   `ONNXRUNTIME_ROOT` to override it.
3. Finds `onnxruntime_cxx_api.h` under `include/` and `libonnxruntime.so` under
   `lib/` or `lib64/`.
4. Builds `ort.cpp`, links OpenCV and ONNX Runtime, and adds the runtime library
   directory to the executable's build RPATH.

The relevant target configuration is:

```cmake
add_executable(ort ort.cpp)
target_include_directories(ort PRIVATE "${ONNXRUNTIME_INCLUDE_DIR}")
target_link_libraries(ort PRIVATE "${ONNXRUNTIME_LIBRARY}" ${OpenCV_LIBS})
```

## Configure and Build

The bundled runtime is the default, so no ONNX Runtime option is required:

```bash
cmake -S . -B build
cmake --build build --target ort
```

To use a different extracted ONNX Runtime package:

```bash
cmake -S . -B build -DONNXRUNTIME_ROOT=/absolute/path/to/onnxruntime
cmake --build build --target ort
```

## Run

The model must be a fixed-size, batch-one YOLOv8 detection export with raw
output (`nms=False`):

```bash
./build/demos/ort_cpu_demo/ort /absolute/path/to/yolov8n.onnx /absolute/path/to/image.jpg
```

Example output:

```text
detections: 2
class=0 confidence=0.91 x=52 y=41 width=120 height=280
class=5 confidence=0.83 x=230 y=95 width=310 height=190
```

The class values are numeric model class IDs. The demo intentionally does not
load a label-name file or draw boxes.

## Download and Convert YOLOv8 Nano

Install Ultralytics and its ONNX export dependencies in your Python
environment:

```bash
python3 -m pip install ultralytics onnx onnxslim
```

Download the official YOLOv8 nano PyTorch model:

```bash
curl -fL https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8n.pt -o demos/ort_cpu_demo/yolov8n.pt
```

Convert it to the fixed-size raw-output ONNX format expected by this demo:

```bash
yolo export model=demos/ort_cpu_demo/yolov8n.pt format=onnx imgsz=640 batch=1 dynamic=False nms=False
```

## Processing Stages

1. OpenCV loads the BGR image, resizes it without changing its aspect ratio,
   and pads it with pixel value 114.
2. The demo converts pixels to normalized RGB float values in NCHW order.
3. ONNX Runtime runs the model on the CPU.
4. The demo decodes `[center_x, center_y, width, height, class scores...]`, maps
   coordinates back to the original image, and applies class-aware NMS.
