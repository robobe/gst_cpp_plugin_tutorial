# CPU NanoTrack GStreamer plugin

`cpunanotrack` follows one selected object on a Linux x86-64 PC using ONNX
Runtime CPU inference and OpenCV CPU resizing. It keeps the ROI/enable controls
and metadata contract of [rknnnanotrack](../nanotracker/README.md). No Radxa,
RKNN, RGA, CUDA, or GPU device is required. The plugin adds metadata; it does
not draw boxes or open a window.

See the [agreed design](../../docs/design/cpunanotracker-plugin.md).

## Build and inspect

Run these commands on the PC from the repository root:

```bash
cmake -S src/cpunanotracker -B build-cpunanotracker -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpunanotracker
export GST_PLUGIN_PATH="$PWD/build-cpunanotracker${GST_PLUGIN_PATH:+:$GST_PLUGIN_PATH}"
gst-inspect-1.0 cpunanotrack
```

This is a native build: do not pass the Radxa toolchain or sysroot. Dependencies
are a C++17 compiler, CMake, Ninja, pkg-config, GStreamer core/base/video
development files, OpenCV core/imgproc, and ONNX Runtime. The repository already
contains `third_party/onnxruntime-linux-x64-1.29.0`, which CMake selects by
default. No OpenCV highgui/videoio or Python package is needed by this plugin.
The root CMake project does not build this target; use the command above.

Outputs:

| File | Purpose |
| --- | --- |
| `libgstcpunanotrack.so` | GStreamer factory `cpunanotrack`. |
| `libgstmetaprint.so` | Existing reader for NanoTrack, YOLO, and tutorial metadata. |
| `cpunanotracker-check` | Local real-model integration check. |

The build retains a native library RUNPATH to find the bundled ONNX Runtime
when GStreamer's plugin scanner loads it. Keep that SDK directory available.
When moving the plugin to another machine, supply compatible libraries and
set their loader search path or rebuild there. To select another SDK, configure
a fresh build directory with `-DONNXRUNTIME_ROOT=/absolute/path/to/onnxruntime`.

## Run tracking

### Simple usage

Build the plugin from the repository root:

```bash
cmake -S src/cpunanotracker -B build-cpunanotracker -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpunanotracker
```

Set the plugin path and verify it:

```bash
export GST_PLUGIN_PATH="$PWD/build-cpunanotracker"
gst-inspect-1.0 cpunanotrack
```

Run tracking on the sample video. Replace the ROI with your target's
`x,y,width,height` in the first decoded frame:

```bash
gst-launch-1.0 -q \
  filesrc location="$PWD/assets/camera_run_2s_10s.mp4" ! \
  decodebin ! videoconvert ! video/x-raw,format=BGR ! \
  cpunanotrack enabled=true roi="100,80,60,90" \
    models-dir="$PWD/demos/nanotracker/onnx" ! \
  metaprint ! fakesink
```

The first result is initialization metadata. Following results contain the
tracked rectangle and confidence. To pass frames through without tracking,
leave `enabled` at its default or set it to `false`:

```text
cpunanotrack enabled=false
```

The existing model directory is ready to use. Models are loaded when the
element enters PLAYING, and the load duration is logged at `GST_INFO` level.
First try a short synthetic
sequence to verify plugin loading and metadata output:

```bash
gst-launch-1.0 -q videotestsrc num-buffers=3 ! \
  video/x-raw,format=BGR,width=320,height=240 ! \
  cpunanotrack enabled=true roi="80,60,64,64" \
    models-dir="$PWD/demos/nanotracker/onnx" ! \
  metaprint ! fakesink
```

For the sample video, replace the example ROI with the desired target's
`x,y,width,height` in the first decoded frame, measured in input pixels:

```bash
gst-launch-1.0 -q \
  filesrc location="$PWD/assets/camera_run_2s_10s.mp4" ! \
  decodebin ! videoconvert ! video/x-raw,format=BGR ! \
  cpunanotrack enabled=true roi="100,80,60,90" \
    models-dir="$PWD/demos/nanotracker/onnx" ! \
  metaprint ! fakesink
```

