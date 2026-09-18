# Free Harvest v1.2.2

**The dryer's own batch logs, read off the machine and charted — and the
defrost screens decoded.**

## Batch history

Your dryer has been keeping a CSV log of every batch it has ever run, a row a
minute, on its own drive. Settings → **Batch history** lists those files and
reads one over the same USB cable.

That gets you two things the adapter could not give you before: **runs from
before you owned this adapter**, and the **ambient thermocouple**, which no
live frame carries at all.

Open one and it is charted — shelf and room temperature, vacuum on its own
scale, and the dryer's own phases as coloured bands — with a table of how long
each phase took, how cold it got, how deep the vacuum went, and what the room
was doing. The raw file is a download away, exactly as the dryer wrote it.

A few things worth knowing:

- **Nothing is stored on the adapter.** The bytes go to your browser as they
  arrive.
- **The dashboard stays live while it reads.** The transfer runs in the
  background and the page shows it arriving. A small batch takes a second; a
  50-hour one takes about a minute, at the dryer's own pace of a kilobyte
  every 170 ms.
- **Reading is refused while a batch is running** unless you say otherwise on
  that page. A running machine has better things to do with its USB port.
- **There is an on/off switch**, and nothing is sent to the dryer until you ask
  for a listing or a file.

The page can also ask the dryer which unit its own panel is set to, and follow
it — a button in Settings → Temperature unit.

## The defrost screens

Four screen types the parser did not know are now decoded:

- **The pump purge** (the screen after you choose DEFROST at the end of a
  batch) shows its countdown on the dashboard. On a machine with an oil-free
  pump, the dryer runs the pump for five minutes to clear it while the panel
  offers a defrost time. The batch counter keeps advancing on that screen even
  though the run is over — the app no longer mistakes that for a running batch.
- **Final dry** sends one frame of a different type at the handover to its
  timed stage. It is the same screen, and the logbook now counts it as such —
  treated as its own phase, it was quietly losing a couple of minutes of final
  dry from every record.
- **Defrosting** and **defrost complete** are named, but nothing more is
  claimed: nobody has captured either frame yet. If your machine defrosts with
  the adapter plugged in, that capture is wanted.

## Safety

Both of the file verbs read and nothing else, and every file name and pattern
is checked before it can reach the wire.

That check matters more than it sounds. The dryer picks a command by searching
the whole line for a verb it knows, so text in an *argument* can be read as a
command — and a carriage return in one would end the frame and turn the rest
into a second command of somebody else's choosing. Names and patterns are now
restricted to printable characters, with no separators, path characters, or
any verb the machine tests before these two. The same hazard, and the same
fix, as recipe names.

Credit where it is due: the wire facts behind both features were worked out and
reported by **vskiwi** (PRs #10 and #11), including the discovery that
`FILEREAD` had worked all along — the mysterious `A1` in our notes was the
checksum at the end of a block, the only part of the answer that survived a
reassembler built for short printable lines.

## Also

- `/api/state` and the MQTT state document gain `purge_s`, the pump-purge
  countdown.
- The MQTT state buffer grew: adding that field pushed the worst case past its
  old size, and the builder refuses rather than truncates, so a long document
  would have gone unpublished without a word.

## Installing

**Over the air** — Settings → Firmware update, upload `hr_wifi_adapter.bin`.
If a PIN is set you are asked for it before the upload starts.

**First-time flash over USB** — all four files. Omitting `ota_data_initial.bin`
boots the old image and looks exactly like a failed flash.

| file | offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `hr_wifi_adapter.bin` | `0x20000` |

## What has and has not been tested

**Verified:** 18 host test suites, about 10,500 checks, including the file
protocol and its argument gate; a clean `esp32s3` build with zero warnings
(31% of the app partition still free); and the whole Batch history page driven
in a browser against a stand-in for the dryer — listing, a read with live
progress, the chart, the phase table, °C conversion, the unit button, a
non-CSV record, and phone width.

**Not yet:** any of this against a real dryer. The file client has never spoken
to a machine from this code — the protocol facts come from vskiwi's captures,
but this implementation is new. If you run it, the first listing and the first
batch log are the interesting moments, and the Device log will say what
happened either way.

Still outstanding from 1.2.0: **no complete freeze-drying cycle has been
reported on any 1.2.x release.**
