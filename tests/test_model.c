#include "ndsrom/ndsmodel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../src/ndsbuf.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void test_lz40(void) {
    /* flags are stored negated: literals a b c, then copy disp 3 len 5 */
    const uint8_t s[] = {0x40, 8, 0, 0, 0xF0, 'a', 'b', 'c', 0x35, 0x00};
    uint8_t *out;
    uint32_t n;
    CHECK(nds_decompress(s, sizeof s, &out, &n) == NDS_OK);
    CHECK(n == 8 && memcmp(out, "abcabcab", 8) == 0);
    free(out);

    /* long form: length bytes are little-endian, 0x110 + 2 */
    const uint8_t l[] = {0x40, 0x13, 0x01, 0, 0xC0, 'x', 0x11, 0x00, 0x02, 0x00};
    CHECK(nds_decompress(l, sizeof l, &out, &n) == NDS_OK);
    CHECK(n == 275 && out[0] == 'x' && out[274] == 'x');
    free(out);

    const uint8_t bad[] = {0x40, 8, 0, 0, 0x80, 0x35, 0x00};
    CHECK(nds_decompress(bad, sizeof bad, &out, &n) != NDS_OK);
}

static void put16(uint8_t *b, size_t o, unsigned v) { b[o] = v; b[o + 1] = v >> 8; }
static void put32(uint8_t *b, size_t o, uint32_t v) { put16(b, o, v & 0xFFFF); put16(b, o + 2, v >> 16); }

static void test_texture(void) {
    uint8_t f[20 + 0xD8];
    memset(f, 0, sizeof f);
    memcpy(f, "BTX0", 4);
    put16(f, 4, 0xFEFF);
    put32(f, 8, sizeof f);
    put16(f, 12, 16);
    put16(f, 14, 1);
    put32(f, 16, 20);
    uint8_t *t = f + 20;
    memcpy(t, "TEX0", 4);
    put32(t, 4, 0xD8);
    put16(t, 0x0E, 0x40);
    put32(t, 0x14, 0x98);
    put32(t, 0x30, 32 >> 3);
    put32(t, 0x34, 0x6C);
    put32(t, 0x38, 0xB8);

    /* texture dict: one 8-byte entry */
    put16(t, 0x40 + 1, 0); t[0x41] = 1; put16(t, 0x42, 44);
    put16(t, 0x44, 8);
    put16(t, 0x40 + 16, 8); put16(t, 0x40 + 18, 44);
    put16(t, 0x40 + 22, 0xC00);
    memcpy(t + 0x40 + 28, "tex", 3);
    /* palette dict: one 4-byte entry */
    t[0x6C + 1] = 1; put16(t, 0x6C + 2, 40);
    put16(t, 0x6C + 16, 4); put16(t, 0x6C + 18, 40);
    memcpy(t + 0x6C + 24, "tex_pl", 6);

    t[0x98] = 0x21;
    put16(t, 0xB8 + 2, 0x001F);
    put16(t, 0xB8 + 4, 0x03E0);

    NdsTexSet ts;
    CHECK(nds_texset_parse(f, sizeof f, &ts) == NDS_OK);
    CHECK(ts.tex_count == 1 && ts.pal_count == 1);
    CHECK(ts.tex[0].width == 8 && ts.tex[0].height == 8 && ts.tex[0].format == 3);
    CHECK(nds_texset_find_tex(&ts, "tex") == 0 && nds_texset_default_pal(&ts, 0) == 0);
    NdsImage img;
    CHECK(nds_texset_decode(&ts, 0, -1, &img) == NDS_OK);
    CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[3] == 255);
    CHECK(img.rgba[4] == 0 && img.rgba[5] == 255);
    free(img.rgba);
    nds_texset_free(&ts);

    /* truncated input must fail cleanly rather than read out of bounds */
    for (size_t n = 0; n < sizeof f; n += 7) {
        NdsTexSet x;
        if (nds_texset_parse(f, n, &x) == NDS_OK) {
            NdsImage im;
            if (nds_texset_decode(&x, 0, -1, &im) == NDS_OK) free(im.rgba);
            nds_texset_free(&x);
        }
    }
}

static void test_pivot_and_inverse(void) {
    for (unsigned sel = 0; sel < 9; sel++)
        for (unsigned neg = 0; neg < 8; neg++) {
            double m[9];
            if (!(((neg >> 1) ^ (neg >> 2)) & 1)) continue; /* only these sign pairs form a rotation */
            nds_pivot_mat(sel, neg, 0.6, 0.8, m);
            for (int p = 0; p < 3; p++)
                for (int q = 0; q < 3; q++) {
                    double dot = 0;
                    for (int r = 0; r < 3; r++) dot += m[3 * p + r] * m[3 * q + r];
                    CHECK(fabs(dot - (p == q)) < 1e-9);
                }
        }
    const double a[12] = {0, -2, 0, 5, 3, 0, 0, -1, 0, 0, 4, 7}, ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    double inv[12];
    CHECK(nds_affine_inverse(a, inv) == 0);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            double t = 0;
            for (int k = 0; k < 3; k++) t += a[i * 4 + k] * inv[k * 4 + j];
            if (j == 3) t += a[i * 4 + 3];
            CHECK(fabs(t - ident[i * 4 + j]) < 1e-9);
        }
    const double sing[12] = {0};
    CHECK(nds_affine_inverse(sing, inv) != 0);
}

int main(void) {
    test_pivot_and_inverse();
    test_lz40();
    test_texture();
    puts(failures ? "FAILED" : "all tests passed");
    return failures != 0;
}
