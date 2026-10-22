/*
 * olivia_mod.h - MFSK modulation layer of Olivia (transmit)
 *
 * Written from scratch from the public Olivia specification.
 * The SP9VRC reference code is GPL-2 and was NOT copied.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 * Part of OpenX6100 (github.com/OpenX6100)
 */

#ifndef OLIVIA_MOD_H
#define OLIVIA_MOD_H

#include <stdint.h>
#include <stddef.h>

#include "olivia.h"

typedef struct {
    olivia_cfg_t cfg;

    int sample_rate;      /* Hz, 8000 on the radio                       */
    int symbol_separ;     /* samples between symbol starts               */
    int symbol_len;       /* length of the shaping window in samples     */
    int carrier_separ;    /* tone spacing in FFT bins (always 4)         */
    int first_carrier;    /* bin of the first (lowest) tone              */
    double lower_edge;    /* Hz, lower band edge                         */

    float *shape;         /* shaping window, symbol_len samples          */
    float *cosine;        /* cosine table, symbol_len samples            */
    float *tap;           /* circular overlap buffer, symbol_len samples */
    int tap_ptr;
    int wrap_mask;

    int phase;            /* current phase, unit = 1/symbol_len turn     */
    int phase_differ;     /* 1 = random +-90 deg between symbols (normal) */
    uint32_t rng;         /* generator for the random phase shift        */

    /* block buffer */
    uint8_t chars[OLIVIA_MAX_BITS];
    int char_count;
} olivia_mod_t;

/*
 * Initialise. lower_edge is the lower edge of the audio band in Hz
 * (typically 500; in fldigi it is the cursor frequency minus half the
 * bandwidth). Returns 0, or -1 for invalid parameters.
 */
int olivia_mod_init(olivia_mod_t *mod, int tones, int bandwidth,
                    int sample_rate, double lower_edge);

void olivia_mod_free(olivia_mod_t *mod);

/* Turn the random phase shift off. FOR COMPARISON TESTS ONLY - on air it
 * must stay on, or a repeated symbol becomes a pure tone. */
void olivia_mod_set_phase_differ(olivia_mod_t *mod, int on);

/* Set a deterministic seed for the phase generator - for tests. */
void olivia_mod_seed(olivia_mod_t *mod, uint32_t seed);

/*
 * Send one symbol. symbol_separ float samples, roughly -1..+1, go to out.
 * Returns the number of samples.
 */
int olivia_mod_symbol(olivia_mod_t *mod, uint8_t symbol, float *out);

/*
 * Send a whole block: cfg.bits characters -> 64 * symbol_separ samples.
 * Returns the number of samples.
 */
int olivia_mod_block(olivia_mod_t *mod, const uint8_t *chars, float *out);

/* Samples in one block. */
int olivia_mod_block_samples(const olivia_mod_t *mod);

/* Centre frequency of a tone in Hz - for the waterfall and tuning. */
double olivia_mod_tone_freq(const olivia_mod_t *mod, int tone);

#endif /* OLIVIA_MOD_H */
