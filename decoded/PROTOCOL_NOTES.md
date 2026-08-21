# HarvestRight ↔ WiFi Adapter Protocol — Static-Analysis Notes

> ## ⚠️⚠️ REMOTE CONTROL EXISTS: `ADV` (2026-08-21)
>
> **Every earlier statement in this file that remote cycle control is impossible
> was WRONG.** It was based on no START/CONTINUE/CANCEL verb existing in the
> command table. That fact is true and the inference from it was not: control is
> screen-relative, so there is no per-function verb - there is a verb that
> presses whatever the screen currently offers.
>
> Probed against an idle machine:
>
> ```
> ADV   ->  stat_type 1 -> 2   Ready -> Starting batch   reply: NTFY,2,0,,0,
> ```
>
> `ADV` = advance. It acts as a press on the panel's current option. This is the
> mechanism behind "mirror every screen function": read the screen from STAT,
> press with ADV.
>
> **`ADV` MUST NOT be exposed casually.** It can start a cycle. It belongs behind
> an explicit confirmation, never on a dashboard button.
>
> In this test the machine reached "Starting batch", did not energise the
> compressor, and returned to Ready on its own. The dryer also rebooted once
> during the sweep; the cause is not isolated.
>
> ### WHAT ADV ACTUALLY DOES: it SKIPS, it does not press
>
> Observed on the machine: `ADV` from Ready does not press START and run the
> normal flow. It jumps straight to the **"load trays"** step, skipping the
> pre-freeze entirely. The dryer then believes it is mid-batch without ever
> having chilled, so pressing CONTINUE has nothing coherent to do and the
> machine hangs. The compressor never engages at any point.
>
> So ADV = advance the state machine, NOT press the highlighted button. It
> bypasses the sequence rather than following it, which is why the resulting
> state is unrecoverable without a power cycle.
>
> The argument is ignored: bare `ADV`, `ADV START` and `ADV START Auto` all
> land in the same place.
>
> **This is a dead end for starting a cycle, and an actively harmful one.**
> A correct start almost certainly has to establish batch state first - the
> `SENDBATCH` / `SENDCANDY` / `SENDCUSTOM` family is the obvious candidate,
> since those push a batch definition rather than skipping the machine into
> one. Sent bare they do nothing, so they need a payload we do not have.

> ### ADV CAN STRAND THE MACHINE (2026-08-21)
>
> After `ADV` reached "Starting batch" (type 2), the dryer became stuck there
> and needed a **hard power reset** to recover. The compressor never engaged,
> so nothing was damaged - but a screen-press verb can leave the machine in a
> state it will not leave on its own.
>
> This is the strongest argument for keeping `ADV` behind a confirmation and
> out of any allow-list reachable from the network.

> ### STAT layout varies by type - do not decode unconfirmed types
>
> The mode string is at index **9** for types 4-7 and 17, but index **11** for
> type 1. Our decoder used to fall back to the type-1 layout for anything
> unrecognised, which a panel walk exposed as nonsense:
>
> | screen | reported "mode" | reality |
> |---|---|---|
> | type 2 (starting batch) | `5` | field [11] means something else |
> | type 31 (recipe settings) | `-15` | likely a temperature setting |
> | type 15 (diagnostics) | `5` | unknown |
>
> Confirmed layouts: **1, 4, 5, 6, 7, 17**. Everything else now decodes only
> the shared header, and reports an empty mode rather than a misleading
> number.
>
> Note also that `phase=0 / type 0` is NOT a screen - it is the adapter's own
> "no telemetry" state during a link drop or dryer reboot.

> ### Replies captured from a real machine
>
> | Command | Reply |
> |---|---|
> | `REQCFG` | `CFG,37C9-355A-3037,HM-4B~04,PSTF231103561BKC,` |
> | `FDNAME` | `SNM,Freezie McDry,` |
> | `REQSYSINF` | `SYSINF,2026-08-20_18.34,30,635,158,4` |
> | `REQPREF` | `SYSPREF,0,18000,90,14400,13,` |
> | `REQTSUM` | `TESTSUM,0,0,0,0,0,0,0,0,0, ,` |
> | `REQTHST` | `TESTHST,0,0;` |
> | `FDFILES` | `FDFILELIST,FactoryTest.log,0,596` |
> | `FILEREAD` | `FDFILEBLOCK,,0,0,0,00` |
>
> **`REQCFG` carries the serial number**: `PSTF231103561BKC` is the data-plate
> value `P-STF 2311-03561 BKC` with separators stripped. We spent effort reading
> it off the machine; one command would have done it. `HM-4B~04` looks like a
> model code and `37C9-355A-3037` like a device ID.
>
> **`FDNAME` is answered with `SNM,<name>,`** - the friendly name, here
> "Freezie McDry". That settles the question the stock-adapter work left open,
> and confirms the SNM shape guessed there.
>
> **`REQPREF` holds the dry-time settings**: 18000s = 5h and 14400s = 4h.
>
> ### Verbs that did nothing
>
> `ECHO PRINT DIR DIRC DUMP MEMSIZE SERIAL STATE STATUS GOTIT ADD REQSTAT
> REQBATSUM REQSCIENCE SENDBATCH SENDCANDY SENDCUSTOM SETDATE SETBNAME XWIFI
> CLICK` produced no reply and no state change when sent without arguments. Most
> of the SET/SEND family presumably need a payload.
>
> ### Not probed, deliberately
>
> `DEL RMOLD COPY FDRENAME` (destroy files), `SETSN` (overwrites the serial),
> `DUTY HCS SPC` (energise heater/shelf/pump), `MEMTEST FUZZY` (may block the
> firmware), `REBOOT`.



