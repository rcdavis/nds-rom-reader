#include "ndsrom/ndssdat.h"
#include "ndsrom/ndstext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void test_text(void) {
    /* Two entries: "Hi 5" + newline + name placeholder, and "ok" */
    const uint16_t body[] = {0x50 + 7, 0x6A + 8, 0x00, 0x05, 0xFFFD, 0xFFEC, 0xFFFF,
                             0x6A + 14, 0x6A + 10, 0xFFFF};
    uint8_t buf[8 + sizeof body];
    uint32_t o0 = 8, o1 = 8 + 7 * 2;
    memcpy(buf, &o0, 4);
    memcpy(buf + 4, &o1, 4);
    memcpy(buf + 8, body, sizeof body);

    NdsMsgTable t;
    CHECK(nds_msgtable_parse(buf, sizeof buf, &t) == NDS_OK);
    CHECK(t.count == 2 && t.len[0] == 6 && t.len[1] == 2);
    char *s = nds_msg_decode(t.codes[0], t.len[0]);
    CHECK(s && strcmp(s, "Hi 5\n{name}") == 0);
    free(s);
    s = nds_msg_decode(t.codes[1], t.len[1]);
    CHECK(s && strcmp(s, "ok") == 0);
    free(s);
    nds_msgtable_free(&t);

    uint32_t bad = 1000; /* first offset is beyond the data */
    memcpy(buf, &bad, 4);
    CHECK(nds_msgtable_parse(buf, sizeof buf, &t) == NDS_ERR_FORMAT);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static void test_swar(void) {
    uint8_t d[0x3C + 4 + 12 + 8];
    memset(d, 0, sizeof d);
    memcpy(d, "SWAR", 4);
    memcpy(d + 0x10, "DATA", 4);
    put32(d + 0x38, 1);
    put32(d + 0x3C, 0x40);
    uint8_t *w = d + 0x40;
    w[0] = 0;                 /* PCM8 */
    w[1] = 1;                 /* loops */
    w[2] = 0x22; w[3] = 0x56; /* 22050 Hz */
    w[6] = 1;                 /* loop starts after one word */
    w[8] = 1;                 /* one word of loop data */
    const int8_t pcm[8] = {0, 10, -10, 127, -128, 1, 2, 3};
    memcpy(w + 12, pcm, 8);

    uint32_t count;
    CHECK(nds_swar_count(d, sizeof d, &count) == NDS_OK && count == 1);
    NdsAudio a;
    CHECK(nds_swar_decode(d, sizeof d, 0, &a) == NDS_OK);
    CHECK(a.rate == 22050 && a.channels == 1 && a.frames == 8);
    CHECK(a.pcm[1] == 10 * 256 && a.pcm[3] == 127 * 256 && a.pcm[4] == -128 * 256);
    CHECK(a.loop && a.loop_start == 4);
    nds_audio_free(&a);
    CHECK(nds_swar_decode(d, sizeof d, 1, &a) == NDS_ERR_NOTFOUND);
    CHECK(nds_swar_decode(d, 8, 0, &a) == NDS_ERR_FORMAT);
}

static void test_adpcm(void) {
    /* ADPCM header (predictor 100, step index 0) then nibbles of 4, each adding
       step + (step >> 3) = 7 + 0 at step 7. */
    uint8_t d[0x40 + 12 + 8];
    memset(d, 0, sizeof d);
    memcpy(d, "SWAR", 4);
    memcpy(d + 0x10, "DATA", 4);
    put32(d + 0x38, 1);
    put32(d + 0x3C, 0x40);
    uint8_t *w = d + 0x40;
    w[0] = 2;
    w[2] = 0x40; w[3] = 0x1F; /* 8000 Hz */
    w[8] = 2;                 /* two words: header + 4 data bytes */
    w[12] = 100; w[14] = 0;   /* predictor 100, step index 0 */
    w[16] = 0x44;             /* two nibbles of 4: each adds step + step>>3 */
    NdsAudio a;
    CHECK(nds_swar_decode(d, sizeof d, 0, &a) == NDS_OK);
    CHECK(a.frames == 8);
    CHECK(a.pcm[0] == 100 + 7);
    CHECK(a.pcm[1] > a.pcm[0]);
    nds_audio_free(&a);
}

int main(void) {
    test_text();
    test_swar();
    test_adpcm();
    puts(failures ? "FAILED" : "all tests passed");
    return failures != 0;
}
