#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndsbuf.h"
#include "ndsrom/ndsmodel.h"

#define ANIM_FPS 30.0

/* ---- Small growable buffers ---- */

typedef struct { uint8_t *d; size_t n, cap; int fail; } Bytes;
typedef struct { char *s; size_t n, cap; int fail; } Str;

static void bytes_add(Bytes *b, const void *p, size_t n) {
    if (b->fail) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t *nd = realloc(b->d, cap);
        if (!nd) { b->fail = 1; return; }
        b->d = nd;
        b->cap = cap;
    }
    if (n) memcpy(b->d + b->n, p, n);
    b->n += n;
}

static void bytes_align(Bytes *b) {
    static const uint8_t z[4] = {0, 0, 0, 0};
    if (b->n & 3) bytes_add(b, z, 4 - (b->n & 3));
}

static void str_printf(Str *s, const char *fmt, ...) {
    if (s->fail) return;
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof tmp) { s->fail = 1; return; }
    if (s->n + (size_t)n + 1 > s->cap) {
        size_t cap = s->cap ? s->cap : 1024;
        while (cap < s->n + (size_t)n + 1) cap *= 2;
        char *ns = realloc(s->s, cap);
        if (!ns) { s->fail = 1; return; }
        s->s = ns;
        s->cap = cap;
    }
    memcpy(s->s + s->n, tmp, (size_t)n + 1);
    s->n += (size_t)n;
}

static void str_append(Str *dst, const Str *src) {
    if (!src->s || dst->fail) return;
    if (dst->n + src->n + 1 > dst->cap) {
        size_t cap = dst->cap ? dst->cap : 1024;
        while (cap < dst->n + src->n + 1) cap *= 2;
        char *ns = realloc(dst->s, cap);
        if (!ns) { dst->fail = 1; return; }
        dst->s = ns;
        dst->cap = cap;
    }
    memcpy(dst->s + dst->n, src->s, src->n + 1);
    dst->n += src->n;
}

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

static uint8_t *slurp(const char *path, uint32_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = sz > 0 ? malloc((size_t)sz) : NULL;
    if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
    fclose(f);
    *n = d ? (uint32_t)sz : 0;
    return d;
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- Document under construction ---- */

typedef struct {
    Bytes bin;
    Str views, accs;
    int nviews, naccs;
} Doc;

static int add_view(Doc *d, const void *data, size_t len, int target) {
    bytes_align(&d->bin);
    size_t off = d->bin.n;
    bytes_add(&d->bin, data, len);
    str_printf(&d->views, "%s{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu", d->nviews ? "," : "", off, len);
    if (target) str_printf(&d->views, ",\"target\":%d", target);
    str_printf(&d->views, "}");
    return d->nviews++;
}

/* ctype: 5126 float, 5125 u32. ncomp > 0 emits min/max (float accessors only). */
static int add_acc(Doc *d, int view, int ctype, uint32_t count, const char *type, const float *mn,
                   const float *mx, int ncomp) {
    str_printf(&d->accs, "%s{\"bufferView\":%d,\"componentType\":%d,\"count\":%u,\"type\":\"%s\"",
               d->naccs ? "," : "", view, ctype, count, type);
    if (ncomp) {
        str_printf(&d->accs, ",\"min\":[");
        for (int i = 0; i < ncomp; i++) str_printf(&d->accs, "%s%.9g", i ? "," : "", mn[i]);
        str_printf(&d->accs, "],\"max\":[");
        for (int i = 0; i < ncomp; i++) str_printf(&d->accs, "%s%.9g", i ? "," : "", mx[i]);
        str_printf(&d->accs, "]");
    }
    str_printf(&d->accs, "}");
    return d->naccs++;
}

/* ---- Rotation helpers ---- */

/* rot is column-major (rot[3*c + i]); returns a unit quaternion x,y,z,w. */
static void mat_to_quat(const double *rot, double q[4]) {
    double m[3][3];
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) m[i][c] = rot[3 * c + i];
    /* Orthonormalise the columns so non-orthogonal data still gives a valid quaternion. */
    double col[3][3];
    for (int c = 0; c < 3; c++) {
        for (int i = 0; i < 3; i++) col[c][i] = m[i][c];
        for (int k = 0; k < c; k++) {
            double dot = col[c][0] * col[k][0] + col[c][1] * col[k][1] + col[c][2] * col[k][2];
            for (int i = 0; i < 3; i++) col[c][i] -= dot * col[k][i];
        }
        double len = sqrt(col[c][0] * col[c][0] + col[c][1] * col[c][1] + col[c][2] * col[c][2]);
        if (len < 1e-9) { q[0] = q[1] = q[2] = 0; q[3] = 1; return; }
        for (int i = 0; i < 3; i++) col[c][i] /= len;
    }
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 3; i++) m[i][c] = col[c][i];

    double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        double s = sqrt(tr + 1) * 2;
        q[3] = s / 4; q[0] = (m[2][1] - m[1][2]) / s; q[1] = (m[0][2] - m[2][0]) / s; q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        double s = sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s; q[0] = s / 4; q[1] = (m[0][1] + m[1][0]) / s; q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        double s = sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s; q[0] = (m[0][1] + m[1][0]) / s; q[1] = s / 4; q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        double s = sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s; q[0] = (m[0][2] + m[2][0]) / s; q[1] = (m[1][2] + m[2][1]) / s; q[2] = s / 4;
    }
    double len = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= len;
}

