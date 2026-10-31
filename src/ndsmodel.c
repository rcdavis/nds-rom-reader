#include "ndsrom/ndsmodel.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndsbuf.h"

#define MAX_TRIS (1u << 22)
#define STACK_SIZE 32

/* ---- 3x4 affine matrices (column-vector convention: p' = A * p) ---- */

typedef struct { double a[3][4]; } Mat;

static Mat mat_identity(void) {
    Mat m = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
    return m;
}

static Mat mat_mul(const Mat *x, const Mat *y) {
    Mat r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            r.a[i][j] = x->a[i][0] * y->a[0][j] + x->a[i][1] * y->a[1][j] + x->a[i][2] * y->a[2][j] +
                        (j == 3 ? x->a[i][3] : 0);
        }
    return r;
}

static void mat_apply(const Mat *m, const double p[3], double out[3]) {
    for (int i = 0; i < 3; i++)
        out[i] = m->a[i][0] * p[0] + m->a[i][1] * p[1] + m->a[i][2] * p[2] + m->a[i][3];
}

/* ---- Per-model parse state ---- */

typedef struct {
    size_t dl;
    uint32_t dl_size;
} Shape;

typedef struct {
    Buf *b;
    NdsModel *model;
    double pos_scale, inv_scale;
    NdsNode *nodes;       /* node transforms; filled in as the render commands run */
    uint32_t node_count;
    int stack_node[STACK_SIZE]; /* which node each stack matrix belongs to, or -1 */
    int cur_node;
    Shape *shapes;
    uint32_t shape_count;
    Mat stack[STACK_SIZE];
    size_t inv_bind;      /* offset of the inverse bind matrices, 0 if absent */
    uint32_t tri_cap;
    uint32_t cur_material;
} Ctx;

void nds_pivot_mat(unsigned sel, unsigned neg, double a, double b, double out[9]) {
    memset(out, 0, 9 * sizeof out[0]);
    if (sel >= 9) {
        out[0] = -a;
        return;
    }
    double o = (neg & 1) ? -1 : 1, c = (neg & 2) ? -b : b, d = (neg & 4) ? -a : a;
    static const int T[9][9] = {
        /* each row lists, for the 9 column-major cells, which symbol goes there:
           0 zero, 1 o, 2 a, 3 b, 4 c, 5 d */
        {1, 0, 0, 0, 2, 3, 0, 4, 5}, {0, 1, 0, 2, 0, 3, 4, 0, 5}, {0, 0, 1, 2, 3, 0, 4, 5, 0},
        {0, 2, 3, 1, 0, 0, 0, 4, 5}, {2, 0, 3, 0, 1, 0, 4, 0, 5}, {2, 3, 0, 0, 0, 1, 4, 5, 0},
        {0, 2, 3, 0, 4, 5, 1, 0, 0}, {2, 0, 3, 4, 0, 5, 0, 1, 0}, {2, 3, 0, 4, 5, 0, 0, 0, 1},
    };
    const double sym[6] = {0, o, a, b, c, d};
    for (int i = 0; i < 9; i++) out[i] = sym[T[sel][i]];
}

static Mat node_matrix(const NdsNode *n) {
    Mat m;
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 3; c++) m.a[i][c] = n->rot[3 * c + i] * n->scale[c];
        m.a[i][3] = n->trans[i];
    }
    return m;
}

static void read_node(Buf *b, size_t o, NdsNode *n) {
    uint32_t flags = rd16(b, o);
    int16_t m00 = (int16_t)rd16(b, o + 2);
    o += 4;
    static const double ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(n->rot, ident, sizeof ident);
    n->scale[0] = n->scale[1] = n->scale[2] = 1;
    n->trans[0] = n->trans[1] = n->trans[2] = 0;

    if (!(flags & 1)) {
        for (int i = 0; i < 3; i++) n->trans[i] = rds32(b, o + i * 4) / 4096.0;
        o += 12;
    }
    if (flags & 8) {
        double a = rds16(b, o) / 4096.0, bb = rds16(b, o + 2) / 4096.0;
        o += 4;
        nds_pivot_mat((flags >> 4) & 15, (flags >> 8) & 15, a, bb, n->rot);
    } else if (!(flags & 2)) {
        n->rot[0] = m00 / 4096.0;
        for (int i = 1; i < 9; i++) n->rot[i] = rds16(b, o + (i - 1) * 2) / 4096.0;
        o += 16;
    }
    if (!(flags & 4)) {
        for (int i = 0; i < 3; i++) n->scale[i] = rds32(b, o + i * 4) / 4096.0;
    }
}

