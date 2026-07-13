# Plugin Building Blocks

This page explains the plugin for a developer who is new to GStreamer.
GStreamer is built around pipelines: data flows from one element to the next.
Each element has pads, negotiates the type of data it accepts, and receives
buffers during playback.

## Pipeline

A pipeline connects elements with `!`:

```sh
videotestsrc ! video/x-raw ! minimalfilter ! fakesink
```

In that example:

- `videotestsrc` creates test video frames.
- `video/x-raw` asks for uncompressed video.
- `minimalfilter` is this plugin's element.
- `fakesink` receives and discards the frames.

## Element

The plugin registers one element named `minimalfilter`.

```cpp
gst_element_register(plugin, "minimalfilter", GST_RANK_NONE, GST_TYPE_MINIMAL_FILTER);
```

An element is a reusable processing node. Users put the element name in a
pipeline, and GStreamer creates an object instance for it.

## GObject Type

GStreamer elements are GObject types, even when the file is C++.

```cpp
G_DEFINE_TYPE(GstMinimalFilter, gst_minimal_filter, GST_TYPE_BASE_TRANSFORM)
```

This creates the type glue for `GstMinimalFilter` and says it inherits from
`GstBaseTransform`.

## Base Transform

`GstBaseTransform` is the base class for one-input, one-output filters.
It is a good fit for simple filters like:

- inspect a buffer
- modify a buffer
- convert data
- pass data through unchanged

This plugin uses in-place processing:

```cpp
gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(self), FALSE);
```

In-place means the callback receives the input buffer and may edit that same
buffer instead of creating a new one.

## Pads

Pads are connection points on an element.

- A sink pad receives data.
- A source pad sends data.

This plugin has one of each:

```cpp
GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw"))
GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw"))
```

`GST_PAD_ALWAYS` means the pads always exist. `video/x-raw` means the element
accepts and outputs raw video.

## Caps

Caps describe media format. For example, raw video caps can include width,
height, pixel format, and framerate.

This minimal plugin only declares:

```text
video/x-raw
```

That keeps negotiation broad. A real plugin often narrows caps to formats it
can actually process.

## Buffers

A buffer is one chunk of media data. For video, that usually means one frame.

The callback receives each buffer:

```cpp
static GstFlowReturn gst_minimal_filter_transform_ip(
    GstBaseTransform* base,
    GstBuffer* buffer)
```

This plugin reads the buffer PTS:

```cpp
const GstClockTime pts = GST_BUFFER_PTS(buffer);
```

PTS means presentation timestamp: when this buffer should be shown downstream.

## Class Initialization

Class initialization runs once for the type. This is where the element declares
metadata, pads, and callbacks.

```cpp
gst_element_class_set_static_metadata(...);
gst_element_class_add_static_pad_template(...);
transform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_minimal_filter_transform_ip);
```

The important line is `transform_ip`: it tells `GstBaseTransform` which function
to call for each buffer.

## Instance Initialization

Instance initialization runs for every created element object.

```cpp
static void gst_minimal_filter_init(GstMinimalFilter* self)
```

Put per-instance defaults here. If the element later has properties, counters,
or cached state, initialize them here.

## Plugin Registration

`GST_PLUGIN_DEFINE` exposes the plugin descriptor that GStreamer scans from the
shared library.

```cpp
GST_PLUGIN_DEFINE(..., minimalfilter, ..., plugin_init, ...)
```

When GStreamer loads the `.so`, it calls `plugin_init()`. That function registers
the `minimalfilter` element factory.

## Mental Model

Think of this plugin as:

```text
raw video buffer in -> transform_ip() -> same raw video buffer out
```

The current implementation does not change pixels. It only proves that the
element is loaded, linked into a pipeline, and called for each video buffer.
