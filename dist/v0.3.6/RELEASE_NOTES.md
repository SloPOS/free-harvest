# Free Harvest v0.3.6

Remote control, recipes, and a security fix.

**Update over Wi-Fi.** Settings → Firmware update, upload `hr_wifi_adapter.bin`.
No cable needed if v0.3.4 or later is already installed.

---

## Security: update if you are on v0.3.5 or earlier

`CLICK` was in the SAFE allow-list, annotated "benign local effects (no state
change)". It is the opposite — it is the dryer's control verb, and

```
POST /api/cmd  verb=CLICK&args=1,10,54779,175300
```

from the web UI's raw-command box would have started a 24-hour cycle on an
empty machine. `SETSN` (overwrites the serial number) and `FDRENAME` were
likewise sendable over MQTT while sitting on the project's own "never probe"
list.

All three are refused now, and the tests exercise the call the UI would really
have made rather than checking a classification that happened to pass.

---

## Remote control

The dashboard can drive the dryer. **Off by default**, behind a switch in
Settings → Remote control — monitoring is useful to people who never want
remote control, and the default should not be the option that can start a
day-long cycle.

Twelve actions across seven screens, every one captured from the genuine app.

**The browser never names a button number.** It posts an action name plus the
screen it believed it was showing, and the firmware refuses if telemetry
disagrees. Button numbers are screen-relative — End Batch is button 4 on
Freezing and button 1 on Drying — so a stale view does not mis-fire, it presses
something else entirely. A UI cannot solve that, because a UI is always behind.

Screens whose buttons have never been captured offer **nothing**, rather than
guesses. Anything that starts, ends or skips part of a cycle asks first and
names the cost: *"You will lose 18h 04m of progress on this batch."*

The settings/diagnostics button is deliberately not offered. It stops the dryer
servicing USB — telemetry went silent for 147 seconds in testing and returned
only when the panel was dismissed by hand. No confirmation dialog makes
"someone must now walk to the machine" acceptable.

## Recipes

Save, recall and send Candy and Custom recipes. Stored on the adapter, so they
survive reboots and read the same from every device. Name, free notes, run
count. Sending with Start begins a batch immediately.

**The adapter learns.** If drying had to be extended, it counts the extensions
and offers to add that time to the recipe. It detects this from the screen
returning from Complete to Drying, so it works when the button is pressed by
hand on the panel — which is how it usually happens.

**Recipe names are validated against the protocol.** The dryer matches verbs by
substring over the whole line and checks `ADD`, `DIR`, `DEL` and others
*before* `SENDCANDY`, so a recipe called `ADDED SUGAR` or `RED DIRT` would be
routed to a different command entirely. Those are refused. Lowercase is
accepted — the dryer's compare is case-sensitive, and being stricter would
block most ordinary names for nothing.

## Fixes

**Five HTTP routes silently failed to register.** `max_uri_handlers` was 20
against 25 routes; handlers past the limit failed at boot with only a log
warning and whichever fell off the end returned 404.

**Capture downloads returned an empty file.** `stat()` reported 26,359 bytes on
a log whose first read returned nothing — SPIFFS metadata and data disagreeing
on a partition mounted with `format_if_mount_failed`, which is how a 270 KB
capture silently became 26 KB. The OTA reboot never unmounted the filesystem;
it now does, and the download no longer trusts `stat()`.

**`/api/state` reports `uptime_s` and `reset_reason`,** so a restart is a
reading rather than something you catch by noticing a counter run backwards.

## Known issues

- `reset_reason` reports `panic` after an OTA reboot where a clean restart
  should report `sw`. The new image boots and runs correctly and the clean
  unmount protects the filesystem, but the cause is unfound.
- `SENDCANDY`/`SENDCUSTOM` sent through `/api/cmd` or MQTT are built by the
  generic field builder and would be malformed. The recipe endpoints build them
  correctly; the raw paths do not.
- Custom recipes have three settings against Candy's eight, and the fields
  governing Custom's drying are not identified — so the extra-dry suggestion is
  offered for Candy recipes only.

## Verification

4,395 host-side checks pass. Two of them pin the built recipe frames to the
exact bytes captured from the genuine app, so a drift in the wire format fails
the build rather than a batch.

Checked against a live adapter and a real dryer: recipes save, list and delete;
a name carrying a protocol verb is refused; a minutes/seconds mix-up is
refused; a start without confirmation is refused; a button meant for another
screen is refused; and everything is refused with control switched off.

## Files

| file | offset | needed for |
|---|---|---|
| `hr_wifi_adapter.bin` | `0x20000` | OTA update, and manual flash |
| `bootloader.bin` | `0x0` | manual flash only |
| `partition-table.bin` | `0x8000` | manual flash only |
| `ota_data_initial.bin` | `0xf000` | manual flash only — **easy to forget, and omitting it boots the old image** |
