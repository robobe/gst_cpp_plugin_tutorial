#!/usr/bin/env python3

import argparse
import time
import tkinter as tk
from tkinter import ttk

import zmq

from bridge_project.messages import MessageError, decode_message, encode_message


class BridgeGui:
    def __init__(self, args):
        self.args = args
        self.context = zmq.Context.instance()
        self.last_message_time = 0.0

        self.command_socket = self.context.socket(zmq.PUB)
        self.command_socket.setsockopt(zmq.LINGER, 0)
        self.command_socket.connect(args.command_endpoint)

        self.telemetry_socket = self.context.socket(zmq.SUB)
        self.telemetry_socket.setsockopt(zmq.LINGER, 0)
        self.telemetry_socket.setsockopt(zmq.SUBSCRIBE, b"")
        self.telemetry_socket.connect(args.telemetry_endpoint)

        self.root = tk.Tk()
        self.root.title("Bridge Detector Control")
        self.root.protocol("WM_DELETE_WINDOW", self.close)

        self.enabled = tk.BooleanVar(value=True)
        self.low_h = tk.IntVar(value=0)
        self.low_s = tk.IntVar(value=100)
        self.low_v = tk.IntVar(value=100)
        self.high_h = tk.IntVar(value=10)
        self.high_s = tk.IntVar(value=255)
        self.high_v = tk.IntVar(value=255)
        self.status = tk.StringVar(value="waiting for telemetry")
        self.result = tk.StringVar(value="found=false x=0 y=0 width=0 height=0")

        self.build_widgets()

    def build_widgets(self):
        main = ttk.Frame(self.root, padding=12)
        main.grid(row=0, column=0, sticky="nsew")
        self.root.columnconfigure(0, weight=1)
        self.root.rowconfigure(0, weight=1)

        enabled = ttk.Checkbutton(
            main,
            text="Detection enabled",
            variable=self.enabled,
            command=self.send_enabled,
        )
        enabled.grid(row=0, column=0, columnspan=2, sticky="w")

        controls = (
            ("low-h", self.low_h, 0, 179),
            ("low-s", self.low_s, 0, 255),
            ("low-v", self.low_v, 0, 255),
            ("high-h", self.high_h, 0, 179),
            ("high-s", self.high_s, 0, 255),
            ("high-v", self.high_v, 0, 255),
        )

        for row, (label, variable, minimum, maximum) in enumerate(controls, start=1):
            ttk.Label(main, text=label).grid(row=row, column=0, sticky="w", pady=2)
            spinbox = ttk.Spinbox(
                main,
                from_=minimum,
                to=maximum,
                textvariable=variable,
                width=8,
            )
            spinbox.grid(row=row, column=1, sticky="ew", pady=2)

        apply_button = ttk.Button(
            main,
            text="Apply HSV",
            command=self.send_hsv,
        )
        apply_button.grid(row=7, column=0, columnspan=2, sticky="ew", pady=(8, 4))

        ttk.Label(main, textvariable=self.result).grid(
            row=8,
            column=0,
            columnspan=2,
            sticky="w",
            pady=(8, 2),
        )

        ttk.Label(main, textvariable=self.status).grid(
            row=9,
            column=0,
            columnspan=2,
            sticky="w",
        )

        main.columnconfigure(1, weight=1)

    def send_enabled(self):
        self.send_command(
            {
                "type": "set_detection_enabled",
                "enabled": self.enabled.get(),
            }
        )

    def send_hsv(self):
        self.send_command(
            {
                "type": "set_hsv_thresholds",
                "low_h": self.low_h.get(),
                "low_s": self.low_s.get(),
                "low_v": self.low_v.get(),
                "high_h": self.high_h.get(),
                "high_s": self.high_s.get(),
                "high_v": self.high_v.get(),
            }
        )

    def send_command(self, command):
        self.command_socket.send(encode_message(command), flags=zmq.NOBLOCK)

    def poll_telemetry(self):
        while True:
            try:
                message = decode_message(
                    self.telemetry_socket.recv(flags=zmq.NOBLOCK)
                )
            except zmq.Again:
                break
            except MessageError as exc:
                self.status.set(f"invalid telemetry: {exc}")
                break

            if message.get("type") != "detection":
                continue

            self.last_message_time = time.monotonic()
            self.result.set(
                "frame={frame} found={found} x={x} y={y} "
                "width={width} height={height}".format(**message)
            )

        if time.monotonic() - self.last_message_time < 1.0:
            self.status.set("bridge publishing")
        else:
            self.status.set("waiting for telemetry")

        self.root.after(50, self.poll_telemetry)

    def run(self):
        self.root.after(50, self.poll_telemetry)
        self.root.mainloop()

    def close(self):
        self.command_socket.close(0)
        self.telemetry_socket.close(0)
        self.root.destroy()


def parse_args():
    parser = argparse.ArgumentParser(
        description="Control the bridge detector through ZMQ."
    )
    parser.add_argument("--command-endpoint", default="tcp://127.0.0.1:5555")
    parser.add_argument("--telemetry-endpoint", default="tcp://127.0.0.1:5556")
    return parser.parse_args()


def main():
    BridgeGui(parse_args()).run()


if __name__ == "__main__":
    main()
