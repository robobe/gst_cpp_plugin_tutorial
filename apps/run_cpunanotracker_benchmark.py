#!/usr/bin/env python3
"""Interactively benchmark cpunanotrack on a video from a YAML catalog."""

import argparse
import os
import re
import sys
import time
import tkinter as tk
from collections import deque
from pathlib import Path
from tkinter import filedialog, ttk

import cv2
import gi
import numpy as np
import yaml

gi.require_version("Gst", "1.0")
gi.require_version("GstVideo", "1.0")
from gi.repository import GLib, Gst, GstVideo


WINDOW_TITLE = "CPU NanoTrack benchmark"
NANOTRACK_ID = GLib.quark_from_string("nanotrack")
ROI_META_ID = 0
LOST_CONFIDENCE = 0.50
FPS_CHOICES = ("Auto", "1", "5", "10", "20", "30")
IMAGE_SEQUENCE = re.compile(r"^(.*?)(\d+)(\.(?:jpg|jpeg|png))$", re.IGNORECASE)


def load_videos(config_path):
    try:
        data = yaml.safe_load(config_path.read_text()) or {}
    except (OSError, yaml.YAMLError) as error:
        raise ValueError(f"Cannot read {config_path}: {error}") from error

    videos = data.get("videos")
    if not isinstance(videos, dict) or not videos:
        raise ValueError(f"{config_path}: videos must be a non-empty name-to-path mapping")

    resolved = {}
    for name, path in videos.items():
        if not isinstance(name, str) or not isinstance(path, str) or not name or not path:
            raise ValueError(f"{config_path}: every video name and path must be non-empty text")
        video_path = Path(path)
        resolved[name] = (config_path.parent / video_path).resolve() if not video_path.is_absolute() else video_path
    return resolved


def gst_string(value):
    return '"' + str(value).replace("\\", "\\\\").replace('"', '\\"') + '"'


def image_sequence(directory):
    groups = {}
    for image in directory.iterdir():
        match = IMAGE_SEQUENCE.match(image.name) if image.is_file() else None
        if match:
            prefix, index, suffix = match.groups()
            key = prefix, len(index), suffix.lower()
            groups.setdefault(key, []).append(int(index))
    if not groups:
        raise ValueError(f"{directory}: no numbered PNG/JPEG images found")
    if len(groups) != 1:
        raise ValueError(f"{directory}: expected one numbered image pattern")

    (prefix, width, suffix), indices = next(iter(groups.items()))
    indices.sort()
    if indices != list(range(indices[0], indices[-1] + 1)):
        raise ValueError(f"{directory}: image indexes must be continuous")
    mime, decoder = ("image/png", "pngdec") if suffix == ".png" else ("image/jpeg", "jpegdec")
    pattern = f"{directory}/{prefix}%0{width}d{suffix}"
    return pattern, indices[0], indices[-1], mime, decoder


def validate_source(path):
    if path.is_file():
        return
    if path.is_dir():
        image_sequence(path)
        return
    raise ValueError(f"Missing source: {path}")


def frame_from_sample(sample):
    caps = sample.get_caps().get_structure(0)
    ok_width, width = caps.get_int("width")
    ok_height, height = caps.get_int("height")
    if not ok_width or not ok_height:
        raise RuntimeError("appsink sample has no video dimensions")

    buffer = sample.get_buffer()
    video_meta = GstVideo.buffer_get_video_meta(buffer)
    stride = video_meta.stride[0] if video_meta else width * 3
    ok, mapped = buffer.map(Gst.MapFlags.READ)
    if not ok:
        raise RuntimeError("Cannot map appsink frame")
    try:
        return np.ndarray((height, width, 3), dtype=np.uint8, buffer=mapped.data,
                          strides=(stride, 3, 1)).copy()
    finally:
        buffer.unmap(mapped)