`fakesink` runs headlessly. The first active frame emits the initial rectangle
with `initialized=true`; subsequent frames emit tracking rectangles and
confidence. Pixels and timestamps are preserved. No annotated video or CSV
file is written. For an unannotated display, replace `fakesink` with
`videoconvert ! autovideosink` in an environment with a display.

A disabled plugin passes frames through without loading any models:

```bash
gst-launch-1.0 -q videotestsrc num-buffers=3 ! \
  video/x-raw,format=BGR,width=320,height=240 ! cpunanotrack ! fakesink
```

Input is progressive, CPU-mappable BGR video, 10–16,384 pixels on each axis.
Mapped row strides are honored. Use `videoconvert` upstream for other formats.
Cropping may add context outside the frame using the initialization frame's
mean color; an allocated crop side greater than 16,384 is rejected.

## Properties and runtime control

| Property | Default | When writable | Meaning |
| --- | --- | --- | --- |
| `enabled` | false | Any state | Start/stop tracking. Enabling initializes the stored ROI on the next frame. |
| `roi` | Empty | Any state | Atomic `x,y,width,height` string. Every assignment requests a fresh template, including repeated values. |
| `models-dir` | Empty | NULL / READY | Directory containing the three ONNX files below; required when enabled. |

The plugin has no `precision` or `resize` property. All sessions run on the CPU,
and all image resizing uses OpenCV. Setting `models-dir` during PAUSED/PLAYING
is rejected with a warning; return to READY before changing it.

In an application, with a reference to the element:

```cpp
// Configure the complete ROI before enabling.
g_object_set(tracker, "roi", "100,80,60,90", "enabled", TRUE, nullptr);
// While running, choose a new target.
g_object_set(tracker, "roi", "200,120,48,64", nullptr);
// Keep video flowing without adding new tracking metadata.
g_object_set(tracker, "enabled", FALSE, nullptr);
// Capture a new template from the stored ROI.
g_object_set(tracker, "enabled", TRUE, nullptr);
```

ROI values must be four finite numbers, x/y nonnegative, width/height at least
one pixel, and the entire rectangle inside the current frame. Fractional values
and whitespace are allowed; trailing non-whitespace text is rejected. An empty
or invalid ROI while enabled is a pipeline error. Disable with `enabled=false`,
not an empty ROI. An already-enabled `enabled=true` assignment does not reset.

A per-instance mutex serializes property access and processing; setters may
wait for the current inference to finish. The ROI property is atomic, but a
multi-property `g_object_set` is not a single transaction. Already-emitted
frames are unaffected by later changes.

ROI assignments, enable transitions, new streams/segments (including seeks),
flush-stop, DISCONT buffers, and frame-size changes reset the template. The
next active frame initializes from the stored ROI. If it no longer fits the
frame, processing fails. Each instance has independent sessions and state.
Stopping the element releases models; disabling keeps them for later reuse.

## Models and implementation

The following existing files are loaded from `models-dir`. They must be
FLOAT32 graphs with these fixed NCHW shapes:

| File | Input | Output | When run |
| --- | --- | --- | --- |
| `nanotrack_backbone_template.onnx` | `[1,3,127,127]` BGR pixels | `[1,96,8,8]` features | Once per initialization. |
| `nanotrack_backbone.onnx` | `[1,3,255,255]` BGR pixels | `[1,96,16,16]` features | Every subsequent active frame. |
| `nanotrack_head.onnx` | Template `[1,96,8,8]`, search `[1,96,16,16]` | Classification `[1,2,15,15]`, distances `[1,4,15,15]` | Every subsequent active frame. |

`Model` queries input/output names, validates counts/types/shapes, and owns its
ONNX session and output tensors. `Tracker` owns the environment and three
models. Template output stays alive until the next initialization. Backbone
feature buffers are passed to the head as CPU tensor views for its synchronous
run; output memory is owned by ONNX Runtime.

`crop()` builds a mean-padded BGR square centered on the previous target,
resizes with `INTER_LINEAR`, converts UINT8 to FLOAT32 without normalization,
and splits interleaved channels into contiguous NCHW. Pixel values remain
0–255; there is no RGB swap. The head compares the template and search features.
The tracking algorithm uses the same stable softmax, 15 × 15 grid, stride 16,
scale/aspect penalty (0.138), Hann window blend (0.455), and size update factor
(0.348) as the Radxa plugin. Output coordinates are restored to source pixels.