/* ---- Export ---- */

typedef struct {
    uint32_t *tris;            /* triangle ids owned by this group */
    uint32_t n;
    int mesh;
} Group;

static NdsResult write_gltf(const NdsModel *m, const NdsTexSet *tex, const NdsJointAnimSet *anims,
                            const char *dir, const char *base, int glb) {
    if (!m || !dir || !base) return NDS_ERR_ARG;
    if (m->tri_count == 0) return NDS_ERR_NOTFOUND;

    uint32_t N = m->node_count, nmat = m->material_count, ngroups = N + 1;
    size_t plen = strlen(dir) + strlen(base) + 64;
    char *path = malloc(plen);
    Group *groups = calloc(ngroups, sizeof *groups);
    uint32_t *group_of = malloc(m->tri_count * sizeof *group_of);
    double (*inv)[12] = calloc(N ? N : 1, sizeof *inv);
    int *inv_ok = calloc(N ? N : 1, sizeof *inv_ok);
    int *image = calloc(nmat ? nmat : 1, sizeof *image);
    char (*img_name)[40] = calloc(nmat ? nmat : 1, sizeof *img_name);
    uint8_t **png = calloc(nmat ? nmat : 1, sizeof *png);
    uint32_t *png_len = calloc(nmat ? nmat : 1, sizeof *png_len);
    Doc doc;
    memset(&doc, 0, sizeof doc);
    Str meshes = {0}, mats = {0}, nodes = {0}, texs = {0}, anim_json = {0}, json = {0};
    NdsResult res = NDS_OK;
    uint32_t nimg = 0;

    if (!path || !groups || !group_of || !inv || !inv_ok || !image || !img_name || !png || !png_len) {
        res = NDS_ERR_NOMEM;
        goto done;
    }

    /* Triangles go into the node that owns them (local space); the rest stay static. */
    for (uint32_t i = 0; i < N; i++) inv_ok[i] = nds_affine_inverse(m->nodes[i].world, inv[i]) == 0;
    for (uint32_t t = 0; t < m->tri_count; t++) {
        int n = m->tri_node ? m->tri_node[t] : -1;
        uint32_t g = (n >= 0 && (uint32_t)n < N && inv_ok[n]) ? (uint32_t)n : N;
        group_of[t] = g;
        groups[g].n++;
    }
    for (uint32_t g = 0; g < ngroups; g++) {
        groups[g].mesh = -1;
        if (!groups[g].n) continue;
        groups[g].tris = malloc(groups[g].n * sizeof(uint32_t));
        if (!groups[g].tris) { res = NDS_ERR_NOMEM; goto done; }
        groups[g].n = 0;
    }
    for (uint32_t t = 0; t < m->tri_count; t++) groups[group_of[t]].tris[groups[group_of[t]].n++] = t;

    /* One PNG per distinct texture used. */
    for (uint32_t i = 0; i < nmat; i++) {
        image[i] = -1;
        const NdsMaterial *mt = &m->materials[i];
        int used = 0;
        for (uint32_t t = 0; t < m->tri_count && !used; t++) used = m->tri_material[t] == i;
        int ti = mt->tex_name[0] && used ? nds_texset_find_tex(tex, mt->tex_name) : -1;
        if (ti < 0) continue;
        char name[40];
        sanitize(name, mt->tex_name, sizeof name);
        for (uint32_t k = 0; k < nimg; k++)
            if (!strcmp(img_name[k], name)) image[i] = (int)k;
        if (image[i] >= 0) continue;
        NdsImage img;
        int pi = mt->pal_name[0] ? nds_texset_find_pal(tex, mt->pal_name) : -1;
        if (nds_texset_decode(tex, (uint32_t)ti, pi, &img) != NDS_OK) continue;
        if (glb) snprintf(path, plen, "%s/%s.tmp.png", dir, base);
        else snprintf(path, plen, "%s/%s.png", dir, name);
        NdsResult wr = nds_write_png(path, &img);
        free(img.rgba);
        if (wr != NDS_OK) { res = wr; goto done; }
        if (glb) {
            png[nimg] = slurp(path, &png_len[nimg]);
            remove(path);
            if (!png[nimg]) { res = NDS_ERR_IO; goto done; }
        }
        strcpy(img_name[nimg], name);
        image[i] = (int)nimg++;
    }

    /* Meshes: positions (local space), uvs, then an index list per material. */
    int nmesh = 0;
    for (uint32_t g = 0; g < ngroups; g++) {
        Group *gr = &groups[g];
        if (!gr->n) continue;
        uint32_t nv = gr->n * 3;
        float *pos = malloc(nv * 3 * sizeof *pos), *uv = malloc(nv * 2 * sizeof *uv);
        if (!pos || !uv) { free(pos); free(uv); res = NDS_ERR_NOMEM; goto done; }
        float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
        for (uint32_t j = 0; j < gr->n; j++) {
            for (int k = 0; k < 3; k++) {
                const NdsVertex *v = &m->verts[(size_t)gr->tris[j] * 3 + k];
                double p[3] = {v->pos[0], v->pos[1], v->pos[2]}, l[3];
                if (g < N) {
                    for (int a = 0; a < 3; a++)
                        l[a] = inv[g][a * 4] * p[0] + inv[g][a * 4 + 1] * p[1] + inv[g][a * 4 + 2] * p[2] + inv[g][a * 4 + 3];
                } else memcpy(l, p, sizeof l);
                for (int a = 0; a < 3; a++) {
                    float f = (float)l[a];
                    pos[(j * 3 + k) * 3 + a] = f;
                    if (j == 0 && k == 0) mn[a] = mx[a] = f;
                    if (f < mn[a]) mn[a] = f;
                    if (f > mx[a]) mx[a] = f;
                }
                uv[(j * 3 + k) * 2] = v->uv[0];
                uv[(j * 3 + k) * 2 + 1] = v->uv[1];
            }
        }
        int pv = add_view(&doc, pos, nv * 12, 34962), uvv = add_view(&doc, uv, nv * 8, 34962);
        free(pos);
        free(uv);
        int pa = add_acc(&doc, pv, 5126, nv, "VEC3", mn, mx, 3), ua = add_acc(&doc, uvv, 5126, nv, "VEC2", NULL, NULL, 0);

        str_printf(&meshes, "%s{\"name\":\"mesh%u\",\"primitives\":[", nmesh ? "," : "", g);
        int first = 1;
        for (uint32_t mi = 0; mi < nmat; mi++) {
            uint32_t *idx = malloc(gr->n * 3 * sizeof *idx), cnt = 0;
            if (!idx) { res = NDS_ERR_NOMEM; goto done; }
            for (uint32_t j = 0; j < gr->n; j++) {
                if (m->tri_material[gr->tris[j]] != mi) continue;
                /* NDS front faces are clockwise, glTF's are counter-clockwise */
                idx[cnt++] = j * 3;
                idx[cnt++] = j * 3 + 2;
                idx[cnt++] = j * 3 + 1;
            }
            if (cnt) {
                int iv = add_view(&doc, idx, cnt * 4, 34963), ia = add_acc(&doc, iv, 5125, cnt, "SCALAR", NULL, NULL, 0);
                str_printf(&meshes, "%s{\"attributes\":{\"POSITION\":%d,\"TEXCOORD_0\":%d},\"indices\":%d,\"material\":%u}",
                           first ? "" : ",", pa, ua, ia, mi);
                first = 0;
            }
            free(idx);
        }
        str_printf(&meshes, "]}");
        gr->mesh = nmesh++;
    }

    /* Materials */
    for (uint32_t mi = 0; mi < nmat; mi++) {
        const NdsMaterial *mt = &m->materials[mi];
        char name[40];
        sanitize(name, mt->name, sizeof name);
        float a = mt->alpha > 0 ? mt->alpha : 1.0f;
        int tx = image[mi] >= 0;
        str_printf(&mats, "%s{\"name\":\"%s\",\"doubleSided\":%s,\"pbrMetallicRoughness\":{"
                          "\"baseColorFactor\":[%.4f,%.4f,%.4f,%.4f],\"metallicFactor\":0,\"roughnessFactor\":1",
                   mi ? "," : "", name, (!mt->cull_back && !mt->cull_front) ? "true" : "false",
                   tx ? 1.0f : mt->diffuse[0], tx ? 1.0f : mt->diffuse[1], tx ? 1.0f : mt->diffuse[2], a);
        if (tx) str_printf(&mats, ",\"baseColorTexture\":{\"index\":%d}", image[mi]);
        str_printf(&mats, "}");
        if (tx || a < 1.0f) str_printf(&mats, ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5");
        str_printf(&mats, "}");
    }

    /* Nodes: the skeleton, plus one extra node for geometry with no owner. */
    uint32_t total_nodes = N + (groups[N].n ? 1 : 0);
    for (uint32_t i = 0; i < N; i++) {
        const NdsNode *nd = &m->nodes[i];
        char name[40];
        sanitize(name, nd->name, sizeof name);
        double q[4];
        mat_to_quat(nd->rot, q);
        str_printf(&nodes, "%s{\"name\":\"%s\",\"translation\":[%.9g,%.9g,%.9g],\"rotation\":[%.9g,%.9g,%.9g,%.9g],"
                           "\"scale\":[%.9g,%.9g,%.9g]",
                   i ? "," : "", name[0] ? name : "node", nd->trans[0], nd->trans[1], nd->trans[2], q[0], q[1], q[2],
                   q[3], nd->scale[0], nd->scale[1], nd->scale[2]);
        int nchild = 0;
        for (uint32_t c = 0; c < N; c++) {
            if (m->nodes[c].parent != (int)i || c == i) continue;
            str_printf(&nodes, "%s%u", nchild++ ? "," : ",\"children\":[", c);
        }
        if (nchild) str_printf(&nodes, "]");
        if (groups[i].mesh >= 0) str_printf(&nodes, ",\"mesh\":%d", groups[i].mesh);
        str_printf(&nodes, "}");
    }
    if (groups[N].n) str_printf(&nodes, "%s{\"name\":\"static\",\"mesh\":%d}", N ? "," : "", groups[N].mesh);

    Str roots = {0};
    for (uint32_t i = 0; i < N; i++) {
        int p = m->nodes[i].parent;
        if (p < 0 || (uint32_t)p >= N || (uint32_t)p == i) str_printf(&roots, "%s%u", roots.n ? "," : "", i);
    }
    if (groups[N].n) str_printf(&roots, "%s%u", roots.n ? "," : "", N);

    /* Animations: every integer frame is sampled for each node the animation drives. */
    int nanim = 0;
    for (uint32_t ai = 0; anims && ai < anims->count; ai++) {
        const NdsJointAnim *an = &anims->anims[ai];
        if (!an->num_frames || !an->obj_count || !N) continue;
        uint32_t F = an->num_frames;
        double *t = malloc((size_t)N * 15 * sizeof *t);
        float *times = malloc(F * sizeof *times);
        if (!t || !times) { free(t); free(times); res = NDS_ERR_NOMEM; goto done; }
        double *r = t + 3 * N, *s = t + 12 * N;
        for (uint32_t f = 0; f < F; f++) times[f] = (float)(f / ANIM_FPS);
        float tmin = 0, tmax = times[F - 1];
        int tv = add_view(&doc, times, F * 4, 0), ta = add_acc(&doc, tv, 5126, F, "SCALAR", &tmin, &tmax, 1);
        free(times);

        float *tr = malloc((size_t)F * 3 * sizeof *tr), *sc = malloc((size_t)F * 3 * sizeof *sc), *ro = malloc((size_t)F * 4 * sizeof *ro);
        Str samplers = {0}, channels = {0};
        int ns = 0;
        if (!tr || !sc || !ro) { free(tr); free(sc); free(ro); free(t); res = NDS_ERR_NOMEM; goto done; }
        for (uint32_t oi = 0; oi < an->obj_count; oi++) {
            const NdsJointCurves *o = &an->objs[oi];
            if (o->node >= N) continue;
            int has_t = o->trans[0].kind || o->trans[1].kind || o->trans[2].kind;
            int has_s = o->scale[0].kind || o->scale[1].kind || o->scale[2].kind;
            int has_r = o->rot.kind;
            if (!has_t && !has_s && !has_r) continue;
            double prev[4] = {0, 0, 0, 1};
            for (uint32_t f = 0; f < F; f++) {
                nds_jntanim_eval(m, an, f, t, r, s);
                for (int k = 0; k < 3; k++) {
                    tr[f * 3 + k] = (float)t[3 * o->node + k];
                    sc[f * 3 + k] = (float)s[3 * o->node + k];
                }
                double q[4];
                mat_to_quat(&r[9 * o->node], q);
                if (f && q[0] * prev[0] + q[1] * prev[1] + q[2] * prev[2] + q[3] * prev[3] < 0)
                    for (int k = 0; k < 4; k++) q[k] = -q[k];
                memcpy(prev, q, sizeof q);
                for (int k = 0; k < 4; k++) ro[f * 4 + k] = (float)q[k];
            }
            const char *paths[3] = {"translation", "rotation", "scale"};
            const void *data[3] = {tr, ro, sc};
            const char *types[3] = {"VEC3", "VEC4", "VEC3"};
            size_t sizes[3] = {12, 16, 12};
            int use[3] = {has_t, has_r, has_s};
            for (int c = 0; c < 3; c++) {
                if (!use[c]) continue;
                int vv = add_view(&doc, data[c], F * sizes[c], 0), va = add_acc(&doc, vv, 5126, F, types[c], NULL, NULL, 0);
                str_printf(&samplers, "%s{\"input\":%d,\"output\":%d,\"interpolation\":\"LINEAR\"}", ns ? "," : "", ta, va);
                str_printf(&channels, "%s{\"sampler\":%d,\"target\":{\"node\":%u,\"path\":\"%s\"}}", ns ? "," : "", ns,
                           o->node, paths[c]);
                ns++;
            }
        }
        free(tr); free(sc); free(ro); free(t);
        if (ns) {
            char name[40];
            sanitize(name, an->name, sizeof name);
            str_printf(&anim_json, "%s{\"name\":\"%s\",\"samplers\":[", nanim++ ? "," : "", name);
            str_append(&anim_json, &samplers);
            str_printf(&anim_json, "],\"channels\":[");
            str_append(&anim_json, &channels);
            str_printf(&anim_json, "]}");
        }
        free(samplers.s);
        free(channels.s);
    }

    /* Textures */
    if (nimg) {
        str_printf(&texs, ",\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728,\"wrapS\":10497,\"wrapT\":10497}],\"textures\":[");
        for (uint32_t k = 0; k < nimg; k++) str_printf(&texs, "%s{\"sampler\":0,\"source\":%u}", k ? "," : "", k);
        str_printf(&texs, "],\"images\":[");
        for (uint32_t k = 0; k < nimg; k++) {
            if (glb) {
                int iv = add_view(&doc, png[k], png_len[k], 0);
                str_printf(&texs, "%s{\"bufferView\":%d,\"mimeType\":\"image/png\"}", k ? "," : "", iv);
            } else str_printf(&texs, "%s{\"uri\":\"%s.png\"}", k ? "," : "", img_name[k]);
        }
        str_printf(&texs, "]");
    }
    bytes_align(&doc.bin);

    char bname[96];
    sanitize(bname, base, sizeof bname);
    str_printf(&json, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"nds-rom-reader\"},\"scene\":0,\"scenes\":[{\"nodes\":[");
    str_append(&json, &roots);
    str_printf(&json, "]}],\"nodes\":[");
    str_append(&json, &nodes);
    str_printf(&json, "],\"meshes\":[");
    str_append(&json, &meshes);
    str_printf(&json, "],\"materials\":[");
    str_append(&json, &mats);
    str_printf(&json, "]");
    str_append(&json, &texs);
    if (nanim) {
        str_printf(&json, ",\"animations\":[");
        str_append(&json, &anim_json);
        str_printf(&json, "]");
    }
    str_printf(&json, ",\"accessors\":[");
    str_append(&json, &doc.accs);
    str_printf(&json, "],\"bufferViews\":[");
    str_append(&json, &doc.views);
    str_printf(&json, "],\"buffers\":[{");
    if (!glb) str_printf(&json, "\"uri\":\"%s.bin\",", bname);
    str_printf(&json, "\"byteLength\":%zu}]}", doc.bin.n);
    (void)total_nodes;

    if (json.fail || doc.bin.fail || doc.views.fail || doc.accs.fail || meshes.fail || nodes.fail || mats.fail ||
        anim_json.fail || texs.fail || roots.fail) {
        free(roots.s);
        res = NDS_ERR_NOMEM;
        goto done;
    }
    free(roots.s);

    if (!glb) {
        snprintf(path, plen, "%s/%s.bin", dir, base);
        FILE *bin = fopen(path, "wb");
        if (!bin) { res = NDS_ERR_IO; goto done; }
        if (fwrite(doc.bin.d, 1, doc.bin.n, bin) != doc.bin.n) res = NDS_ERR_IO;
        if (fclose(bin)) res = NDS_ERR_IO;
        snprintf(path, plen, "%s/%s.gltf", dir, base);
        FILE *js = fopen(path, "wb");
        if (!js) { res = NDS_ERR_IO; goto done; }
        if (fwrite(json.s, 1, json.n, js) != json.n) res = NDS_ERR_IO;
        if (fclose(js)) res = NDS_ERR_IO;
    } else {
        uint32_t jpad = ((uint32_t)json.n + 3) & ~3u;
        snprintf(path, plen, "%s/%s.glb", dir, base);
        FILE *out = fopen(path, "wb");
        if (!out) { res = NDS_ERR_IO; goto done; }
        uint8_t hdr[20];
        memcpy(hdr, "glTF", 4);
        put_le32(hdr + 4, 2);
        put_le32(hdr + 8, 12 + 8 + jpad + 8 + (uint32_t)doc.bin.n);
        put_le32(hdr + 12, jpad);
        memcpy(hdr + 16, "JSON", 4);
        int ok = fwrite(hdr, 1, 20, out) == 20 && fwrite(json.s, 1, json.n, out) == json.n;
        for (uint32_t i = (uint32_t)json.n; i < jpad && ok; i++) ok = fputc(' ', out) != EOF;
        put_le32(hdr, (uint32_t)doc.bin.n);
        memcpy(hdr + 4, "BIN\0", 4);
        ok = ok && fwrite(hdr, 1, 8, out) == 8 && fwrite(doc.bin.d, 1, doc.bin.n, out) == doc.bin.n;
        if (fclose(out) || !ok) res = NDS_ERR_IO;
    }

done:
    if (groups)
        for (uint32_t g = 0; g < ngroups; g++) free(groups[g].tris);
    if (png)
        for (uint32_t k = 0; k < nmat; k++) free(png[k]);
    free(png); free(png_len); free(groups); free(group_of); free(inv); free(inv_ok); free(image);
    free(img_name); free(path);
    free(doc.bin.d); free(doc.views.s); free(doc.accs.s);
    free(meshes.s); free(mats.s); free(nodes.s); free(texs.s); free(anim_json.s); free(json.s);
    return res;
}

NdsResult nds_model_write_gltf(const NdsModel *m, const NdsTexSet *tex, const NdsJointAnimSet *anims,
                               const char *dir, const char *base) {
    return write_gltf(m, tex, anims, dir, base, 0);
}

NdsResult nds_model_write_glb(const NdsModel *m, const NdsTexSet *tex, const NdsJointAnimSet *anims,
                              const char *dir, const char *base) {
    return write_gltf(m, tex, anims, dir, base, 1);
}
