# YOLO26 ONNX

Create the raw YOLO26n model used by `yolodetect` and the benchmark selector:

```bash
.venv/bin/pip install --index-url https://download.pytorch.org/whl/cpu \
  'torch==2.5.1+cpu' 'torchvision==0.20.1+cpu'
.venv/bin/pip install --no-deps 'ultralytics==8.4.146' onnxslim
.venv/bin/python demos/yolo26/export_yolo26.py
```

The exporter downloads the official `yolo26n.pt` checkpoint when needed and
creates `yolo26n.onnx`. It deliberately exports `nms=None`: `yolodetect`
expects raw `[1, 4 + classes, candidates]` predictions and performs NMS itself.
