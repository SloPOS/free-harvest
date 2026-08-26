<h1>
<img src="docs/img/leaf.svg" width="28" align="top" alt=""> Free Harvest — Beta
</h1>

**This is the testing branch.** It carries everything on `main` plus the changes
listed below, which have not been through a full freeze-drying cycle yet.

> ### 📖 Documentation lives on `main`
>
> **[Read the main README](https://github.com/SloPOS/free-harvest/blob/main/README.md)**
> for what Free Harvest is, hardware, flashing, first-time setup, normal
> operation, Home Assistant/MQTT, the control surface and its limits, and
> everything else. None of that is repeated here.
>
> This page documents **only what beta does differently.**

---

## Which one should I run?

**`main`.** It is the released firmware and the one that has been run against a
real dryer for full cycles.

Run beta only if you are deliberately testing one of the changes below, and are
willing to have a batch's logging behave differently than documented. Beta and
`main` are kept as separate lines on purpose so features can be tried without
destabilising the release.

Beta tracks `main`'s version numbers with a `-beta` suffix — a beta built from
1.0.5 reports **`1.0.5-beta`**, shown in Settings → About. `main` is merged
forward into beta; beta is never merged back without a release.

**Going back** is an ordinary OTA update: download the current `main` release
and upload it in Settings → Firmware update. Nothing on the device needs
resetting, though see the capture-log note below.

---

## What beta changes

### The capture log rotates instead of stopping

On `main`, the frame capture is a single file that **stops appending** when the
partition nears full. On a dryer left running that quietly becomes a useless
log: it preserves the oldest hours and discards everything since — the wrong
half to keep, because defrost and the end-of-cycle states happen at the *end* of
a run.

Beta writes four rotating segments, deleting the next before it becomes active,
so **recording never stops**. Four rather than two so a rotation costs a quarter
of the history instead of half; the retained window stays above roughly 2 MB, or
about two full cycles.

An existing `frames.log` is migrated into the first segment rather than
orphaned. If it is larger than one segment it cannot be kept — four segments
have to fit the partition — and it is dropped, with the projection and the
budget both named in the log. **Download anything you still want from
`/api/capture` before switching to beta.**

### Repeated frames are collapsed

An idle dryer repeats itself. `REQINFO` arrives every 10 seconds doing nothing,
and every 2 seconds while the panel sits on the Wi-Fi screen — measured there at
80% of all inbound frames.

Identical consecutive frames now collapse to a single summary line:

```
45231   REQINFO,
105240  ~repeat REQINFO, x30 45231..105240
```

The count and time span are kept, so the **cadence survives** — which matters,
because the 10s-versus-2s `REQINFO` rate is how a machine parked on its Wi-Fi
screen is identified. The summary is denser than the frames it replaces, not
lossier. Runs are flushed when a different frame arrives, when the log is read
or cleared, and every 60 seconds regardless, so a long idle stretch is never
wholly absent.

If you have tooling that parses the capture format, it needs to handle
`~repeat` lines.

### Recipe verbs are refused on the raw command path

`SENDCANDY` carries one quoted CSV plus a counter. The generic field builder —
used by `/api/cmd` and the MQTT command topic — splits arguments on commas and
emits each as its own field:

```
want: SENDCANDY "4,70,140,150,160,300,7200,300,CANDY,0," 100001
got:  SENDCANDY 4 70 140 150 160 300 7200 300 CANDY 0
```

That is not a frame the dryer rejects. It is a **different, still valid recipe**,
which on a machine about to run sets temperatures and times nobody chose, and
reports success. Beta refuses `SENDCANDY`, `SENDCUSTOM`, `SENDBATCH` and
`SENDSCIENCE` on that path rather than reshaping them.

The validated route — Settings → Recipes, and `/api/recipes/send` and
`/api/recipes/apply` — is unchanged and remains the way to send one.

### The freeze estimate follows the recent rate

`main` measures seconds-per-percent from the first observation to the latest,
which is a whole-run average. Freezing decelerates as the chamber approaches its
target, so that average keeps reporting the early, fast rate long after the
machine has stopped achieving it.

Beta measures across the last six percent observations, falling back to the
whole-run figure until that fills so an estimate still appears early.

This is **not** the curve fit from the roadmap. Fitting Newton's law of cooling
needs a captured freeze curve to choose a model against, and none has been
recorded yet. This is the model-free improvement that can be made honestly in
the meantime, and it will still under-predict while deceleration continues.

---

## Reporting

Include the firmware version from Settings → About (it will end in `-beta`), and
attach `/api/log` and `/api/capture` if the problem involves the dryer link.
Say plainly that you were on beta — the changes above alter what the logs look
like, and a `~repeat` line in a bug report from someone who thought they were on
`main` wastes everybody's time.
