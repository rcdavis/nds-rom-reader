#ifndef NDSMODEL_H
#define NDSMODEL_H

#include "ndsgfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Textures (TEX0 block of NSBTX / NSBMD files) ---- */

typedef struct {
    char name[17];
    int format;            /* 1 A3I5, 2 4-colour, 3 16-colour, 4 256-colour, 5 4x4 compressed,
                              6 A5I3, 7 direct colour */
    uint32_t width, height;
    int color0_transparent;
    int repeat_s, repeat_t, flip_s, flip_t;
    uint32_t offset;       /* bytes into the texture (or compressed texture) data */
} NdsTexInfo;

typedef struct {
    char name[17];
    uint32_t offset;       /* bytes into the palette data */
    uint32_t size;         /* bytes, up to the next palette or the end of the data */
} NdsPalInfo;

typedef struct {
    NdsTexInfo *tex;
    uint32_t tex_count;
    NdsPalInfo *pal;
    uint32_t pal_count;
    uint8_t *block;        /* private copy of the TEX0 block */
    size_t block_size;
    uint32_t tex_data, comp_data, comp_info, pal_data; /* offsets into block */
} NdsTexSet;

/* Accepts a whole BTX0/BMD0 file. Returns NDS_ERR_NOTFOUND if it has no TEX0 block. */
NdsResult nds_texset_parse(const uint8_t *data, size_t size, NdsTexSet *out);
void nds_texset_free(NdsTexSet *ts);

/* Index of the named texture or palette, or -1. */
int nds_texset_find_tex(const NdsTexSet *ts, const char *name);
int nds_texset_find_pal(const NdsTexSet *ts, const char *name);

/* The palette conventionally paired with a texture ("<name>_pl"), or -1. */
int nds_texset_default_pal(const NdsTexSet *ts, uint32_t tex);

/* pal < 0 picks the default palette. */
NdsResult nds_texset_decode(const NdsTexSet *ts, uint32_t tex, int pal, NdsImage *out);

/* ---- Models (MDL0 block of NSBMD files) ---- */

typedef struct {
    float pos[3];
    float uv[2];
    float color[3];
} NdsVertex;

typedef struct {
    char name[17];
    char tex_name[17];     /* empty if untextured */
    char pal_name[17];
    float diffuse[3];
    float alpha;           /* 0..1 */
    int cull_back, cull_front;
    uint32_t width, height; /* of the texture, in pixels */
} NdsMaterial;

typedef struct {
    char name[17];
    NdsMaterial *materials;
    uint32_t material_count;
    uint32_t tri_count;
    NdsVertex *verts;          /* 3 per triangle, already in model space */
    uint16_t *tri_material;    /* per triangle */
    float bmin[3], bmax[3];
} NdsModel;

typedef struct {
    NdsModel *models;
    uint32_t model_count;
    NdsTexSet tex;             /* textures embedded in the file, if has_tex */
    int has_tex;
} NdsModelFile;

NdsResult nds_modelfile_parse(const uint8_t *data, size_t size, NdsModelFile *out);
void nds_modelfile_free(NdsModelFile *mf);

/* Writes <dir>/<base>.obj, <base>.mtl and one PNG per texture used. */
NdsResult nds_model_write_obj(const NdsModel *m, const NdsTexSet *tex, const char *dir,
                              const char *base);

/* Writes <dir>/<base>.gltf (glTF 2.0), <base>.bin and one PNG per texture used.
   Returns NDS_ERR_NOTFOUND if the model has no triangles. */
NdsResult nds_model_write_gltf(const NdsModel *m, const NdsTexSet *tex, const char *dir,
                               const char *base);

/* Writes a self-contained <dir>/<base>.glb with the textures embedded. */
NdsResult nds_model_write_glb(const NdsModel *m, const NdsTexSet *tex, const char *dir,
                              const char *base);

/* Software preview (orthographic, nearest-neighbour textures, no lighting). */
NdsResult nds_model_render(const NdsModel *m, const NdsTexSet *tex, float yaw_deg,
                           float pitch_deg, uint32_t width, uint32_t height, NdsImage *out);

#ifdef __cplusplus
}
#endif

#endif
