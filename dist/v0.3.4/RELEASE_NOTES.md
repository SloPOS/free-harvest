**This is the upgrade from v0.2. v0.3, v0.3.1, v0.3.2 and v0.3.3 are withdrawn — do not install them.**

Every 0.3.x build before this one crashes the adapter on **every frame the dryer
sends**. If you are running one, upgrade now. If you are on v0.2, you are fine,
and this is a large step forward.

## Needs one USB flash

Rollback protection is enforced by the **bootloader**, and OTA never rewrites the
bootloader. So `bootloader.bin` must be written at `0x0` once, over USB. Every
update after this one goes over Wi-Fi, and a bad one reverts itself.

```
python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset \
  write-flash --flash-mode dio --flash-size 8MB --flash-freq 80m \
  0x0 bootloader.bin 0x8000 partition-table.bin \
  0xf000 ota_data_initial.bin 0x20000 hr_wifi_adapter.bin
```

All four regions are required. Omitting `ota_data_initial.bin` leaves otadata
pointing at the previous slot, so the bootloader silently boots the **old**
firmware — a genuinely confusing failure. Confirm **Settings → About** reads
`0.3.4` afterwards.

## Why 0.3 through 0.3.3 were withdrawn

The capture log writes each frame to flash with `fopen`/`fprintf`/`fclose`, and
the frame observer runs **inside the USB CDC RX callback** — on the TinyUSB task,
whose stack defaulted to 4096 bytes. File I/O through VFS and newlib needs far
more, so **the first inbound frame overflowed the stack and panicked the chip.**

It rebooted and re-enumerated fast enough to look perfectly healthy the whole
time. The adapter appeared connected and idle, and the received-byte counter read
zero *because it reset along with the chip*. It was indistinguishable from a dryer
that had simply stopped talking — and was misdiagnosed as exactly that for a long
while.

The fix is one line: `CONFIG_TINYUSB_TASK_STACK_SIZE` 4096 → 8192.

It was found by bisecting v0.2 forward with OTA plus `tools/probe_adapter.py` as
the test, then toggling a single call:

| build | result |
|---|---|
| v0.2 | `GOTIT` — works |
| first commit after v0.2 | dead |
| that commit minus the capture-log mount | `GOTIT` — works |
| with the larger TinyUSB stack | `GOTIT` — works |

Confirmed directly: probing the adapter made its uptime jump 30,459 ms →
10,466 ms. It was rebooting on every byte received.

## New: temperature trend graph

The dashboard now charts the run in progress. Inline SVG, no charting library —
the page is served by the adapter itself with no route to the internet, so every
byte has to already be there.

Readings are bucketed into fixed 30-second slots, keeping the **median** of each:
a median discards a lone outlier outright, where a mean blends it in. Frames
arrive irregularly — roughly every 15 s when idle, with 76-second gaps seen
mid-run — so they cannot feed a fixed-interval chart directly. Gaps are drawn as
gaps, never as interpolated guesses.

**Smoothing that adapts to temperature.** The dryer reports whole degrees
Fahrenheit, and cooling is exponential, so as the chamber approaches its target
the true rate of change drops below 1 °F per bucket and the reading *must*
alternate between adjacent integers — there is no third value available. That is
precisely why the flapping gets worse the colder it gets. So the deadband widens
as temperature falls (±0.5 °F above freezing → ±2 °F at −20 °F), with hysteresis
requiring several consecutive same-direction buckets before the line moves.

Raw is drawn faintly beneath the smoothed line, deliberately: without seeing the
noise it removes, you cannot judge whether the smoothing is helping or lying to
you.

Verified live mid-freeze at −8 °F — raw `-8,-8,-9,-9,-9,-8`, smoothed line flat.

Vacuum is an opt-in second series on its own right-hand scale; microns and
degrees share no meaningful axis.

Known limitation: the series lives in RAM, so a reboot loses the current run's
graph. Persisting it requires moving flash writes onto a worker queue first — the
same discipline the crash above taught us.

