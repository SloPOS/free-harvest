#!/usr/bin/env python3
"""
Interrogate a GENUINE HarvestRight Wi-Fi adapter from a PC.

No USB sniffer required. The dryer is a USB *host* and the adapter is a USB
*device*, so a PC can stand in for the dryer exactly: plug the official adapter
into this machine, and it enumerates as a serial port we can talk to.

WHAT THIS IS FOR

`decoded/PROTOCOL_NOTES.md` records one unknown that matters more than the rest:

    "Capture the exact GOTIT reply contents; that's the session gate."

Our own firmware answers REQINFO with a guessed payload (CONFIG_HR_ACK_PAYLOAD,
"ESP32ADAPTER"). Whether the dryer actually validates it is unknown. This tool
asks the real adapter what it sends, which settles it.

WHAT IT WILL NOT FIND

Remote cycle control. The dryer's own firmware was disassembled and its command
table contains no START / CONTINUE / DEFROST / CANCEL verb - the strings that
look promising (ADV, HCS, SPC, GETR, GETP, DUTY, UNIQUE) appear only in the
dispatcher's strcmp chain with no handler behind them. All flow control is
panel-only. Sniffing cannot reveal a command that does not exist; what it CAN
reveal is the handshake, the ack payload, and anything the official adapter
sends that we do not.

USAGE

    python tools/interrogate_adapter.py                 # auto-detect the port
    python tools/interrogate_adapter.py COM12
    python tools/interrogate_adapter.py COM12 --listen  # passive only

Everything is written to a timestamped transcript with byte-exact framing and
relative timings, so the session can be analysed afterwards.
"""

import argparse
import datetime
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing:  python -m pip install pyserial")

CR = b"\r"

# The dryer's discovery sequence, taken from the printf format strings in the
# decoded v6 firmware. Sent in the order the dryer itself uses.
#
# UID first (identity/discovery), then REQINFO - which is the frame that should
# provoke a GOTIT from the adapter. The STAT frames afterwards are ordinary
# idle-cadence telemetry, included because the adapter may only start behaving
# normally once it believes a real dryer is present.
DISCOVERY = [
    ("UID",     ["3A916C02-11D48E77-00000000-00000000", "1", "6.4.0",
                 "1", "0", "0", "0", "0", "0", "0"]),
    ("SNM",     ["FD-TEST-0001"]),
    ("REQINFO", []),
]

IDLE_STAT = ("STAT", ["1", "0", "0", "0", "68", "151697", "265", "0", "38",
                      "1", "1", "Auto", "v6.4", ""])


def build(verb, fields):
    return (",".join([verb] + [str(f) for f in fields])).encode("ascii") + CR


class Transcript:
    """Byte-exact log with relative timestamps, echoed to the console."""

    def __init__(self, path):
        self.f = open(path, "w", encoding="utf-8")
        self.t0 = time.time()
        self.path = path

    def note(self, text):
        line = f"{time.time() - self.t0:8.3f}  {text}"
        print(line)
        self.f.write(line + "\n")
        self.f.flush()

    def data(self, direction, raw):
        """direction: '->' sent to adapter, '<-' received from adapter."""
        printable = raw.decode("ascii", "replace").replace("\r", "<CR>")
        self.note(f"{direction} {printable}    raw={raw!r}")

    def close(self):
        self.f.close()


def describe_device(port, tr):
    """
    Record the adapter's USB identity.

    Worth capturing even if the protocol turns out to be identity-agnostic: if
    the dryer ever DOES validate who it is talking to, these are the values our
    firmware would have to present, and they are unobtainable once the hardware
    goes back.
    """
    for p in list_ports.comports():
        if p.device != port:
            continue
        tr.note("")
        tr.note("== USB identity ==")
        for label, val in [
            ("vid", f"{p.vid:#06x}" if p.vid else None),
            ("pid", f"{p.pid:#06x}" if p.pid else None),
            ("manufacturer", p.manufacturer),
            ("product", p.product),
            ("serial_number", p.serial_number),
            ("description", p.description),
            ("hwid", p.hwid),
            ("location", p.location),
            ("interface", p.interface),
        ]:
            tr.note(f"   {label:14} {val}")
        tr.note("")
        tr.note("   Ours for comparison: vid 0x303a pid 0x4001 "
                "(Espressif TinyUSB defaults)")
        return
    tr.note(f"   (no port metadata found for {port})")


