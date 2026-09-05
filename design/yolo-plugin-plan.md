# Synchronous YOLOv8 GStreamer Plugin

## Summary

Build `yolodetect`, a synchronous C++ `GstBaseTransform` that runs Ultralytics YOLOv8 detection through ONNX Runtime CPU and attaches standard GStreamer ROI metadata without modifying the frame.

## Milestones

### 1. Dependency and element skeleton

- Require `ONNXRUNTIME_ROOT` during CMake configuration and fail clearly when headers or libraries are missing.
- Add `yolodetect` with RGB sink/source caps and three READY-only properties:
  - `model-path`: required ONNX path
  - `confidence-threshold`: double, default `0.25`
  - `iou-threshold`: double, default `0.45`
- Create the ONNX Runtime session when the element starts and release it when stopped.
- Validate a static batch-1 YOLOv8 detection contract: float NCHW input and raw `[1, 4 + classes, candidates]` output. Reject missing names metadata, dynamic or invalid dimensions, NMS-embedded exports, and non-detection models.

### 2. Synchronous inference

- Map each RGB frame read-only using `GstVideoFrame`.
- Apply Ultralytics-compatible letterboxing: preserve aspect ratio, pad with `114`, normalize to `[0,1]`, and produce RGB float NCHW input.
- Run one CPU inference synchronously per buffer.
- Decode center-based boxes, select the highest class score, apply the confidence threshold, then class-aware NMS using the IoU threshold.
- Map boxes back through the letterbox transform, clip them to source dimensions, discard zero-area boxes, and retain at most 300 detections.

### 3. Metadata and verification

- Attach one `GstVideoRegionOfInterestMeta` per retained box.
- Use the class name as ROI type and add a `yolo` parameter structure containing:
  - `class-id`: integer
  - `label`: string
  - `confidence`: double
- Preserve buffer pixels, timestamps, duration, and ordering. Zero detections means zero ROI metas.
- Extend the existing `metaprint` element to enumerate and print every ROI with frame PTS and detection fields.
- Any model-loading, mapping, inference, decoding, or metadata failure posts a GStreamer element error and stops the pipeline.

### 4. Tutorial and synchronous benchmark

- Document configuration with `-DONNXRUNTIME_ROOT=/absolute/path`, inspection, and pipelines for user-supplied ONNX and media files.
- Include a headless `decodebin ! videoconvert ! RGB caps ! yolodetect ! metaprint ! fakesink` example.
- Add debug-level timing for preprocessing, inference, and post-processing so the synchronous bottleneck can be measured.

### 5. Later asynchronous milestone

- Reuse the synchronous element unchanged behind `tee ! queue leaky=downstream max-size-buffers=1`.
- Keep display on an independent non-leaky branch; consume inference-branch buffers and ROI metadata separately, correlated by PTS.
- Add CUDA, batching, or a custom worker element only if measurements show the leaky branch is insufficient.

## Test Plan

- Build with the external ONNX Runtime root and verify `gst-inspect-1.0 yolodetect` reports the expected caps and properties.
- Add one focused runnable check covering letterbox coordinate reversal, confidence filtering, class-aware NMS, clipping, and an empty result.
- Confirm missing model paths and incompatible ONNX shapes fail pipeline startup.
- Run a finite synthetic stream to confirm clean EOS even with zero detections.
- Run user-supplied COCO-like media and verify multiple printed ROIs have valid labels, confidence values, source-frame coordinates, and original PTS.
- Confirm changing either threshold before playback changes retained detections.

## Assumptions

- The supplied model is an Ultralytics YOLOv8 detection export with `nms=False`, fixed batch and image dimensions, FP32 tensors, and embedded class-name metadata. Ultralytics documents [YOLOv8 ONNX detection support and ONNX metadata embedding](https://docs.ultralytics.com/integrations/onnx).
- ONNX Runtime is externally installed; no model/runtime downloading or vendored binaries are added.
- V1 excludes CUDA, live property mutation, drawing boxes, YUV/BGR input, segmentation, pose, tracking, batching, and support for other YOLO generations.
