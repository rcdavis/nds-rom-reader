#include "ndsrom/ndsgfx.h"

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
            "  png <path> <out.png> [--pal <path>] [--scr <path>] [--width <tiles>] [--bank <n>]\n"
            "                       render an NCGR graphic (palette/map optional, auto-detected\n"
            "                       from same-named files in the directory if omitted)\n"
            "  png-all <dir>        render every NCGR graphic in the ROM to PNG files\n",
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
    const char *pal, *scr;
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
    if (o->scr) {
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
        }

        char *out = malloc(strlen(dir) + strlen(g->path) + 16);
        if (!out) break;
        RenderOpts sheet = {o.pal, NULL, 0, 0};
        sprintf(out, "%s/%s.png", dir, g->path);
        int bad = mkdirs(out) != 0 || render_png(rom, g->path, &sheet, out);
        if (!bad && o.scr) {
            sprintf(out, "%s/%s_screen.png", dir, g->path);
            bad = render_png(rom, g->path, &o, out);
        }
        free(out);
        if (bad) failed++; else done++;
    }
    free(kinds);
    printf("Rendered %zu graphics to %s (%zu failed)\n", done, dir, failed);
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
    else if (!strcmp(cmd, "unpack") && argc > 4) rc = cmd_unpack(rom, argv[3], argv[4]);
    else if (!strcmp(cmd, "png") && argc > 4) {
        RenderOpts o = {0};
        for (int i = 5; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--pal")) o.pal = argv[i + 1];
            else if (!strcmp(argv[i], "--scr")) o.scr = argv[i + 1];
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
