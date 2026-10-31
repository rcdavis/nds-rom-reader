#include "ndsrom/ndsgfx.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ANIM_BYTES (256u * 1024u * 1024u)
#define MAX_ANIM_DIM 1024

static int16_t rds16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

NdsResult nds_animbank_parse(const uint8_t *d, size_t n, NdsAnimBank *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (n < 0x30 || memcmp(d, "RNAN", 4) != 0) return NDS_ERR_FORMAT;

    size_t pos = rd16(d + 12);
    if (pos < 16) pos = 16;
    const uint8_t *c = NULL;
    uint32_t csz = 0;
    while (pos + 8 <= n) {
        uint32_t sz = rd32(d + pos + 4);
        if (sz < 8 || sz > n - pos) return NDS_ERR_FORMAT;
        if (!memcmp(d + pos, "KNBA", 4)) { c = d + pos; csz = sz; break; }
        pos += sz;
    }
    if (!c || csz < 0x20) return NDS_ERR_FORMAT;

    /* All offsets are relative to the chunk body (after the 8-byte tag/size). */
    const uint8_t *body = c + 8;
    uint32_t body_sz = csz - 8;
    uint32_t nseq = rd16(body), nframe = rd16(body + 2);
    uint32_t seq_off = rd32(body + 4), frame_off = rd32(body + 8), data_off = rd32(body + 12);
    if (nseq == 0 || (uint64_t)seq_off + (uint64_t)nseq * 16 > body_sz ||
        (uint64_t)frame_off + (uint64_t)nframe * 8 > body_sz || data_off > body_sz)
        return NDS_ERR_FORMAT;

    out->seqs = calloc(nseq, sizeof *out->seqs);
    out->frames = calloc(nframe ? nframe : 1, sizeof *out->frames);
    if (!out->seqs || !out->frames) { nds_animbank_free(out); return NDS_ERR_NOMEM; }

    for (uint32_t i = 0; i < nseq; i++) {
        const uint8_t *s = body + seq_off + i * 16;
        NdsAnimSeq *q = &out->seqs[i];
        q->frame_count = rd16(s);
        q->loop_start = rd16(s + 2);
        q->type = rd16(s + 4);
        q->play_mode = rd32(s + 8);
        uint32_t byte_off = rd32(s + 12);
        if (byte_off % 8 || (uint64_t)byte_off / 8 + q->frame_count > nframe) {
            nds_animbank_free(out);
            return NDS_ERR_FORMAT;
        }
        q->first_frame = byte_off / 8;
    }

    for (uint32_t i = 0; i < nframe; i++) {
        const uint8_t *f = body + frame_off + i * 8;
        uint32_t off = rd32(f);
        NdsAnimFrame *a = &out->frames[i];
        a->duration = rd16(f + 4);

        /* Element layout depends on the sequence type that references it. */
        uint16_t type = 0;
        for (uint32_t k = 0; k < nseq; k++)
            if (i >= out->seqs[k].first_frame && i < out->seqs[k].first_frame + out->seqs[k].frame_count)
                type = out->seqs[k].type;
        uint32_t need = type == 1 ? 16 : type == 2 ? 8 : 2;
        if ((uint64_t)data_off + off + need > body_sz) { nds_animbank_free(out); return NDS_ERR_FORMAT; }
        const uint8_t *e = body + data_off + off;
        a->cell = rd16(e);
        if (type == 1) { a->tx = rds16(e + 12); a->ty = rds16(e + 14); }
        else if (type == 2) { a->tx = rds16(e + 4); a->ty = rds16(e + 6); }
    }
    out->seq_count = nseq;
    out->frame_count = nframe;
    return NDS_OK;
}

void nds_animbank_free(NdsAnimBank *ab) {
    if (!ab) return;
    free(ab->seqs);
    free(ab->frames);
    memset(ab, 0, sizeof *ab);
}

void nds_animation_free(NdsAnimation *a) {
    if (!a) return;
    free(a->rgba);
    free(a->duration);
    memset(a, 0, sizeof *a);
}

NdsResult nds_render_anim(const NdsAnimBank *ab, uint32_t seq, const NdsCellBank *cb,
                          const NdsTileSet *ts, const NdsPalette *pal, NdsAnimation *out) {
    if (!ab || !cb || !ts || !out || seq >= ab->seq_count) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    const NdsAnimSeq *q = &ab->seqs[seq];
    if (q->frame_count == 0) return NDS_ERR_NOTFOUND;

    NdsImage *imgs = calloc(q->frame_count, sizeof *imgs);
    int *px = calloc(q->frame_count, sizeof *px), *py = calloc(q->frame_count, sizeof *py);
    NdsResult r = NDS_OK;
    if (!imgs || !px || !py) { r = NDS_ERR_NOMEM; goto done; }

    int minx = 0, miny = 0, maxx = 0, maxy = 0, any = 0;
    for (uint32_t i = 0; i < q->frame_count; i++) {
        const NdsAnimFrame *f = &ab->frames[q->first_frame + i];
        int ox = 0, oy = 0;
        NdsResult cr = f->cell < cb->cell_count
                           ? nds_render_cell_ex(cb, f->cell, ts, pal, &imgs[i], &ox, &oy)
                           : NDS_ERR_NOTFOUND;
        if (cr == NDS_ERR_NOMEM) { r = cr; goto done; }
        if (cr != NDS_OK) continue; /* empty cell: frame stays blank */
        px[i] = f->tx - ox;
        py[i] = f->ty - oy;
        int w = (int)imgs[i].width, h = (int)imgs[i].height;
        if (!any || px[i] < minx) minx = px[i];
        if (!any || py[i] < miny) miny = py[i];
        if (!any || px[i] + w > maxx) maxx = px[i] + w;
        if (!any || py[i] + h > maxy) maxy = py[i] + h;
        any = 1;
    }
    if (!any) { r = NDS_ERR_NOTFOUND; goto done; }

    int W = maxx - minx, H = maxy - miny;
    if (W > MAX_ANIM_DIM || H > MAX_ANIM_DIM ||
        (uint64_t)W * H * 4 * q->frame_count > MAX_ANIM_BYTES) { r = NDS_ERR_FORMAT; goto done; }

    out->width = (uint32_t)W;
    out->height = (uint32_t)H;
    out->count = q->frame_count;
    size_t frame_bytes = (size_t)W * H * 4;
    out->rgba = calloc(q->frame_count, frame_bytes);
    out->duration = calloc(q->frame_count, sizeof *out->duration);
    if (!out->rgba || !out->duration) { r = NDS_ERR_NOMEM; nds_animation_free(out); goto done; }
    out->loop = q->play_mode == 2 || q->play_mode == 4;

    for (uint32_t i = 0; i < q->frame_count; i++) {
        out->duration[i] = ab->frames[q->first_frame + i].duration;
        if (!imgs[i].rgba) continue;
        for (uint32_t y = 0; y < imgs[i].height; y++)
            memcpy(out->rgba + i * frame_bytes +
                       ((size_t)(py[i] - miny + (int)y) * W + (px[i] - minx)) * 4,
                   imgs[i].rgba + (size_t)y * imgs[i].width * 4, (size_t)imgs[i].width * 4);
    }
done:
    if (imgs) for (uint32_t i = 0; i < q->frame_count; i++) nds_image_free(&imgs[i]);
    free(imgs);
    free(px);
    free(py);
    return r;
}
