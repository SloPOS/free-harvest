# The 6.0.644170 encoded USB transport

Dryer firmware **6.0.644170** does not go silent, the way it first appeared to.
After the adapter sends `UNIQUE lH`, the dryer flips its *outgoing* side to an
encoded transport: it still accepts our plaintext commands, but every reply and
every spontaneous message now arrives as a `)S` frame instead of a plaintext,
`\r`-terminated line. Earlier releases read that stream as garbage and reported
the machine as unresponsive.

**Credit:** the encoded transport was discovered by **vskiwi**, who caught the
dryer answering over it, characterised the `)S` framing and length encoding, and
captured the datasets this decode was built and validated against. The framing
and capture plumbing are their work (`)S`+length framer, `e` capture records,
`/api/enc`). This document adds the payload codec on top of that.

Everything below is an interoperability description of a wire format, derived
from observed behaviour and validated against captures. No vendor firmware,
code, or strings are reproduced here.

## Framing

```
 ')' 'S' L1 L2   <payload>
```

- `L1 L2` encode the whole-frame length in characters: `L = (L1-0x23)*64 + (L2-0x23)`.
- The payload is base64 in a 64-character alphabet offset by `0x23`: a character
  `c` carries the 6-bit value `c - 0x23`, so the alphabet runs `#`(0x23) through
  `b`(0x62). `!`(0x21) appears only as trailing padding and counts as 0.
- Four characters decode to three bytes, most-significant first.
- Frames carry no terminator and are sent back-to-back with no separator, so a
  reader splits them purely on the declared length.

Observed frame sizes and their plaintext counterparts:

| Header | L | plaintext |
|---|---|---|
| `)S#7` | 20 | `REQINFO,` |
| `)S#G` | 36 | `SNM,…` |
| `)S$+` | 72 | `CFG,…` |
| `)S$3` | 80 | periodic `STAT,…` |
| `)S$C` | 96 | `UID,…` |

## Decoded payload

The decoded bytes are:

```
 [ 3-byte header / nonce ] [ body ]
```

The body is the plaintext line (including its trailing `\r`) plus a small amount
of padding. The three header bytes, taken most-significant-first as one 24-bit
value, are the **nonce** that seeds the cipher for that frame.

## Cipher

The body is the plaintext XORed with a keystream. The keystream comes from a
xorshift128-style generator over four 32-bit words, seeded from the nonce alone.
In every frame observed, the seed used only the nonce and fixed constants — no
device-specific value — so decoding needs nothing beyond the frame itself:

```
seed:   s0 = 2 ^ nonce      s1 = 1
        s2 = ~nonce ^ 4      s3 = 3

per output byte:
        t   = s0 ^ (s0 << 23)
        s1n = s1 ^ ((s1 << 23) | (s0 >> 9))
        nw  = ((s2 >> 26) | (s3 << 6)) ^ s2 ^ t ^ ((t >> 17) | (s1n << 15))
        v10 = (s3 ^ (s3 >> 26) ^ s1n) ^ (s1n >> 17)
        keystream_byte = (nw + s2) & 0xff
        (s0, s1, s2, s3) = (s2, s3, nw, v10)

plaintext_byte = body_byte ^ keystream_byte,   stopping at the first '\r'
```

The nonce itself is a rolling counter mixed with a 16-bit hash of the payload;
because it is transmitted in the frame, a decoder never has to reconstruct it —
it just reads it and runs the keystream.

The firmware also defines a second seed schedule that additionally mixes in the
dryer's 128-bit UniqueID (the four groups it sends in its `UID,` line). It was
not used by any captured frame, but because that UniqueID is transmitted in the
clear during the handshake, an adapter could derive it too if a dryer ever
selects it. The decoder tries the nonce-only schedule first.

## Validation

The decoder was checked against vskiwi's 15-minute live capture: **454 of 454
frames** decoded to a clean, `\r`-terminated plaintext line — 201 `REQINFO`, 132
`STAT`, 60 `SNM`, 60 `CFG`, 1 `UID` — with the recovered `UID`/`CFG` lines
matching the dryer's known serial. The four `REQINFO` fixtures in
`test/test_hr_enc.c` are a subset of that set (they carry no identity).

## Using it

Because the dryer still accepts our plaintext commands, this is **receive-side
only**: the adapter keeps sending plaintext, detects `)S` frames on the RX path,
decodes each one back to its plaintext line with `hr_enc_decode()`, and feeds
that line to the existing parser. Nothing about the transmit path changes.

The encoded mode is sticky once entered and only clears on a USB re-attach
(adapter reboot or dryer power cycle), so a session that has flipped to it stays
encoded until then.
