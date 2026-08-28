# Free Harvest v1.0.6

**The feature release — plus a reversion, because we spent weeks chasing a bug
that was never ours.**

> ## ⚠️ Check your dryer's firmware
>
> Free Harvest is developed and tested against dryer firmware **`6.0.641041`**.
> Settings → Diagnostics on the machine shows the build.
>
> **Do not run `6.0.644170`.** That build is broken. On a dryer running it,
> *nothing* can talk to the machine — not Free Harvest, and **not HarvestRight's
> own adapter either**. A working dryer was updated to it, both adapters went
> silent, and reverting to the build on harvestright.com brought both back.

## What's new

| | |
|---|---|
| 🔄 **Reboot the dryer** | Settings → Debug & advanced, behind a confirmation. For a locked-up panel |
| 🏷️ **Name a batch when you start it** | Optional field on every start confirmation, instead of only in Settings |
| ✅ **Every recipe send confirms** | Two paths used to reach the dryer with no prompt at all — both replace the machine's configured recipe |
| ♻️ **The capture log rotates** | It used to *stop* when the partition filled, keeping the oldest hours and discarding everything since. Four segments now; recording never stops |
| 🗜️ **Repeated frames collapse** | An idle dryer repeats itself. Identical consecutive frames become one summary line that keeps the count and the timespan, so the cadence survives |
| ⏱️ **Better freeze estimate** | Measured across recent progress instead of a whole-run average, which kept reporting the early fast rate long after the machine stopped achieving it |
| 🩺 **Much better diagnostics** | Both halves of the conversation logged, `frames_bad`/`unknown` in the status line, a log that holds minutes instead of 45 seconds |

## What came back out

Several changes were made chasing a dryer that answered the identity query and
then went silent. That dryer was running `6.0.644170`, and the fault was in
**its firmware** — the genuine HarvestRight adapter could not talk to it either.
Everything added to work around it was solving nothing, and some of it cost
healthy machines:

- **The paced handshake.** Frames were spaced 250 ms apart to work around a
  receive path that was never the problem. That was three quarters of a second
  of startup on every dryer. The handshake is one burst again, as it was in
  1.0.0 — measured at 93 ms.
- **`UNIQUE lH`.** The argument was read out of the dryer's firmware and taken
  as required. It isn't: the genuine adapter was later captured sending
  `UNIQUE` bare.
- **Re-asking until answered.** Real adapter behaviour, but it exists to rescue
  a dryer that doesn't answer — and a dryer that doesn't answer is broken in a
  way no amount of retrying reaches.

Two tests now pin the reverted behaviour, because "pace the handshake" and
"keep asking" both look like good ideas and would otherwise get reinvented.

## What was kept from the investigation

Not all of it was wasted:

- **The Wi-Fi panel shows a connection.** Two `WIFIINFO` fields were hardcoded
  to zero for the life of the project, so the dryer's own screen showed your
  network and then nothing.
- **`STATUS`** — a verb the genuine adapter sends and we never did. It's a live
  telemetry request, so the first reading now arrives in about a second instead
  of waiting up to fifteen for the dryer to volunteer one.
- **The adapter identifies itself as `HR_<mac>`**, matching every genuine capture.
- **Real bugs found on the way**: the dryer-clock button had never worked, the
  raw-command box never sent its arguments, `SETBNAME` was dead for the same
  reason, and "Dryer serial" was showing the machine's *name*.

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

## Verified on hardware

Flashed to a live dryer on `6.0.641041`: handshake in 93 ms, `UID`, `SNM`,
`CFG` and telemetry all answered, capture rotation active, and every safety
guard still refusing — `REBOOT` unreachable from the command box, recipe verbs
refused on the generic path, and the reboot route declining without its
confirmation.
