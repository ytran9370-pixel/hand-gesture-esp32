"""hg_cam.py - webcam demo: crops the hand from a fixed box, sends it to the ESP32-S3, shows the result.

    python hg_cam.py COM5                  (Windows PowerShell: the webcam is not visible inside WSL)
    python hg_cam.py COM5 --camera 1       another camera

Hold your hand inside the green box. Keys: q = quit, + / - = bigger / smaller box.
The model was trained on hand crops, so the hand should fill most of the box, on a plain background.
"""
import argparse
import time

import cv2

from hg_link import ACTION_NAMES, LOCK_NAMES, REASON_NAMES, GestureClient

GREEN, RED, WHITE, YELLOW = (0, 200, 0), (0, 0, 255), (255, 255, 255), (0, 220, 255)


def crop_box(frame, size):
    """Square box in the centre of the frame."""
    h, w = frame.shape[:2]
    size = min(size, h, w)
    x0, y0 = (w - size) // 2, (h - size) // 2
    return x0, y0, size


def to_model_input(frame, x0, y0, size) -> bytes:
    crop = frame[y0:y0 + size, x0:x0 + size]
    crop = cv2.resize(crop, (64, 64), interpolation=cv2.INTER_AREA)
    return cv2.cvtColor(crop, cv2.COLOR_BGR2RGB).tobytes()   # model input: RGB, 64x64, HWC


def put(img, text, y, color=WHITE, scale=0.6):
    cv2.putText(img, text, (10, y), cv2.FONT_HERSHEY_SIMPLEX, scale, (0, 0, 0), 4, cv2.LINE_AA)
    cv2.putText(img, text, (10, y), cv2.FONT_HERSHEY_SIMPLEX, scale, color, 1, cv2.LINE_AA)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="COM5 or /dev/ttyUSB0")
    ap.add_argument("--camera", type=int, default=0)
    ap.add_argument("--box", type=int, default=280, help="crop box size in pixels")
    a = ap.parse_args()

    last_event = {"text": "", "until": 0.0}

    def on_event(state, reason):
        last_event["text"] = f"{REASON_NAMES.get(reason, reason)} -> {LOCK_NAMES.get(state.lock_state)}"
        last_event["until"] = time.monotonic() + 3

    cam = cv2.VideoCapture(a.camera)
    if not cam.isOpened():
        raise SystemExit(f"cannot open camera {a.camera}")

    with GestureClient(a.port, on_event=on_event) as dev:
        info = dev.ping()
        print(f"connected: firmware {info['firmware']}, model {info['model']}")
        box, fps, last_action = a.box, 0.0, ("", 0.0)
        while True:
            ok, frame = cam.read()
            if not ok:
                break
            x0, y0, size = crop_box(frame, box)
            t0 = time.perf_counter()
            r = dev.infer(to_model_input(frame, x0, y0, size))
            fps = 0.9 * fps + 0.1 * (1.0 / max(time.perf_counter() - t0, 1e-3))
            if r.action:
                last_action = (ACTION_NAMES[r.action], time.monotonic() + 2)

            # draw on a mirrored copy so moving the hand feels natural
            view = cv2.flip(frame, 1)
            h, w = view.shape[:2]
            confident = r.conf >= 0.8
            cv2.rectangle(view, (w - x0 - size, y0), (w - x0, y0 + size), GREEN if confident else RED, 2)
            put(view, f"{r.label}  {r.conf:.0%}  [{'#' * r.filter_count}{'.' * (3 - r.filter_count)}]", 30,
                GREEN if confident else WHITE, 0.8)
            o = r.outputs
            leds = " ".join(f"L{i + 1}:{'ON' if o >> i & 1 else '--'}" for i in range(3))
            put(view, f"{LOCK_NAMES.get(r.lock_state)}   {leds}   door:{'OPEN' if o & 8 else 'CLOSED'}", 60)
            put(view, f"infer {r.infer_us / 1000:.1f} ms   round trip {r.round_trip_ms:.0f} ms   {fps:.1f} FPS",
                h - 15, WHITE, 0.5)
            if time.monotonic() < last_action[1]:
                put(view, f"ACTION: {last_action[0]}", 95, YELLOW, 0.8)
            if time.monotonic() < last_event["until"]:
                put(view, last_event["text"], 125, YELLOW)

            cv2.imshow("Hand Gesture ESP32-S3", view)
            key = cv2.waitKey(1) & 0xFF
            if key == ord("q"):
                break
            if key in (ord("+"), ord("=")):
                box = min(box + 20, min(h, w))
            if key == ord("-"):
                box = max(box - 20, 120)

    cam.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
