# Batch logbook — design

**Status:** approved 2026-08-22
**Target:** the first of the Free Harvest 1.0 features

## Why

Free Harvest records a live trend and stores recipes with notes, but keeps
nothing once a batch ends. Three things follow from that gap:

- **Recipe tuning is guesswork.** The adapter already notices when drying had
  to be extended, but nothing remembers that it happened last time too.
- **Machine health is invisible.** A pump losing suction or a perishing door
  seal shows up as gradually worse pull-down over months. Nobody sees a trend
  that is never written down.
- **There is no record of what was made.** Jars go in a cupboard with no date.

One compact record per completed batch addresses all three, and is also the
training data the time-remaining estimator will need — which is why this is
first.

## Constraints that shaped it

Measured on the device, not assumed:

| resource | state | consequence |
|---|---|---|
| App flash | 731 KB free per OTA slot | Not a constraint |
| SPIFFS | 3 MB, **shared with the capture log** | Needs a reserve; see below |
| NVS | **24 KB total**, shared with WiFi/MQTT/recipes | Only the single in-progress record lives here |
| RAM | **No PSRAM.** 512 KB SRAM, most spoken for | Nothing may buffer; compute live, write once |

The SPIFFS partition has already corrupted once, and the OTA reboot still
reports `reset_reason: panic`. Both argue for the most failure-tolerant
storage shape available rather than the most compact one.

## Design

### Storage: append-only CSV

Batch records go in `/cap/batches.0.csv` and `/cap/batches.1.csv`, one line per batch,
appended and never rewritten.

**Append-only is the point.** The trend file uses a whole-file `"wb"` rewrite;
a torn write there loses the entire history. A torn append loses one record.
Given a partition that has already lost data once, that difference decides it.

Each line carries a trailing checksum over the rest of the line. A partial or
corrupt line fails its checksum and is skipped on read, rather than being
parsed into a plausible-looking wrong batch.

CSV rather than a packed binary record costs roughly 60 bytes per batch and
buys two things: the file is readable when something goes wrong, and **export
is free** — "download as spreadsheet" serves the file directly instead of
needing a second encoder that can disagree with the first.

### Partition sharing

The capture log may grow to fill all 3 MB, which would leave no room for batch
records. Reserve **256 KB** for the logbook — about 1,700 batches at ~150 bytes
— and cap the capture log at the remainder.

This conflict only appears after months of use, which is exactly why it is
worth fixing before anyone hits it.

When the reserve fills, the logbook **rotates between two segment files** of
128 KB each. Writes go to the active segment; when it fills, the inactive one is
deleted and becomes the new active segment. Reads concatenate both, oldest
first.

Rotation rather than trimming the oldest lines in place, because trimming a file
means rewriting it — precisely the operation append-only exists to avoid. Two
segments preserve the append-only property while bounding total size, at the
cost of dropping up to half the history at each rollover. A logbook that forgets
the distant past is better than one that silently stops recording, and far
better than one that rewrites itself and loses everything to a torn write.

### Batch boundary detection

Reuse the rule already in `main.c` rather than inventing a second definition:
**`elapsed_s` going backwards means a new run.** A second, disagreeing notion of
"a batch" in the same firmware would be a reliable source of bugs.

Combine it with phase, because a comment in the UI already warns that the dryer
keeps the previous batch's `elapsed` while idle, so `elapsed > 0` does not mean
running:

- **Start** — phase leaves idle (screen 1 → 17 or 2)
- **End** — phase reaches Complete (7), or returns to idle from a running phase

The in-progress batch is a single record in NVS, updated as the run proceeds
and committed to the CSV when it ends. One record, not a growing list, so NVS
usage stays flat.

On reboot mid-batch: if `elapsed_s` is still monotonic the batch continues; if
not, the open record is closed with an `interrupted` outcome. Interrupted runs
are recorded rather than discarded — a batch that died at hour 18 is worth
knowing about when tuning a recipe.

### Record contents

| purpose | fields |
|---|---|
| What's in the jars | start date/time, batch name, duration |
| Recipe tuning | recipe family, all params as sent, extra dry time added |
| Machine health | min temperature reached, deepest vacuum, pull-down time to 500 µm |
| Integrity | outcome (completed / ended early / interrupted), checksum |

Health metrics are accumulated live into a small RAM struct as telemetry
arrives and written once at the end. Nothing re-reads the trend and nothing
buffers a series.

### Clock

The adapter currently has no clock at all. `SETDATE` sends the *browser's* time
to the *dryer*; the ESP never learns the date.

Add `POST /api/time`. The web app posts the browser clock on load, mirroring
what `SETDATE` already does for the dryer. No SNTP, so the "nothing phones
home" property holds and isolated networks still work. Drift between visits is
irrelevant at logbook resolution.

Records written before the clock has ever been set are marked **date unknown**
rather than given a fabricated 1970 timestamp.

## Components

**`components/hr_protocol/hr_batchlog.[ch]`** — portable and host-tested, like
the rest of the protocol core:

- `hr_batch_t` and encode/decode to a CSV line, with checksum
- `hr_batch_tracker_t` — fed telemetry, accumulates health metrics, reports
  batch start and end
- No filesystem or network dependencies, so it is testable without hardware

**`main/hr_batchstore.[ch]`** — the device-side half: SPIFFS append, the 256 KB
reserve, segment rotation, and the NVS in-progress record.

**`main/hr_http.c`** — `GET /api/batches` (streamed, never buffered),
`GET /api/batches.csv` (both segments concatenated), `POST /api/time`.

**`main/www/index.html`** — a Logbook card in Settings, and the browser clock
post on load.

## Testing

Host tests in `test/test_hr_batchlog.c`:

- Record encode → decode round-trip preserves every field
- A truncated line fails its checksum and is skipped, not parsed
- Batch boundary detected across a full phase sequence (1 → 17 → 2 → 4 → 5 → 6 → 7 → 1)
- `elapsed` going backwards mid-run closes the batch as interrupted
- Idle `elapsed > 0` does not start a batch — the specific trap the UI comment warns about
- Health metrics track the minimum and the deepest vacuum, not the last value
- A full segment rotates rather than refusing new records, and reads still
  return the surviving history in order

Device-side verification: write records, reboot, confirm they survive and the
in-progress record resumes.

## Out of scope

The estimator that will consume this data, pump-health alerting, and MQTT
notification of batch completion. Each is its own spec.
