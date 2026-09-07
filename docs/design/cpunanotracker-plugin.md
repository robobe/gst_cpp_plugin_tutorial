# CPU NanoTrack GStreamer plugin

Status: agreed after the grill-me session; implementation authorized.

Create a separate `cpunanotrack` GStreamer plugin under `src/cpunanotracker/`,
with source, native CMake project, README, and runnable integration check.
Target this Linux x86-64 PC. Reuse the bundled ONNX Runtime and the three
NanoTrack ONNX models in `demos/nanotracker/onnx/`. Use only ONNX Runtime's CPU
execution provider and OpenCV CPU resizing; no RKNN, RGA, CUDA, or GPU providers.

Preserve the existing RKNN tracker's BGR input, one-object tracking algorithm,
unchanged pixels/timestamps, `enabled` and atomic `roi` runtime properties, and
`models-dir` setting. Default disabled. When `models-dir` is set, model loading
happens during the element's start transition before the first frame, and its
duration is logged. `models-dir` is writable only in NULL/READY. Model filenames are
`nanotrack_backbone_template.onnx`, `nanotrack_backbone.onnx`, and
`nanotrack_head.onnx`. Omit precision and resize properties.

Initialize from the configured ROI on the next active frame after enable,
ROI assignment, discontinuity, new segment/seek, stream restart, flush-stop,
or frame-size change. Reassigning the same ROI resets; setting enabled true
when already enabled does not. Validate finite positive in-frame ROIs; invalid
ROI/model/frame processing produces a GStreamer error. Keep tracking at low
confidence, with no automatic reacquisition or confidence cutoff.

Publish the same `GstVideoRegionOfInterestMeta`: ROI type and parameter
structure `nanotrack`, clipped integer rectangle, boolean `initialized`, and
confidence only after matching. Existing `metaprint` reads it without changes.
Process every frame synchronously. Live applications may put a leaky queue
upstream when freshness matters; the plugin itself does not skip frames.

Use NCHW FLOAT32 BGR pixels in range 0–255 for the ONNX backbones, and NCHW
FLOAT32 feature tensors for the head. Validate fixed tensor shapes and cache
template features until reinitialization. Preserve the existing penalties,
Hann window, coordinate restoration, size smoothing, and stable softmax.
Own sessions, CPU buffers, and tracking state per element instance.

Validate locally with the actual models: passthrough without models, unchanged
pixels/timestamps/upstream metadata, padded row strides, initialization and
matching output, runtime controls, stream resets and resizing, invalid inputs,
stopped-only properties, and independent instances. Build a real-video pipeline
as a smoke test; distinguish functional testing from accuracy/performance claims.

## Implementation and verification

Implemented `gstcpunanotrack.cpp`, native `CMakeLists.txt`, `README.md`, and
`check.cpp` under `src/cpunanotracker/`. The check is registered with CTest.
The Radxa plugin and shared metadata reader required no modifications.

Native Release build, plugin inspection, and real-model integration checks
passed. Two matching frames agreed with the Python CPU reference within one
pixel for metadata boxes and `1e-4` for confidence. A headless sample-video run
completed at EOS with 240 metadata records and one initialization. The sessions
register no accelerated execution providers and use CPU tensor memory.