/* ---- Materials ---- */

static void read_materials(Buf *b, size_t mats, NdsModel *m) {
    NdsDict md;
    if (nds_dict_read(b, mats + 4, &md)) return;
    m->materials = calloc(md.n ? md.n : 1, sizeof *m->materials);
    if (!m->materials) { b->bad = 1; return; }
    m->material_count = md.n;

    for (uint32_t i = 0; i < md.n; i++) {
        NdsMaterial *mt = &m->materials[i];
        size_t o = mats + rd32(b, md.ent[i]);
        memcpy(mt->name, md.name[i], 17);
        uint32_t dif = rd32(b, o + 4), attr = rd32(b, o + 12);
        mt->diffuse[0] = (dif & 31) / 31.0f;
        mt->diffuse[1] = ((dif >> 5) & 31) / 31.0f;
        mt->diffuse[2] = ((dif >> 10) & 31) / 31.0f;
        mt->alpha = ((attr >> 16) & 31) / 31.0f;
        mt->cull_back = !((attr >> 6) & 1);
        mt->cull_front = !((attr >> 7) & 1);
        mt->width = rd16(b, o + 0x20);
        mt->height = rd16(b, o + 0x22);
    }

    for (int pass = 0; pass < 2; pass++) {
        NdsDict nd;
        size_t list = mats + rd16(b, mats + pass * 2);
        if (nds_dict_read(b, list, &nd)) { b->bad = 0; continue; }
        for (uint32_t i = 0; i < nd.n; i++) {
            size_t ids = mats + rd16(b, nd.ent[i]);
            uint32_t cnt = rd8(b, nd.ent[i] + 2);
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t id = rd8(b, ids + k);
                if (id >= m->material_count) continue;
                char *dst = pass ? m->materials[id].pal_name : m->materials[id].tex_name;
                memcpy(dst, nd.name[i], 17);
            }
        }
    }
}

/* ---- Display lists ---- */

static int cmd_params(unsigned c) {
    switch (c) {
    case 0x00: case 0x11: case 0x15: case 0x41: return 0;
    case 0x10: case 0x12: case 0x13: case 0x14: case 0x20: case 0x21: case 0x22: case 0x24:
    case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x30:
    case 0x31: case 0x32: case 0x33: case 0x40: case 0x50: case 0x60: case 0x72:
        return 1;
    case 0x23: case 0x71: return 2;
    case 0x1B: case 0x1C: case 0x70: return 3;
    case 0x1A: return 9;
    case 0x17: case 0x19: return 12;
    case 0x16: case 0x18: return 16;
    case 0x34: return 32;
    default: return -1;
    }
}

static int sext(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1;
    return (int)((v ^ m) - m);
}

typedef struct {
    NdsVertex v[4096];
} VtxBuf;

static int add_tri(Ctx *c, const NdsVertex *a, const NdsVertex *b2, const NdsVertex *c2) {
    NdsModel *m = c->model;
    if (m->tri_count >= MAX_TRIS) return -1;
    if (m->tri_count == c->tri_cap) {
        uint32_t cap = c->tri_cap ? c->tri_cap * 2 : 1024;
        NdsVertex *nv = realloc(m->verts, (size_t)cap * 3 * sizeof *nv);
        if (!nv) return -1;
        m->verts = nv;
        uint16_t *nm = realloc(m->tri_material, (size_t)cap * sizeof *nm);
        if (!nm) return -1;
        m->tri_material = nm;
        int16_t *nn = realloc(m->tri_node, (size_t)cap * sizeof *nn);
        if (!nn) return -1;
        m->tri_node = nn;
        c->tri_cap = cap;
    }
    NdsVertex *dst = m->verts + (size_t)m->tri_count * 3;
    dst[0] = *a;
    dst[1] = *b2;
    dst[2] = *c2;
    m->tri_node[m->tri_count] = (int16_t)c->cur_node;
    m->tri_material[m->tri_count++] = (uint16_t)c->cur_material;
    return 0;
}

