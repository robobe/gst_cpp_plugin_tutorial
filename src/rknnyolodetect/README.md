# RKNN YOLO detection plugin

`rknnyolodetect` runs synchronous YOLOv8 detection on the Radxa RK3566 NPU and
attaches one standard GStreamer ROI metadata entry per retained detection.
[RGA](rknnyolodetect.cpp) performs image padding and resizing; RKNN performs
inference; C++ code decodes boxes and applies non-maximum suppression (NMS).
The plugin forwards video pixels unchanged and does not draw boxes.

This directory contains the detector [source](rknnyolodetect.cpp) and its
standalone [CMake project](CMakeLists.txt), following the layout of
[the NanoTrack plugin](../nanotracker/README.md). The detector source was moved
from `src/rknnyolodetect.cpp`; CPU preprocessing fallback was added afterward. It detects objects
independently on every frame; it does not maintain tracking IDs or require an
initial target ROI.

## Build on the PC for Radxa

From the repository root:

```bash
cmake -S src/rknnyolodetect -B build-radxa-yolo -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/radxa-zero3w.cmake" \
  -DRADXA_SYSROOT=/home/user/sysroot \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-radxa-yolo
file build-radxa-yolo/libgstrknnyolodetect.so
```

The C++17 project builds `libgstrknnyolodetect.so` and the shared metadata reader
`libgstmetaprint.so` from `../metaprint.cpp`. It requires GStreamer core/base/video,
librga headers/library, `rknn_api.h` plus librknnrt, and OpenCV core/imgproc
for CPU preprocessing fallback.
The build uses the existing GCC 12 cross compiler and board sysroot, and omits
host RPATHs from the libraries. The root CMake project does not build this target.

See the [cross-compilation guide](../../radxa/nano/CROSS_COMPILING.md) for
sysroot/toolchain details and the [older setup guide](../../docs/usage/rknnyolodetect.md)
for GStreamer development files. On this PC, `/home/user/sysroot` aliases
`/home/user/sysroots/radxa`. Use a fresh build directory if compiler or sysroot
settings change. The old `cmake -S radxa` command is replaced by the command above.

For a native build on the board, when development dependencies are installed:

```bash
cmake -S src/rknnyolodetect -B build-yolo -DCMAKE_BUILD_TYPE=Release
cmake --build build-yolo
```

## Deploy and run

On the PC, copy the plugins into the existing demo directory:

```bash
ssh radxa 'mkdir -p ~/gst-yolo-demo/{plugins,assets,models}'
scp build-radxa-yolo/libgstrknnyolodetect.so \
    build-radxa-yolo/libgstmetaprint.so radxa:gst-yolo-demo/plugins/
```

The connected board already has `~/gst-yolo-demo/assets/bus.jpg` and
`~/gst-yolo-demo/models/yolov8n.rknn`. For another board, also copy an image and a
compatible compiled model; replace the model source path with your actual file:

```bash
scp assets/bus.jpg radxa:gst-yolo-demo/assets/
scp /path/to/yolov8n.rknn radxa:gst-yolo-demo/models/
```

Connect and inspect the loaded plugin:

```bash
ssh radxa
export GST_PLUGIN_PATH="$HOME/gst-yolo-demo/plugins${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
gst-inspect-1.0 rknnyolodetect
gst-inspect-1.0 metaprint
```

Check the library filename in inspection output if an older installed plugin
appears instead of your new build. Then run the image smoke test:

```bash
gst-launch-1.0 -q \
  filesrc location="$HOME/gst-yolo-demo/assets/bus.jpg" ! \
  jpegdec ! videoconvert ! video/x-raw,format=RGB ! \
  rknnyolodetect model-path="$HOME/gst-yolo-demo/models/yolov8n.rknn" \
    confidence-threshold=0.25 iou-threshold=0.45 ! \
  metaprint ! fakesink
```

`metaprint` prints class IDs, confidence, rectangles, and buffer timestamps.
For the provided bus image/model, detections include people (class 0) and a bus
(class 5). A still-image buffer can have an unset PTS, printed as
`GST_CLOCK_TIME_NONE`.

For a video on the connected board:

```bash
gst-launch-1.0 -q \
  filesrc location="$HOME/projects/rknn_demo2/assets/camera_run_2s_10s.mp4" ! \
  decodebin ! videoconvert ! video/x-raw,format=RGB ! \
  rknnyolodetect model-path="$HOME/gst-yolo-demo/models/yolov8n.rknn" ! \
  metaprint ! fakesink
```

The sink/source caps are `video/x-raw,format=RGB`. Upstream decoding and color
conversion are GStreamer responsibilities. The input must be CPU-mappable.
`fakesink` consumes frames without a window. Replacing it with
`videoconvert ! autovideosink` displays the video, but still does not draw
metadata rectangles; an overlay consumer is needed for that.

