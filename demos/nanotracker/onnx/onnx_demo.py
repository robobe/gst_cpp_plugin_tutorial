"""Standalone NanoTrackV3 tracking with ONNX Runtime and no PyTorch.

How it works
------------
NanoTrack is a Siamese single-object tracker. The user identifies the object once
by drawing a bounding box on the first frame. Tracking then has two stages:

1. Initialization (runs once)
   A square 127x127 template crop is taken around the selected object. The
   template backbone converts it into a (1, 96, 8, 8) feature tensor, which is
   saved for the lifetime of the tracker.

2. Tracking (runs for every following frame)
   A larger 255x255 search crop is taken around the last known object position.
   The search backbone converts it into a (1, 96, 16, 16) feature tensor. The
   NanoTrack head compares the saved template features with the new search
   features and produces:

       classification: (1, 2, 15, 15)  object/background scores
       localization:   (1, 4, 15, 15)  left/top/right/bottom distances

   The output grid represents 225 possible object locations. The decoder applies
   scale and aspect-ratio penalties, plus a Hann window that discourages sudden
   large motion. The best candidate updates the object center and size. That
   updated box determines the search region in the next frame.

Why there is a separate template model
--------------------------------------
The template model is not a second independently trained neural network. It is
the same NanoTrackV3 backbone, with the same layers and learned weights, but its
ONNX input and output dimensions are declared for the smaller template image.

The original repository provides one backbone ONNX file whose dimensions are
fixed as follows:

    input:  (1, 3, 255, 255)
    output: (1, 96, 16, 16)

That shape is correct for a search image, but NanoTrack also needs to pass a
127x127 template image through the backbone. ONNX Runtime rejects that smaller
image when the graph declares a fixed 255x255 input. The locally generated
``nanotrack_backbone_template.onnx`` solves this by declaring:

    input:  (1, 3, 127, 127)
    output: (1, 96, 8, 8)

No weights were retrained or changed. Only the spatial dimensions declared by
the backbone graph differ. The convolutional backbone naturally produces an 8x8
feature map when its input is 127x127.

Template processing, step by step
---------------------------------
1. The program reads the first video frame.
2. The user draws an ROI around the object to track. This is the only manual
   object selection.
3. The ROI is converted from ``(x, y, width, height)`` to an object center and
   size.
4. Some surrounding image context is added to the ROI. Context helps the model
   distinguish the target from similar-looking objects.
5. That square region is cropped and resized to 127x127 pixels. If it crosses an
   image boundary, missing pixels are filled with the frame's average color.
6. OpenCV's ``height x width x BGR`` image is converted into the ONNX tensor
   layout ``(1, 3, 127, 127)`` using float32 values.
7. ``nanotrack_backbone_template.onnx`` converts the pixels into a compact
   ``(1, 96, 8, 8)`` feature tensor. These values describe the selected object's
   appearance.
8. The program stores that tensor in ``self.template_features``. The template
   backbone is normally run only once and the original pixels are no longer
   needed by the model.
9. For every later frame, the search backbone produces a ``(1, 96, 16, 16)``
   feature tensor from a larger region around the previous object position.
10. The head receives both tensors—saved template features and current search
    features—and measures where the template appears inside the search region.

In short:

    selected ROI -> 127x127 template -> template backbone -> saved 8x8 features

    next frame -> 255x255 search -> search backbone -> 16x16 features
                                                   +
                                     saved template features
                                                   |
                                                   v
                                            tracking head -> new box

Configuration values and tuning
-------------------------------
The constants at the top of ``NanoTrackONNX`` come from the TRACK and POINT
sections of the original ``models/config/configv3.yaml``. They fall into two
groups: model-shape values that must match the exported ONNX graphs, and runtime
tracking values that may be tuned for a particular dataset or application.

Model-shape values -- do not tune with the current ONNX files:

``exemplar_size = 127``
    Pixel width and height of the template sent to the template backbone. This
    must remain 127 because ``nanotrack_backbone_template.onnx`` declares a
    ``(1, 3, 127, 127)`` input. Changing it requires a compatible ONNX export and
    may require retraining or validating the model.

``instance_size = 255``
    Pixel width and height of the per-frame search image. It must remain 255
    because the upstream search backbone declares ``(1, 3, 255, 255)``. A larger
    search input could cover more motion but costs more computation and is not
    accepted by the current fixed-shape model.

``output_size = 15``
    Width and height of the tracking head's prediction grid. A 15x15 grid gives
    225 candidate locations. It must match the ONNX head output and the Hann
    window/point-grid construction, so it should remain 15.

``stride = 16``
    Distance, in search-image pixels, between neighboring prediction points.
    This comes from the backbone's total downsampling. It is part of the model's
    geometry, not an arbitrary movement step. An incorrect value decodes boxes
    at the wrong positions, so it should remain 16.

Runtime tracking values -- tunable without changing the ONNX files:

``context_amount = 0.5``
    Controls how much background is included around the object when making the
    template and search crops. Increasing it gives the model more context and a
    wider effective search area, which can help with fast motion, but makes the
    object smaller in the resized crop. Decreasing it gives the object more
    pixels but makes it easier to leave the search area. Start with 0.5.

``window_influence = 0.455``
    Blends model confidence with a Hann window centered on the previous object
    position. Increasing it makes tracking smoother and less likely to jump to a
    distractor, but can lose a fast-moving target. Decreasing it follows model
    confidence more aggressively, allowing faster motion but more jumps. This is
    usually one of the most useful values to tune.

``penalty_k = 0.138``
    Strength of the penalty applied to sudden changes in target scale and aspect
    ratio. Increasing it keeps width and height stable but reacts slowly when the
    object turns or moves toward the camera. Decreasing it allows faster shape
    changes but may make the box unstable. Tune it when scale changes are either
    too sluggish or too noisy.

``update_lr = 0.348``
    Smoothing rate for the predicted width and height. It does not update neural
    network weights or the saved template. Increasing it makes the box size
    follow each new prediction faster; decreasing it produces a steadier box.
    Tune it when box size visibly lags or oscillates.

Do these values need tuning?
----------------------------
For a first application, use the supplied V3 values unchanged. They were chosen
for general tracking benchmarks and are coupled: changing one can alter the best
setting for the others. Tune only the three main runtime values
``window_influence``, ``penalty_k``, and ``update_lr`` when you have a collection
of representative, annotated videos and an objective metric such as IoU or
success rate. Avoid selecting values based on a single good-looking video.

A practical manual tuning order is:

1. Tune ``window_influence`` for the motion-versus-stability tradeoff.
2. Tune ``penalty_k`` for scale/aspect-ratio stability.
3. Tune ``update_lr`` for box-size responsiveness.
4. Consider ``context_amount`` only if targets frequently leave the search area
   or appear too small in the crop.

Change one value at a time, evaluate the same videos, and keep a baseline. Do not
change ``exemplar_size``, ``instance_size``, ``output_size``, or ``stride`` unless
you are also changing/exporting the model architecture and validating its tensor
shapes.

Inputs are OpenCV BGR images converted from HWC uint8 to NCHW float32. The
original NanoTrack code does not normalize or reorder their pixel values, so this
example intentionally preserves that behavior.

Run this file from the project root:

    python onnx/onnx_demo.py --video upstream/NanoTrack/bin/girl_dance.mp4

Draw a box, then press Enter or Space. Press Q or Escape to stop tracking.
Pass --video "" to use webcam 0, or --save output.mp4 to record the result.
"""

