#!/usr/bin/env python3

import argparse
import logging
import queue
import threading

from bridge_project.pipeline import BridgePipeline, init_gst
from bridge_project.transport import ZmqBridgeTransport


LOGGER = logging.getLogger(__name__)


def publish_loop(detection_queue, transport, stop_event):
    while not stop_event.is_set():
        try:
            message = detection_queue.get(timeout=0.1)
            LOGGER.info(message)
        except queue.Empty:
            continue

        try:
            transport.publish(message)
        except Exception as exc:
            LOGGER.warning("failed to publish detection message: %s", exc)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run the bridge pipeline with ZMQ command and telemetry sockets."
    )
    parser.add_argument("--command-endpoint", default="tcp://127.0.0.1:5555")
    parser.add_argument("--telemetry-endpoint", default="tcp://127.0.0.1:5556")
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
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s: %(message)s",
    )
    args = parse_args()

    init_gst()

    detection_queue = queue.Queue(maxsize=1)
    command_queue = queue.Queue(maxsize=10)
    stop_event = threading.Event()
    pipeline = BridgePipeline(args, detection_queue, command_queue)
    transport = ZmqBridgeTransport(
        command_endpoint=args.command_endpoint,
        telemetry_endpoint=args.telemetry_endpoint,
    )

    publisher_thread = threading.Thread(
        target=publish_loop,
        args=(detection_queue, transport, stop_event),
        name="bridge-telemetry-publisher",
        daemon=True,
    )

    try:
        transport.start(command_queue)
        publisher_thread.start()
        pipeline.run()
    finally:
        stop_event.set()
        transport.stop()
        publisher_thread.join(timeout=1.0)


if __name__ == "__main__":
    main()
