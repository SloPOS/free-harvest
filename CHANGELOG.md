# Changelog

## v0.3.7

The batch setup panel: configure and start a batch entirely from the app.

### Candy and Custom setup screens, replicated

A **Batch setup** card on the dashboard mirrors the dryer's own Candy and
Custom configuration screens, with sliders for every setting we have decoded.
Cooling runs -50..0 F, heating 70..150 F.

A value already outside those limits **widens its own slider** rather than
being clamped into range. Silently pulling a number nobody touched to the
nearest limit is worse than showing an unusual one - and this is not
hypothetical: a captured Candy recipe carried a prewarm temperature of 175.

Values are seeded from the machine when it is already showing the matching
screen, so the panel edits what is genuinely loaded rather than a remembered
default. "Skip prewarm" is presented as a toggle although the wire has no such
flag - it is prewarm time set to zero, and re-enabling restores the default
rather than leaving an enabled prewarm of zero minutes.

### Submit, then Start

Editing a control changes nothing on the machine. **Submit** sends the whole
recipe; only then does **Start** unlock. Change anything afterwards and Start
greys out again until the new values are submitted.

So the dryer is never holding a half-finished set of edits, and Start always
runs settings that were explicitly confirmed. With nothing changed, Submit is
the greyed-out one and Start simply runs what the machine already holds.

Batch names are checked as you type. The dryer matches verbs by substring and
tests `ADD`, `DIR`, `DEL` and others before `SENDCANDY`, so `ADDED SUGAR` would
be routed to a different command. The warning names the offending word and
points out that lower case is fine.

### Also

- `/api/state` exposes the raw `last_stat` frame, which is what lets the editor
  read live recipe values off the configuration screens.
- `/api/recipes/apply` sends explicit values without saving them as a recipe.
- The mirrored on-screen button list hides itself on screens 43 and 31, since
  the setup panel replaces it there. Every other screen still shows its
  buttons.

### Known issues

Unchanged from v0.3.6: `reset_reason` reports `panic` after an OTA reboot, and
`SENDCANDY`/`SENDCUSTOM` through `/api/cmd` or MQTT are still built by the
generic field builder and would be malformed.


## v0.3.6

Remote control, recipes, and a security fix. **Update over Wi-Fi.**

### Security: CLICK was reachable from the web UI

`CLICK` sat in the SAFE allow-list annotated "benign local effects (no state
change)". It is the opposite — it is the dryer's control verb, and

```
POST /api/cmd  verb=CLICK&args=1,10,54779,175300
```

from the raw-command box would have started a 24-hour cycle. `SETSN` and
`FDRENAME` were likewise sendable over MQTT while sitting on our own
"never probe" list. All three are now refused, and the tests assert it by
exercising the call the UI would really have made rather than merely checking a
classification.

Anyone running v0.3.5 or earlier on an untrusted network should update.

### New: remote control

The dashboard can now drive the dryer, **off by default** behind a switch in
Settings → Remote control.

Twelve actions across seven screens, every one captured from the genuine app.
The browser never names a button number: it posts an action name plus the
screen it believed it was showing, and the firmware refuses if telemetry
disagrees. That matters because button numbers are screen-relative — End Batch
is button 4 on Freezing and button 1 on Drying — so a stale view does not
mis-fire, it presses something else entirely.

Screens whose buttons have never been captured offer nothing at all rather than
guesses. Anything that starts, ends or skips part of a cycle asks first, and
names the cost: "You will lose 18h 04m of progress on this batch."

The settings/diagnostics button is deliberately **not** offered. It stops the
dryer servicing USB — telemetry went silent for 147 seconds in testing and only
returned when the panel was dismissed by hand. It is the one control that
destroys the channel it was sent over, and no confirmation dialog makes
"someone must now walk to the machine" acceptable.

### New: recipes

Save, recall and send Candy and Custom recipes, stored on the adapter so they
survive reboots and read the same from every device. Each carries a name, free
notes, and a run count.

Sending with Start begins a batch immediately.

The adapter also **learns**: if drying had to be extended, it counts the
extensions and offers to add that time to the recipe. It detects this from the
screen returning from Complete to Drying, so it works when the button is
pressed by hand on the panel — which is how it usually happens.

