#!/usr/bin/env python3
"""Run the complete YOLOv8 ONNX inference pipeline on ``assets/bus.jpg``.

The model cannot consume an image file directly. Each function below owns one
stage of the pipeline so the data transformation is visible from top to bottom:

    paths -> ONNX session -> image -> letterbox -> FP32 RGB NCHW tensor
          -> raw YOLO output -> confidence filtering -> source coordinates
          -> class-aware NMS -> printed bounding boxes

The bundled YOLOv8n model has input ``[1, 3, 640, 640]`` and raw output
``[1, 84, 8400]``. The 84 output channels contain four box values followed by
80 COCO class scores. The model was exported with embedded NMS disabled, so
this script performs filtering and NMS after ONNX Runtime returns.

OpenCV is used only for image loading, resizing, and BGR-to-RGB conversion.
ONNX Runtime performs inference; OpenCV DNN is not used.
"""

import argparse
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def parse_arguments() -> argparse.Namespace:
    """Read optional model and image paths from the command line."""
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "model",
        nargs="?",
        type=Path,
        default=PROJECT_ROOT / "models/yolov8n.onnx",
    )
    parser.add_argument(
        "image",
        nargs="?",
        type=Path,
        default=PROJECT_ROOT / "assets/bus.jpg",
    )
    parser.add_argument("--confidence", type=float, default=0.25)
    parser.add_argument("--iou", type=float, default=0.45)
    return parser.parse_args()


def create_session(model_path: Path) -> ort.InferenceSession:
    """Load the ONNX graph and select ONNX Runtime's CPU provider."""
    return ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])


def inspect_model(session: ort.InferenceSession) -> tuple[str, str, int, int]:
    """Fetch tensor names and validate the fixed YOLO image interface."""
    if len(session.get_inputs()) != 1 or len(session.get_outputs()) != 1:
        raise ValueError("expected exactly one model input and one output")

    model_input = session.get_inputs()[0]
    model_output = session.get_outputs()[0]
    if (
        len(model_input.shape) != 4
        or model_input.shape[0] != 1
        or model_input.shape[1] != 3
        or not isinstance(model_input.shape[2], int)
        or not isinstance(model_input.shape[3], int)
    ):
        raise ValueError(f"expected fixed NCHW image input, got {model_input.shape}")

    return (
        model_input.name,
        model_output.name,
        model_input.shape[2],
        model_input.shape[3],
    )


def load_image(image_path: Path) -> np.ndarray:
    """Load the source image as interleaved uint8 BGR pixels."""
    image = cv2.imread(str(image_path))
    if image is None:
        raise FileNotFoundError(f"cannot read image: {image_path}")
    return image


def letterbox(
    image: np.ndarray, input_height: int, input_width: int
) -> tuple[np.ndarray, float, int, int]:
    """Resize without distortion and center the result on a gray canvas."""
    scale = min(input_width / image.shape[1], input_height / image.shape[0])
    resized_width = round(image.shape[1] * scale)
    resized_height = round(image.shape[0] * scale)
    resized = cv2.resize(image, (resized_width, resized_height))

    pad_x = (input_width - resized_width) // 2
    pad_y = (input_height - resized_height) // 2
    canvas = np.full((input_height, input_width, 3), 114, dtype=np.uint8)
    canvas[pad_y : pad_y + resized_height, pad_x : pad_x + resized_width] = resized
    return canvas, scale, pad_x, pad_y


def create_input_tensor(letterboxed_image: np.ndarray) -> np.ndarray:
    """Convert BGR HWC bytes to normalized RGB NCHW floats with batch size 1."""
    rgb = cv2.cvtColor(letterboxed_image, cv2.COLOR_BGR2RGB)
    return np.transpose(rgb, (2, 0, 1))[None].astype(np.float32) / 255.0


def run_inference(
    session: ort.InferenceSession,
    input_name: str,
    output_name: str,
    input_tensor: np.ndarray,
) -> np.ndarray:
    """Run the graph synchronously and return its first output tensor."""
    return session.run([output_name], {input_name: input_tensor})[0]