No accelerated provider is registered. ONNX Runtime uses its default CPU
execution provider, as described in the official
[execution-provider documentation](https://onnxruntime.ai/docs/execution-providers/).
Sessions use one intra-op and one inter-op thread to avoid creating three
machine-sized thread pools. OpenCV may use its own CPU workers. These are
implementation defaults, not a guarantee of one process thread or a performance
benchmark.

Processing is synchronous and no frames are deliberately dropped by the
plugin. File playback can run slower than real time. For a live source that
should prefer recent frames, place this upstream of `cpunanotrack`:

```text
queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream
```

Large frame-to-frame jumps can still cause tracking loss. Low confidence is
reported without automatic disabling, reacquisition, or object detection.

## Metadata

Metadata matches the Radxa plugin, so the existing `metaprint` works unchanged:

| Field | Value |
| --- | --- |
| Meta type | `GstVideoRegionOfInterestMeta` |
| ROI type | `nanotrack` |
| Rectangle | `x,y,w,h` in input pixels, clipped to frame bounds; floor left/top, ceil right/bottom. |
| Parameter structure | `nanotrack` |
| `initialized` | Boolean, true only on the template-initialization frame. |
| `confidence` | Double in [0,1], omitted on the initialization frame. |

No class ID or tracking ID is assigned. Existing upstream metadata remains
attached. Read the result from a pad probe or appsink sample:

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
    // Use roi->x/y/w/h and GST_BUFFER_PTS(buffer).
    // Confidence is valid only when has_confidence is TRUE.
}
```

The buffer owns the metadata pointers. Copy values before releasing the buffer
if they are needed later. Missing/invalid models fail the PLAYING transition;
bad ROIs, mapping failures, and invalid model outputs post GStreamer errors and
stop processing. The plugin
never installs signal handlers or terminates the hosting process deliberately.

## Tests and troubleshooting

Run the integrated check locally:

```bash
ctest --test-dir build-cpunanotracker --output-on-failure
```

Or run it directly with a model directory:

```bash
GST_PLUGIN_PATH="$PWD/build-cpunanotracker" \
  ./build-cpunanotracker/cpunanotracker-check "$PWD/demos/nanotracker/onnx"
```

The check uses deterministic synthetic frames and real CPU inference. It covers
disabled operation without models, shared buffer headers, unchanged pixels and
timestamps, preserved upstream ROI metadata, padded row strides, initialization
and matching, live ROI/enable updates, repeated ROI assignment, stream resets,
resolution changes, stop/restart, independent instances, malformed ROIs, and
missing model errors. Assertions remain active in Release builds. `appsrc` and
`appsink` must be installed for this test; no Python test dependency is needed.

Use `GST_DEBUG=cpunanotrack:4` to see the model-load message, or
`GST_DEBUG=cpunanotrack:6` for per-frame state/box/confidence logs. The startup
message looks like:

```text
cpunanotrack: Loaded NanoTrack ONNX models from /path/to/models in 184.532 ms
```

The model-loading time is paid during the state transition before the first
frame. The first frame still spends time creating the template features, but
it no longer constructs the ONNX sessions. If
`gst-inspect-1.0` cannot find the plugin, check `GST_PLUGIN_PATH` and
`ldd build-cpunanotracker/libgstcpunanotrack.so` for missing libraries. If model
loading fails, check the `.onnx` filenames and tensor contract rather than
pointing it at the Radxa `.rknn` directory.

Validation performed on this PC: Release build and plugin inspection passed;
CTest passed with ONNX Runtime 1.29.0, GStreamer 1.24.2, and OpenCV 4.6.0. Two
matching frames were compared with the existing Python CPU tracker: metadata
boxes agreed within one pixel and confidence within `1e-4`. The sample video
ran through EOS with DISPLAY/WAYLAND_DISPLAY unset, producing 240 metadata
records with one initialization. This is functional validation, not a general
tracking-accuracy or real-time performance guarantee.
