#include "ndsrom/ndstext.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 65536

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

NdsResult nds_msgtable_parse(const uint8_t *d, size_t n, NdsMsgTable *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (n < 8 || !d) return NDS_ERR_FORMAT;

    uint32_t first = rd32(d);
    uint32_t count = first / 4;
    if (first % 4 || count == 0 || count > MAX_ENTRIES || first > n) return NDS_ERR_FORMAT;

    uint32_t *start = malloc(count * sizeof *start);
    out->len = calloc(count, sizeof *out->len);
    out->codes = calloc(count, sizeof *out->codes);
    out->storage = malloc(n / 2 * 2 + 2);
    if (!start || !out->len || !out->codes || !out->storage) {
        free(start);
        nds_msgtable_free(out);
        return NDS_ERR_NOMEM;
    }
    memcpy(out->storage, d, n / 2 * 2);
    for (size_t i = 0; i < n / 2; i++) out->storage[i] = (uint16_t)(d[i * 2] | (d[i * 2 + 1] << 8));

    uint32_t valid = 0;
    while (valid < count) {
        start[valid] = rd32(d + valid * 4);
        if (start[valid] % 2 || start[valid] < first || start[valid] >= n) break;
        valid++;
    }
    if (valid != count) {
        free(start);
        nds_msgtable_free(out);
        return NDS_ERR_FORMAT;
    }
    for (uint32_t i = 0; i < count; i++) {
        size_t pos = start[i] / 2, end = n / 2, len = 0;
        while (pos + len < end && out->storage[pos + len] != 0xFFFF) len++;
        out->codes[i] = out->storage + pos;
        out->len[i] = (uint32_t)len;
    }
    out->count = count;
    free(start);
    return NDS_OK;
}

void nds_msgtable_free(NdsMsgTable *t) {
    if (!t) return;
    free(t->len);
    free(t->codes);
    free(t->storage);
    memset(t, 0, sizeof *t);
}

static const char *glyph(uint16_t c) {
    static char buf[2];
    if (c == 0x00) return " ";
    if (c >= 0x01 && c <= 0x09) { buf[0] = (char)('0' + c); return buf; }
    if (c == 0x0A) return "0";
    if (c >= 0x50 && c <= 0x69) { buf[0] = (char)('A' + c - 0x50); return buf; }
    if (c >= 0x6A && c <= 0x83) { buf[0] = (char)('a' + c - 0x6A); return buf; }
    switch (c) {
    case 0x0B: return "/";
    case 0x0C: return "%";
    case 0x0E: return "'";
    case 0x11: return ",";
    case 0x12: case 0x1C: return ".";
    case 0x13: return "!";
    case 0x14: return "?";
    case 0x17: return "+";
    case 0x18: return "-";
    case 0xFFFD: return "\n";
    case 0xFFFE: return "\n[page]\n";
    case 0xFFEC: return "{name}";
    case 0xFFED: return "{target}";
    case 0xFFEE: case 0xFFEF: return "{hl}";
    case 0xFFF0: return "{var0}";
    case 0xFFF1: return "{var1}";
    case 0xFFF2: return "{var2}";
    }
    return NULL;
}

char *nds_msg_decode(const uint16_t *codes, uint32_t len) {
    size_t cap = (size_t)len * 9 + 1, used = 0;
    char *s = malloc(cap);
    if (!s) return NULL;
    for (uint32_t i = 0; i < len; i++) {
        const char *g = glyph(codes[i]);
        char tmp[8];
        if (!g) { snprintf(tmp, sizeof tmp, "<%X>", codes[i]); g = tmp; }
        size_t gl = strlen(g);
        memcpy(s + used, g, gl);
        used += gl;
    }
    s[used] = '\0';
    return s;
}
