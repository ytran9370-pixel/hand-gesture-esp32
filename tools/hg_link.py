"""hg_link.py - Python side of the UART frame protocol v2 (same format as firmware/main/core/proto.h).

    A5 5A | TYPE | SEQ | LEN (u16 LE) | PAYLOAD | CRC-16/CCITT-FALSE (u16 LE, over TYPE..PAYLOAD)

Usage:
    from hg_link import GestureClient
    with GestureClient("COM5") as dev:          # Linux: "/dev/ttyUSB0", QEMU: "socket://localhost:5555"
        print(dev.ping())
        result = dev.infer(rgb_64x64x3_bytes)
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass, field

PROTO_VERSION = 2
SOF = b"\xA5\x5A"
IMAGE_BYTES = 64 * 64 * 3
MAX_PAYLOAD = IMAGE_BYTES + 64

PING, INFER, SELFTEST, STATS, PROFILE, GET_STATE, SET_OUTPUT = range(1, 8)
EVT_STATE_CHANGED = 0x41
ERROR = 0x7F

LABELS = ["call", "dislike", "fist", "four", "like", "mute", "ok", "one", "palm", "peace", "peace_inverted",
          "rock", "stop", "stop_inverted", "three", "three2", "two_up", "two_up_inverted"]
LOCK_NAMES = {0: "LOCKED", 1: "UNLOCKED", 2: "LOCKOUT"}
ACTION_NAMES = {0: "-", 1: "LED1 toggle", 2: "LED2 toggle", 3: "LED3 toggle", 4: "all lights ON",
                5: "all lights OFF", 6: "door OPEN", 7: "door CLOSE", 8: "LOCK"}
REASON_NAMES = {1: "card ok", 2: "card denied", 3: "gesture", 4: "auto-lock", 5: "PC command", 6: "lockout end"}
ERROR_NAMES = {1: "CRC", 2: "LEN", 3: "UNKNOWN_CMD", 4: "BUSY", 5: "TIMEOUT", 6: "INTERNAL", 7: "LOCKED"}
TARGETS = {"led1": 1, "led2": 2, "led3": 3, "door": 4}


def crc16(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode_frame(msg_type: int, seq: int, payload: bytes = b"") -> bytes:
    body = struct.pack("<BBH", msg_type, seq, len(payload)) + payload
    return SOF + body + struct.pack("<H", crc16(body))


@dataclass
class Frame:
    type: int
    seq: int
    payload: bytes


class FrameParser:
    """Byte-stream -> frames. Skips noise and frames with a bad CRC (they are counted)."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.crc_errors = 0

    def feed(self, data: bytes) -> list[Frame]:
        self.buf += data
        frames = []
        while True:
            start = self.buf.find(SOF)
            if start < 0:
                del self.buf[:-1]           # keep a trailing A5 that may start the next frame
                return frames
            del self.buf[:start]
            if len(self.buf) < 6:
                return frames
            msg_type, seq, length = struct.unpack_from("<BBH", self.buf, 2)
            if length > MAX_PAYLOAD:
                del self.buf[:2]            # impossible length: resynchronise
                continue
            total = 6 + length + 2
            if len(self.buf) < total:
                return frames
            body = bytes(self.buf[2:6 + length])
            (rx_crc,) = struct.unpack_from("<H", self.buf, 6 + length)
            if rx_crc == crc16(body):
                frames.append(Frame(msg_type, seq, body[4:]))
                del self.buf[:total]
            else:
                self.crc_errors += 1
                del self.buf[:2]


# ---------------------------------------------------------------- decoded payloads
@dataclass
class SysState:
    lock_state: int
    outputs: int
    last_gesture: int
    denied_count: int
    unlock_remaining_s: int

    @classmethod
    def decode(cls, p: bytes) -> "SysState":
        return cls(*struct.unpack_from("<BBBBH", p))

    def __str__(self) -> str:
        o = self.outputs
        leds = " ".join(f"L{i + 1}:{'ON' if o >> i & 1 else '--'}" for i in range(3))
        last = LABELS[self.last_gesture] if self.last_gesture < len(LABELS) else "-"
        timer = f" ({self.unlock_remaining_s}s)" if self.lock_state != 0 else ""
        return (f"{LOCK_NAMES.get(self.lock_state, '?')}{timer}  {leds}  door:{'OPEN' if o & 8 else 'CLOSED'}"
                f"  last:{last}")


@dataclass
class InferResult:
    class_id: int
    conf: float
    infer_us: int
    device_us: int
    action: int
    filter_count: int
    lock_state: int
    outputs: int
    logits: list[int] = field(repr=False)
    round_trip_ms: float = 0.0

    @property
    def label(self) -> str:
        return LABELS[self.class_id]

    @classmethod
    def decode(cls, p: bytes) -> "InferResult":
        cid, n, conf, infer_us, dev_us, action, count, lock, outputs = struct.unpack_from("<BBHIIBBBB", p)
        logits = list(struct.unpack_from(f"<{n}i", p, 16))
        return cls(cid, conf / 1000.0, infer_us, dev_us, action, count, lock, outputs, logits)


class DeviceError(Exception):
    def __init__(self, code: int, req_type: int):
        super().__init__(f"device error {ERROR_NAMES.get(code, code)} for request 0x{req_type:02X}")
        self.code = code


