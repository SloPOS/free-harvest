# Free Harvest v1.0.5.2

**A supplement to [v1.0.5](../../releases/tag/v1.0.5), not a new feature release.**
It contains v1.0.5 plus three fixes to how the adapter introduces
itself, and nothing else.

If v1.0.5 talks to your dryer, this changes almost nothing — your handshake
takes about three quarters of a second longer and that is the whole difference.
**If your dryer answers `UID` and then goes quiet, this is the update to try.**

## The handshake was sent as a burst

The adapter introduced itself with four frames back to back — under 15
milliseconds end to end:

```
TX -> STATE
TX -> UNIQUE
TX -> FDNAME
TX -> REQCFG
```

On one dryer running firmware **6.0.644170**, only the *second* frame was ever
answered. `UNIQUE` returned its `UID`; `FDNAME` and `REQCFG` drew nothing at
all, repeatably, and no telemetry ever followed. That is what a receive path
looks like when it takes a frame or two and discards the rest of what arrived
alongside them.

The genuine HarvestRight adapter does not send a burst. From the USB capture:

```
STATE 2 0
UNIQUE
FDNAME
  <- UID          the dryer answers HERE
REQCFG
  <- SNM
```

It waits. So does this release — one frame every 250 ms.

## The handshake did not restart after a USB re-attach

Using **Reattach USB** in Settings gives the dryer a fresh connection, and it
forgets the adapter introduced itself. We never introduced ourselves again.

The restart was tied to the *protocol* link dropping, which is a different
event: `REQINFO` keeps arriving across a re-attach, so the link never went
down and the handshake never re-ran. A user's log shows it exactly — the
adapter re-enumerates, and then nothing but `REQINFO` forever.

Now keyed on the USB mount count as well, so a re-attach re-introduces.

## The adapter identified itself with a name no dryer has seen

`WIFIINFO` field 4 is the adapter's own identifier. Every genuine capture
carries `HR_` followed by the MAC with no separators:

```
genuine : WIFIINFO 5 81 "MyNetwork" 1 HR_aabbccddeeff 0 1 37
ours    : WIFIINFO 5 100 "MyNetwork" 1 HR-Adapter-Setup 0 1 5627
```

The same string appears a second time in `esp/version.txt` on the stock
adapter's mass-storage volume, which the dryer also reads — so a real adapter
presents that identifier to the machine twice, in that format. We were sending
`HR-Adapter-Setup`, which matches neither.

Now sent as `HR_<mac>`, e.g. `HR_aabbccddeeff`.

**Whether any firmware checks it is unknown.** This closes a measured
divergence from the genuine adapter; it is not a demonstrated fix. Your setup
hotspot keeps its existing name — only the field sent to the dryer changed.

## What this does not include

None of the 1.0.6 work in progress — no dryer reboot control, no batch naming,
no capture-log rotation, no recipe-verb guard, no estimator change. This is
v1.0.5 with the handshake fixed, deliberately, so it can be tried without
taking on anything else.

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

Flashed to a live dryer. The replies now land between the frames, as the
capture shows:

```
7271  TX -> STATE 5 100
7522  TX -> UNIQUE
7581  RX <- UID,...
7773  TX -> FDNAME
7844  RX <- SNM,...
8024  TX -> REQCFG
8108  RX <- CFG,...
```

A forced re-attach was also confirmed to re-introduce: mount #2, hello 231 ms
later, all three identity frames answered again.

## Honest about the odds

This is well-founded, not certain. The dryer that prompted it answered exactly
the frame the burst theory predicts it would, and the genuine adapter paces —
but nobody has yet confirmed that pacing makes a 644170 machine complete its
handshake. If it still answers only `UNIQUE`, the next thing to compare is the
CDC `DTR` line, which that machine reports as `0` after re-enumeration.
