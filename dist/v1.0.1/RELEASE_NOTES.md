# Free Harvest v1.0.1

Two fixes, both found by users rather than by us.

### Dryers that never sent telemetry

A user on a slightly different firmware version saw the adapter receive 1,300+
frames without a single one being useful - every one was `REQINFO`, repeated
every two seconds forever, and `trend pts=0` throughout.

We were answering `REQINFO` with `GOTIT`, using a payload the source itself
described as UNVERIFIED. The genuine adapter answers with **`WIFIINFO`** - our
own USB capture shows it plainly:

    ->  REQINFO,
    <-  WIFIINFO 2 0 "" 1 HR_aabbccddeeff 0 0 7

`GOTIT` is in fact the DRYER's acknowledgement of a `CLICK`, so we were
answering a question with someone else's answer. One firmware tolerated it and
sent telemetry anyway; another waited for a reply it recognised, indefinitely.

The adapter now reports its real link state, signal strength, SSID and AP name.
A test pins the frame to the captured bytes so this cannot regress quietly.

**If your dryer connects but never shows readings, this is the fix.**

### Personal data removed from the repository

Simulator transcripts containing a dryer serial number and a home network name
had been committed. They are untracked now and the ignore rules cover the
`tools/` copies that the earlier rule missed. Real identifiers in source
comments, tools and notes are replaced with obvious stand-ins.

**Note that git history still contains them.** Untracking removes a file from
future commits, not from the past. A history rewrite is required before this
repository is made public - see the README.

---

Update over Wi-Fi: Settings -> Firmware update, upload hr_wifi_adapter.bin.
