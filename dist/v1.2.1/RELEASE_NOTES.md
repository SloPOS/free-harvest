# Free Harvest v1.2.1

**A lapsed IP address no longer strands the adapter, and temperatures can be
shown in °C.**

## Recovering from a lost DHCP lease

Your router lends the adapter its IP address for a set time and expects it to be
renewed. If the renewals keep failing, the lease runs out and the address drops
to 0.0.0.0, but the Wi-Fi link itself stays up, so no disconnect is ever
reported. Until now the adapter went on saying "connected", to the web app and
to the dryer's own Wi-Fi panel, while nothing could reach it: no page and no
MQTT until someone power-cycled it at the machine.

It now watches its own address:

- **After 5 s without one:** it reports `no-ip`, in the app and on the dryer's
  panel.
- **At 15 s:** it restarts DHCP.
- **At 45 s:** it leaves and rejoins the network. While the address stays
  missing, the wait doubles on each rejoin until it reaches one every 6 minutes,
  and it keeps going for as long as the adapter is powered.

The setup hotspot is never reopened for this. `/api/state` gains `sta_assoc`,
`noip_s`, and counters for episodes, DHCP restarts and rejoins.

**vskiwi** found this on a live adapter after several hours of uptime and wrote
the fix. It is the one commit from the v1.2.0 security review that did not make
that release (#8), and on vskiwi's adapter it has already recovered from a real
lease loss on its own.

## °F or °C

**Settings → Temperature unit** switches the dashboard, the live reading, the
chart and the logbook to Celsius. It is a display choice saved in your browser,
like the light/dark theme, so it asks for no PIN and changes nothing on the
adapter. The dryer, the stored data and the API stay in °F. Recipe setpoints
stay in °F too, because they are sent to the dryer in its own unit. Home
Assistant already shows the temperature sensor in your own unit setting, so
nothing changes there.

The conversion and chart code comes from vskiwi's #7.

## Fixes

- **`/api/state` could send more than it built.** It sent however many bytes
  the JSON asked for, even when they did not fit the buffer, which reads past
  it into the handler's stack. The stored PIN was loaded onto that same stack
  just to report whether one is set, and this endpoint needs no PIN. Only an
  oversized, malformed frame from the dryer could trigger it, and nothing
  suggests it ever has. The length is now checked: if the raw last frame is
  what does not fit, it is left out and the rest still goes. Whether a PIN is
  set is now checked without reading it.
- **A browser that blocks site data got an empty dashboard.** Reading the saved
  theme threw at page load and stopped the rest of the page from starting. A
  blocked read now falls back to the default.

## Installing

**Over the air**: Settings → Firmware update, then upload
`hr_wifi_adapter.bin`. If a PIN is set, you are asked for it before the upload
starts.

**First-time flash over USB**: write all four files. Leaving out
`ota_data_initial.bin` boots the old image, which looks exactly like a failed
flash.

| file | offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `hr_wifi_adapter.bin` | `0x20000` |

## What has and has not been tested

**Verified:**
- 17 host test suites, about 10,000 checks, including 84 for the watchdog.
- A clean `esp32s3` build with zero warnings.
- The °F/°C switch in a browser against the repo's mock dryer API: dashboard,
  chart, logbook, reload, phone width, and a browser with site data blocked.
- vskiwi ran the watchdog on a live adapter for 17 hours, through one real
  lease loss.

**Not yet:**
- This exact build on a live dryer.
- A complete freeze-drying cycle on 1.2.x, which still has not been reported.
- An automated test for the `/api/state` change. `hr_http.c` only builds for
  the ESP32, so that fix rests on the build and a read-through.
