/*
 * olivia.c - FEC layer of Olivia MFSK
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 * Part of OpenX6100 (github.com/OpenX6100)
 *
 * How it works (from the ARRL specification):
 *
 *  - a block is 64 symbols; each symbol carries K = log2(N) bits, so a
 *    block carries K 7-bit ASCII characters,
 *  - every character becomes a 64-bit Walsh vector: a biorthogonal code,
 *    characters 0..63 are rows of the Hadamard matrix, characters 64..127
 *    the same rows negated,
 *  - the vector is scrambled with the sequence 0xE257E6D0291574EC,
 *    rotated right by 13 bits for each further character in the block,
 *  - the interleave is diagonal: bit (i + t) mod K of symbol t comes from
 *    the vector of character i. A corrupted symbol therefore damages
 *    exactly one bit in each of the K vectors.
 */

#include "olivia.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */

static int log2_exact(int v)
{
    int n = 0;
    if (v < 2)
        return -1;
    while ((v & 1) == 0) {
        v >>= 1;
        n++;
    }
    return (v == 1) ? n : -1;
}

int olivia_cfg_init(olivia_cfg_t *cfg, int tones, int bandwidth)
{
    int bits = log2_exact(tones);

    if (bits < 1 || bits > OLIVIA_MAX_BITS)
        return -1;

    if (bandwidth != 125 && bandwidth != 250 && bandwidth != 500 &&
        bandwidth != 1000 && bandwidth != 2000)
        return -1;

    cfg->tones = tones;
    cfg->bits = bits;
    cfg->bandwidth = bandwidth;
    return 0;
}

double olivia_baud(const olivia_cfg_t *cfg)
{
    return (double)cfg->bandwidth / (double)cfg->tones;
}

double olivia_block_period(const olivia_cfg_t *cfg)
{
    return (double)OLIVIA_BLOCK_SYMBOLS / olivia_baud(cfg);
}

/* ------------------------------------------------------------------ */
/* Gray code. Tone 0 is the lowest frequency, tones go upwards.         */

uint8_t olivia_symbol_to_tone(uint8_t symbol)
{
    return (uint8_t)(symbol ^ (symbol >> 1));
}

uint8_t olivia_tone_to_symbol(uint8_t tone)
{
    tone ^= (uint8_t)(tone >> 4);
    tone ^= (uint8_t)(tone >> 2);
    tone ^= (uint8_t)(tone >> 1);
    return tone;
}

/* ------------------------------------------------------------------ */
/* Fast Hadamard transform, natural order, length 64.                   */

/*
 * NOTE: Olivia does not use the textbook Hadamard transform. The forward
 * butterfly is (a,b) -> (a+b, b-a), not (a+b, a-b), and the inverse runs
 * the stages in the opposite order giving (a,b) -> (a-b, a+b). The two
 * are inverses of each other up to a scale of 64, but NEITHER is
 * self-inverse. Matched to the SP9VRC reference code, because on-air
 * compatibility of the code words depends on it - do not "fix" it into
 * the textbook version.
 */

void olivia_fht_f(float *v)
{
    int step, i, j;

    for (step = 1; step < OLIVIA_BLOCK_SYMBOLS; step <<= 1) {
        for (i = 0; i < OLIVIA_BLOCK_SYMBOLS; i += (step << 1)) {
            for (j = i; j < i + step; j++) {
                float a = v[j];
                float b = v[j + step];
                v[j] = b + a;
                v[j + step] = b - a;
            }
        }
    }
}

void olivia_ifht_f(float *v)
{
    int step, i, j;

    for (step = OLIVIA_BLOCK_SYMBOLS / 2; step; step >>= 1) {
        for (i = 0; i < OLIVIA_BLOCK_SYMBOLS; i += (step << 1)) {
            for (j = i; j < i + step; j++) {
                float a = v[j];
                float b = v[j + step];
                v[j] = a - b;
                v[j + step] = a + b;
            }
        }
    }
}

/*
 * Build the Walsh vector of a character and scramble it straight away.
 * out[] gets +1.0 / -1.0.
 */
