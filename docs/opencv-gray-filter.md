# OpenCV Gray Filter

`grayfilter` is a minimal `GstBaseTransform` plugin based on `minimalfilter`.
It maps each video buffer, wraps the buffer memory with `cv::Mat`, converts the
image to grayscale, and writes the grayscale pixels back into the same buffer.

## Pipeline

Display generated video from `simplevideosrc` through `grayfilter`:

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 simplevideosrc ! grayfilter ! videoconvert ! autovideosink
```

Headless test:

```sh
GST_PLUGIN_PATH="$PWD/build" gst-launch-1.0 simplevideosrc num-buffers=5 ! grayfilter ! fakesink
```

## Why RGB

`simplevideosrc` outputs:

```text
video/x-raw,format=RGB,width=320,height=240,framerate=30/1
```

So `grayfilter` also declares RGB on its sink and source pads. The plugin keeps
the output as 3-channel RGB, but each channel has the same gray value. That lets
the filter modify the buffer in place without changing caps.

## Mapping GstBuffer To cv::Mat

Do not treat every video buffer as tightly packed memory. Video frames can have
padding at the end of each row. Use `GstVideoFrame` so GStreamer gives you the
real width, height, plane pointer, and stride.

```cpp
GstVideoFrame video_frame;
gst_video_frame_map(
    &video_frame,
    &video_info,
    buffer,
    GST_MAP_READWRITE
);
```

Then get the plane data:

```cpp
guint8* pixels = GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0);
gint width = GST_VIDEO_FRAME_WIDTH(&video_frame);
gint height = GST_VIDEO_FRAME_HEIGHT(&video_frame);
gsize stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);
```

Wrap that memory with OpenCV:

```cpp
cv::Mat rgb(height, width, CV_8UC3, pixels, stride);
```

This does not copy the image. `cv::Mat` points directly at the mapped
`GstBuffer` memory.

## Converting To Gray

OpenCV conversion:

```cpp
cv::Mat gray;
cv::cvtColor(rgb, gray, cv::COLOR_RGB2GRAY);
cv::cvtColor(gray, rgb, cv::COLOR_GRAY2RGB);
```

The second conversion writes the grayscale result back into the original RGB
buffer.

## Releasing The Buffer

After OpenCV is done, unmap the frame:

```cpp
gst_video_frame_unmap(&video_frame);
```

That releases the mapping. Do not keep the `cv::Mat` after unmapping, because it
points to memory owned by the mapped GStreamer buffer.
