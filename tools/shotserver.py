#!/usr/bin/env python3
"""
Serve the real web UI against canned API responses, for screenshots and for
looking at states that are awkward to reach on a live machine.

WHY THIS EXISTS

Several panels only appear on particular dryer screens - the Candy editor needs
STAT type 43, the Custom editor type 31, the end-of-cycle buttons type 7. Making
a real freeze dryer visit each of those to take a picture means driving someone's
machine around for documentation, which is a poor reason to press buttons on it.

This serves main/www/index.html unmodified. Only the API responses are faked, so
what you photograph is genuinely the shipped UI rather than a mock-up of it.

    python tools/shotserver.py            # http://localhost:8099/?s=idle
    python tools/shotserver.py --port 9000

Scenarios are chosen with ?s=<name>; see SCENARIOS below.
"""

import argparse
import http.server
import json
import os
import re
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
WWW = os.path.join(HERE, "..", "main", "www", "index.html")

BASE = {
    "link": "up", "serial": "Freezie McDry", "uid": "0-33303337-33353541-33374339",
    "frames_in": 4213, "frames_out": 38, "unknown_verbs": 0, "frames_bad": 0,
    "latest_seq": 4213, "wifi": "connected", "ip": "192.168.1.42",
    "ssid": "Ourplace", "phase": 1, "phase_label": "Ready", "have_tel": True,
    "temp_f": 69, "pressure": 151637, "elapsed_s": 0, "prep_s": 0,
    "mode": "Auto", "stat_type": 1, "freeze_pct": 0, "freeze_eta_s": -1,
    "phase_pct": 0, "phase_s": 0, "vacuum_um": 0, "vacuum_ok": False,
    "usb_mounted": True, "usb_suspended": False, "usb_mounts": 1,
    "usb_rx_bytes": 184320, "uptime_s": 74210, "reset_reason": "poweron",
    "control": True, "actions": [], "last_stat": "", "version": "1.0.0",
}

READY_ACTIONS = [
    {"name": "candy_setup",  "label": "Candy Setup…",  "sev": 1},
    {"name": "custom_setup", "label": "Custom Setup…", "sev": 1},
    {"name": "start_auto",   "label": "Start Auto",         "sev": 1},
]

SCENARIOS = {
    "idle": dict(BASE, actions=READY_ACTIONS,
                 last_stat="STAT,1,0,0,0,69,151637,0,0,38,1,1,Auto,v6.4,,"),

    "candy": dict(BASE, stat_type=43, phase_label="Recipe settings",
                  actions=[],
                  last_stat="STAT,43,0,0,0,69,151701,119,0,40,1023,70,140,"
                            "150,160,5,120,0,CANDY,,"),

    "custom": dict(BASE, stat_type=31, phase_label="Recipe settings",
                   actions=[],
                   last_stat="STAT,31,0,0,0,69,158913,33,0,28,36095,16082,"
                             "-15,120,125,120,500,900,100,CUSTOM,90,14400,,"),

    "drying": dict(BASE, stat_type=5, phase=8, phase_label="Drying",
                   temp_f=41, pressure=452, elapsed_s=65400, vacuum_um=452,
                   vacuum_ok=True, phase_pct=61, phase_s=18300, mode="Auto",
                   actions=[{"name": "dry_end", "label": "End Batch", "sev": 2}],
                   last_stat="STAT,5,0,0,0,41,452,18300,0,46,Auto,1,34,0,0,5,0,0,0,"),

    "complete": dict(BASE, stat_type=7, phase=10, phase_label="Complete",
                     temp_f=69, elapsed_s=93600,
                     actions=[
                         {"name": "done_defrost",    "label": "Defrost",             "sev": 1},
                         {"name": "done_more_dry",   "label": "More Dry Time (+2h)", "sev": 0},
                         {"name": "done_no_defrost", "label": "Finish, No Defrost",  "sev": 1},
                         {"name": "done_warm_trays", "label": "Warm Trays",          "sev": 1}],
                     last_stat="STAT,7,0,0,0,69,151638,5,0,48,296,11,0,90,Auto,,"),
}