> ## ⚠️ Corrections from a genuine adapter (2026-08-20)
>
> A stock HarvestRight adapter was interrogated over its OTG port. Three things
> below were **wrong**, and they were wrong in ways that mattered.
>
> ### 1. The framing is ASYMMETRIC
>
> This document said "ASCII, comma-delimited, CR-terminated" for both
> directions. Only the dryer→adapter direction is comma-delimited. The
> adapter→dryer direction is **space-delimited**:
>
> ```
> STATE 1 0
> UNIQUE lH
> FDNAME
> REQCFG
> WIFIINFO 1 0 "" 0 HR_3cdc75f95aac 0 0 161
> ```
>
> If the dryer tokenised on commas, `STATE 1 0` could never match the verb
> `STATE`, so its inbound parser must split on whitespace. **Every
> comma-delimited command we sent therefore arrived as one unrecognised token**
> and was discarded as "Command not found". Fixed in hr_protocol.c.
>
> ### 2. `GOTIT` is not the session gate
>
> This document called the GOTIT payload "the session gate" and our firmware
> answered `REQINFO` with it. The real adapter answers `REQINFO` with
> **`WIFIINFO`** (0.2 s later, unmistakably a reply). GOTIT was a guess built on
> a format string, not on observed behaviour.
>
> ### 3. `UNIQUE` is live
>
> Previously recorded as referenced by the dispatcher but with no handler. The
> real adapter sends `UNIQUE lH` unprompted at startup, so it is in active use.
>
> ### The adapter is a COMPOSITE device (CDC + Mass Storage)
>
> Not a correction so much as a large omission. `VID 0x303A / PID 0x4003`, two
> interfaces:
>
> | Interface | Class | Descriptor string |
> |---|---|---|
> | MI_00 | `02/02/00` CDC-ACM | `HarvestRightCDC Device` |
> | MI_02 | `08/06/50` Mass Storage, SCSI bulk-only | `HarvestRightMSC Device` |
>
> Ours presents CDC only, as `0x303A/0x4001`. This finally explains the dryer
> firmware strings that never fit: *"USB thumb drive timed out, continuing to
> CDC"*, *"Waiting on USB MSC Init thread"*, *"Error in initializing FAT on
> USB_HMSC"*. The mass-storage volume is how file transfer works — `FDFILELIST`,
> `FDFILEBLOCK`, `HRWiFi.txt`, `version.txt`, the `1:/esp` paths.
>
> The dryer probes MSC *first* and waits for it to time out before starting CDC,
> which is why the link takes as long as it does to come up.
>
> ### Screen mirroring CONFIRMED by observation (2026-08-21)
>
> With the stock adapter linked and a batch started, the user reports the app
> offers **only the options currently on the dryer's screen**, and acting on
> one behaves exactly as though a human pressed the panel. So control is
> screen-relative, not a per-function verb - which is why no START verb exists
> and why looking for one was the wrong search.
>
> **GETP / GETR are not it.** Tried bare and with page/row arguments (0, 1,
> "1,1") against an idle machine: no reply beyond routine STAT/REQINFO, and no
> crash. They are safe but inert for this purpose.
>
> The mechanism is therefore still unidentified. Remaining candidates: ADV
> (deliberately untested - it means "advance" and would ACT), STATE with
> arguments, or something carried over the mass-storage channel rather than
> the serial link.

> ### Prep-phase frame decoded (2026-08-21)
>
> From STAT type 17 captured during a real batch start:
>
> ```
> STAT,17,0,0,0,64,10000,36,0,42,Auto,1,1,0,0,5,0,864,0,
>                          ^f6                    ^f16
> ```
>
> **f6 + f16 = 900 in every frame** - f6 counts up, f16 counts down, and they
> sum to the 15-minute pre-cool exactly. f11 steps 1 -> 5 -> 10, a progress
> percentage. Our existing mapping already reads f16 as prep_remaining_s and
> f11 as phase_pct, so this confirms the decode rather than correcting it.

> ### CONFIRMED: commands reach the dryer (2026-08-21)
>
> Sending `BEEP` with the corrected space-delimited framing produced a reply
> from the real machine:
>
> ```
> Thanks for Beeping!
> ```
>
> That is the first command this project has ever landed. It also confirms the
> asymmetric-framing finding end to end: comma-delimited commands were being
> discarded silently for the entire life of the firmware, and nothing in the UI
> could have shown it, because the dryer does not NAK - it just ignores.
>
> Note the reply is a bare human-readable string with no comma, so our parser
> records the whole line as a "verb". Harmless (19 chars against a 24-byte
> field) but worth knowing when reading the verb table.
>
> **Still unanswered:** `SERIAL`, `REQSYSINF`, `REQCFG`, `REQPREF`, `REQBATSUM`
> and `STATE` produced no reply at all, and some of them REBOOT our adapter.
> The adapter is otherwise rock stable - five clean uptime samples over 80s
> with no traffic - so the crash is triggered by sending, and it is our bug.
> The dryer also sets line coding to **9600 baud**, which no PC capture showed.

