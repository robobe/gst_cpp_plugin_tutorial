# Bridge Project Requirements

## Goal

Build a Python-controlled bridge project that extends the capstone pipeline with
external command and telemetry channels.

The bridge demo should:

- run the GStreamer video pipeline from Python
- receive detector control commands over ZMQ
- publish detection results from the pipeline over ZMQ
- serialize all ZMQ messages with MessagePack
- keep the GStreamer pipeline independent from bridge and GUI code
- avoid letting slow ZMQ clients or GUI updates block video processing

## Final Architecture

The project should have three separated parts:

```text
GStreamer pipeline
    produces video and detection metadata

Python bridge
    owns the pipeline
    receives ZMQ commands
    applies commands to the detector element
    publishes detection results from appsink

Python desktop GUI
    sends ZMQ commands
    subscribes to detection results
    displays detector state and latest result
```

The GUI must communicate with the detector only through ZMQ. It must not import
or directly control the pipeline implementation.

## Final Pipeline

The final pipeline should have this shape:

```text
moving red-box source
-> controllable red detection filter
-> tee

tee branch 1:
queue -> videoconvert -> autovideosink

tee branch 2:
queue -> appsink -> Python bridge publishes detection metadata
```

Example conceptual pipeline:

```text
colorboxsrc ! controlledreddetect name=detector detection-enabled=true ! tee name=t \
    t. ! queue ! videoconvert ! autovideosink \
    t. ! queue ! appsink name=metadata_sink emit-signals=true sync=false
```

The exact Python construction can use `Gst.parse_launch` or programmatic element
creation, but the behavior must match this pipeline.

## Required Components

The bridge project must include:

- A Python GStreamer application that builds and runs the pipeline.
- A detector plugin based on `reddetect`.
- A ZMQ command subscriber that receives control messages.
- A ZMQ telemetry publisher that publishes detector metadata.
- MessagePack serialization for all command and telemetry payloads.
- A metadata branch that ends in `appsink`.
- A Python desktop GUI for detector control and result display.
- Clear separation between pipeline logic, ZMQ transport logic, message models,
  and GUI code.

## Plugin Requirements

### Controllable Red Detection Filter

The detector plugin should be named `controlledreddetect`.

The plugin must be based on the existing `reddetect` behavior:

- Receive raw RGB video buffers.
- Map each `GstBuffer` safely with `GstVideoFrame`.
- Use OpenCV HSV thresholding when detection is enabled.
- Compute one bounding box around the detected red region.
- Attach custom detection metadata to every buffer.
- Forward the buffer downstream.

The plugin must expose the same HSV threshold properties as `reddetect`:

- `low-h`
- `low-s`
- `low-v`
- `high-h`
- `high-s`
- `high-v`

The plugin must also expose:

- `detection-enabled`: boolean, default `true`

When `detection-enabled=true`:

- the plugin runs the OpenCV detection routine
- metadata reports the real detection result

When `detection-enabled=false`:

- the plugin skips the OpenCV detection routine
- metadata still exists
- `found` is `false`
- `x`, `y`, `width`, and `height` are `0`

## Metadata Requirements

The detector metadata must use the same field names as the capstone detector:

- `found`: boolean
- `x`: integer
- `y`: integer
- `width`: integer
- `height`: integer

The bridge may also add application-level fields to the published ZMQ message,
such as frame number and timestamp, but the detector metadata fields must remain
unchanged.

## Python Bridge Requirements

The Python bridge application must:

- Use GStreamer Python bindings.
- Set `GST_PLUGIN_PATH` or document that it must point at `build`.
- Build and start the full split pipeline.
- Keep a reference to the `controlledreddetect` element as `detector`.
- Pull samples from `appsink`.
- Read detection metadata from each sample buffer.
- Publish detection results over ZMQ.
- Receive ZMQ commands and apply them to the detector element.
- Stop cleanly on `EOS`, GStreamer error, `KeyboardInterrupt`, or GUI shutdown.

The bridge should keep these responsibilities separate:

- pipeline setup and lifecycle
- appsink sample handling
- detector command handling
- ZMQ socket management
- MessagePack encoding and decoding