static int run_display_list(Ctx *c, const Shape *sh, const Mat *base) {
    Buf *b = c->b;
    const NdsMaterial *mt = c->cur_material < c->model->material_count
                                ? &c->model->materials[c->cur_material] : NULL;
    double tw = mt && mt->width ? mt->width : 1, th = mt && mt->height ? mt->height : 1;

    Mat g = *base;
    int raw[3] = {0, 0, 0};
    double s = 0, t = 0;
    float color[3] = {1, 1, 1};
    int prim = -1;
    uint32_t nverts = 0;
    NdsVertex pv[4];          /* the last vertices of the current primitive */
    uint32_t count = 0;       /* vertices since BEGIN_VTXS */

    size_t pos = sh->dl, end = sh->dl + sh->dl_size;
    if (end > b->n) end = b->n;

    while (pos + 4 <= end && !b->bad) {
        uint8_t cmds[4];
        for (int i = 0; i < 4; i++) cmds[i] = (uint8_t)rd8(b, pos + i);
        pos += 4;
        for (int i = 0; i < 4; i++) {
            unsigned cmd = cmds[i];
            int np = cmd_params(cmd);
            if (np < 0) return 0;
            if (pos + (size_t)np * 4 > end) return 0;
            size_t p = pos;
            pos += (size_t)np * 4;

            switch (cmd) {
            case 0x13: c->stack[rd32(b, p) & 31] = g; break;
            case 0x14: g = c->stack[rd32(b, p) & 31]; break;
            case 0x15: g = mat_identity(); break;
            case 0x1B: {
                Mat sm = mat_identity();
                for (int k = 0; k < 3; k++) sm.a[k][k] = rds32(b, p + k * 4) / 4096.0;
                g = mat_mul(&g, &sm);
                break;
            }
            case 0x1C: {
                Mat tm = mat_identity();
                for (int k = 0; k < 3; k++) tm.a[k][3] = rds32(b, p + k * 4) / 4096.0;
                g = mat_mul(&g, &tm);
                break;
            }
            case 0x20: {
                uint32_t v = rd32(b, p);
                color[0] = (v & 31) / 31.0f;
                color[1] = ((v >> 5) & 31) / 31.0f;
                color[2] = ((v >> 10) & 31) / 31.0f;
                break;
            }
            case 0x22: {
                uint32_t v = rd32(b, p);
                s = (int16_t)(v & 0xFFFF) / 16.0;
                t = (int16_t)(v >> 16) / 16.0;
                break;
            }
            case 0x40:
                prim = (int)(rd32(b, p) & 3);
                count = 0;
                nverts = 0;
                break;
            case 0x41: prim = -1; break;
            case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: {
                uint32_t v = rd32(b, p);
                if (cmd == 0x23) {
                    raw[0] = (int16_t)(v & 0xFFFF);
                    raw[1] = (int16_t)(v >> 16);
                    raw[2] = (int16_t)(rd32(b, p + 4) & 0xFFFF);
                } else if (cmd == 0x24) {
                    raw[0] = sext(v, 10) * 64;
                    raw[1] = sext(v >> 10, 10) * 64;
                    raw[2] = sext(v >> 20, 10) * 64;
                } else if (cmd == 0x25) {
                    raw[0] = (int16_t)(v & 0xFFFF);
                    raw[1] = (int16_t)(v >> 16);
                } else if (cmd == 0x26) {
                    raw[0] = (int16_t)(v & 0xFFFF);
                    raw[2] = (int16_t)(v >> 16);
                } else if (cmd == 0x27) {
                    raw[1] = (int16_t)(v & 0xFFFF);
                    raw[2] = (int16_t)(v >> 16);
                } else {
                    raw[0] += sext(v, 10);
                    raw[1] += sext(v >> 10, 10);
                    raw[2] += sext(v >> 20, 10);
                }
                if (prim < 0) break;

                double in[3] = {raw[0] / 4096.0, raw[1] / 4096.0, raw[2] / 4096.0}, w[3];
                mat_apply(&g, in, w);
                NdsVertex nv;
                for (int k = 0; k < 3; k++) {
                    nv.pos[k] = (float)w[k];
                    nv.color[k] = color[k];
                }
                nv.uv[0] = (float)(s / tw);
                nv.uv[1] = (float)(t / th);

                /* Keep the last four vertices; emit triangles as primitives complete. */
                if (nverts < 4) pv[nverts++] = nv;
                else {
                    memmove(pv, pv + 1, 3 * sizeof pv[0]);
                    pv[3] = nv;
                }
                count++;
                int rc = 0;
                if (prim == 0 && count % 3 == 0)
                    rc = add_tri(c, &pv[nverts - 3], &pv[nverts - 2], &pv[nverts - 1]);
                else if (prim == 1 && count % 4 == 0) {
                    rc = add_tri(c, &pv[0], &pv[1], &pv[2]);
                    if (!rc) rc = add_tri(c, &pv[0], &pv[2], &pv[3]);
                } else if (prim == 2 && count >= 3) {
                    if (count & 1) rc = add_tri(c, &pv[nverts - 2], &pv[nverts - 3], &pv[nverts - 1]);
                    else rc = add_tri(c, &pv[nverts - 3], &pv[nverts - 2], &pv[nverts - 1]);
                } else if (prim == 3 && count >= 4 && count % 2 == 0) {
                    rc = add_tri(c, &pv[0], &pv[1], &pv[3]);
                    if (!rc) rc = add_tri(c, &pv[0], &pv[3], &pv[2]);
                }
                if (rc) return -1;
                break;
            }
            default: break;
            }
        }
    }
    return 0;
}

