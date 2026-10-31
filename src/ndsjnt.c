#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ndsbuf.h"
#include "ndsrom/ndsmodel.h"

int nds_affine_inverse(const double m[12], double out[12]) {
    double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (fabs(det) < 1e-12) return -1;
    double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                     (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                     (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) out[r * 4 + k] = inv[r * 3 + k];
        out[r * 4 + 3] = -(inv[r * 3] * m[3] + inv[r * 3 + 1] * m[7] + inv[r * 3 + 2] * m[11]);
    }
    return 0;
}

/* ---- Parsing ---- */

static double fix_s(uint32_t x, int bits, int frac) {
    uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    x &= mask;
    int64_t v = x;
    if (x & (1u << (bits - 1))) v -= (int64_t)1 << bits;
    return (double)v / (double)(1 << frac);
}

static void free_curve(NdsCurve *c) {
    free(c->v);
    memset(c, 0, sizeof *c);
}

static void free_anim(NdsJointAnim *a) {
    for (uint32_t i = 0; i < a->obj_count; i++) {
        NdsJointCurves *o = &a->objs[i];
        for (int k = 0; k < 3; k++) {
            free_curve(&o->trans[k]);
            free_curve(&o->scale[k]);
        }
        free_curve(&o->rot);
    }
    free(a->objs);
    memset(a, 0, sizeof *a);
}

void nds_jntanim_free(NdsJointAnimSet *set) {
    if (!set) return;
    for (uint32_t i = 0; i < set->count; i++) free_anim(&set->anims[i]);
    free(set->anims);
    memset(set, 0, sizeof *set);
}

typedef struct {
    Buf *b;
    size_t base, pivot, basis;
} AnimCtx;

static void rotation_from_ref(AnimCtx *x, uint32_t ref, double out[9]) {
    Buf *b = x->b;
    uint32_t idx = ref & 0x7FFF;
    if (ref & 0x8000) {
        size_t o = x->pivot + (size_t)idx * 6;
        uint32_t selneg = rd16(b, o);
        nds_pivot_mat(selneg & 15, (selneg >> 4) & 15, fix_s(rd16(b, o + 2), 16, 12),
                      fix_s(rd16(b, o + 4), 16, 12), out);
    } else {
        size_t o = x->basis + (size_t)idx * 10;
        uint32_t in[5], input[5];
        for (int i = 0; i < 5; i++) in[i] = rd16(b, o + i * 2);
        input[0] = in[4]; input[1] = in[0]; input[2] = in[1]; input[3] = in[2]; input[4] = in[3];
        uint32_t v[6] = {0};
        for (int i = 0; i < 5; i++) {
            v[i] = input[i] >> 3;
            v[5] = (v[5] << 3) | (input[i] & 7);
        }
        double f[6];
        for (int i = 0; i < 6; i++) f[i] = fix_s(v[i], 13, 12);
        double A[3] = {f[1], f[2], f[3]}, B[3] = {f[4], f[0], f[5]};
        double C[3] = {A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0]};
        memcpy(out, A, sizeof A);
        memcpy(out + 3, B, sizeof B);
        memcpy(out + 6, C, sizeof C);
    }
}

typedef enum { VAL_FIX32, VAL_SCALE, VAL_ROT } ValKind;

/* Reads a curve header (info + data offset) and its samples. */
static int read_curve(AnimCtx *x, size_t *cur, int dim, int pair, NdsCurve *c) {
    Buf *b = x->b;
    uint32_t info = rd32(b, *cur), off = rd32(b, *cur + 4);
    *cur += 8;
    uint32_t start = info & 0xFFFF, end = (info >> 16) & 0xFFF, width = (info >> 28) & 3, rate = info >> 30;
    if (b->bad || start >= end) return -1;
    uint32_t n = (end - start) >> rate;
    if (n == 0 || n > 65536) return -1;
    c->v = malloc((size_t)n * dim * sizeof *c->v);
    if (!c->v) return -1;
    c->kind = 2;
    c->start = start;
    c->end = end;
    c->step = 1u << rate;
    c->count = n;
    size_t p = x->base + off;
    for (uint32_t i = 0; i < n; i++) {
        if (dim == 9) {
            rotation_from_ref(x, rd16(b, p + (size_t)i * 2), c->v + (size_t)i * 9);
        } else if (width == 0) {
            c->v[i] = fix_s(rd32(b, p + (size_t)i * 4 * pair), 32, 12);
        } else {
            c->v[i] = fix_s(rd16(b, p + (size_t)i * 2 * pair), 16, 12);
        }
    }
    return b->bad ? -1 : 0;
}

