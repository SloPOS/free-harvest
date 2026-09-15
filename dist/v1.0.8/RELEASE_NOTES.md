# Free Harvest v1.0.8

**One run, one logbook entry — even when the adapter loses power mid-batch.**

A real 26-hour batch produced three logbook records, none of them describing
what happened, and an estimate five hours short. This release fixes both, and
records the evidence needed to diagnose the next one from the log alone.

> ## ⚠️ Check your dryer's firmware
>
> Free Harvest is developed and tested against dryer firmware **`6.0.641041`**.
> Settings → Diagnostics on the machine shows the build.
>
> **Do not run `6.0.644170`.** That build is broken. On a dryer running it,
> *nothing* can talk to the machine — not Free Harvest, and **not HarvestRight's
> own adapter either**.

---

## The adapter restarts mid-run, and that is not a bug we can fix

The adapter is powered from the dryer's USB port. It has no battery and no
supply of its own, so when the dryer browns that rail out — most visibly when
it switches a heavy load — the adapter restarts and comes back inside a minute.
Nothing is visible from the front of the machine. It happened **twice inside one
26-hour run**, once as freezing began and once seconds after the Complete screen
appeared.

No firmware change prevents that. What the firmware can stop doing is turning
one physical run into several records.

**A run now resumes across a restart.** The in-progress record was already
checkpointed once a minute; what was missing was the tracker state to reopen it
with, so on boot the record was simply closed as *interrupted* and the next
running frame started a fresh batch. The checkpoint now carries the run's origin
on the dryer's clock, and on boot the record is **held** rather than closed —
until the dryer has been heard from and can say whether it is still running the
same batch. If it is, the record reopens and the totals continue.

Replayed against the capture from that run, this turns three records into one
correct entry.

**The counter is not perfectly monotonic**, either. That same run reported one
frame 1,031 seconds ahead and then came back where it had been. Requiring the
elapsed counter to move strictly forwards meant a single stray frame beside a
power cut cost the whole resume, so a bounded step backwards is now tolerated.
A genuinely new run is not mistaken for one: it restarts the counter near zero,
tens of thousands of seconds below where the last one left off.

## Final dry recorded 33 minutes of a 5¾-hour phase

Phase durations were recomputed on every frame as *now minus when the phase
started*. Anything that moved that origin late in a phase — a momentary frame on
another screen, re-entering Final Dry after More Dry Time — discarded everything
before it and reported the last stretch as the whole phase. Nothing about the
resulting number looks wrong.

One real run recorded **1,966 seconds of final dry against 20,707 actually spent
in it**, and the estimate built on it was five hours short.

Phase time is now **accumulated** a sample at a time, which is immune to that and
survives a restart as well.

**The guard against nonsense values had to be loosened considerably**, and this
is worth knowing if you are reading the code: the dryer's reporting rate is
wildly uneven between phases. Freezing and final dry arrive every 5–15 seconds,
but **drying has a median gap of 320 seconds and reaches 2,222**. A five-minute
cap looked entirely reasonable and silently discarded more than half of the
drying time. Time lost while the adapter is off is excluded by the resume path
instead, which is the right place for it.

## The log now says why it restarted

A restart used to show in the capture only as the timestamp jumping back to
zero, with nothing to say whether the adapter had crashed or simply lost power —
and the handshake that would confirm it is usually over before the capture
partition has finished mounting, so it is not in the file either. Establishing
that a restart was a power event, not a crash, meant querying a live device for
a reason that is gone the moment it restarts again.

Every mount now writes a first line:

```
~boot reset=poweron heap=91204 seg3 used=1626777
```

`poweron` and `brownout` mean the rail dropped. `panic` means a crash. That
distinction needs opposite responses, and it is now readable straight from the
log you send someone.

The same reading backs `/api/state`, so the two can never disagree.

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

## Verified

The full capture of the run described above was replayed through the fixed
tracker. Before: three records, final dry 0.55 h, phases summing five hours short
of the run. After: **one record**, freeze 6.54 h, dry 10.98 h, final dry 5.75 h —
each matching the figures derived independently from the raw frames — with both
outages resumed, one across a 49-second gap and one across the backwards jump.

12 host test suites pass, including new coverage pinning phase accumulation
across a mid-phase excursion, the loosened nonsense bound, and a run resuming
across power loss with its duration still measured from the original start.
