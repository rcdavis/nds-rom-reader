#include "ndsrom/ndsgfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HASH_SIZE 5003

typedef struct {
    FILE *f;
    uint8_t block[255];
    int len;
    uint32_t acc;
    int bits;
    int err;
} Bits;

static void flush_block(Bits *b) {
    if (!b->len) return;
    uint8_t n = (uint8_t)b->len;
    if (fwrite(&n, 1, 1, b->f) != 1 || fwrite(b->block, 1, (size_t)b->len, b->f) != (size_t)b->len)
        b->err = 1;
    b->len = 0;
}

static void put_code(Bits *b, uint32_t code, int size) {
    b->acc |= code << b->bits;
    b->bits += size;
    while (b->bits >= 8) {
        b->block[b->len++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->bits -= 8;
        if (b->len == 255) flush_block(b);
    }
}

static void lzw(Bits *b, const uint8_t *idx, size_t n) {
    const int clear = 256, eoi = 257;
    int32_t keys[HASH_SIZE];
    uint16_t vals[HASH_SIZE];
    int next = 258, size = 9;
    for (int i = 0; i < HASH_SIZE; i++) keys[i] = -1;

    put_code(b, clear, size);
    uint32_t prefix = idx[0];
    for (size_t i = 1; i < n; i++) {
        uint8_t c = idx[i];
        int32_t key = (int32_t)(prefix << 8 | c);
        uint32_t h = ((uint32_t)key * 2654435761u) % HASH_SIZE;
        while (keys[h] != -1 && keys[h] != key) h = (h + 1) % HASH_SIZE;
        if (keys[h] == key) { prefix = vals[h]; continue; }

        put_code(b, prefix, size);
        if (next < 4096) {
            keys[h] = key;
            vals[h] = (uint16_t)next;
            if (next == (1 << size) && size < 12) size++;
            next++;
        } else {
            put_code(b, clear, size);
            for (int k = 0; k < HASH_SIZE; k++) keys[k] = -1;
            next = 258;
            size = 9;
        }
        prefix = c;
    }
    put_code(b, prefix, size);
    put_code(b, eoi, size);
    if (b->bits) put_code(b, 0, 8 - b->bits);
    flush_block(b);
}

static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* Colors are collected across all frames; index 0 is transparent. */
NdsResult nds_write_gif(const char *path, const NdsAnimation *a) {
    if (!path || !a || !a->rgba || !a->count || !a->width || !a->height ||
        a->width > 0xFFFF || a->height > 0xFFFF)
        return NDS_ERR_ARG;
    size_t npix = (size_t)a->width * a->height * a->count;
    uint8_t *idx = malloc(npix);
    if (!idx) return NDS_ERR_NOMEM;

    uint32_t colors[256];
    int ncolors = 1;
    int32_t table[HASH_SIZE];
    uint8_t table_idx[HASH_SIZE];
    for (int i = 0; i < HASH_SIZE; i++) table[i] = -1;
    for (size_t i = 0; i < npix; i++) {
        const uint8_t *p = a->rgba + i * 4;
        if (p[3] < 128) { idx[i] = 0; continue; }
        uint32_t rgb = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        uint32_t h = (rgb * 2654435761u) % HASH_SIZE;
        while (table[h] != -1 && (uint32_t)table[h] != rgb) h = (h + 1) % HASH_SIZE;
        if (table[h] == -1) {
            if (ncolors >= 256) { free(idx); return NDS_ERR_FORMAT; }
            table[h] = (int32_t)rgb;
            table_idx[h] = (uint8_t)ncolors;
            colors[ncolors++] = rgb;
        }
        idx[i] = table_idx[h];
    }

    FILE *f = fopen(path, "wb");
    if (!f) { free(idx); return NDS_ERR_IO; }
    uint8_t hdr[13 + 768];
    memcpy(hdr, "GIF89a", 6);
    w16(hdr + 6, a->width);
    w16(hdr + 8, a->height);
    hdr[10] = 0xF7; /* global palette, 256 entries */
    hdr[11] = hdr[12] = 0;
    memset(hdr + 13, 0, 768);
    for (int i = 1; i < ncolors; i++) {
        hdr[13 + i * 3] = (uint8_t)(colors[i] >> 16);
        hdr[14 + i * 3] = (uint8_t)(colors[i] >> 8);
        hdr[15 + i * 3] = (uint8_t)colors[i];
    }
    int ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr;
    if (ok && a->loop) {
        static const uint8_t ext[] = {0x21, 0xFF, 0x0B, 'N','E','T','S','C','A','P','E','2','.','0',
                                      3, 1, 0, 0, 0};
        ok = fwrite(ext, 1, sizeof ext, f) == sizeof ext;
    }

    size_t frame_px = (size_t)a->width * a->height;
    for (uint32_t i = 0; i < a->count && ok; i++) {
        unsigned delay = ((unsigned)a->duration[i] * 100 + 30) / 60;
        if (delay < 2) delay = 2;
        uint8_t gce[8] = {0x21, 0xF9, 4, 0x09, 0, 0, 0, 0}; /* dispose to background, transparent */
        w16(gce + 4, delay);
        uint8_t desc[10] = {0x2C};
        w16(desc + 5, a->width);
        w16(desc + 7, a->height);
        uint8_t min_code = 8;
        ok = fwrite(gce, 1, 8, f) == 8 && fwrite(desc, 1, 10, f) == 10 &&
             fwrite(&min_code, 1, 1, f) == 1;
        Bits b = {.f = f};
        lzw(&b, idx + i * frame_px, frame_px);
        uint8_t zero = 0;
        ok = ok && !b.err && fwrite(&zero, 1, 1, f) == 1;
    }
    if (ok) { uint8_t t = 0x3B; ok = fwrite(&t, 1, 1, f) == 1; }
    free(idx);
    if (fclose(f) != 0) ok = 0;
    return ok ? NDS_OK : NDS_ERR_IO;
}
