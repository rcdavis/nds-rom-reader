#include "ndsrom/ndssdat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAMES (64u * 1024u * 1024u)
#define MAX_RECORDS 65536

struct NdsSdat {
    NdsSdatEntry *entries;
    size_t count;
};

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static int in_range(uint64_t off, uint64_t len, uint64_t size) { return off <= size && len <= size - off; }

const char *nds_sdat_type_name(NdsSdatType t) {
    switch (t) {
    case NDS_SDAT_SEQ: return "SEQ";
    case NDS_SDAT_SEQARC: return "SEQARC";
    case NDS_SDAT_BANK: return "BANK";
    case NDS_SDAT_WAVEARC: return "WAVEARC";
    case NDS_SDAT_STRM: return "STRM";
    }
    return "?";
}

const char *nds_sdat_type_ext(NdsSdatType t) {
    switch (t) {
    case NDS_SDAT_SEQ: return "sseq";
    case NDS_SDAT_SEQARC: return "ssar";
    case NDS_SDAT_BANK: return "sbnk";
    case NDS_SDAT_WAVEARC: return "swar";
    case NDS_SDAT_STRM: return "strm";
    }
    return "bin";
}

static char *dup_name(const uint8_t *symb, uint32_t symb_size, uint32_t off) {
    if (off >= symb_size) return NULL;
    size_t max = symb_size - off, n = 0;
    while (n < max && symb[off + n]) n++;
    if (n == max || n == 0) return NULL;
    char *s = malloc(n + 1);
    if (s) { memcpy(s, symb + off, n); s[n] = '\0'; }
    return s;
}

/* Name of record `index` of record type `type`, or NULL. */
static char *symbol_name(const uint8_t *symb, uint32_t symb_size, int type, uint32_t index) {
    if (!symb || symb_size < 8 + 32) return NULL;
    uint32_t list = rd32(symb + 8 + type * 4);
    if (!in_range(list, 4, symb_size)) return NULL;
    uint32_t count = rd32(symb + list);
    if (index >= count || !in_range(list + 4, (uint64_t)count * 4, symb_size)) return NULL;
    uint32_t off = rd32(symb + list + 4 + index * 4);
    if (type == NDS_SDAT_SEQARC) { /* record is {name offset, sub-list offset} */
        if (!in_range(off, 8, symb_size)) return NULL;
        off = rd32(symb + off);
    }
    return dup_name(symb, symb_size, off);
}

static NdsResult add_entry(struct NdsSdat *s, size_t *cap, NdsSdatEntry e) {
    if (s->count == *cap) {
        size_t ncap = *cap ? *cap * 2 : 128;
        NdsSdatEntry *n = realloc(s->entries, ncap * sizeof *n);
        if (!n) return NDS_ERR_NOMEM;
        s->entries = n;
        *cap = ncap;
    }
    s->entries[s->count++] = e;
    return NDS_OK;
}

