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
# The state we present decides what the APP offers. Presenting a mid-cycle
# machine gets you end-of-cycle buttons; to capture a START sequence the app has
# to believe the dryer is idle and ready.
#
# All of these are real frames captured from the user's own machine.
STATES = {
    "idle":   "STAT,1,0,0,0,69,151637,0,0,38,1,1,Auto,v6.4,,",
    "prep":   "STAT,17,0,0,0,64,10000,36,0,42,Auto,1,1,0,0,5,0,864,0,",
    "freeze": "STAT,4,0,0,0,13,10000,3676,0,60,Auto,1,83,0,0,5,0,0,0,",
    "dry":    "STAT,5,0,0,0,41,452,18300,0,46,Auto,1,34,0,0,5,0,0,0,",
    "final":  "STAT,6,0,0,0,154,335,3581,0,68,CANDY,4,49,0,0,7,57,3619,0,,",

    # Screens 2, 7 and 43, captured 2026-08-21. All three are reachable in
    # normal use and none has had its buttons mapped yet.
    #
    #   starting - the "Starting batch" screen between Preparing and Freezing.
    #              It carries a CONTINUE button that must be pressed for the
    #              batch to proceed, which is why a remote start currently
    #              stalls here.
    #   complete - end of cycle / venting.
    #   candycfg - the recipe configuration screen that Candy and Custom open.
    #              The trailing numbers are recipe parameters: a capture showed
    #              160 change to 0 while values were being edited on screen.
    "starting": "STAT,2,0,0,0,68,10000,10,0,43,Auto,1,1,0,0,5,2,120,0,0,,",
    "complete": "STAT,7,0,0,0,69,151638,5,0,48,296,11,0,90,Auto,,",
    "candycfg": "STAT,43,0,0,0,69,151701,119,0,40,1023,70,140,150,160,5,120,0,CANDY,,",

    # Custom recipe configuration - screen 31, CAPTURED 2026-08-21.
    #
    # An earlier guess put Custom on screen 43 with the recipe name swapped.
    # That was wrong: Candy and Custom are separate screens, and Custom offers
    # three settings against Candy's eight.
    #
    # Field map, derived from two frames taken either side of editing the
    # panel - only three values moved, and they are exactly the three settings
    # the screen offers:
    #
    #     ...,28,36095,16082,-15,120,125,120,500,900,100,CUSTOM,90,14400,,
    #     ...,28,36087,16082,-10,  0,120,120,500,900,100,CUSTOM,90,14400,,
    #                             ^^^ ^^^ ^^^
    #             initial freeze temp F  |   |
    #             extra freeze time, MINUTES |
    #                          drying temp F
    "customcfg": ("STAT,31,0,0,0,69,158913,33,0,28,36095,16082,"
                  "-15,120,125,120,500,900,100,CUSTOM,90,14400,,"),
}
REAL_STAT = STATES["idle"]
# IDENTITY
#
# The adapter forwards the dryer's identity to HarvestRight's cloud, which binds
# adapter to machine 1:1. Present the wrong identity and the app offers to
# REGISTER the unit and then refuses, because the "cpu code" does not match
# their record - which is what happens with the placeholder below.
#
# The UID frame's format, from the firmware:
#
#     UID,%lX-%lX-%lX-%lX,%d,%d.%d.%ld,%d,%d,%d,%d,%d,%d,%d,
#          \________ 128-bit MCU unique ID ________/
#
# %lX drops leading zeros, so a word of 0 prints as a bare "0" - which is why a
# 12-hex-digit cpu code like "9ABC 5678 1234" can correspond to a four-word UID
# whose last word is zero. --cpu builds the frame that way.
#
# THIS DEFAULT IS A PLACEHOLDER AND IS KNOWN WRONG for this user's machine: the
# server reported the real cpu code as 9ABC 5678 1234 while this UID was being
# presented. Pass --uid (preferred, measured) or --cpu (derived) to correct it.
# MEASURED 2026-08-21 from a USB capture of the real dryer answering the
# official adapter. Not inferred - these are the bytes on the wire.
#
# The unique-ID words are ASCII TEXT, not numbers: 31323334 is "1234",
# 35363738 is "5678", 39414243 is "9ABC". Read in reverse word order that
# spells 9ABC 5678 1234 - exactly the cpu code the app displays. So the
# app shows the ID as characters while the wire carries their hex codes.
# PLACEHOLDER, and deliberately not anyone's real machine.
#
# The words are ASCII codes - 31323334 is "1234", 35363738 is "5678",
# 39414243 is "9ABC" - so the encoding this demonstrates is intact while
# the identity is not real. Reversed, they spell the cpu code 9ABC 5678
# 1234 that the app would display.
#
# The app will refuse commands against this and offer to register the
# unit. Pass --uid (read from your own dryer via /api/state) or --cpu
# (the code the app shows) to present your actual machine.
PLACEHOLDER_UID = "0-31323334-35363738-39414243"