def tracking_result(buffer):
    meta = GstVideo.buffer_get_video_region_of_interest_meta_id(buffer, ROI_META_ID)
    if meta is None or meta.roi_type != NANOTRACK_ID:
        return None
    params = meta.get_param("nanotrack")
    if params is None:
        return None
    initialized = bool(params.get_value("initialized"))
    confidence = params.get_value("confidence")
    return meta.x, meta.y, meta.w, meta.h, initialized, confidence


class BenchmarkApp:
    def __init__(self, videos, root_dir):
        self.videos = videos
        self.root_dir = root_dir
        self.pipeline = None
        self.sink = None
        self.bus = None
        self.tracker = None
        self.paused = False
        self.roi_selected = False
        self.last_frame = None
        self.frame_times = deque()
        self.reset_statistics()

        self.root = tk.Tk()
        self.root.title("CPU NanoTrack benchmark")
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.selected = tk.StringVar(value=next(iter(videos)))
        self.status = tk.StringVar(value="Choose a video and load its first frame")

        main = ttk.Frame(self.root, padding=12)
        main.grid(row=0, column=0, sticky="nsew")
        ttk.Label(main, text="Source").grid(row=0, column=0, sticky="w")
        self.combo = ttk.Combobox(main, textvariable=self.selected, values=list(videos), state="readonly", width=28)
        self.combo.grid(row=0, column=1, sticky="ew", padx=(8, 0))
        self.fps = tk.StringVar(value="Auto")
        ttk.Label(main, text="Playback FPS").grid(row=1, column=0, sticky="w")
        ttk.Combobox(main, textvariable=self.fps, values=FPS_CHOICES, state="readonly", width=28).grid(
            row=1, column=1, sticky="ew", padx=(8, 0)
        )
        ttk.Button(main, text="Load selected", command=self.load_video).grid(row=2, column=0, sticky="ew", pady=(8, 4))
        ttk.Button(main, text="Browse file", command=self.browse_file).grid(row=2, column=1, sticky="ew", padx=(8, 0), pady=(8, 4))
        ttk.Button(main, text="Browse folder", command=self.browse_folder).grid(row=3, column=0, columnspan=2, sticky="ew", pady=(4, 4))
        self.play_button = ttk.Button(main, text="Play", command=self.toggle_pause, state="disabled")
        self.play_button.grid(row=4, column=0, sticky="ew", pady=(4, 4))
        self.roi_button = ttk.Button(main, text="Select ROI", command=self.select_roi, state="disabled")
        self.roi_button.grid(row=4, column=1, sticky="ew", padx=(8, 0), pady=(4, 4))
        ttk.Label(main, textvariable=self.status, wraplength=300).grid(row=5, column=0, columnspan=2, sticky="w")
        main.columnconfigure(1, weight=1)

    def update_controls(self):
        state = "normal" if self.pipeline else "disabled"
        self.play_button.configure(state=state, text="Play" if self.paused else "Pause")
        self.roi_button.configure(state=state)

    def reset_statistics(self):
        self.stats_started_at = None
        self.stats_elapsed = 0.0
        self.stats_frames = 0
        self.confidence_sum = 0.0
        self.confidence_frames = 0
        self.low_confidence_frames = 0

    def statistics_summary(self):
        if not self.roi_selected or not self.stats_frames:
            return "Video finished — no tracker statistics (no ROI was tracked)"
        elapsed = self.stats_elapsed
        if self.stats_started_at is not None:
            elapsed += time.monotonic() - self.stats_started_at
        average_fps = self.stats_frames / max(elapsed, 1e-9)
        if not self.confidence_frames:
            return f"Video finished — average FPS: {average_fps:.2f}; no confidence frames"
        average_score = self.confidence_sum / self.confidence_frames
        lost = "yes" if self.low_confidence_frames else "no"
        return (
            f"Video finished — average FPS: {average_fps:.2f}; "
            f"average confidence: {average_score:.3f}; tracker lost: {lost} "
            f"({self.low_confidence_frames}/{self.confidence_frames} frames below {LOST_CONFIDENCE:.2f})"
        )

    def build_pipeline(self, source_path):
        models = gst_string(self.root_dir / "demos/nanotracker/onnx")
        fps = self.fps.get()
        output_caps = "video/x-raw,format=BGR,interlace-mode=progressive"
        if source_path.is_file():
            source = f"filesrc location={gst_string(source_path)} ! decodebin ! videoconvert"
            if fps != "Auto":
                source += " ! videorate"
                output_caps += f",framerate={fps}/1"
            self.source_kind = "video"
        elif source_path.is_dir():
            pattern, start, stop, mime, decoder = image_sequence(source_path)
            sequence_fps = "20" if fps == "Auto" else fps
            source = (
                f"multifilesrc location={gst_string(pattern)} start-index={start} stop-index={stop} "
                f"caps={gst_string(f'{mime},framerate={sequence_fps}/1')} ! {decoder} ! videoconvert"
            )
            self.source_kind = f"image sequence ({sequence_fps} FPS)"
        else:
            raise ValueError(f"Missing source: {source_path}")
        description = (
            f"{source} ! {output_caps} ! "
            f"cpunanotrack name=tracker enabled=false models-dir={models} ! "
            f"appsink name=sink sync=true max-buffers=1 drop=true"
        )
        self.pipeline = Gst.parse_launch(description)
        self.tracker = self.pipeline.get_by_name("tracker")
        self.sink = self.pipeline.get_by_name("sink")
        self.bus = self.pipeline.get_bus()

    def load_video(self):
        self.load_source(self.videos[self.selected.get()])

    def browse_file(self):
        source = filedialog.askopenfilename(title="Choose a video file")
        if source:
            self.load_source(Path(source))

    def browse_folder(self):
        source = filedialog.askdirectory(title="Choose an image-sequence folder")
        if source:
            self.load_source(Path(source))

    def load_source(self, source_path):
        self.stop_video()
        try:
            validate_source(source_path)
        except ValueError as error:
            self.status.set(str(error))
            return

        try:
            self.build_pipeline(source_path)
            self.pipeline.set_state(Gst.State.PAUSED)
            result, _, _ = self.pipeline.get_state(10 * Gst.SECOND)
            if result == Gst.StateChangeReturn.FAILURE:
                raise RuntimeError("GStreamer could not pause the pipeline")
            sample = self.sink.emit("pull-preroll")
            if sample is None:
                raise RuntimeError("Video has no decodable first frame")
            self.last_frame = frame_from_sample(sample)
            cv2.namedWindow(WINDOW_TITLE, cv2.WINDOW_NORMAL)
            cv2.imshow(WINDOW_TITLE, self.last_frame)
            self.paused = True
            self.roi_selected = False
            self.reset_statistics()
            self.update_controls()
            self.status.set(f"Ready ({self.source_kind}) — Play now, or select an ROI before playing")
        except (GLib.Error, RuntimeError, ValueError) as error:
            self.status.set(str(error))
            self.stop_video()

    def select_roi(self):
        if not self.pipeline or self.last_frame is None:
            return
        if not self.paused:
            self.status.set("Pause playback before selecting an ROI")
            return
        roi = cv2.selectROI(WINDOW_TITLE, self.last_frame, showCrosshair=True, fromCenter=False)
        if roi[2] < 1 or roi[3] < 1:
            self.status.set("ROI selection cancelled")
            return
        self.tracker.set_property("roi", ",".join(str(value) for value in roi))
        self.tracker.set_property("enabled", True)
        self.roi_selected = True
        self.frame_times.clear()
        self.reset_statistics()
        self.status.set("ROI selected — press Play to start tracking")

    def show_frame(self, sample):
        frame = frame_from_sample(sample)
        result = tracking_result(sample.get_buffer())
        now = time.monotonic()
        self.frame_times.append(now)
        while self.frame_times and now - self.frame_times[0] > 1.0:
            self.frame_times.popleft()
        fps = len(self.frame_times)

        text = f"FPS: {fps:.0f}"
        if result is None:
            text += "  select ROI" if not self.roi_selected else "  initializing"
        else:
            x, y, width, height, initialized, confidence = result
            score = float(confidence) if confidence is not None else None
            color = (0, 255, 255) if initialized or score is None else (0, 0, 255) if score < LOST_CONFIDENCE else (0, 255, 0)
            cv2.rectangle(frame, (x, y), (x + width, y + height), color, 2)
            text += "  initializing" if initialized or score is None else f"  confidence: {score:.3f}"
            if self.roi_selected:
                self.stats_frames += 1
                if score is not None:
                    self.confidence_sum += score
                    self.confidence_frames += 1
                    if score < LOST_CONFIDENCE:
                        self.low_confidence_frames += 1
        cv2.putText(frame, text, (12, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)
        self.last_frame = frame
        cv2.imshow(WINDOW_TITLE, frame)

    def check_bus(self):
        while True:
            message = self.bus.timed_pop_filtered(0, Gst.MessageType.ERROR | Gst.MessageType.EOS)
            if message is None:
                return False
            if message.type == Gst.MessageType.ERROR:
                error, _ = message.parse_error()
                self.status.set(f"GStreamer error: {error.message}")
            else:
                summary = self.statistics_summary()
                print(summary, flush=True)
                self.status.set(summary)
            self.stop_video()
            return True

    def toggle_pause(self):
        if not self.pipeline:
            return
        now = time.monotonic()
        if self.paused and self.roi_selected:
            self.stats_started_at = now
        elif not self.paused and self.stats_started_at is not None:
            self.stats_elapsed += now - self.stats_started_at
            self.stats_started_at = None
        self.paused = not self.paused
        self.pipeline.set_state(Gst.State.PAUSED if self.paused else Gst.State.PLAYING)
        self.update_controls()
        self.status.set("Paused — select an ROI or press Play" if self.paused else "Playing")

    def stop_video(self):
        if self.pipeline:
            self.pipeline.set_state(Gst.State.NULL)
        self.pipeline = self.sink = self.bus = self.tracker = None
        self.paused = False
        self.roi_selected = False
        self.frame_times.clear()
        self.reset_statistics()
        self.update_controls()
        try:
            cv2.destroyWindow(WINDOW_TITLE)
        except cv2.error:
            pass

    def tick(self):
        if self.pipeline and not self.check_bus() and not self.paused:
            sample = self.sink.emit("try-pull-sample", 0)
            if sample is not None:
                try:
                    self.show_frame(sample)
                except RuntimeError as error:
                    self.status.set(str(error))
                    self.stop_video()

        key = cv2.waitKey(1) & 0xFF
        if key in (27, ord("q")):
            self.stop_video()
        elif key == ord(" "):
            self.toggle_pause()
        elif key == ord("r"):
            self.select_roi()
        self.root.after(1, self.tick)

    def run(self):
        self.root.after(1, self.tick)
        self.root.mainloop()

    def close(self):
        self.stop_video()
        self.root.destroy()


def parse_args():
    parser = argparse.ArgumentParser(description="Interactive cpunanotrack benchmark viewer")
    parser.add_argument("--config", type=Path, default=Path(__file__).with_name("cpunanotracker_benchmark.yaml"))
    parser.add_argument("--check-config", action="store_true", help="Validate the YAML catalog without opening windows")
    return parser.parse_args()


def main():
    args = parse_args()
    config_path = args.config.resolve()
    try:
        videos = load_videos(config_path)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 1

    if args.check_config:
        try:
            for path in videos.values():
                validate_source(path)
        except ValueError as error:
            print(error, file=sys.stderr)
            return 1
        print(f"Valid catalog: {len(videos)} source(s)")
        return 0

    root_dir = Path(__file__).resolve().parents[1]
    plugin_dir = root_dir / "build-cpunanotracker"
    existing = os.environ.get("GST_PLUGIN_PATH")
    os.environ["GST_PLUGIN_PATH"] = str(plugin_dir) + (f":{existing}" if existing else "")
    Gst.init(None)
    BenchmarkApp(videos, root_dir).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