static int read_const(NdsCurve *c, int dim, const double *v) {
    c->v = malloc((size_t)dim * sizeof *c->v);
    if (!c->v) return -1;
    memcpy(c->v, v, (size_t)dim * sizeof *c->v);
    c->kind = 1;
    c->count = 1;
    return 0;
}

static int read_object(AnimCtx *x, size_t o, NdsJointCurves *oc) {
    Buf *b = x->b;
    uint32_t flags = rd16(b, o);
    oc->node = rd8(b, o + 3);
    size_t cur = o + 4;
    if (b->bad) return -1;
    if (flags & 1) return 0;                       /* not animated: model rest pose */

    if (!((flags >> 1) & 3)) {
        for (int i = 0; i < 3; i++) {
            if ((flags >> (3 + i)) & 1) {
                double v = fix_s(rd32(b, cur), 32, 12);
                cur += 4;
                if (read_const(&oc->trans[i], 1, &v)) return -1;
            } else if (read_curve(x, &cur, 1, 1, &oc->trans[i])) return -1;
        }
    }
    if (!((flags >> 6) & 3)) {
        if ((flags >> 8) & 1) {
            double m[9];
            rotation_from_ref(x, rd16(b, cur), m);
            cur += 4;
            if (read_const(&oc->rot, 9, m)) return -1;
        } else if (read_curve(x, &cur, 9, 1, &oc->rot)) return -1;
    }
    if (!((flags >> 9) & 3)) {
        for (int i = 0; i < 3; i++) {
            if ((flags >> (11 + i)) & 1) {
                double v = fix_s(rd32(b, cur), 32, 12);
                cur += 8;
                if (read_const(&oc->scale[i], 1, &v)) return -1;
            } else if (read_curve(x, &cur, 1, 2, &oc->scale[i])) return -1;
        }
    }
    return b->bad ? -1 : 0;
}

static NdsResult read_anim(Buf *b, size_t base, const char *name, NdsJointAnim *a) {
    memcpy(a->name, name, 17);
    if (rd32(b, base) != 0x4341004Au) return NDS_ERR_FORMAT;   /* "J\0AC" */
    a->num_frames = rd16(b, base + 4);
    uint32_t n = rd16(b, base + 6);
    AnimCtx x = {b, base, base + rd32(b, base + 12), base + rd32(b, base + 16)};
    if (b->bad || n > 1024) return NDS_ERR_FORMAT;
    a->objs = calloc(n ? n : 1, sizeof *a->objs);
    if (!a->objs) return NDS_ERR_NOMEM;
    a->obj_count = n;
    for (uint32_t i = 0; i < n; i++) {
        if (read_object(&x, base + rd16(b, base + 20 + i * 2), &a->objs[i])) return NDS_ERR_FORMAT;
    }
    return NDS_OK;
}

NdsResult nds_jntanim_parse(const uint8_t *data, size_t size, NdsJointAnimSet *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!data || size < 16 || memcmp(data, "BCA0", 4)) return NDS_ERR_FORMAT;
    Buf b = {data, size, 0};
    size_t jnt = nds_nitro_block(&b, "JNT0");
    NdsDict d;
    if (!jnt || nds_dict_read(&b, jnt + 8, &d)) return NDS_ERR_FORMAT;
    out->anims = calloc(d.n ? d.n : 1, sizeof *out->anims);
    if (!out->anims) return NDS_ERR_NOMEM;
    uint32_t good = 0;
    for (uint32_t i = 0; i < d.n; i++) {
        NdsResult r = read_anim(&b, jnt + rd32(&b, d.ent[i]), d.name[i], &out->anims[i]);
        if (r == NDS_OK && b.bad) r = NDS_ERR_FORMAT;
        if (r != NDS_OK) {
            free_anim(&out->anims[i]);
            if (r == NDS_ERR_NOMEM) { nds_jntanim_free(out); return r; }
            b.bad = 0;
            continue;                              /* skip a bad animation, keep the rest */
        }
        good++;
    }
    out->count = d.n;                              /* failed entries stay empty (0 frames) */
    if (good == 0) {
        nds_jntanim_free(out);
        return NDS_ERR_FORMAT;
    }
    return NDS_OK;
}

/* ---- Evaluation ---- */

static void sample(const NdsCurve *c, int dim, double frame, double *out) {
    if (c->kind == 1) {
        memcpy(out, c->v, (size_t)dim * sizeof *out);
        return;
    }
    double pos = (frame - c->start) / (double)c->step;
    if (pos <= 0 || c->count == 1) pos = 0;
    if (pos > c->count - 1) pos = c->count - 1;
    uint32_t lo = (uint32_t)floor(pos), hi = lo + 1 < c->count ? lo + 1 : lo;
    double g = pos - lo;
    for (int i = 0; i < dim; i++) out[i] = c->v[(size_t)lo * dim + i] * (1 - g) + c->v[(size_t)hi * dim + i] * g;
}