# The two fields that differ between the machines we have seen:
#
#     ...,4,6.0.641041,...   a dryer that works with Free Harvest
#     ...,5,6.0.644170,...   a dryer that answers UNIQUE and then nothing
#
# The integer before the version is its own field in the firmware's format
# string, separate from the version triple. What it means is NOT established -
# "hardware level" is a guess from it being 4 on one machine and 5 on another.
# It is exposed here so the stock adapter can be asked whether it CARES,
# which is a question only the adapter can answer.
HW_LEVEL = 4
FW_VERSION = "6.0.641041"

# Set by --stuck. Reproduces a dryer that answers UNIQUE and then goes
# quiet, so the stock adapter's reaction can be recorded.
STUCK = False


def uid_frame(words):
    """Build a UID frame around `words`, honouring --hw and --fw."""
    return "UID,%s,%d,%s,0,5,2,1,51,204,255," % (words, HW_LEVEL, FW_VERSION)


REAL_UID = uid_frame(PLACEHOLDER_UID)

# SNM is a NAME, not a serial number. The real machine answers with its
# user-set name; we had been sending the data-plate serial, which is a
# different thing entirely and is not what the adapter expects.
REAL_SNM = "SNM,My Freeze Dryer,"

# Answers to what the adapter asks for. CFG is CONFIRMED - replying with this
# made a real adapter stop asking REQCFG (3 requests in 45s -> 0).
# Dryer->adapter is COMMA-delimited; adapter->dryer is space-delimited.
# VERIFIED handshake, in the order the adapter drives it:
#   STATUS   -> STATUS,1,
#   FDNAME   -> SNM + FDFILELIST + FDFILEBLOCK   (as a FILE, not a frame)
#   REQCFG   -> CFG,1,1,0,0,Auto,v6.4
#   FDFILES  -> ignore
# After that the adapter stops asking for anything and just heartbeats
# STATE/UNIQUE every ~15s, which is what a satisfied adapter looks like.
ANSWERS = {
    "REQCFG":     "CFG,1,1,0,0,Auto,v6.4",
    # REQSTAT is resolved at send time, not here - see reply_for().
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
# FDName.txt holds the machine NAME, not the serial number. Two independent
# confirmations: the real dryer answered FDNAME with "SNM,My Freeze Dryer,"
# in the USB capture, and the app renames the machine by sending
#     FDRENAME "My Freeze Dryer"
# and immediately re-reading FDNAME. We had been serving the data-plate
# serial here, which is a different thing entirely.
FD_NAME = "My Freeze Dryer"
FDNAME_REPLY = [
    f"SNM,{FD_NAME},",
    f"FDFILELIST,FDName.txt,0,{len(FD_NAME)}",
    f"FDFILEBLOCK,FDName.txt,0,0,0,{FD_NAME}",
]

# DO NOT answer FDFILES.
#
# Answering it wrongly is what produced an infinite loop: the adapter rejected
# each reply and re-asked roughly once a second. Left alone it asks exactly ONCE
# and moves on - verified over 187s of silence afterwards. An empty file list is
# apparently not expressible, but not answering is fine.
FDFILES_ANSWER = []


# WIFIINFO field map, worked out by watching fields change against known state.
#
#   WIFIINFO <f1> <f2> <ssid> <f4> <ap-name> <f6> <f7> <uptime>
#
# f6 flipped 0 -> 1 the moment FDNAME and REQCFG were both satisfied, and stayed
# there: that is the dryer-connected flag, and the thing the LED reflects.
# f2/ssid stay 0/"" until the adapter is provisioned to a network.
# The last field counts up ~1/sec and resets on reboot - seconds since boot.
def decode_wifiinfo(text):
    """
    WIFIINFO <link> <rssi> <ssid> <registered> <ap> <?> <cloud> <uptime>

    Read off three captures of the SAME adapter at different stages:

      unregistered, no wifi   WIFIINFO 1 0  ""         0 HR_3cdc... 0 0 161
      registered, no wifi     WIFIINFO 2 0  ""         1 HR_3cdc... 0 0 7
      registered, online      WIFIINFO 5 81 "MyNetwork" 1 HR_3cdc... 0 1 37

    f1 climbs 1->2->5 as association progresses; f2 is signal strength, zero
    until associated; f4 went 0->1 across registering the unit in the app and
    stayed there; f7 went 0->1 only once traffic reached the cloud; f8 counts
    seconds since boot.

    f6 is NOT the dryer-connected flag, whatever an earlier note here said - it
    read 0 throughout a session in which the adapter was demonstrably talking to
    a dryer. It has never been observed non-zero, so it stays unlabelled.
    """
    p = text.split(" ")
    if len(p) < 9:
        return None
    return {
        "link": p[1],
        "rssi": p[2],
        "ssid": p[3],
        "registered": p[4],
        "ap_name": p[5],
        "f6": p[6],
        "cloud": p[7],
        "uptime_s": p[8],
    }


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


STATE_KEYS = {"1": "idle", "2": "prep", "3": "freeze", "4": "dry",
              "5": "final", "6": "starting", "7": "complete",
              "8": "candycfg", "9": "customcfg"}

try:
    import msvcrt

    def poll_key():
        """Non-blocking single keypress, or None. Windows console."""
        if msvcrt.kbhit():
            try:
                return msvcrt.getch().decode("ascii", "ignore")
            except Exception:
                return None
        return None
except ImportError:
    def poll_key():
        return None



# ---------------------------------------------------------------------------
# Scripted batch, for testing the adapter's logbook without running the dryer.
#
# A real freeze-drying run takes about a day. This walks the same phase
# sequence in a couple of minutes so the batch tracker sees a genuine start,
# a full set of phases, and a proper ending - which is the only way to exercise
# the record-writing path without committing someone's machine to a 24-hour
# cycle for a test.
#
# Field positions match the decoder: STAT,<type>,_,_,_,<tempF>,<pressure>,
# <elapsed_s>,... so temperature, vacuum and elapsed all land where the adapter
# expects them.
# ---------------------------------------------------------------------------

# phase, seconds to spend, elapsed at start, elapsed at end,
# temp at start, temp at end, pressure at start, pressure at end
BATCH_SCRIPT = [
    ("prep",     17, 12,     0,   900,  69,  64, 10000, 10000),
    ("starting",  2,  8,   900,  1200,  64,  40, 10000, 10000),
    ("freezing",  4, 20,  1200,  7200,  40, -34, 10000, 10000),
    ("drying",    5, 30,  7200, 40000, -34,  20,  1400,   288),
    ("finaldry",  6, 20, 40000, 80000,  20,  41,   288,   400),
    ("complete",  7, 10, 80000, 93600,  41,  69,   400,   400),
]


def stat_frame(kind, elapsed, temp, press):
    """Build a STAT frame for a phase, keeping each type's real tail."""
    tails = {
        17: "0,42,Auto,1,1,0,0,5,0,900,0,",
        2:  "0,43,Auto,1,1,0,0,5,2,120,0,0,,",
        4:  "0,45,Auto,1,1,0,0,5,0,0,,",
        5:  "0,46,Auto,1,34,0,0,5,0,0,0,",
        6:  "0,68,CANDY,4,49,0,0,7,57,3619,0,,",
        7:  "0,48,296,11,0,90,Auto,,",
        1:  "0,38,1,1,Auto,v6.4,,",
    }
    return "STAT,%d,0,0,0,%d,%d,%d,%s" % (
        kind, int(temp), int(press), int(elapsed), tails[kind])


def run_batch(send, log, poll, stat_secs=2.0):
    """
    Walk a whole batch. `send` posts a frame, `poll` services inbound traffic
    for a given number of seconds so the adapter's requests still get answers
    while the script runs.
    """
    log("")
    log("### SCRIPTED BATCH: walking a full run in about two minutes")
    log("")

    # Idle first, with the previous batch's elapsed still showing. This is the
    # case that must NOT be mistaken for a running batch.
    for _ in range(3):
        send(stat_frame(1, 93600, 69, 151637), "idle (stale elapsed)")
        poll(stat_secs)

    names = {"prep": 17, "starting": 2, "freezing": 4,
             "drying": 5, "finaldry": 6, "complete": 7}
    for label, kind, secs, e0, e1, t0, t1, p0, p1 in BATCH_SCRIPT:
        steps = max(2, int(secs / stat_secs))
        log("  -> %s (screen %d)" % (label, kind))
        for i in range(steps):
            f = i / float(steps - 1)
            send(stat_frame(kind,
                            e0 + (e1 - e0) * f,
                            t0 + (t1 - t0) * f,
                            p0 + (p1 - p0) * f), label)
            poll(stat_secs)

    # Back to idle: the run is over and the dryer keeps the final elapsed.
    for _ in range(3):
        send(stat_frame(1, 93600, 69, 151637), "idle (batch finished)")
        poll(stat_secs)

    log("")
    log("### BATCH COMPLETE - check /api/batches on the adapter")
    log("")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port, e.g. COM3")
    ap.add_argument("--minutes", type=float, default=30.0,
                    help="how long to keep playing the dryer")
    ap.add_argument("--stat-secs", type=float, default=15.0,
                    help="idle STAT cadence, matching the real machine")
    ap.add_argument("--reqinfo-secs", type=float, default=30.0,
                    help="how often to poll REQINFO, as the real dryer does")
    ap.add_argument("--uid", default=None,
                    help="the dryer's real 128-bit MCU unique ID as it appears "
                         "in its own UID frame, e.g. 9ABC-5678-1234-0. Read it "
                         "off the machine with our adapter (/api/state 'uid') "
                         "rather than guessing.")
    ap.add_argument("--cpu", default=None,
                    help="the cpu code as the APP displays it, e.g. "
                         "'9ABC 5678 1234'. Converted into a UID frame by "
                         "treating each group as one word and zero-filling the "
                         "rest. A DERIVED value - --uid is the measured one.")
    ap.add_argument("--serial", default=None,
                    help="override the serial number sent in SNM")
    ap.add_argument("--stuck", action="store_true",
                    help="reproduce the failing dryer: answer UNIQUE with UID, "
                         "poll REQINFO, and answer NOTHING else - no SNM, no "
                         "CFG, no telemetry. Shows what the STOCK adapter does "
                         "when a machine goes quiet after the identity query, "
                         "which is the behaviour Free Harvest has to match.")
    ap.add_argument("--hw", type=int, default=None,
                    help="the integer before the version in the UID frame. 4 on "
                         "a dryer that works, 5 on one that does not. Meaning "
                         "unestablished - vary it to ask the STOCK adapter "
                         "whether it behaves differently.")
    ap.add_argument("--fw", default=None,
                    help="the firmware version in the UID frame, e.g. "
                         "6.0.644170. Default 6.0.641041, the build captured "
                         "from a working machine.")
    ap.add_argument("--run-batch", action="store_true",
                    help="walk a whole freeze-drying run in about two minutes, "
                         "to exercise the adapter's batch logbook without "
                         "committing a real machine to a 24-hour cycle")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the identity frames that WOULD be sent, then "
                         "exit without opening the serial port")
    ap.add_argument("--stat", default=None,
                    help="present an ARBITRARY STAT frame instead of a named "
                         "state, e.g. --stat 'STAT,31,0,0,0,69,151637,0,0,38,"
                         "1,1,Auto,v6.4,,'. For probing screens we have not "
                         "captured, without editing this file.")
    ap.add_argument("--state", default="idle", choices=sorted(STATES),
                    help="which machine state to present; 'idle' is what makes "
                         "the app offer START")
    args = ap.parse_args()

    global REAL_STAT, REAL_UID, REAL_SNM, HW_LEVEL, FW_VERSION, STUCK
    STUCK = args.stuck
    if args.hw is not None:
        HW_LEVEL = args.hw
    if args.fw:
        FW_VERSION = args.fw
    REAL_UID = uid_frame(PLACEHOLDER_UID)
    REAL_STAT = STATES[args.state]
    if args.stat:
        if not args.stat.startswith("STAT,"):
            sys.exit("--stat must be a whole frame beginning with STAT,")
        REAL_STAT = args.stat

    if args.uid and args.cpu:
        sys.exit("pass --uid or --cpu, not both")
    if args.cpu:
        words = args.cpu.replace("-", " ").split()
        if not words or len(words) > 4:
            sys.exit("--cpu wants up to 4 hex groups, e.g. '9ABC 5678 1234'")
        for w in words:
            int(w, 16)                      # fail loudly on non-hex
        # The wire carries the ASCII CODES of the displayed characters, in
        # reverse word order, behind a leading zero word. Verified against a
        # real capture: "9ABC 5678 1234" -> 0-31323334-35363738-39414243.
        enc = ["".join("%02X" % ord(c) for c in w) for w in reversed(words)]
        REAL_UID = uid_frame("0-" + "-".join(enc))
    elif args.uid:
        REAL_UID = uid_frame(args.uid)
    if args.serial:
        REAL_SNM = "SNM,%s," % args.serial

    if PLACEHOLDER_UID in REAL_UID:
        print("!! Presenting a PLACEHOLDER identity - not a real machine.",
              file=sys.stderr)
        print("!! The app will refuse commands. Pass --uid or --cpu.",
              file=sys.stderr)

    if args.dry_run:
        print("identity frames this run would present:")
        print("  " + REAL_UID)
        print("  " + REAL_SNM)
        print("  " + REAL_STAT)
        return 0

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
    log("# ours are comma-delimited. Watch for >>> NON-ROUTINE <<< lines.")
    log(f"# Presenting the '{args.state}' screen.")
    log("#")
    log("# PRESS 1=idle 2=prep 3=freeze 4=dry 5=final 6=starting")
    log("#       7=complete 8=candycfg 9=customcfg  to change the")
    log("# app sees, then press buttons in the app. Any frame the adapter emits")
    log("# in response is the control command we are after.")

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
    last_reqinfo = 0.0
    last_state = None          # watch for STATE's fields changing
    last_wifi = None           # watch the dryer-connected / provisioning flags
    if args.run_batch:
        # Service inbound traffic between scripted frames, so the adapter's own
        # requests are still seen and logged while the run plays out.
        def poll(secs):
            nonlocal buf
            end = time.time() + secs
            while time.time() < end:
                try:
                    chunk = ser.read(128)
                except Exception:
                    return
                if chunk:
                    buf += chunk
                    while CR in buf:
                        line, buf = buf.split(CR, 1)
                        t = line.decode("ascii", "replace").strip()
                        if t:
                            log("<- " + t)
                            verb = t.split(" ")[0].split(",")[0]
                            reply = ANSWERS.get(verb)
                            if verb == "UNIQUE":
                                reply = REAL_UID
                            elif verb == "REQSTAT":
                                reply = REAL_STAT
                            if reply:
                                send(reply, "answering " + verb)
                time.sleep(0.05)

        run_batch(send, log, poll, stat_secs=1.5)
        log("transcript: " + log.path)
        return 0

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

                # Anything outside the known housekeeping set is a candidate
                # control command - that is the entire point of this session.
                if verb not in ROUTINE:
                    log("")
                    log(f"  >>> {text}")
                    log(f"  >>> NON-ROUTINE - likely a control command <<<")
                    log("")

                if verb == "WIFIINFO":
                    d = decode_wifiinfo(text)
                    if d and d != last_wifi:
                        log(f"<- {text}")
                        log(f"   link={d['link']} rssi={d['rssi']} "
                            f"ssid={d['ssid']} registered={d['registered']} "
                            f"cloud={d['cloud']}")
                        if last_wifi and d["cloud"] != last_wifi["cloud"]:
                            log(f"   *** CLOUD {last_wifi['cloud']} -> "
                                f"{d['cloud']} - app can now reach this unit ***")
                        if last_wifi and d["registered"] != last_wifi["registered"]:
                            log(f"   *** REGISTERED {last_wifi['registered']} -> "
                                f"{d['registered']} ***")
                        if last_wifi and d["ssid"] != last_wifi["ssid"]:
                            log(f"   *** SSID NOW {d['ssid']!r} - provisioned ***")
                        last_wifi = d
                elif verb == "STATE":
                    if text != last_state:
                        if last_state is not None:
                            log(f"<- {text}    *** STATE CHANGED (was "
                                f"{last_state!r}) ***")
                        else:
                            log(f"<- {text}")
                        last_state = text
                elif verb in ROUTINE:
                    log(f"<- {text}")
                else:
                    # The whole point of the exercise.
                    log(f"<- {text}    *** NEW - not routine housekeeping ***")
                    if verb not in log.new_verbs:
                        log.new_verbs.append(verb)

                # Multi-frame answers first, then the simple table.
                if STUCK and verb != "UNIQUE":
                    # The failing dryer answers the identity query and nothing
                    # else. Staying silent here is the whole experiment: what
                    # the stock adapter does NEXT is what we need to copy.
                    log(f"   (stuck mode: deliberately not answering {verb})")
                elif verb == "FDNAME":
                    for r in FDNAME_REPLY:
                        send(r, "answering FDNAME (as a file)")
                        time.sleep(0.3)
                elif verb == "FDFILES":
                    pass   # deliberately unanswered - see FDFILES_ANSWER
                else:
                    if verb == "REQSTAT":
                        reply = REAL_STAT
                    elif verb == "UNIQUE":
                        # THE identity query. Captured: adapter sends a bare
                        # UNIQUE, dryer answers with its UID frame.
                        reply = REAL_UID
                    else:
                        reply = ANSWERS.get(verb)
                    if reply:
                        send(reply, f"answering {verb}")

        # Switch the presented screen live. The app mirrors whatever screen the
        # machine reports, so stepping through states is how you find out which
        # command each button emits - press 1..5 here, then look at the app.
        key = poll_key()
        if key and key in STATE_KEYS:
            name = STATE_KEYS[key]
            REAL_STAT = STATES[name]
            log("")
            log(f"  ### now presenting '{name}' -> {REAL_STAT}")
            log(f"  ### check the app; its buttons should change to match")
            log("")
            send(REAL_STAT, f"state switched to {name}")
            last_stat = time.time()

        # Keep the telemetry flowing, or the adapter may decide we are gone.
        if not STUCK and time.time() - last_stat >= args.stat_secs:
            last_stat = time.time()
            send(REAL_STAT, "idle cadence")

        # The genuine dryer polls REQINFO - it appears in captures taken from
        # the live machine. We were never sending it, so the adapter was never
        # asked to identify itself the way a real install would ask.
        if time.time() - last_reqinfo >= args.reqinfo_secs:
            last_reqinfo = time.time()
            send("REQINFO,", "identity poll, as the real dryer does")

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
