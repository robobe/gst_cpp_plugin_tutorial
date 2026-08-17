# Minimal Video Source Plugin

`minimalsrc` is a small `GstPushSrc` element. It generates black RGB frames at
320×240 and gives them timestamps for a 30 FPS stream.

## Build and run

```sh
cmake -S . -B build
cmake --build build --target gstminimalsrc
GST_PLUGIN_PATH="$PWD/build" gst-inspect-1.0 minimalsrc
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 minimalsrc num-buffers=5 ! fakesink
```

`num-buffers` comes from `GstBaseSrc`. Without it, the source keeps producing
frames until the pipeline is stopped.

## How the code works

The element derives from `GstPushSrc`, which asks its `create` callback for the
next buffer. Each callback invocation does four things:

1. Allocates enough memory for one 320×240 RGB frame.
2. Fills that memory with zeroes, producing black pixels.
3. Sets the presentation timestamp, duration, and frame offset.
4. Returns the completed buffer to GStreamer.

The source-pad template fixes the output caps to
`video/x-raw,format=RGB,width=320,height=240,framerate=30/1`. Fixed caps avoid
format negotiation code and keep this example focused on the source lifecycle.

The source is non-live, so it can generate buffers as quickly as downstream can
consume them. Its timestamps still describe 30 FPS. A live source additionally
needs clock pacing; see [live-source-plugin.md](live-source-plugin.md) for that
next step.