The bridge must not do long-running ZMQ or GUI work in the GStreamer streaming
callback. The appsink callback should copy the small metadata payload and return
quickly.

## ZMQ And MessagePack Requirements

The bridge must use two ZMQ sockets:

- Command input: `SUB`, default endpoint `tcp://127.0.0.1:5555`
- Detector output: `PUB`, default endpoint `tcp://127.0.0.1:5556`

All ZMQ payloads must be MessagePack maps.

### Command Messages

Enable or disable detection:

```text
{
  "type": "set_detection_enabled",
  "enabled": true
}
```

Update HSV thresholds:

```text
{
  "type": "set_hsv_thresholds",
  "low_h": 0,
  "low_s": 100,
  "low_v": 100,
  "high_h": 10,
  "high_s": 255,
  "high_v": 255
}
```

Unknown command types should be ignored with a warning instead of stopping the
pipeline.

Invalid command values should be rejected with a warning. The bridge should keep
the previous detector property values.

### Detection Output Messages

The bridge must publish one detection message for each appsink sample:

```text
{
  "type": "detection",
  "frame": 42,
  "timestamp_ns": 1400000000,
  "found": true,
  "x": 120,
  "y": 80,
  "width": 40,
  "height": 40
}
```

If detection is disabled or no red object is detected:

```text
{
  "type": "detection",
  "frame": 42,
  "timestamp_ns": 1400000000,
  "found": false,
  "x": 0,
  "y": 0,
  "width": 0,
  "height": 0
}
```

## GUI Requirements

Create a small Python desktop GUI.

The GUI must:

- Publish detector control commands to the bridge command endpoint.
- Subscribe to detector output messages from the bridge telemetry endpoint.
- Provide an enable/disable detection control.
- Provide controls for HSV threshold values.
- Show the latest detection result.
- Show whether the bridge is currently publishing messages.

The GUI should not depend on GStreamer. It should only use the ZMQ command and
telemetry protocol.

## Performance And Separation Requirements

The ZMQ bridge must not influence pipeline performance.

The implementation should satisfy this by:

- using `queue` elements before the video sink and appsink branches
- configuring appsink with bounded buffering, such as `max-buffers=1 drop=true`
- keeping appsink callbacks short
- moving ZMQ publish/subscribe work outside the streaming callback
- dropping stale detection updates if the GUI cannot keep up

The project should follow SOLID design guidelines:

- Pipeline code should not know about GUI widgets.
- GUI code should not know about GStreamer elements.
- Message serialization should be isolated from socket setup.
- Detector commands should be represented by small explicit command handlers.
- Each class or module should have one clear reason to change.

## Acceptance Criteria

The bridge project is complete when:

- `cmake --build build` succeeds.
- `gst-inspect-1.0 controlledreddetect` shows the HSV properties and
  `detection-enabled`.
- A command-line pipeline can run
  `colorboxsrc ! controlledreddetect ! metaprint ! fakesink`.
- When `detection-enabled=true`, metadata reports moving bounding boxes.
- When `detection-enabled=false`, metadata reports `found=false` and zero
  dimensions.
- The Python bridge starts the split pipeline and publishes MessagePack
  detection messages over ZMQ.
- ZMQ commands can enable and disable detection while the pipeline is running.
- ZMQ commands can update HSV thresholds while the pipeline is running.
- The GUI can send commands and display the latest detection result.
- A slow or disconnected GUI does not stop or visibly slow the video pipeline.
- The bridge stops cleanly on `EOS`, errors, or user interruption.

## Suggested Milestones

1. Create `controlledreddetect` based on `reddetect`.
2. Add the `detection-enabled` property.
3. Verify enabled and disabled metadata with `metaprint`.
4. Create Python message encode/decode helpers using MessagePack.
5. Create the Python bridge pipeline runner.
6. Add appsink metadata reading and detection-message publishing.
7. Add the ZMQ command subscriber.
8. Apply command messages to detector properties.
9. Build the split pipeline with video and metadata branches.
10. Create the Python desktop GUI.
11. Verify GUI commands change detector behavior while video is running.
12. Document final bridge and GUI commands.
