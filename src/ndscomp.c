#include "ndsrom/ndsgfx.h"

#include <stdlib.h>
#include <string.h>

#define MAX_UNPACKED (128u * 1024u * 1024u)

int nds_is_compressed(const uint8_t *d, size_t n) {
    return n >= 4 && (d[0] == 0x10 || d[0] == 0x11 || d[0] == 0x30 ||
                       d[0] == 0x40);
}

static NdsResult rle(const uint8_t *in, size_t n, size_t pos, uint8_t *out, uint32_t osz) {
    uint32_t o = 0;
    while (o < osz) {
        if (pos >= n) return NDS_ERR_FORMAT;
        uint8_t f = in[pos++];
        uint32_t len = (f & 0x7F) + ((f & 0x80) ? 3u : 1u);
        if (len > osz - o) return NDS_ERR_FORMAT;
        if (f & 0x80) {
            if (pos >= n) return NDS_ERR_FORMAT;
            memset(out + o, in[pos++], len);
        } else {
            if (len > n - pos) return NDS_ERR_FORMAT;
            memcpy(out + o, in + pos, len);
            pos += len;
        }
        o += len;
    }
    return NDS_OK;
}

static NdsResult lz(const uint8_t *in, size_t n, size_t pos, uint8_t *out,
                    uint32_t osz, int ext) {
    uint32_t o = 0;
    while (o < osz) {
        if (pos >= n) return NDS_ERR_FORMAT;
        uint8_t flags = in[pos++];
        for (int b = 7; b >= 0 && o < osz; b--) {
            if (!(flags & (1 << b))) {
                if (pos >= n) return NDS_ERR_FORMAT;
                out[o++] = in[pos++];
                continue;
            }
            if (pos + 2 > n) return NDS_ERR_FORMAT;
            uint32_t len, disp;
            uint8_t b0 = in[pos], b1 = in[pos + 1];
            unsigned ind = ext ? b0 >> 4 : 2;
            if (ind == 0) {
                if (pos + 3 > n) return NDS_ERR_FORMAT;
                len = (((uint32_t)(b0 & 0xF) << 4) | (b1 >> 4)) + 0x11;
                disp = (((uint32_t)(b1 & 0xF) << 8) | in[pos + 2]) + 1;
                pos += 3;
            } else if (ind == 1) {
                if (pos + 4 > n) return NDS_ERR_FORMAT;
                len = (((uint32_t)(b0 & 0xF) << 12) | ((uint32_t)b1 << 4) |
                       (in[pos + 2] >> 4)) + 0x111;
                disp = (((uint32_t)(in[pos + 2] & 0xF) << 8) | in[pos + 3]) + 1;
                pos += 4;
            } else {
                len = (ext ? ind : (uint32_t)(b0 >> 4)) + (ext ? 1u : 3u);
                disp = (((uint32_t)(b0 & 0xF) << 8) | b1) + 1;
                pos += 2;
            }
            if (disp > o || len > osz - o) return NDS_ERR_FORMAT;
            for (uint32_t i = 0; i < len; i++, o++) out[o] = out[o - disp];
        }
    }
    return NDS_OK;
}

/* Golden Sun: Dark Dawn's LZ variant. The flag byte is stored negated (MSB first,
   1 = back-reference). A reference is disp = (b0 >> 4) | (b1 << 4); the low nibble
   of b0 is the length, except 0 (next byte + 0x10) and 1 (next two bytes + 0x110). */
static NdsResult lz40(const uint8_t *in, size_t n, size_t pos, uint8_t *out, uint32_t osz) {
    uint32_t o = 0;
    while (o < osz) {
        if (pos >= n) return NDS_ERR_FORMAT;
        unsigned flags = (0x100u - in[pos++]) & 0xFF;
        for (int b = 7; b >= 0 && o < osz; b--) {
            if (!(flags & (1u << b))) {
                if (pos >= n) return NDS_ERR_FORMAT;
                out[o++] = in[pos++];
                continue;
            }
            if (pos + 2 > n) return NDS_ERR_FORMAT;
            uint8_t b0 = in[pos], b1 = in[pos + 1];
            uint32_t disp = (b0 >> 4) | ((uint32_t)b1 << 4), len = b0 & 0xF;
            pos += 2;
            if (len == 0) {
                if (pos >= n) return NDS_ERR_FORMAT;
                len = in[pos++] + 0x10u;
            } else if (len == 1) {
                if (pos + 2 > n) return NDS_ERR_FORMAT;
                len = (((uint32_t)in[pos + 1] << 8) | in[pos]) + 0x110u;
                pos += 2;
            }
            if (disp == 0 || disp > o) return NDS_ERR_FORMAT;
            if (len > osz - o) len = osz - o; /* the final copy may overrun */
            for (uint32_t i = 0; i < len; i++, o++) out[o] = out[o - disp];
        }
    }
    return NDS_OK;
}

NdsResult nds_decompress(const uint8_t *in, size_t n, uint8_t **out, uint32_t *out_size) {
    if (!in || !out || !out_size) return NDS_ERR_ARG;
    if (!nds_is_compressed(in, n)) return NDS_ERR_FORMAT;
    uint32_t osz = in[1] | ((uint32_t)in[2] << 8) | ((uint32_t)in[3] << 16);
    size_t pos = 4;
    if (osz == 0) {
        if (n < 8) return NDS_ERR_FORMAT;
        osz = in[4] | ((uint32_t)in[5] << 8) | ((uint32_t)in[6] << 16) |
              ((uint32_t)in[7] << 24);
        pos = 8;
    }
    if (osz == 0 || osz > MAX_UNPACKED) return NDS_ERR_FORMAT;

    uint8_t *buf = malloc(osz);
    if (!buf) return NDS_ERR_NOMEM;
    NdsResult r = in[0] == 0x30   ? rle(in, n, pos, buf, osz)
                  : in[0] == 0x40 ? lz40(in, n, pos, buf, osz)
                                  : lz(in, n, pos, buf, osz, in[0] == 0x11);
    if (r != NDS_OK) {
        free(buf);
        return r;
    }
    *out = buf;
    *out_size = osz;
    return NDS_OK;
}

NdsResult nds_load_file_unpacked(const NdsRom *rom, const NdsFile *file,
                                 uint8_t **data, uint32_t *size, int *was_compressed) {
    uint8_t *raw;
    uint32_t rsz;
    NdsResult r = nds_load_file(rom, file, &raw, &rsz);
    if (r != NDS_OK) return r;

    uint8_t *un;
    uint32_t usz;
    if (nds_is_compressed(raw, rsz) && nds_decompress(raw, rsz, &un, &usz) == NDS_OK) {
        free(raw);
        *data = un;
        *size = usz;
        if (was_compressed) *was_compressed = 1;
        return NDS_OK;
    }
    *data = raw;
    *size = rsz;
    if (was_compressed) *was_compressed = 0;
    return NDS_OK;
}
