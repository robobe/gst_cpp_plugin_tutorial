import os
import queue
import logging
import sys
from pathlib import Path

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst

from .messages import (
    DetectionMessage,
    SetDetectionEnabledCommand,
    SetHsvThresholdsCommand,
)
from .queues import offer_latest


DETECTION_META_NAME = "GstRedDetectionMeta"
LOGGER = logging.getLogger(__name__)


def default_plugin_path():
    return Path(__file__).resolve().parents[2] / "build"


def structure_value(structure, name, default):
    value = structure.get_value(name)
    if value is None:
        return default
    return value


class BridgePipeline:
    def __init__(self, args, detection_queue, command_queue):
        self.args = args
        self.detection_queue = detection_queue
        self.command_queue = command_queue
        self.frame = 0
        self.loop = GLib.MainLoop()
        self.pipeline = None
        self.detector = None
        self.command_poller_id = None

    def build(self):
        sink = "fakesink sync=false" if self.args.no_display else "autovideosink"
        source_limits = (
            f"num-buffers={self.args.num_buffers}"
            if self.args.num_buffers > 0
            else ""
        )

        pipeline_text = f"""
            colorboxsrc {source_limits}
            ! controlledreddetect
                name=detector
                detection-enabled=true
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
        self.detector = self.pipeline.get_by_name("detector")

        appsink = self.pipeline.get_by_name("metadata_sink")
        appsink.connect("new-sample", self.on_new_sample)

        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_bus_message)

    def run(self):
        self.build()
        self.start_command_poller()
        self.pipeline.set_state(Gst.State.PLAYING)

        try:
            self.loop.run()
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    def stop(self):
        if self.command_poller_id is not None:
            GLib.source_remove(self.command_poller_id)
            self.command_poller_id = None

        if self.pipeline is not None:
            self.pipeline.set_state(Gst.State.NULL)

    def start_command_poller(self):
        self.command_poller_id = GLib.timeout_add(50, self.drain_commands)

    def drain_commands(self):
        while True:
            try:
                command = self.command_queue.get_nowait()
            except queue.Empty:
                return GLib.SOURCE_CONTINUE

            try:
                self.apply_command(command)
            except Exception:
                LOGGER.exception("failed to apply command")

    def apply_command(self, command):
        if isinstance(command, SetDetectionEnabledCommand):
            self.detector.set_property("detection-enabled", command.enabled)
            return

        if isinstance(command, SetHsvThresholdsCommand):
            self.detector.set_property("low-h", command.low_h)
            self.detector.set_property("low-s", command.low_s)
            self.detector.set_property("low-v", command.low_v)
            self.detector.set_property("high-h", command.high_h)
            self.detector.set_property("high-s", command.high_s)
            self.detector.set_property("high-v", command.high_v)
            return

        LOGGER.warning("ignoring unsupported command: %r", command)

    def on_new_sample(self, appsink):
        sample = appsink.emit("pull-sample")
        if sample is None:
            return Gst.FlowReturn.ERROR

        buffer = sample.get_buffer()
        message = self.read_detection_message(buffer)
        self.offer_detection(message)
        self.frame += 1

        return Gst.FlowReturn.OK

    def read_detection_message(self, buffer):
        timestamp_ns = int(buffer.pts) if buffer.pts != Gst.CLOCK_TIME_NONE else 0
        meta = buffer.get_custom_meta(DETECTION_META_NAME)

        if meta is None:
            return DetectionMessage(
                frame=self.frame,
                timestamp_ns=timestamp_ns,
                found=False,
                x=0,
                y=0,
                width=0,
                height=0,
            )

        structure = meta.get_structure()
        return DetectionMessage(
            frame=self.frame,
            timestamp_ns=timestamp_ns,
            found=bool(structure_value(structure, "found", False)),
            x=int(structure_value(structure, "x", 0)),
            y=int(structure_value(structure, "y", 0)),
            width=int(structure_value(structure, "width", 0)),
            height=int(structure_value(structure, "height", 0)),
        )

    def offer_detection(self, message):
        offer_latest(self.detection_queue, message)

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


def init_gst():
    os.environ.setdefault("GST_PLUGIN_PATH", str(default_plugin_path()))
    Gst.init(None)
