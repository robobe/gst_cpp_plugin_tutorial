# NanoTrack RKNN model sets

This directory contains the compiled RKNN artifacts used by
[`nanotrack_rknn_rga`](../README.md). The files are for the Radxa RK3566
runtime; the original ONNX files are in
[`demos/nanotracker/onnx`](../../../demos/nanotracker/onnx/).

The application loads exactly three files for each run: a template backbone, a
search backbone, and a tracking head. Choose the set with `--precision`:

| `--precision` | Template backbone | Search backbone | Head | What it means |
| --- | --- | --- | --- | --- |
| `fp16` | `nanotrack_backbone_template.rknn` | `nanotrack_backbone.rknn` | `nanotrack_head.rknn` | Floating-point RKNN graphs. Best starting point for accuracy comparison. |
| `mixed` (default) | `nanotrack_backbone_template.rknn` | `nanotrack_backbone.rknn` | `nanotrack_head_int8.rknn` | Floating-point backbones with an INT8 head. A compromise that leaves image feature extraction unchanged while reducing head cost. |
| `int8` | `nanotrack_backbone_template_int8.rknn` | `nanotrack_backbone_int8.rknn` | `nanotrack_head_int8.rknn` | Ordinary INT8 variants for all three graphs. Lowest-precision end-to-end candidate. |
| `int8-mmse` | `nanotrack_backbone_template_int8_mmse.rknn` | `nanotrack_backbone_int8_mmse.rknn` | `nanotrack_head_int8_mmse.rknn` | INT8 variants built with an MMSE-labelled calibration/scale-selection pass. Compare accuracy with ordinary INT8; the runtime flag does not perform calibration. |

Run from the repository root with:

```bash
./build-radxa-nano/nanotrack_rknn_rga \
  --video assets/camera_run_2s_10s.mp4 \
  --models radxa/nano/models \
  --precision mixed \
  --roi 100,80,60,90 \
  --no-display
```

## What the quantization names mean

### FP16

FP16 stores floating-point values with 16-bit precision instead of FP32's
32-bit precision. It uses substantially less model memory and can be faster on
hardware with FP16 support, while retaining a much wider numeric range than
INT8. The RKNN conversion setting describes compiled graph arithmetic; the C++
program still submits its image buffers as UINT8 BGR and requests FLOAT32
feature/head buffers. The runtime performs any required boundary conversion.

Use `fp16` first as the accuracy baseline. It is the least aggressive reduced-
precision option in this directory, but it is still not guaranteed to match the
original FP32 ONNX result bit-for-bit.

### INT8

INT8 represents a real value with an 8-bit integer, scale, and zero point:

```text
quantized = clamp(round(real / scale) + zero_point)
real      = (quantized - zero_point) * scale
```

Calibration chooses scales and zero points from representative data. It does
not train the tracker and does not use object class labels. INT8 can reduce
model memory and NPU bandwidth, but clipping and rounding can change feature
values, scores, and localization. A small score change can select a different
15 × 15 response cell and cause drift over later frames.

The ordinary `_int8` files are the standard INT8 candidates. Validate them
against the same video and initial ROI used for the FP16 baseline. The template
backbone is run once and cached, so its quantization error affects the rest of a
sequence; the search backbone and head affect every following frame.

### MMSE-labelled INT8

MMSE means minimum mean-square-error-based selection of quantization parameters.
The calibration process searches for scales/clipping choices that reduce the
average squared difference between floating-point and quantized values for the
calibration tensors. It is a calibration strategy, not a fourth runtime tensor
type and not a model-training method.

The `_int8_mmse` suffix identifies artifacts prepared with that calibration
variant. The current C++ program treats them exactly like ordinary INT8 files;
`--precision int8-mmse` only selects their filenames. It does not run MMSE,
load a calibration dataset, or inspect the model to prove how it was built.
The copied artifacts have distinct SHA-256 values; verify their conversion
logs/toolkit settings if reproducibility matters.

MMSE can preserve accuracy better for distributions with outliers, but it is
not automatically better for every graph or dataset. Measure template
initialization, per-frame latency, overlap/drift, and failures on representative
sequences before choosing it.

### Mixed precision

`mixed` is an application-level selection, not a single quantization format.
It combines the unsuffixed floating-point template/search backbones with the
ordinary INT8 head. This isolates INT8 effects to the graph that compares
features and produces scores/box distances. It can be a useful compromise when
full INT8 backbones lose tracking stability, while still reducing the head's
cost.

The C++ model wrapper requests FLOAT32 feature inputs and outputs at graph
boundaries for all modes. Do not assume that two tensors with the same element
count have the same internal quantization scale or layout. The RKNN compiler and
runtime handle each selected graph's internal representation; graph contracts
must still match the fixed NanoTrack shapes documented in the parent README.

## Choosing a set

1. Start with `fp16` and confirm the initial ROI, BGR preprocessing, and model
   outputs against the ONNX tracker.
2. Try `mixed` to measure the effect of quantizing only the head.
3. Try `int8` and `int8-mmse` separately with identical videos and ROIs.
4. Keep the fastest set whose box overlap, drift, and recovery behavior remain
   acceptable. There is no confidence threshold or automatic lost-target
   recovery in this tracker, so quantization errors can persist after a bad
   update.

The files were copied from the board's NanoTrack model directory. Their names
select runtime artifacts only; they do not document the full RKNN Toolkit2
command, target SDK, calibration dataset, or driver version. See the
[conversion guide](../../../docs/usage/nanotrack-onnx-to-rknn.md) for the
conversion workflow and calibration-list requirements.