def find_port(explicit):
    if explicit:
        return explicit
    cands = []
    for p in list_ports.comports():
        # Espressif VID covers both a native-USB S3 and most dev boards. Any
        # other CDC device is still worth offering - we do not know what the
        # official adapter presents until we see one.
        tag = "Espressif" if p.vid == 0x303A else ""
        cands.append((p.device, p.description, p.vid, p.pid, tag))
    if not cands:
        sys.exit("No serial ports found. Is the adapter plugged in?")
    print("Serial ports present:")
    for dev, desc, vid, pid, tag in cands:
        v = f"{vid:#06x}" if vid else "?"
        pi = f"{pid:#06x}" if pid else "?"
        print(f"  {dev:8}  {desc}   VID={v} PID={pi} {tag}")
    esp = [c for c in cands if c[4]]
    if len(esp) == 1:
        print(f"\nUsing {esp[0][0]} (only Espressif device present).")
        return esp[0][0]
    sys.exit("\nPass the port explicitly, e.g. "
             "python tools/interrogate_adapter.py COM12")


def drain(ser, tr, seconds):
    """Read for `seconds`, logging every complete CR-terminated frame."""
    end = time.time() + seconds
    buf = b""
    got = []
    while time.time() < end:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            while CR in buf:
                line, buf = buf.split(CR, 1)
                if line:
                    tr.data("<-", line + CR)
                    got.append(line.decode("ascii", "replace"))
    if buf:
        tr.note(f"   (partial, no CR yet: {buf!r})")
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port, e.g. COM12")
    ap.add_argument("--baud", type=int, default=115200,
                    help="nominal only; CDC ignores it")
    ap.add_argument("--listen", action="store_true",
                    help="passive: log what the adapter says unprompted, send nothing")
    ap.add_argument("--listen-secs", type=float, default=60.0)
    args = ap.parse_args()

    port = find_port(args.port)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    tr = Transcript(f"adapter-transcript-{stamp}.txt")
    tr.note(f"# HarvestRight adapter interrogation, port {port}")
    tr.note("# Frames are ASCII, comma-delimited, CR-terminated.")
    describe_device(port, tr)

    try:
        with serial.Serial(port, args.baud, timeout=0.2) as ser:
            time.sleep(0.4)
            ser.reset_input_buffer()

            # ---------------------------------------------------------------
            # Phase 1 - passive. Does the adapter speak first? Our own firmware
            # never does; if the official one announces itself, that alone is a
            # finding.
            # ---------------------------------------------------------------
            tr.note("")
            tr.note(f"== Phase 1: listening {args.listen_secs:.0f}s, sending nothing ==")
            unprompted = drain(ser, tr, args.listen_secs if args.listen else 15.0)
            tr.note(f"   {len(unprompted)} unprompted frame(s)")
            if args.listen:
                tr.note("\nListen-only mode; done.")
                return 0

            # ---------------------------------------------------------------
            # Phase 2 - discovery. REQINFO is the one that should draw a GOTIT.
            # ---------------------------------------------------------------
            tr.note("")
            tr.note("== Phase 2: dryer discovery sequence ==")
            replies = []
            for verb, fields in DISCOVERY:
                frame = build(verb, fields)
                tr.data("->", frame)
                ser.write(frame)
                ser.flush()
                replies += drain(ser, tr, 3.0)

            # ---------------------------------------------------------------
            # Phase 3 - idle telemetry. Some adapters only settle once they
            # believe a real machine is on the other end.
            # ---------------------------------------------------------------
            tr.note("")
            tr.note("== Phase 3: idle STAT cadence (3 frames, 15s apart) ==")
            for _ in range(3):
                frame = build(*IDLE_STAT)
                tr.data("->", frame)
                ser.write(frame)
                ser.flush()
                replies += drain(ser, tr, 15.0)

            # ---------------------------------------------------------------
            # Findings
            # ---------------------------------------------------------------
            tr.note("")
            tr.note("== Summary ==")
            gotit = [r for r in replies if r.startswith("GOTIT")]
            if gotit:
                tr.note(f"   GOTIT payload: {gotit[0]}")
                tr.note("   ^ THIS is the session gate PROTOCOL_NOTES flagged as")
                tr.note("     unverified. Compare against CONFIG_HR_ACK_PAYLOAD.")
            else:
                tr.note("   No GOTIT seen. Either the adapter needs something we")
                tr.note("   did not send, or it does not answer REQINFO the way")
                tr.note("   our firmware assumes.")
            verbs = sorted({r.split(",")[0] for r in replies if r})
            tr.note(f"   Verbs the adapter sent: {verbs or 'none'}")
            tr.note(f"\n   Transcript: {tr.path}")
    finally:
        tr.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
