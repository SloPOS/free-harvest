> ## ⚠️ Withdrawn — do not install
>
> **This release crashes the adapter on every frame the dryer sends.**
>
> The capture log writes each frame to flash from inside the USB RX callback,
> which runs on the TinyUSB task. Its 4096-byte stack cannot accommodate file
> I/O, so the first inbound frame overflows it and panics the chip. The adapter
> then reboots and re-enumerates fast enough to look healthy, while reporting
> zero bytes received — because the counter resets with it. The result is an
> adapter that never shows telemetry and looks like a dryer that stopped talking.
>
> It also has no OTA rollback protection, so a failed update removes remote
> access permanently and can only be recovered with a USB cable.
>
> Use **[v0.3.4](https://github.com/SloPOS/free-harvest/releases/tag/v0.3.4)**
> instead. It fixes the crash, adds the trial-boot rollback that makes wireless
> updates safe, and adds a temperature trend graph.
>
> The notes below are kept for reference only.

---

