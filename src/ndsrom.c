#include "ndsrom/ndsrom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NDS_HEADER_MIN 0x200
#define MAX_DEPTH 64

struct NdsRom {
    FILE *fp;
    uint32_t file_size;
    NdsHeader header;
    NdsFile *files;
    size_t count, cap;
    uint8_t *fnt;
    uint8_t *fat;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

const char *nds_strerror(NdsResult r) {
    switch (r) {
    case NDS_OK: return "ok";
    case NDS_ERR_IO: return "I/O error";
    case NDS_ERR_NOMEM: return "out of memory";
    case NDS_ERR_FORMAT: return "invalid or corrupt ROM";
    case NDS_ERR_NOTFOUND: return "not found";
    case NDS_ERR_ARG: return "invalid argument";
    }
    return "unknown error";
}

static uint16_t crc16(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static void copy_str(char *dst, const uint8_t *src, size_t n) {
    memcpy(dst, src, n);
    dst[n] = '\0';
    for (size_t i = 0; i < n && dst[i]; i++)
        if ((unsigned char)dst[i] < 0x20 || (unsigned char)dst[i] > 0x7E) dst[i] = '?';
}

static int read_at(FILE *fp, uint32_t off, void *buf, size_t len) {
    if (len == 0) return 1;
    if (fseek(fp, (long)off, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, fp) == len;
}

static void parse_header(NdsHeader *h, const uint8_t *b) {
    copy_str(h->title, b, 12);
    copy_str(h->game_code, b + 0x0C, 4);
    copy_str(h->maker_code, b + 0x10, 2);
    h->unit_code = b[0x12];
    h->version = b[0x1E];
    h->arm9_offset = rd32(b + 0x20);
    h->arm9_entry = rd32(b + 0x24);
    h->arm9_ram = rd32(b + 0x28);
    h->arm9_size = rd32(b + 0x2C);
    h->arm7_offset = rd32(b + 0x30);
    h->arm7_entry = rd32(b + 0x34);
    h->arm7_ram = rd32(b + 0x38);
    h->arm7_size = rd32(b + 0x3C);
    h->fnt_offset = rd32(b + 0x40);
    h->fnt_size = rd32(b + 0x44);
    h->fat_offset = rd32(b + 0x48);
    h->fat_size = rd32(b + 0x4C);
    h->arm9_ovl_offset = rd32(b + 0x50);
    h->arm9_ovl_size = rd32(b + 0x54);
    h->arm7_ovl_offset = rd32(b + 0x58);
    h->arm7_ovl_size = rd32(b + 0x5C);
    h->icon_offset = rd32(b + 0x68);
    h->total_size = rd32(b + 0x80);
    h->header_size = rd32(b + 0x84);
    h->header_crc = rd16(b + 0x15E);
    h->header_crc_ok = crc16(b, 0x15E) == h->header_crc;
}

static int range_ok(uint32_t off, uint32_t size, uint32_t limit) {
    return off <= limit && size <= limit - off;
}

static NdsResult add_file(NdsRom *rom, const char *dir, const uint8_t *name,
                          size_t nlen, uint16_t id) {
    if (id >= rom->header.fat_size / 8) return NDS_ERR_FORMAT;
    uint32_t start = rd32(rom->fat + id * 8u);
    uint32_t end = rd32(rom->fat + id * 8u + 4);
    if (end < start || !range_ok(start, end - start, rom->file_size))
        return NDS_ERR_FORMAT;

    if (rom->count == rom->cap) {
        size_t ncap = rom->cap ? rom->cap * 2 : 256;
        NdsFile *nf = realloc(rom->files, ncap * sizeof *nf);
        if (!nf) return NDS_ERR_NOMEM;
        rom->files = nf;
        rom->cap = ncap;
    }
    size_t dlen = strlen(dir);
    char *path = malloc(dlen + nlen + 1);
    if (!path) return NDS_ERR_NOMEM;
    memcpy(path, dir, dlen);
    memcpy(path + dlen, name, nlen);
    path[dlen + nlen] = '\0';

    NdsFile *f = &rom->files[rom->count++];
    f->path = path;
    f->name = path + dlen;
    f->id = id;
    f->offset = start;
    f->size = end - start;
    return NDS_OK;
}

static NdsResult walk_dir(NdsRom *rom, uint16_t dir_id, const char *prefix, int depth) {
    uint32_t ndirs = rd16(rom->fnt + 6);
    uint32_t idx = dir_id & 0xFFF;
    uint32_t fnt_size = rom->header.fnt_size;
    if (depth > MAX_DEPTH || idx >= ndirs || (idx + 1) * 8 > fnt_size)
        return NDS_ERR_FORMAT;

    uint32_t pos = rd32(rom->fnt + idx * 8);
    uint16_t file_id = rd16(rom->fnt + idx * 8 + 4);

    for (;;) {
        if (pos >= fnt_size) return NDS_ERR_FORMAT;
        uint8_t t = rom->fnt[pos++];
        if (t == 0) return NDS_OK;
        size_t len = t & 0x7F;
        if (len == 0 || pos + len > fnt_size) return NDS_ERR_FORMAT;
        const uint8_t *name = rom->fnt + pos;
        pos += (uint32_t)len;

        NdsResult r;
        if (t & 0x80) {
            if (pos + 2 > fnt_size) return NDS_ERR_FORMAT;
            uint16_t sub = rd16(rom->fnt + pos);
            pos += 2;
            size_t plen = strlen(prefix);
            char *np = malloc(plen + len + 2);
            if (!np) return NDS_ERR_NOMEM;
            memcpy(np, prefix, plen);
            memcpy(np + plen, name, len);
            np[plen + len] = '/';
            np[plen + len + 1] = '\0';
            r = walk_dir(rom, sub, np, depth + 1);
            free(np);
        } else {
            r = add_file(rom, prefix, name, len, file_id++);
        }
        if (r != NDS_OK) return r;
    }
}

static NdsResult load_fs(NdsRom *rom) {
    NdsHeader *h = &rom->header;
    if (h->fnt_size < 8 || h->fat_size == 0 || h->fat_size % 8 ||
        !range_ok(h->fnt_offset, h->fnt_size, rom->file_size) ||
        !range_ok(h->fat_offset, h->fat_size, rom->file_size))
        return NDS_ERR_FORMAT;

    rom->fnt = malloc(h->fnt_size);
    rom->fat = malloc(h->fat_size);
    if (!rom->fnt || !rom->fat) return NDS_ERR_NOMEM;
    if (!read_at(rom->fp, h->fnt_offset, rom->fnt, h->fnt_size) ||
        !read_at(rom->fp, h->fat_offset, rom->fat, h->fat_size))
        return NDS_ERR_IO;
    return walk_dir(rom, 0xF000, "", 0);
}

NdsRom *nds_open(const char *path, NdsResult *result) {
    NdsResult dummy;
    if (!result) result = &dummy;
    *result = NDS_OK;
    if (!path) { *result = NDS_ERR_ARG; return NULL; }

    NdsRom *rom = calloc(1, sizeof *rom);
    if (!rom) { *result = NDS_ERR_NOMEM; return NULL; }
    rom->fp = fopen(path, "rb");
    if (!rom->fp) { *result = NDS_ERR_IO; goto fail; }

    if (fseek(rom->fp, 0, SEEK_END) != 0) { *result = NDS_ERR_IO; goto fail; }
    long sz = ftell(rom->fp);
    if (sz < NDS_HEADER_MIN || (unsigned long)sz > 0xFFFFFFFFul) {
        *result = NDS_ERR_FORMAT;
        goto fail;
    }
    rom->file_size = (uint32_t)sz;

    uint8_t hdr[NDS_HEADER_MIN];
    if (!read_at(rom->fp, 0, hdr, sizeof hdr)) { *result = NDS_ERR_IO; goto fail; }
    parse_header(&rom->header, hdr);

    *result = load_fs(rom);
    if (*result != NDS_OK) goto fail;
    return rom;

fail:
    nds_close(rom);
    return NULL;
}

void nds_close(NdsRom *rom) {
    if (!rom) return;
    for (size_t i = 0; i < rom->count; i++) free(rom->files[i].path);
    free(rom->files);
    free(rom->fnt);
    free(rom->fat);
    if (rom->fp) fclose(rom->fp);
    free(rom);
}

const NdsHeader *nds_header(const NdsRom *rom) { return &rom->header; }
size_t nds_file_count(const NdsRom *rom) { return rom->count; }
const NdsFile *nds_file_at(const NdsRom *rom, size_t i) {
    return i < rom->count ? &rom->files[i] : NULL;
}

const NdsFile *nds_find_file(const NdsRom *rom, const char *path) {
    while (*path == '/') path++;
    for (size_t i = 0; i < rom->count; i++)
        if (strcmp(rom->files[i].path, path) == 0) return &rom->files[i];
    return NULL;
}

long nds_read_file(const NdsRom *rom, const NdsFile *file, uint32_t start,
                   void *buf, size_t len) {
    if (!file || !buf || start > file->size) return -1;
    if (len > file->size - start) len = file->size - start;
    if (!read_at(rom->fp, file->offset + start, buf, len)) return -1;
    return (long)len;
}

NdsResult nds_load_file(const NdsRom *rom, const NdsFile *file, uint8_t **data,
                        uint32_t *size) {
    if (!file || !data || !size) return NDS_ERR_ARG;
    uint8_t *b = malloc(file->size ? file->size : 1);
    if (!b) return NDS_ERR_NOMEM;
    if (nds_read_file(rom, file, 0, b, file->size) < 0) {
        free(b);
        return NDS_ERR_IO;
    }
    *data = b;
    *size = file->size;
    return NDS_OK;
}

NdsResult nds_extract_file(const NdsRom *rom, const NdsFile *file,
                           const char *dest_path) {
    if (!file || !dest_path) return NDS_ERR_ARG;
    FILE *out = fopen(dest_path, "wb");
    if (!out) return NDS_ERR_IO;
    uint8_t buf[65536];
    uint32_t done = 0;
    NdsResult r = NDS_OK;
    while (done < file->size) {
        long n = nds_read_file(rom, file, done, buf, sizeof buf);
        if (n <= 0 || fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            r = NDS_ERR_IO;
            break;
        }
        done += (uint32_t)n;
    }
    if (fclose(out) != 0) r = NDS_ERR_IO;
    return r;
}