NdsResult nds_sdat_open(const uint8_t *d, size_t n, NdsSdat **out) {
    if (!d || !out) return NDS_ERR_ARG;
    *out = NULL;
    if (n < 0x40 || memcmp(d, "SDAT", 4) != 0) return NDS_ERR_FORMAT;

    uint32_t symb_off = rd32(d + 0x10), symb_sz = rd32(d + 0x14);
    uint32_t info_off = rd32(d + 0x18), info_sz = rd32(d + 0x1C);
    uint32_t fat_off = rd32(d + 0x20), fat_sz = rd32(d + 0x24);
    if (!in_range(info_off, info_sz, n) || !in_range(fat_off, fat_sz, n) || info_sz < 0x28 ||
        fat_sz < 12)
        return NDS_ERR_FORMAT;
    const uint8_t *symb = NULL;
    if (symb_off && in_range(symb_off, symb_sz, n) && !memcmp(d + symb_off, "SYMB", 4))
        symb = d + symb_off;
    else
        symb_sz = 0;
    const uint8_t *info = d + info_off, *fat = d + fat_off;
    if (memcmp(info, "INFO", 4) != 0 || memcmp(fat, "FAT ", 4) != 0) return NDS_ERR_FORMAT;
    uint32_t nfiles = rd32(fat + 8);
    if (!in_range(12, (uint64_t)nfiles * 16, fat_sz)) return NDS_ERR_FORMAT;

    struct NdsSdat *s = calloc(1, sizeof *s);
    if (!s) return NDS_ERR_NOMEM;
    size_t cap = 0;
    static const int types[] = {NDS_SDAT_SEQ, NDS_SDAT_SEQARC, NDS_SDAT_BANK, NDS_SDAT_WAVEARC,
                                NDS_SDAT_STRM};

    for (size_t t = 0; t < sizeof types / sizeof *types; t++) {
        uint32_t list = rd32(info + 8 + types[t] * 4);
        if (list == 0) continue;
        if (!in_range(list, 4, info_sz)) { nds_sdat_close(s); return NDS_ERR_FORMAT; }
        uint32_t count = rd32(info + list);
        if (count > MAX_RECORDS || !in_range(list + 4, (uint64_t)count * 4, info_sz)) {
            nds_sdat_close(s);
            return NDS_ERR_FORMAT;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t rec = rd32(info + list + 4 + i * 4);
            if (rec == 0) continue; /* unused slot */
            if (!in_range(rec, 4, info_sz)) { nds_sdat_close(s); return NDS_ERR_FORMAT; }
            uint32_t fid = rd32(info + rec);
            if (fid >= nfiles) continue;
            uint32_t foff = rd32(fat + 12 + fid * 16), fsz = rd32(fat + 12 + fid * 16 + 4);
            if (!in_range(foff, fsz, n)) continue;

            NdsSdatEntry e = {(NdsSdatType)types[t], i, NULL, fid, d + foff, fsz};
            e.name = symbol_name(symb, symb_sz, types[t], i);
            if (!e.name) {
                e.name = malloc(24);
                if (e.name) snprintf(e.name, 24, "%s_%04u", nds_sdat_type_name(e.type), i);
            }
            NdsResult r = e.name ? add_entry(s, &cap, e) : NDS_ERR_NOMEM;
            if (r != NDS_OK) { free(e.name); nds_sdat_close(s); return r; }
        }
    }
    *out = s;
    return NDS_OK;
}

void nds_sdat_close(NdsSdat *s) {
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) free(s->entries[i].name);
    free(s->entries);
    free(s);
}

size_t nds_sdat_count(const NdsSdat *s) { return s->count; }
const NdsSdatEntry *nds_sdat_at(const NdsSdat *s, size_t i) {
    return i < s->count ? &s->entries[i] : NULL;
}

void nds_audio_free(NdsAudio *a) {
    if (a) { free(a->pcm); memset(a, 0, sizeof *a); }
}

/* ---- IMA ADPCM ---- */

static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767};
static const int8_t index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* 4-byte header (predictor, step index) followed by nibbles, low nibble first.
   Writes (len - 4) * 2 samples at out with the given stride. */
static void adpcm_block(const uint8_t *in, uint32_t len, int16_t *out, size_t stride) {
    int pred = (int16_t)rd16(in), idx = rd16(in + 2) & 0x7F;
    if (idx > 88) idx = 88;
    for (uint32_t i = 4; i < len; i++) {
        for (int h = 0; h < 2; h++) {
            int nib = h ? in[i] >> 4 : in[i] & 0xF;
            int step = step_table[idx];
            int diff = step >> 3;
            if (nib & 1) diff += step >> 2;
            if (nib & 2) diff += step >> 1;
            if (nib & 4) diff += step;
            pred += (nib & 8) ? -diff : diff;
            if (pred > 32767) pred = 32767;
            if (pred < -32768) pred = -32768;
            idx += index_table[nib & 7];
            if (idx < 0) idx = 0;
            if (idx > 88) idx = 88;
            out[((size_t)(i - 4) * 2 + h) * stride] = (int16_t)pred;
        }
    }
}

