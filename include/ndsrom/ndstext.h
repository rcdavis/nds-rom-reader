#ifndef NDSTEXT_H
#define NDSTEXT_H

#include "ndsrom.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Message banks (.sdb): a table of u32 offsets to entries of u16 character
   codes, each ending in 0xFFFF. */
typedef struct {
    uint32_t count;
    uint32_t *len;      /* codes per entry, excluding the terminator */
    uint16_t **codes;   /* pointers into storage */
    uint16_t *storage;
} NdsMsgTable;

NdsResult nds_msgtable_parse(const uint8_t *data, size_t size, NdsMsgTable *out);
void nds_msgtable_free(NdsMsgTable *t);

/* Decodes one entry to a malloc'd UTF-8 string using the game's English font
   order (A-Z at 0x50, a-z at 0x6A). Control codes become {name}, and codes with
   no known glyph become <hex>. */
char *nds_msg_decode(const uint16_t *codes, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif
