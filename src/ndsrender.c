#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ndsrom/ndsmodel.h"

typedef struct {
    float x, y, z, u, v, r, g, b;
} RV;

static float edge(const RV *a, const RV *b, float px, float py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

NdsResult nds_model_render(const NdsModel *m, const NdsTexSet *tex, float yaw_deg, float pitch_deg,
                           uint32_t width, uint32_t height, NdsImage *out) {
    if (!m || !out || width == 0 || height == 0 || width > 8192 || height > 8192) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);

    size_t npx = (size_t)width * height;
    uint8_t *rgba = calloc(npx, 4);
    float *zbuf = malloc(npx * sizeof *zbuf);
    NdsImage *imgs = calloc(m->material_count ? m->material_count : 1, sizeof *imgs);
    if (!rgba || !zbuf || !imgs) {
        free(rgba); free(zbuf); free(imgs);
        return NDS_ERR_NOMEM;
    }
    for (size_t i = 0; i < npx; i++) zbuf[i] = 1e30f;
    for (size_t i = 0; i < npx; i++) { rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 40; rgba[i * 4 + 3] = 255; }

    for (uint32_t i = 0; i < m->material_count; i++) {
        const NdsMaterial *mt = &m->materials[i];
        int ti = mt->tex_name[0] ? nds_texset_find_tex(tex, mt->tex_name) : -1;
        if (ti < 0) continue;
        int pi = mt->pal_name[0] ? nds_texset_find_pal(tex, mt->pal_name) : -1;
        if (nds_texset_decode(tex, (uint32_t)ti, pi, &imgs[i]) != NDS_OK) memset(&imgs[i], 0, sizeof imgs[i]);
    }

    /* Y is up in the model; rotate around Y by yaw, then around X by pitch. */
    float yaw = yaw_deg * 3.14159265f / 180, pitch = pitch_deg * 3.14159265f / 180;
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    float ctr[3], ext = 0;
    for (int k = 0; k < 3; k++) {
        ctr[k] = (m->bmin[k] + m->bmax[k]) / 2;
        float e = m->bmax[k] - m->bmin[k];
        if (e > ext) ext = e;
    }
    if (ext <= 0) ext = 1;
    float side = width < height ? width : height;
    float scale = side * 0.9f / (ext * 1.75f);

    for (uint32_t t = 0; t < m->tri_count; t++) {
        const NdsMaterial *mt = t < m->tri_count && m->tri_material[t] < m->material_count
                                    ? &m->materials[m->tri_material[t]] : NULL;
        const NdsImage *img = mt && imgs[m->tri_material[t]].rgba ? &imgs[m->tri_material[t]] : NULL;
        RV v[3];
        for (int i = 0; i < 3; i++) {
            const NdsVertex *s = &m->verts[t * 3 + i];
            float x = s->pos[0] - ctr[0], y = s->pos[1] - ctr[1], z = s->pos[2] - ctr[2];
            float x1 = x * cy + z * sy, z1 = -x * sy + z * cy;
            float y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
            v[i] = (RV){width / 2.0f + x1 * scale, height / 2.0f - y2 * scale, z2, s->uv[0], s->uv[1],
                        s->color[0], s->color[1], s->color[2]};
        }
        float area = edge(&v[0], &v[1], v[2].x, v[2].y);
        if (area == 0) continue;
        /* Screen y is flipped, so counter-clockwise model triangles have negative area here. */
        int front = area > 0;
        if (mt && ((front && !mt->cull_front) || (!front && !mt->cull_back))) {
            if (!(mt->cull_front && mt->cull_back)) { /* both culled keeps nothing; neither culls nothing */ }
        }
        if (mt && ((front && mt->cull_front) || (!front && mt->cull_back))) continue;

        float minx = fminf(v[0].x, fminf(v[1].x, v[2].x)), maxx = fmaxf(v[0].x, fmaxf(v[1].x, v[2].x));
        float miny = fminf(v[0].y, fminf(v[1].y, v[2].y)), maxy = fmaxf(v[0].y, fmaxf(v[1].y, v[2].y));
        int x0 = (int)fmaxf(0, floorf(minx)), x1 = (int)fminf((float)width - 1, ceilf(maxx));
        int y0 = (int)fmaxf(0, floorf(miny)), y1 = (int)fminf((float)height - 1, ceilf(maxy));
        for (int py = y0; py <= y1; py++)
            for (int px = x0; px <= x1; px++) {
                float fx = px + 0.5f, fy = py + 0.5f;
                float w0 = edge(&v[1], &v[2], fx, fy) / area, w1 = edge(&v[2], &v[0], fx, fy) / area,
                      w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * v[0].z + w1 * v[1].z + w2 * v[2].z;
                size_t idx = (size_t)py * width + px;
                if (z >= zbuf[idx]) continue;
                float r = w0 * v[0].r + w1 * v[1].r + w2 * v[2].r;
                float g = w0 * v[0].g + w1 * v[1].g + w2 * v[2].g;
                float b = w0 * v[0].b + w1 * v[1].b + w2 * v[2].b;
                float cr = mt ? mt->diffuse[0] : 1, cg = mt ? mt->diffuse[1] : 1, cb = mt ? mt->diffuse[2] : 1;
                if (img) {
                    float u = w0 * v[0].u + w1 * v[1].u + w2 * v[2].u;
                    float vv = w0 * v[0].v + w1 * v[1].v + w2 * v[2].v;
                    u -= floorf(u);
                    vv -= floorf(vv);
                    int tx = (int)(u * img->width), ty = (int)(vv * img->height);
                    if (tx >= (int)img->width) tx = (int)img->width - 1;
                    if (ty >= (int)img->height) ty = (int)img->height - 1;
                    const uint8_t *tp = img->rgba + ((size_t)ty * img->width + tx) * 4;
                    if (tp[3] < 128) continue;
                    cr = tp[0] / 255.0f; cg = tp[1] / 255.0f; cb = tp[2] / 255.0f;
                    r = g = b = 1;
                } else {
                    r = r * cr; g = g * cg; b = b * cb;
                    cr = r; cg = g; cb = b;
                    r = g = b = 1;
                }
                zbuf[idx] = z;
                rgba[idx * 4] = (uint8_t)(fminf(cr * r, 1) * 255);
                rgba[idx * 4 + 1] = (uint8_t)(fminf(cg * g, 1) * 255);
                rgba[idx * 4 + 2] = (uint8_t)(fminf(cb * b, 1) * 255);
            }
    }

    for (uint32_t i = 0; i < m->material_count; i++) free(imgs[i].rgba);
    free(imgs);
    free(zbuf);
    out->width = width;
    out->height = height;
    out->rgba = rgba;
    return NDS_OK;
}
