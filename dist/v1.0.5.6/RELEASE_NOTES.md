# Free Harvest v1.0.5.6

**The adapter now keeps asking, the way the real one does.**

If v1.0.5 works for you this is invisible. **If your dryer returns its `UID` and
then never sends telemetry, this is the release to try** — and unlike the last
one, this change was measured from a genuine adapter rather than inferred.

## What was wrong

Free Harvest introduced itself once, at link-up, and never again. A dryer that
missed that single request — or wasn't ready for it — was never asked a second
time. Its log showed one unanswered `FDNAME` and then nothing but a heartbeat,
forever.

## What the genuine adapter does

We taught the dryer simulator to play a *failing* machine: answer the identity
query and then go silent, exactly as a 6.0.644170 dryer does. Then we put a
stock HarvestRight adapter in front of it:

```
 32.39  <- UNIQUE          answered
 32.39  <- FDNAME          deliberately unanswered
 32.39  <- REQCFG          deliberately unanswered
 47.44  <- FDNAME / REQCFG     +15.1s
 62.56  <- FDNAME / REQCFG     +15.1s
 77.66  <- FDNAME / REQCFG     +15.1s
 92.76  <- FDNAME / REQCFG     +15.1s
107.88  <- FDNAME / REQCFG     +15.1s
```

**It re-asks on every heartbeat, indefinitely, until it gets an answer.** Free
Harvest now does the same: it re-sends `FDNAME` until the dryer's name arrives
and `REQCFG` until its configuration does, then stops. A dryer that answers
everything the first time sees no change at all.

## And it now asks for telemetry

`STATUS` is a verb the genuine adapter sends and Free Harvest never did. On
firmware 6.0.641041 it is a live telemetry request — three trials, a reading
back in 79–100 ms every time — and it is **not** a synonym for `REQSTAT`: the
two run different code inside the dryer.

That matters because the failing machine ignores `REQSTAT` and has never been
sent a `STATUS`. It is the one untried route to telemetry.

It is now part of the handshake, and it is re-asked until a reading arrives.
On a healthy dryer the side effect is pleasant: first telemetry lands in about
three seconds instead of waiting up to fifteen for the machine to volunteer it.

## Correcting v1.0.5.4

v1.0.5.4 claimed `UNIQUE` had to carry the argument `lH`, based on an
instruction in the dryer's firmware.

**That was overstated.** The same experiment caught the genuine adapter sending
a bare `UNIQUE`, and the instruction we relied on searches a buffer that isn't
the one the command parser uses. The argument is still sent — an earlier capture
did show `UNIQUE lH`, both are real behaviour, and it is verified harmless — but
it is not the reason any dryer goes quiet. The retry above is.

We would rather correct this in public than leave a wrong explanation standing.

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

On firmware 6.0.641041 the handshake completes as before — `UID`, `SNM`, `CFG`
and telemetry — and because every request is answered, **no retries are sent at
all**: the heartbeat goes back to a bare `STATE`. The retry only appears when a
dryer has actually failed to answer.

Whether it fixes 6.0.644170 is still unconfirmed. What is different is that this
behaviour was recorded from a working adapter facing the exact failure, rather
than deduced.
