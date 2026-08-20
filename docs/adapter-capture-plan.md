# Capturing the official adapter

Order matters here: each step is more invasive than the last, and the cheap ones
may make the expensive ones unnecessary. Do them in order.

## Set expectations first

**Remote cycle control does not exist in this protocol.** The dryer's firmware
was disassembled and its command table contains no START, CONTINUE, DEFROST,
MORE DRY TIME, END BATCH or CANCEL verb. The strings that look promising —
`ADV`, `HCS`, `SPC`, `GETR`, `GETP`, `DUTY`, `UNIQUE` — appear only in the
dispatcher's `strcmp` chain with no handler behind them. All flow control is
panel-only.

No amount of sniffing can reveal a command that isn't implemented. That is a
settled negative result, not a gap in our capture.

What capture **can** settle:

| Unknown | Why it matters |
|---|---|
| **The exact `GOTIT` payload** | `PROTOCOL_NOTES.md` calls this "the session gate". Our firmware sends a *guess* (`CONFIG_HR_ACK_PAYLOAD`). If the dryer validates it, this is the single most important byte sequence to get right. |
| Whether the adapter speaks first | Ours never does. If the official one announces itself, our handshake is incomplete. |
| Verbs the official adapter sends | Anything we don't send is a capability we don't have. |
| Cadence and timeouts | Whether the dryer expects replies inside a window. |
| Cloud/provisioning traffic | What it sends to `harvestrightapp.com`, and what the dryer relays. |

## Step 1 — Interrogate it from a PC (no sniffer)

The dryer is a USB **host**; the adapter is a USB **device**. A PC is also a
host, so it can stand in for the dryer exactly. Plug the official adapter into
your PC and it should enumerate as a serial port.

```
python tools/interrogate_adapter.py
```

It listens passively first, then replays the dryer's real discovery sequence
(`UID` → `SNM` → `REQINFO`) and idle `STAT` cadence, logging every frame
byte-exact with relative timings to `adapter-transcript-<stamp>.txt`.

If a `GOTIT` comes back, that answers the biggest open question outright and
costs nothing.

Passive first if you'd rather not transmit at all:

```
python tools/interrogate_adapter.py COM12 --listen
```

**If no serial port appears**, the adapter may present a vendor-specific
interface rather than CDC, or may not power up without the dryer. That itself is
a finding — note the VID/PID from Device Manager and move to step 2.

## Step 2 — Dump its firmware

The adapter is an ESP32 (the stock unit uses a YD-ESP32-23 board), so its flash
can be read with `esptool` — the same tool we already use, already installed.

1. Put the board in **download mode**: hold **BOOT** (GPIO0), tap **RESET**,
   release BOOT. A serial port appears.
2. Read the whole flash:

```
python -m esptool --port COM12 --baud 921600 read-flash 0 0x400000 hr_adapter_stock.bin
```

Adjust the size if it reports a different flash chip — `flash-id` tells you:

```
python -m esptool --port COM12 flash-id
```

Then pull the strings and load it into Ghidra the same way we did the dryer's
firmware. The `esp32` target is Xtensa, not ARM, so use `Xtensa:LE:32:default`
rather than the Cortex settings used for `G0641041_mainapp.bin`.

Reading flash is non-destructive — it does not erase or modify the device.
**Do not `write-flash` to it**; keep the stock adapter working as a reference.

> **Never commit the dump, or anything derived from it that contains their
> code.** Same rule as the dryer firmware: vendor binaries stay out of the repo.
> Our own notes and analysis are fine; their bytes are not.

## Step 3 — Sniff the wire (only if 1 and 2 leave gaps)

Needed only to see the *live* conversation with a real dryer — timing, ordering,
and anything the adapter does that it won't do for a PC.

Tap **D+/D− between the dryer and the adapter**. This is USB CDC bulk traffic,
not a UART, so a logic analyser on TX/RX pins will see nothing.

Options, cheapest first:

- **Pi Pico** running a USB-sniffer firmware, wired inline on D+/D−.
- **A USB passthrough breakout** with test points, plus a logic analyser that
  can decode USB full-speed (12 Mbit/s — many cheap analysers cannot).
- **A hardware USB protocol analyser** (Beagle USB 12 or similar) if borrowing
  one is an option; far and away the least painful.

Capture the **first 30 seconds after the dryer powers on**, which is when
discovery and the handshake happen. The idle traffic afterwards we already
understand.

## What to do with the results

If `GOTIT` differs from our guess, set it in `menuconfig` under
`CONFIG_HR_ACK_PAYLOAD` and the difference should be visible immediately — the
dryer either accepts our adapter faster or stops treating it as unknown.

Paste the transcript back and we can decode it against `PROTOCOL_NOTES.md`,
which already documents every verb and field we've confirmed.
