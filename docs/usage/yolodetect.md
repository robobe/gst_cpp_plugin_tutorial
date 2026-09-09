# Using the YOLO Detection Plugin

Run these commands from the project root.

## Build

On Ubuntu 24.04, install the analytics development API once:

```bash
sudo apt install -y libgstreamer-plugins-bad1.0-dev
```

```bash
cmake -S . -B build
cmake --build build
```

## Run Detection on the Example Image

This pipeline decodes `assets/bus.jpg`, converts it to the RGB format required
by `yolodetect`, and runs inference. The plugin attaches analytics
object-detection metadata to the frame:

```bash
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 -q \
  filesrc location="$PWD/assets/bus.jpg" ! \
  jpegdec ! \
  videoconvert ! \
  video/x-raw,format=RGB ! \
  yolodetect model-path="$PWD/demos/ort_cpu_demo/yolov8n.onnx" ! \
  fakesink
```

Object types are numeric YOLO class IDs; for the COCO model, `0` is a person
and `5` is a bus.

## Limit CPU Threads

`intra-op-threads=0` (the default) lets ONNX Runtime choose its worker count.
Set a positive value to cap inference workers, for example one thread:

```bash
yolodetect model-path="$PWD/demos/ort_cpu_demo/yolov8n.onnx" intra-op-threads=1
```

## Create an INT8 Model

Create a separate static-INT8 model calibrated from the bundled detection
video. It validates the output tensor and prints FP32 versus INT8 CPU timings:

```bash
python3 onnx/python/quantize_yolo.py --threads=1
```

Use the result only when its reported speedup is greater than `1.00x` and it
has been validated on representative deployment footage. Quantization is
hardware-dependent and may be slower than FP32.

To test a faster model with the plugin:

```bash
yolodetect model-path="$PWD/models/yolov8n.int8.onnx" intra-op-threads=1
```

## Draw Detection Boxes

Ubuntu's packaged 1.24.2 `objectdetectionoverlay` does not forward EOS, so a
pipeline ending in that element does not terminate. Use the newer GStreamer
development environment described in
[ONNX YOLO GStreamer Build](onnx-yolo-gstreamer-build.md) to render boxes:

```bash
meson devenv -C "$HOME/src/gstreamer/build"
```

From that shell, run:

```bash
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 -e \
  filesrc location="$PWD/assets/detection-demo.mp4" ! \
  decodebin ! videoconvert ! video/x-raw,format=RGB ! \
  yolodetect model-path="$PWD/demos/ort_cpu_demo/yolov8n.onnx" ! \
  objectdetectionoverlay ! videoconvert ! autovideosink sync=false
```

## Show Processing Times

Enable the plugin's GStreamer log category to print preprocessing, inference,
and postprocessing time:

```bash
GST_DEBUG_NO_COLOR=1 \
GST_DEBUG=yolodetect:6 \
GST_PLUGIN_PATH="$PWD/build" \
gst-launch-1.0 -q \
  filesrc location="$PWD/assets/bus.jpg" ! \
  jpegdec ! videoconvert ! video/x-raw,format=RGB ! \
  yolodetect model-path="$PWD/demos/ort_cpu_demo/yolov8n.onnx" ! \
  fakesink
```

## Run Detection on a Demo Video

Download Intel's sample video containing people, bicycles, and cars:

```bash
curl -fL \
  https://github.com/intel-iot-devkit/sample-videos/raw/master/person-bicycle-car-detection.mp4 \
  -o assets/detection-demo.mp4
```

Use the drawing pipeline above from the newer GStreamer environment. It
displays bounding boxes and numeric class labels.
