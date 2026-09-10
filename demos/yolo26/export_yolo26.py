#!/usr/bin/env python3
"""Download YOLO26n and export the raw ONNX layout consumed by yolodetect."""
import os
import sys
from pathlib import Path

import onnx
from ultralytics import YOLO


def main():
    output_dir = Path(__file__).parent
    output_dir.mkdir(parents=True, exist_ok=True)
    original_dir = Path.cwd()
    try:
        os.chdir(output_dir)
        model = YOLO("yolo26n.pt")  # Ultralytics downloads the official checkpoint when absent.
        exported = Path(model.export(format="onnx", imgsz=640, batch=1, dynamic=False, nms=None, simplify=True)).resolve()
    finally:
        os.chdir(original_dir)
    model = onnx.load(exported)
    if len(model.graph.input) != 1 or len(model.graph.output) != 1:
        raise SystemExit("Expected one input and one raw YOLO output")
    shape = [dimension.dim_value for dimension in model.graph.output[0].type.tensor_type.shape.dim]
    if len(shape) != 3 or shape[0] != 1 or shape[1] < 5 or shape[2] <= shape[1]:
        raise SystemExit(f"Expected raw [1, 4 + classes, candidates] output, got {shape}")
    print(f"Exported {exported} with output {shape}")


if __name__ == "__main__":
    main()