RECIPES = {
    "extra_dry_s": 7200,
    "slots": [
        {"slot": 0, "family": 4, "name": "Strawberries",
         "notes": "Sliced 6mm. Needed two extra hours in August - humid.",
         "runs": 4, "nnum": 8, "suggest_dry_s": 14400,
         "num": [4, 70, 140, 150, 160, 300, 7200, 300]},
        {"slot": 1, "family": 5, "name": "Beef mince",
         "notes": "Pre-cooked and drained. -30F freeze holds better.",
         "runs": 2, "nnum": 10, "suggest_dry_s": 0,
         "num": [5, -30, 10800, 125, 7200, 500, 1, 100, 54000, 0]},
    ],
}


def trend_for(scen):
    """
    A plausible run, for the chart to have something to draw.

    Shaped like a real one rather than a sine wave: a fast plunge while
    freezing, a long slow climb through drying as the shelves warm, and raw
    readings that jitter a degree or two around the smoothed line - which is
    the whole reason the smoothing exists.
    """
    if scen not in ("drying", "complete"):
        return {"n": 0, "bucket_s": 30, "stride": 1,
                "smooth": [], "temp": [], "press": []}

    import math, random
    random.seed(7)
    n = 150
    smooth, temp, press = [], [], []
    for i in range(n):
        f = i / (n - 1.0)
        if f < 0.18:                       # freezing: steep fall
            t = 68 - (68 + 34) * (f / 0.18)
        else:                              # drying: long exponential climb
            t = -34 + 78 * (1 - math.exp(-3.1 * (f - 0.18)))
        smooth.append(int(round(t * 100)))
        temp.append(int(round(t + random.uniform(-1.4, 1.4))))
        press.append(int(round(400 + 900 * math.exp(-4.0 * f)))
                     if f > 0.2 else None)
    return {"n": n, "bucket_s": 30, "stride": 4,
            "smooth": smooth, "temp": temp, "press": press}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        scen = q.get("s", ["idle"])[0]
        theme = q.get("theme", [""])[0]

        if u.path in ("/", "/index.html"):
            html = open(WWW, encoding="utf-8").read()
            # The page reads its own URL for nothing, so the scenario has to be
            # threaded through to the API calls it makes. Simplest reliable way:
            # rewrite the fetch paths to carry the query string.
            html = html.replace('"/api/', '"/api/'.replace("/api/", "/api/"))
            html = re.sub(r'fetch\("(/api/[a-z/]+)"',
                          lambda m: 'fetch("%s?s=%s"' % (m.group(1), scen), html)
            html = re.sub(r'urlopen\("(/api/[a-z/]+)"',
                          lambda m: 'urlopen("%s?s=%s"' % (m.group(1), scen), html)
            # Force a theme for the screenshot. The app reads fh-theme from
            # localStorage on load, so seeding it before the page script runs
            # is enough - no clicking, and the page is otherwise untouched.
            if theme in ("light", "dark"):
                inject = ("<script>try{localStorage.setItem('fh-theme','%s');"
                          "document.documentElement.setAttribute("
                          "'data-theme','%s');}catch(e){}</script>"
                          % (theme, theme))
                html = html.replace("<script", inject + "<script", 1)
            return self._send(html, "text/html; charset=utf-8")

        if u.path == "/api/state":
            return self._send(json.dumps(SCENARIOS.get(scen, SCENARIOS["idle"])))
        if u.path == "/api/recipes":
            return self._send(json.dumps(RECIPES))
        if u.path == "/api/trend":
            return self._send(json.dumps(trend_for(scen)))
        if u.path == "/api/mqtt":
            return self._send(json.dumps({"host": "", "port": 1883,
                                          "connected": False}))
        if u.path == "/api/log":
            return self._send(json.dumps([]))
        if u.path.startswith("/api/"):
            return self._send("{}")
        self.send_error(404)

    def do_POST(self):
        self._send('{"ok":true}')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8099)
    a = ap.parse_args()
    print("scenarios: " + ", ".join(sorted(SCENARIOS)))
    print("http://localhost:%d/?s=idle" % a.port)
    http.server.HTTPServer(("127.0.0.1", a.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
