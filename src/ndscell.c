#include "ndsrom/ndsgfx.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PIXELS (64u * 1024u * 1024u)
#define MAX_CELL_DIM 512

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static const uint8_t obj_w[3][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}};
static const uint8_t obj_h[3][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}};

NdsResult nds_cellbank_parse(const uint8_t *d, size_t n, NdsCellBank *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (n < 0x30 || memcmp(d, "RECN", 4) != 0) return NDS_ERR_FORMAT;

    size_t pos = rd16(d + 12);
    if (pos < 16) pos = 16;
    const uint8_t *c = NULL;
    uint32_t csz = 0;
    while (pos + 8 <= n) {
        uint32_t sz = rd32(d + pos + 4);
        if (sz < 8 || sz > n - pos) return NDS_ERR_FORMAT;
        if (!memcmp(d + pos, "KBEC", 4)) { c = d + pos; csz = sz; break; }
        pos += sz;
    }
    if (!c || csz < 0x20) return NDS_ERR_FORMAT;

    uint32_t cells = rd16(c + 8);
    int extended = rd16(c + 10) & 1;
    uint32_t cell_off = rd32(c + 12);
    uint32_t cell_size = extended ? 16 : 8;
    out->tile_shift = rd32(c + 16) & 3;
    if (cells == 0) return NDS_ERR_FORMAT;

    /* Offsets are relative to the chunk body, which follows the 8-byte tag/size. */
    uint64_t cell_end = 8 + (uint64_t)cell_off + (uint64_t)cells * cell_size;
    if (cell_off < 0x18 || cell_end > csz) return NDS_ERR_FORMAT;
    const uint8_t *cp = c + 8 + cell_off;
    const uint8_t *oam_base = c + cell_end;
    uint32_t oam_avail = (uint32_t)((csz - cell_end) / 6);

    out->cells = calloc(cells, sizeof *out->cells);
    if (!out->cells) return NDS_ERR_NOMEM;
    out->oams = malloc((size_t)oam_avail * sizeof *out->oams + 1);
    if (!out->oams) { nds_cellbank_free(out); return NDS_ERR_NOMEM; }

    /* Cells may share/overlap OAM data, so copy per-cell and append. */
    for (uint32_t i = 0; i < cells; i++) {
        uint32_t cnt = rd16(cp + i * cell_size);
        uint32_t off = rd32(cp + i * cell_size + 4);
        if ((uint64_t)off + (uint64_t)cnt * 6 > (uint64_t)oam_avail * 6) {
            nds_cellbank_free(out);
            return NDS_ERR_FORMAT;
        }
        NdsOam *tmp = realloc(out->oams, ((size_t)out->oam_count + cnt + 1) * sizeof *tmp);
        if (!tmp) { nds_cellbank_free(out); return NDS_ERR_NOMEM; }
        out->oams = tmp;
        out->cells[i].first_oam = out->oam_count;
        out->cells[i].oam_count = cnt;
        for (uint32_t k = 0; k < cnt; k++) {
            const uint8_t *o = oam_base + off + k * 6;
            NdsOam *dst = &out->oams[out->oam_count++];
            dst->attr0 = rd16(o);
            dst->attr1 = rd16(o + 2);
            dst->attr2 = rd16(o + 4);
        }
    }
    out->cell_count = cells;
    return NDS_OK;
}

void nds_cellbank_free(NdsCellBank *cb) {
    if (!cb) return;
    free(cb->cells);
    free(cb->oams);
    memset(cb, 0, sizeof *cb);
}

typedef struct {
    int x, y, w, h;
    int visible;
} Extent;

static Extent oam_extent(const NdsOam *o) {
    Extent e = {0, 0, 0, 0, 1};
    unsigned shape = (o->attr0 >> 14) & 3, size = (o->attr1 >> 14) & 3;
    int affine = (o->attr0 >> 8) & 1, disabled = (o->attr0 >> 9) & 1;
    if (shape == 3 || (!affine && disabled)) { e.visible = 0; return e; }
    e.w = obj_w[shape][size];
    e.h = obj_h[shape][size];
    e.x = (int)(o->attr1 & 0x1FF);
    if (e.x >= 256) e.x -= 512;
    e.y = (int)(o->attr0 & 0xFF);
    if (e.y >= 128) e.y -= 256;
    return e;
}

static int obj_pixel(const NdsTileSet *ts, uint64_t base, int wt, int bpp, int x, int y) {
    uint32_t tb = bpp == 4 ? 32 : 64;
    uint64_t off = base + ((uint64_t)(y / 8) * wt + x / 8) * tb + (y % 8) * (bpp == 4 ? 4 : 8);
    off += bpp == 4 ? (x % 8) / 2 : x % 8;
    if (off >= ts->size) return -1;
    uint8_t b = ts->data[off];
    return bpp == 4 ? ((x & 1) ? b >> 4 : b & 0xF) : b;
}

NdsResult nds_render_cell(const NdsCellBank *cb, uint32_t cell, const NdsTileSet *ts,
                          const NdsPalette *pal, NdsImage *out) {
    return nds_render_cell_ex(cb, cell, ts, pal, out, NULL, NULL);
}

