/*
 * hr_enc - see hr_enc.h. Decoder for the 6.0.644170 ")S" encoded transport.
 *
 * Recovered from the dryer image and validated byte-for-byte against a 454-frame
 * live capture: every REQINFO/SNM/CFG/STAT/UID frame reproduced its exact
 * plaintext. The keystream generator and key schedule below are a clean
 * reimplementation of the observed algorithm, written from the wire behaviour -
 * no vendor code is reproduced.
 */
#include "hr_enc.h"

#include <string.h>

/* ---- base64 in the '#'..'b' alphabet ----------------------------------- */
/* char - 0x23 gives the 6-bit value; '!' (0x21) is padding, worth 0. Anything
 * else is invalid. Returns 0..63, or -1 on an out-of-alphabet char. */
static int b64_val(unsigned char c)
{
    if (c == '!') {
        return 0;
    }
    unsigned v = (unsigned)(c - 0x23);
    return (v < 64u) ? (int)v : -1;
}

size_t hr_enc_frame_len(const char *buf, size_t len)
{
    if (len < 4 || buf[0] != HR_ENC_PREFIX0 || buf[1] != HR_ENC_PREFIX1) {
        return 0;
    }
    int l1 = b64_val((unsigned char)buf[2]);
    int l2 = b64_val((unsigned char)buf[3]);
    if (l1 < 0 || l2 < 0) {
        return 0;
    }
    return (size_t)(l1 * 64 + l2);
}

/* ---- the stream cipher -------------------------------------------------- */
/*
 * A xorshift128-style state of four 32-bit words, seeded from the 24-bit nonce
 * carried in each frame's 3-byte header. Every frame in the captured traffic
 * used the "state 1" schedule below, whose seed is the nonce alone - no device
 * key - which is why decoding needs nothing but the frame itself.
 */
typedef struct {
    uint32_t s0, s1, s2, s3;
} cipher_t;

static void cipher_init(cipher_t *c, uint32_t nonce)
{
    /* state-1 key schedule: constants mixed with the per-frame nonce. */
    c->s0 = 2u ^ nonce;      /* k0 */
    c->s1 = 1u;              /* k1 */
    c->s2 = (~nonce) ^ 4u;   /* k2 */
    c->s3 = 3u;              /* k3 */
}

/* Advance the state one step and return the next keystream byte. */
static uint8_t cipher_next(cipher_t *c)
{
    uint32_t s0 = c->s0, s1 = c->s1, s2 = c->s2, s3 = c->s3;

    uint32_t t   = s0 ^ (s0 << 23);
    uint32_t s1n = s1 ^ ((s1 << 23) | (s0 >> 9));
    uint32_t nw  = ((s2 >> 26) | (s3 << 6)) ^ s2 ^ t ^ ((t >> 17) | (s1n << 15));
    uint32_t v9  = s3 ^ (s3 >> 26) ^ s1n;
    uint32_t v10 = v9 ^ (s1n >> 17);

    uint8_t ks = (uint8_t)(nw + s2);   /* low byte of (new_word + old s2) */

    c->s0 = s2;
    c->s1 = s3;
    c->s2 = nw;
    c->s3 = v10;
    return ks;
}

/* ---- frame decode ------------------------------------------------------- */
int hr_enc_decode(const char *frame, size_t frame_len, char *out, size_t outcap)
{
    if (frame == NULL || out == NULL || outcap == 0) {
        return -1;
    }
    size_t declared = hr_enc_frame_len(frame, frame_len);
    if (declared < 8 || declared > frame_len) {
        return -1;   /* need header + at least the nonce group */
    }

    /* Decode the payload (everything after ')S' L1 L2) base64 -> bytes,
     * straight into the caller's buffer in two roles: first 3 bytes are the
     * nonce, the rest is the ciphertext body we overwrite with plaintext. */
    const char *pay = frame + 4;
    size_t nchars = declared - 4;
    if ((nchars & 3u) != 0) {
        return -1;   /* payload must be whole 4-char groups */
    }
    size_t nbytes = (nchars / 4) * 3;
    if (nbytes < 4 || nbytes - 3 >= outcap) {
        return -1;   /* need a nonce + some body, and room to NUL-terminate */
    }

    /* First group -> the 3 nonce bytes. */
    int a = b64_val((unsigned char)pay[0]);
    int b = b64_val((unsigned char)pay[1]);
    int c = b64_val((unsigned char)pay[2]);
    int d = b64_val((unsigned char)pay[3]);
    if ((a | b | c | d) < 0) {
        return -1;
    }
    uint32_t g = ((uint32_t)a << 18) | ((uint32_t)b << 12) |
                 ((uint32_t)c << 6) | (uint32_t)d;
    /* The three header bytes ARE the nonce, as one 24-bit value: the group's
     * decoded bytes are g>>16, g>>8, g, and nonce = (b0<<16)|(b1<<8)|b2 = g. */
    uint32_t nonce = g & 0xffffffu;

    cipher_t ci;
    cipher_init(&ci, nonce);

    /* Remaining groups -> body bytes, XORed with the keystream, stopping at
     * the first '\r'. */
    size_t outn = 0;
    for (size_t p = 4; p < nchars; p += 4) {
        int v0 = b64_val((unsigned char)pay[p]);
        int v1 = b64_val((unsigned char)pay[p + 1]);
        int v2 = b64_val((unsigned char)pay[p + 2]);
        int v3 = b64_val((unsigned char)pay[p + 3]);
        if ((v0 | v1 | v2 | v3) < 0) {
            return -1;
        }
        uint32_t gg = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12) |
                      ((uint32_t)v2 << 6) | (uint32_t)v3;
        uint8_t triplet[3] = {
            (uint8_t)(gg >> 16), (uint8_t)(gg >> 8), (uint8_t)gg
        };
        for (int k = 0; k < 3; k++) {
            uint8_t pt = triplet[k] ^ cipher_next(&ci);
            if (pt == '\r') {
                out[outn] = '\0';
                return (int)outn;
            }
            if (outn + 1 >= outcap) {
                return -1;
            }
            out[outn++] = (char)pt;
        }
    }
    return -1;   /* no '\r' found - not a complete line */
}
