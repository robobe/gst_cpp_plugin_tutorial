# GStreamer tracker benchmark viewer

The viewer loads configured video files or numbered image-sequence folders as
a player. It pauses on the first frame, lets you play without tracking, and
lets you pause later to select an ROI. It displays the selected tracker rectangle,
confidence, and playback FPS.

Build the tracker first from the repository root:

```bash
cmake -S src/cpunanotracker -B build-cpunanotracker -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpunanotracker
cmake -S src/cpulighttrack -B build-cpulighttrack -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpulighttrack
python3 apps/run_cpunanotracker_benchmark.py
```

The source combo box is populated from
`apps/cpunanotracker_benchmark.yaml`:

```yaml
videos:
  camera-run: ../assets/camera_run_2s_10s.mp4
  detection-demo: ../assets/detection-demo.mp4
trackers:
  CPU NanoTrack:
    element: cpunanotrack
    models-dir: ../demos/nanotracker/onnx
    metadata: nanotrack
  CPU LightTrack:
    element: cpulighttrack
    models-dir: ../demos/lighttrack/onnx
    metadata: lighttrack
```

Paths are relative to the YAML file. A path may be a video file or a folder
containing one continuous numbered PNG, JPG, or JPEG sequence, such as
`frame_0001.jpg`, `frame_0002.jpg`. The folder must contain exactly one image
name pattern with no missing indexes. Use another catalog with
`--config /path/to/videos.yaml`; validate it without opening windows with
`--check-config`.

Choose **Auto**, **1**, **5**, **10**, **20**, or **30** from **Playback FPS**
before loading. Auto preserves a video file's source timing and plays image
sequences at 20 FPS. A selected number sets the playback rate for either type.
Choose a tracker before loading: the player builds its pipeline using that
tracker's `element`, `models-dir`, and ROI `metadata` configuration.

Click **Load selected** to load the chosen YAML entry. **Browse file** loads a
video outside the catalog; **Browse folder** loads an image sequence outside
the catalog. From the control window, click **Play** to watch without
tracking, or click **Select ROI** while paused and drag the target rectangle in
the OpenCV frame. After an ROI is selected, click **Play** to start tracking.
During playback, Space pauses or resumes, `r` opens ROI selection while
paused, and `q` or Esc stops the current video. Selecting a new ROI while
paused reinitializes the tracker when playback resumes.

FPS is the number of frames processed during the last second and reflects the
selected playback pace (or source timing in Auto mode).
Confidence is omitted on the first tracker-initialization frame and shown from
the following frame onward. At end of stream, the Tk status text and terminal
report average tracking FPS, average confidence, and whether the tracker was
lost. The CPU NanoTrack plugin has no explicit lost event, so this viewer marks
it lost if any confidence score is below `0.50`; the report includes the count
of low-confidence frames. The live box is yellow while initializing, green at
or above `0.50`, and red below `0.50`.
