#ifndef NDSROM_H
#define NDSROM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NDS_OK = 0,
    NDS_ERR_IO,
    NDS_ERR_NOMEM,
    NDS_ERR_FORMAT,
    NDS_ERR_NOTFOUND,
    NDS_ERR_ARG
} NdsResult;

typedef struct {
    char title[13];
    char game_code[5];
    char maker_code[3];
    uint8_t unit_code;
    uint8_t version;
    uint32_t arm9_offset, arm9_entry, arm9_ram, arm9_size;
    uint32_t arm7_offset, arm7_entry, arm7_ram, arm7_size;
    uint32_t fnt_offset, fnt_size;
    uint32_t fat_offset, fat_size;
    uint32_t arm9_ovl_offset, arm9_ovl_size;
    uint32_t arm7_ovl_offset, arm7_ovl_size;
    uint32_t icon_offset;
    uint32_t total_size;
    uint32_t header_size;
    uint16_t header_crc;
    int header_crc_ok;
} NdsHeader;

typedef struct {
    char *path;          /* full path without leading slash, e.g. "data/a.bin" */
    const char *name;    /* points into path */
    uint16_t id;         /* file id, index into the FAT */
    uint32_t offset;     /* absolute ROM offset */
    uint32_t size;
} NdsFile;

typedef struct NdsRom NdsRom;

NdsRom *nds_open(const char *path, NdsResult *result);
void nds_close(NdsRom *rom);
const char *nds_strerror(NdsResult r);

const NdsHeader *nds_header(const NdsRom *rom);

/* Files found in the filesystem (FNT/FAT), in directory-walk order. */
size_t nds_file_count(const NdsRom *rom);
const NdsFile *nds_file_at(const NdsRom *rom, size_t index);
const NdsFile *nds_find_file(const NdsRom *rom, const char *path);

/* Copies up to len bytes of file contents starting at start into buf.
   Returns the number of bytes read, or -1 on error. */
long nds_read_file(const NdsRom *rom, const NdsFile *file, uint32_t start,
                   void *buf, size_t len);

/* Reads the whole file into a malloc'd buffer (free with free()). */
NdsResult nds_load_file(const NdsRom *rom, const NdsFile *file, uint8_t **data,
                        uint32_t *size);

NdsResult nds_extract_file(const NdsRom *rom, const NdsFile *file,
                           const char *dest_path);

#ifdef __cplusplus
}
#endif

#endif
