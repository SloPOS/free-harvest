/*
 * hr_enc - decoder for the 6.0.644170 ")S" encoded transport.
 *
 * Fixtures are real REQINFO frames captured from a dryer on 6.0.644170 (thanks
 * to vskiwi, who discovered the encoded transport). REQINFO frames carry no
 * device identity - their plaintext is literally "REQINFO," - so they are safe
 * to embed. Each must decode to exactly that, which exercises the framing, the
 * base64, the nonce, the key schedule and the full keystream.
 */
#include "hr_enc.h"
#include "test_util.h"

#include <string.h>

/* Four distinct REQINFO frames (different nonces) from the 15-minute capture. */
static const char *const REQINFO_FRAMES[] = {
    ")S#7M<,,(OV)449.H8K_",
    ")S#7NGO`$:Oa[6DWOU,?",
    ")S#7N.CP0%NBEAB)WBI4",
    ")S#7RCG\\%?3aBZX1LUQ#",   /* the backslash is one payload char */
};

static void test_reqinfo_frames_decode(void)
{
    char out[64];
    for (size_t i = 0; i < sizeof(REQINFO_FRAMES) / sizeof(REQINFO_FRAMES[0]);
         i++) {
        const char *f = REQINFO_FRAMES[i];
        int n = hr_enc_decode(f, strlen(f), out, sizeof(out));
        CHECK_INT(n, 8);
        CHECK_STR(out, "REQINFO,");
    }
}

static void test_frame_length_and_gate(void)
{
    /* ')S' '#' '7' -> (0x23-0x23)*64 + (0x37-0x23) = 20 */
    CHECK_INT((int)hr_enc_frame_len(")S#7xxxxxxxxxxxxxxxx", 20), 20);
    /* '$' 'C' -> 1*64 + (0x43-0x23) = 96 */
    CHECK_INT((int)hr_enc_frame_len(")S$C", 4), 96);
    /* not a frame */
    CHECK_INT((int)hr_enc_frame_len("STAT,1", 6), 0);

    CHECK(hr_enc_is_frame(")S", 2));
    CHECK(hr_enc_is_frame(")S#7", 4));
    CHECK(!hr_enc_is_frame("ST", 2));
    CHECK(!hr_enc_is_frame(")", 1));
}

static void test_concatenated_frames_split(void)
{
    /* The dryer sends frames back-to-back with no separator; a framer must
     * split on the declared length. Two REQINFO frames concatenated: */
    char buf[64];
    buf[0] = '\0';
    strcat(buf, REQINFO_FRAMES[0]);
    strcat(buf, REQINFO_FRAMES[1]);
    size_t total = strlen(buf);

    size_t l0 = hr_enc_frame_len(buf, total);
    CHECK_INT((int)l0, 20);

    char out[64];
    CHECK_INT(hr_enc_decode(buf, l0, out, sizeof(out)), 8);
    CHECK_STR(out, "REQINFO,");

    size_t rem = total - l0;
    size_t l1 = hr_enc_frame_len(buf + l0, rem);
    CHECK_INT((int)l1, 20);
    CHECK_INT(hr_enc_decode(buf + l0, l1, out, sizeof(out)), 8);
    CHECK_STR(out, "REQINFO,");
}

static void test_malformed_is_rejected(void)
{
    char out[64];
    /* wrong prefix */
    CHECK_INT(hr_enc_decode("XY#7M<,,(OV)449.H8K_", 20, out, sizeof(out)), -1);
    /* declared length longer than the buffer */
    CHECK_INT(hr_enc_decode(")Sb b", 5, out, sizeof(out)), -1);
    /* out buffer too small for the plaintext */
    char tiny[4];
    CHECK_INT(hr_enc_decode(REQINFO_FRAMES[0], 20, tiny, sizeof(tiny)), -1);
    /* a space is outside the alphabet */
    CHECK_INT(hr_enc_decode(")S#7M<,,(OV)44 .H8K_", 20, out, sizeof(out)), -1);
}

int main(void)
{
    test_reqinfo_frames_decode();
    test_frame_length_and_gate();
    test_concatenated_frames_split();
    test_malformed_is_rejected();
    return TEST_REPORT();
}
