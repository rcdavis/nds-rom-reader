#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ndsrom/ndsmodel.h"

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

static void put_u32(uint8_t **p, uint32_t v) {
    (*p)[0] = (uint8_t)v; (*p)[1] = (uint8_t)(v >> 8); (*p)[2] = (uint8_t)(v >> 16); (*p)[3] = (uint8_t)(v >> 24);
    *p += 4;
}

static void put_f32(uint8_t **p, float v) {
    uint32_t u;
    memcpy(&u, &v, 4);
    put_u32(p, u);
}

/* Reads a whole file; used to embed PNGs in a GLB. */
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

static NdsResult write_gltf(const NdsModel *m, const NdsTexSet *tex, const char *dir,
                            const char *base, int glb) {
    if (!m || !dir || !base) return NDS_ERR_ARG;
    if (m->tri_count == 0) return NDS_ERR_NOTFOUND;

    size_t plen = strlen(dir) + strlen(base) + 64;
    char *path = malloc(plen);
    uint32_t nmat = m->material_count;
    int *image = calloc(nmat ? nmat : 1, sizeof *image);   /* per material: image index or -1 */
    uint32_t *tri_n = calloc(nmat ? nmat : 1, sizeof *tri_n);
    char (*img_name)[40] = calloc(nmat ? nmat : 1, sizeof *img_name);
    if (!path || !image || !tri_n || !img_name) {
        free(path); free(image); free(tri_n); free(img_name);
        return NDS_ERR_NOMEM;
    }
    NdsResult res = NDS_OK;
    FILE *js = NULL;
    uint32_t nimg = 0;
    uint8_t *geo = NULL, **png = calloc(nmat ? nmat : 1, sizeof *png);
    uint32_t *png_len = calloc(nmat ? nmat : 1, sizeof *png_len);
    if (!png || !png_len) { free(png); free(png_len); free(path); free(image); free(tri_n); free(img_name); return NDS_ERR_NOMEM; }

    for (uint32_t t = 0; t < m->tri_count; t++)
        if (m->tri_material[t] < nmat) tri_n[m->tri_material[t]]++;

    /* One PNG per distinct texture used by a material that has triangles. */
    for (uint32_t i = 0; i < nmat; i++) {
        image[i] = -1;
        const NdsMaterial *mt = &m->materials[i];
        int ti = mt->tex_name[0] && tri_n[i] ? nds_texset_find_tex(tex, mt->tex_name) : -1;
        if (ti < 0) continue;
        char name[40];
        sanitize(name, mt->tex_name, sizeof name);
        for (uint32_t k = 0; k < nimg; k++)
            if (!strcmp(img_name[k], name)) image[i] = (int)k;
        if (image[i] >= 0) continue;
        NdsImage img;
        int pi = mt->pal_name[0] ? nds_texset_find_pal(tex, mt->pal_name) : -1;
        if (nds_texset_decode(tex, (uint32_t)ti, pi, &img) != NDS_OK) continue;
        /* A GLB embeds the PNG, so it goes through a temporary file. */
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

    snprintf(path, plen, "%s/%s.%s", dir, base, glb ? "json.tmp" : "gltf");
    js = fopen(path, "w");
    if (!js) { res = NDS_ERR_IO; goto done; }

    uint32_t nv = m->tri_count * 3;
    float mn[3], mx[3];
    for (uint32_t i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++) {
            float v = m->verts[i].pos[k];
            if (i == 0 || v < mn[k]) mn[k] = v;
            if (i == 0 || v > mx[k]) mx[k] = v;
        }

    /* Buffer layout: positions, uvs, then one index list per material. */
    uint32_t geo_size = nv * 20 + m->tri_count * 12, bin_size = geo_size;
    if (glb)
        for (uint32_t k = 0; k < nimg; k++) bin_size += (png_len[k] + 3) & ~3u;
    geo = calloc(bin_size, 1);
    if (!geo) { res = NDS_ERR_NOMEM; goto done; }
    uint8_t *w = geo;
    for (uint32_t i = 0; i < nv; i++)
        for (int k = 0; k < 3; k++) put_f32(&w, m->verts[i].pos[k]);
    for (uint32_t i = 0; i < nv; i++) {
        put_f32(&w, m->verts[i].uv[0]);
        put_f32(&w, m->verts[i].uv[1]);
    }
    for (uint32_t mi = 0; mi < nmat; mi++)
        for (uint32_t t = 0; t < m->tri_count; t++)
            if (m->tri_material[t] == mi)
                for (uint32_t k = 0; k < 3; k++) put_u32(&w, t * 3 + (k ? 3 - k : 0)); /* NDS front faces are clockwise */
    uint32_t img_off[64];
    if (glb) {
        uint32_t o = geo_size;
        for (uint32_t k = 0; k < nimg && k < 64; k++) {
            img_off[k] = o;
            memcpy(geo + o, png[k], png_len[k]);
            o += (png_len[k] + 3) & ~3u;
        }
        if (nimg > 64) { res = NDS_ERR_FORMAT; goto done; }
    }

    char bname[96];
    sanitize(bname, base, sizeof bname);
    fprintf(js, "{\"asset\":{\"version\":\"2.0\",\"generator\":\"nds-rom-reader\"},\"scene\":0,"
                "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"name\":\"%s\"}],"
                "\"buffers\":[{", bname);
    if (!glb) fprintf(js, "\"uri\":\"%s.bin\",", bname);
    fprintf(js, "\"byteLength\":%u}],", bin_size);

    fprintf(js, "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u,\"target\":34962},"
                "{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34962}",
            nv * 12, nv * 12, nv * 8);
    uint32_t off = nv * 20;
    for (uint32_t mi = 0; mi < nmat; mi++) {
        if (!tri_n[mi]) continue;
        fprintf(js, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"target\":34963}", off, tri_n[mi] * 12);
        off += tri_n[mi] * 12;
    }

    if (glb)
        for (uint32_t k = 0; k < nimg; k++)
            fprintf(js, ",{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}", img_off[k], png_len[k]);

    fprintf(js, "],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":%u,\"type\":\"VEC3\","
                "\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]},"
                "{\"bufferView\":1,\"componentType\":5126,\"count\":%u,\"type\":\"VEC2\"}",
            nv, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], nv);
    uint32_t view = 2;
    for (uint32_t mi = 0; mi < nmat; mi++)
        if (tri_n[mi])
            fprintf(js, ",{\"bufferView\":%u,\"componentType\":5125,\"count\":%u,\"type\":\"SCALAR\"}", view++,
                    tri_n[mi] * 3);
    uint32_t first_img_view = view;

    fprintf(js, "],\"meshes\":[{\"name\":\"%s\",\"primitives\":[", bname);
    uint32_t acc = 2;
    int first = 1;
    for (uint32_t mi = 0; mi < nmat; mi++) {
        if (!tri_n[mi]) continue;
        fprintf(js, "%s{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":%u,\"material\":%u}",
                first ? "" : ",", acc++, mi);
        first = 0;
    }
    fputs("]}],\"materials\":[", js);
    for (uint32_t mi = 0; mi < nmat; mi++) {
        const NdsMaterial *mt = &m->materials[mi];
        char name[40];
        sanitize(name, mt->name, sizeof name);
        float a = mt->alpha > 0 ? mt->alpha : 1.0f;
        int tx = image[mi] >= 0;
        fprintf(js, "%s{\"name\":\"%s\",\"doubleSided\":%s,\"pbrMetallicRoughness\":{"
                    "\"baseColorFactor\":[%.4f,%.4f,%.4f,%.4f],\"metallicFactor\":0,\"roughnessFactor\":1",
                mi ? "," : "", name, (!mt->cull_back && !mt->cull_front) ? "true" : "false",
                tx ? 1.0f : mt->diffuse[0], tx ? 1.0f : mt->diffuse[1], tx ? 1.0f : mt->diffuse[2], a);
        if (tx) fprintf(js, ",\"baseColorTexture\":{\"index\":%d}", image[mi]);
        fputc('}', js);
        if (tx || a < 1.0f) fputs(",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5", js);
        fputc('}', js);
    }
    fputc(']', js);

    if (nimg) {
        fputs(",\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728,\"wrapS\":10497,\"wrapT\":10497}],"
              "\"textures\":[", js);
        for (uint32_t k = 0; k < nimg; k++) fprintf(js, "%s{\"sampler\":0,\"source\":%u}", k ? "," : "", k);
        fputs("],\"images\":[", js);
        for (uint32_t k = 0; k < nimg; k++) {
            if (glb) fprintf(js, "%s{\"bufferView\":%u,\"mimeType\":\"image/png\"}", k ? "," : "", first_img_view + k);
            else fprintf(js, "%s{\"uri\":\"%s.png\"}", k ? "," : "", img_name[k]);
        }
        fputc(']', js);
    }
    fputs("}\n", js);

    if (fclose(js)) res = NDS_ERR_IO;
    js = NULL;
    if (res != NDS_OK) goto done;

    if (!glb) {
        snprintf(path, plen, "%s/%s.bin", dir, base);
        FILE *bin = fopen(path, "wb");
        if (!bin) { res = NDS_ERR_IO; goto done; }
        if (fwrite(geo, 1, bin_size, bin) != bin_size) res = NDS_ERR_IO;
        if (fclose(bin)) res = NDS_ERR_IO;
    } else {
        char *tmp = malloc(plen);
        if (!tmp) { res = NDS_ERR_NOMEM; goto done; }
        snprintf(tmp, plen, "%s/%s.json.tmp", dir, base);
        uint32_t jn;
        uint8_t *json = slurp(tmp, &jn);
        remove(tmp);
        free(tmp);
        if (!json) { res = NDS_ERR_IO; goto done; }
        uint32_t jpad = (jn + 3) & ~3u;
        snprintf(path, plen, "%s/%s.glb", dir, base);
        FILE *out = fopen(path, "wb");
        if (!out) { free(json); res = NDS_ERR_IO; goto done; }
        uint8_t hdr[20], *h = hdr;
        put_u32(&h, 0x46546C67);                     /* "glTF" */
        put_u32(&h, 2);
        put_u32(&h, 12 + 8 + jpad + 8 + bin_size);
        put_u32(&h, jpad);
        put_u32(&h, 0x4E4F534A);                     /* "JSON" */
        int ok = fwrite(hdr, 1, 20, out) == 20 && fwrite(json, 1, jn, out) == jn;
        for (uint32_t i = jn; i < jpad && ok; i++) ok = fputc(' ', out) != EOF;
        h = hdr;
        put_u32(&h, bin_size);
        put_u32(&h, 0x004E4942);                     /* "BIN\0" */
        ok = ok && fwrite(hdr, 1, 8, out) == 8 && fwrite(geo, 1, bin_size, out) == bin_size;
        if (fclose(out) || !ok) res = NDS_ERR_IO;
        free(json);
    }

done:
    if (js) { fclose(js); snprintf(path, plen, "%s/%s.json.tmp", dir, base); remove(path); }
    for (uint32_t k = 0; k < nmat; k++) free(png[k]);
    free(png); free(png_len); free(geo);
    free(path); free(image); free(tri_n); free(img_name);
    return res;
}

NdsResult nds_model_write_gltf(const NdsModel *m, const NdsTexSet *tex, const char *dir,
                               const char *base) {
    return write_gltf(m, tex, dir, base, 0);
}

NdsResult nds_model_write_glb(const NdsModel *m, const NdsTexSet *tex, const char *dir,
                              const char *base) {
    return write_gltf(m, tex, dir, base, 1);
}
