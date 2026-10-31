#ifndef NDSGFX_H
#define NDSGFX_H

#include "ndsrom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compression (BIOS-style: 0x10 LZ77, 0x11 LZ77 ext, 0x30 RLE) ---- */

int nds_is_compressed(const uint8_t *data, size_t size);

/* Decompresses a buffer carrying a compression header. Returns NDS_ERR_FORMAT
   if the data is not (validly) compressed. Free *out with free(). */
NdsResult nds_decompress(const uint8_t *in, size_t in_size, uint8_t **out,
                         uint32_t *out_size);

/* Loads a file and transparently decompresses it if it carries a valid
   compression header. was_compressed may be NULL. */
NdsResult nds_load_file_unpacked(const NdsRom *rom, const NdsFile *file,
                                 uint8_t **data, uint32_t *size,
                                 int *was_compressed);

/* ---- Image formats ---- */

typedef enum {
    NDS_KIND_UNKNOWN = 0,
    NDS_KIND_NCLR, /* palette */
    NDS_KIND_NCGR, /* tile graphics */
    NDS_KIND_NSCR, /* screen / tile map */
    NDS_KIND_NCER, /* cell bank (not decoded) */
    NDS_KIND_NANR  /* animation bank (not decoded) */
} NdsKind;

NdsKind nds_detect_kind(const uint8_t *data, size_t size);
const char *nds_kind_name(NdsKind kind);

typedef struct {
    uint32_t width, height;
    uint8_t *rgba; /* width*height*4 */
} NdsImage;

typedef struct {
    uint32_t count;
    int bpp;         /* 4 or 8 */
    uint8_t *rgba;   /* count*4 */
} NdsPalette;

typedef struct {
    uint32_t width_tiles, height_tiles; /* 0 if the file doesn't say */
    int bpp;                            /* 4 or 8 */
    int tiled;                          /* 0 = linear bitmap */
    uint8_t *data;
    uint32_t size;
} NdsTileSet;

typedef struct {
    uint32_t width, height; /* pixels */
    uint32_t count;
    uint16_t *entries;      /* tile | hflip<<10 | vflip<<11 | bank<<12 */
} NdsScreen;

NdsResult nds_palette_parse(const uint8_t *data, size_t size, NdsPalette *out);
NdsResult nds_tileset_parse(const uint8_t *data, size_t size, NdsTileSet *out);
NdsResult nds_screen_parse(const uint8_t *data, size_t size, NdsScreen *out);
void nds_palette_free(NdsPalette *p);
void nds_tileset_free(NdsTileSet *t);
void nds_screen_free(NdsScreen *s);
void nds_image_free(NdsImage *img);

/* Renders all tiles as a sheet. width_tiles overrides the file's width when
   nonzero (default 32 if the file has none). pal may be NULL (grayscale).
   Palette index 0 is transparent. */
NdsResult nds_render_tiles(const NdsTileSet *ts, const NdsPalette *pal,
                           unsigned bank, uint32_t width_tiles, NdsImage *out);

/* Renders a tile map using the tileset and palette. */
NdsResult nds_render_screen(const NdsTileSet *ts, const NdsPalette *pal,
                            const NdsScreen *scr, NdsImage *out);

NdsResult nds_write_png(const char *path, const NdsImage *img);

#ifdef __cplusplus
}
#endif

#endif
