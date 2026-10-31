#include "ndsrom/ndsgfx.h"
#include "ndsrom/ndssdat.h"
#include "ndsrom/ndstext.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

static void usage(const char *p) {
    fprintf(stderr,
            "Usage: %s <rom.nds> [command]\n"
            "Commands:\n"
            "  info                 show ROM header information (default)\n"
            "  list                 list asset files in the ROM filesystem\n"
            "  cat <path>           write a file's contents to stdout\n"
            "  hex <path> [bytes]   hex dump the start of a file (default 256)\n"
            "  extract <path> <out> extract one file\n"
            "  extract-all <dir>    extract every file, preserving directories\n"
            "  unpack <path> <out>  extract a file, decompressing it if needed\n"
            "  png <path> <out.png> [--pal <path>] [--scr <path>] [--cer <path>] [--width <tiles>] [--bank <n>]\n"
            "                       render an NCGR graphic; --scr applies a tile map, --cer\n"
            "                       assembles sprites from an NCER cell bank. A palette is\n"
            "                       found from same-named files if omitted\n"
            "  png-all <dir>        render every NCGR graphic in the ROM to PNG files\n"
            "  anim <path> <prefix> [--pal <path>] [--cer <path>] [--anr <path>]\n"
            "                       write each NANR sequence of a sprite as <prefix>_<n>.gif;\n"
            "                       palette, cells and animations are found from same-named\n"
            "                       files if omitted\n"
            "  anim-all <dir>       write animations for every sprite in the ROM\n"
            "  text <path>          print the messages in an .sdb message bank\n"
            "  text-all <dir>       write every .sdb message bank to <dir>/<path>.txt\n"
            "  sdat <path>          list the contents of an SDAT sound archive\n"
            "  audio <path> <dir>   extract an SDAT: sequences/banks as files, waves and\n"
            "                       streams as .wav\n",
            p);
}

static void cmd_info(const NdsRom *rom) {
    const NdsHeader *h = nds_header(rom);
    printf("Title:       %s\nGame code:   %s\nMaker code:  %s\nVersion:     %u\n",
           h->title, h->game_code, h->maker_code, h->version);
    printf("ARM9:        offset 0x%X, size %u, entry 0x%X, ram 0x%X\n", h->arm9_offset,
           h->arm9_size, h->arm9_entry, h->arm9_ram);
    printf("ARM7:        offset 0x%X, size %u, entry 0x%X, ram 0x%X\n", h->arm7_offset,
           h->arm7_size, h->arm7_entry, h->arm7_ram);
    printf("FNT:         offset 0x%X, size %u\nFAT:         offset 0x%X, size %u\n",
           h->fnt_offset, h->fnt_size, h->fat_offset, h->fat_size);
    printf("ROM size:    %u\nHeader CRC:  0x%04X (%s)\nFiles:       %zu\n", h->total_size,
           h->header_crc, h->header_crc_ok ? "ok" : "mismatch", nds_file_count(rom));
}

static void cmd_list(const NdsRom *rom) {
    for (size_t i = 0; i < nds_file_count(rom); i++) {
        const NdsFile *f = nds_file_at(rom, i);
        printf("%5u  0x%08X  %10u  %s\n", f->id, f->offset, f->size, f->path);
    }
}

static int cmd_hex(const NdsRom *rom, const NdsFile *f, uint32_t n) {
    if (n > f->size) n = f->size;
    uint8_t *buf = malloc(n ? n : 1);
    if (!buf || nds_read_file(rom, f, 0, buf, n) < 0) { free(buf); return 1; }
    for (uint32_t i = 0; i < n; i += 16) {
        printf("%08X  ", i);
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < n) printf("%02X ", buf[i + j]);
            else printf("   ");
        }
        printf(" |");
        for (uint32_t j = 0; j < 16 && i + j < n; j++)
            putchar(buf[i + j] >= 32 && buf[i + j] < 127 ? buf[i + j] : '.');
        printf("|\n");
    }
    free(buf);
    return 0;
}

