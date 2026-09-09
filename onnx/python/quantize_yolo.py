#!/usr/bin/env python3
"""Create and benchmark a static INT8 YOLOv8 ONNX model for CPU inference."""

import argparse
from pathlib import Path
import statistics
import time

import cv2
import numpy as np
import onnx
import onnxruntime as ort
from onnxruntime.quantization import (
    CalibrationDataReader,
    QuantFormat,
    QuantType,
    quantize_static,
)


PROJECT_ROOT = Path(__file__).resolve().parents[2]


def preprocess(image: np.ndarray, height: int, width: int) -> np.ndarray:
    scale = min(width / image.shape[1], height / image.shape[0])
    resized = cv2.resize(image, (round(image.shape[1] * scale), round(image.shape[0] * scale)))
    canvas = np.full((height, width, 3), 114, dtype=np.uint8)
    top = (height - resized.shape[0]) // 2
    left = (width - resized.shape[1]) // 2
    canvas[top : top + resized.shape[0], left : left + resized.shape[1]] = resized
    rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
    return np.transpose(rgb, (2, 0, 1))[None].astype(np.float32) / 255.0


class VideoCalibrationReader(CalibrationDataReader):
    def __init__(self, video: Path, input_name: str, height: int, width: int, samples: int):
        self.video = video
        self.input_name = input_name
        self.height = height
        self.width = width
        self.samples = samples
        self.rewind()

    def rewind(self) -> None:
        self.capture = cv2.VideoCapture(str(self.video))
        frame_count = int(self.capture.get(cv2.CAP_PROP_FRAME_COUNT))
        if not self.capture.isOpened() or frame_count < 1:
            raise ValueError(f"cannot read calibration video: {self.video}")
        # ponytail: this demo clip is narrow calibration data; use deployment clips before an accuracy-sensitive release.
        self.positions = iter(np.linspace(0, frame_count - 1, self.samples, dtype=int))

    def get_next(self):
        try:
            position = next(self.positions)
        except StopIteration:
            self.capture.release()
            return None
        self.capture.set(cv2.CAP_PROP_POS_FRAMES, int(position))
        ok, frame = self.capture.read()
        if not ok:
            raise ValueError(f"cannot read calibration frame {position} from {self.video}")
        return {self.input_name: preprocess(frame, self.height, self.width)}


def create_session(model: Path, threads: int) -> ort.InferenceSession:
    options = ort.SessionOptions()
    options.intra_op_num_threads = threads
    return ort.InferenceSession(model, options, providers=["CPUExecutionProvider"])


def median_milliseconds(session: ort.InferenceSession, input_name: str, tensor: np.ndarray) -> float:
    for _ in range(5):
        session.run(None, {input_name: tensor})
    timings = []
    for _ in range(20):
        start = time.perf_counter()
        session.run(None, {input_name: tensor})
        timings.append((time.perf_counter() - start) * 1000)
    return statistics.median(timings)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, default=PROJECT_ROOT / "models/yolov8n.onnx")
    parser.add_argument("--output", type=Path, default=PROJECT_ROOT / "models/yolov8n.int8.onnx")
    parser.add_argument(
        "--calibration-video",
        type=Path,
        default=PROJECT_ROOT / "assets/detection-demo.mp4",
    )
    parser.add_argument("--samples", type=int, default=64)
    parser.add_argument("--threads", type=int, default=1)
    args = parser.parse_args()

    if args.output == args.model:
        raise ValueError("output must differ from the FP32 model")
    if args.samples < 1 or args.threads < 1:
        raise ValueError("samples and threads must be positive")

    model = onnx.load(args.model)
    input_tensor = model.graph.input[0]
    dims = [dimension.dim_value for dimension in input_tensor.type.tensor_type.shape.dim]
    if len(dims) != 4 or dims[:2] != [1, 3] or not all(dims[2:]):
        raise ValueError(f"expected static FP32 [1, 3, height, width] input, got {dims}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    reader = VideoCalibrationReader(args.calibration_video, input_tensor.name, dims[2], dims[3], args.samples)
    quantize_static(
        args.model,
        args.output,
        reader,
        quant_format=QuantFormat.QDQ,
        activation_type=QuantType.QInt8,
        weight_type=QuantType.QInt8,
        op_types_to_quantize=["Conv"],
        per_channel=True,
    )
    onnx.checker.check_model(args.output)

    image = cv2.imread(str(PROJECT_ROOT / "assets/bus.jpg"))
    if image is None:
        raise ValueError("cannot read assets/bus.jpg for validation")
    tensor = preprocess(image, dims[2], dims[3])
    fp32 = create_session(args.model, args.threads)
    int8 = create_session(args.output, args.threads)
    fp32_output = fp32.run(None, {input_tensor.name: tensor})[0]
    int8_output = int8.run(None, {input_tensor.name: tensor})[0]
    if fp32_output.shape != int8_output.shape or not np.isfinite(int8_output).all():
        raise ValueError("INT8 model failed output validation")

    fp32_ms = median_milliseconds(fp32, input_tensor.name, tensor)
    int8_ms = median_milliseconds(int8, input_tensor.name, tensor)
    print(f"wrote: {args.output}")
    print(f"output mean absolute error: {np.mean(np.abs(fp32_output - int8_output)):.6f}")
    print(f"FP32 median: {fp32_ms:.2f} ms; INT8 median: {int8_ms:.2f} ms; speedup: {fp32_ms / int8_ms:.2f}x")


if __name__ == "__main__":
    main()
