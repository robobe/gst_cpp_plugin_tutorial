# CPU Lucas–Kanade tracker

`cpulktracker` follows one selected BGR ROI with OpenCV Shi–Tomasi corners and pyramidal Lucas–Kanade optical flow. It has no model files and does not re-identify an object after loss.

```bash
cmake -S src/cpulktracker -B build-cpulktracker -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpulktracker
GST_PLUGIN_PATH="$PWD/build-cpulktracker" gst-launch-1.0 -v \
  filesrc location=assets/camera_run_2s_10s.mp4 ! decodebin ! videoconvert ! \
  video/x-raw,format=BGR ! cpulktracker enabled=true roi="100,80,60,90" ! fakesink
```

It writes one `GstVideoRegionOfInterestMeta` of type `flowtrack`. Its `flowtrack` parameter structure contains `initialized`, then `confidence` (surviving features divided by the original corner count), `feature-count`, and `lost` when fewer than eight features remain. Assigning `roi` resets tracking.
