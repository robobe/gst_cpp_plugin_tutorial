import logging
import threading

import zmq

from .messages import MessageError, decode_command, encode_message


LOGGER = logging.getLogger(__name__)


class ZmqBridgeTransport:
    def __init__(self, command_endpoint, telemetry_endpoint):
        self.command_endpoint = command_endpoint
        self.telemetry_endpoint = telemetry_endpoint
        self.context = zmq.Context.instance()
        self.command_socket = None
        self.telemetry_socket = None
        self.command_thread = None
        self.running = threading.Event()

    def start(self, command_handler):
        self.telemetry_socket = self.context.socket(zmq.PUB)
        self.telemetry_socket.setsockopt(zmq.LINGER, 0)
        self.telemetry_socket.setsockopt(zmq.SNDHWM, 1)
        self.telemetry_socket.bind(self.telemetry_endpoint)

        self.command_socket = self.context.socket(zmq.SUB)
        self.command_socket.setsockopt(zmq.LINGER, 0)
        self.command_socket.setsockopt(zmq.RCVHWM, 10)
        self.command_socket.setsockopt(zmq.SUBSCRIBE, b"")
        self.command_socket.bind(self.command_endpoint)

        self.running.set()
        self.command_thread = threading.Thread(
            target=self._command_loop,
            args=(command_handler,),
            name="bridge-command-subscriber",
            daemon=True,
        )
        self.command_thread.start()

    def publish(self, message):
        if self.telemetry_socket is None:
            return

        self.telemetry_socket.send(
            encode_message(message),
            flags=zmq.NOBLOCK,
        )

    def stop(self):
        self.running.clear()

        if self.command_socket is not None:
            self.command_socket.close(0)
            self.command_socket = None

        if self.telemetry_socket is not None:
            self.telemetry_socket.close(0)
            self.telemetry_socket = None

        if self.command_thread is not None:
            self.command_thread.join(timeout=1.0)
            self.command_thread = None

    def _command_loop(self, command_handler):
        poller = zmq.Poller()
        poller.register(self.command_socket, zmq.POLLIN)

        while self.running.is_set():
            try:
                events = dict(poller.poll(100))
            except zmq.ZMQError:
                return

            if self.command_socket not in events:
                continue

            try:
                payload = self.command_socket.recv(flags=zmq.NOBLOCK)
                command = decode_command(payload)
            except zmq.Again:
                continue
            except MessageError as exc:
                LOGGER.warning("ignoring command: %s", exc)
                continue
            except zmq.ZMQError:
                return

            command_handler(command)
