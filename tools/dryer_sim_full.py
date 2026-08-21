#!/usr/bin/env python3
"""
Impersonate a freeze dryer convincingly enough for the HarvestRight APP.

WHY THIS EXISTS

The product claims the app can "start, stop, and mirror every screen function".
The dryer's serial command table has no START verb - it holds 48 verbs and none
of them start a cycle - so control cannot work the way we assumed. If the app
mirrors the SCREEN, the mechanism is generic: read what is displayed, then act
on it. The likely candidates are already in that table and were previously
dismissed:

    GETP / GETR   read page / row?      (screen contents)
    STATE         set state             (the stock adapter emits "STATE 1 0")
    ADV           advance               (next phase / next screen?)

Disassembly has now been wrong twice about this firmware, so guessing further is
a poor use of the remaining time. The reliable answer is to WATCH the stock
adapter issue a control command and read it off the wire.

HOW TO USE IT

  1. Plug the stock adapter's OTG port into this PC. It becomes a serial port;
     this script plays the dryer on the other end of it.
  2. Run this. It answers the adapter's polls so the adapter believes a real
     machine is attached and finishes its startup.
  3. Join the adapter's Wi-Fi AP (HR_<mac>) and provision it with the
     HarvestRight app, exactly as you would a real install.
  4. In the app, press a control - START, STOP, add dry time, change a setting.
  5. Read the transcript. Any frame the adapter sends that is NOT in the routine
     poll set is flagged as *** NEW *** - that is the command you want.

THIS IS TIME-CRITICAL if the adapter is going back. Nothing else we can do
recovers this information once the hardware is gone.

    python tools/dryer_sim_full.py COM3
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

# Verbs the adapter emits as routine housekeeping. Anything outside this set is
# worth stopping for - it is a response to something YOU did in the app.
ROUTINE = {"STATE", "UNIQUE", "FDNAME", "REQCFG", "STATUS", "WIFIINFO",
           "FDFILES"}

# Real frames, captured from the user's own machine. Using genuine values rather
# than invented ones matters: the app may well sanity-check what it is shown.
REAL_STAT = ("STAT,6,0,0,0,154,335,3581,0,68,CANDY,4,49,0,0,7,57,3619,0,,")
REAL_UID = ("UID,3A916C02-11D48E77-00000000-00000000,1,6.4.0,1,0,0,0,0,0,0")
REAL_SNM = "SNM,FD-2024-8871,"

# Answers to what the adapter asks for. CFG is CONFIRMED - replying with this
# made a real adapter stop asking REQCFG (3 requests in 45s -> 0).
# Dryer->adapter is COMMA-delimited; adapter->dryer is space-delimited.
ANSWERS = {
    "REQCFG":     "CFG,1,1,0,0,Auto,v6.4",
    "REQSTAT":    REAL_STAT,
    "REQSYSINF":  "SYSINF,HarvestRight Pro",
    "REQPREF":    "SYSPREF,0",
    "REQBATSUM":  "BATSUM,0,",
    "STATUS":     "STATUS,1,",
}

# FDNAME is a FILE request, not a frame request.
#
# The firmware holds the string "FDName.txt", and answering FDNAME with an
# inline frame did NOT satisfy the adapter - it kept asking (2-3 times per 40s).
# Sending the file instead did, and it has not asked since. The dryer ships
# files with FDFILELIST (announce) then FDFILEBLOCK (contents).
FD_NAME = "Freeze Dryer"
FDNAME_REPLY = [
    f"SNM,{FD_NAME},",
    f"FDFILELIST,FDName.txt,0,{len(FD_NAME)}",
    f"FDFILEBLOCK,FDName.txt,0,0,0,{FD_NAME}",
]

# The adapter answers a file transfer by asking for MORE files:
#     <- FDFILES .dat 1
# The dryer's batch history lives in HB/HS/HH.%05ld.dat, so it is asking for
# batch records. An empty list is a valid answer - we have no history to give -
# and lets it move on rather than stall waiting.
def fdfiles_reply(arg):
    ext = arg.strip() or ".dat"
    return [f"FDFILELIST,,0,0", f"FDEXT,0"]


class Log:
    def __init__(self, path):
        self.f = open(path, "w", encoding="utf-8")
        self.t0 = time.time()
        self.path = path
        self.new_verbs = []

    def __call__(self, s):
        line = f"{time.time() - self.t0:8.2f}  {s}"
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port, e.g. COM3")
    ap.add_argument("--minutes", type=float, default=30.0,
                    help="how long to keep playing the dryer")
    ap.add_argument("--stat-secs", type=float, default=15.0,
                    help="idle STAT cadence, matching the real machine")
    args = ap.parse_args()

    port = args.port
    if not port:
        cands = [p.device for p in list_ports.comports() if p.vid == 0x303A]
        if len(cands) != 1:
            for p in list_ports.comports():
                print(f"  {p.device}  {p.description}")
            sys.exit("Pass the port explicitly, e.g. tools/dryer_sim_full.py COM3")
        port = cands[0]

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    log = Log(f"dryer-sim-{stamp}.txt")
    log(f"# Playing the dryer on {port}. Adapter frames are space-delimited;")
    log("# ours are comma-delimited. Watch for *** NEW *** lines.")

    ser = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.4)
    ser.reset_input_buffer()

    def send(text, why=""):
        raw = text.encode("ascii") + CR
        log(f"-> {text}" + (f"    ({why})" if why else ""))
        try:
            ser.write(raw)
            ser.flush()
        except Exception as e:
            log(f"   (write failed: {e})")

    # Opening handshake, in the order a real dryer uses.
    send(REAL_UID, "identity")
    time.sleep(0.5)
    send(REAL_SNM, "serial number")
    time.sleep(0.5)
    send(REAL_STAT, "initial status")

    buf = b""
    last_stat = time.time()
    deadline = time.time() + args.minutes * 60

    while time.time() < deadline:
        try:
            chunk = ser.read(256)
        except Exception as e:
            log(f"   (port dropped: {type(e).__name__}; reopening)")
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(2)
            try:
                ser = serial.Serial(port, 115200, timeout=0.2)
                buf = b""
            except Exception:
                continue
            continue

        if chunk:
            buf += chunk
            while CR in buf:
                line, buf = buf.split(CR, 1)
                if not line:
                    continue
                text = line.decode("ascii", "replace")
                verb = text.split(" ")[0].strip()

                if verb in ROUTINE:
                    log(f"<- {text}")
                else:
                    # The whole point of the exercise.
                    log(f"<- {text}    *** NEW - not routine housekeeping ***")
                    if verb not in log.new_verbs:
                        log.new_verbs.append(verb)

                # Multi-frame answers first, then the simple table.
                if verb == "FDNAME":
                    for r in FDNAME_REPLY:
                        send(r, "answering FDNAME (as a file)")
                        time.sleep(0.3)
                elif verb == "FDFILES":
                    parts = text.split(" ", 1)
                    for r in fdfiles_reply(parts[1] if len(parts) > 1 else ""):
                        send(r, "answering FDFILES")
                        time.sleep(0.3)
                else:
                    reply = ANSWERS.get(verb)
                    if reply:
                        send(reply, f"answering {verb}")

        # Keep the telemetry flowing, or the adapter may decide we are gone.
        if time.time() - last_stat >= args.stat_secs:
            last_stat = time.time()
            send(REAL_STAT, "idle cadence")

    log("")
    log("=== summary ===")
    if log.new_verbs:
        log(f"  NON-ROUTINE verbs seen: {log.new_verbs}")
        log("  ^ these appeared outside the normal poll - most likely the")
        log("    control path. Send the transcript over.")
    else:
        log("  Nothing outside routine housekeeping. Either the app never")
        log("  reached the adapter, or control travels by another route")
        log("  (the mass-storage channel is the obvious candidate).")
    log(f"  Transcript: {log.path}")
    ser.close()


if __name__ == "__main__":
    sys.exit(main())
