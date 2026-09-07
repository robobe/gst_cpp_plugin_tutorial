# NanoTrack RKNN GStreamer plugin

Status: design agreed with the user after the grill-me session.

## Goal and files

Turn the NanoTrack algorithm in `radxa/nano/nanotrack_rknn_rga.cpp` into a
synchronous, single-object GStreamer metadata filter named `rknnnanotrack`.
Put the plugin source, CMake project, README, and runnable integration check in
`src/nanotracker/`. Preserve the standalone example. Extend `src/metaprint.cpp`
to print tracker results. Reuse `cmake/toolchains/radxa-zero3w.cmake` and
`/home/user/sysroot` for cross compilation.

## Public contract

- Input/output: CPU-mappable `video/x-raw,format=BGR`, with row stride respected.
  Pixels, timestamps, and existing metadata are preserved. No display or drawing.
- `enabled`: boolean, default false, mutable during playback. Disabled frames
  pass through without new tracker metadata. Re-enabling initializes a new
  template from the stored ROI on the next frame.
- `roi`: atomic string `x,y,width,height`, mutable during playback. Coordinates
  refer to the input frame. Each assignment requests a fresh template on the
  next active frame, including assignment of the same rectangle. Validate four
  finite values, positive dimensions, and containment in the current frame.
  Enabling without a valid ROI is a pipeline error. Use `enabled=false` to stop
  tracking, not an empty ROI command.
- `models-dir`: directory of the existing three-model filename sets. Required
  for active tracking; no models are needed for disabled passthrough.
- `precision`: fp16, mixed (default), int8, int8-mmse; same filename mapping as
  the standalone application. Mixed uses unsuffixed backbones and an INT8 head.
- `resize`: auto (default) or cpu. Auto tries RGA and falls back permanently to
  CPU resize for that running instance after recoverable RGA failure.
- Model/precision/resize settings may change only in NULL or READY, with actual
  rejection of changes during PAUSED/PLAYING. Mutable ROI and enable updates
  are synchronized and take effect at frame boundaries.

## State and output

Keep model contexts and buffers per plugin instance; never share tracker state
or RGA fallback flags across instances. Load models lazily on the first enabled
frame; release them on stop. Each initialization captures a template and the
current frame's mean padding color. Subsequent frames run search backbone and
head, then the existing penalties, Hann window, and smoothed box update.

Reset the template on ROI assignment, enable transitions, stream restart,
seek/segment reset, discontinuous input, and frame-size changes. The next active
frame initializes the stored ROI; if it no longer fits, report a pipeline error.
Do not treat normal frame progression as reinitialization.

Attach one `GstVideoRegionOfInterestMeta` with ROI type `nanotrack` and a
`GstStructure` parameter named `nanotrack`. Rectangle fields are clipped to
frame bounds. Parameters:

- `initialized` (boolean): true only on the template initialization frame.
- `confidence` (double): present only after a search/head match has run.

The initialization frame reports the supplied rectangle. Later frames report
the decoded tracking box. There is no class ID, automatic confidence cutoff,
lost-target state, detection, or reacquisition. Low confidence remains visible
to the application, which can disable tracking or supply another ROI.

## Safety and integration

Follow `src/rknnyolodetect/rknnyolodetect.cpp` for GStreamer registration, video mapping, ROI
metadata attachment, and error reporting. Catch C++ exceptions at GStreamer
callbacks, post `GST_ELEMENT_ERROR`, and return a failed flow/state transition.
Do not import the executable's signal handlers, process exits, CLI, CSV, GUI,
or VideoCapture into the plugin. Validate the fixed model tensor contract and
use stable softmax and finite-value checks before constructing metadata.

Use the existing RKNN shared-memory/RGA path where the actual board runtime
supports it; verify real inference, not just successful compilation. A
recoverable RGA import/resize failure falls back to CPU. Other model, mapping,
or processing failures stop the pipeline with an error rather than stale boxes.

## Build and validation

Build `libgstrknnnanotrack.so` and the existing `libgstmetaprint.so` through the
new standalone CMake project, using GStreamer core/base/video, OpenCV core and
imgproc, RKNN, and RGA. Keep the plugin free of OpenCV GUI/video capture
requirements. Do not embed host sysroot RPATHs.

Cross-build on the PC, verify AArch64 ELF, and load with `gst-inspect-1.0` on the
Radxa. Leave a runnable integration check for disabled passthrough, unchanged
pixels/timestamps, initialization and tracked metadata, runtime ROI updates,
disable/re-enable, reset events/resolution changes, invalid ROI and model
errors, and independent plugin instances. Exercise CPU and auto resize and all
available precision filename sets. Update the README with exact commands,
property semantics, metadata access, and actual validation limits.

## Implementation result

Implemented in `src/nanotracker/gstrknnnanotrack.cpp`, with the standalone
CMake project, README, and `check.cpp`. Updated `src/metaprint.cpp` to recognize
`nanotrack` parameter structures in addition to YOLO metadata. No changes to
the standalone tracker or cross toolchain were needed for this implementation.

Cross-build and board plugin inspection passed. Real-model integration passed
for fp16/mixed/int8/int8-mmse with CPU and auto resize, covering the controls and
reset/error behavior above. Shared input buffers preserved upstream metadata
and pixels. A 1 × 1 ROI exercised recoverable RGA scaling failure and CPU
fallback. The available decoded video ran to EOS with metadata output; tracking
accuracy and performance are not established by these functional checks.