## Properties

| Property | Default | Meaning |
| --- | --- | --- |
| `model-path` | Unset; required | Compiled RK3566 YOLOv8 RKNN file with the tensor contract below. |
| `confidence-threshold` | 0.25 | Class-confidence filter, range 0–1. Increasing it retains fewer candidates. |
| `iou-threshold` | 0.45 | Same-class NMS threshold, range 0–1. A lower value suppresses overlapping boxes more aggressively. |

Configure all three in NULL or READY, before playback. They are advertised with
`GST_PARAM_MUTABLE_READY`; the current setters do not enforce that state or
synchronize runtime updates. In particular, assigning `model-path` during
playback does not reload the current runtime. Stop to READY before changing
settings, then restart. There is no enable flag or precision selector in this detector. CPU resize
fallback is automatic; it requires no property setting.

## Model contract

The code supports a specific three-branch YOLOv8 INT8 export, not arbitrary
YOLO `.rknn` files. It validates one 640 × 640 three-channel input (NCHW or NHWC
logical layout), and nine affine-quantized INT8 NCHW outputs with positive
quantization scales:

| Output indices | Grid | Box distribution | Class scores | Score sums |
| --- | --- | --- | --- | --- |
| 0, 1, 2 | 80 × 80 | `[1,64,80,80]` | `[1,80,80,80]` | `[1,1,80,80]` |
| 3, 4, 5 | 40 × 40 | `[1,64,40,40]` | `[1,80,40,40]` | `[1,1,40,40]` |
| 6, 7, 8 | 20 × 20 | `[1,64,20,20]` | `[1,80,20,20]` | `[1,1,20,20]` |

Each box output has four distances with 16 distribution bins per distance.
There are 80 classes. Models with combined outputs, different class counts,
FP16 outputs, or another output order need different decoding code. ONNX files
cannot be passed directly to `model-path`. The application submits UINT8 NHWC
RGB pixels; conversion/normalization compatibility must be preserved in the
compiled model.

## Source walkthrough

### GStreamer element lifecycle

`GstRknnYoloDetect` subclasses `GstBaseTransform` and stores negotiated video
information, property values, and a `RknnRuntime*`. `class_init()` registers the
RGB pads, properties, and callbacks; `plugin_init()` registers the element
factory as `rknnyolodetect`.

`start()` requires `model-path`, creates the runtime, and logs the RKNN API and
driver versions. `set_caps()` records `GstVideoInfo`. `transform_ip()` maps each
frame read-only, preprocesses it, unmaps it, runs detection, and attaches ROI
metadata. In-place mode keeps the original pixels, and passthrough is disabled
so equal input/output caps do not skip processing. `stop()` deletes the runtime;
`finalize()` also releases any remaining runtime and the model-path string.

### RGA preprocessing and CPU fallback

`preprocess()` checks dimensions and row stride. It uses the mapped RGB memory
directly only when rows are tightly packed and width is divisible by 16.
Otherwise it copies rows into reusable storage whose width is aligned to 16
pixels, respecting the original mapped stride.

It computes the scale to fit the frame into 640 × 640. The resized width is
rounded down to a multiple of four and height to a multiple of two, then the
image is centered. `imfill()` fills the destination with gray value 114
(`0x72`); `improcess()` resizes into that padded image. Because alignment can
slightly change the aspect ratio, `Letterbox` records separate actual x/y
scales and integer padding for restoring boxes later. The CPU vectors are
wrapped for RGA by virtual address; this detector does not import DMA-BUF
handles like the NanoTrack implementation.

If RGA fill or resize reports failure, the same frame is rebuilt on the CPU:
OpenCV fills the complete 640 × 640 input with gray 114 and uses `INTER_LINEAR`
resize into the same aligned rectangle. It reads the original RGB frame with
its mapped row stride. Padding and coordinate restoration are unchanged, though
interpolation results can differ slightly from RGA. A warning is logged once,
and subsequent frames bypass RGA and its alignment copy. Stopping and restarting
the element creates a new runtime and tries RGA again. Each instance has its
own fallback state. RKNN inference remains on the NPU. This handles reported
RGA errors, not native crashes or a missing librga shared library at load time.

### RKNN execution and ownership

`read_file()` loads nonempty model bytes. `RknnRuntime` initializes the RKNN
context, validates tensor shapes/types, and allocates the reusable RGB input
vector. Copying the runtime is disabled and destruction releases its context.

`infer()` submits the preprocessed image with `rknn_inputs_set()` and
`pass_through=0`, runs `rknn_run()`, and retrieves all nine outputs with
`want_float=0`, retaining quantized INT8 values. It releases the output buffers
on both success and decoding exceptions. Every frame is processed synchronously
on the streaming thread; there is no internal queue or frame-skipping policy.

### Box decoding and filtering