Recipe names are validated against the protocol. The dryer matches verbs by
substring over the whole line and checks `ADD`, `DIR`, `DEL` and others
*before* `SENDCANDY`, so a recipe called `ADDED SUGAR` or `RED DIRT` would be
routed to a different command. Those names are refused. Lowercase is safe and
accepted — the dryer's compare is case-sensitive.

### Fixed: five HTTP routes silently failed to register

`max_uri_handlers` was 20 against 25 routes. Handlers past the limit failed at
boot with only a log warning, and whichever fell off the end simply returned
404.

### Fixed: capture downloads returned an empty file

`stat()` reported 26,359 bytes on a log whose first read returned nothing —
SPIFFS metadata and data disagreeing on a partition mounted with
`format_if_mount_failed`, which is how a 270 KB capture silently became 26 KB.
The OTA reboot never unmounted the filesystem; it now does. The download no
longer trusts `stat()` either: a file that reads empty falls back to the RAM
ring and says so, instead of serving a successful download of nothing.

### Also

- `/api/state` reports `uptime_s` and `reset_reason`, so a restart is a reading
  rather than something you notice by catching a counter going backwards.
- `/api/state` carries the actions valid on the current screen, so the UI
  renders from the machine rather than a hardcoded list.

### Known issues

- `reset_reason` reports `panic` after an OTA reboot where a clean restart
  should report `sw`. The new image boots and runs correctly and the clean
  unmount protects the filesystem, but the cause is unfound.
- `SENDCANDY`/`SENDCUSTOM` sent through `/api/cmd` or MQTT are still built by
  the generic field builder and would be malformed. The recipe endpoints build
  them correctly; the raw paths do not.


## v0.3.5

Point fix on top of v0.3.4. **Update over Wi-Fi — no cable needed** (assuming
v0.3.4 is already installed, which is what put the bootloader in place).

### Fixed: web UI unreachable from a phone

Connecting from a mobile browser could fail with nothing in the UI and this in
the device log:

```
W httpd: httpd_server: error accepting new connection
E httpd: httpd_accept_conn: error in accept (23)
```

errno 23 is `ENFILE`, "too many open files in system" — the lwIP descriptor
table was exhausted, so `accept()` had no fd to give an incoming connection.

`esp_http_server` requires `max_open_sockets + 3 <= CONFIG_LWIP_MAX_SOCKETS`.
We ran `max_open_sockets = 7` against the IDF default of 10, so **7 + 3 = 10 of
10 — the web server claimed the entire socket table.** That configuration is
legal and the server starts normally, but once seven clients are connected there
is no spare descriptor, and `accept()` fails with `ENFILE` *before*
`lru_purge_enable` can reclaim a slot.

**Why it looked mobile-specific.** The page runs six poll timers — frames,
state, verbs, MQTT, capture, trend. On a desktop LAN each request completes long
before the next tick. On a phone the higher latency lets them overlap, so the
browser opens extra parallel connections and saturates all seven slots. Same
firmware, same network; only the round-trip time differs.

Two changes, one per half of the cause:

- `CONFIG_LWIP_MAX_SOCKETS` 10 → 16, leaving six spare descriptors for
  `accept()`, MQTT, DNS and the captive-portal responder. A `_Static_assert`
  now encodes the invariant *including* the spare capacity, so a future config
  change becomes a build failure with an explanation rather than an errno in the
  field.
- The web UI no longer starts a poll while its own previous request is still in
  flight, so requests cannot stack up on a slow link.

This is **not** the `/api/trend` socket leak fixed in v0.3.4 (unchecked chunk
sends leaving responses unterminated). They are two independent socket faults
with the same symptom, which is why v0.3.4 did not resolve this one.

v0.3.4 remains usable for a single client and is not withdrawn.

## v0.3.4

