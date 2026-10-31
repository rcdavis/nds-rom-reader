#ifndef NDSSDAT_H
#define NDSSDAT_H

#include "ndsrom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NDS_SDAT_SEQ = 0,     /* SSEQ sequence (music/SE script) */
    NDS_SDAT_SEQARC = 1,  /* SSAR sequence archive */
    NDS_SDAT_BANK = 2,    /* SBNK instrument bank */
    NDS_SDAT_WAVEARC = 3, /* SWAR wave archive */
    NDS_SDAT_STRM = 7     /* STRM streamed audio */
} NdsSdatType;

typedef struct {
    NdsSdatType type;
    uint32_t index;        /* index within its record type */
    char *name;            /* from the symbol block, or generated */
    uint32_t file_id;
    const uint8_t *data;   /* points into the buffer passed to nds_sdat_open */
    uint32_t size;
} NdsSdatEntry;

typedef struct NdsSdat NdsSdat;

/* The buffer must outlive the NdsSdat. */
NdsResult nds_sdat_open(const uint8_t *data, size_t size, NdsSdat **out);
void nds_sdat_close(NdsSdat *s);
size_t nds_sdat_count(const NdsSdat *s);
const NdsSdatEntry *nds_sdat_at(const NdsSdat *s, size_t i);
const char *nds_sdat_type_name(NdsSdatType t);
const char *nds_sdat_type_ext(NdsSdatType t);

/* ---- Audio decoding ---- */

typedef struct {
    uint32_t rate;
    uint16_t channels;
    uint32_t frames;
    int16_t *pcm;          /* interleaved */
    int loop;
    uint32_t loop_start;   /* in frames */
} NdsAudio;

void nds_audio_free(NdsAudio *a);

/* SWAR wave archives hold several waves (SWAV). */
NdsResult nds_swar_count(const uint8_t *data, size_t size, uint32_t *count);
NdsResult nds_swar_decode(const uint8_t *data, size_t size, uint32_t index, NdsAudio *out);

/* STRM streams (PCM8/PCM16/IMA-ADPCM, mono or stereo). */
NdsResult nds_strm_decode(const uint8_t *data, size_t size, NdsAudio *out);

NdsResult nds_write_wav(const char *path, const NdsAudio *a);

#ifdef __cplusplus
}
#endif

#endif
