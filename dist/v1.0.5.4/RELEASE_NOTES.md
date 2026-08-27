# Free Harvest v1.0.5.4

**`UNIQUE` was missing an argument, and that is why some dryers answer once
and then go quiet.**

If v1.0.5 works for you, this changes nothing you can see. **If your dryer
returns its `UID` and then never sends telemetry, this is the release to try.**

## The one-line change

The adapter introduced itself with a bare `UNIQUE`. The genuine adapter sends
`UNIQUE lH`, and that argument is load-bearing. From the dryer's own firmware:

```
ldr  r1, ="lH"
bl   strstr            search the argument for "lH"
cbz  r0, skip          not found -> skip the next two instructions
movs r2, #1
strb r2, [mode]        mode = 1, ONLY when "lH" is present
```

The `UID` reply sits further down the same handler and is sent either way.
**That is why this hid for so long**: a bare `UNIQUE` looks like it worked. It
returns the right answer while quietly leaving the dryer's mode byte at zero,
and that byte is read again in the dryer's USB layer. On firmware 6.0.644170 a
machine left at zero answers `UNIQUE` and then nothing — no name, no
configuration, no reply to a status request sent on its own, and no telemetry
at all.

## How the project got it wrong

Our protocol notes recorded `UNIQUE lH` from an early capture. A later capture
showed `UNIQUE` bare, was treated as the more reliable observation, and the
bare form was adopted — the argument read as noise.

Both captures were real. The difference between them was the entire bug. A
capture showing a shorter frame does not disprove one showing a longer frame,
and the safe reading was the superset.

## What made it findable

A user's adapter, after 6725 seconds:

```
frames_in 673 | usb_rx_bytes 6114 | frames_bad 0 | unknown_verbs 0
serial "" | uid populated | have_tel false
```

672 × `REQINFO` at 9 bytes, plus one `UID` at 66 bytes, is 6114 bytes exactly.
Nothing dropped, nothing malformed, nothing unparsed — so the dryer really did
send one `UID` and then nothing but a WiFi poll, every ten seconds, for two
hours. That accounting eliminated every "the reply is arriving and we are
mishandling it" explanation and left the frame we send as the only variable.

The rest came from disassembling the dryer's own firmware and following the
path from a received byte to a transmitted reply.

## Also in this release

- **`frames_bad` and `unknown_verbs` in the ten-second status line.** They were
  only in `/api/state`, so a log could not distinguish "nothing arrived" from
  "something arrived and we rejected it". That distinction is what this
  investigation turned on.

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

## Verified, and what is still open

On firmware 6.0.641041, `UNIQUE lH` produces `UID`, `SNM`, `CFG` and telemetry
exactly as the bare form did — the argument does not disturb a machine that
never needed it.

Whether it fixes **6.0.644170** is unconfirmed; nobody has run it on one yet.
What is different about this attempt is that it has a mechanism behind it —
a specific instruction in the dryer's firmware that tests for this exact
string — rather than a resemblance to a capture.