`quantize()` maps thresholds using `round(value/scale + zero_point)` and clamps
them to INT8. `dequantize()` restores `(quantized-zero_point)*scale`.

For each grid cell, `decode()` first filters the quantized score sum, then
selects the greatest of 80 class scores and compares it with the quantized
confidence threshold. It does not apply a new sigmoid to class scores.

`decode_dfl()` dequantizes each 16-bin distance distribution, uses softmax with
maximum subtraction, and computes its expected bin index. The four resulting
distances define corners around the cell center `(column+0.5, row+0.5)`, scaled
by stride 8, 16, or 32. The decoder removes letterbox padding, divides by the
actual x/y scales, clips to source bounds, and discards empty rectangles.

Candidates are sorted by decreasing confidence. Greedy NMS retains a candidate
unless an already-kept box of the same class has IoU greater than the configured
threshold. Different classes do not suppress each other. This is a quadratic
scan of filtered candidates; there is no fixed maximum detection count.

## Metadata and downstream use

Each retained detection produces `GstVideoRegionOfInterestMeta` with:

| Field | Value |
| --- | --- |
| `roi_type` | Quark for `yolo-detection` |
| `x`, `y`, `w`, `h` | Source-pixel rectangle: floor left/top, ceil right/bottom. |
| Parameter structure | `yolo` |
| `class-id` | `G_TYPE_INT`, selected model class index. |
| `confidence` | `G_TYPE_DOUBLE`, dequantized selected class score. |

If there are no retained detections, the plugin adds no detection metadata.
An absence of metadata is not a separate `found=false` record. Existing input
metadata is not removed. Timestamps remain on the original video buffer.

In a pad probe or appsink consumer, iterate the ROI metadata and inspect its
`yolo` parameter structure, as the shared [metaprint source](../metaprint.cpp)
does:

```cpp
gpointer state = nullptr;
while (GstMeta* meta = gst_buffer_iterate_meta_filtered(
           buffer, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) {
    auto* roi = reinterpret_cast<GstVideoRegionOfInterestMeta*>(meta);
    GstStructure* parameters = gst_video_region_of_interest_meta_get_param(roi, "yolo");
    if (!parameters) continue;
    gint class_id = -1;
    gdouble confidence = 0;
    gst_structure_get_int(parameters, "class-id", &class_id);
    gst_structure_get_double(parameters, "confidence", &confidence);
    // Consume roi->x/y/w/h, class_id, confidence, and GST_BUFFER_PTS(buffer).
}
```

The buffer owns these metadata pointers; copy the values if they must outlive it.
The plugin emits class indices, not human-readable class names or tracking IDs.

## Diagnostics and limitations

Enable per-frame timing with the same image pipeline:

```bash
GST_DEBUG_NO_COLOR=1 GST_DEBUG=rknnyolodetect:6 \
gst-launch-1.0 -q \
  filesrc location="$HOME/gst-yolo-demo/assets/bus.jpg" ! \
  jpegdec ! videoconvert ! video/x-raw,format=RGB ! \
  rknnyolodetect model-path="$HOME/gst-yolo-demo/models/yolov8n.rknn" ! \
  metaprint ! fakesink
```

The `rga`, `rknn`, `postprocess`, and `metadata` timings cover their respective
processing stages, excluding upstream decoding and downstream display. The
preprocessing interval also includes any row copying and frame unmapping; the
`rga` label includes CPU resizing after fallback.

Missing/invalid models fail startup with a GStreamer error. Frame mapping,
RKNN, CPU resize, and metadata-attachment failures post a bus error and return
`GST_FLOW_ERROR`. Reported RGA fill/resize failures switch to CPU preprocessing
for that instance until stop/restart; they do not stop the pipeline.
This move does not change the existing detector's model support or runtime
property semantics.

Validation of the initial move: the source matched the original byte-for-byte;
Release cross-build passed with GCC 12 and the existing sysroot. The AArch64
library loaded through `gst-inspect-1.0` on the Radxa. The bus-image pipeline
completed successfully and printed three class-0 detections and one class-5
detection. Test copies are in `/home/radxa/rknnyolodetect-refactor-check/`;
the existing demo plugin directory was not overwritten by validation.


## Check CPU resize fallback

With the updated plugin directory in `GST_PLUGIN_PATH`, run on the board:

```bash
bash check-fallback.sh "$HOME/gst-yolo-demo/models/yolov8n.rknn"
```

Copy `src/rknnyolodetect/check-fallback.sh` to the board first. The check sends
three 16 × 16 frames: resizing to 640 × 640 exceeds this RK3566 RGA's scaling
limit. It requires successful pipeline completion and exactly one fallback
warning, verifying that subsequent frames keep using CPU preprocessing.

After adding fallback, the Release cross-build and this three-frame failure-path
check passed on the Radxa. The normal RGA bus-image run still produced the same
three person detections and one bus detection as before the change.
