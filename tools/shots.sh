#!/usr/bin/env bash
# Regenerate the README screenshots from the real web UI.
#
# tools/shotserver.py serves main/www/index.html against canned API responses,
# so these are photographs of the shipped interface rather than mock-ups - but
# without driving a real freeze dryer through every screen to get them. Several
# panels only exist on particular dryer screens (the Candy editor on STAT 43,
# Custom on 31, the end-of-cycle buttons on 7), and pressing buttons on
# someone's machine for documentation is a poor reason to press them.
#
#   bash tools/shots.sh
#
# Needs Chrome or Edge for headless capture. Writes into docs/img/.
set -u
cd "$(dirname "$0")/.."

CHROME=""
for c in "C:/Program Files/Google/Chrome/Application/chrome.exe" \
         "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe" \
         "$(command -v google-chrome 2>/dev/null)" \
         "$(command -v chromium 2>/dev/null)"; do
    [ -n "$c" ] && [ -x "$c" ] && CHROME="$c" && break
done
[ -z "$CHROME" ] && { echo "no Chrome or Edge found"; exit 1; }

# A random port each run.
#
# A fixed port silently binds to a LEFTOVER server from an earlier run - which
# happened, and produced light-mode screenshots that were byte-identical to the
# dark ones because the old process did not understand the theme parameter. The
# failure looks like a broken feature rather than a stale process.
PORT=${PORT:-$((8200 + RANDOM % 400))}
python3 tools/shotserver.py --port "$PORT" >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 2

# Confirm we are talking to OUR server, not something else on that port.
if ! curl -s "http://localhost:$PORT/api/state?s=idle" | grep -q '"version"'; then
    echo "shotserver did not come up on port $PORT"; exit 1
fi

OUT=docs/img
mkdir -p "$OUT"
# Chrome resolves --screenshot against its own cwd, not the shell's.
ABS="$(cd "$OUT" && pwd)"
TMP=$(mktemp -d)

# name:scenario:theme:width:height
#
# Mobile is 480 wide; desktop 1280 so the wide layout's media query applies and
# the settings modal renders as a modal rather than a separate view.
SHOTS="
dashboard-running:drying:dark:480:1320
dashboard-controls:idle:dark:480:1180
dashboard-drying:drying:dark:480:1180
dashboard-complete:complete:dark:480:1180
setup-candy:candy:dark:480:1600
setup-custom:custom:dark:480:1250
dashboard-light:drying:light:480:1320
setup-candy-light:candy:light:480:1600
desktop-dark:drying:dark:1280:900
desktop-light:drying:light:1280:900
"

for row in $SHOTS; do
    name="${row%%:*}"; rest="${row#*:}"
    scen="${rest%%:*}"; rest="${rest#*:}"
    theme="${rest%%:*}"; rest="${rest#*:}"
    w="${rest%%:*}"; h="${rest##*:}"
    echo "  $name  ($scen, $theme, ${w}x${h})"
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars \
        --force-device-scale-factor=2 \
        --window-size="$w,$h" \
        --virtual-time-budget=4000 \
        --user-data-dir="$TMP/$name" \
        --screenshot="$ABS/$name.png" \
        "http://localhost:$PORT/?s=$scen&theme=$theme" >/dev/null 2>&1
done

rm -rf "$TMP"
echo
ls -la "$OUT"/*.png | awk '{print "  " $9, $5 " bytes"}'
