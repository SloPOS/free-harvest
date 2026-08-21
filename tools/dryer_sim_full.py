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
}
REAL_STAT = STATES["idle"]
REAL_UID = ("UID,3A916C02-11D48E77-00000000-00000000,1,6.4.0,1,0,0,0,0,0,0")
# The real serial number, read off the machine's data plate.
REAL_SNM = "SNM,P-STF 2311-03561 BKC,"

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
FD_NAME = "Freeze Dryer"
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
      registered, online      WIFIINFO 5 81 "Ourplace" 1 HR_3cdc... 0 1 37

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


STATE_KEYS = {"1": "idle", "2": "prep", "3": "freeze", "4": "dry", "5": "final"}

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
    ap.add_argument("--state", default="idle", choices=sorted(STATES),
                    help="which machine state to present; 'idle' is what makes "
                         "the app offer START")
    args = ap.parse_args()

    global REAL_STAT
    REAL_STAT = STATES[args.state]

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
    log("# PRESS 1=idle 2=prep 3=freeze 4=dry 5=final to change the screen the")
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
                if verb == "FDNAME":
                    for r in FDNAME_REPLY:
                        send(r, "answering FDNAME (as a file)")
                        time.sleep(0.3)
                elif verb == "FDFILES":
                    pass   # deliberately unanswered - see FDFILES_ANSWER
                else:
                    reply = REAL_STAT if verb == "REQSTAT" else ANSWERS.get(verb)
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
        if time.time() - last_stat >= args.stat_secs:
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
