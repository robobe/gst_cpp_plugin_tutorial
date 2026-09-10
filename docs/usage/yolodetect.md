# Using the YOLO Detection Plugin

Run these commands from the project root.

## Build
```bash
cmake -S . -B build
cmake --build build
```

## Run Detection on the Example Image

This pipeline decodes `assets/bus.jpg`, converts it to the RGB format required
by `yolodetect`, and runs inference. Each retained object is attached as
`GstVideoRegionOfInterestMeta`, matching `rknnyolodetect`: ROI type
`yolo-detection` and a `yolo` parameter structure with `class-id` and
`confidence`.

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

## YOLO26

The same element supports a raw YOLO26 ONNX export. Download and create the
bundled YOLO26n model once:

```bash
.venv/bin/python demos/yolo26/export_yolo26.py
```

Then use it exactly like YOLOv8:

```bash
yolodetect model-path="$PWD/demos/yolo26/yolo26n.onnx"
```

The exporter uses `nms=None`, yielding raw `[1,84,8400]` COCO predictions;
`yolodetect` applies confidence filtering and NMS. Do not export YOLO26 with
`nms=False`, which creates an incompatible end-to-end `[1,300,6]` output.

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

## Print Detection Metadata

Use `metaprint` to inspect the shared YOLO ROI metadata:

```bash
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 -q \
  filesrc location="$PWD/assets/bus.jpg" ! jpegdec ! videoconvert ! \
  video/x-raw,format=RGB ! \
  yolodetect model-path="$PWD/demos/ort_cpu_demo/yolov8n.onnx" ! metaprint ! fakesink
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

Use the metadata-printing pipeline above with the downloaded video source to
inspect boxes and numeric class labels.