/* ---- Render commands (SBC) ---- */

static int run_sbc(Ctx *c, size_t pc, size_t limit) {
    Buf *b = c->b;
    Mat cur = mat_identity();
    int visible = 1;
    for (int i = 0; i < STACK_SIZE; i++) {
        c->stack[i] = mat_identity();
        c->stack_node[i] = -1;
    }
    c->cur_node = -1;

    for (int steps = 0; steps < 100000 && pc < limit && !b->bad; steps++) {
        unsigned op = rd8(b, pc++), cmd = op & 0x1F;
        switch (cmd) {
        case 0x00: break;
        case 0x01: return 0;
        case 0x02:
            visible = rd8(b, pc + 1) != 0;
            pc += 2;
            break;
        case 0x03:
            cur = c->stack[rd8(b, pc) & 31];
            c->cur_node = c->stack_node[rd8(b, pc) & 31];
            pc += 1;
            break;
        case 0x04:
            c->cur_material = rd8(b, pc);
            pc += 1;
            break;
        case 0x05: {
            uint32_t id = rd8(b, pc++);
            if (visible && id < c->shape_count && run_display_list(c, &c->shapes[id], &cur)) return -1;
            break;
        }
        case 0x06: {
            uint32_t node = rd8(b, pc);
            pc += 3;
            int store = -1, restore = -1;
            if (op & 0x20) store = rd8(b, pc++) & 31;
            if (op & 0x40) restore = rd8(b, pc++) & 31;
            if (restore >= 0) {
                cur = c->stack[restore];
                c->cur_node = c->stack_node[restore];
            }
            if (node < c->node_count) {
                NdsNode *nd = &c->nodes[node];
                Mat local = node_matrix(nd);
                if (!nd->placed) nd->parent = c->cur_node;
                cur = mat_mul(&cur, &local);
                if (!nd->placed)
                    for (int r = 0; r < 3; r++)
                        for (int k = 0; k < 4; k++) nd->world[r * 4 + k] = cur.a[r][k];
                nd->placed = 1;
                c->cur_node = (int)node;
            } else c->cur_node = -1;
            if (store >= 0) {
                c->stack[store] = cur;
                c->stack_node[store] = c->cur_node;
            }
            break;
        }
        case 0x07: case 0x08: pc += 1; break;
        case 0x09: {
            uint32_t dst = rd8(b, pc) & 31, n = rd8(b, pc + 1);
            pc += 2;
            Mat acc;
            memset(&acc, 0, sizeof acc);
            for (uint32_t k = 0; k < n && !b->bad; k++, pc += 3) {
                uint32_t src = rd8(b, pc) & 31, joint = rd8(b, pc + 1);
                double w = rd8(b, pc + 2) / 256.0;
                Mat ib = mat_identity();
                if (c->inv_bind && joint < c->node_count) {
                    size_t o = c->inv_bind + (size_t)joint * 0x54;
                    for (int col = 0; col < 4; col++)
                        for (int r = 0; r < 3; r++) ib.a[r][col] = rds32(b, o + (col * 3 + r) * 4) / 4096.0;
                }
                Mat prod = mat_mul(&c->stack[src], &ib);
                for (int r = 0; r < 3; r++)
                    for (int col = 0; col < 4; col++) acc.a[r][col] += w * prod.a[r][col];
            }
            c->stack[dst] = acc;
            c->stack_node[dst] = -1;
            break;
        }
        case 0x0A: pc += 8; break;
        case 0x0B: {
            double sc = (op & 0x20) ? c->inv_scale : c->pos_scale;
            Mat sm = mat_identity();
            for (int k = 0; k < 3; k++) sm.a[k][k] = sc;
            cur = mat_mul(&cur, &sm);
            break;
        }
        case 0x0C: case 0x0D: pc += 2; break;
        default: return 0;
        }
    }
    return 0;
}

