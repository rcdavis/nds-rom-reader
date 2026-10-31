#include "ndsrom/ndsmodel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndsbuf.h"

/* Finds a block ("TEX0", "MDL0") of a BMD0/BTX0 container; returns its offset or 0. */
size_t nds_nitro_block(Buf *b, const char *magic) {
    if (b->n < 16) return 0;
    uint32_t count = rd16(b, 14);
    if (count > 16) return 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t off = rd32(b, 16 + i * 4);
        if (off + 8 <= b->n && memcmp(b->d + off, magic, 4) == 0) return off;
    }
    return 0;
}

int nds_dict_read(Buf *b, size_t base, NdsDict *d) {
    d->n = rd8(b, base + 1);
    size_t hdr = base + 8 + ((size_t)d->n + 1) * 4;
    d->es = rd16(b, hdr);
    size_t ents = hdr + 4, names = ents + (size_t)d->n * d->es;
    if (b->bad || d->es == 0 || names + (size_t)d->n * 16 > b->n) {
        b->bad = 1;
        d->n = 0;
        return -1;
    }
    for (uint32_t i = 0; i < d->n; i++) {
        d->ent[i] = ents + (size_t)i * d->es;
        memcpy(d->name[i], b->d + names + i * 16, 16);
        d->name[i][16] = 0;
    }
    return 0;
}

void nds_texset_free(NdsTexSet *ts) {
    if (!ts) return;
    free(ts->tex);
    free(ts->pal);
    free(ts->block);
    memset(ts, 0, sizeof *ts);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

NdsResult nds_texset_parse(const uint8_t *data, size_t size, NdsTexSet *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!data || size < 16 || (memcmp(data, "BTX0", 4) && memcmp(data, "BMD0", 4)))
        return NDS_ERR_FORMAT;

    Buf file = {data, size, 0};
    size_t t = nds_nitro_block(&file, "TEX0");
    if (!t) return NDS_ERR_NOTFOUND;
    size_t bsize = rd32(&file, t + 4);
    if (bsize < 0x40 || bsize > size - t) bsize = size - t;

    out->block = malloc(bsize);
    if (!out->block) return NDS_ERR_NOMEM;
    memcpy(out->block, data + t, bsize);
    out->block_size = bsize;
    Buf b = {out->block, bsize, 0};

    uint32_t tex_dict = rd16(&b, 0x0E), pal_dict = rd32(&b, 0x34);
    out->tex_data = rd32(&b, 0x14);
    out->comp_data = rd32(&b, 0x24);
    out->comp_info = rd32(&b, 0x28);
    out->pal_data = rd32(&b, 0x38);
    uint32_t pal_size = rd32(&b, 0x30) << 3;

    NdsDict td, pd;
    if (b.bad || nds_dict_read(&b, tex_dict, &td) || nds_dict_read(&b, pal_dict, &pd)) {
        nds_texset_free(out);
        return NDS_ERR_FORMAT;
    }

    out->tex = calloc(td.n ? td.n : 1, sizeof *out->tex);
    out->pal = calloc(pd.n ? pd.n : 1, sizeof *out->pal);
    if (!out->tex || !out->pal) {
        nds_texset_free(out);
        return NDS_ERR_NOMEM;
    }

    for (uint32_t i = 0; i < td.n; i++) {
        NdsTexInfo *ti = &out->tex[i];
        uint32_t param = rd16(&b, td.ent[i] + 2);
        memcpy(ti->name, td.name[i], 17);
        ti->offset = rd16(&b, td.ent[i]) << 3;
        ti->repeat_s = param & 1;
        ti->repeat_t = (param >> 1) & 1;
        ti->flip_s = (param >> 2) & 1;
        ti->flip_t = (param >> 3) & 1;
        ti->width = 8u << ((param >> 4) & 7);
        ti->height = 8u << ((param >> 7) & 7);
        ti->format = (param >> 10) & 7;
        ti->color0_transparent = (param >> 13) & 1;
    }
    out->tex_count = td.n;

    uint32_t offs[256];
    for (uint32_t i = 0; i < pd.n; i++) {
        memcpy(out->pal[i].name, pd.name[i], 17);
        out->pal[i].offset = offs[i] = rd16(&b, pd.ent[i]) << 3;
    }
    qsort(offs, pd.n, sizeof offs[0], cmp_u32);
    for (uint32_t i = 0; i < pd.n; i++) {
        uint32_t end = pal_size;
        for (uint32_t j = 0; j < pd.n; j++)
            if (offs[j] > out->pal[i].offset) { end = offs[j]; break; }
        out->pal[i].size = end > out->pal[i].offset ? end - out->pal[i].offset : 0;
    }
    out->pal_count = pd.n;
    if (b.bad) {
        nds_texset_free(out);
        return NDS_ERR_FORMAT;
    }
    return NDS_OK;
}

int nds_texset_find_tex(const NdsTexSet *ts, const char *name) {
    for (uint32_t i = 0; ts && i < ts->tex_count; i++)
        if (!strcmp(ts->tex[i].name, name)) return (int)i;
    return -1;
}

int nds_texset_find_pal(const NdsTexSet *ts, const char *name) {
    for (uint32_t i = 0; ts && i < ts->pal_count; i++)
        if (!strcmp(ts->pal[i].name, name)) return (int)i;
    return -1;
}

