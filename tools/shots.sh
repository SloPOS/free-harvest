#!/usr/bin/env bash
# Regenerate the README screenshots from the real web UI.
#
# tools/shotserver.py serves main/www/index.html against canned API responses,
# so these are photographs of the shipped interface rather than mock-ups - but
# without driving a real freeze dryer through every screen to get them.
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

PORT=${PORT:-8099}
python3 tools/shotserver.py --port "$PORT" >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 2

OUT=docs/img
mkdir -p "$OUT"
# Chrome resolves --screenshot against its own cwd, not the shell's.
ABS="$(cd "$OUT" && pwd)"
TMP=$(mktemp -d)

# name:scenario:width:height
SHOTS="
dashboard-idle:idle:480:1180
dashboard-controls:idle:480:1180
setup-candy:candy:480:1600
setup-custom:custom:480:1250
dashboard-drying:drying:480:1180
dashboard-running:drying:480:1320
dashboard-complete:complete:480:1180
"

for row in $SHOTS; do
    name="${row%%:*}"; rest="${row#*:}"
    scen="${rest%%:*}"; rest="${rest#*:}"
    w="${rest%%:*}"; h="${rest##*:}"
    echo "  $name  (scenario $scen, ${w}x${h})"
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars \
        --force-device-scale-factor=2 \
        --window-size="$w,$h" \
        --virtual-time-budget=4000 \
        --user-data-dir="$TMP/$name" \
        --screenshot="$ABS/$name.png" \
        "http://localhost:$PORT/?s=$scen" >/dev/null 2>&1
done

rm -rf "$TMP"
echo
ls -la "$OUT"/*.png | awk '{print "  " $9, $5 " bytes"}'
