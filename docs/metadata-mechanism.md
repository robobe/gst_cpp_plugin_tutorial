# GStreamer Metadata Mechanism

GStreamer has more than one kind of metadata. The right choice depends on what
the data describes.

## Common Metadata Types

- Buffer metadata: data attached to one `GstBuffer`, such as a frame number,
  detection result, region of interest, timestamp detail, or custom per-frame
  value.
- Caps fields: format information negotiated between elements, such as width,
  height, pixel format, and framerate.
- Tags: stream-level information such as title, artist, codec, or container
  metadata.
- Events and messages: control or status data that travels through the pipeline
  or to the application bus.

For this tutorial, start with buffer metadata because it is the simplest match
for "attach information to this frame and read it downstream".

## Simple Choice: GstCustomMeta

This project uses `GstCustomMeta`.

`GstCustomMeta` is a named buffer metadata object that stores fields in a
`GstStructure`. That means the metadata can hold simple typed values without
creating a full custom C struct.

The two example elements are:

- `metaattach`: adds `GstTutorialMeta` to each buffer.
- `metaprint`: reads `GstTutorialMeta` and prints it.

## Run

```sh
cmake --build build
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 videotestsrc num-buffers=3 ! video/x-raw ! metaattach ! metaprint ! fakesink
```

Expected output:

```text
metaprint: message="hello from metaattach" frame=0 pts=0:00:00.000000000
metaprint: message="hello from metaattach" frame=1 pts=0:00:00.033333333
metaprint: message="hello from metaattach" frame=2 pts=0:00:00.066666666
```

## How The Attach Element Works

`metaattach` registers a metadata name:

```cpp
gst_meta_register_custom_simple("GstTutorialMeta");
```

Then it adds that metadata to each buffer:

```cpp
GstCustomMeta* meta = gst_buffer_add_custom_meta(buffer, "GstTutorialMeta");
```

The metadata stores fields in a `GstStructure`:

```cpp
GstStructure* structure = gst_custom_meta_get_structure(meta);
gst_structure_set(
    structure,
    "message", G_TYPE_STRING, "hello from metaattach",
    "frame-number", G_TYPE_UINT64, frame_count,
    NULL
);
```

## How The Reader Element Works

`metaprint` asks the buffer for metadata with the same name:

```cpp
GstCustomMeta* meta = gst_buffer_get_custom_meta(buffer, "GstTutorialMeta");
```

Then it reads fields from the structure:

```cpp
GstStructure* structure = gst_custom_meta_get_structure(meta);
const gchar* message = gst_structure_get_string(structure, "message");
```

## Important Limit

This simple example works when the same buffer travels from `metaattach` to
`metaprint`.

Some elements copy or replace buffers. A simple custom meta may not survive
those operations unless a transform function is registered for the metadata.
That is the next step after this basic example.