> ### Live probing results (2026-08-21)
>
> Driving the adapter with dryer-side frames settled several more points.
>
> **Its inbound parser is COMMA-delimited.** Replying `CFG,1,1,0,0,Auto,v6.4`
> made it *stop asking* `REQCFG` — 3 requests in 45 s before, 0 after. The same
> reply space-delimited caused it to **reset** (USB dropped, then `REQCFG`
> resumed). So the asymmetry is real and strict in both directions:
> dryer→adapter commas, adapter→dryer spaces.
>
> **Empty fields are quoted.** `WIFIINFO 1 0 "" 0 HR_... 0 0 347` — with a space
> separator an unquoted empty field would collapse into the whitespace and shift
> every later field by one. hr_build_str() now emits `""`.
>
> **It polls on a ~15 s cycle**, repeating `STATE 1 0` / `UNIQUE lH` / `FDNAME` /
> `REQCFG` until answered. It is reactive otherwise — silent unless spoken to.
>
> **`STATUS`** is sent once when the host asserts DTR. Not previously observed.
>
> **`UNIQUE lH`** is stable across power cycles, so it is an identifier rather
> than a nonce or challenge.
>
> **`WIFIINFO`'s last field is a counter**, not a channel: 161 in one session,
> 347 in another. Consistent with seconds since boot.
>
> **`FDNAME,<name>` was NOT accepted** — it kept asking. The reply shape for that
> one is still unknown.
>
> ### The MSC volume carries the update payload
>
> Its filesystem holds `esp/version.txt` plus the dryer firmware images
> (`.h6r`, `.b6r`, `.hfs`, `.hft`). So mass storage is how updates physically
> reach the machine.
>
> `esp/version.txt` reads `HR_3cdc75f95aac,v1.1.2,1,0/0` — AP name (matching the
> `WIFIINFO` field), adapter firmware version, then two unknown fields.
> Comma-delimited, like everything the dryer reads.



Derived from the decoded main-app firmware (`G0641041_mainapp.bin`, Renesas RA / FSP 5.7.0 / FreeRTOS).
Everything here is from firmware strings + string-table layout. **Field meanings marked "(guess)" need sniffer confirmation.**

## Transport
- Adapter = **USB device**, dryer = **USB host**, enumerated as **USB-CDC ACM** (virtual serial).
- No VID/PID allowlist and no crypto/identity check found — dryer accepts any CDC-ACM device.
- Baud is nominal: dryer reads `USB_CDC_GET_LINE_CODING` but CDC transfers ignore real baud. For your Pico sniffer, tap the **USB D+/D- CDC bulk endpoints**, not a UART.
- Framing: **ASCII, comma-delimited, `\r` (CR) terminated.** Firmware explicitly warns: "command does not have a terminating CR as required." Every frame must end in CR.
- RX handler in firmware: `ProcessIncomingSerialStream`. TX uses FreeRTOS `xMessageBufferSend`.

## Direction A — Dryer → Adapter (outbound frames, printf format strings)
These are emitted by the dryer with live data. `%d`=int, `%ld`=long, `%s`=string, `%lX`=hex long, `%u`=unsigned.