NdsResult nds_render_cell_ex(const NdsCellBank *cb, uint32_t cell, const NdsTileSet *ts,
                             const NdsPalette *pal, NdsImage *out, int *origin_x,
                             int *origin_y) {
    if (!cb || !ts || !out || !ts->data || cell >= cb->cell_count) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    const NdsCell *c = &cb->cells[cell];

    int minx = 0, miny = 0, maxx = 0, maxy = 0, any = 0;
    for (uint32_t k = 0; k < c->oam_count; k++) {
        Extent e = oam_extent(&cb->oams[c->first_oam + k]);
        if (!e.visible) continue;
        if (!any || e.x < minx) minx = e.x;
        if (!any || e.y < miny) miny = e.y;
        if (!any || e.x + e.w > maxx) maxx = e.x + e.w;
        if (!any || e.y + e.h > maxy) maxy = e.y + e.h;
        any = 1;
    }
    if (!any) return NDS_ERR_NOTFOUND;
    int W = maxx - minx, H = maxy - miny;
    if (W > MAX_CELL_DIM || H > MAX_CELL_DIM) return NDS_ERR_FORMAT;

    out->width = (uint32_t)W;
    out->height = (uint32_t)H;
    out->rgba = calloc((size_t)W * H, 4);
    if (!out->rgba) return NDS_ERR_NOMEM;
    if (origin_x) *origin_x = -minx;
    if (origin_y) *origin_y = -miny;

    /* Lower OAM indices have higher priority, so draw them last. */
    for (uint32_t k = c->oam_count; k-- > 0;) {
        const NdsOam *o = &cb->oams[c->first_oam + k];
        Extent e = oam_extent(o);
        if (!e.visible) continue;
        int bpp = (o->attr0 & 0x2000) ? 8 : 4;
        int affine = (o->attr0 >> 8) & 1;
        int hf = !affine && (o->attr1 & 0x1000), vf = !affine && (o->attr1 & 0x2000);
        unsigned bank = o->attr2 >> 12;
        uint64_t base = (uint64_t)(o->attr2 & 0x3FF) * (32u << cb->tile_shift);

        for (int y = 0; y < e.h; y++) {
            for (int x = 0; x < e.w; x++) {
                int idx = obj_pixel(ts, base, e.w / 8, bpp, hf ? e.w - 1 - x : x,
                                    vf ? e.h - 1 - y : y);
                if (idx <= 0) continue;
                if (bpp == 4) idx += (int)bank * 16;
                uint8_t *px = out->rgba + ((size_t)(e.y - miny + y) * W + (e.x - minx + x)) * 4;
                if (pal && (uint32_t)idx < pal->count) memcpy(px, pal->rgba + idx * 4, 4);
                else px[0] = px[1] = px[2] = (uint8_t)(bpp == 4 ? (idx & 15) * 17 : idx), px[3] = 255;
            }
        }
    }
    return NDS_OK;
}

NdsResult nds_render_cell_atlas(const NdsCellBank *cb, const NdsTileSet *ts,
                                const NdsPalette *pal, NdsImage *out) {
    if (!cb || !ts || !out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    NdsImage *imgs = calloc(cb->cell_count, sizeof *imgs);
    if (!imgs) return NDS_ERR_NOMEM;

    uint32_t cw = 1, ch = 1, used = 0;
    NdsResult r = NDS_OK;
    for (uint32_t i = 0; i < cb->cell_count; i++) {
        NdsResult cr = nds_render_cell(cb, i, ts, pal, &imgs[i]);
        if (cr == NDS_ERR_NOMEM) { r = cr; goto done; }
        if (cr != NDS_OK) continue;
        used++;
        if (imgs[i].width > cw) cw = imgs[i].width;
        if (imgs[i].height > ch) ch = imgs[i].height;
    }
    if (!used) { r = NDS_ERR_NOTFOUND; goto done; }

    /* Pad slots by 1px so neighbours stay distinguishable. */
    cw++, ch++;
    uint32_t cols = 1;
    while (cols * cols < cb->cell_count) cols++;
    uint32_t rows = (cb->cell_count + cols - 1) / cols;
    if ((uint64_t)cols * cw * rows * ch > MAX_PIXELS) { r = NDS_ERR_FORMAT; goto done; }

    out->width = cols * cw;
    out->height = rows * ch;
    out->rgba = calloc((size_t)out->width * out->height, 4);
    if (!out->rgba) { r = NDS_ERR_NOMEM; goto done; }

    for (uint32_t i = 0; i < cb->cell_count; i++) {
        if (!imgs[i].rgba) continue;
        uint32_t ox = (i % cols) * cw, oy = (i / cols) * ch;
        for (uint32_t y = 0; y < imgs[i].height; y++)
            memcpy(out->rgba + ((size_t)(oy + y) * out->width + ox) * 4,
                   imgs[i].rgba + (size_t)y * imgs[i].width * 4, (size_t)imgs[i].width * 4);
    }
done:
    for (uint32_t i = 0; i < cb->cell_count; i++) nds_image_free(&imgs[i]);
    free(imgs);
    return r;
}