/* ---- Model / file parse ---- */

static void free_model(NdsModel *m) {
    free(m->materials);
    free(m->verts);
    free(m->tri_material);
    free(m->tri_node);
    free(m->nodes);
    memset(m, 0, sizeof *m);
}

static NdsResult parse_model(Buf *b, size_t mo, const char *name, NdsModel *out) {
    memcpy(out->name, name, 17);
    Ctx c;
    memset(&c, 0, sizeof c);
    c.b = b;
    c.model = out;
    c.pos_scale = rds32(b, mo + 0x1C) / 4096.0;
    c.inv_scale = rds32(b, mo + 0x20) / 4096.0;
    if (c.pos_scale == 0) c.pos_scale = 1;
    if (c.inv_scale == 0) c.inv_scale = 1;

    size_t sbc = mo + rd32(b, mo + 4), mats = mo + rd32(b, mo + 8), polys = mo + rd32(b, mo + 12);
    uint32_t inv = rd32(b, mo + 0x10), msize = rd32(b, mo);
    if (b->bad) return NDS_ERR_FORMAT;
    c.inv_bind = inv && inv < msize ? mo + inv : 0;

    NdsDict nodes, shapes;
    if (nds_dict_read(b, mo + 0x40, &nodes) || nds_dict_read(b, polys, &shapes)) return NDS_ERR_FORMAT;

    c.nodes = calloc(nodes.n ? nodes.n : 1, sizeof *c.nodes);
    c.shapes = malloc((shapes.n ? shapes.n : 1) * sizeof *c.shapes);
    if (!c.nodes || !c.shapes) {
        free(c.nodes);
        free(c.shapes);
        return NDS_ERR_NOMEM;
    }
    c.node_count = nodes.n;
    c.shape_count = shapes.n;
    for (uint32_t i = 0; i < nodes.n; i++) {
        memcpy(c.nodes[i].name, nodes.name[i], 17);
        c.nodes[i].parent = -1;
        read_node(b, mo + 0x40 + rd32(b, nodes.ent[i]), &c.nodes[i]);
    }
    for (uint32_t i = 0; i < shapes.n; i++) {
        size_t so = polys + rd32(b, shapes.ent[i]);
        c.shapes[i].dl = so + rd32(b, so + 8);
        c.shapes[i].dl_size = rd32(b, so + 12);
    }
    read_materials(b, mats, out);

    NdsResult r = NDS_OK;
    if (b->bad) r = NDS_ERR_FORMAT;
    else if (run_sbc(&c, sbc, b->n)) r = NDS_ERR_NOMEM;
    if (b->bad && r == NDS_OK) r = NDS_ERR_FORMAT;

    free(c.shapes);
    if (r != NDS_OK) {
        free(c.nodes);
        return r;
    }
    out->nodes = c.nodes;
    out->node_count = c.node_count;
    for (uint32_t i = 0; i < out->node_count; i++) {
        NdsNode *nd = &out->nodes[i];
        if (nd->placed) continue;
        Mat local = node_matrix(nd);
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 4; k++) nd->world[r * 4 + k] = local.a[r][k];
    }

    for (int k = 0; k < 3; k++) out->bmin[k] = out->bmax[k] = 0;
    for (uint32_t i = 0; i < out->tri_count * 3; i++)
        for (int k = 0; k < 3; k++) {
            float v = out->verts[i].pos[k];
            if (i == 0 || v < out->bmin[k]) out->bmin[k] = v;
            if (i == 0 || v > out->bmax[k]) out->bmax[k] = v;
        }
    return NDS_OK;
}

void nds_modelfile_free(NdsModelFile *mf) {
    if (!mf) return;
    for (uint32_t i = 0; i < mf->model_count; i++) free_model(&mf->models[i]);
    free(mf->models);
    nds_texset_free(&mf->tex);
    memset(mf, 0, sizeof *mf);
}

