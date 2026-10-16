/*
 * olivia.h - FEC layer of Olivia MFSK (Walsh-Hadamard + interleave + scrambler)
 *
 * Written from scratch from the public Olivia specification (ARRL,
 * "The Draft Specification For The Olivia HF Transmission System").
 * The SP9VRC reference code is GPL-2 and was NOT copied - it was used
 * only to verify the results.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 */

#ifndef OLIVIA_H
#define OLIVIA_H

#include <stdint.h>
#include <stddef.h>

#define OLIVIA_BLOCK_SYMBOLS 64   /* Walsh vector length = symbols per block */
#define OLIVIA_MAX_BITS      8    /* max log2(tones), i.e. up to 256 tones   */

/* Scrambling sequence from the specification. */
#define OLIVIA_SCRAMBLER 0xE257E6D0291574ECULL

typedef struct {
    int tones;       /* 2,4,8,...,256                                */
    int bits;        /* log2(tones) - ASCII characters per block      */
    int bandwidth;   /* Hz: 125,250,500,1000,2000                     */
} olivia_cfg_t;

/* Fill in a configuration. Returns 0 when the parameters are valid, -1
 * when not. */
int olivia_cfg_init(olivia_cfg_t *cfg, int tones, int bandwidth);

/* Symbol rate [Bd] = bandwidth / tones. */
double olivia_baud(const olivia_cfg_t *cfg);

/* Block duration [s]. */
double olivia_block_period(const olivia_cfg_t *cfg);

/* ---- FEC layer ---- */

/*
 * Encode a block of cfg->bits 7-bit characters into 64 symbols (values
 * 0..tones-1, still WITHOUT the Gray code - the modulation layer does
 * that).
 * chars[]   : cfg->bits input characters
 * symbols[] : 64 output symbols
 */
void olivia_encode_block(const olivia_cfg_t *cfg,
                         const uint8_t *chars,
                         uint8_t *symbols);

/*
 * Decode a block from hard symbols.
 * symbols[] : 64 symbols (without Gray)
 * chars[]   : cfg->bits output characters
 * snr[]     : optional (may be NULL) - quality per character, FHT peak /
 *             RMS of the rest. Below about 3.0 decoding is unreliable.
 */
void olivia_decode_block_hard(const olivia_cfg_t *cfg,
                              const uint8_t *symbols,
                              uint8_t *chars,
                              float *snr);

/*
 * Decode a block from soft decisions - the version used on the radio.
 * soft : array [64][cfg->bits], soft[t*bits + b] is the soft value of bit
 *        b of symbol t. POSITIVE = bit is 0, NEGATIVE = bit is 1, the
 *        magnitude is the confidence. Zeros are allowed (no information).
 */
void olivia_decode_block_soft(const olivia_cfg_t *cfg,
                              const float *soft,
                              uint8_t *chars,
                              float *snr);

/* ---- Modulation layer: tone numbering ---- */

/* Symbol -> tone number (Gray code). Tone 0 = lowest frequency. */
uint8_t olivia_symbol_to_tone(uint8_t symbol);

/* Tone number -> symbol. */
uint8_t olivia_tone_to_symbol(uint8_t tone);

/* ---- Helpers (exposed for tests) ---- */

/* Fast Hadamard transform in place, length 64. Olivia's forward and
 * inverse pair - see olivia.c; neither is self-inverse. */
void olivia_fht_f(float *v);
void olivia_ifht_f(float *v);

#endif /* OLIVIA_H */
