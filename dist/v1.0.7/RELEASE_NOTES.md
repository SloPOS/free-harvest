# Free Harvest v1.0.7

**The logbook and the graph now survive a real run.**

Everything here came out of one 28-hour batch — roughly four pounds of unfrozen
banana on an Auto cycle — that finished correctly on the machine and left almost
no trace in the app. Four separate faults, each of which alone would have been
enough to lose the run.

> ## ⚠️ Check your dryer's firmware
>
> Free Harvest is developed and tested against dryer firmware **`6.0.641041`**.
> Settings → Diagnostics on the machine shows the build.
>
> **Do not run `6.0.644170`.** That build is broken. On a dryer running it,
> *nothing* can talk to the machine — not Free Harvest, and **not HarvestRight's
> own adapter either**.

---

## A finished batch left no logbook entry

The dryer counts Preparing and Starting on their own clock, then **restarts the
counter at zero** when the batch clock proper takes over at Freezing. On the run
this was found in: `0…892` preparing, `900…1834` starting, then `1` at Freezing.

The tracker read that restart the way it reads any backwards step in the elapsed
counter — as a new batch beginning. So it closed a 1834-second *interrupted*
record at the exact moment freezing started, and the 28-hour run that followed
became a second batch whose ending nothing was watching for. The logbook came
out of a complete run holding one entry, for the twenty minutes before it began.

The restart is now recognised for what it is and the run is rebased onto the new
origin rather than ended. Batch duration is a delta from where the run was first
seen, instead of the counter's absolute value — which, while the dryer sits
idle, is the *previous* run's total.

## The graph showed a flat line for the whole run

Two faults compounding.

**Empty buckets carried the last reading forward for ever.** A bucket with no
frames in it recorded a gap in the raw series but held the smoothed level
unchanged, "so the line stays continuous". Across a genuinely idle dryer that is
not smoothing but invention: a long idle spell before a run painted every point
at its last idle temperature. A gap is now bridged for two minutes — enough for
a missed frame or a USB re-enumeration — and reads as a gap after that.

**A full window stopped accepting data.** Once the 30-hour ring filled it kept
the oldest points and silently discarded everything after, so that a run
starting after an idle spell was recorded *not at all*. The window now drops its
oldest point and keeps recording. Losing the first half hour of a very long run
is much the smaller loss.

**And the graph is cleared by the phase, not the counter.** A new run is now
detected from the dryer moving out of idle, using the same definition of
"running" the logbook uses — one definition of "a batch is under way" in the
firmware instead of two that could disagree.

## Two buttons that were not on the machine

Final Dry offered **More Dry Time** and **Less Dry Time**. Neither is on that
screen — they belong to Batch Complete, and to the extra-dry phase that follows
it. Nothing in Final Dry telemetry marks a state where they would apply: across
a whole final-dry phase every field except temperature, vacuum, the two elapsed
counters and the progress percent is constant, so there was no gate to put them
behind. They are gone from the screen a run spends its last ten hours on. More
Dry Time remains where it belongs, on Batch Complete.

## Downloading the capture log could hand you a fragment

`/api/capture` served the small in-memory ring — the last few minutes — whenever
the flash log could not be read, and said nothing about having done so. A 1.6 MB
log downloaded as sixty lines of idle chatter and looked complete. It is now
impossible to miss: the fallback is labelled at the top of the file in terms
that cannot be read as anything else.

**A damaged segment no longer costs everything after it.** Flash wears, and
SPIFFS does not repair itself; a spot that returns EIO used to end the download
there. A log is exactly the file where the bytes *after* the damage are the ones
worth having, so the reader now steps over the bad region a page at a time and
carries on. On the machine this was found on, that recovered the end of the run.

**And failures say so.** A segment that reports bytes and will not open, or
reads short, now logs which segment and which errno instead of quietly serving
less than it has. The logbook reader does the same.

## Estimating the next run

Batch records now carry **per-phase durations** — freeze, dry and final dry,
each measured on the dryer's own counter — and the logbook shows them.

From those, the Logbook screen estimates the next run: the **median** of this
dryer's completed runs, not the mean, so one interrupted run or one left sitting
on Complete overnight does not skew every future estimate. Runs that did not
reach Complete are ignored entirely.

Until your machine has finished a run of its own it falls back to one measured
reference batch, and says so rather than presenting it as your dryer's history:

| phase | reference |
|---|---|
| freeze | 8.84 h |
| dry | 8.96 h |
| final dry | 10.60 h |
| **total** | **28.4 h** |

That is one batch of unfrozen fruit, not a population. A full load, prefrozen
food or a different recipe will all move it, which is why the estimate says what
it rests on and replaces it with your own history at the first opportunity.

## Recovering a corrupted partition

`POST /api/storage/format` reformats the capture partition — the recovery path
for a filesystem that reports free space and refuses every write. It wipes the
capture log, the trend and the logbook, then re-mounts and re-initialises both
stores so recording resumes without a reboot.

It has a companion fix: mounting a partition whose stored log will not fit the
budget used to **delete a whole segment**. It now lowers the per-segment ceiling
instead, all the way to a floor, and only drops recorded frames when the files
themselves overflow — because a ceiling costs future headroom and a deletion
costs the run somebody is about to download.

## The record format

Batch records are version 2, adding the three phase durations. Version 1 records
still decode, and are simply not counted toward an estimate — they carry no
phase times, and counting their zeros would drag every estimate toward zero
without ever looking wrong.

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
tracker: **one batch, no spurious records**, phases measured at 8.84 h / 8.96 h /
8.73 h against the same figures derived independently from the raw frames.

On hardware, on `6.0.641041`: the damaged-segment skip recovered the tail of
that run from a partition with a permanently unreadable region; the estimate
serves 28.39 h flagged as resting on a single reference batch; and 12 host test
suites pass, including new coverage pinning the preparation-countdown restart,
per-phase recording, the estimator's seed-then-learn behaviour, and the absence
of the two Final Dry buttons.