import argparse
import time

import cv2
import numpy as np
import onnxruntime as ort


class NanoTrackONNX:
    """CPU-only NanoTrackV3 inference and tracking state."""

    # These values come from models/config/configv3.yaml in the original repo.
    exemplar_size = 127
    instance_size = 255
    output_size = 15
    stride = 16
    context_amount = 0.5
    window_influence = 0.455
    penalty_k = 0.138
    update_lr = 0.348

    def __init__(self, template_backbone_path, search_backbone_path, head_path):
        # The demo deliberately requests only the CPU execution provider.
        providers = ["CPUExecutionProvider"]
        self.template_backbone = ort.InferenceSession(
            template_backbone_path, providers=providers
        )
        self.search_backbone = ort.InferenceSession(
            search_backbone_path, providers=providers
        )
        self.head = ort.InferenceSession(head_path, providers=providers)
        self.template_input = self.template_backbone.get_inputs()[0].name
        self.search_input = self.search_backbone.get_inputs()[0].name
        self.head_inputs = [item.name for item in self.head.get_inputs()]

        # Map every cell in the 15x15 head output to an image-space point.
        axis = np.arange(self.output_size, dtype=np.float32)
        axis = (axis - self.output_size // 2) * self.stride
        xx, yy = np.meshgrid(axis, axis)
        self.points = np.stack((xx.ravel(), yy.ravel()), axis=1)
        # The Hann window favors motion near the previous object position.
        hann = np.hanning(self.output_size)
        self.window = np.outer(hann, hann).ravel()

    def init(self, image, box):
        """Extract and remember the target template from the first frame."""
        x, y, width, height = box
        self.center = np.array(
            [x + (width - 1) / 2, y + (height - 1) / 2], dtype=np.float32
        )
        self.size = np.array([width, height], dtype=np.float32)
        self.average = image.mean(axis=(0, 1))
        # Add surrounding context so the template contains more than the object.
        wz = width + self.context_amount * (width + height)
        hz = height + self.context_amount * (width + height)
        crop = crop_for_model(
            image, self.center, self.exemplar_size,
            round(np.sqrt(wz * hz)), self.average,
        )
        self.template_features = self.template_backbone.run(
            None, {self.template_input: crop}
        )[0]

    def track(self, image):
        """Locate the initialized target in one new frame."""
        # Build a search region centered at the previous predicted position.
        wz = self.size[0] + self.context_amount * self.size.sum()
        hz = self.size[1] + self.context_amount * self.size.sum()
        template_scale = np.sqrt(wz * hz)
        scale = self.exemplar_size / template_scale
        search_size = template_scale * self.instance_size / self.exemplar_size
        search = crop_for_model(
            image, self.center, self.instance_size, round(search_size), self.average
        )
        search_features = self.search_backbone.run(
            None, {self.search_input: search}
        )[0]
        # Compare template and search features using the Siamese tracking head.
        cls, loc = self.head.run(
            None,
            {
                self.head_inputs[0]: self.template_features,
                self.head_inputs[1]: search_features,
            },
        )

        # Convert two-class logits into an object probability for each grid cell.
        score = softmax(cls[0].reshape(2, -1).T)[:, 1]

        # Decode distances from each grid point into center-x/y, width, height.
        boxes = loc[0].reshape(4, -1).copy()
        x1 = self.points[:, 0] - boxes[0]
        y1 = self.points[:, 1] - boxes[1]
        x2 = self.points[:, 0] + boxes[2]
        y2 = self.points[:, 1] + boxes[3]
        boxes = np.vstack(((x1 + x2) / 2, (y1 + y2) / 2, x2 - x1, y2 - y1))

        # Penalize candidates whose size or aspect ratio changes abruptly.
        size_ratio = size_with_padding(boxes[2], boxes[3]) / size_with_padding(
            self.size[0] * scale, self.size[1] * scale
        )
        size_penalty = np.maximum(size_ratio, 1.0 / size_ratio)
        ratio = (self.size[0] / self.size[1]) / (boxes[2] / boxes[3])
        ratio_penalty = np.maximum(ratio, 1.0 / ratio)
        penalty = np.exp(-(ratio_penalty * size_penalty - 1) * self.penalty_k)
        penalized = penalty * score
        # Blend confidence with the motion window and select the best candidate.
        penalized = penalized * (1 - self.window_influence) + self.window * self.window_influence
        best = int(np.argmax(penalized))

        # Convert from search-crop coordinates back to full-frame coordinates.
        box = boxes[:, best] / scale
        interpolation = penalty[best] * score[best] * self.update_lr
        center_x, center_y = box[:2] + self.center
        # Smooth size changes rather than replacing the old size immediately.
        width = self.size[0] * (1 - interpolation) + box[2] * interpolation
        height = self.size[1] * (1 - interpolation) + box[3] * interpolation
        center_x = np.clip(center_x, 0, image.shape[1])
        center_y = np.clip(center_y, 0, image.shape[0])
        width = np.clip(width, 10, image.shape[1])
        height = np.clip(height, 10, image.shape[0])
        # Store state for the next frame.
        self.center = np.array([center_x, center_y])
        self.size = np.array([width, height])
        return [center_x - width / 2, center_y - height / 2, width, height], score[best]

def main():
    args = parse_args()
    tracker = NanoTrackONNX(
        args.template_backbone, args.search_backbone, args.head
    )
    source = 0 if args.video == "" else args.video
    capture = cv2.VideoCapture(source)
    if not capture.isOpened():
        raise RuntimeError("Could not open video source: {}".format(source))
    ok, frame = capture.read()
    if not ok:
        raise RuntimeError("Could not read the first frame")

    window = "NanoTrack ONNX"
    # The first-frame ROI is the only manual input the tracker needs.
    box = cv2.selectROI(window, frame, showCrosshair=True, fromCenter=False)
    if box[2] <= 0 or box[3] <= 0:
        capture.release()
        return
    tracker.init(frame, box)

    writer = None
    if args.save:
        fps = capture.get(cv2.CAP_PROP_FPS) or 30.0
        writer = cv2.VideoWriter(
            args.save, cv2.VideoWriter_fourcc(*"mp4v"), fps,
            (frame.shape[1], frame.shape[0]),
        )

    # Tracking FPS measures tracker.track() only. Wall FPS also includes video
    # decoding, drawing, display, keyboard handling, and optional video writing.
    processed_frames = 0
    tracking_seconds = 0.0
    wall_start = time.perf_counter()

    while True:
        ok, frame = capture.read()
        if not ok:
            break

        tracking_start = time.perf_counter()
        box, score = tracker.track(frame)
        frame_tracking_seconds = time.perf_counter() - tracking_start
        tracking_seconds += frame_tracking_seconds
        processed_frames += 1

        current_fps = 1.0 / frame_tracking_seconds if frame_tracking_seconds else 0.0
        average_fps = processed_frames / tracking_seconds if tracking_seconds else 0.0
        x, y, width, height = (int(value) for value in box)
        cv2.rectangle(frame, (x, y), (x + width, y + height), (0, 255, 0), 2)
        cv2.putText(frame, "{:.3f}".format(score), (x, max(20, y - 8)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
        cv2.putText(
            frame,
            "FPS: {:.1f}  Avg: {:.1f}".format(current_fps, average_fps),
            (15, 30),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.7,
            (0, 255, 255),
            2,
        )
        cv2.imshow(window, frame)
        if writer:
            writer.write(frame)
        if cv2.waitKey(1) & 0xFF in (27, ord("q")):
            break

    wall_seconds = time.perf_counter() - wall_start
    capture.release()
    if writer:
        writer.release()
    cv2.destroyAllWindows()

    average_tracking_fps = (
        processed_frames / tracking_seconds if tracking_seconds else 0.0
    )
    average_frame_ms = (
        tracking_seconds * 1000.0 / processed_frames if processed_frames else 0.0
    )
    wall_fps = processed_frames / wall_seconds if wall_seconds else 0.0
    print("\nNanoTrack run summary")
    print("---------------------")
    print("Processed frames:       {}".format(processed_frames))
    print("Tracking time:          {:.3f} s".format(tracking_seconds))
    print("Average tracking time:  {:.2f} ms/frame".format(average_frame_ms))
    print("Average tracking FPS:   {:.2f}".format(average_tracking_fps))
    print("Total wall time:        {:.3f} s".format(wall_seconds))
    print("Wall-clock throughput:  {:.2f} FPS".format(wall_fps))


# region Utility functions
# Keep this region collapsed when you want to focus only on the tracking pipeline.

def crop_for_model(image, center, model_size, original_size, average):
    """Crop around center, pad outside pixels, resize, and create NCHW input."""
    size = int(original_size)
    half = (size + 1) / 2
    xmin = int(np.floor(center[0] - half + 0.5))
    ymin = int(np.floor(center[1] - half + 0.5))
    xmax, ymax = xmin + size - 1, ymin + size - 1

    left, top = max(0, -xmin), max(0, -ymin)
    right = max(0, xmax - image.shape[1] + 1)
    bottom = max(0, ymax - image.shape[0] + 1)
    xmin, xmax = xmin + left, xmax + left
    ymin, ymax = ymin + top, ymax + top

    # Pad crops crossing an image boundary with the video's average color.
    if any((left, top, right, bottom)):
        image = cv2.copyMakeBorder(
            image,
            top,
            bottom,
            left,
            right,
            cv2.BORDER_CONSTANT,
            value=tuple(float(value) for value in average),
        )

    patch = image[ymin:ymax + 1, xmin:xmax + 1]
    if patch.shape[0] != model_size or patch.shape[1] != model_size:
        patch = cv2.resize(patch, (model_size, model_size))

    # OpenCV gives HWC uint8; ONNX expects NCHW float32 with a batch dimension.
    return np.ascontiguousarray(
        patch.transpose(2, 0, 1)[None].astype(np.float32)
    )


def softmax(values):
    """Convert two-class logits into probabilities in a stable way."""
    values = values - values.max(axis=1, keepdims=True)
    values = np.exp(values)
    return values / values.sum(axis=1, keepdims=True)


def size_with_padding(width, height):
    """Measure target size while accounting for surrounding context."""
    padding = (width + height) * 0.5
    return np.sqrt((width + padding) * (height + padding))


def parse_args():
    parser = argparse.ArgumentParser(description="Standalone NanoTrackV3 ONNX demo")
    parser.add_argument(
        "--template-backbone", default="demos/nanotracker/onnx/nanotrack_backbone_template.onnx"
    )
    parser.add_argument(
        "--search-backbone",
        default="demos/nanotracker/onnx/nanotrack_backbone.onnx",
    )
    parser.add_argument(
        "--head", default="demos/nanotracker/onnx/nanotrack_head.onnx"
    )
    parser.add_argument(
        "--video", default="assets/camera_run_2s_10s.mp4",
        help="Empty string uses webcam 0",
    )
    parser.add_argument("--save", default="", help="Optional output MP4 path")
    return parser.parse_args()


# endregion Utility functions


if __name__ == "__main__":
    main()
