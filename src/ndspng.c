#include "ndsrom/ndsgfx.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t crc_table[256];

static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t hdr[8], tail[4];
    be32(hdr, n);
    for (int i = 0; i < 4; i++) hdr[4 + i] = (uint8_t)type[i];
    uint32_t crc = crc_update(0xFFFFFFFFu, hdr + 4, 4);
    if (n) crc = crc_update(crc, d, n);
    be32(tail, ~crc);
    return fwrite(hdr, 1, 8, f) == 8 && (n == 0 || fwrite(d, 1, n, f) == n) &&
           fwrite(tail, 1, 4, f) == 4;
}

/* Writes an RGBA PNG using stored (uncompressed) deflate blocks. */
NdsResult nds_write_png(const char *path, const NdsImage *img) {
    if (!path || !img || !img->rgba || !img->width || !img->height) return NDS_ERR_ARG;
    crc_init();

    size_t stride = (size_t)img->width * 4 + 1;
    size_t raw_len = stride * img->height;
    if (img->width > 0x3FFFFFFFu / 4 || raw_len / stride != img->height) return NDS_ERR_ARG;
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len);
    if (!z) return NDS_ERR_NOMEM;

    size_t zp = 0;
    z[zp++] = 0x78;
    z[zp++] = 0x01;
    uint32_t a = 1, b = 0;
    size_t rp = 0; /* position in the virtual raw stream */
    while (rp < raw_len) {
        size_t n = raw_len - rp > 65535 ? 65535 : raw_len - rp;
        z[zp++] = rp + n == raw_len;
        z[zp++] = (uint8_t)n; z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)~n; z[zp++] = (uint8_t)(~n >> 8);
        for (size_t i = 0; i < n; i++, rp++) {
            size_t col = rp % stride;
            uint8_t v = col == 0 ? 0 : img->rgba[(rp / stride) * (stride - 1) + col - 1];
            z[zp++] = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
    }
    be32(z + zp, (b << 16) | a);
    zp += 4;

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return NDS_ERR_IO; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    be32(ihdr, img->width);
    be32(ihdr + 4, img->height);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    int ok = fwrite(sig, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
             chunk(f, "IDAT", z, (uint32_t)zp) && chunk(f, "IEND", NULL, 0);
    free(z);
    if (fclose(f) != 0) ok = 0;
    return ok ? NDS_OK : NDS_ERR_IO;
}
