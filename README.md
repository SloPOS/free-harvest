<h1>
<img src="docs/img/leaf.svg" width="28" align="top" alt=""> Free Harvest
</h1>

**Open-source Wi-Fi monitoring _and control_ for Harvest Right freeze dryers, running entirely on a $10 ESP32-S3.**

Free Harvest replaces the proprietary Harvest Right Wi-Fi adapter with hardware you
own and firmware you can read. It plugs into the freeze dryer's USB port, decodes the
machine's telemetry, and serves a mobile-friendly web app — with optional MQTT so the
dryer appears in Home Assistant automatically.

Start a batch, configure a recipe, end a run, add drying time — from your phone, over
your own network. No cloud account. No vendor lock-in.

Stop by our discord and say hey: https://discord.gg/KphHBYh9KC

> ## ⚠️ Check your dryer's firmware first
>
> **Free Harvest is developed and tested against dryer firmware `6.0.641041`.**
> Settings → Diagnostics on the machine shows the build.
>
> You need **firmware 6 or later**: earlier builds do not send the signals Free
> Harvest reads, and no adapter can change that.
>
> **Do not run `6.0.644170`.** That build is broken. On a dryer running it,
> *nothing* can talk to the machine — not Free Harvest, and **not HarvestRight's
> own adapter either**. We proved it the hard way: a working dryer was updated to
> it, both adapters went silent, and reverting to the build on
> [harvestright.com](https://harvestright.com) brought both straight back.
>
> ### If you have already updated to it
>
> **You can go back.** The build published on harvestright.com is the working
> one, and installing it over `6.0.644170` restored a dryer that had gone
> completely silent - both Free Harvest and the HarvestRight adapter started
> talking again immediately.
>
> We are deliberately not writing the procedure here, because we have done it
> once and that is not enough to instruct anyone else on updating a freeze
> dryer. Get the firmware and the steps from HarvestRight.
>
> If your dryer answers the identity query once and then goes quiet, check the
> firmware build before you suspect the adapter. That symptom cost this project
> days of chasing a bug that was never in the adapter at all.

![Dashboard while a batch runs](docs/img/dashboard-running.png)

---

## Contents

- [What it does](#what-it-does)
- [Remote control, and its limits](#remote-control-and-its-limits)
- [Hardware](#hardware)
- [Install the firmware](#install-the-firmware)
- [First-time setup](#first-time-setup)
- [Normal operation](#normal-operation)
- [Home Assistant / MQTT](#home-assistant--mqtt)
- [Settings reference](#settings-reference)
- [Updating (OTA)](#updating-ota)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [How it works](#how-it-works)
- [Contributing](#contributing)

---

## What it does

| | |
|---|---|
| 📒 **Batch logbook** | Every run recorded: duration, coldest point, deepest vacuum, pull-down time, extra drying |
| 🔑 **Control PIN** | Optional PIN on anything that changes the dryer; monitoring stays open |
| 🎛️ **Remote control** | Start, end, skip a stage, add drying time, defrost — the buttons the dryer is offering right now |
| 🧪 **Recipe editor** | The dryer's own Candy and Custom setup screens, with sliders, replicated in the app |
| 💾 **Saved recipes** | Store recipes with notes and a run count; the adapter suggests extra drying time when a batch needed it |
| 📊 **Live dashboard** | Cycle phase, temperature, vacuum (microns), batch + phase timers, countdowns |
| 🧭 **Phase-aware guidance** | Shows only the options valid right now, using the owner's-manual wording |
| 🏠 **Home Assistant** | MQTT auto-discovery — sensors appear automatically, no YAML |
| 📈 **Trend graph** | Temperature over the whole run, smoothed to kill the ±1 °F sensor flapping |
| 📡 **Raw data feed** | Every frame the dryer sends, with changed-field highlighting |
| 🩺 **USB diagnostics** | USB-level counters that tell you *why* the dryer is not being seen |
| ⬆️ **OTA updates** | Flash new firmware from the web page; no cable |
| 🔒 **Local only** | Setup hotspot auto-closes after 5 minutes; nothing phones home |

Two ways to run it:

- **Standalone** — the ESP32 serves the whole app. Browse to its IP from any device
  on your network. Nothing else required.
- **With Home Assistant** — additionally publish to your MQTT broker for dashboards,
  history, and automations. (Use a VPN like WireGuard/Tailscale for outside access —
  don't port-forward it.)

---

## Remote control, and its limits

**Free Harvest can start, configure and end a cycle.** Earlier versions of this
README said that was impossible. It was wrong, and the correction is worth
explaining because the mistake is an instructive one.

The dryer's firmware was disassembled and its serial protocol found to accept ~50
verbs, none of them `START`, `CONTINUE` or `END BATCH`. That fact is true. The
inference drawn from it — that control was therefore not exposed — was not. Control
is **screen-relative**: a single verb, `CLICK`, presses whatever the panel is
currently showing.

    CLICK <screen> <button> <counter> <session>
    CLICK 1 10 54779 175300      <- Start, on the Ready screen

So there is no per-function verb to find, and searching for one finds nothing. The
mechanism only became visible by watching the genuine app drive a simulated dryer.
Every button in this app was captured that way, not guessed.

### What that means in practice

| | |
|---|---|
| **Start a batch** | Auto, or a Candy/Custom recipe you configured in the app |
| **During a run** | End the batch, skip the current stage |
| **Final dry** | Add or remove drying time |
| **When complete** | Defrost, finish without defrost, warm trays, add two more hours |
| **Recipes** | Full Candy and Custom editors, saved with notes on the adapter |

### The safety model

Control is **off by default**, behind a switch in Settings. Monitoring is useful to
people who never want remote control, and the default should not be the option that
can start a day-long cycle.

Button numbers are screen-relative, which makes a **stale view** the real hazard —
End Batch is button 4 on Freezing and button 1 on Drying, so a phone still showing
the old screen would not mis-fire, it would press something else. The app therefore
never sends a button number. It sends an action name plus the screen it *believed*
it was showing, and the firmware refuses if telemetry disagrees.

Screens whose buttons have never been captured offer **nothing at all** rather than
guesses. Anything that starts, ends or skips part of a cycle asks first, and names
the cost: *"You will lose 18h 04m of progress on this batch."*

**A PIN is available** and gates every endpoint that can change the machine —
control, recipes, raw commands, firmware updates. Monitoring stays completely
open. Five wrong attempts lock control for a minute, which turns guessing a
four-digit PIN from seconds into weeks.

It is not a login system, and the UI says so: no accounts, no sessions. The
threat it addresses is a housemate or a guest tapping Start, not a determined
attacker. Note there is **no recovery** — a forgotten PIN means reflashing.

Some things stay blocked in firmware and cannot be reached from the app or MQTT:
`DUTY`, `HCS`, `SPC` (hardware duty cycles), `REBOOT`, `SETSN` (overwrites the
serial number) and `FDRENAME`. The dryer's own settings/diagnostics page is also not
offered — opening it stops the machine servicing USB, so it is the one control that
would destroy the connection it was sent over.

**This is still a vacuum pump and a heater running unattended for 24 hours.** Remote
control does not change that. Nothing here removes the need to load the trays and
shut the drain valve by hand.

---

## Hardware

| Item | Notes |
|---|---|
| **ESP32-S3 board** | Must be **S3** (or S2) — needs native USB. An ESP32-S3 N16R8 DevKitC-1 works best: [https://amzn.to/4hGcVpa](https://amzn.to/4zDtFEi). ~$12. |
| **USB-C to USB-A cable** | From the board's **USB** port to the freeze dryer's USB-A port. |
| Freeze dryer | Harvest Right, firmware v6.x (developed against v6.4 / build 641041). |

> ### ⚠️ The original ESP32 will not work
> A classic ESP32 (WROOM-32, DevKitC-32) has **no USB device controller** — its USB
> port is only a serial bridge for flashing. It physically cannot present itself to
> the dryer. You need an **ESP32-S3**.

> ### ⚠️ Use the right port on the S3
> The DevKitC-1 has **two** USB-C sockets:
> - **`USB`** — native USB. **This one goes to the freeze dryer.**
> - **`UART`** — flashing/console bridge. Use this for the first flash only.

---

## Install the firmware

### Option A — pre-built binary (easiest)

1. Download `hr_wifi_adapter.bin` from the [latest release](../../releases).
2. Connect the board's **UART** port to your computer.
3. Flash with [esptool](https://github.com/espressif/esptool):

```bash
python -m esptool --chip esp32s3 -p COM7 -b 460800 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 hr_wifi_adapter.bin
```

Replace `COM7` with your port (`/dev/ttyUSB0` on Linux, `/dev/cu.usbserial-*` on macOS).

> **Don't omit `ota_data_initial.bin`.** It resets the OTA boot selector. If you
> skip it, the bootloader keeps whatever slot it was last told to use and will
> happily boot your *previous* firmware instead of the one you just flashed —
> the symptoms are confusing (old behaviour, failing OTA). Check
> **Settings → About** after flashing: if the version isn't the one you
> installed, that's what happened.

<p align="center">
  <img src="docs/img/settings-about.png" width="330"
       alt="About screen showing the installed version">
</p>

### Option B — build it yourself

See [Building from source](#building-from-source).

After the first flash, **all future updates go over Wi-Fi** — see [Updating (OTA)](#updating-ota).

---

## First-time setup

<img src="docs/img/setup.png" width="330" align="right" alt="Setup screen">

**1. Power the board.** Any USB power source. It boots into setup mode.

**2. Join its hotspot.** On your phone or laptop, connect to the Wi-Fi network:

```
HR-Adapter-Setup
```

A setup page should open automatically (captive portal). If not, browse to
**http://192.168.4.1**.

**3. Pick your Wi-Fi.** Tap **Rescan**, choose your network, enter the password,
tap **Connect**.

> The page will briefly disconnect — that's normal and expected. The ESP32 has one
> radio, so joining your network moves the hotspot too. Wait a few seconds and it
> shows you the address to use.

**4. Note the address.** When connected it displays something like
`http://192.168.1.42/`. Bookmark it. (Or set a DHCP reservation in your router so
it never changes.)

**5. Plug into the dryer.** Connect the board's **USB** port to the freeze dryer's
USB-A port with the USB-C→USB-A cable. Within ~15 seconds the dot in the header turns
green and readings appear.

<br clear="right">

> 🔒 **Security note:** the setup hotspot stays open for **5 minutes only**. If no
> connection is made it shuts down and requires a reboot to reopen — so a router
> outage can't leave an open access point broadcasting indefinitely. Once connected
> to your network, the hotspot is disabled entirely.

---

## Normal operation

Browse to the adapter's address from any device on your network. The dashboard shows
whatever the dryer is doing right now.

| Idle | Freezing | Drying | Complete |
|---|---|---|---|
| ![Idle](docs/img/dashboard-controls.png) | ![Freezing](docs/img/dashboard-freezing.png) | ![Drying](docs/img/dashboard-drying.png) | ![Complete](docs/img/dashboard-complete.png) |
| Start a batch, or open a recipe | % frozen + estimated time to 100% | Live vacuum in microns, phase timer, End Batch | Defrost, warm trays, or add two more hours |

### Batch setup

Pressing **Candy Setup** or **Custom Setup** moves the dryer to its configuration
screen, and the app replicates that screen with sliders:

| Candy | Custom |
|---|---|
| ![Candy setup](docs/img/setup-candy.png) | ![Custom setup](docs/img/setup-custom.png) |
| Tray load, prewarm, dry temperatures and times | Initial freeze temperature, extra freeze time, dry temperature |

Values are read from the machine, so you are editing what is genuinely loaded rather
than a remembered default.

**Submit, then Start.** Moving a slider changes nothing on the dryer. *Submit
settings* sends the whole recipe; only then does *Start batch* unlock. Change
anything afterwards and Start greys out until the new values are submitted — so the
dryer never holds a half-finished set of edits, and Start always runs something that
was explicitly confirmed. With nothing changed the roles invert: Submit is the greyed
one, and Start simply runs what the machine already holds.

### Saved recipes

Recipes are stored on the adapter, not in your browser, so they survive reboots and
read the same on every device. Each carries free-text notes and a run count.

If a batch needed extra drying, the adapter notices — it watches for the screen
returning from Complete to Drying, so it counts presses made by hand on the panel too
— and offers to fold that time into the recipe.

One quirk worth knowing: recipe names are checked against the protocol. The dryer
matches verbs by substring and tests `ADD`, `DIR` and `DEL` *before* `SENDCANDY`, so a
recipe called `ADDED SUGAR` would be routed to a different command entirely. Those
names are refused. Lower case is fine — the dryer's comparison is case-sensitive.

The **phase card** below the dial always tells you what's happening and what comes
next, quoting the owner's manual — including the drain-valve steps that are easy to
get wrong.

Also available regardless of the control setting:

- **🔄 Refresh now** — asks the dryer for an immediate status update
- **🔔 Beep** — makes the dryer beep (handy for confirming the link end-to-end)

---

### Batch logbook

Every completed run is recorded on the adapter — name, date, duration, the
coldest temperature reached, the deepest vacuum, how long the pump took to pull
down, and how much extra drying was needed. Download it as CSV for a
spreadsheet.

It stores the **extremes**, not the last reading, because "how cold did it
actually get" is the question worth answering. Runs that were ended early or
interrupted by a power cut are recorded too, flagged as such — a batch that died
at hour 18 is worth knowing about when tuning a recipe.

The adapter has no clock and does not use SNTP, so it learns the time from your
browser when you open the app. Records written before that ever happens say
"date not recorded" rather than claiming 1970.

### Light and dark, phone and desktop

The interface follows your system theme, or you can pin it in Settings.

| Dark | Light |
|---|---|
| ![Dark](docs/img/dashboard-running.png) | ![Light](docs/img/dashboard-light.png) |

On a wide screen the layout goes two-column and Settings becomes a modal over
the dashboard rather than a separate page:

| Desktop, dark | Desktop, light |
|---|---|
| ![Desktop dark](docs/img/desktop-dark.png) | ![Desktop light](docs/img/desktop-light.png) |

---

## Home Assistant / MQTT

<img src="docs/img/settings-mqtt.png" width="330" align="right" alt="MQTT settings">

Free Harvest connects to **your** broker as a client (the Mosquitto add-on is ideal).
It does not run a broker itself.

1. Go to **Settings → Home Assistant / MQTT**
2. Enter the broker host (usually your Home Assistant machine's IP)
3. Port **1883** for plain MQTT — *not* 8123 (HA's web UI) or 8443 (HTTPS)
4. Username / password if your broker requires them
5. **Save & connect**

The dryer then appears in Home Assistant automatically via MQTT discovery, as a device
with these entities:

| Entity | Type |
|---|---|
| Temperature | sensor (°F) |
| Pressure (raw) | sensor |
| State Code / Mode | sensor |
| Batch Elapsed / Prep Remaining | sensor (duration) |
| Refresh Status / Beep | button |
| Batch Name | text |

**Topics** (`<id>` is derived from the board's MAC):

```
hrdryer/<id>/state        telemetry JSON   (retained)
hrdryer/<id>/avail        online/offline   (retained, LWT)
hrdryer/<id>/frame        raw frames
hrdryer/<id>/cmd          → send a command
hrdryer/<id>/config/set   → set batch name
```

<br clear="right">

---

## Settings reference

<img src="docs/img/settings.png" width="330" align="right" alt="Settings menu">

| Section | Contains |
|---|---|
| **Batch** | Name the current batch |
| **Recipes** | Save, edit, send and delete Candy and Custom recipes |
| **Logbook** | Past batches, CSV download, clear |
| **Control PIN** | Set, change or remove the PIN |
| **Remote control** | Master switch for control. Off by default |
| **Wi-Fi** | Network status, change network, forget network |
| **Home Assistant / MQTT** | Broker host, port, credentials, connection status |
| **Live data feed** | Verbs seen, live frame log, download capture |
| **Firmware update** | OTA upload |
| **Debug & advanced** | Device log, set dryer clock, raw commands, counters |
| **About** | Version number — quote this when reporting issues |

<br clear="right">

---

## Updating (OTA)

No cable needed after the first flash — **and from v0.3.4 onward a bad update
reverts itself.**

> **Upgrading from v0.2 or any 0.3.x needs one USB flash**, including
> `bootloader.bin`. Rollback protection is enforced by the bootloader, and OTA
> never rewrites the bootloader. That one cable trip is what buys you safe
> wireless updates afterwards.

1. Download the new `hr_wifi_adapter.bin`
2. **Settings → Firmware update → choose file → Upload & install**
3. It writes to the spare slot, verifies, then reboots into the new version

If the upload fails or the file is invalid, it's **rejected and the current firmware
keeps running** — the device won't be bricked by a bad upload.

Beyond that, every OTA image boots **on trial**. It is kept only once the adapter
is reachable again — Wi-Fi joined, or the setup hotspot up — and otherwise rolls
back to the previous firmware after 120 seconds.

Reachability is deliberately the test rather than "working properly": a build
that cannot decode a single dryer frame is still keepable, because you can reach
it to upload another one. A build that cannot get on the network is not. Note
that *not seeing the dryer* is *not* a rollback trigger — the dryer may simply be
unplugged, and reverting good firmware over that would be worse than the
problem.

---

## Troubleshooting

<img src="docs/img/settings-usb.png" width="330" align="right" alt="USB link diagnostics">

**If the dryer is not being seen, start here: Settings → Debug & advanced → USB link
to dryer.** It reports USB-level counters measured *before* any protocol decoding,
and states which fault this actually is — never enumerated, enumerated but silent,
or bytes arriving that we fail to parse. Those three look identical on the
dashboard and have completely different causes.

**For anything else, the Device log** is the place to look — the adapter's internal log is
visible in the browser and names the actual error.

| Symptom | Cause / fix |
|---|---|
| Dot stays red, no readings | Open **Settings → Debug & advanced → USB link to dryer** — it states which of the three possible faults this is. First check you are using the socket labelled **USB**, not **UART** |
| USB panel says "enumerated, 0 bytes" | The dryer sees a USB device but is not talking to it. Try **Reconnect USB** on the same screen; if that fails, power-cycle the dryer |
| USB panel says "never connected" | Cable, socket or power — not firmware |
| MQTT won't connect, log shows `invalid header=0x48` | You pointed it at a web server. `0x48` is `H` from `HTTP` — use port **1883** |
| MQTT log shows `broker REFUSED: return_code=4/5` | Wrong username/password, or that user isn't authorised on the broker |
| MQTT log shows `TCP error: sock_errno=104/111` | Nothing listening — check Mosquitto is running and reachable on your LAN |
| Can't find the setup hotspot | It closes after 5 minutes. Power-cycle the board to reopen it |
| Shows "Ready" during a real batch | Normal for ~30s after a reboot: it must observe the elapsed counter advance before claiming a run |
| Page unreachable after Wi-Fi change | Its IP probably changed — check your router's client list |

---

## Building from source

Requires [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/) **v5.0+** (developed on 6.0.1).

```bash
git clone https://github.com/SloPOS/free-harvest.git
cd free-harvest
idf.py set-target esp32s3
idf.py build
idf.py -p COM7 flash monitor
```

### Run the tests (no hardware needed)

The protocol core is plain C11 with no ESP-IDF dependencies, so it builds and runs on
your PC:

```bash
bash test/run_tests.sh
```

**201 checks across 8 suites.** Covers frame parsing, stream reassembly, frame
building, the command allow-list, telemetry decoding, phase detection, URL decoding,
and JSON output.

### Layout

```
components/hr_protocol/     portable core (no ESP-IDF deps, fully testable)
  hr_protocol.[ch]            framing: parse / build / reassemble
  hr_session.[ch]             link state machine, command allow-list
  hr_history.[ch]             frame ring buffer, per-verb table, field diffing
  hr_telemetry.[ch]           STAT decoding, cycle-phase detection
  hr_trend.[ch]               30s series + temperature-adaptive smoothing
main/                       ESP-IDF layer
  main.c                      wiring
  hr_usb.[ch]                 TinyUSB CDC-ACM device (the dryer is USB host)
  hr_wifi.[ch]                provisioning, captive portal, AP timeout
  hr_http.[ch]                web server + REST API
  hr_mqtt.[ch]                MQTT client + Home Assistant discovery
  hr_log.[ch]                 in-app log capture
  hr_capture.[ch]             persistent flash log of every frame
  www/index.html              the web app (single file, embedded in firmware)
test/                       host unit tests
tools/
  mock_dryer.py               simulate a dryer over serial (no hardware)
  probe_adapter.py            make a PC play the dryer: proves the USB CDC
                              path end to end with no dryer attached
```

---

## How it works

The freeze dryer is a **USB host**; the adapter presents itself as a **USB CDC-ACM
device** (which is why an S3 is required). Over that link the dryer speaks a plain
ASCII protocol:

```
STAT,1,0,0,0,69,151882,0,0,38,0,1,QUALITY,v6.4,,
NTFY,1,0, ,0,
REQINFO,
```

Comma-delimited fields, terminated by CR. `STAT` is multiplexed — the field after the
verb is a **type discriminator** that changes the whole layout (type 17 is the
15-minute prep countdown, type 1 is normal status, type 15 is diagnostics, and so on).

Field meanings were recovered by correlating live captures against the machine's
screen and against strings in the dryer's own firmware.

📄 **[decoded/PROTOCOL_NOTES.md](decoded/PROTOCOL_NOTES.md)** documents the wire format,
every known verb, and which fields are confirmed vs. inferred.

### Known gaps

- **The full cycle is now decoded** — Freezing (type 4), Drying (5), Final dry (6),
  Complete/vent (7) and Idle (1) — from a 22-hour capture, with real vacuum
  readings, per-phase timers and live % frozen. **Defrost has still never been
  captured.** A transient type 44 appeared once inside final dry and remains
  unmapped.
- **Time-remaining is still naive.** The freeze estimate extrapolates linearly,
  but cooling is exponential (Newton's law of cooling), so it under-estimates the
  final few degrees — which are the slowest. A curve-fitting estimator that learns
  from past cycles is in progress; the trend graph is its first piece.
- **Alerts and a learning time estimator are designed but not shipped.** Both
  are specified in `docs/superpowers/specs/`; the estimator needs a body of real
  batches before it can be validated, which is exactly what the logbook is now
  collecting.
- **Screens 15 (Diagnostics) and 44 are unmapped**, so the app offers no buttons on
  them. Screen 2's Continue, and every other screen, is mapped.
- **Defrost has never been captured.** It can now be triggered (`CLICK 7 1`), so
  this is one run away from being closed.
- **`SENDCANDY`/`SENDCUSTOM` sent through the raw command box or MQTT are
  malformed.** Those paths use the generic field builder, which takes the quoted
  recipe payload apart. The recipe editor builds them correctly.
- **`reset_reason` reports `panic` after an OTA reboot** where a clean restart should
  report `sw`. The new image boots and runs correctly and the filesystem is
  unmounted cleanly first, but the cause is unfound.
- **Several `SENDCANDY`/`SENDCUSTOM` fields are carried but not understood.** They
  are passed through untouched rather than exposed for editing.
- **Field meanings marked "inferred"** in the protocol notes need confirmation.

Captures are very welcome — see below.

---

## Contributing

The most useful contribution right now is **a full-cycle capture**: run a batch with
the adapter attached, then **Settings → Live data feed → Download log**, and open an
issue with the file. That unlocks sub-phase detection and the pressure scale.

Code contributions welcome. Please keep the portable core (`components/hr_protocol/`)
free of ESP-IDF dependencies and covered by tests — `bash test/run_tests.sh` must pass.

---

## Licence

MIT — see [LICENSE](LICENSE).

Not affiliated with, endorsed by, or supported by Harvest Right. "Harvest Right" is a
trademark of its respective owner. Use at your own risk; this software does not control
your freeze dryer and cannot make it unsafe, but you are responsible for your machine.
