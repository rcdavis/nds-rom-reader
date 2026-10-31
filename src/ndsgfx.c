#include "ndsrom/ndsgfx.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_WIDTH_TILES 32
#define MAX_PIXELS (64u * 1024u * 1024u)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* NNS files: 16-byte header, then chunks of [tag(4), size(4), body]. Tags are
   stored byte-reversed (e.g. "RAHC" for CHAR). Returns the chunk start and its
   (clamped) size, or NULL. */
static const uint8_t *find_chunk(const uint8_t *d, size_t n, const char *tag,
                                 uint32_t *csize) {
    if (n < 16) return NULL;
    size_t pos = rd16(d + 12);
    if (pos < 16) pos = 16;
    while (pos + 8 <= n) {
        uint32_t sz = rd32(d + pos + 4);
        if (sz < 8) return NULL;
        if (memcmp(d + pos, tag, 4) == 0) {
            *csize = sz > n - pos ? (uint32_t)(n - pos) : sz;
            return d + pos;
        }
        if (sz > n - pos) return NULL;
        pos += sz;
    }
    return NULL;
}

NdsKind nds_detect_kind(const uint8_t *d, size_t n) {
    if (n < 16) return NDS_KIND_UNKNOWN;
    if (!memcmp(d, "RLCN", 4)) return NDS_KIND_NCLR;
    if (!memcmp(d, "RGCN", 4)) return NDS_KIND_NCGR;
    if (!memcmp(d, "RCSN", 4)) return NDS_KIND_NSCR;
    if (!memcmp(d, "RECN", 4)) return NDS_KIND_NCER;
    if (!memcmp(d, "RNAN", 4)) return NDS_KIND_NANR;
    return NDS_KIND_UNKNOWN;
}

const char *nds_kind_name(NdsKind k) {
    switch (k) {
    case NDS_KIND_NCLR: return "NCLR";
    case NDS_KIND_NCGR: return "NCGR";
    case NDS_KIND_NSCR: return "NSCR";
    case NDS_KIND_NCER: return "NCER";
    case NDS_KIND_NANR: return "NANR";
    default: return "unknown";
    }
}

NdsResult nds_palette_parse(const uint8_t *d, size_t n, NdsPalette *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (nds_detect_kind(d, n) != NDS_KIND_NCLR) return NDS_ERR_FORMAT;
    uint32_t cs;
    const uint8_t *c = find_chunk(d, n, "TTLP", &cs);
    if (!c || cs < 0x18) return NDS_ERR_FORMAT;

    /* Declared data size is unreliable (0 or too large in some files). */
    uint32_t avail = cs - 0x18;
    uint32_t dsz = rd32(c + 0x10);
    if (dsz == 0 || dsz > avail) dsz = avail;
    uint32_t count = dsz / 2;
    if (count == 0) return NDS_ERR_FORMAT;

    out->rgba = malloc((size_t)count * 4);
    if (!out->rgba) return NDS_ERR_NOMEM;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t v = rd16(c + 0x18 + i * 2);
        out->rgba[i * 4 + 0] = (uint8_t)(((v >> 0) & 31) * 255 / 31);
        out->rgba[i * 4 + 1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
        out->rgba[i * 4 + 2] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
        out->rgba[i * 4 + 3] = 255;
    }
    out->count = count;
    out->bpp = rd32(c + 8) == 3 ? 4 : 8;
    return NDS_OK;
}

NdsResult nds_tileset_parse(const uint8_t *d, size_t n, NdsTileSet *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (nds_detect_kind(d, n) != NDS_KIND_NCGR) return NDS_ERR_FORMAT;
    uint32_t cs;
    const uint8_t *c = find_chunk(d, n, "RAHC", &cs);
    if (!c || cs < 0x20) return NDS_ERR_FORMAT;

    uint16_t th = rd16(c + 8), tw = rd16(c + 10);
    uint32_t depth = rd32(c + 12);
    if (depth != 3 && depth != 4) return NDS_ERR_FORMAT;
    uint32_t avail = cs - 0x20;
    uint32_t dsz = rd32(c + 0x18);
    if (dsz > avail) dsz = avail;
    if (dsz == 0) return NDS_ERR_FORMAT;

    out->data = malloc(dsz);
    if (!out->data) return NDS_ERR_NOMEM;
    memcpy(out->data, c + 0x20, dsz);
    out->size = dsz;
    out->bpp = depth == 3 ? 4 : 8;
    out->tiled = rd32(c + 0x14) == 0;
    out->width_tiles = tw == 0xFFFF ? 0 : tw;
    out->height_tiles = th == 0xFFFF ? 0 : th;
    return NDS_OK;
}

NdsResult nds_screen_parse(const uint8_t *d, size_t n, NdsScreen *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (nds_detect_kind(d, n) != NDS_KIND_NSCR) return NDS_ERR_FORMAT;
    uint32_t cs;
    const uint8_t *c = find_chunk(d, n, "NRCS", &cs);
    if (!c || cs < 0x14) return NDS_ERR_FORMAT;

    uint32_t avail = cs - 0x14;
    uint32_t dsz = rd32(c + 0x10);
    if (dsz > avail) dsz = avail;
    uint32_t count = dsz / 2;
    out->width = rd16(c + 8);
    out->height = rd16(c + 10);
    if (count == 0 || !out->width || !out->height) return NDS_ERR_FORMAT;

    out->entries = malloc((size_t)count * 2);
    if (!out->entries) return NDS_ERR_NOMEM;
    for (uint32_t i = 0; i < count; i++) out->entries[i] = rd16(c + 0x14 + i * 2);
    out->count = count;
    return NDS_OK;
}

