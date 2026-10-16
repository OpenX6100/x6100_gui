/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  WSPR modulator: continuous phase 4-FSK
 *
 *  Unlike Olivia, this modulator does NOT need an integer number of
 *  samples per symbol. There are no FFT blocks on the transmit side,
 *  just a phase accumulator advanced in real time, so audio can be
 *  generated directly at any sample rate (12000 Hz for the X6100 player,
 *  for instance) with no resampler at all.
 *
 *  The frequency steps at each symbol boundary while the phase stays
 *  continuous. That is how simple hardware beacons (DDS, Si5351) work
 *  too, so this is not a simplification that costs compatibility - it
 *  is exactly what the specification calls for.
 */

#ifndef WSPR_MOD_H
#define WSPR_MOD_H

#include <stdint.h>
#include <stddef.h>

#include "wspr_encode.h"

/* Tone spacing and symbol rate are the same number, 12000/8192 Hz. */
#define WSPR_TONE_SPACING_HZ (12000.0 / 8192.0)
#define WSPR_SYMBOL_PERIOD_S (8192.0 / 12000.0)

/* Total transmission length: 162 symbols, about 110.6 s. */
#define WSPR_TX_DURATION_S ((double)WSPR_SYMBOLS * WSPR_SYMBOL_PERIOD_S)

typedef struct {
    int sample_rate;
    double base_freq_hz;    /* frequency of the lowest tone */
    double phase;           /* phase accumulator in radians */
} wspr_mod_t;

/*
 * Initialise. base_freq_hz is the audio frequency of the LOWEST tone
 * (symbol 0), not the band centre. WSPR only spans about 5.86 Hz so the
 * difference barely matters, but it must stay consistent with the
 * demodulator.
 *
 * Returns 0 on success, -1 on invalid parameters.
 */
int wspr_mod_init(wspr_mod_t *mod, int sample_rate, double base_freq_hz);

/* Samples the whole transmission will occupy, for buffer sizing. */
long wspr_mod_total_samples(const wspr_mod_t *mod);

/*
 * Generate the whole transmission from 162 symbols (0..3, from
 * wspr_encode). out must hold wspr_mod_total_samples() floats, roughly
 * in the range -1..+1. Returns the number of samples written.
 *
 * Convenient offline, but even at 12000 Hz the whole transmission is 1.3
 * million samples, so the radio uses wspr_mod_symbol() instead and
 * plays each symbol as it is produced.
 */
long wspr_mod_generate(wspr_mod_t *mod, const uint8_t symbols[WSPR_SYMBOLS],
                       float *out);

/* Longest a single symbol can be, for sizing an incremental buffer. */
long wspr_mod_max_symbol_samples(const wspr_mod_t *mod);

/*
 * Generate symbol number k (0..161) only, carrying the phase forward
 * from the previous call. Symbol boundaries are computed from the
 * absolute symbol index rather than by accumulating, so rounding cannot
 * drift over the 110 s transmission.
 *
 * Returns the number of samples written, or 0 on bad arguments.
 */
long wspr_mod_symbol(wspr_mod_t *mod, int k, uint8_t symbol, float *out);

#endif /* WSPR_MOD_H */