/* Decodes one block of `type` (0 PCM8, 1 PCM16, 2 ADPCM) to `frames` samples. */
static void decode_block(int type, const uint8_t *in, uint32_t len, int16_t *out, size_t stride,
                         uint32_t frames) {
    if (type == 2) {
        adpcm_block(in, len, out, stride);
    } else if (type == 1) {
        for (uint32_t i = 0; i < frames; i++) out[i * stride] = (int16_t)rd16(in + i * 2);
    } else {
        for (uint32_t i = 0; i < frames; i++) out[i * stride] = (int16_t)((int8_t)in[i] * 256);
    }
}

static uint32_t block_frames(int type, uint32_t len) {
    return type == 2 ? (len > 4 ? (len - 4) * 2 : 0) : type == 1 ? len / 2 : len;
}

/* ---- SWAR ---- */

static NdsResult swar_table(const uint8_t *d, size_t n, uint32_t *count) {
    if (!d || n < 0x2C || memcmp(d, "SWAR", 4) != 0 || memcmp(d + 0x10, "DATA", 4) != 0)
        return NDS_ERR_FORMAT;
    uint32_t c = rd32(d + 0x38);
    if (c > MAX_RECORDS || !in_range(0x3C, (uint64_t)c * 4, n)) return NDS_ERR_FORMAT;
    *count = c;
    return NDS_OK;
}

NdsResult nds_swar_count(const uint8_t *d, size_t n, uint32_t *count) {
    if (!count) return NDS_ERR_ARG;
    return swar_table(d, n, count);
}

NdsResult nds_swar_decode(const uint8_t *d, size_t n, uint32_t index, NdsAudio *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    uint32_t count;
    NdsResult r = swar_table(d, n, &count);
    if (r != NDS_OK) return r;
    if (index >= count) return NDS_ERR_NOTFOUND;

    uint32_t off = rd32(d + 0x3C + index * 4);
    if (off == 0 || !in_range(off, 12, n)) return NDS_ERR_FORMAT;
    int type = d[off];
    uint32_t loop_words = rd16(d + off + 6), len_words = rd32(d + off + 8);
    if (type > 2) return NDS_ERR_FORMAT;
    uint64_t bytes = ((uint64_t)loop_words + len_words) * 4;
    if (bytes > n - off - 12) bytes = n - off - 12;
    const uint8_t *data = d + off + 12;

    uint32_t frames = block_frames(type, (uint32_t)bytes);
    if (frames == 0 || frames > MAX_FRAMES) return NDS_ERR_FORMAT;
    out->pcm = malloc((size_t)frames * 2);
    if (!out->pcm) return NDS_ERR_NOMEM;
    decode_block(type, data, (uint32_t)bytes, out->pcm, 1, frames);

    out->rate = rd16(d + off + 2);
    out->channels = 1;
    out->frames = frames;
    out->loop = d[off + 1] != 0 && len_words > 0;
    uint32_t start = type == 2 ? (loop_words > 0 ? (loop_words * 4 - 4) * 2 : 0)
                               : loop_words * 4 / (type == 1 ? 2 : 1);
    out->loop_start = start < frames ? start : 0;
    return NDS_OK;
}

/* ---- STRM ---- */

