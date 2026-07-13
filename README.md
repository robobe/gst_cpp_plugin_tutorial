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

## Property Example

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 videotestsrc num-buffers=3 ! video/x-raw ! propertyfilter print-pts=false ! fakesink
```

## Read Next

See [docs/plugin-building-blocks.md](docs/plugin-building-blocks.md) for a
developer-focused explanation of the GStreamer concepts used by this plugin.

See [docs/property-mechanism.md](docs/property-mechanism.md) for the property
example.
