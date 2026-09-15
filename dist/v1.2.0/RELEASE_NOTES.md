# Free Harvest v1.2.0

**Every current dryer firmware is now supported.**

`6.0.644170` was never broken. It moved the USB link to an *encoded* transport
that neither Free Harvest nor HarvestRight's own adapter could read, so both went
silent on it and it looked like a dead machine. This release reads it.

**vskiwi** found that the dryer was still answering after `UNIQUE lH`, worked out
the `)S` framing, and built the framer, the capture records and the compat
switch. The cipher itself was then recovered from the `6.0.644170` image by
decompilation. Between them, a 644170 dryer now behaves like any other from the
app's point of view.

---

## Talking to `6.0.644170`

The encoded transport is base64 in a shifted alphabet, length-prefixed, carrying
a per-frame nonce that seeds a stream cipher. Frames are decoded inside the
session and then take **exactly the path a plaintext frame takes** — so
telemetry, the logbook, the graph, MQTT, the web UI and the dryer's own Wi-Fi
panel all work unchanged.

**There is nothing to turn on.** The adapter reads the dryer's build from its
`UID` reply and enables the encoded handshake itself the first time it sees
`6.0.644170`, then re-introduces itself so the machine starts answering. A
plaintext dryer is never touched, and the decoder only ever runs on a real `)S`
frame. The manual toggle stays in Settings → Debug (or `POST /api/compat
compat644170=1`) as an override, and a choice made by hand is remembered rather
than overridden on the next `UID`.

The decoder was validated byte-for-byte against a 454-frame live capture from a
running machine: every `REQINFO`, `SNM`, `CFG`, `STAT` and `UID` frame
reproduced its exact plaintext.

## A security and robustness pass

A read-through of v1.0.8 by **vskiwi** turned up a long list of real problems,
all fixed here. The ones worth naming:

| | |
|---|---|
| **OTA accepted any image** | `/api/ota` had no PIN check and no identity check — anyone on the LAN, or on the open setup AP, could flash the adapter. It now requires the PIN and refuses any image that is not Free Harvest. |
| **A bad Wi-Fi password could brick the adapter** | A rejected password aborted the chip *after* it had been stored, so the next boot aborted too, and only a reflash with erased NVS recovered it. Credentials are now validated and applied before they are stored. |
| **Wi-Fi gave up permanently** | After eight fast failures nothing ever retried, and with the setup window closed the adapter was unreachable until unplugged. It now keeps retrying for as long as it is powered. |
| **Cross-origin POSTs** | Any page on any device on the network could drive the control endpoints. They are refused now. |
| **Frames could interleave** | Four tasks wrote the USB endpoint unserialised, and a send that never left the buffer still reported success. |
| **Controls against a stale screen** | Telemetry was never marked stale, so a button could be built against a screen seen hours earlier. A control now requires a live link and a recent reading. |
| **Two unbounded JSON builders** | `/api/scan` and `/api/recipes` could overrun their buffers and ship the overrun to the client. |
| **MQTT publishing blocked USB** | Telemetry was published from the USB task, where a slow broker stalled the link the dryer depends on. |

Plus fixes to the PIN lockout timer, NVS error handling on the CLICK counter,
over-long Wi-Fi and MQTT values being saved truncated, and several
locking and boot-safety problems.

## The PIN is asked for up front

Monitoring stays completely open — the dashboard and the log downloads never ask
for anything. Everything past that is gated, and gated **before** the action
rather than after it.

That includes **opening a control screen**: Candy/Custom setup and the Wi-Fi,
MQTT, firmware and debug pages ask on the way in. The firmware upload is the
clearest case — being refused after the upload meant pushing the whole 1.3 MB a
second time just to learn a PIN was needed.

A new `/api/pin/verify` confirms a PIN with no side effect, so the app can settle
this before it acts, under the same five-try lockout as everything else. With no
PIN set, nothing prompts and nothing changes.

## Also in this release

Carried up from 1.0.7 and 1.0.8, for anyone coming from 1.0.6:

- **A finished batch is one logbook entry**, even when the dryer browns out its
  own USB rail mid-run and restarts the adapter — the run resumes instead of
  fragmenting into three records.
- **Per-phase durations** (freeze / dry / final dry) are recorded, and the next
  run is estimated from the median of this dryer's own finished cycles.
- **The graph draws the run that happened** rather than a flat line: gaps read as
  gaps instead of carrying the last value forward, and a full window drops its
  oldest point instead of silently discarding the entire run.
- **The capture download no longer hands back a fragment.** A damaged segment is
  stepped over rather than ending the download, and a fallback to the in-memory
  ring says so loudly instead of looking like a complete log.
- **Defrost (screen 10) is decoded.** The duration set with the machine's own
  arrows is still not transmitted by the dryer, so it cannot be read or set.

## Installing

**Over the air** — Settings → Firmware update, upload `hr_wifi_adapter.bin`. If a
PIN is set you are asked for it before the upload starts.

**First-time flash over USB** — all four files. Omitting `ota_data_initial.bin`
boots the old image and looks exactly like a failed flash.

| file | offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `hr_wifi_adapter.bin` | `0x20000` |

## What has and has not been tested

Verified: the 454-frame decode against a real capture; 15 host test suites; a
clean `esp32s3` build; and this build running on a live `6.0.641041` dryer with
the handshake, telemetry, logbook and capture all healthy and the decoder
correctly dormant. vskiwi ran the decode path against a live `6.0.644170` machine
for a ten-minute session.

Not yet: **a complete freeze-drying cycle on this build.** The concurrency
changes above — the USB serialisation, the MQTT publish task, the Wi-Fi retry —
are exactly the paths a 24-hour run exercises and a short session does not. If
you are running a batch on this release, a report either way is the most useful
thing you can send.
