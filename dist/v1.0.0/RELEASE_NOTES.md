# Free Harvest v1.0.0

The adapter now **remembers** what it did, and can be locked down.

### Batch logbook

Every completed run is recorded on the adapter: name, date, duration, coldest
temperature, deepest vacuum, how long the pump took to pull down, and how much
extra drying was needed. Downloadable as CSV.

It stores extremes rather than last readings, because "how cold did it actually
get" is the question worth answering. Runs ended early or interrupted by a power
cut are recorded too and flagged as such - a batch that died at hour 18 is worth
knowing about when tuning a recipe.

Storage is append-only CSV in two rotating segments. The trend file uses a
whole-file rewrite and that partition has already corrupted once; a torn append
costs one record where a torn rewrite costs everything. Each line carries a
checksum, and a line that fails it is skipped rather than parsed into a
plausible-looking wrong batch.

### The adapter learns the date

It has no clock and deliberately does not use SNTP, so it takes the time from
your browser when you open the app - the same arrangement `SETDATE` already uses
for the dryer. Nothing phones home, and isolated networks still work. Records
written before that ever happens say "date not recorded" rather than claiming
1970.

### Control PIN

Optional, and gates every endpoint that can change the machine - control,
recipes, raw commands, firmware updates. Monitoring stays completely open. Five
wrong attempts lock control for a minute.

Not a login system, and the UI says so. The threat it addresses is a housemate
tapping Start, not a determined attacker. **There is no recovery** - a forgotten
PIN means reflashing.

### Storage recovery

`POST /api/storage/format`, and a button in Settings. SPIFFS can reach a state
where it reports free space and refuses every write while reads keep working -
which this device did, silently, during testing. Without a way back, a device in
that state never records anything again and looks fine doing it.

### Fixed

- **SPIFFS descriptors** raised from 6 to 12. The logbook's streaming reader
  holds one for a whole response, and two browsers plus the frame writer
  exhausted the pool - surfacing as EIO on the trend save.
- **Request fan-out** on page load reduced. Logbook and PIN data now load when
  Settings is opened rather than on every dashboard poll; the extra requests
  were enough to lock a second device out.
- **Unnamed batch records.** The name field was designed and nothing filled it.

### Known gaps

Alerts and a learning time estimator are specified in
`docs/superpowers/specs/` but deferred to 1.1. The estimator cannot honestly be
validated without a body of real batches, which the logbook has only just
started collecting.

`reset_reason` still reports `panic` after an OTA reboot where a clean restart
should say `sw`. The image boots correctly and the filesystem is unmounted
first, but the cause is unfound.

---

Update over Wi-Fi: Settings -> Firmware update, upload hr_wifi_adapter.bin.

Fresh install needs all four files - see the README. Omitting
ota_data_initial.bin boots the old image and looks like a failed flash.
