# RKNN NanoTrack GStreamer plugin

`rknnnanotrack` follows one user-selected object and attaches its rectangle to
each active video buffer. It adapts the algorithm and RKNN/RGA memory path from
[the standalone tracker](../../radxa/nano/nanotrack_rknn_rga.cpp), using the
standard ROI metadata mechanism already used by
[rknnyolodetect](../rknnyolodetect/rknnyolodetect.cpp).

The [agreed design](../../docs/design/nanotracker-plugin.md) records the behavior
decided in the planning session. This directory contains the plugin source,
standalone CMake project, and a runnable integration check. The standalone
tracker is unchanged.

## Quick start: build, deploy, and track

Follow these steps in order; the complete commands are in the sections below:

1. On the PC, from the repository root, [cross-build](#cross-build-on-the-pc)
   using the GCC 12 toolchain and `/home/user/sysroot`.
2. [Copy the plugins and models](#deploy-and-inspect) to the Radxa board.
3. Connect with `ssh radxa`, export `GST_PLUGIN_PATH`, and check that
   `gst-inspect-1.0 rknnnanotrack` finds the plugin.
4. Run the [real-video pipeline](#run-a-pipeline) with `enabled=true` and
   `roi="x,y,width,height"`. Replace the example rectangle with the target's
   coordinates in the first decoded frame, measured in pixels.
5. Read the results printed by `metaprint`: the first active frame has
   `initialized=true` and no confidence; later frames contain the tracked box
   and confidence. The plugin attaches metadata and does not draw a box.

Start with `precision=mixed resize=auto`. To bypass RGA resize, use `resize=cpu`.
Tracking defaults to disabled, so remember `enabled=true` when using
`gst-launch-1.0`. To change the target or toggle tracking during playback from
your application, use the [C++ property examples](#properties-and-state-transitions).

## Cross-build on the PC

Use the existing board sysroot and GCC 12 toolchain. No additional sysroot files
were needed for this plugin. See the
[sysroot and cross-compilation guide](../../radxa/nano/CROSS_COMPILING.md) for
its setup and the reason for selecting the board's C/C++ runtime libraries.

From the repository root:

```bash
cmake -S src/nanotracker -B build-radxa-nanotracker -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/radxa-zero3w.cmake" \
  -DRADXA_SYSROOT=/home/user/sysroot \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-radxa-nanotracker
file build-radxa-nanotracker/libgstrknnnanotrack.so
```

Outputs are `libgstrknnnanotrack.so`, `libgstmetaprint.so`, and
`nanotracker-check`. The check is a host-built AArch64 executable to run on the
board, not on the x86-64 PC. Build in a fresh directory if compiler/sysroot
settings change. Native board compilation uses the same commands without the
toolchain and sysroot arguments, when all development packages are available.

Dependencies: C++17, GStreamer core/base/video, OpenCV core/imgproc, librga, and
librknnrt. The plugin does not depend on OpenCV highgui or videoio. GStreamer
handles capture, decoding, scheduling, and display. The check additionally needs
the board's `appsrc` and `appsink` elements at runtime; no gst-app development
headers are required. No host sysroot RPATH is embedded in the binaries.

## Deploy and inspect

Run on the PC:

```bash
ssh radxa 'mkdir -p ~/nanotracker-plugin-check/models'
scp build-radxa-nanotracker/libgstrknnnanotrack.so \
    build-radxa-nanotracker/libgstmetaprint.so \
    build-radxa-nanotracker/nanotracker-check \
    radxa:nanotracker-plugin-check/
scp radxa/nano/models/*.rknn radxa:nanotracker-plugin-check/models/
```

Connect from the PC:

```bash
ssh radxa
```

Then run on the board:

```bash
export GST_PLUGIN_PATH="$HOME/nanotracker-plugin-check${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
gst-inspect-1.0 rknnnanotrack
gst-inspect-1.0 metaprint
```

The factory is `rknnnanotrack`; its library is `libgstrknnnanotrack.so`.
Check the filename printed by `gst-inspect-1.0 metaprint` if an older installed
copy hides the updated reader. The plugin works with the existing model files;
see the [nine-model descriptions](../../radxa/nano/README.md#included-model-files).

## Run a pipeline

A disabled plugin passes frames through without loading models or adding
tracking metadata:

```bash
gst-launch-1.0 -q videotestsrc num-buffers=10 ! \
  video/x-raw,format=BGR,width=320,height=240 ! \
  rknnnanotrack ! fakesink
```

An active synthetic-video smoke test:

```bash
gst-launch-1.0 -q videotestsrc num-buffers=10 ! \
  video/x-raw,format=BGR,width=320,height=240 ! \
  rknnnanotrack enabled=true roi="80,60,64,64" \
    models-dir="$HOME/nanotracker-plugin-check/models" precision=mixed resize=cpu ! \
  metaprint ! fakesink
```

For a real video, replace the file and ROI with the desired target's coordinates
in the first decoded frame. This path exists on the board used for validation:

```bash
gst-launch-1.0 -q \
  filesrc location="$HOME/projects/rknn_demo2/assets/camera_run_2s_10s.mp4" ! \
  decodebin ! videoconvert ! video/x-raw,format=BGR ! \
  rknnnanotrack enabled=true roi="100,80,60,90" \
    models-dir="$HOME/nanotracker-plugin-check/models" precision=mixed resize=auto ! \
  metaprint ! fakesink
```

The plugin accepts progressive, CPU-mappable BGR video, with dimensions from
10 to 16,384 on each axis. It honors mapped row strides, including padded rows.
Use `videoconvert` for other pixel formats and `deinterlace` for interlaced input.
Direct NV12/DMABuf-only negotiation is not implemented. Source pixels,
timestamps, and existing metadata are preserved; the plugin adds its own ROI.
No box is drawn into the image and no CSV/video file is written.

## Properties and state transitions

| Property | Default | When writable | Meaning |
| --- | --- | --- | --- |
| `enabled` | `false` | Any state | Enable tracking using the stored ROI. A false-to-true transition initializes a fresh template on the next frame. |
| `roi` | Empty | Any state | Atomic string `x,y,width,height` in input pixels. Every assignment requests initialization on the next active frame, even if unchanged. |
| `models-dir` | Empty | NULL / READY | Directory containing the selected three RKNN model files. Required when tracking is enabled. |
| `precision` | `mixed` | NULL / READY | String: `fp16`, `mixed`, `int8`, or `int8-mmse`. Selects filenames, not runtime quantization. |
| `resize` | `auto` | NULL / READY | String: `auto` uses RGA with CPU fallback; `cpu` uses OpenCV resize. Both use RKNN inference. |

Changing a stopped-only property while PAUSED/PLAYING is rejected with a
GStreamer warning and retains the previous value. Invalid precision/resize
strings fail the transition that starts processing. Model loading is lazy on
the first enabled frame, so a disabled pipeline needs neither a model directory
nor an ROI. Models and tensor memory remain allocated while disabled and are
released when the element stops (PAUSED to READY).

ROI values must be exactly four finite numbers separated by commas, with
nonnegative x/y, dimensions at least one pixel, and full containment in the
current image. Fractional values are accepted. Whitespace is allowed; trailing
non-whitespace text is rejected. The ROI is validated when an active frame is
initialized. An invalid ROI, including empty ROI while enabled, produces a
pipeline error. Set `enabled=false` to stop tracking; an empty ROI is not a
stop command.

Example application control, with an existing element reference:

```cpp
// Set ROI first, then enable, so the next frame has a complete target selection.
g_object_set(tracker, "roi", "100,80,60,90", "enabled", TRUE, nullptr);
// While running, this captures a new template on the next frame.
g_object_set(tracker, "roi", "200,120,48,64", nullptr);
g_object_set(tracker, "enabled", FALSE, nullptr); // Video continues without new tracker metadata.
g_object_set(tracker, "enabled", TRUE, nullptr);  // Reinitialize the stored ROI.
```

The ROI string is updated atomically; separate `g_object_set` properties are
not a combined transaction. A per-instance mutex serializes setters and frame
processing, so a property call may wait for the current inference to finish.
Disabling does not retract metadata already emitted or a frame already being
processed. Reassigning `enabled=true` while already enabled does not reset;
reassign the ROI when an explicit reset is needed.

A new stream, new segment (including a seek), flush-stop, DISCONT buffer, or
frame-size change resets the template. The next enabled frame uses the stored
ROI and its own mean color for padding. If the ROI no longer fits the new
frame, processing fails. A continuous segment with ordinary frames keeps its
template. Each plugin instance owns its models, position, size, and RGA fallback
state independently.

## Metadata contract

Each active frame receives one `GstVideoRegionOfInterestMeta`:

| Field | Value |
| --- | --- |
| `roi_type` | Quark for `nanotrack` |
| `x`, `y`, `w`, `h` | Integer rectangle in input-frame pixels, clipped to frame bounds; floor left/top and ceil right/bottom. |
| Parameter structure name | `nanotrack` |
| `initialized` | Boolean: true on the initialization frame; false on subsequent matching frames. |
| `confidence` | Double in [0,1], present only on matching frames. |

The initialization frame reports the supplied ROI and no confidence because
only the template backbone ran. Matching frames report the selected foreground
probability before penalty/window weighting. There is no class ID. Low
confidence does not disable tracking or omit its ROI: downstream application
logic decides whether to disable or select another target.

Read metadata from an appsink sample or a downstream pad probe:

```cpp
gpointer state = nullptr;
while (GstMeta* meta = gst_buffer_iterate_meta_filtered(
           buffer, &state, GST_VIDEO_REGION_OF_INTEREST_META_API_TYPE)) {
    auto* roi = reinterpret_cast<GstVideoRegionOfInterestMeta*>(meta);
    if (roi->roi_type != g_quark_from_static_string("nanotrack")) continue;
    GstStructure* params = gst_video_region_of_interest_meta_get_param(roi, "nanotrack");
    if (!params) continue;
    gboolean initialized = FALSE;
    gdouble confidence = 0;
    gst_structure_get_boolean(params, "initialized", &initialized);
    gboolean has_confidence = gst_structure_get_double(params, "confidence", &confidence);
    // roi->x/y/w/h and GST_BUFFER_PTS(buffer) identify the result on this frame.
    // Only use confidence when has_confidence is TRUE.
}
```

Pointers into the metadata remain owned by the buffer. Copy values if you need
them after releasing the sample/buffer. See the official
[GStreamer ROI metadata reference](https://gstreamer.freedesktop.org/documentation/video/gstvideometa.html#GstVideoRegionOfInterestMeta)
for the standard structure and parameter API.

The updated `metaprint` also preserves its YOLO and existing custom-metadata
support. Tracker output looks like:

```text
metaprint: roi=nanotrack initialized=true x=80 y=60 width=64 height=64 pts=0:00:00.000000000
metaprint: roi=nanotrack initialized=false x=84 y=60 width=58 height=69 confidence=0.971863 pts=0:00:00.033333333
```

Different images, models, and resize implementations can give different boxes.

## Implementation and error handling

`Model` owns each RKNN context and its allocated/imported buffers. Construction
validates model I/O counts and the fixed NanoTrack shapes before binding memory:
127 × 127 template input, 255 × 255 search input, 96 × 8 × 8 and 96 × 16 × 16
features, and 2/4 × 15 × 15 head outputs. Backbones request UINT8 NHWC BGR image
input and FLOAT32 NHWC features. The head imports those feature DMA-BUFs and
requests FLOAT32 NCHW output. Its imports are destroyed before the backbones.

`Tracker` maintains center, size, template features, mean padding color, and a
15 × 15 Hann window. CPU code constructs a mean-padded crop around the previous
position; RGA or OpenCV resizes it into the aligned RKNN image buffer. Tracking
uses the same scale/aspect penalties and constants as the standalone program:
context 0.5, stride 16, penalty 0.138, window weight 0.455, update rate 0.348.
Softmax subtracts the maximum logit for stability. Invalid/non-finite output
values fail processing rather than escaping into integer metadata coordinates.

RGA imports/resizes that report failure switch that instance to CPU resizing
until the element stops. Reinitializing an ROI does not retry RGA. Synchronizing
RKNN memory, loading models, mapping frames, or inference failures produce a
GStreamer bus error and `GST_FLOW_ERROR`. The plugin installs no process signal
handlers and never calls an exit function. Native driver/process faults cannot
be converted into C++ exceptions or safely recovered by the plugin.

The element subclasses `GstBaseTransform` and transforms in place, without
passthrough optimization suppressing its metadata callback. See the official
[base-transform documentation](https://gstreamer.freedesktop.org/documentation/base/gstbasetransform.html)
for the framework's buffer-processing lifecycle.

Enable diagnostic logs with:

```bash
GST_DEBUG=rknnnanotrack:6 gst-launch-1.0 -q \
  videotestsrc num-buffers=3 ! video/x-raw,format=BGR,width=320,height=240 ! \
  rknnnanotrack enabled=true roi="80,60,64,64" \
    models-dir="$HOME/nanotracker-plugin-check/models" ! fakesink
```

## Runnable validation

On the board, after exporting `GST_PLUGIN_PATH`:

```bash
~/nanotracker-plugin-check/nanotracker-check \
  ~/nanotracker-plugin-check/models mixed cpu
```

The check uses actual RKNN models with deterministic synthetic BGR frames. It
checks disabled operation without models, unchanged bytes and timestamps,
preservation of upstream ROI metadata, padded rows, initialization/matching
fields, ROI assignment including repeated values, disable/re-enable, DISCONT,
segment/flush/stream resets, resizing, stopped-only properties, independent
instances, stop/restart, malformed/out-of-bounds ROI, and missing model errors.
Failures return a nonzero exit status; checks remain active in Release builds.

Exercise every provided filename set and both resize paths:

```bash
for precision in fp16 mixed int8 int8-mmse; do
  for resize in cpu auto; do
    ~/nanotracker-plugin-check/nanotracker-check \
      ~/nanotracker-plugin-check/models "$precision" "$resize" || exit 1
  done
done
```

Synthetic integration checks establish control/metadata behavior, not tracking
accuracy on real objects. Inspect trajectories on representative video before
using confidence as an application decision. There is no automatic object
detection, lost-target recovery, multiclass labeling, or multi-object tracking.

Validation performed on the connected RK3566 board: Release cross-build and
`gst-inspect-1.0` passed; the integration check passed for all four precision
modes with both CPU and auto resize. Additional checks passed with shared input
buffer headers and a 1 × 1 ROI that forced RGA's scaling-limit error and CPU
fallback. A decoded `camera_run_2s_10s.mp4` pipeline ran through EOS and printed
tracking metadata. These runs establish functional execution, not a measured
accuracy or performance claim. The tested libraries and check executable are
in `/home/radxa/nanotracker-plugin-check/` on the board; tests used its existing
models in `/home/radxa/projects/rknn_demo2/rknn/models/`.