static void walsh_vector(uint8_t chr, int char_index, float *out)
{
    uint64_t code = OLIVIA_SCRAMBLER;
    int shift = (char_index * 13) & (OLIVIA_BLOCK_SYMBOLS - 1);
    int t;

    chr &= 0x7f;

    for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++)
        out[t] = 0.0f;

    /* Biorthogonal code: below 64 a Hadamard row, from 64 its negation. */
    if (chr < OLIVIA_BLOCK_SYMBOLS)
        out[chr] = 1.0f;
    else
        out[chr - OLIVIA_BLOCK_SYMBOLS] = -1.0f;

    olivia_ifht_f(out);

    /* Scrambling: bit (shift + t) mod 64 of the sequence flips sample t. */
    for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++) {
        int bit = (shift + t) & (OLIVIA_BLOCK_SYMBOLS - 1);
        if ((code >> bit) & 1ULL)
            out[t] = -out[t];
    }
}

/* ------------------------------------------------------------------ */

void olivia_encode_block(const olivia_cfg_t *cfg,
                         const uint8_t *chars,
                         uint8_t *symbols)
{
    float vec[OLIVIA_BLOCK_SYMBOLS];
    int i, t;

    for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++)
        symbols[t] = 0;

    for (i = 0; i < cfg->bits; i++) {
        walsh_vector(chars[i], i, vec);

        for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++) {
            /* diagonal interleave */
            int bit = (i + t) % cfg->bits;
            /* a negative value in the Walsh vector means the bit is set */
            if (vec[t] < 0.0f)
                symbols[t] |= (uint8_t)(1u << bit);
        }
    }
}

/* ------------------------------------------------------------------ */

/*
 * Common decoder core. soft[t*bits + b] > 0 means bit = 0.
 */
static void decode_core(const olivia_cfg_t *cfg,
                        const float *soft,
                        uint8_t *chars,
                        float *snr)
{
    int i, t;

    for (i = 0; i < cfg->bits; i++) {
        float vec[OLIVIA_BLOCK_SYMBOLS];
        uint64_t code = OLIVIA_SCRAMBLER;
        int shift = (i * 13) & (OLIVIA_BLOCK_SYMBOLS - 1);
        float peak = 0.0f;
        float sqr_sum = 0.0f;
        int peak_pos = 0;
        float rest;

        /* de-interleave + descramble */
        for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++) {
            int bit = (i + t) % cfg->bits;
            int cbit = (shift + t) & (OLIVIA_BLOCK_SYMBOLS - 1);
            float s = soft[t * cfg->bits + bit];

            if ((code >> cbit) & 1ULL)
                s = -s;
            vec[t] = s;
        }

        olivia_fht_f(vec);

        for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++) {
            float s = vec[t];
            sqr_sum += s * s;
            if (fabsf(s) > fabsf(peak)) {
                peak = s;
                peak_pos = t;
            }
        }

        /* The sign of the peak decides which half of the biorthogonal code. */
        chars[i] = (uint8_t)(peak > 0.0f ? peak_pos
                                         : peak_pos + OLIVIA_BLOCK_SYMBOLS);

        if (snr) {
            rest = sqr_sum - peak * peak;
            /*
             * A zero remainder does NOT mean perfect confidence - it means
             * no information. That is what silence at the input looks like:
             * all soft decisions are zero, the FHT puts everything in bin
             * 0 and the remainder comes out zero. Returning a high value
             * then gave a false lock showing "100%" and no text.
             */
            if (rest <= 1e-12f || sqr_sum <= 1e-12f) {
                snr[i] = 0.0f;
            } else {
                rest = sqrtf(rest / (float)(OLIVIA_BLOCK_SYMBOLS - 1));
                snr[i] = fabsf(peak) / rest;
            }
        }
    }
}

void olivia_decode_block_soft(const olivia_cfg_t *cfg,
                              const float *soft,
                              uint8_t *chars,
                              float *snr)
{
    decode_core(cfg, soft, chars, snr);
}

void olivia_decode_block_hard(const olivia_cfg_t *cfg,
                              const uint8_t *symbols,
                              uint8_t *chars,
                              float *snr)
{
    float soft[OLIVIA_BLOCK_SYMBOLS * OLIVIA_MAX_BITS];
    int t, b;

    for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++) {
        for (b = 0; b < cfg->bits; b++) {
            int set = (symbols[t] >> b) & 1;
            soft[t * cfg->bits + b] = set ? -1.0f : 1.0f;
        }
    }

    decode_core(cfg, soft, chars, snr);
}