## New: OTA you can rely on

Every OTA image now boots **on trial** and is kept only once the adapter is
genuinely *reachable* — Wi-Fi joined, or the setup hotspot up — otherwise it rolls
back after 120 seconds.

Reachability is the test rather than "working correctly", on purpose. A build that
cannot decode a single dryer frame is still keepable, because you can reach it to
upload another. A build that cannot get on the network is not. Failing to see the
dryer is explicitly **not** a rollback trigger — the dryer may simply be
unplugged, and reverting good firmware over that is worse than the problem.

Three separate upload-path fixes, each independently able to fail a transfer: the
HTTP server stack was too small for the flash-write path; Wi-Fi power save was
adding multi-second stalls to a 1.3 MB push for no benefit on a mains-powered
device; and the web UI's own five poll timers were competing with the upload for a
single-worker server, filling the socket table. Failures now report where they
died instead of just "rejected".

## New: USB link diagnostics, and Reconnect USB

**Settings → Debug & advanced → USB link to dryer** reports USB-level counters
measured *before* any protocol decoding, and states what they mean. They separate
three faults that look identical on the dashboard: never enumerated
(cable/port/power), enumerated but silent, and bytes arriving that we fail to
parse.

This lives in the web UI because **the serial console cannot reach the adapter
while it is plugged into the dryer** — which is why diagnosing the crash by
console was hopeless.

**Reconnect USB** forces a detach/re-attach without rebooting, then watches for 90
seconds and reports whether bytes started flowing. Safe mid-batch: the adapter is
a passive monitor, and the dryer sees the same detach/attach it gets on every OTA
reboot.

## New: `tools/probe_adapter.py`

Plug the adapter's native USB socket into a PC and the PC plays the dryer's role:
it opens the CDC port, sends `REQINFO`, and the adapter must answer `GOTIT`. That
exercises enumeration, the CDC pipe, reassembly, parsing and transmit end to end
**with no dryer attached** — it reproduces USB faults in seconds and is what made
the bisection above possible.

## Everything else since v0.2

- **Full cycle decoded** from a 22-hour capture: Freezing → Drying → Final dry →
  Complete/vent → Idle. The vacuum release from ~206 µm to atmosphere makes the
  end of cycle unmistakable.
- **Vacuum is microns (mTorr), not Torr** — proven by the dryer's own firmware
  string `HighmTorr`. Readings of exactly `10000` mean "pump off"; higher values
  are the uncalibrated at-atmosphere reading. Neither is shown as a vacuum now.
- **Persistent capture log** on a 3 MB flash partition. The old RAM ring held only
  ~16 minutes and lost a full-cycle capture.
- **Fixed: connection flapping every ~15 s while idle** — the link timeout was
  15,000 ms against real idle gaps of 15,020–15,041 ms, so it expired milliseconds
  before each frame arrived.
- **Fixed: "batch running" while idle** — the dryer's elapsed counter retains the
  last batch's value and stops advancing, so `elapsed > 0` does not mean running.
  The correct test is that it is *increasing*.
- **Fixed: setup screen lockout** on the setup hotspot, where there was no Wi-Fi
  form and no tab bar — no route to configure anything at all.
- Phase artwork in the progress ring, scrolling facts strip, mobile-first
  Dashboard/Settings split.

Full detail in [CHANGELOG.md](CHANGELOG.md).

## Still missing

- **Time-remaining is naive.** The freeze estimate extrapolates linearly, but
  cooling is exponential, so it under-estimates the final few degrees — the
  slowest ones. A curve-fitting estimator that learns across cycles is in
  progress; this graph is its first piece.
- **Defrost has never been captured**, and a transient state seen once inside
  final dry remains unmapped.
- **No remote cycle control exists, and none is possible** — the dryer's serial
  protocol contains no START/CONTINUE/DEFROST/CANCEL verb. Established by
  disassembling its firmware, not by guessing. All flow control is panel-only.