static int cmd_cat(const NdsRom *rom, const NdsFile *f) {
    uint8_t buf[65536];
    for (uint32_t done = 0; done < f->size;) {
        long n = nds_read_file(rom, f, done, buf, sizeof buf);
        if (n <= 0 || fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) return 1;
        done += (uint32_t)n;
    }
    return 0;
}

static int mkdirs(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (MKDIR(path) != 0 && errno != EEXIST) { *p = '/'; return -1; }
            *p = '/';
        }
    }
    return 0;
}

static int cmd_extract_all(const NdsRom *rom, const char *dir) {
    size_t dl = strlen(dir);
    int rc = 0;
    for (size_t i = 0; i < nds_file_count(rom); i++) {
        const NdsFile *f = nds_file_at(rom, i);
        char *out = malloc(dl + strlen(f->path) + 2);
        if (!out) return 1;
        sprintf(out, "%s/%s", dir, f->path);
        if (mkdirs(out) != 0 || nds_extract_file(rom, f, out) != NDS_OK) {
            fprintf(stderr, "failed to extract %s\n", out);
            rc = 1;
        }
        free(out);
    }
    if (!rc) printf("Extracted %zu files to %s\n", nds_file_count(rom), dir);
    return rc;
}


static NdsResult load_unpacked(const NdsRom *rom, const char *path, uint8_t **d, uint32_t *n) {
    const NdsFile *f = nds_find_file(rom, path);
    if (!f) return NDS_ERR_NOTFOUND;
    return nds_load_file_unpacked(rom, f, d, n, NULL);
}

static int write_file(const char *path, const uint8_t *d, uint32_t n) {
    FILE *o = fopen(path, "wb");
    if (!o) return 1;
    int ok = fwrite(d, 1, n, o) == n;
    return (fclose(o) == 0 && ok) ? 0 : 1;
}

static int cmd_unpack(const NdsRom *rom, const char *path, const char *out) {
    uint8_t *d;
    uint32_t n;
    NdsResult r = load_unpacked(rom, path, &d, &n);
    if (r != NDS_OK) { fprintf(stderr, "%s: %s\n", path, nds_strerror(r)); return 1; }
    int rc = write_file(out, d, n);
    if (rc) fprintf(stderr, "cannot write %s\n", out);
    free(d);
    return rc;
}

typedef struct {
    const char *pal, *scr, *cer;
    uint32_t width, bank;
} RenderOpts;

/* Renders graphic `gfx` (and optional palette/screen paths) into out_png. */
static int render_png(const NdsRom *rom, const char *gfx, const RenderOpts *o,
                      const char *out_png) {
    uint8_t *d = NULL;
    uint32_t n;
    NdsTileSet ts = {0};
    NdsPalette pal = {0};
    NdsScreen scr = {0};
    NdsCellBank cb = {0};
    NdsImage img = {0};
    int rc = 1;
    NdsResult r = load_unpacked(rom, gfx, &d, &n);
    if (r == NDS_OK) r = nds_tileset_parse(d, n, &ts);
    free(d);
    d = NULL;
    if (r != NDS_OK) { fprintf(stderr, "%s: not a valid NCGR (%s)\n", gfx, nds_strerror(r)); goto done; }

    if (o->pal) {
        r = load_unpacked(rom, o->pal, &d, &n);
        if (r == NDS_OK) r = nds_palette_parse(d, n, &pal);
        free(d);
        d = NULL;
        if (r != NDS_OK) { fprintf(stderr, "%s: bad palette (%s)\n", o->pal, nds_strerror(r)); goto done; }
    }
    if (o->cer) {
        r = load_unpacked(rom, o->cer, &d, &n);
        if (r == NDS_OK) r = nds_cellbank_parse(d, n, &cb);
        free(d);
        d = NULL;
        if (r != NDS_OK) { fprintf(stderr, "%s: bad cell bank (%s)\n", o->cer, nds_strerror(r)); goto done; }
        r = nds_render_cell_atlas(&cb, &ts, o->pal ? &pal : NULL, &img);
    } else if (o->scr) {
        r = load_unpacked(rom, o->scr, &d, &n);
        if (r == NDS_OK) r = nds_screen_parse(d, n, &scr);
        free(d);
        d = NULL;
        if (r != NDS_OK) { fprintf(stderr, "%s: bad screen (%s)\n", o->scr, nds_strerror(r)); goto done; }
        r = nds_render_screen(&ts, o->pal ? &pal : NULL, &scr, &img);
    } else {
        r = nds_render_tiles(&ts, o->pal ? &pal : NULL, o->bank, o->width, &img);
    }
    if (r == NDS_OK) r = nds_write_png(out_png, &img);
    if (r != NDS_OK) { fprintf(stderr, "%s: %s\n", out_png, nds_strerror(r)); goto done; }
    rc = 0;
done:
    nds_tileset_free(&ts);
    nds_palette_free(&pal);
    nds_screen_free(&scr);
    nds_cellbank_free(&cb);
    nds_image_free(&img);
    return rc;
}

