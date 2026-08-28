<h1>
<img src="docs/img/leaf.svg" width="28" align="top" alt=""> Free Harvest — Beta
</h1>

**This branch currently has nothing that `main` does not.**

Everything it was holding — the dryer reboot control, batch naming, recipe-send
confirmations, capture-log rotation, repeated-frame collapsing and the windowed
freeze estimate — shipped in **v1.0.6**. The code here is byte-identical to
`main`; only the version string differs, so Settings → About reads
`1.0.6-beta` and you can tell which image a device is running.

> ### 👉 Run `main`
>
> **[Read the main README](https://github.com/SloPOS/free-harvest/blob/main/README.md)**
> for what Free Harvest is, the hardware, flashing, setup, Home Assistant, the
> control surface and its limits — and for the **dryer firmware warning**, which
> matters more than anything on this page.
>
> Use the [latest release](https://github.com/SloPOS/free-harvest/releases/latest).
> There is no reason to run beta right now.

---

## What this branch is for

A place to try changes against a real dryer before they reach anyone else. It
earned its keep during the 6.0.644170 investigation: a partly-built mass-storage
experiment lived here, was tested, and was parked rather than shipped once the
dryer's firmware turned out to be the fault.

When the next feature needs that, this page comes back and documents only the
difference. Until then it would be dishonest for it to list anything.

## Version numbering

Beta tracks `main`'s numbers with a `-beta` suffix. `main` merges forward into
beta; beta is never merged back without a release.

**Going back** to `main` is an ordinary OTA update — download the current
release and upload it in Settings → Firmware update.
