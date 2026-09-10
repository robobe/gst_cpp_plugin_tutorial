# CPU LightTrack-Mobile

`cpulighttrack` tracks one BGR ROI with the fixed-shape LightTrack-Mobile ONNX models in `demos/lighttrack/onnx`.

```bash
cmake -S src/cpulighttrack -B build-cpulighttrack -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpulighttrack
GST_PLUGIN_PATH="$PWD/build-cpulighttrack" gst-launch-1.0 -v \
  videotestsrc ! videoconvert ! video/x-raw,format=BGR,interlace-mode=progressive ! \
  cpulighttrack enabled=true roi="100,80,80,80" models-dir="$PWD/demos/lighttrack/onnx" ! fakesink
```

Properties intentionally match `cpunanotrack`: `enabled`, `roi`, and `models-dir`. It writes `lighttrack` ROI metadata with `initialized` and, after initialization, `confidence`.

To re-export the committed models, clone [LightTrack](https://github.com/researchmm/LightTrack) at `39c426f48ee674795cdf0e00301a0b4ad0785d2a`, then run:

```bash
.venv/bin/python demos/lighttrack/export_lighttrack.py --lighttrack-root /path/to/LightTrack
```
