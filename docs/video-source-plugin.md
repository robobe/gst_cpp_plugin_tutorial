# Minimal Video Source Plugin

The simplest way to generate video in a GStreamer plugin is to subclass
`GstPushSrc`.

`GstPushSrc` asks your element for one buffer at a time by calling `create()`.
Your code allocates a `GstBuffer`, fills it with pixels, timestamps it, and
returns it.

## Example Element

This project includes:

```text
simplevideosrc
```

It generates fixed raw RGB video:

```text
video/x-raw,format=RGB,width=320,height=240,framerate=30/1
```

## Run

Headless test:

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 simplevideosrc num-buffers=5 ! fakesink
```

Display test:

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 simplevideosrc ! videoconvert ! autovideosink
```

## Creating One Video Buffer

The minimal buffer flow is:

```cpp
GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame_size, nullptr);

GstMapInfo map_info;
gst_buffer_map(buffer, &map_info, GST_MAP_WRITE);

// Write RGB bytes into map_info.data.

gst_buffer_unmap(buffer, &map_info);
```

For RGB video, each pixel is 3 bytes:

```text
R G B
```

For a `320x240` RGB frame:

```cpp
frame_size = 320 * 240 * 3;
```

The byte offset for pixel `(x, y)` is:

```cpp
offset = (y * width + x) * 3;
```

Then write:

```cpp
data[offset + 0] = red;
data[offset + 1] = green;
data[offset + 2] = blue;
```

## Timestamps

A source should timestamp buffers. For 30 fps, each frame lasts:

```cpp
duration = gst_util_uint64_scale_int(GST_SECOND, 1, 30);
```

Then set:

```cpp
GST_BUFFER_PTS(buffer) = frame_number * duration;
GST_BUFFER_DURATION(buffer) = duration;
```

Without timestamps, downstream elements may not know when frames should be
displayed.

## Mental Model

```text
GstPushSrc create()
-> allocate GstBuffer
-> map writable memory
-> write pixels
-> set timestamp
-> return buffer
```