/* Name without extension; extensionless names drop their last character
   (the game names sets like "0c"/"0p"/"0l"). */
static size_t stem_len(const char *name) {
    const char *dot = strrchr(name, '.');
    if (dot) return (size_t)(dot - name);
    size_t n = strlen(name);
    return n > 1 ? n - 1 : n;
}

static size_t dir_len(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? (size_t)(s - path) + 1 : 0;
}

static int same_set(const NdsFile *a, const NdsFile *b) {
    size_t da = dir_len(a->path), db = dir_len(b->path);
    size_t sa = stem_len(a->name), sb = stem_len(b->name);
    return da == db && sa == sb && !strncmp(a->path, b->path, da) &&
           !strncmp(a->name, b->name, sa);
}

#define KIND_SCAN_MAX (4u * 1024u * 1024u)

static NdsKind kind_of(const NdsRom *rom, const NdsFile *f) {
    uint8_t *d;
    uint32_t n;
    if (f->size < 16 || f->size > KIND_SCAN_MAX) return NDS_KIND_UNKNOWN;
    if (nds_load_file_unpacked(rom, f, &d, &n, NULL) != NDS_OK) return NDS_KIND_UNKNOWN;
    NdsKind k = nds_detect_kind(d, n);
    free(d);
    return k;
}

static int cmd_png_all(const NdsRom *rom, const char *dir) {
    size_t count = nds_file_count(rom);
    NdsKind *kinds = calloc(count, sizeof *kinds);
    if (!kinds) return 1;

    for (size_t i = 0; i < count; i++) kinds[i] = kind_of(rom, nds_file_at(rom, i));

    size_t done = 0, failed = 0;
    for (size_t i = 0; i < count; i++) {
        if (kinds[i] != NDS_KIND_NCGR) continue;
        const NdsFile *g = nds_file_at(rom, i);
        RenderOpts o = {0};
        for (size_t j = 0; j < count; j++) {
            const NdsFile *c = nds_file_at(rom, j);
            if (!same_set(g, c)) continue;
            if (kinds[j] == NDS_KIND_NCLR && !o.pal) o.pal = c->path;
            if (kinds[j] == NDS_KIND_NSCR && !o.scr) o.scr = c->path;
            if (kinds[j] == NDS_KIND_NCER && !o.cer) o.cer = c->path;
        }

        char *out = malloc(strlen(dir) + strlen(g->path) + 32);
        if (!out) break;
        RenderOpts sheet = {o.pal, NULL, NULL, 0, 0};
        sprintf(out, "%s/%s.png", dir, g->path);
        int bad = mkdirs(out) != 0 || render_png(rom, g->path, &sheet, out);
        if (!bad && o.scr) {
            sprintf(out, "%s/%s_screen.png", dir, g->path);
            RenderOpts m = {o.pal, o.scr, NULL, 0, 0};
            bad = render_png(rom, g->path, &m, out);
        }
        if (!bad && o.cer) {
            sprintf(out, "%s/%s_cells.png", dir, g->path);
            RenderOpts cells = {o.pal, NULL, o.cer, 0, 0};
            /* Banks that contain no visible cells are not an error. */
            render_png(rom, g->path, &cells, out);
        }
        free(out);
        if (bad) failed++; else done++;
    }
    free(kinds);
    printf("Rendered %zu graphics to %s (%zu failed)\n", done, dir, failed);
    return failed ? 1 : 0;
}

