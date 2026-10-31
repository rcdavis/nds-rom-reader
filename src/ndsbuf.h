#ifndef NDSBUF_H
#define NDSBUF_H

#include <stddef.h>
#include <stdint.h>

/* Bounds-checked little-endian reader. Out-of-range reads return 0 and set bad. */
typedef struct {
    const uint8_t *d;
    size_t n;
    int bad;
} Buf;

static inline uint32_t rd8(Buf *b, size_t o) {
    if (o >= b->n) { b->bad = 1; return 0; }
    return b->d[o];
}

static inline uint32_t rd16(Buf *b, size_t o) {
    if (o > b->n || b->n - o < 2) { b->bad = 1; return 0; }
    return b->d[o] | ((uint32_t)b->d[o + 1] << 8);
}

static inline uint32_t rd32(Buf *b, size_t o) {
    if (o > b->n || b->n - o < 4) { b->bad = 1; return 0; }
    return b->d[o] | ((uint32_t)b->d[o + 1] << 8) | ((uint32_t)b->d[o + 2] << 16) |
           ((uint32_t)b->d[o + 3] << 24);
}

static inline int32_t rds16(Buf *b, size_t o) { return (int16_t)rd16(b, o); }
static inline int32_t rds32(Buf *b, size_t o) { return (int32_t)rd32(b, o); }

/* A Nitro "dictionary": a name table with fixed-size data entries. */
typedef struct {
    uint32_t n, es;
    size_t ent[256];
    char name[256][17];
} NdsDict;

/* Pivot rotation: select 0-8, neg bits, a and b = the two stored values.
   out is column-major (out[3*c + i]). */
void nds_pivot_mat(unsigned sel, unsigned neg, double a, double b, double out[9]);

/* Inverts a 3x4 row-major affine matrix; returns -1 if it is singular. */
int nds_affine_inverse(const double m[12], double out[12]);

size_t nds_nitro_block(Buf *b, const char *magic);
int nds_dict_read(Buf *b, size_t base, NdsDict *d);

#endif
