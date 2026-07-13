# GStreamer Plugin Learning Path

This learning path is for building C++ GStreamer plugins step by step. The
examples in this repository already cover a basic transform plugin, a plugin
with a property, and a plugin that uses OpenCV on video buffers.

## 1. Basic Plugin Shape

Goal: understand the required structure of a GStreamer plugin.

Learn:

- `G_DEFINE_TYPE`
- instance struct
- class struct
- `class_init`
- `init`
- `GST_PLUGIN_DEFINE`
- `gst_element_register`

Practice:

- Read `gstminimalfilter.cpp`.
- Run `gst-inspect-1.0 minimalfilter`.
- Change the plugin description and confirm it changes in `gst-inspect-1.0`.

Demo:

```bash
GST_PLUGIN_PATH=./build gst-inspect-1.0 minimalfilter
```

## 2. Buffer Processing

Goal: understand where most filter logic runs.

Learn:

- `GstBaseTransform`
- `transform_ip`
- `GstBuffer`
- `GST_FLOW_OK`
- in-place processing

Practice:

- Log buffer timestamps.
- Count processed buffers.
- Drop or pass buffers based on simple logic.

Demo:

```bash
GST_PLUGIN_PATH=./build gst-launch-1.0 -q \
    videotestsrc num-buffers=5 ! \
    minimalfilter ! \
    fakesink
```

## 3. Timestamps

Goal: understand timing information attached to buffers.

Learn:

- `GST_BUFFER_PTS`
- `GST_BUFFER_DTS`
- `GST_BUFFER_DURATION`
- `GST_BUFFER_OFFSET`
- `GST_CLOCK_TIME_IS_VALID`
- `GST_TIME_FORMAT`
- `GST_TIME_ARGS`

Practice:

- Print PTS and duration for each buffer.
- Compare timestamps from `videotestsrc` with different framerates.

Demo:

```bash
GST_PLUGIN_PATH=./build gst-launch-1.0 -q \
    videotestsrc num-buffers=5 ! \
    video/x-raw,framerate=10/1 ! \
    minimalfilter ! \
    fakesink
```

## 4. GObject Properties

Goal: make plugins configurable from a pipeline.

Learn:

- property IDs
- `set_property`
- `get_property`
- `g_object_class_install_property`
- `g_param_spec_boolean`
- default values

Practice:

- Read `gstpropertyfilter.cpp`.
- Add an integer property named `print-every`.
- Print only every Nth buffer.

Demo:

```bash
GST_PLUGIN_PATH=./build gst-launch-1.0 -q \
    videotestsrc num-buffers=5 ! \
    propertyfilter enabled=true ! \
    fakesink
```

## 5. Caps Negotiation

Goal: understand how a plugin knows the media format.

Learn:

- caps
- fixed caps
- pad templates
- `set_caps`
- `GstVideoInfo`
- width, height, format, stride

Practice:

- Read `gstopencvfilter.cpp`.
- Change the accepted format from `BGR` to `RGB`.
- Print negotiated width and height in `set_caps`.

Demo:

```bash
GST_PLUGIN_PATH=./build gst-launch-1.0 -q \
    videotestsrc num-buffers=5 ! \
    videoconvert ! \
    video/x-raw,format=BGR ! \
    opencvfilter ! \
    fakesink
```

## 6. Video Buffer Mapping

Goal: safely access raw video pixels.

Learn:

- `gst_buffer_map`
- `gst_video_frame_map`
- `gst_video_frame_unmap`
- `GST_VIDEO_FRAME_PLANE_DATA`
- `GST_VIDEO_FRAME_PLANE_STRIDE`

Practice:

- Invert image colors.
- Draw a rectangle.
- Draw text.
- Modify only a small region of the frame.

Demo:

```bash
GST_PLUGIN_PATH=./build gst-launch-1.0 \
    videotestsrc ! \
    videoconvert ! \
    video/x-raw,format=BGR ! \
    opencvfilter ! \
    videoconvert ! \
    autovideosink
```

## 7. Buffer Metadata

Goal: attach information to buffers without modifying pixels.

Learn:

- `GstMeta`
- `GstVideoMeta`
- `GstReferenceTimestampMeta`
- custom metadata
- copying metadata between buffers

Use metadata for:

- object detection boxes
- tracking IDs
- camera information
- inference results
- extra timestamps

Practice:

- Create one plugin that attaches simple metadata.
- Create another plugin that reads and prints that metadata.

Suggested exercise:

```text
metawriter ! metareader
```

The writer attaches metadata like:

```text
frame_index = 42
label = "demo"
```

The reader prints it downstream.

## 8. Events

Goal: understand control messages that travel through pads.

Learn:

- `CAPS`
- `SEGMENT`
- `EOS`
- `FLUSH_START`
- `FLUSH_STOP`
- `SEEK`
- `QOS`

Practice:

- Override event handling.
- Print when EOS arrives.
- Print segment information.

Useful command:

```bash
GST_DEBUG=GST_EVENT:5 gst-launch-1.0 videotestsrc num-buffers=3 ! fakesink
```

## 9. Queries

Goal: understand how elements ask each other for information.

Learn:

- `CAPS` query
- `POSITION` query
- `DURATION` query
- `LATENCY` query
- `ALLOCATION` query

Practice:

- Log allocation queries.
- Inspect how downstream asks for supported caps.

## 10. Allocation and Buffer Pools

Goal: learn performance-oriented buffer handling.

Learn:

- allocation negotiation
- buffer pools
- memory features
- zero-copy concepts
- DMABuf basics

Practice:

- Observe allocation queries.
- Learn when buffers are copied and when they are reused.

This topic matters more when working with cameras, hardware encoders, GPU
memory, or high-resolution video.

## 11. Bus Messages and Errors

Goal: report useful information to the application.

Learn:

- `GST_ELEMENT_ERROR`
- `GST_ELEMENT_WARNING`
- `gst_element_post_message`
- application bus
- custom messages

Practice:

- Post an error when caps are unsupported.
- Post a custom message every 100 frames.

## 12. Debug Logging

Goal: debug plugins without using `g_print` everywhere.

Learn:

- `GST_DEBUG_OBJECT`
- `GST_INFO_OBJECT`
- `GST_WARNING_OBJECT`
- debug categories
- `GST_DEBUG`

Practice:

- Replace some `g_print` calls with GStreamer logging macros.
- Run with different debug levels.

Example:

```bash
GST_DEBUG=propertyfilter:5 GST_PLUGIN_PATH=./build \
    gst-launch-1.0 videotestsrc num-buffers=3 ! propertyfilter ! fakesink
```

## Recommended Next Exercise

Build a metadata pair:

- `metawriter`: attaches frame index metadata to each buffer.
- `metareader`: reads and prints the metadata downstream.

Pipeline:

```bash
GST_PLUGIN_PATH=./build gst-launch-1.0 -q \
    videotestsrc num-buffers=5 ! \
    metawriter ! \
    metareader ! \
    fakesink
```

This exercise teaches a core GStreamer idea: buffers can carry both media data
and structured side information.