typedef struct {
    const char *pal, *cer, *anr;
} AnimOpts;

/* Looks up the palette/cell/animation files that belong with `g`. */
static void find_anim_set(const NdsRom *rom, const NdsFile *g, const NdsKind *kinds,
                          AnimOpts *o) {
    for (size_t j = 0; j < nds_file_count(rom); j++) {
        const NdsFile *c = nds_file_at(rom, j);
        if (c == g || !same_set(g, c)) continue;
        NdsKind k = kinds ? kinds[j] : kind_of(rom, c);
        if (k == NDS_KIND_NCLR && !o->pal) o->pal = c->path;
        if (k == NDS_KIND_NCER && !o->cer) o->cer = c->path;
        if (k == NDS_KIND_NANR && !o->anr) o->anr = c->path;
    }
}

/* Writes one GIF per sequence. Returns the number written, or -1 on a load error. */
static int write_anims(const NdsRom *rom, const char *gfx, const AnimOpts *o,
                       const char *prefix) {
    uint8_t *d = NULL;
    uint32_t n;
    NdsTileSet ts = {0};
    NdsPalette pal = {0};
    NdsCellBank cb = {0};
    NdsAnimBank ab = {0};
    int written = -1;

    NdsResult r = load_unpacked(rom, gfx, &d, &n);
    if (r == NDS_OK) r = nds_tileset_parse(d, n, &ts);
    free(d);
    d = NULL;
    if (r != NDS_OK) { fprintf(stderr, "%s: not a valid NCGR (%s)\n", gfx, nds_strerror(r)); goto done; }

    if (o->pal) {
        r = load_unpacked(rom, o->pal, &d, &n);
        if (r == NDS_OK) r = nds_palette_parse(d, n, &pal);
        free(d);
        d = NULL;
        if (r != NDS_OK) { fprintf(stderr, "%s: bad palette (%s)\n", o->pal, nds_strerror(r)); goto done; }
    }
    if (!o->cer || !o->anr) { fprintf(stderr, "%s: no cell/animation files found\n", gfx); goto done; }
    r = load_unpacked(rom, o->cer, &d, &n);
    if (r == NDS_OK) r = nds_cellbank_parse(d, n, &cb);
    free(d);
    d = NULL;
    if (r != NDS_OK) { fprintf(stderr, "%s: bad cell bank (%s)\n", o->cer, nds_strerror(r)); goto done; }
    r = load_unpacked(rom, o->anr, &d, &n);
    if (r == NDS_OK) r = nds_animbank_parse(d, n, &ab);
    free(d);
    d = NULL;
    if (r != NDS_OK) { fprintf(stderr, "%s: bad animation bank (%s)\n", o->anr, nds_strerror(r)); goto done; }

    written = 0;
    char *out = malloc(strlen(prefix) + 16);
    if (!out) { written = -1; goto done; }
    for (uint32_t i = 0; i < ab.seq_count; i++) {
        NdsAnimation a;
        if (nds_render_anim(&ab, i, &cb, &ts, o->pal ? &pal : NULL, &a) != NDS_OK) continue;
        sprintf(out, "%s_%u.gif", prefix, i);
        if (nds_write_gif(out, &a) == NDS_OK) written++;
        else fprintf(stderr, "cannot write %s\n", out);
        nds_animation_free(&a);
    }
    free(out);
done:
    nds_tileset_free(&ts);
    nds_palette_free(&pal);
    nds_cellbank_free(&cb);
    nds_animbank_free(&ab);
    return written;
}