NdsResult nds_modelfile_parse(const uint8_t *data, size_t size, NdsModelFile *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!data || size < 16 || memcmp(data, "BMD0", 4)) return NDS_ERR_FORMAT;

    Buf b = {data, size, 0};
    size_t mdl = nds_nitro_block(&b, "MDL0");
    if (!mdl) return NDS_ERR_FORMAT;

    NdsDict md;
    if (nds_dict_read(&b, mdl + 8, &md)) return NDS_ERR_FORMAT;
    out->models = calloc(md.n ? md.n : 1, sizeof *out->models);
    if (!out->models) return NDS_ERR_NOMEM;
    out->model_count = md.n;

    for (uint32_t i = 0; i < md.n; i++) {
        NdsResult r = parse_model(&b, mdl + rd32(&b, md.ent[i]), md.name[i], &out->models[i]);
        if (r != NDS_OK) {
            nds_modelfile_free(out);
            return r;
        }
    }
    if (nds_texset_parse(data, size, &out->tex) == NDS_OK) out->has_tex = 1;
    return NDS_OK;
}

/* ---- OBJ export ---- */

static void sanitize(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        char ch = src[i];
        dst[i] = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                         ch == '_' || ch == '-' || ch == '.'
                     ? ch : '_';
    }
    dst[i] = 0;
}

NdsResult nds_model_write_obj(const NdsModel *m, const NdsTexSet *tex, const char *dir,
                              const char *base) {
    if (!m || !dir || !base) return NDS_ERR_ARG;
    size_t plen = strlen(dir) + strlen(base) + 64;
    char *path = malloc(plen);
    if (!path) return NDS_ERR_NOMEM;
    NdsResult res = NDS_OK;

    snprintf(path, plen, "%s/%s.mtl", dir, base);
    FILE *mtl = fopen(path, "w");
    snprintf(path, plen, "%s/%s.obj", dir, base);
    FILE *obj = fopen(path, "w");
    if (!mtl || !obj) {
        res = NDS_ERR_IO;
        goto done;
    }

    for (uint32_t i = 0; i < m->material_count; i++) {
        const NdsMaterial *mt = &m->materials[i];
        char mname[40];
        sanitize(mname, mt->name, sizeof mname);
        fprintf(mtl, "newmtl %s\nKd %.3f %.3f %.3f\nd %.3f\n", mname, mt->diffuse[0], mt->diffuse[1],
                mt->diffuse[2], mt->alpha > 0 ? mt->alpha : 1.0f);
        int ti = mt->tex_name[0] ? nds_texset_find_tex(tex, mt->tex_name) : -1;
        if (ti >= 0) {
            int pi = mt->pal_name[0] ? nds_texset_find_pal(tex, mt->pal_name) : -1;
            NdsImage img;
            char tname[40];
            sanitize(tname, mt->tex_name, sizeof tname);
            if (nds_texset_decode(tex, (uint32_t)ti, pi, &img) == NDS_OK) {
                snprintf(path, plen, "%s/%s.png", dir, tname);
                NdsResult wr = nds_write_png(path, &img);
                free(img.rgba);
                if (wr == NDS_OK) fprintf(mtl, "map_Kd %s.png\n", tname);
            }
        }
        fputc('\n', mtl);
    }

    fprintf(obj, "mtllib %s.mtl\no %s\n", base, m->name);
    for (uint32_t i = 0; i < m->tri_count * 3; i++) {
        const NdsVertex *v = &m->verts[i];
        fprintf(obj, "v %.5f %.5f %.5f\nvt %.5f %.5f\n", v->pos[0], v->pos[1], v->pos[2], v->uv[0],
                1.0f - v->uv[1]);
    }
    for (uint32_t mi = 0; mi < m->material_count; mi++) {
        int started = 0;
        for (uint32_t t = 0; t < m->tri_count; t++) {
            if (m->tri_material[t] != mi) continue;
            if (!started) {
                char mname[40];
                sanitize(mname, m->materials[mi].name, sizeof mname);
                fprintf(obj, "usemtl %s\n", mname);
                started = 1;
            }
            uint32_t a = t * 3 + 1;
            fprintf(obj, "f %u/%u %u/%u %u/%u\n", a, a, a + 2, a + 2, a + 1, a + 1);
        }
    }

done:
    if (mtl) fclose(mtl);
    if (obj) fclose(obj);
    free(path);
    return res;
}
