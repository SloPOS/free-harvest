# Free Harvest v1.0.5

**Your dryer's WiFi panel will now show a connection.**

Until this release it showed your network name and, below it, nothing —
no connection to HarvestRight, no matter how healthy the link actually was.

`WIFIINFO` carries two flags the dryer's own panel renders: field 3
(`registered`) and field 6 (`cloud`). Both were hardcoded to `0` for the entire
life of this project, because nothing ever wrote them — the function that sets
the WiFi state didn't even accept them as arguments. So the adapter told the
dryer, every few seconds, that it had no network beyond the local one.

```
ours before : WIFIINFO 5 48 "MyNetwork" 0 HR-Adapter-Setup 0 0 2726
genuine     : WIFIINFO 5 81 "MyNetwork" 1 HR_aabbccddeeff  0 1 37
                                        ↑                    ↑
                                  registered              cloud
```

Both flags now follow your WiFi connection: `0/0` before the adapter has a
network, `1/1` once it is associated — matching the genuine adapter's captured
transitions.

**This does not connect anything to HarvestRight.** Nothing in this firmware
talks to their servers, and it never has. The flags say *this adapter is
connected and serving your dryer*, which is true, and which is what the panel is
asking about. It is not a claim of account registration.

## Why it might matter beyond the display

Some dryer firmware appears to wait on this before it will proceed. A machine
sitting on its WiFi panel re-asking for adapter info every two seconds is
consistent with waiting to be told the link is complete. If your dryer connects
but never starts reporting, this release is worth trying.

## Longer logs

The log held about **45 seconds**. It now holds roughly **3.5 minutes on a busy
link, or 10 minutes on an idle one** — enough to capture a boot and the
handshake that follows, which is the window that matters when something won't
start.

The real limit was never the buffer size. `/api/log` built its whole response in
one 8 KB buffer, which capped it near 80 lines regardless. It now streams in
chunks, so the ring is the only limit. The ring itself went 80 → 384 lines.

The ten-second status line also reports free heap now, so memory headroom is
something you can read rather than assume.

## Smaller things

- **`/api/log` accepts POST as well as GET.** It's read-only either way, and
  pasting it next to a POST that sends a command made a `405 Method Not Allowed`
  easy to hit — which looked like a firmware fault rather than a typo.
- **`POST /api/wififlags`** (`registered=0|1&cloud=0|1`) overrides the two flags
  by hand for diagnostics. A manual setting sticks until reboot and is not
  undone by the automatic rule.

## Installing

**Over the air** — Settings → Firmware update, upload `hr_wifi_adapter.bin`.

**First-time flash over USB** — all four files. Omitting `ota_data_initial.bin`
boots the old image and looks exactly like a failed flash.

| file | offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `hr_wifi_adapter.bin` | `0x20000` |

Verified on hardware: flashed over OTA, confirmed and kept, handshake completed,
telemetry resumed, and the connection flags asserted correctly from a cold boot
with 91 KB of heap free.

## Known gap

Whether a dryer told `cloud=1` will try cloud operations that then fail is
untested. Nothing of the sort appeared on 6.0.641041 across a full session, but
it has not been exercised on newer firmware. If you see new errors after
updating, `POST /api/wififlags` with `registered=0&cloud=0` restores the old
behaviour without downgrading.