NdsResult nds_jntanim_eval(const NdsModel *m, const NdsJointAnim *a, double frame, double *t,
                           double *r, double *s) {
    if (!m || !t || !r || !s) return NDS_ERR_ARG;
    for (uint32_t i = 0; i < m->node_count; i++) {
        memcpy(t + 3 * i, m->nodes[i].trans, 3 * sizeof *t);
        memcpy(r + 9 * i, m->nodes[i].rot, 9 * sizeof *r);
        memcpy(s + 3 * i, m->nodes[i].scale, 3 * sizeof *s);
    }
    for (uint32_t i = 0; a && i < a->obj_count; i++) {
        const NdsJointCurves *o = &a->objs[i];
        if (o->node >= m->node_count) continue;
        for (int k = 0; k < 3; k++) {
            if (o->trans[k].kind) sample(&o->trans[k], 1, frame, &t[3 * o->node + k]);
            if (o->scale[k].kind) sample(&o->scale[k], 1, frame, &s[3 * o->node + k]);
        }
        if (o->rot.kind) sample(&o->rot, 9, frame, &r[9 * o->node]);
    }
    return NDS_OK;
}

static void world_of(const NdsModel *m, const double *t, const double *r, const double *s, int *state,
                     double (*w)[12], uint32_t n) {
    if (state[n] == 2) return;
    if (state[n] == 1) {               /* cycle: treat as a root */
        memset(w[n], 0, sizeof w[n]);
        w[n][0] = w[n][5] = w[n][10] = 1;
        return;
    }
    state[n] = 1;
    double local[12];
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 3; c++) local[i * 4 + c] = r[9 * n + 3 * c + i] * s[3 * n + c];
        local[i * 4 + 3] = t[3 * n + i];
    }
    int p = m->nodes[n].parent;
    if (p < 0 || (uint32_t)p >= m->node_count) {
        memcpy(w[n], local, sizeof local);
    } else {
        world_of(m, t, r, s, state, w, (uint32_t)p);
        const double *pw = w[p];
        for (int i = 0; i < 3; i++) {
            for (int k = 0; k < 3; k++)
                w[n][i * 4 + k] = pw[i * 4] * local[k] + pw[i * 4 + 1] * local[4 + k] + pw[i * 4 + 2] * local[8 + k];
            w[n][i * 4 + 3] = pw[i * 4] * local[3] + pw[i * 4 + 1] * local[7] + pw[i * 4 + 2] * local[11] + pw[i * 4 + 3];
        }
    }
    state[n] = 2;
}

NdsResult nds_model_pose(const NdsModel *m, const NdsJointAnim *a, double frame, NdsVertex *out) {
    if (!m || !out) return NDS_ERR_ARG;
    uint32_t nn = m->node_count;
    double *buf = malloc((nn ? nn : 1) * 15 * sizeof *buf);
    double (*w)[12] = malloc((nn ? nn : 1) * sizeof *w), (*inv)[12] = malloc((nn ? nn : 1) * sizeof *inv);
    int *state = calloc(nn ? nn : 1, sizeof *state), *ok = calloc(nn ? nn : 1, sizeof *ok);
    if (!buf || !w || !inv || !state || !ok) {
        free(buf); free(w); free(inv); free(state); free(ok);
        return NDS_ERR_NOMEM;
    }
    double *t = buf, *r = buf + 3 * nn, *s = buf + 12 * nn;
    nds_jntanim_eval(m, a, frame, t, r, s);
    for (uint32_t i = 0; i < nn; i++) {
        world_of(m, t, r, s, state, w, i);
        ok[i] = nds_affine_inverse(m->nodes[i].world, inv[i]) == 0;
    }
    for (uint32_t i = 0; i < m->tri_count * 3; i++) {
        out[i] = m->verts[i];
        int n = m->tri_node ? m->tri_node[i / 3] : -1;
        if (n < 0 || (uint32_t)n >= nn || !ok[n]) continue;
        double p[3] = {m->verts[i].pos[0], m->verts[i].pos[1], m->verts[i].pos[2]}, l[3], q[3];
        for (int k = 0; k < 3; k++)
            l[k] = inv[n][k * 4] * p[0] + inv[n][k * 4 + 1] * p[1] + inv[n][k * 4 + 2] * p[2] + inv[n][k * 4 + 3];
        for (int k = 0; k < 3; k++)
            q[k] = w[n][k * 4] * l[0] + w[n][k * 4 + 1] * l[1] + w[n][k * 4 + 2] * l[2] + w[n][k * 4 + 3];
        for (int k = 0; k < 3; k++) out[i].pos[k] = (float)q[k];
    }
    free(buf); free(w); free(inv); free(state); free(ok);
    return NDS_OK;
}
