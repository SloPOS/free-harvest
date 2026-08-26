# Free Harvest v1.0.3

**The adapter now introduces itself to the dryer.** Until this release it
initiated nothing at all - it only ever answered.

A user reported that their dryer talked only while the Wi-Fi settings screen was
open, and even then sent nothing but `REQINFO` on a two-second loop. Their
network appeared correctly on that screen, so the link itself was fine; the
dryer simply never progressed to sending telemetry.

The genuine adapter opens a connection like this, captured over USB:

    STATE 2 0
    UNIQUE      -> dryer answers UID
    FDNAME      -> dryer answers SNM
    REQCFG      -> dryer answers CFG

then emits `STATE` roughly every 15 seconds. We did none of it. One dryer
firmware streams telemetry regardless and hid the gap for months; another waits
to be introduced to.

**If your dryer connects but only ever sends REQINFO, this is the fix to try.**

### Side effect: the dryer's identity is now known

Because nothing ever sent `UNIQUE` or `FDNAME`, `/api/state` has reported an
empty `serial` and `uid` for the entire life of this project. They are populated
now - the machine name and its unique ID - which also gives the logbook a real
machine identity to work with later.

Every verb in the opening exchange is a read or a status report. None changes
machine state.

---

Update over Wi-Fi: Settings -> Firmware update, upload hr_wifi_adapter.bin.
