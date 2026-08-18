**Point fix on top of [v0.3.4](https://github.com/SloPOS/free-harvest/releases/tag/v0.3.4). Update over Wi-Fi — no cable needed.**

If v0.3.4 is already installed, just upload `hr_wifi_adapter.bin` via
**Settings → Firmware update**. The other three files are only needed for a
first-time install from an older version; see the v0.3.4 notes for that.

## Fixed: web UI unreachable from a phone

Connecting from a mobile browser could fail with nothing on screen and this in
the device log:

```
W httpd: httpd_server: error accepting new connection
E httpd: httpd_accept_conn: error in accept (23)
```

errno 23 is `ENFILE` — "too many open files in system". The lwIP descriptor
table was exhausted, so `accept()` had no file descriptor left to give an
incoming connection.

`esp_http_server` requires `max_open_sockets + 3 <= CONFIG_LWIP_MAX_SOCKETS`.
We ran `max_open_sockets = 7` against the IDF default of 10, so **7 + 3 = 10 of
10 — the web server claimed the entire socket table.** That configuration is
perfectly legal and the server starts without complaint, but once seven clients
are connected there is no spare descriptor, and `accept()` fails with `ENFILE`
*before* `lru_purge_enable` can reclaim a slot.

**Why it only showed up on a phone.** The page runs six poll timers — frames,
state, verbs, MQTT, capture, trend. On a desktop LAN each request finishes well
before the next tick. On a phone the higher latency lets them overlap, so the
browser opens extra parallel connections and saturates all seven slots. Same
firmware, same network — only the round-trip time differs.

Two changes, one for each half of the cause:

- **`CONFIG_LWIP_MAX_SOCKETS` 10 → 16**, leaving six spare descriptors for
  `accept()`, MQTT, DNS and the captive-portal responder. A `_Static_assert` now
  encodes the invariant *including* that spare capacity, so a future config
  change becomes a build failure with an explanation rather than an errno in the
  field.
- **The web UI no longer starts a poll while its own previous request is still in
  flight**, so requests cannot stack up on a slow link.

## Note

This is **not** the `/api/trend` socket leak fixed in v0.3.4, which was unchecked
chunk sends leaving responses unterminated. Two independent socket faults with
the same symptom — which is why v0.3.4 did not resolve this one.

**v0.3.4 is not withdrawn.** It works fine for a single client; this release
matters if you use the app from a phone, or from more than one device.

Full detail in [CHANGELOG.md](CHANGELOG.md).