| Frame | Format | Purpose (guess) |
|---|---|---|
| `UID`  | `UID,%lX-%lX-%lX-%lX,%d,%d.%d.%ld,%d,%d,%d,%d,%d,%d,%d,` | Identity/discovery: 128-bit MCU unique ID, then version (maj.min.build) + config flags |
| `STAT` | `STAT,%d,%s,%d,%d,%ld,%ld,%d,%s,%s,` (also 8-field variant) | Periodic machine status (state, temps, vacuum, times) |
| `NTFY` | `NTFY,%d,%ld,%s,%d,` | Event/notification |
| `SNM`  | `SNM,%s,` | Serial number |
| `CFG`  | `CFG,%s` | Config blob |
| `SYSINF` | `SYSINF,%s` | System info |
| `SYSPREF`| `SYSPREF,%s` | User preferences |
| `BATSUM` | `BATSUM,%d,%s` | Batch summary |
| `TESTSUM`/`TESTHST` | `TESTSUM,%s` / `TESTHST,%s` | Test summary / history |
| `LIM`  | `LIM,%d,%d,%d,%d,%d,%d,%u,%d,%d,` | Limits/setpoints (also used for shelf boards) |
| `SCIRCP` | `SCIRCP,%d,%s,%s,%d,` | Recipe transfer |
| `FDFILELIST` | `FDFILELIST,%s,%d,%ld` | File list entry (name, index, size) |
| `FDFILEBLOCK`| `FDFILEBLOCK,%s,%d,%d,%ld,` | File data block (name, block#, ?, offset) |
| `FDEXT` | `FDEXT,%d` | File extension/type |
| `REQINFO` | `REQINFO,` / `REQINFO,%d,` | Dryer asking adapter to report info |
| `GOTIT` | `GOTIT,%s,` | Ack |
| `WRST` | `WRST,` | WiFi reset (guess) |
| `DIS,1,` | literal | Disconnect/discovery marker |
| `KHNK,` | literal | (unknown — possibly obfuscated/"KHNK"=knock handshake, guess) |

## Direction B — Adapter/App → Dryer (inbound command keywords)
Parsed by the dispatch table near the `"Command not found = %s len=%d"` error string. These are the commands **your DIY adapter would send to the dryer**:

Handshake/status: `GOTIT` `REQSTAT` `REQSYSINF` `REQBATSUM` `REQCFG` `REQTSUM` `REQTHST` `REQPREF` `STATUS` `STATE`
Setters: `SETSN` `SETDATE` `SETPREF` `SETBNAME` `FDRENAME` `FDNAME`
Data/recipes: `SENDBATCH` `SENDCANDY` `SENDCUSTOM` `SENDSCIENCE` `REQSCIENCE`
Files: `FDFILES` `FILEREAD` `DIR` `DIRC` `DEL` `COPY` `DUMP` `RMOLD`
WiFi/adapter: `WIFIINFO` `XWIFI` `XW` `SERIAL` `ADV` `ADD` `UNIQUE`
Diagnostics/toys: `ECHO` `BEEP` `CLICK` `PRINT` `MEMTEST` `MEMSIZE` `DUTY` `FUZZY` `ADC` `VAC` `HCS` `SPC` `REBOOT`
Firmware channels: `HRW` `HRC` `HRH` `HRF` `HRV` (match update files HRWiFi/HRCode/etc.)

**Best first-contact tests once hardware arrives:** `ECHO` (echo-back), `BEEP` / `CLICK` (audible confirmation the dryer received & parsed your frame), `REQSTAT` (should trigger a `STAT` reply). These need no valid session state.

## Suggested capture plan (when adapter + sniffer arrive)
1. Log the **connect sequence** first — expect the dryer to send `UID`/`REQINFO` and the adapter to answer. Capture the exact `GOTIT` reply contents; that's the session gate.
2. Log a full `STAT` frame with the machine idle → decode each `%d/%s` field against a known screen reading.
3. Send `ECHO`/`BEEP` from a DIY CDC device to prove the RX path before implementing real logic.
4. Diff every observed frame against the tables above; fill in the "(guess)" fields.

---

# Live-Capture Decode (2026-07-30, batch-start capture)

Confirmed against real traffic + user annotations (idle → batch prep → run).

## STAT is a MULTIPLEXED frame — field 1 is a TYPE discriminator
Field 1 selects the layout AND field count. Types seen:

| type | fields | meaning |
|---|---|---|
| 1  | 16 | idle / normal running status (most common) |
| 2  | 22 | vacuum-test / transitional |
| 15 | 34 | **full diagnostics snapshot** (carries version + zc/bl/ht codes) |
| 17 | 20 | **15-minute batch-prep countdown** |
| 31 | 24 | recipe/profile parameters (CUSTOM shown) |

## Shared header (fields 0-9, same across types)
- `[0]` `STAT`
- `[1]` **TYPE discriminator** (see table)
- `[2] [3] [4]` state/sub-state codes (mostly 0; `[2]` was 10 briefly at mode change)
- `[5]` **TEMPERATURE °F** ✓ (68-70 = room; user-confirmed "field 4 = temperature")
- `[6]` **PRESSURE, raw sensor counts** ✓ (user-confirmed "field 5 = pressure"). Idle ~120k-155k; forced to exactly `10000` during prep/vac-test.
- `[7]` **total batch elapsed seconds** (0 when idle; climbs 0→193→265… once a batch starts, persists across type changes)
- `[8]` 0 (unknown)
- `[9]` a second temperature/state (38 idle type1; 42/43/52 during prep/diag)

## TYPE 17 — batch prep countdown (CONFIRMED math)
`STAT,17,0,0,0,TEMP,10000,ELAPSED,0,42,MODE,1,PROG,0,0,5,0,REMAIN,0,`
- `[6]` prep elapsed seconds (0→…, counts UP)
- `[16]` prep seconds **remaining** (900→0, counts DOWN)
- **`[6] + [16] = 900` always = 15:00.** This is the "prepare the dryer" timer the user identified.
- `[10]` = MODE string ("Auto"); `[12]` steps 1→5→10→15 (prep progress %/stage).

## NTFY = state-transition notifications (user theory CONFIRMED)
`NTFY,<type>,0,<mode>,<n>,` — the `<type>` matches the STAT type it precedes
(1→17→2…). Fires at the touch action that causes the transition, which is why
they appear when interacting with the screen. NTFY are the dryer ANNOUNCING a
mode change, not commands into it.

## SYSINF — human-readable status
`SYSINF,2026/02/24 08:02,26,550,91,4`
- `[1]` RTC date/time (real clock)
- `[2]` 26 (temp/count) · `[3]` 550 (**likely vacuum in mTorr**) · `[4]` 91 (percent?) · `[5]` 4 (state)

## TYPE 15 — diagnostics snapshot (richest)
Carries `[29]` full version `v6.4.0.641041` and `[30]` `zc:OK  bl:5  ht:B`.
Firmware format (Diagnostics/Test-Shelf screen): **`zc:%s  bl:%d  ht:%s`**
= zero-cal status / bl level / heater state. Fields `[16][17][18]` = extra
temperatures (shelf/condenser, values 66/69/70). `[10]`=8256, `[13]`=8100 look
like raw ADC thresholds. Engineering units nearby in firmware: `mT` (milliTorr).

## KNOWN LIMITATION
Dryer's **diagnostic mode drops the adapter link** (user observed) — so
individual relay/heater/pump toggles can't be captured that way. The per-output
codes (J17-J20 thermocouples, DC on/off, DutyCycle) are visible as firmware
strings but not yet correlated to live values.

---

# Freezing phase discovered (2026-08-11 capture)

## STAT type 4 = FREEZING
Occurs after the user loads trays and presses CONTINUE, before vacuum is pulled.
Confirmed over ~13 minutes of capture at batch-elapsed 6158-6910s (~1.7-1.9 h in).

`STAT,4,0,0,0,TEMP,10000,ELAPSED,0,45,MODE,1,PCT,0,0,5,0,0,,`

- `[4]` **temperature °F** — observed falling 5 → 4 → 3 °F (deep freeze)
- `[5]` pressure pinned at the `10000` placeholder — **vacuum not yet pulled**,
  consistent with freeze-before-vacuum
- `[6]` batch elapsed seconds (advancing normally)
- `[9]` mode string ("Auto") — same slot as the type-17 prep layout
- `[11]` **freeze progress %** toward the target

## Field [11] is a PERCENTAGE, not a mode
Initially misread as "mode 85". It moves inversely with temperature:

| temp °F | [11] |
|---|---|
| 5 | 83 |
| 4 | 84 |
| 3 | 85 |

They happen to sum to 88 in this capture, but that is coincidence — in type 1/17
frames `[4]` is plainly the temperature (69 °F at room temp) and `[11]` is a mode
string or other value. `[11]` rising as the chamber cools is a progress figure.

## Rate / ETA
83% → 85% took 482 s of batch time ≈ **241 s per percent**. Free Harvest
extrapolates linearly to estimate time-to-100%, measured against the dryer's own
elapsed counter so it survives adapter reboots. Treat as an estimate: freezing
typically slows near the target.

## Updated cycle map

| STAT type | phase | notes |
|---|---|---|
| 17 | Preparing | 15-min pre-cool, countdown at `[16]` |
| 2 | Transition | load trays / press CONTINUE |
| **4** | **Freezing** | **temp falls, `[11]` = % frozen, no vacuum yet** |
| 1 | Idle *or* Running | running only while `[6]` is increasing |
| 15 | Diagnostics | carries version + `zc:/bl:/ht:` codes |
| 31 | Recipe | custom profile parameters |

Still unobserved: drying, extra-dry, process-complete, defrost.

---

# Drying + real vacuum (2026-08-11 capture, part 2)

## Pressure field [5] is REAL microns once the pump runs
Previously only ever seen as the flat `10000`. This capture shows it falling
`1209 → 440` during a genuine pulldown, then holding ~433-440 while drying.

- `10000` exactly = **placeholder**, pump off (idle / prep / early freeze)
- values **> 10000** (~120k-155k) = uncalibrated sensor at atmosphere, **not a
  vacuum reading**
- values **< 10000** = real vacuum in **microns (mTorr)**

No scale factor needed — the units are microns directly.

## STAT type 5 = DRYING
Transition observed at batch-elapsed 10965 s, announced by `NTFY,5`.

`STAT,5,0,0,0,TEMP,VAC,ELAPSED,PHASE_S,46,MODE,1,PCT,0,0,7,0,,`

Distinguishing signature vs. freezing:

| | type 4 (freezing) | type 5 (drying) |
|---|---|---|
| temperature | **falling** (5 → −18 °F) | **rising** (−18 → −13 °F, shelf heat) |
| vacuum | placeholder, then pulling down | holding ~433-440 µm |
| field [8] | 60 | 46 |
| field [11] | 83→99 (progress) | reset to 1 |

## Field [7] = phase-elapsed seconds
A **second** elapsed counter that **resets at each phase change**. Confirmed:
at the type-4→5 transition it went to 1 while batch-elapsed `[6]` continued
from 10965. Thereafter both advance in lockstep (+9, +4, +3, +8, +5, +5).

So: `[6]` = seconds since batch start, `[7]` = seconds since this phase started.

## Field [11] is phase progress, not temperature progress
Earlier reading ("tracks cooling") was too narrow. It is a general
**progress-within-phase** percentage:
- during freezing it tracked temperature (83→85 as temp fell 5→3 °F)
- during the vacuum pull it tracked pressure (94→99 as vacuum went 1209→441 µm)
- it **reset to 1** when drying began

Renamed to `phase_pct` in the firmware (`freeze_pct` kept as an alias).

## Updated cycle map

| STAT type | phase | signature |
|---|---|---|
| 17 | Preparing | 15-min countdown at `[16]` |
| 2 | Transition | load trays / press CONTINUE |
| 4 | Freezing | temp falling, then vacuum pulling down |
| **5** | **Drying** | **vacuum holding, temp rising** |
| 1 | Idle *or* Running | running only while `[6]` increases |
| 15 | Diagnostics | version + `zc:/bl:/ht:` codes |
| 31 | Recipe | custom profile parameters |

Still unobserved: extra-dry time, process-complete, defrost.

## Identity frames, and a passive adapter (2026-08-21)

`/api/state` on a live link reported `frames_in:43, frames_out:0` with `serial`
and `uid` both empty. Our adapter had never transmitted a single frame in its
entire uptime.

That matters because the roles are not symmetric. `STAT` telemetry arrives
unprompted - `have_tel` was true and frames kept climbing - but the identity
frames `UID` and `SNM` do not. The stock adapter drives a handshake (`STATUS`,
`FDNAME`, `REQCFG`) and heartbeats `STATE`/`UNIQUE`/`WIFIINFO` every ~15s; ours
only listens, so nothing ever asks the dryer who it is.

`SERIAL` is INERT. Sent to a real machine: `frames_out` 0 -> 1, no reply,
`unknown_verbs` and `frames_bad` both stayed 0, `serial` stayed empty. The
`SNM` handler in hr_session.c would have populated it, so this is silence and
not a parse failure. Add it to the dead list with `GETP` and `GETR`.

This is NOT a transmit bug. `ADV` was verified the same day moving the machine
from Ready (type 1) to Starting batch (type 2) with an `NTFY,2` ack, so the
`\r`-terminated space-delimited outbound format reaches the dryer's parser
intact.

Two consequences worth chasing separately:

  - We never send `WIFIINFO`, so the dryer's own panel cannot show link state,
    RSSI or SSID while our adapter is attached. Directly checkable on the
    machine's display.

  - A permanently silent adapter is a candidate explanation for the link going
    stale, which was previously and wrongly attributed to needing a power
    cycle. Unconfirmed - telemetry was flowing fine at the time of this
    reading - but it is the first hypothesis that does not blame the dryer.

### CPU code (cloud-side, not protocol)

Registering while the adapter was attached to the SIMULATOR produced:

    "The given cpu code does not match our records. Changing the cpu code from
     37C9 355A 3037 to 1 will require the approval of a Harvest Right team
     member."

So their cloud binds adapter to machine 1:1 by a hardware identity forwarded
from the dryer, and `37C9 355A 3037` is the real machine's. The `1` is ours -
the scalar `%d` following the unique ID in
`UID,%lX-%lX-%lX-%lX,%d,%d.%d.%ld,...`.

This constrains us not at all, since Free Harvest replaces that cloud path
entirely. It does confirm the dryer exposes a stable hardware ID over serial,
which is the natural key for deciding "same machine, new adapter" when
resuming batch history.

It also means the simulator's `REAL_UID` is NOT this user's dryer, despite the
comment claiming captured values. The server's disagreement is the evidence.

## Newer firmware G0644170 decompiled (2026-08-21)

Recovered from the stock adapter's USB volume and decoded with
`tools/decode_h6r.py`. Same app region (0x18000-0xA238C) but 436KB of 566KB
differs from G0641041 - a genuinely newer build. Ran the four HR*.java scripts
against it. Function addresses moved; the scripts that locate targets by STRING
still worked, the ones with hardcoded addresses pointed at the wrong code.

New-build addresses (supersede the July analysis where they differ):
  - Serial verb dispatcher (strcmp chain):   FUN_00029418   (was 0x291b0)
  - Verb->handler references all land here, confirmed for
    ADV@2976a HCS@2982e SPC@2983c GETR@2974e GETP@2975c DUTY@2967e
    UNIQUE@29786 and @2958c (referenced TWICE - the second, outside the main
    cluster, is consistent with UNIQUE having a real handler / being live)
  - ADD is referenced from FUN_0004d72c, NOT the serial dispatcher - so ADD is
    an internal op, not a wire verb, despite being in the string table.
  - Router/logger:  FUN_0001b5e4 -> FUN_0001b254, still just a varargs wrapper
    that LOGS "Command found = %s". Confirms the dispatcher logs the match; the
    action happens elsewhere.
  - Shared run-state handler:  FUN_00021414  (was 0x218ec / 0x404c8 era)

FUN_00021414(zone, code) is the state-machine updater - the thing that makes
ADV able to start a cycle:
  - param_1 is a small phase/zone index 0..8 (Ghidra renders 4/6/8 as &Reset /
    &DAT_00000006 / &NMI - those are low-address vector symbols, not pointers).
  - param_2 is the state/command code. Writes it into run-state global
    *DAT_000216a0, gated on zone and prior state, and keeps a state-history
    ring capped at 15 entries (DAT_000216bc[]).
  - Special block: when (zone==5 && code==9) AND a temperature-like global
    (*DAT_0002c2e0) is over ~90 (or over 14 with a flag set) AND *DAT_0002c2e4
    == 5, it increments a batch counter (*DAT_0002c34c), sets flags, and
    formats a record with FUN_0007d8d8 (an sprintf). Reads like the
    batch-complete / final-dry summary transition.

CONFIRMED AT THE BINARY LEVEL: state globals are written by BOTH serial-reachable
and touchscreen functions - e.g. global 0x20002794 has 28 writers, 13
serial-reachable and 15 not. This is the mechanism behind ADV driving run-state,
and it means the serial path and the panel share one executor. So the difference
between a WORKING start (panel/app) and a BROKEN one (raw ADV -> "load trays",
compressor never engages) is NOT a different executor - it is the SEQUENCE of
(zone, code) transitions and the preconditions (temperature gates, prior state)
the proper path satisfies and ADV skips.

=> The proper-start question reduces to: what does SENDBATCH/SENDCANDY/
   SENDCUSTOM feed into this machine, and in what order? That is the next
   decompilation target (tools' HRSendBatch.java).

Dispatchers FUN_00076dc0 and FUN_00077078 (up near the entry point) also
reference command verbs - a probable second/higher-level command path, not yet
read.

## COMPLETE verb -> command-ID map (G0644170, 2026-08-21)

Extracted mechanically from dispatcher `FUN_00029418` by `HRVerbCodes.java`.
49 verbs, IDs 0x01-0x34. IDs are assigned in source order, which is why the
table is near-sequential - and that regularity is itself the check on the
extraction.

     0x01 MEMTEST     0x02 PRINT       0x03 BEEP        0x04 DIR
     0x05 MEMSIZE     0x06 RMOLD       0x07 XWIFI       0x08 XW
     0x09 DUTY        0x0A SERIAL      0x0B FUZZY       0x0C DUMP
     0x0D COPY        0x0E DEL         0x0F DIRC        0x10 GETR
     0x11 ADV         0x12 GETP        0x13 ADD         0x14 UNIQUE
     0x15 FDNAME      0x16 STATUS      0x17 CLICK       0x18 STATE
     0x19 FDRENAME    0x1A SENDBATCH   0x1B SENDCANDY   0x1C SENDCUSTOM
     0x1D ECHO        0x1E GOTIT       0x1F SETBNAME    0x20 HCS
     0x21 SPC         0x22 WIFIINFO    0x23 REQCFG      0x24 REQTSUM
     0x25 REQTHST     0x26 SETSN       0x27 REQSTAT     0x28 REBOOT
     0x29 REQSYSINF   0x2A REQBATSUM   0x2B FDFILES     0x2C FILEREAD
     0x30 REQPREF     0x31 SETPREF     0x32 SETDATE     0x33 SENDSCIENCE
     0x34 REQSCIENCE

**0x2D, 0x2E and 0x2F have no verb.** Three IDs in the middle of a contiguous
run, skipped. Either withdrawn commands or IDs reachable only from inside the
firmware. Worth checking whether the executor handles them - if the panel's own
START is an ID with no wire verb, this gap is where it lives.

A caveat on one pair: the extractor pairs a verb with the next immediate, and
GETR/ADV/GETP appear in a different order in the address space than in ID
space. ADV=0x11 / GETP=0x12 is the reading, but that specific pair is the one
place the heuristic could have transposed. Everything else is corroborated by
the sequential run.

### Three structural findings that change how commands must be sent

**1. Matching is SUBSTRING, not equality.** `FUN_0007e108` is a strstr - it
scans the whole input for the needle. A command matches if the verb appears
ANYWHERE in the line, so the chain ORDER sets precedence and a payload
containing another verb as a substring can be misrouted. This is a real hazard
for `SEND*` payloads carrying free text such as a batch name.

**2. The dispatcher is a MAILBOX, not an executor.** It ends with:

        *DAT_000296dc = uVar15;     /* store the command ID, and return */

It never calls an action. A separate task consumes the ID. This is why tracing
call edges out of the dispatcher found nothing in July and led to the wrong
"no remote control" conclusion - the edge is a shared global, not a call.

**3. There is a mode that accepts ONLY adapter housekeeping.** Guarded by
`(*DAT_000296d0 == 2 && param_1 == 0x1c)`, the dispatcher checks just four
verbs - UNIQUE, STATE, WIFIINFO, FDNAME - and silently returns 0 for anything
else. That is exactly our adapter's heartbeat set. If the dryer is in this mode,
control commands are dropped without a reply, which would look identical to a
verb being "inert". **This is a candidate explanation for SERIAL appearing dead
and for GETP/GETR appearing inert** - they may not be inert at all, merely sent
while the machine was in housekeeping-only mode.

Next target: find what READS `DAT_000296dc`. That reader is the executor, and it
is where SENDBATCH's payload is consumed.

## USB wire capture of the real dryer + official adapter (2026-08-21)

A hardware capture (LINKTYPE_USB_2_0, raw PID-level) of the genuine pair, 21
seconds covering enumeration and handshake. This is measured ground truth and
it corrects several things we had inferred.

### The identity handshake, verbatim

    ADPT->DRYER  ep2   STATE 2 0
    ADPT->DRYER  ep2   UNIQUE
    ADPT->DRYER  ep2   FDNAME
    DRYER->ADPT  ep2   UID,0-33303337-33353541-33374339,4,6.0.641041,0,5,2,1,51,204,255,
    ADPT->DRYER  ep2   REQCFG
    DRYER->ADPT  ep2   SNM,Freezie McDry,

**`UNIQUE`, sent bare with no arguments, is the identity query.** The adapter
sends it and the dryer answers with its `UID` frame. Not `SERIAL` - which we had
been probing, and which is a different thing. Our simulator was ignoring
`UNIQUE` entirely, so the adapter could never learn what machine it was attached
to, which is the likely reason the app refused to accept commands.

### The unique ID is ASCII text, not a number

    33303337 -> "3037"      33353541 -> "355A"      33374339 -> "37C9"

Each 32-bit word holds four ASCII characters. Read in reverse word order they
spell **37C9 355A 3037**, exactly the "cpu code" the app displays. So the app
shows the ID as characters while the wire carries their hex codes, behind a
leading zero word.

An earlier guess here - that the code mapped directly onto `%lX` words with
zero-fill, giving `37C9-355A-3037-0` - was wrong in both encoding and word
order. `tools/dryer_sim_full.py --cpu "37C9 355A 3037"` now reproduces the
captured frame byte for byte, which is the check that the reading is right.

### SNM is a NAME, not a serial number

The real machine answers `SNM,Freezie McDry,` - a user-set name. We had been
sending the data-plate serial `P-STF 2311-03561 BKC`. Note our own firmware
stores this field as `info.serial` and publishes it as `"serial"` in
`/api/state`; the label is wrong, though the plumbing is fine.

### Other observations

- The dryer reports firmware **6.0.641041** - the OLDER build. The adapter
  carries `G0644170` on its USB volume, so it ships a newer image than the
  machine it was attached to is running.
- The official adapter's MSC inquiry string is `TinyUSB Flash Storage 0.1`, the
  TinyUSB default. It is a composite CDC+MSC device on the same stack we use.
- Frames terminate with a single `\r`, confirming our encoder.
- The protocol runs on **ep2**; ep3 is the mass-storage endpoint.
- Traffic is overwhelmingly NAKs - 127,283 of 130,976 packets in 21 seconds are
  the host polling an endpoint with nothing to say. The link is near-idle by
  design, which is worth remembering before reading anything into quiet periods.

## CLICK IS THE CONTROL VERB (2026-08-21) — captured from the real app

With the simulator presenting the measured identity, the app connected, offered
Start / Custom / Candy / Config, and emitted:

    CLICK 1 10 54779 175300      <- Start
    CLICK 1  9 54780 175300      <- Custom
    CLICK 1  8 54781 175300      <- Candy
    CLICK 1  3 54782 175300      <- Config

    CLICK <screen> <button> <counter> <session>

- **screen** - 1 here, matching STAT type 1 (Ready). This is why control is
  screen-relative: the same button number means different things per screen.
- **button** - the widget on that screen. Start=10, Custom=9, Candy=8, Config=3.
- **counter** - increments once per DISTINCT press and is REUSED across retries.
  The app resent each click 4-6 times when unacknowledged and the counter held
  steady, so it is an idempotency key, not a nonce.
- **session** - 175300, constant for the whole session.

The app retries until the machine's state changes to match. That is the
acknowledgement: not a reply frame, but the next STAT reflecting the new screen.
A simulator that never changes state makes the app give up gracefully.

### This was a live security hole in v0.3.5

`CLICK` sat in the SAFE allow-list described as a "benign local effect (no state
change)". It is the opposite. And the exposure was reachable, not theoretical:
`hr_session_send_config()` accepts `HR_CMD_SAFE` as well as `HR_CMD_CONFIG`, and
`/api/cmd` routes there whenever args are present, so

    POST /api/cmd  verb=CLICK&args=1,10,54779,175300

from the web UI's raw-command box would have started a 24-hour cycle. Removed
from the allow-list, from the UI's verb picker, and the unit test now asserts
`HR_CMD_UNKNOWN`. Verified against the live adapter: CLICK returns
`{"ok":false,"reason":"not allowed"}` while BEEP still returns ok.

**The lesson generalises.** `CLICK` was classified benign by reading the verb
name and seeing no obvious handler - the same reasoning that declared `ADV`
dead and remote control impossible. Verb names are not evidence. Anything not
demonstrated inert on real hardware should default to excluded.

### Still unknown

- What acknowledges a CLICK, beyond the state change itself.
- Whether the counter must be monotonic, or merely different from the last.
- What `session` (175300) is derived from, and whether the dryer validates it.
- Button IDs for every screen other than Ready. Walking the simulator through
  states 2-5 with the app connected would enumerate them.

## FDRENAME, config-in-files, and a second allow-list gap (2026-08-21)

Renaming the dryer from the app produced:

    ADPT->DRYER   FDRENAME "Freezie McDry"
    ADPT->DRYER   FDNAME                      (immediately re-read)
    ADPT->DRYER   FDFILES .dat 1

So `FDRENAME "<name>"` takes a QUOTED argument, space-delimited like everything
else outbound, and the app re-reads `FDNAME` straight afterwards to confirm.

**FDName.txt holds the machine NAME, not the serial.** Confirmed twice: the real
dryer answered `FDNAME` with `SNM,Freezie McDry,` in the USB capture, and the
rename writes that same string. Our simulator had been serving the data-plate
serial. Fixed.

**Settings live in .dat files.** `FDFILES .dat 1` asks for a listing, and the
file-transfer verbs (`FDFILELIST` announce, `FDFILEBLOCK` contents) move them.
That is how the Config screen works, and answering `FDFILES` is the next capture
worth running - it should reveal the settings file format.

### The exposure, stated correctly

`SETSN` and `FDRENAME` were in the CONFIG allow-list while simultaneously
appearing on our own "never probe" list. Both are now removed.

I first described them as reachable from `/api/cmd`. **That was wrong.**
`/api/cmd` gates on `hr_cmd_classify(verb) != HR_CMD_SAFE` and returns 403, so
CONFIG-class verbs never reach `send_config` over HTTP at all.

The real path is **MQTT**. `hr_mqtt.c` splits the cmd topic payload on a comma
and calls `hr_session_send_config(verb, args)` **with no gate of its own**,
relying entirely on that function's class check - which accepts SAFE *and*
CONFIG. So anyone able to publish to the cmd topic could have sent `SETSN,...`
or `FDRENAME,...`.

`CLICK` was worse because it was SAFE: reachable over BOTH transports.

This also means CONFIG-class verbs (`SETPREF`, `SETDATE`, `SETBNAME`, `SEND*`)
are an MQTT-only feature by design - `/api/cmd` cannot reach them. Worth
knowing before treating an HTTP 403 as a bug.

### Known issue

OTA now reports `reset_reason: "panic"` on the reboot that follows a successful
update, reproducibly, across two updates. The new image boots and runs
correctly, so this is cosmetic in effect - but a clean `esp_restart()` should
report `sw`, and something is crashing on the way down. Only visible because
reset_reason was added earlier the same day.

## The complete control surface, measured (2026-08-21)

Captured from the genuine app driving the simulator through every screen.

    screen  btn  action                    verb
    ------  ---  ------------------------  --------------------------
      1      10  Start Auto                CLICK 1 10 26457 175300
      1       9  Start Custom              CLICK 1 9 49057 175300
      1       8  Start Candy               CLICK 1 8 49058 175300
      1       3  Machine config            CLICK 1 3 49059 175300
     17       3  Advance to next step      CLICK 17 3 48716 175300
     17       4  End batch (prep)          CLICK 17 4 48727 175300
      4       3  Advance to next step      CLICK 4 3 26460 175300
      4       4  End batch (freezing)      CLICK 4 4 48719 175300
      5       1  End batch (drying)        CLICK 5 1 48725 175300
      6       1  End batch (final dry)     CLICK 6 1 48722 175300
      6       2  Increase final dry time   CLICK 6 2 48720 175300
      6       3  Decrease final dry time   CLICK 6 3 48721 175300

**End Batch is button 4 on screens 17 and 4, but button 1 on screens 5 and 6.**
Button numbers carry no meaning across screens. This is the concrete
justification for refusing a stale view rather than trusting the caller.

**The counter is GLOBAL across verbs, not per-verb.** A captured
`CLICK 1 3 49059` was immediately followed by `SPC 255 76 80 49060`. So it is a
single command sequence for the whole session.

**The trailing 175300 is CONSTANT.** It held identical across three capture
sessions on different days with counters in the 26k, 48k and 49k ranges. It is
not a session token or a nonce, and can be sent as a literal.

### Non-CLICK commands the app uses

    Rename machine        FDRENAME "Freezie McDry"
    Set machine colour    SPC 255 76 80 49060        <- RGB red, + counter
    Reboot after update   REBOOT 6000                <- takes a delay argument
    End-batch options     REQPREF
    Batch history         REQBATSUM 0 5              <- paginated: start, count
    System details        REQSYSINF

**Correction: SPC is not what we assumed.** Our never-probe list described it as
energising the shelf/pump, alongside DUTY and HCS. The observed use is setting
the machine's display COLOUR as an RGB triple. That reclassification is based on
a single observation, so SPC stays off the allow-list - but the stated reason
for excluding it was wrong, and a wrong reason is worth correcting even when the
conclusion holds.

**REBOOT takes an argument** (`REBOOT 6000`), presumably a delay. Still excluded.

**REQPREF crashes our simulator's peer.** Answering it with `SYSPREF,0` dropped
the serial connection - so the reply shape is wrong and REQPREF expects
something richer. The app uses it for end-batch options.

### Still unknown

- Whether the counter must be monotonic, merely distinct, or is ignored.
- Screen 2 ("Starting batch") has no captured buttons. That is the screen the
  machine stranded on during the ADV probe, so it is the one screen where
  guessing would be worst.
- Screens 7 (Complete/venting), 15 (Diagnostics) and 31 (Recipe settings).
