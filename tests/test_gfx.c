#include "ndsrom/ndsgfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void test_rle(void) {
    /* 5 literals "hello", then a run of 4 'x' */
    const uint8_t in[] = {0x30, 9, 0, 0, 0x04, 'h', 'e', 'l', 'l', 'o', 0x81, 'x'};
    uint8_t *out;
    uint32_t n;
    CHECK(nds_decompress(in, sizeof in, &out, &n) == NDS_OK);
    CHECK(n == 9 && memcmp(out, "helloxxxx", 9) == 0);
    free(out);
    CHECK(nds_decompress(in, sizeof in - 1, &out, &n) == NDS_ERR_FORMAT);
}

static void test_lz(void) {
    /* LZ10: literal 'a','b', then copy 4 bytes from distance 2 -> "ababab" */
    const uint8_t in[] = {0x10, 6, 0, 0, 0x20, 'a', 'b', 0x10, 0x01};
    uint8_t *out;
    uint32_t n;
    CHECK(nds_decompress(in, sizeof in, &out, &n) == NDS_OK);
    CHECK(n == 6 && memcmp(out, "ababab", 6) == 0);
    free(out);

    /* LZ11 short form: len = indicator + 1 (here 3) */
    const uint8_t in11[] = {0x11, 5, 0, 0, 0x20, 'a', 'b', 0x20, 0x01};
    CHECK(nds_decompress(in11, sizeof in11, &out, &n) == NDS_OK);
    CHECK(n == 5 && memcmp(out, "ababa", 5) == 0);
    free(out);
}

static void test_render(void) {
    /* One-colour-pair palette: index 1 = pure red (BGR555 0x001F). */
    const uint8_t pal[] = {'R', 'L', 'C', 'N', 0xFF, 0xFE, 0, 1, 0x2C, 0, 0, 0, 0x10, 0, 1, 0,
                           'T', 'T', 'L', 'P', 0x1C, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0,
                           4, 0, 0, 0, 0x10, 0, 0, 0, 0x00, 0x00, 0x1F, 0x00};
    NdsPalette p;
    CHECK(nds_palette_parse(pal, sizeof pal, &p) == NDS_OK);
    CHECK(p.count == 2 && p.rgba[4] == 255 && p.rgba[5] == 0);

    /* One 4bpp tile, every pixel = index 1. */
    uint8_t gfx[0x30 + 32] = {'R', 'G', 'C', 'N', 0xFF, 0xFE, 1, 1, 0x50, 0, 0, 0, 0x10, 0, 1, 0,
                              'R', 'A', 'H', 'C', 0x40, 0, 0, 0, 1, 0, 1, 0, 3, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0, 0, 0, 0x18, 0, 0, 0};
    memset(gfx + 0x30, 0x11, 32);
    NdsTileSet ts;
    NdsImage img;
    CHECK(nds_tileset_parse(gfx, sizeof gfx, &ts) == NDS_OK);
    CHECK(nds_render_tiles(&ts, &p, 0, 0, &img) == NDS_OK);
    CHECK(img.width == 8 && img.height == 8 && img.rgba[0] == 255 && img.rgba[3] == 255);
    CHECK(nds_write_png("test_out.png", &img) == NDS_OK);
    remove("test_out.png");
    nds_image_free(&img);
    nds_tileset_free(&ts);
    nds_palette_free(&p);
}

int main(void) {
    test_rle();
    test_lz();
    test_render();
    puts(failures ? "FAILED" : "all tests passed");
    return failures != 0;
}
