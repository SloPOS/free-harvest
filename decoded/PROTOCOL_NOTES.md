# HarvestRight ↔ WiFi Adapter Protocol — Static-Analysis Notes

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
