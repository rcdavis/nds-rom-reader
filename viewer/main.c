#include "ndsrom/ndsrom.h"

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
            "  extract-all <dir>    extract every file, preserving directories\n",
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
