#!/usr/bin/env python3
"""
Map the dryer's screens to STAT type codes, by walking the panel by hand.

WHY THIS AND NOT DISASSEMBLY

STAT's first field is the SCREEN ID. Control is screen-relative - ADV presses
whatever the current screen offers - so the control surface is exactly "which
screen am I on, and what does it offer". That correspondence can be read
directly off the machine in twenty minutes, with no firmware work and no risk.

Static analysis of this firmware has misled us twice: UNIQUE was called
handler-less and is live, and "no remote control exists" was wrong because the
mechanism was screen-relative rather than per-verb. So the sane division of
labour is: measure what can be measured, and reserve disassembly for the parts
that cannot.

HOW TO USE

  1. Start this. It watches the adapter and prints a line every time the screen
     ID changes.
  2. Walk the dryer's panel by hand - every menu, every option, every screen you
     can reach WITHOUT starting a cycle.
  3. Say out loud (or note) what is on screen as each new type appears.
  4. Ctrl-C. It prints the map.

Nothing is sent to the dryer. This only observes.

    python tools/map_screens.py
"""

import argparse
import datetime
import json
import sys
import time
import urllib.request

# Screens already identified, from cycle captures and the ADV probe.
KNOWN = {
    1:  "Idle / Ready",
    2:  "Load trays / Continue  (btn 1 continue, 2 end batch)",
    4:  "Freezing  (btn 3 advance, 4 end batch)",
    5:  "Drying  (btn 1 end batch)",
    6:  "Final dry  (btn 1 end, 2 more time, 3 less time)",
    7:  "Complete  (btn 1 defrost, 2 +2h dry, 3 no defrost, 5 warm trays)",
    15: "Diagnostics",
    17: "Preparing / pre-cool  (btn 3 advance, 4 end batch)",
    31: "CUSTOM recipe config  (values via SENDCUSTOM; buttons unmapped)",
    43: "Candy recipe config  (btn 18 cancel; values go via SENDCANDY)",
    44: "seen once inside final dry, unmapped",
}

# WANTED: button numbers for screens 31 and 43.
#
# Both recipe-configuration screens are identified now, but only Candy's Cancel
# (CLICK 43 18) has been captured. Everything else on them edits values through
# SENDCANDY / SENDCUSTOM rather than button presses, so the remaining question
# is which buttons exist at all - Save, Start, Reset, Cancel.
#
# Screen 44, seen once inside final dry, is still unidentified.


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.86.79")
    ap.add_argument("--poll", type=float, default=1.0)
    args = ap.parse_args()

    base = f"http://{args.host}"
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = f"screen-map-{stamp}.txt"
    out = open(path, "w", encoding="utf-8")

    def emit(s):
        print(s, flush=True)
        out.write(s + "\n")
        out.flush()

    emit("# Walk the panel. Every screen change is logged with its STAT type.")
    emit("# Note what is on screen as each new type appears. Ctrl-C to finish.")
    emit("")

    seen = {}
    last = None
    t0 = time.time()
    try:
        while True:
            try:
                d = json.load(urllib.request.urlopen(base + "/api/state", timeout=6))
            except Exception:
                time.sleep(args.poll)
                continue

            t = d.get("stat_type")

            # "type 0" is NOT a screen - it is our own null state, reported when
            # the adapter has no telemetry (link down, or the dryer rebooting).
            # A screen walk that logs it as an unknown screen sends you looking
            # for a panel that does not exist.
            if not d.get("have_tel") or d.get("link") != "up":
                if last is not None:
                    emit(f"{time.time()-t0:7.1f}s   (no telemetry - dryer link "
                         f"down or rebooting; not a screen)")
                    last = None
                time.sleep(args.poll)
                continue

            if t is not None and t != last:
                tag = KNOWN.get(t, "*** UNKNOWN - note what is on screen NOW ***")
                emit(f"{time.time()-t0:7.1f}s   type {t:<4} {tag}")
                emit(f"           phase={d.get('phase_label')} "
                     f"temp={d.get('temp_f')}F elapsed={d.get('elapsed_s')}s "
                     f"mode={d.get('mode')}")
                seen[t] = seen.get(t, 0) + 1
                last = t
            time.sleep(args.poll)
    except KeyboardInterrupt:
        pass

    emit("")
    emit("=== screen types observed ===")
    for t in sorted(seen):
        emit(f"  type {t:<4} seen {seen[t]}x   {KNOWN.get(t, 'UNKNOWN')}")
    unknown = [t for t in seen if t not in KNOWN]
    if unknown:
        emit("")
        emit(f"  NEW types this session: {unknown}")
        emit("  Pair each with the screen you were on - that is the map.")
    emit(f"\n  saved to {path}")
    out.close()


if __name__ == "__main__":
    sys.exit(main())
