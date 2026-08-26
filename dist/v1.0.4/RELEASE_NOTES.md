# Free Harvest v1.0.4

**The log now shows what the adapter sends, not just what it receives.**

Every release before this one logged only one direction. A log looked like this:

    RX <- REQINFO,
    RX <- REQINFO,
    RX <- REQINFO,

which reads as a dryer asking a question over and over and an adapter that never
answers. The adapter was answering every single one of them, in about a
millisecond. There was no way to tell from the log — you had to go read the
firmware source to find out.

That is a bad diagnostic, and it cost real time while troubleshooting a dryer
that genuinely was not communicating. The same log now reads:

    RX <- REQINFO,
    TX -> WIFIINFO 5 51 "MyNetwork" 0 HR-Adapter-Setup 0 0 2
    TX -> STATE 5 51
    TX -> UNIQUE
    TX -> FDNAME
    TX -> REQCFG
    RX <- UID,...
    RX <- SNM,...
    RX <- CFG,...

## What changed

- **`TX -> ` logging** for every frame the adapter transmits, mirroring the
  existing `RX <- ` lines. The frame terminator is trimmed so each frame is one
  readable line.
- **`frames_out`** now appears beside `frames_in` in the ten-second status line.

Nothing else. No protocol changes, no behaviour changes, no new commands. If
1.0.3 works for you, 1.0.4 works identically — it just tells you more.

## Reading your log

**`REQINFO` on a repeating cycle is normal.** A healthy dryer interleaves it
with `STAT` roughly every ten seconds, forever. A log full of `REQINFO` is not a
fault by itself.

**What matters is whether `STAT` ever appears.** `STAT` frames carry the
temperature, pressure and phase — they are the telemetry. No `STAT` means the
dryer is not reporting, regardless of how healthy everything else looks.

A working exchange after plugging in looks like this, within the first four
seconds:

| you should see | meaning |
|---|---|
| `TX -> STATE` / `UNIQUE` / `FDNAME` / `REQCFG` | the adapter introducing itself |
| `RX <- UID,...` | the dryer identifying its CPU |
| `RX <- SNM,...` | the dryer's name |
| `RX <- CFG,...` | its recipe configuration |
| `RX <- STAT,...` | telemetry, and everything is working |

If you get `UID` but never `SNM`, `CFG` or `STAT`, the adapter is reaching your
dryer and being understood — the dryer is answering the one query it handles
directly and dropping the rest. That points at the dryer's state, not at the
adapter or your network. Grab the log and open an issue.

## Installing

**Over the air** — Settings → Firmware update, upload `hr_wifi_adapter.bin`.

**First-time flash over USB** — all four files, offsets below. Omitting
`ota_data_initial.bin` boots the old image and looks exactly like a failed
flash.

| file | offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `hr_wifi_adapter.bin` | `0x20000` |

Verified on hardware: flashed over OTA to a running 1.0.3, confirmed and kept,
handshake completed and telemetry resumed with both directions logging.
