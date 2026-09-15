/*
 * hr_enc - decode the ")S" encoded USB transport that dryer firmware 6.0.644170
 * uses for everything it SENDS. The dryer still accepts our plaintext commands,
 * so this is a receive-side decoder only: turn one encoded frame back into the
 * plaintext line the older firmware would have sent, then hand it to the normal
 * parser.
 *
 * The scheme, recovered from the 6.0.644170 image and validated byte-for-byte
 * against 454 captured frames (see decoded/ENCODED_TRANSPORT_644170.md):
 *
 *   frame  = ')' 'S' L1 L2  payload
 *   L      = (L1-0x23)*64 + (L2-0x23)      whole-frame length in chars
 *   payload= base64, alphabet char = value + '#' (0x23..0x62 -> 0..63),
 *            '!' (0x21) is trailing pad; 4 chars -> 3 bytes, MSB first
 *   bytes  = [3-byte nonce] [body]         (body = plaintext incl '\r', + pad)
 *   key    = a fixed schedule seeded by the 24-bit nonce (no device secret)
 *   body   = plaintext XOR keystream, keystream from a xorshift128-style PRNG
 *
 * Nothing here allocates or blocks; it is safe on the USB RX path.
 */
#ifndef HR_ENC_H
#define HR_ENC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A frame begins with these two bytes. */
#define HR_ENC_PREFIX0 ')'
#define HR_ENC_PREFIX1 'S'

/*
 * True if `buf` (at least 2 bytes) starts a ")S" encoded frame. Cheap gate for
 * the RX path: only frames that pass this need hr_enc_decode().
 */
static inline bool hr_enc_is_frame(const char *buf, size_t len)
{
    return len >= 2 && buf[0] == HR_ENC_PREFIX0 && buf[1] == HR_ENC_PREFIX1;
}

/*
 * Whole-frame length (in chars, header included) declared by a frame header,
 * or 0 if the first four bytes are not a valid ")S" + length header. Lets a
 * byte-stream framer split concatenated frames, which the dryer sends with no
 * separator between them.
 */
size_t hr_enc_frame_len(const char *buf, size_t len);

/*
 * Decode one complete ")S" frame into its plaintext line.
 *
 * `frame`/`frame_len` cover the whole frame from ')' through the last payload
 * char. The decoded plaintext (the body, up to but not including the first
 * '\r') is written to `out` and NUL-terminated; the trailing '\r' and any pad
 * bytes are dropped, so `out` is exactly what the plaintext parser expects.
 *
 * Returns the plaintext length (excluding the NUL), or -1 on any malformed
 * input (bad header, bad length, non-alphabet char, body too short, output too
 * small, no '\r' in the body).
 */
int hr_enc_decode(const char *frame, size_t frame_len, char *out, size_t outcap);

#ifdef __cplusplus
}
#endif

#endif /* HR_ENC_H */