int nds_texset_default_pal(const NdsTexSet *ts, uint32_t tex) {
    if (!ts || tex >= ts->tex_count) return -1;
    char name[40];
    snprintf(name, sizeof name, "%s_pl", ts->tex[tex].name);
    int p = nds_texset_find_pal(ts, name);
    if (p >= 0) return p;
    return tex < ts->pal_count ? (int)tex : (ts->pal_count ? 0 : -1);
}

static void put_rgba(uint8_t *px, unsigned c, unsigned a) {
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    px[0] = (uint8_t)((r << 3) | (r >> 2));
    px[1] = (uint8_t)((g << 3) | (g >> 2));
    px[2] = (uint8_t)((b << 3) | (b >> 2));
    px[3] = (uint8_t)a;
}

NdsResult nds_texset_decode(const NdsTexSet *ts, uint32_t tex, int pal, NdsImage *out) {
    if (!ts || !out || tex >= ts->tex_count) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    const NdsTexInfo *t = &ts->tex[tex];
    if (t->format < 1 || t->format > 7) return NDS_ERR_FORMAT;

    Buf b = {ts->block, ts->block_size, 0};
    uint32_t w = t->width, h = t->height;
    size_t npx = (size_t)w * h;

    if (pal < 0) pal = nds_texset_default_pal(ts, tex);
    size_t pbase = 0, pbytes = 0;
    if (t->format != 7) {
        if (pal < 0 || (uint32_t)pal >= ts->pal_count) return NDS_ERR_NOTFOUND;
        pbase = (size_t)ts->pal_data + ts->pal[pal].offset;
        pbytes = ts->pal[pal].size;
    }

    uint8_t *rgba = calloc(npx, 4);
    if (!rgba) return NDS_ERR_NOMEM;

    size_t base = (size_t)(t->format == 5 ? ts->comp_data : ts->tex_data) + t->offset;
    unsigned ncol = (unsigned)(pbytes / 2);

    /* Palette lookup that tolerates indices past the end of the palette. */
    #define PAL(i) (((i) < ncol) ? rd16(&b, pbase + (size_t)(i) * 2) : 0u)

    for (uint32_t y = 0; y < h && !b.bad; y++) {
        for (uint32_t x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            uint8_t *px = rgba + i * 4;
            switch (t->format) {
            case 1: {
                unsigned v = rd8(&b, base + i), a = v >> 5;
                put_rgba(px, PAL(v & 31), (a << 5) | (a << 2) | (a >> 1));
                break;
            }
            case 2: {
                unsigned v = (rd8(&b, base + i / 4) >> ((i & 3) * 2)) & 3;
                put_rgba(px, PAL(v), (v == 0 && t->color0_transparent) ? 0 : 255);
                break;
            }
            case 3: {
                unsigned v = (rd8(&b, base + i / 2) >> ((i & 1) * 4)) & 15;
                put_rgba(px, PAL(v), (v == 0 && t->color0_transparent) ? 0 : 255);
                break;
            }
            case 4: {
                unsigned v = rd8(&b, base + i);
                put_rgba(px, PAL(v), (v == 0 && t->color0_transparent) ? 0 : 255);
                break;
            }
            case 5: {
                size_t blk = (size_t)(y / 4) * (w / 4) + x / 4;
                uint32_t texels = rd32(&b, base + blk * 4);
                unsigned info = rd16(&b, (size_t)ts->comp_info + t->offset / 2 + blk * 2);
                unsigned idx = (texels >> (((y & 3) * 4 + (x & 3)) * 2)) & 3;
                unsigned mode = info >> 14;
                size_t po = pbase + (size_t)(info & 0x3FFF) * 4;
                unsigned c0 = rd16(&b, po), c1 = rd16(&b, po + 2), c = 0, a = 255;
                if (idx == 0) c = c0;
                else if (idx == 1) c = c1;
                else if (mode == 0 || mode == 2) {
                    if (idx == 2) c = rd16(&b, po + 4);
                    else if (mode == 0) a = 0;
                    else c = rd16(&b, po + 6);
                } else if (mode == 1) {
                    if (idx == 3) a = 0;
                    else {
                        unsigned r = ((c0 & 31) + (c1 & 31)) / 2, g = (((c0 >> 5) & 31) + ((c1 >> 5) & 31)) / 2,
                                 bl = (((c0 >> 10) & 31) + ((c1 >> 10) & 31)) / 2;
                        c = r | (g << 5) | (bl << 10);
                    }
                } else {
                    unsigned wa = idx == 2 ? 5 : 3, wb = 8 - wa, r = 0, comp[3];
                    for (int k = 0; k < 3; k++)
                        comp[k] = (wa * ((c0 >> (5 * k)) & 31) + wb * ((c1 >> (5 * k)) & 31)) / 8;
                    r = comp[0] | (comp[1] << 5) | (comp[2] << 10);
                    c = r;
                }
                put_rgba(px, c, a);
                break;
            }
            case 6: {
                unsigned v = rd8(&b, base + i), a = v >> 3;
                put_rgba(px, PAL(v & 7), (a << 3) | (a >> 2));
                break;
            }
            default: {
                unsigned v = rd16(&b, base + i * 2);
                put_rgba(px, v, (v & 0x8000) ? 255 : 0);
                break;
            }
            }
        }
    }
    #undef PAL

    if (b.bad) {
        free(rgba);
        return NDS_ERR_FORMAT;
    }
    out->width = w;
    out->height = h;
    out->rgba = rgba;
    return NDS_OK;
}