void nds_palette_free(NdsPalette *p) { if (p) { free(p->rgba); memset(p, 0, sizeof *p); } }
void nds_tileset_free(NdsTileSet *t) { if (t) { free(t->data); memset(t, 0, sizeof *t); } }
void nds_screen_free(NdsScreen *s) { if (s) { free(s->entries); memset(s, 0, sizeof *s); } }
void nds_image_free(NdsImage *i) { if (i) { free(i->rgba); memset(i, 0, sizeof *i); } }

static void put_pixel(uint8_t *px, const NdsPalette *pal, unsigned idx, int bpp) {
    if (idx == 0) { px[0] = px[1] = px[2] = px[3] = 0; return; }
    if (pal) {
        if (idx < pal->count) memcpy(px, pal->rgba + idx * 4, 4);
        else { px[0] = px[1] = px[2] = 255; px[3] = 255; }
    } else {
        px[0] = px[1] = px[2] = (uint8_t)(bpp == 4 ? (idx & 15) * 17 : idx);
        px[3] = 255;
    }
}

/* Palette index of pixel (x, y) within tile `tile`, or -1 if out of data. */
static int tile_pixel(const NdsTileSet *ts, uint32_t tile, unsigned x, unsigned y) {
    uint32_t bytes_per_tile = ts->bpp == 4 ? 32 : 64;
    uint64_t off = (uint64_t)tile * bytes_per_tile + y * (ts->bpp == 4 ? 4 : 8);
    off += ts->bpp == 4 ? x / 2 : x;
    if (off >= ts->size) return -1;
    uint8_t b = ts->data[off];
    return ts->bpp == 4 ? ((x & 1) ? b >> 4 : b & 0xF) : b;
}

NdsResult nds_render_tiles(const NdsTileSet *ts, const NdsPalette *pal, unsigned bank,
                           uint32_t wt, NdsImage *out) {
    if (!ts || !out || !ts->data) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    uint32_t tile_bytes = ts->bpp == 4 ? 32 : 64;
    uint32_t tiles = ts->size / tile_bytes;
    if (tiles == 0) return NDS_ERR_FORMAT;
    if (!wt) wt = ts->width_tiles ? ts->width_tiles : DEFAULT_WIDTH_TILES;
    if (wt > tiles && !ts->width_tiles) wt = tiles;
    uint32_t ht = (tiles + wt - 1) / wt;
    if ((uint64_t)wt * ht * 64 > MAX_PIXELS) return NDS_ERR_FORMAT;

    out->width = wt * 8;
    out->height = ht * 8;
    out->rgba = calloc((size_t)out->width * out->height, 4);
    if (!out->rgba) return NDS_ERR_NOMEM;

    for (uint32_t y = 0; y < out->height; y++) {
        for (uint32_t x = 0; x < out->width; x++) {
            int idx;
            if (ts->tiled)
                idx = tile_pixel(ts, (y / 8) * wt + x / 8, x % 8, y % 8);
            else {
                uint64_t o = (uint64_t)y * out->width + x;
                if (ts->bpp == 4) {
                    uint64_t bo = o / 2;
                    idx = bo < ts->size ? ((o & 1) ? ts->data[bo] >> 4 : ts->data[bo] & 0xF) : -1;
                } else
                    idx = o < ts->size ? ts->data[o] : -1;
            }
            if (idx < 0) continue;
            if (ts->bpp == 4 && idx) idx += (int)bank * 16;
            put_pixel(out->rgba + ((size_t)y * out->width + x) * 4, pal, (unsigned)idx, ts->bpp);
        }
    }
    return NDS_OK;
}

NdsResult nds_render_screen(const NdsTileSet *ts, const NdsPalette *pal,
                            const NdsScreen *scr, NdsImage *out) {
    if (!ts || !scr || !out || !ts->data) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    uint32_t wt = scr->width / 8, ht = scr->height / 8;
    if (!wt || !ht || (uint64_t)wt * ht * 64 > MAX_PIXELS) return NDS_ERR_FORMAT;

    out->width = wt * 8;
    out->height = ht * 8;
    out->rgba = calloc((size_t)out->width * out->height, 4);
    if (!out->rgba) return NDS_ERR_NOMEM;

    for (uint32_t ty = 0; ty < ht; ty++) {
        for (uint32_t tx = 0; tx < wt; tx++) {
            uint32_t e = ty * wt + tx;
            if (e >= scr->count) continue;
            uint16_t v = scr->entries[e];
            uint32_t tile = v & 0x3FF;
            int hf = (v >> 10) & 1, vf = (v >> 11) & 1;
            unsigned bank = v >> 12;
            for (unsigned y = 0; y < 8; y++) {
                for (unsigned x = 0; x < 8; x++) {
                    int idx = tile_pixel(ts, tile, hf ? 7 - x : x, vf ? 7 - y : y);
                    if (idx < 0) continue;
                    if (ts->bpp == 4 && idx) idx += (int)bank * 16;
                    put_pixel(out->rgba + ((size_t)(ty * 8 + y) * out->width + tx * 8 + x) * 4,
                              pal, (unsigned)idx, ts->bpp);
                }
            }
        }
    }
    return NDS_OK;
}