static int cmd_anim_all(const NdsRom *rom, const char *dir) {
    size_t count = nds_file_count(rom);
    NdsKind *kinds = calloc(count, sizeof *kinds);
    if (!kinds) return 1;
    for (size_t i = 0; i < count; i++) kinds[i] = kind_of(rom, nds_file_at(rom, i));

    size_t sprites = 0, gifs = 0, failed = 0;
    for (size_t i = 0; i < count; i++) {
        if (kinds[i] != NDS_KIND_NCGR) continue;
        const NdsFile *g = nds_file_at(rom, i);
        AnimOpts o = {0};
        find_anim_set(rom, g, kinds, &o);
        if (!o.cer || !o.anr) continue;

        char *prefix = malloc(strlen(dir) + strlen(g->path) + 2);
        if (!prefix) break;
        sprintf(prefix, "%s/%s", dir, g->path);
        int n = mkdirs(prefix) == 0 ? write_anims(rom, g->path, &o, prefix) : -1;
        free(prefix);
        if (n < 0) failed++;
        else { sprites++; gifs += (size_t)n; }
    }
    free(kinds);
    printf("Wrote %zu animations for %zu sprites to %s (%zu failed)\n", gifs, sprites, dir, failed);
    return failed ? 1 : 0;
}

static NdsResult load_msgs(const NdsRom *rom, const char *path, NdsMsgTable *t) {
    uint8_t *d;
    uint32_t n;
    NdsResult r = load_unpacked(rom, path, &d, &n);
    if (r == NDS_OK) {
        r = nds_msgtable_parse(d, n, t);
        free(d);
        if (r == NDS_OK) return r;
    }
    /* A table whose first offset looks like a compression header is stored raw. */
    const NdsFile *f = nds_find_file(rom, path);
    if (!f) return NDS_ERR_NOTFOUND;
    r = nds_load_file(rom, f, &d, &n);
    if (r != NDS_OK) return r;
    r = nds_msgtable_parse(d, n, t);
    free(d);
    return r;
}

static void print_msgs(FILE *o, const NdsMsgTable *t) {
    for (uint32_t i = 0; i < t->count; i++) {
        char *s = nds_msg_decode(t->codes[i], t->len[i]);
        if (!s) continue;
        fprintf(o, "[%u]\n%s\n\n", i, s);
        free(s);
    }
}

static int cmd_text(const NdsRom *rom, const char *path) {
    NdsMsgTable t;
    NdsResult r = load_msgs(rom, path, &t);
    if (r != NDS_OK) { fprintf(stderr, "%s: %s\n", path, nds_strerror(r)); return 1; }
    print_msgs(stdout, &t);
    nds_msgtable_free(&t);
    return 0;
}

static int cmd_text_all(const NdsRom *rom, const char *dir) {
    size_t done = 0, skipped = 0;
    for (size_t i = 0; i < nds_file_count(rom); i++) {
        const NdsFile *f = nds_file_at(rom, i);
        size_t pl = strlen(f->path);
        if (pl < 4 || strcmp(f->path + pl - 4, ".sdb") != 0) continue;
        NdsMsgTable t;
        if (load_msgs(rom, f->path, &t) != NDS_OK) { skipped++; continue; }
        char *out = malloc(strlen(dir) + pl + 8);
        if (!out) { nds_msgtable_free(&t); break; }
        sprintf(out, "%s/%s.txt", dir, f->path);
        FILE *o = mkdirs(out) == 0 ? fopen(out, "w") : NULL;
        if (o) {
            print_msgs(o, &t);
            fclose(o);
            done++;
        } else skipped++;
        free(out);
        nds_msgtable_free(&t);
    }
    printf("Wrote %zu message banks to %s (%zu skipped)\n", done, dir, skipped);
    return 0;
}

