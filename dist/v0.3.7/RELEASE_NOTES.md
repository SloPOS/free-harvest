# Free Harvest v0.3.7

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

---

Update over Wi-Fi: Settings -> Firmware update, upload hr_wifi_adapter.bin.
