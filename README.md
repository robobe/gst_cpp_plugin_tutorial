# Minimal GStreamer C++ Plugin

Tiny `GstBaseTransform` plugin that accepts raw video buffers, prints each
buffer PTS, and forwards the buffer unchanged.

## Build

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

## Run

```sh
GST_PLUGIN_PATH="$PWD/build" gst-inspect-1.0 minimalfilter
```

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 videotestsrc num-buffers=5 ! video/x-raw ! minimalfilter ! fakesink
```

Expected output includes lines like:

```text
buffer pts: 0:00:00.000000000
```

## Minimal Source Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 minimalsrc num-buffers=5 ! fakesink
```

## Property Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 videotestsrc num-buffers=3 ! video/x-raw ! propertyfilter print-pts=false ! fakesink
```

## Metadata Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 videotestsrc num-buffers=3 ! video/x-raw ! metaattach ! metaprint ! fakesink
```

## Video Source Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 simplevideosrc num-buffers=5 ! fakesink
```

## Live Source Example

Live and clock-paced at 30 fps:

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 -v is_livesrc num-buffers=60 is-live=true ! identity silent=false ! fakesink sync=false
```

Non-live comparison, which finishes as fast as possible:

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 -v is_livesrc num-buffers=60 is-live=false ! identity silent=false ! fakesink sync=false
```

## Capstone Color Box Source Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 colorboxsrc ! videoconvert ! autovideosink
```

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 colorboxsrc box-red=0 box-green=255 box-blue=0 ! videoconvert ! autovideosink
```

## Capstone Red Detection Filter Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-inspect-1.0 reddetect
```

```sh
GST_DEBUG=reddetect:6 GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 colorboxsrc num-buffers=5 ! reddetect low-h=0 high-h=10 ! fakesink
```

## Capstone Metadata Verification Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 colorboxsrc num-buffers=5 ! reddetect ! metaprint ! fakesink
```

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 colorboxsrc num-buffers=3 ! reddetect low-h=60 high-h=70 ! metaprint ! fakesink
```

## Capstone Split Pipeline Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 colorboxsrc ! reddetect ! tee name=t t. ! queue ! videoconvert ! autovideosink t. ! queue ! fakesink
```

## Capstone Python App

```sh
python3 apps/run_capstone.py
```

Headless test:

```sh
python3 apps/run_capstone.py --no-display --num-buffers 5
```

## Bridge Controlled Detector Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-inspect-1.0 controlledreddetect
```

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 colorboxsrc num-buffers=3 ! controlledreddetect detection-enabled=true ! metaprint ! fakesink
```

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 colorboxsrc num-buffers=3 ! controlledreddetect detection-enabled=false ! metaprint ! fakesink
```

## Bridge App

Python dependencies:

```sh
python3 -m pip install -r apps/bridge_project/requirements.txt
```

Headless bridge test:

```sh
python3 apps/run_bridge.py --no-display --num-buffers 5
```

Bridge GUI:

```sh
python3 apps/run_bridge_gui.py
```

## OpenCV Gray Filter Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 simplevideosrc ! grayfilter ! videoconvert ! autovideosink
```

## Read Next

See [docs/minimal-source-plugin.md](docs/minimal-source-plugin.md) for a small
video source that generates timestamped black RGB frames.

See [docs/plugin-building-blocks.md](docs/plugin-building-blocks.md) for a
developer-focused explanation of the GStreamer concepts used by this plugin.

See [docs/property-mechanism.md](docs/property-mechanism.md) for the property
example.

See [docs/metadata-mechanism.md](docs/metadata-mechanism.md) for the metadata
attach/read example.

See [docs/video-source-plugin.md](docs/video-source-plugin.md) for the generated
video source example.

See [docs/live-source-plugin.md](docs/live-source-plugin.md) for the live source,
`NO_PREROLL`, and clock-pacing example.

See [docs/opencv-gray-filter.md](docs/opencv-gray-filter.md) for the OpenCV
buffer mapping example.

# TO READ
- [pytracking](https://github.com/visionml/pytracking)