static NdsSdat *open_sdat(const NdsRom *rom, const char *path, uint8_t **buf) {
    uint32_t n;
    *buf = NULL;
    NdsResult r = load_unpacked(rom, path, buf, &n);
    NdsSdat *s = NULL;
    if (r == NDS_OK) r = nds_sdat_open(*buf, n, &s);
    if (r != NDS_OK) {
        fprintf(stderr, "%s: %s\n", path, nds_strerror(r));
        free(*buf);
        *buf = NULL;
    }
    return s;
}

static int cmd_sdat(const NdsRom *rom, const char *path) {
    uint8_t *buf;
    NdsSdat *s = open_sdat(rom, path, &buf);
    if (!s) return 1;
    for (size_t i = 0; i < nds_sdat_count(s); i++) {
        const NdsSdatEntry *e = nds_sdat_at(s, i);
        printf("%-8s %5u  file %5u  %8u  %s\n", nds_sdat_type_name(e->type), e->index,
               e->file_id, e->size, e->name);
    }
    nds_sdat_close(s);
    free(buf);
    return 0;
}

static void safe_name(char *s) {
    for (; *s; s++)
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              *s == '_' || *s == '-' || *s == '.'))
            *s = '_';
}

static int cmd_audio(const NdsRom *rom, const char *path, const char *dir) {
    uint8_t *buf;
    NdsSdat *s = open_sdat(rom, path, &buf);
    if (!s) return 1;
    size_t files = 0, waves = 0, failed = 0;

    for (size_t i = 0; i < nds_sdat_count(s); i++) {
        const NdsSdatEntry *e = nds_sdat_at(s, i);
        char *name = strdup(e->name);
        char *out = malloc(strlen(dir) + strlen(e->name) * 2 + 64);
        if (!name || !out) { free(name); free(out); break; }
        safe_name(name);

        sprintf(out, "%s/%s/%s.%s", dir, nds_sdat_type_name(e->type), name, nds_sdat_type_ext(e->type));
        if (mkdirs(out) == 0 && write_file(out, e->data, e->size) == 0) files++;
        else failed++;

        if (e->type == NDS_SDAT_STRM) {
            NdsAudio a;
            sprintf(out, "%s/WAV/%s.wav", dir, name);
            if (nds_strm_decode(e->data, e->size, &a) == NDS_OK) {
                if (mkdirs(out) == 0 && nds_write_wav(out, &a) == NDS_OK) waves++;
                else failed++;
                nds_audio_free(&a);
            } else failed++;
        } else if (e->type == NDS_SDAT_WAVEARC) {
            uint32_t count = 0;
            if (nds_swar_count(e->data, e->size, &count) != NDS_OK) { failed++; count = 0; }
            for (uint32_t w = 0; w < count; w++) {
                NdsAudio a;
                sprintf(out, "%s/WAV/%s/%04u.wav", dir, name, w);
                if (nds_swar_decode(e->data, e->size, w, &a) != NDS_OK) continue; /* empty slot */
                if (mkdirs(out) == 0 && nds_write_wav(out, &a) == NDS_OK) waves++;
                else failed++;
                nds_audio_free(&a);
            }
        }
        free(name);
        free(out);
    }
    printf("Extracted %zu files and %zu waves to %s (%zu failed)\n", files, waves, dir, failed);
    nds_sdat_close(s);
    free(buf);
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    NdsResult r;
    NdsRom *rom = nds_open(argv[1], &r);
    if (!rom) {
        fprintf(stderr, "Cannot open %s: %s\n", argv[1], nds_strerror(r));
        return 1;
    }
    const char *cmd = argc > 2 ? argv[2] : "info";
    int rc = 0;
    const NdsFile *f = NULL;

    if (!strcmp(cmd, "info")) cmd_info(rom);
    else if (!strcmp(cmd, "list")) cmd_list(rom);
    else if (!strcmp(cmd, "extract-all") && argc > 3) rc = cmd_extract_all(rom, argv[3]);
    else if (!strcmp(cmd, "png-all") && argc > 3) rc = cmd_png_all(rom, argv[3]);
    else if (!strcmp(cmd, "text") && argc > 3) rc = cmd_text(rom, argv[3]);
    else if (!strcmp(cmd, "text-all") && argc > 3) rc = cmd_text_all(rom, argv[3]);
    else if (!strcmp(cmd, "sdat") && argc > 3) rc = cmd_sdat(rom, argv[3]);
    else if (!strcmp(cmd, "audio") && argc > 4) rc = cmd_audio(rom, argv[3], argv[4]);
    else if (!strcmp(cmd, "anim-all") && argc > 3) rc = cmd_anim_all(rom, argv[3]);
    else if (!strcmp(cmd, "anim") && argc > 4) {
        AnimOpts o = {0};
        for (int i = 5; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--pal")) o.pal = argv[i + 1];
            else if (!strcmp(argv[i], "--cer")) o.cer = argv[i + 1];
            else if (!strcmp(argv[i], "--anr")) o.anr = argv[i + 1];
            else { usage(argv[0]); nds_close(rom); return 2; }
        }
        const NdsFile *g = nds_find_file(rom, argv[3]);
        if (!g) { fprintf(stderr, "File not found: %s\n", argv[3]); rc = 1; }
        else {
            AnimOpts found = {0};
            find_anim_set(rom, g, NULL, &found);
            if (!o.pal) o.pal = found.pal;
            if (!o.cer) o.cer = found.cer;
            if (!o.anr) o.anr = found.anr;
            int n = write_anims(rom, argv[3], &o, argv[4]);
            if (n >= 0) printf("Wrote %d animations\n", n);
            rc = n < 0;
        }
    }
    else if (!strcmp(cmd, "unpack") && argc > 4) rc = cmd_unpack(rom, argv[3], argv[4]);
    else if (!strcmp(cmd, "png") && argc > 4) {
        RenderOpts o = {0};
        for (int i = 5; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--pal")) o.pal = argv[i + 1];
            else if (!strcmp(argv[i], "--scr")) o.scr = argv[i + 1];
            else if (!strcmp(argv[i], "--cer")) o.cer = argv[i + 1];
            else if (!strcmp(argv[i], "--width")) o.width = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            else if (!strcmp(argv[i], "--bank")) o.bank = (uint32_t)strtoul(argv[i + 1], NULL, 0);
            else { usage(argv[0]); nds_close(rom); return 2; }
        }
        if (!o.pal) {
            const NdsFile *g = nds_find_file(rom, argv[3]);
            for (size_t i = 0; g && i < nds_file_count(rom); i++) {
                const NdsFile *c = nds_file_at(rom, i);
                if (c != g && same_set(g, c) && kind_of(rom, c) == NDS_KIND_NCLR) {
                    o.pal = c->path;
                    break;
                }
            }
        }
        rc = render_png(rom, argv[3], &o, argv[4]);
    }
    else if ((!strcmp(cmd, "cat") || !strcmp(cmd, "hex") || !strcmp(cmd, "extract")) &&
             argc > 3) {
        f = nds_find_file(rom, argv[3]);
        if (!f) { fprintf(stderr, "File not found: %s\n", argv[3]); rc = 1; }
        else if (!strcmp(cmd, "cat")) rc = cmd_cat(rom, f);
        else if (!strcmp(cmd, "hex"))
            rc = cmd_hex(rom, f, argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : 256);
        else if (argc > 4) rc = nds_extract_file(rom, f, argv[4]) != NDS_OK;
        else { usage(argv[0]); rc = 2; }
    } else { usage(argv[0]); rc = 2; }

    nds_close(rom);
    return rc;
}
