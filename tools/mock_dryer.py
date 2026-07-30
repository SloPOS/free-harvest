#!/usr/bin/env python3
"""
Mock HarvestRight freeze dryer.

Plays the dryer's side of the conversation over a serial port so the ESP32-S3
adapter firmware can be exercised with no HarvestRight hardware present.

Typical use once the S3 is flashed and plugged into your PC's USB:

    python mock_dryer.py --port COM7            # run the scripted session
    python mock_dryer.py --port COM7 --listen   # passive: just log traffic
    python mock_dryer.py --selftest             # no hardware needed

Frame format (from the decoded v6 firmware): ASCII, comma-delimited fields,
terminated by CR. Empty fields are meaningful.
"""

import argparse
import sys
import time

CR = b"\r"


# --------------------------------------------------------------------------
# Frame codec
# --------------------------------------------------------------------------
def build(verb, *fields):
    """Build a CR-terminated frame. Fields may be str or int; '' is allowed."""
    parts = [verb] + [str(f) for f in fields]
    return (",".join(parts)).encode("ascii") + CR


def parse(line):
    """Parse a frame body (no CR) into (verb, [fields])."""
    if isinstance(line, bytes):
        line = line.decode("ascii", "replace")
    line = line.rstrip("\r\n")
    if not line:
        return None, []
    parts = line.split(",")
    return parts[0], parts[1:]


class Reassembler:
    """Splits an arbitrary byte stream into CR-terminated frames."""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data):
        out = []
        for b in data:
            if b in (0x0D, 0x0A):
                if self._buf:
                    out.append(bytes(self._buf))
                    self._buf.clear()
            else:
                self._buf.append(b)
        return out


# --------------------------------------------------------------------------
# Scripted dryer behaviour
# --------------------------------------------------------------------------
# NOTE: field values below are plausible placeholders, NOT captured from a real
# machine. Replace them once you have a USB capture of the genuine adapter.
SCRIPT = [
    ("REQINFO", []),
    ("UID", ["1A2B3C4D-5E6F-7788-99AA", 3, "6.0.641041", 1, 0, 0, 0, 0, 0, 0, ""]),
    ("SNM", ["FD1234567", ""]),
    ("SYSINF", ["MODEL=HR-MED;HW=6", ""]),
]

STAT_TEMPLATE = ["1", "DRY", 20, -32, 1200, 86400, 3, "a", "b", ""]


def run_session(ser, period, cycles):
    rx = Reassembler()
    seen = {}

    def pump(seconds):
        end = time.time() + seconds
        while time.time() < end:
            data = ser.read(256)
            if data:
                for raw in rx.feed(data):
                    verb, fields = parse(raw)
                    seen[verb] = seen.get(verb, 0) + 1
                    print(f"  <- {raw.decode('ascii', 'replace')}")
            else:
                time.sleep(0.01)

    print("-- scripted dryer session --")
    for verb, fields in SCRIPT:
        frame = build(verb, *fields)
        print(f"  -> {frame[:-1].decode('ascii')}")
        ser.write(frame)
        ser.flush()
        pump(1.0)

    print(f"-- streaming STAT every {period}s ({cycles} cycles) --")
    for i in range(cycles):
        stat = list(STAT_TEMPLATE)
        stat[3] = -32 + i  # vary a field so changes are visible
        frame = build("STAT", *stat)
        print(f"  -> {frame[:-1].decode('ascii')}")
        ser.write(frame)
        ser.flush()
        pump(period)

    print("\n-- frames received from adapter --")
    if not seen:
        print("  (none - adapter sent nothing)")
    for verb, n in sorted(seen.items()):
        print(f"  {verb}: {n}")
    return 0 if seen.get("GOTIT") else 1


def run_listen(ser):
    print("-- passive listen (Ctrl-C to stop) --")
    rx = Reassembler()
    try:
        while True:
            data = ser.read(256)
            if data:
                for raw in rx.feed(data):
                    ts = time.strftime("%H:%M:%S")
                    print(f"[{ts}] {raw.decode('ascii', 'replace')}")
            else:
                time.sleep(0.01)
    except KeyboardInterrupt:
        return 0


# --------------------------------------------------------------------------
# Self-test (no hardware)
# --------------------------------------------------------------------------
def selftest():
    failures = 0

    def check(cond, label):
        nonlocal failures
        if not cond:
            failures += 1
            print(f"  FAIL {label}")

    check(build("REQSTAT") == b"REQSTAT\r", "verb-only frame")
    check(build("STAT", 1, "abc", 42) == b"STAT,1,abc,42\r", "mixed fields")
    check(build("BATSUM", 3, "", "") == b"BATSUM,3,,\r", "trailing empties")

    verb, fields = parse("STAT,1,DRY,20\r")
    check(verb == "STAT", "parse verb")
    check(fields == ["1", "DRY", "20"], "parse fields")

    verb, fields = parse("BATSUM,3,,\r")
    check(fields == ["3", "", ""], "parse preserves empty fields")

    r = Reassembler()
    check(r.feed(b"STA") == [], "partial frame withheld")
    check(r.feed(b"T,1\rBEEP\r") == [b"STAT,1", b"BEEP"], "reassembly")
    check(r.feed(b"\r\r") == [], "empty frames ignored")

    # round trip
    verb, fields = parse(build("GOTIT", "SN1", 7)[:-1])
    check(verb == "GOTIT" and fields == ["SN1", "7"], "round trip")

    print(f"selftest: {'PASSED' if failures == 0 else 'FAILED'} "
          f"({failures} failures)")
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port, e.g. COM7 or /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200,
                    help="nominal only; ignored over USB CDC")
    ap.add_argument("--period", type=float, default=2.0,
                    help="seconds between STAT frames")
    ap.add_argument("--cycles", type=int, default=5,
                    help="number of STAT frames to send")
    ap.add_argument("--listen", action="store_true",
                    help="passive mode: log traffic only")
    ap.add_argument("--selftest", action="store_true",
                    help="exercise the codec with no hardware")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.port:
        ap.error("--port is required (or use --selftest)")

    try:
        import serial  # pyserial
    except ImportError:
        print("pyserial not installed. Run: pip install pyserial", file=sys.stderr)
        return 2

    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        print(f"opened {args.port}")
        if args.listen:
            return run_listen(ser)
        return run_session(ser, args.period, args.cycles)


if __name__ == "__main__":
    sys.exit(main())