NdsResult nds_strm_decode(const uint8_t *d, size_t n, NdsAudio *out) {
    if (!out) return NDS_ERR_ARG;
    memset(out, 0, sizeof *out);
    if (!d || n < 0x48 || memcmp(d, "STRM", 4) != 0 || memcmp(d + 0x10, "HEAD", 4) != 0)
        return NDS_ERR_FORMAT;

    int type = d[0x18];
    unsigned chans = d[0x1A];
    uint32_t rate = rd16(d + 0x1C), loop_off = rd32(d + 0x20), samples = rd32(d + 0x24);
    uint32_t data_off = rd32(d + 0x28), blocks = rd32(d + 0x2C), blen = rd32(d + 0x30);
    uint32_t last_len = rd32(d + 0x38), last_samples = rd32(d + 0x3C);
    if (type > 2 || chans < 1 || chans > 2 || blocks == 0 || samples == 0 || samples > MAX_FRAMES ||
        blen == 0)
        return NDS_ERR_FORMAT;
    uint64_t need = (uint64_t)(blocks - 1) * blen * chans + (uint64_t)last_len * chans;
    if (!in_range(data_off, need, n)) return NDS_ERR_FORMAT;

    out->pcm = calloc((size_t)samples * chans, 2);
    if (!out->pcm) return NDS_ERR_NOMEM;

    uint32_t frame = 0;
    const uint8_t *p = d + data_off;
    for (uint32_t b = 0; b < blocks && frame < samples; b++) {
        int last = b == blocks - 1;
        uint32_t len = last ? last_len : blen;
        uint32_t want = last ? last_samples : block_frames(type, len);
        if (want > samples - frame) want = samples - frame;
        for (unsigned c = 0; c < chans; c++) {
            uint32_t have = block_frames(type, len);
            int16_t *dst = out->pcm + (size_t)frame * chans + c;
            if (have > want) {
                /* decode into the stream; the extra tail would overrun the buffer */
                int16_t *tmp = malloc((size_t)have * 2);
                if (!tmp) { nds_audio_free(out); return NDS_ERR_NOMEM; }
                decode_block(type, p, len, tmp, 1, have);
                for (uint32_t i = 0; i < want; i++) dst[i * chans] = tmp[i];
                free(tmp);
            } else {
                decode_block(type, p, len, dst, chans, have);
            }
            p += len;
        }
        frame += want;
    }
    out->rate = rate;
    out->channels = (uint16_t)chans;
    out->frames = samples;
    out->loop = d[0x19] != 0;
    out->loop_start = loop_off < samples ? loop_off : 0;
    return NDS_OK;
}

/* ---- WAV ---- */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

NdsResult nds_write_wav(const char *path, const NdsAudio *a) {
    if (!path || !a || !a->pcm || !a->frames || !a->channels || !a->rate) return NDS_ERR_ARG;
    uint64_t data_bytes = (uint64_t)a->frames * a->channels * 2;
    if (data_bytes > 0xFFFFFFFFull - 100) return NDS_ERR_FORMAT;
    uint32_t smpl = a->loop ? 8 + 60 : 0;

    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, (uint32_t)(36 + data_bytes + smpl));
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, a->channels);
    put32(h + 24, a->rate);
    put32(h + 28, a->rate * a->channels * 2);
    put16(h + 32, a->channels * 2u);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, (uint32_t)data_bytes);

    FILE *f = fopen(path, "wb");
    if (!f) return NDS_ERR_IO;
    int ok = fwrite(h, 1, 44, f) == 44;
    size_t total = (size_t)a->frames * a->channels;
    for (size_t i = 0; ok && i < total; i += 4096) {
        uint8_t buf[8192];
        size_t cnt = total - i < 4096 ? total - i : 4096;
        for (size_t k = 0; k < cnt; k++) put16(buf + k * 2, (uint16_t)a->pcm[i + k]);
        ok = fwrite(buf, 2, cnt, f) == cnt;
    }
    if (ok && a->loop) {
        uint8_t s[68] = {0};
        memcpy(s, "smpl", 4);
        put32(s + 4, 60);
        put32(s + 16, (uint32_t)(1000000000u / a->rate));
        put32(s + 20, 60);
        put32(s + 36, 1);
        put32(s + 52, a->loop_start);
        put32(s + 56, a->frames - 1);
        ok = fwrite(s, 1, sizeof s, f) == sizeof s;
    }
    if (fclose(f) != 0) ok = 0;
    return ok ? NDS_OK : NDS_ERR_IO;
}
