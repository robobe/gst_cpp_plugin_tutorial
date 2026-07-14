from dataclasses import dataclass

import msgpack


class MessageError(ValueError):
    pass


@dataclass(frozen=True)
class DetectionMessage:
    frame: int
    timestamp_ns: int
    found: bool
    x: int
    y: int
    width: int
    height: int

    def to_dict(self):
        return {
            "type": "detection",
            "frame": self.frame,
            "timestamp_ns": self.timestamp_ns,
            "found": self.found,
            "x": self.x,
            "y": self.y,
            "width": self.width,
            "height": self.height,
        }


@dataclass(frozen=True)
class SetDetectionEnabledCommand:
    enabled: bool


@dataclass(frozen=True)
class SetHsvThresholdsCommand:
    low_h: int
    low_s: int
    low_v: int
    high_h: int
    high_s: int
    high_v: int


def encode_message(message):
    if hasattr(message, "to_dict"):
        payload = message.to_dict()
    else:
        payload = message

    return msgpack.packb(payload, use_bin_type=True)


def decode_message(payload):
    try:
        data = msgpack.unpackb(payload, raw=False)
    except Exception as exc:
        raise MessageError(f"invalid MessagePack payload: {exc}") from exc

    if not isinstance(data, dict):
        raise MessageError("message payload must be a map")

    return data


def decode_command(payload):
    data = decode_message(payload)

    command_type = data.get("type")

    if command_type == "set_detection_enabled":
        enabled = data.get("enabled")
        if not isinstance(enabled, bool):
            raise MessageError("enabled must be a boolean")
        return SetDetectionEnabledCommand(enabled=enabled)

    if command_type == "set_hsv_thresholds":
        values = {
            name: _read_int(data, name)
            for name in (
                "low_h",
                "low_s",
                "low_v",
                "high_h",
                "high_s",
                "high_v",
            )
        }
        _validate_hsv(values)
        return SetHsvThresholdsCommand(**values)

    raise MessageError(f"unknown command type: {command_type}")


def decode_detection_message(payload):
    return parse_detection_message(decode_message(payload))


def parse_detection_message(data):
    message_type = data.get("type")
    if message_type != "detection":
        raise MessageError(f"unexpected message type: {message_type}")

    return DetectionMessage(
        frame=_read_int(data, "frame"),
        timestamp_ns=_read_int(data, "timestamp_ns"),
        found=_read_bool(data, "found"),
        x=_read_int(data, "x"),
        y=_read_int(data, "y"),
        width=_read_int(data, "width"),
        height=_read_int(data, "height"),
    )


def _read_bool(data, name):
    value = data.get(name)
    if not isinstance(value, bool):
        raise MessageError(f"{name} must be a boolean")
    return value


def _read_int(data, name):
    value = data.get(name)
    if not isinstance(value, int) or isinstance(value, bool):
        raise MessageError(f"{name} must be an integer")
    return value


def _validate_hsv(values):
    for name in ("low_h", "high_h"):
        if values[name] < 0 or values[name] > 179:
            raise MessageError(f"{name} must be between 0 and 179")

    for name in ("low_s", "low_v", "high_s", "high_v"):
        if values[name] < 0 or values[name] > 255:
            raise MessageError(f"{name} must be between 0 and 255")
