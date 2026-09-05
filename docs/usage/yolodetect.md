# Using the YOLO Detection Plugin

Run these commands from the project root.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run Detection on the Example Image

This pipeline decodes `assets/bus.jpg`, converts it to the RGB format required
by `yolodetect`, runs inference, prints the detection metadata, and discards the
image at the end of the pipeline:

```bash
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 -q \
  filesrc location="$PWD/assets/bus.jpg" ! \
  jpegdec ! \
  videoconvert ! \
  video/x-raw,format=RGB ! \
  yolodetect model-path="$PWD/demos/ort_cpu_demo/yolov8n.onnx" ! \
  metaprint ! \
  fakesink
```

Example output:

```text
metaprint: roi=yolo-detection class=0 confidence=0.88 x=211 y=241 width=73 height=267 ...
metaprint: roi=yolo-detection class=5 confidence=0.84 x=97 y=136 width=453 height=321 ...
```

For the COCO model used by YOLOv8, class `0` is a person and class `5` is a
bus. The plugin publishes metadata without drawing boxes on the image.

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
  metaprint ! fakesink
```

## Run Detection on a Demo Video

Download Intel's sample video containing people, bicycles, and cars:

```bash
curl -fL \
  https://github.com/intel-iot-devkit/sample-videos/raw/master/person-bicycle-car-detection.mp4 \
  -o assets/detection-demo.mp4
```

Decode the video, convert each frame to RGB, run YOLO inference, print the
detection metadata, and display the original frames:

```bash
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 -e \
  filesrc location="$PWD/assets/detection-demo.mp4" ! \
  decodebin ! videoconvert ! video/x-raw,format=RGB ! \
  yolodetect model-path="$PWD/demos/ort_cpu_demo/yolov8n.onnx" ! \
  metaprint ! videoconvert ! autovideosink sync=false
```

This pipeline displays the video and prints detections. It does not draw the
bounding boxes because `yolodetect` currently publishes metadata without
rendering it into the frame.
