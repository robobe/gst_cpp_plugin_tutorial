#!/usr/bin/env python3

import argparse
import os
import sys
from pathlib import Path

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib


DETECTION_META_NAME = "GstRedDetectionMeta"


def bool_text(value):
    return "true" if value else "false"


def structure_value(structure, name, default):
    value = structure.get_value(name)
    if value is None:
        return default
    return value


class CapstoneRunner:
    def __init__(self, args):
        self.args = args
        self.frame = 0
        self.loop = GLib.MainLoop()
        self.pipeline = None

    def build_pipeline(self):
        sink = "fakesink sync=false" if self.args.no_display else "autovideosink"
        source_limits = (
            f"num-buffers={self.args.num_buffers}"
            if self.args.num_buffers > 0
            else ""
        )

        pipeline_text = f"""
            colorboxsrc {source_limits}
            ! reddetect
                low-h={self.args.low_h}
                low-s={self.args.low_s}
                low-v={self.args.low_v}
                high-h={self.args.high_h}
                high-s={self.args.high_s}
                high-v={self.args.high_v}
            ! tee name=t
                t. ! queue ! videoconvert ! {sink}
                t. ! queue ! appsink name=metadata_sink
                    emit-signals=true
                    sync=false
                    max-buffers=1
                    drop=true
        """

        self.pipeline = Gst.parse_launch(pipeline_text)

        appsink = self.pipeline.get_by_name("metadata_sink")
        appsink.connect("new-sample", self.on_new_sample)

        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_bus_message)

    def on_new_sample(self, appsink):
        sample = appsink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.ERROR

        buffer = sample.get_buffer()
        meta = buffer.get_custom_meta(DETECTION_META_NAME)

        if meta is None:
            print(
                f"frame={self.frame} found=false x=0 y=0 width=0 height=0",
                flush=True,
            )
            self.frame += 1
            return Gst.FlowReturn.OK

        structure = meta.get_structure()
        found = bool(structure_value(structure, "found", False))
        x = int(structure_value(structure, "x", 0))
        y = int(structure_value(structure, "y", 0))
        width = int(structure_value(structure, "width", 0))
        height = int(structure_value(structure, "height", 0))

        print(
            f"frame={self.frame} found={bool_text(found)} "
            f"x={x} y={y} width={width} height={height}",
            flush=True,
        )

        self.frame += 1
        return Gst.FlowReturn.OK

    def on_bus_message(self, bus, message):
        if message.type == Gst.MessageType.ERROR:
            error, debug = message.parse_error()
            print(f"error: {error.message}", file=sys.stderr)
            if debug:
                print(f"debug: {debug}", file=sys.stderr)
            self.loop.quit()
            return

        if message.type == Gst.MessageType.EOS:
            self.loop.quit()

    def run(self):
        self.build_pipeline()
        self.pipeline.set_state(Gst.State.PLAYING)

        try:
            self.loop.run()
        except KeyboardInterrupt:
            pass
        finally:
            self.pipeline.set_state(Gst.State.NULL)


def default_plugin_path():
    return Path(__file__).resolve().parents[1] / "build"


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the capstone colorboxsrc -> reddetect split pipeline."
    )
    parser.add_argument("--low-h", type=int, default=0)
    parser.add_argument("--low-s", type=int, default=100)
    parser.add_argument("--low-v", type=int, default=100)
    parser.add_argument("--high-h", type=int, default=10)
    parser.add_argument("--high-s", type=int, default=255)
    parser.add_argument("--high-v", type=int, default=255)
    parser.add_argument(
        "--num-buffers",
        type=int,
        default=0,
        help="Stop after this many source buffers. Use 0 to run until interrupted.",
    )
    parser.add_argument(
        "--no-display",
        action="store_true",
        help="Use fakesink instead of autovideosink for headless testing.",
    )
    return parser.parse_args()


def main():
    os.environ.setdefault("GST_PLUGIN_PATH", str(default_plugin_path()))

    Gst.init(None)

    args = parse_args()
    runner = CapstoneRunner(args)
    runner.run()


if __name__ == "__main__":
    main()