def calculate_iou(left: np.ndarray, right: np.ndarray) -> float:
    """Calculate intersection-over-union for two ``[x1, y1, x2, y2]`` boxes."""
    intersection_width = max(0.0, min(left[2], right[2]) - max(left[0], right[0]))
    intersection_height = max(0.0, min(left[3], right[3]) - max(left[1], right[1]))
    intersection = intersection_width * intersection_height
    left_area = (left[2] - left[0]) * (left[3] - left[1])
    right_area = (right[2] - right[0]) * (right[3] - right[1])
    union = left_area + right_area - intersection
    return intersection / union if union > 0 else 0.0


def apply_nms(
    detections: list[tuple[int, float, np.ndarray]], iou_threshold: float
) -> list[tuple[int, float, np.ndarray]]:
    """Keep strong boxes and suppress overlapping boxes of the same class."""
    kept: list[tuple[int, float, np.ndarray]] = []
    for candidate in sorted(detections, key=lambda item: item[1], reverse=True):
        overlaps = any(
            candidate[0] == existing[0]
            and calculate_iou(candidate[2], existing[2]) > iou_threshold
            for existing in kept
        )
        if not overlaps:
            kept.append(candidate)
    return kept


def postprocess(
    output: np.ndarray,
    source_shape: tuple[int, ...],
    scale: float,
    pad_x: int,
    pad_y: int,
    confidence_threshold: float,
    iou_threshold: float,
) -> list[tuple[int, float, np.ndarray]]:
    """Decode raw YOLO candidates into filtered source-image bounding boxes."""
    if output.ndim != 3 or output.shape[0] != 1 or output.shape[1] < 5:
        raise ValueError(f"expected raw YOLO output [1, 4 + classes, N], got {output.shape}")

    predictions = output[0]
    class_ids = np.argmax(predictions[4:], axis=0)
    confidences = np.max(predictions[4:], axis=0)
    detections: list[tuple[int, float, np.ndarray]] = []

    for index in np.flatnonzero(confidences >= confidence_threshold):
        center_x, center_y, width, height = predictions[:4, index]
        box = np.array(
            [
                (center_x - width / 2 - pad_x) / scale,
                (center_y - height / 2 - pad_y) / scale,
                (center_x + width / 2 - pad_x) / scale,
                (center_y + height / 2 - pad_y) / scale,
            ],
            dtype=np.float32,
        )
        box[[0, 2]] = np.clip(box[[0, 2]], 0, source_shape[1])
        box[[1, 3]] = np.clip(box[[1, 3]], 0, source_shape[0])
        if box[2] > box[0] and box[3] > box[1]:
            detections.append((int(class_ids[index]), float(confidences[index]), box))

    return apply_nms(detections, iou_threshold)


def print_detections(detections: list[tuple[int, float, np.ndarray]]) -> None:
    """Print final boxes using source-image pixel coordinates."""
    print(f"detections: {len(detections)}")
    for class_id, confidence, box in detections:
        left, top, right, bottom = box
        print(
            f"class={class_id} confidence={confidence:.6f} "
            f"x={left:.0f} y={top:.0f} "
            f"width={right - left:.0f} height={bottom - top:.0f}"
        )


def run_pipeline(args: argparse.Namespace) -> None:
    """Connect the individual pipeline stages for one image."""
    session = create_session(args.model)
    input_name, output_name, input_height, input_width = inspect_model(session)
    image = load_image(args.image)
    canvas, scale, pad_x, pad_y = letterbox(image, input_height, input_width)
    input_tensor = create_input_tensor(canvas)
    output = run_inference(session, input_name, output_name, input_tensor)
    detections = postprocess(
        output,
        image.shape,
        scale,
        pad_x,
        pad_y,
        args.confidence,
        args.iou,
    )

    print(f"input:  {input_name} {input_tensor.shape} {input_tensor.dtype}")
    print(f"output: {output_name} {output.shape} {output.dtype}")
    print_detections(detections)


if __name__ == "__main__":
    run_pipeline(parse_arguments())
