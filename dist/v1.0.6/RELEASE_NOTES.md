# Free Harvest v1.0.6

**Everything since 1.0.0, in one release.**

The 1.0.1–1.0.5 releases have been withdrawn. Each was a narrow fix shipped
while diagnosing one user's dryer, and 1.0.6 contains all of them — so there is
nothing between 1.0.0 and here worth installing separately. Their tags remain in
the repository if you want to read the history.

> ## ⚠️ Check your dryer's firmware
>
> Free Harvest is developed and tested against dryer firmware **`6.0.641041`**.
> Settings → Diagnostics on the machine shows the build.
>
> **Do not run `6.0.644170`.** That build is broken. On a dryer running it,
> *nothing* can talk to the machine — not Free Harvest, and **not HarvestRight's
> own adapter either**. A working dryer was updated to it, both adapters went
> silent, and reverting to the build on harvestright.com brought both back.

---

## If your dryer connected but never showed readings

Three separate faults caused that, all fixed here.

**The adapter answered the wrong question.** The dryer asks `REQINFO`; we replied
with `GOTIT`, whose payload this project had invented. `GOTIT` is the *dryer's*
acknowledgement of a button press — we were answering with someone else's answer.
One firmware tolerated it and sent telemetry anyway; another re-asked every two
seconds forever. It now answers with `WIFIINFO`, matching the genuine adapter.

**The adapter never introduced itself.** It only ever replied — it initiated
nothing. The genuine adapter opens every connection by identifying itself and
asking the machine who it is. Until this was added, `serial` and `uid` had been
empty for the entire life of the project, because nothing had ever asked.

**The dryer's Wi-Fi panel never showed a connection.** Two `WIFIINFO` fields were
hardcoded to zero because nothing ever wrote them, so the machine's own screen
displayed your network and, below it, nothing. They now follow your actual
connection.

## If the setup hotspot was hard to reach

**Four HTTP routes were silently dead**, including the captive-portal endpoints a
phone uses to find the setup page. `max_uri_handlers` was set below the number of
registered routes, and anything past the limit failed to register and returned
404 forever — with no error anywhere. The limit is raised, and a failed
registration is now logged loudly instead of vanishing.

## New in this release

| | |
|---|---|
| 🔄 **Reboot the dryer** | Settings → Debug & advanced, behind a confirmation. For a locked-up panel |
| 🏷️ **Name a batch when you start it** | Optional field on every start confirmation, instead of only in Settings |
| ✅ **Every recipe send confirms** | Two paths reached the dryer with no prompt at all — both replace the machine's configured recipe |
| ♻️ **The capture log rotates** | It used to *stop* when the partition filled, keeping the oldest hours and discarding everything since. Four segments now; recording never stops |
| 🗜️ **Repeated frames collapse** | An idle dryer repeats itself. Identical consecutive frames become one summary line that keeps the count and timespan, so the cadence survives |
| ⏱️ **Better freeze estimate** | Measured across recent progress instead of a whole-run average, which kept reporting the early fast rate long after the machine stopped achieving it |
| ⚡ **Telemetry arrives sooner** | The handshake now asks for a reading, so the first one lands in about a second instead of waiting up to fifteen for the dryer to volunteer it |

## Things that had never worked

Found while diagnosing something else, and all fixed here:

- **The "set dryer clock" button.** It returned *"Refused: not allowed"* on every
  version that shipped it — the command was rejected before reaching the dryer.
- **Arguments in the raw command box.** They were packed into the wrong field and
  silently discarded, which also meant **`SETBNAME` had never worked**.
- **"Dryer serial" was showing the machine's name.** The real serial is now read
  from the dryer and shown beside it.
- **A recipe sent from MQTT or the raw command box could become a *different*
  recipe.** The generic field builder reshaped it into something still valid — so
  it reported success while setting temperatures nobody chose. Those verbs are now
  refused there; the recipe editor was never affected.

## Diagnostics

If you ever need to work out why a dryer is not being seen, this release is a
different experience from 1.0.0:

- **Both halves of the conversation are logged.** Only received frames used to
  appear, so a log showed the dryer asking and nothing apparently answering —
  indistinguishable from a firmware that never replies, even while it was
  replying in about a millisecond.
- **The log holds minutes, not 45 seconds.** The real limit was a response buffer,
  not the log size, so the log is now streamed.
- **`frames_bad` and `unknown_verbs` in the status line.** Without them a log
  cannot distinguish "nothing arrived" from "something arrived and we rejected
  it" — the distinction that ended a two-day hunt.
- **Free heap in the status line**, so memory headroom is something you read
  rather than assume.

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

## What was tried and taken back out

Between 26 and 27 August, several changes were made chasing a dryer that answered
the identity query and then went silent. That dryer was running `6.0.644170`, and
the fault was in **its firmware** — the genuine HarvestRight adapter could not
talk to it either. Everything added to work around it was solving nothing, and
some of it slowed down dryers that were working fine:

- **A paced handshake**, spacing frames 250 ms apart — three quarters of a second
  of startup on every dryer, to work around a receive path that was never the
  problem. It is one burst again, measured at 93 ms.
- **An extra argument on the identity query**, read out of the dryer's firmware
  and taken as required. It is not: the genuine adapter was later captured
  sending the bare form.
- **Re-asking until answered.** Genuine adapter behaviour, but it exists to
  rescue a dryer that does not answer — and one that does not answer is broken in
  a way no retry reaches.

Two tests now pin the reverted behaviour, because both ideas look sensible and
would otherwise get reinvented.

## Verified on hardware

Flashed to a live dryer on `6.0.641041`: handshake in 93 ms, identity and
configuration all answered, telemetry flowing, capture rotation active, and every
safety guard still refusing — `REBOOT` unreachable from the command box, recipe
verbs refused on the generic path, and the reboot route declining without its
confirmation.
