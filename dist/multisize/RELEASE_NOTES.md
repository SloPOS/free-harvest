# Free Harvest 1.0.2 — multi-size test build

> **This is a test release and will probably be deleted.** It exists to try
> flash-size variants against one specific dryer that is not communicating.
> **[v1.0.2](../../releases/tag/v1.0.2) is the real release** — use that unless
> you were sent here deliberately.

**The firmware is byte-for-byte the same code as v1.0.2.** Only the partition
layout differs, so if v1.0.2 already works on your adapter, there is nothing
here for you.

## Pick by flash size, not by module name

| download | flash | app headroom | capture history | modules |
|---|---|---|---|---|
| `free-harvest-1.0.2-4mb.zip` | 4 MB | 13% | ~60 hours | N4R2, any 4 MB S3 |
| `free-harvest-1.0.2-8mb.zip` | 8 MB | 35% | ~200 hours | **N8R2, N8R8 — the usual choice** |
| `free-harvest-1.0.2-16mb.zip` | 16 MB | 35% | ~800 hours | N16R8, N16R2 |

**PSRAM does not matter.** `CONFIG_SPIRAM` is not set and nothing in this
firmware allocates from PSRAM, so an N8R2 and an N8R8 take the identical image.
The R-number in a module name changes nothing here, which is why these are named
by flash size — naming them N8R2 and N16R8 would invent a decision you do not
have to make, and would be wrong for anyone holding an N8R0 or an N16R2.

The compiled code is identical across all three. The images differ in 69 bytes:
the flash-size nibble in the image header, plus build metadata. Behaviour does
not change with flash size — only how much capture history fits.

## Which one do I need?

If you do not know your flash size, read it off the chip:

```
python -m esptool --chip esp32s3 flash_id
```

The `Detected flash size` line is the answer. Most ESP32-S3-DevKitC-1 boards are
8 MB.

## Installing

**Over the air** — Settings → Firmware update, upload the matching
`ota-<size>-hr_wifi_adapter.bin`. Nothing else needed.

**First-time flash over USB** — use the zip; all four files, offsets in the
README inside it. Omitting `ota_data_initial.bin` boots the old image and looks
exactly like a failed flash.

## Caveats

- **Untested on real 4 MB or 16 MB hardware.** These were verified to build,
  fit, and produce a correct partition table. "Boots and talks to a dryer" has
  been confirmed on 8 MB only.
- **Do not cross-flash sizes.** The code is identical, so an 8 MB image would
  probably run on a 16 MB board, but the partition table would be wrong and this
  has not been tested. Match the size.
- Flashing an 8 MB image to a 4 MB board puts the filesystem and the second OTA
  slot past the end of flash. The app may still boot, which makes this
  particularly unpleasant to diagnose.