class LinkTimeout(Exception):
    pass


# ---------------------------------------------------------------- client
class GestureClient:
    """Request/response with sequence numbers, 1 s timeout and 2 retries (FR-COM-06).
    Events (EVT_STATE_CHANGED) that arrive at any time are passed to `on_event(state, reason)`."""

    def __init__(self, port: str, baud: int = 921600, timeout_s: float = 1.0, retries: int = 2, on_event=None):
        import serial  # pyserial; imported here so the protocol code above can be tested without it

        self.ser = serial.serial_for_url(port, baudrate=baud, timeout=0.02)
        self.timeout_s = timeout_s
        self.retries = retries
        self.on_event = on_event
        self.parser = FrameParser()
        self.pending: list[Frame] = []
        self.seq = 0
        self.stats = {"requests": 0, "retries": 0, "timeouts": 0}

    def close(self) -> None:
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _next_seq(self) -> int:
        self.seq = self.seq % 255 + 1       # 1..255, 0 is used by the device for unsolicited errors
        return self.seq

    def _read_frames(self, until: float) -> list[Frame]:
        while not self.pending and time.monotonic() < until:
            data = self.ser.read(4096)
            if data:
                self.pending += self.parser.feed(data)
        frames, self.pending = self.pending, []
        return frames

    def _dispatch_event(self, f: Frame) -> None:
        if f.type == EVT_STATE_CHANGED and self.on_event is not None:
            self.on_event(SysState.decode(f.payload), f.payload[6])

    def request(self, msg_type: int, payload: bytes = b"") -> Frame:
        self.stats["requests"] += 1
        for attempt in range(self.retries + 1):
            if attempt:
                self.stats["retries"] += 1
            seq = self._next_seq()
            self.ser.write(encode_frame(msg_type, seq, payload))
            deadline = time.monotonic() + self.timeout_s
            retry = False
            while not retry and time.monotonic() < deadline:
                for f in self._read_frames(deadline):
                    if 0x40 <= f.type <= 0x4F:
                        self._dispatch_event(f)
                    elif f.seq != seq:
                        continue                        # late answer to an earlier attempt
                    elif f.type == ERROR:
                        code, req = f.payload[0], f.payload[1]
                        if code in (1, 4):              # CRC or BUSY: send again
                            time.sleep(0.05)
                            retry = True
                            break
                        raise DeviceError(code, req)
                    elif f.type == msg_type | 0x80:
                        return f
            if not retry:
                self.stats["timeouts"] += 1
        raise LinkTimeout(f"no answer to request 0x{msg_type:02X} after {self.retries + 1} attempts")

    def poll(self, seconds: float) -> None:
        """Only listen for events (nothing is sent)."""
        for f in self._read_frames(time.monotonic() + seconds):
            self._dispatch_event(f)

    # ---- commands ----
    def ping(self) -> dict:
        p = self.request(PING).payload
        v, ncls, w, h, c, nl, mhash, macs, mhz = struct.unpack_from("<BBHHBBIII", p)
        info = {"proto_version": v, "n_classes": ncls, "input": f"{w}x{h}x{c}", "n_layers": nl,
                "model_hash": f"0x{mhash:08X}", "macs": macs, "cpu_mhz": mhz,
                "backend": p[20:28].split(b"\0")[0].decode(), "model": p[28:60].split(b"\0")[0].decode(),
                "firmware": p[60:76].split(b"\0")[0].decode()}
        if v != PROTO_VERSION:
            raise RuntimeError(f"device speaks protocol v{v}, this tool expects v{PROTO_VERSION}")
        return info

    def infer(self, rgb: bytes) -> InferResult:
        if len(rgb) != IMAGE_BYTES:
            raise ValueError(f"INFER needs {IMAGE_BYTES} bytes (64x64 RGB), got {len(rgb)}")
        t0 = time.perf_counter()
        r = InferResult.decode(self.request(INFER, bytes(rgb)).payload)
        r.round_trip_ms = (time.perf_counter() - t0) * 1000
        return r

    def selftest(self) -> dict:
        passed, total, _, avg, mn, mx = struct.unpack("<BBHIII", self.request(SELFTEST).payload)
        return {"passed": passed, "total": total, "avg_ms": avg / 1000, "min_ms": mn / 1000, "max_ms": mx / 1000}

    def stats_(self) -> dict:
        names = ["uptime_ms", "frames_ok", "infer", "crc_err", "len_err", "timeout", "heap_free", "heap_min",
                 "psram_free", "last_infer_us", "post_passed", "post_total"]
        return dict(zip(names, struct.unpack("<10IBBxx", self.request(STATS).payload)))

    def profile(self) -> list[tuple[str, int]]:
        p = self.request(PROFILE).payload
        return [(p[i:i + 12].split(b"\0")[0].decode(), struct.unpack_from("<I", p, i + 12)[0])
                for i in range(0, len(p), 16)]

    def get_state(self) -> SysState:
        return SysState.decode(self.request(GET_STATE).payload)

    def set_output(self, target: str, on: bool) -> SysState:
        return SysState.decode(self.request(SET_OUTPUT, bytes([TARGETS[target], int(on)])).payload)
