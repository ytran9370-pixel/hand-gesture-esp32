"""hg_cli.py - command-line tool to test the board without a webcam.

    python hg_cli.py COM5 ping                 device and model information
    python hg_cli.py COM5 selftest             golden images on the chip: bit-exact? how fast?
    python hg_cli.py COM5 stats | profile      counters, free RAM / time of each layer
    python hg_cli.py COM5 state                lock state, lights, door
    python hg_cli.py COM5 set led2 on          manual output control (only when UNLOCKED)
    python hg_cli.py COM5 infer hand.jpg       classify one image file (needs opencv-python)
    python hg_cli.py COM5 bench 100            send 100 random images: round-trip time, errors
    python hg_cli.py COM5 monitor              print every state change (tap the RFID card...)
"""
import argparse
import os
import statistics
import sys

from hg_link import ACTION_NAMES, IMAGE_BYTES, REASON_NAMES, DeviceError, GestureClient, LinkTimeout


def load_image(path: str) -> bytes:
    import cv2  # only needed for this command

    img = cv2.imread(path)
    if img is None:
        sys.exit(f"cannot read {path}")
    img = cv2.resize(img, (64, 64), interpolation=cv2.INTER_AREA)
    return cv2.cvtColor(img, cv2.COLOR_BGR2RGB).tobytes()   # OpenCV is BGR, the model wants RGB


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="COM5, /dev/ttyUSB0 or socket://localhost:5555")
    ap.add_argument("cmd", choices=["ping", "selftest", "stats", "profile", "state", "set", "infer", "bench",
                                    "monitor"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--baud", type=int, default=921600)
    a = ap.parse_args()

    def on_event(state, reason):
        print(f"  [event] {REASON_NAMES.get(reason, reason):12s} -> {state}")

    with GestureClient(a.port, a.baud, on_event=on_event) as dev:
        if a.cmd == "ping":
            for k, v in dev.ping().items():
                print(f"  {k:14s} {v}")
        elif a.cmd == "selftest":
            r = dev.selftest()
            print(f"  {r['passed']}/{r['total']} golden images bit-exact, {r['avg_ms']:.2f} ms per image "
                  f"(min {r['min_ms']:.2f}, max {r['max_ms']:.2f})")
        elif a.cmd == "stats":
            for k, v in dev.stats_().items():
                print(f"  {k:14s} {v}")
        elif a.cmd == "profile":
            layers = dev.profile()
            total = sum(us for _, us in layers) or 1
            for name, us in layers:
                print(f"  {name:12s} {us / 1000:8.2f} ms  {100 * us / total:5.1f} %")
        elif a.cmd == "state":
            print(" ", dev.get_state())
        elif a.cmd == "set":
            if len(a.args) != 2 or a.args[0] not in ("led1", "led2", "led3", "door") or a.args[1] not in ("on", "off"):
                sys.exit("usage: set led1|led2|led3|door on|off")
            print(" ", dev.set_output(a.args[0], a.args[1] == "on"))
        elif a.cmd == "infer":
            r = dev.infer(load_image(a.args[0]))
            print(f"  {r.label} {r.conf:.1%}  infer {r.infer_us / 1000:.1f} ms  round trip {r.round_trip_ms:.1f} ms"
                  f"  action: {ACTION_NAMES[r.action]}")
        elif a.cmd == "bench":
            n = int(a.args[0]) if a.args else 100
            rtt, inf = [], []
            for i in range(n):
                r = dev.infer(os.urandom(IMAGE_BYTES))
                rtt.append(r.round_trip_ms)
                inf.append(r.infer_us / 1000)
                print(f"\r  {i + 1}/{n}", end="", flush=True)
            print(f"\n  inference  : mean {statistics.mean(inf):.2f} ms, max {max(inf):.2f} ms")
            print(f"  round trip : mean {statistics.mean(rtt):.1f} ms, max {max(rtt):.1f} ms "
                  f"({1000 / statistics.mean(rtt):.1f} images/s)")
            print(f"  link       : {dev.stats}, CRC errors seen by PC: {dev.parser.crc_errors}")
            s = dev.stats_()
            print(f"  device     : crc_err={s['crc_err']} timeout={s['timeout']} heap_min={s['heap_min']} bytes")
        elif a.cmd == "monitor":
            print("  listening for events, Ctrl+C to stop")
            print(" ", dev.get_state())
            try:
                while True:
                    dev.poll(0.5)
            except KeyboardInterrupt:
                pass


if __name__ == "__main__":
    try:
        main()
    except DeviceError as e:
        sys.exit(f"  {e}  (LOCKED = tap an authorized card first)")
    except LinkTimeout as e:
        sys.exit(f"  {e}  - check the COM port, the cable (UART/COM connector) and that the board is running")