The first release after v0.2. **v0.3, v0.3.1, v0.3.2 and v0.3.3 were withdrawn**
— see [Withdrawn versions](#withdrawn-versions) below. If you are on v0.2, this
is your upgrade. If you somehow installed any 0.3.x, upgrade immediately: they
crash on every frame the dryer sends.

### Requires one USB flash

Rollback protection is enforced by the **bootloader**, and OTA never rewrites the
bootloader. So `bootloader.bin` has to be written at `0x0` once, over USB. Every
update after this one can go over Wi-Fi.

```
0x0     bootloader.bin          <-- the one that matters this time
0x8000  partition-table.bin
0xf000  ota_data_initial.bin
0x20000 hr_wifi_adapter.bin
```

All four regions are required. Omitting `ota_data_initial.bin` leaves otadata
pointing at the previous slot, so the bootloader silently boots the *old*
firmware. Confirm **Settings → About** reads `0.3.4` afterwards.

### Fixed: the adapter crashed on every frame the dryer sent

This is the headline fix and the reason 0.3–0.3.3 were withdrawn.

`hr_capture_append()` writes each frame to the flash capture log with
`fopen`/`fprintf`/`fclose`, and the frame observer runs **inside the USB CDC RX
callback** — on the TinyUSB task, whose stack defaulted to 4096 bytes. VFS plus
newlib file I/O needs far more than that, so **the first inbound frame overflowed
the stack and panicked the chip.** It rebooted and re-enumerated fast enough to
look continuously healthy.

Every symptom pointed the wrong way: the adapter appeared enumerated and idle,
and the byte counter read zero *because it reset along with the chip*. It looked
exactly like a dryer that had stopped talking.

Fix: `CONFIG_TINYUSB_TASK_STACK_SIZE` 4096 → 8192.

Found by bisecting v0.2..HEAD using OTA plus `tools/probe_adapter.py` as the
test, then toggling a single line:

| build | result |
|---|---|
| v0.2 | `GOTIT` — works |
| `9ca8b0c` (first commit after v0.2) | dead |
| `9ca8b0c` minus `hr_capture_init()` | `GOTIT` — works |
| with the larger TinyUSB stack | `GOTIT` — works |

Confirmed directly: probing the device made its uptime jump 30,459 ms →
10,466 ms. It was rebooting on every received byte.

### New: temperature trend graph

The dashboard now charts the current run. Hand-rolled inline SVG — no charting
library, because the page is served by the adapter itself with no route to the
internet.

`hr_trend` (portable, host-tested) buckets readings into fixed 30-second slots
and keeps the **median** of each: a median discards a lone outlier outright,
where a mean blends it in. Frames arrive irregularly — roughly every 15 s when
idle, with 76-second gaps observed mid-run — so they cannot feed a
fixed-interval chart directly. Gaps become gaps in the line rather than
interpolated guesses.

**Smoothing that adapts to temperature.** The dryer reports whole degrees
Fahrenheit, and cooling is exponential, so as the chamber nears its target the
true rate of change falls below 1 °F per bucket and the reading *must* alternate
between adjacent integers — there is no third value available to it. That is why
the flapping gets worse the colder it gets. The deadband therefore widens as
temperature falls (±0.5 °F above freezing → ±2 °F at −20 °F), with hysteresis
requiring 2–6 consecutive same-direction buckets before the line moves.

Raw is drawn faintly beneath the smoothed line. Without seeing the noise it
removes, you cannot tell whether smoothing is helping or lying to you.

Verified live mid-freeze at −8 °F: raw `-8,-8,-9,-9,-9,-8`, smoothed line flat.

Known limitation: the series lives in RAM, so a reboot loses the current run's
graph. Persisting it requires moving flash writes onto a worker queue first, so
that no flash I/O ever lands on the USB RX path again.

### New: OTA you can rely on

**A bad update now reverts itself.** Every OTA image boots on trial and is kept
only once the adapter is genuinely *reachable* — Wi-Fi joined, or the setup AP
up — otherwise it rolls back after 120 seconds.

Reachability is deliberately the test rather than "working correctly". A build
that cannot decode a single dryer frame is still keepable, because you can reach
it to upload another. A build that cannot get on the network is not. Failing to
see the dryer is explicitly **not** a rollback trigger — the dryer may simply be
unplugged, and reverting good firmware over that would be worse.

Three fixes to the upload path, each independently able to fail a transfer:

- **HTTP server stack too small.** The OTA handler puts a 1 KB receive buffer on
  it and then calls through `esp_ota_write()` into the SPI flash driver. An
  overflow looked exactly like a failed upload with nothing logged. 4096 → 8192.
- **Wi-Fi power save was on.** The adapter is mains-powered from the dryer's USB
  port, so parking the radio between beacons bought nothing and added
  multi-second stalls to a 1.3 MB push. Now disabled; the web UI is more
  responsive too.
- **The UI fought its own upload.** The server handles everything from a single
  worker, which the upload blocks for the whole transfer, while five poll timers
  kept queueing requests and filling the socket table — and `lru_purge` would
  then close a socket to make room, potentially the upload's own. Polling now
  pauses for the duration.

Failures report where they died (`OTA stalled after N of M bytes`) instead of
just "rejected", and a vanished client times out after ~2.5 minutes rather than
hanging forever.

OTA was exercised roughly a dozen times during development without a failure.

### New: USB link diagnostics

**Settings → Debug & advanced → USB link to dryer** reports USB-level counters
measured *before* any protocol decoding, and states the conclusion they support.
They separate three faults that look identical on the dashboard: never
enumerated (cable/port/power), enumerated but silent, and bytes arriving that we
fail to parse.

This lives in the web UI deliberately. **The serial console cannot reach the
adapter while it is plugged into the dryer**, which is why diagnosing the crash
by console was hopeless — every log cut off within seconds of boot, long before
the dryer's ~15 s idle interval had elapsed once.

Also added: `tud_mount_cb`/`umount`/`suspend`/`resume` hooks, a raw pre-parser
byte counter, `SET_LINE_CODING` logging (which identifies *which* host is on the
wire), and a 10-second heartbeat. Previously the main loop was silent unless the
link state changed, so a short console capture was indistinguishable from a dead
adapter.

### New: Reconnect USB

**Settings → Debug & advanced → Reconnect USB** forces a detach/re-attach
(`tud_disconnect()`, 1.5 s, `tud_connect()`) without rebooting, then watches for
90 seconds and reports whether bytes started flowing. Safe mid-batch: the adapter
is a passive monitor — the protocol exposes no cycle control at all — and the
dryer sees the same detach/attach it gets on every OTA reboot.

### New: `tools/probe_adapter.py`

Plug the adapter's native USB socket into a PC and the PC plays the dryer's role:
it opens the CDC port and sends `REQINFO`, which the adapter must answer with
`GOTIT`. That exercises enumeration, the CDC pipe, reassembly, parsing and
transmit end to end, **with no dryer attached**. It reproduces USB faults in
seconds and is what made the bisection above possible.

### Also in this release

- **Full cycle decoded** from a 22-hour capture: type 4 Freezing → 5 Drying →
  6 Final dry → 7 Complete/vent → 1 Idle. Type 7 is the end-of-cycle marker; the
  vacuum release from ~206 µm to atmosphere is unmistakable.
- **Vacuum is microns (mTorr), not Torr** — proven by the dryer's own firmware
  string `HighmTorr`. Added torr/bar/atm/pascal conversions and contextual
  comparisons. Readings of exactly `10000` mean "pump off" and values above it
  are the uncalibrated at-atmosphere reading; neither is now shown as a vacuum.
- **Persistent capture log** on a 3 MB SPIFFS partition. The old 64-frame RAM
  ring held only ~16 minutes, which lost a full-cycle capture.
- **Fixed: connection flapping every ~15 s while idle.** The link timeout was
  15,000 ms against real idle frame gaps of 15,020–15,041 ms, so it expired
  milliseconds before each frame arrived. Now 45,000 ms.
- **Fixed: "batch running" while idle.** The dryer's elapsed counter retains the
  previous batch's value and simply stops advancing, so `elapsed > 0` does not
  mean a batch is running. The correct test is that it is *increasing*.
- **Fixed: setup screen lockout** on the setup hotspot — no Wi-Fi form and no tab
  bar, so no route to configure anything. The tab bar is now never hidden.
- Phase artwork in the progress ring, a scrolling facts strip, and a mobile-first
  Dashboard/Settings split.

### Withdrawn versions

**v0.3, v0.3.1, v0.3.2, v0.3.3 — do not use.**

All of them contain the TinyUSB stack overflow described above, so the adapter
panics on every frame the dryer sends and never reports telemetry. v0.3.2 and
v0.3.3 were never tagged or released publicly; they existed only as local
development builds. v0.3 and v0.3.1 were published and are now marked
deprecated.

v0.3 and v0.3.1 additionally lack OTA rollback protection, so a failed update
takes remote access away permanently and can only be recovered with a USB cable.

## v0.2

Drying phase (STAT type 5), real vacuum decoding, phase-elapsed timer.

## v0.1

First release: USB CDC-ACM transport, protocol core, Wi-Fi provisioning with
captive portal, web UI, MQTT with Home Assistant discovery, OTA, safe-command
allow-list.
