/*
 * olivia_demod.h - MFSK demodulation layer of Olivia (receive)
 *
 * Written from scratch from the public Olivia specification.
 * The SP9VRC reference code is GPL-2 and was NOT copied.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 * This module ONLY demodulates at a given synchronisation: it turns
 * audio samples into soft decisions. Finding the block boundary and the
 * frequency offset is the job of the synchroniser (olivia_rx.c).
 */

#ifndef OLIVIA_DEMOD_H
#define OLIVIA_DEMOD_H

#include <stdint.h>
#include <stddef.h>

#include "olivia.h"

/* FFT slices per symbol. Gives the time synchronisation a resolution of
 * 1/4 symbol. */
#define OLIVIA_SLICES_PER_SYMBOL 4

typedef struct {
    olivia_cfg_t cfg;

    int sample_rate;
    int symbol_separ;
    int symbol_len;       /* FFT and window length                      */
    int slice_separ;      /* samples between slices = symbol_separ / 4  */
    int carrier_separ;    /* tone spacing in bins (4)                   */
    int first_carrier;    /* bin of the lowest tone at zero offset      */

    int margin;           /* tuning margin in TONES                     */
    int width;            /* number of bins in the result (resolution
                             1 bin = 1/4 of the tone spacing)           */
    int offsets;          /* number of frequency hypotheses             */
    int offset_step;      /* hypothesis step in bins                    */


    float *shape;         /* window, the same as the modulator's        */
    float *tap;           /* circular input buffer                      */
    int tap_ptr;
    int wrap_mask;

    float *fft_re;
    float *fft_im;
    float *cos_tab;
    float *sin_tab;
    int *bit_rev;
} olivia_demod_t;

int olivia_demod_init(olivia_demod_t *d, int tones, int bandwidth,
                      int sample_rate, double lower_edge, int margin);

void olivia_demod_free(olivia_demod_t *d);

/* Samples consumed by one slice. */
int olivia_demod_slice_samples(const olivia_demod_t *d);

/*
 * Process one slice: take slice_separ samples, return the energy in
 * energy[] of length d->width. Index 0 is the bin margin tones below
 * tone zero.
 */
void olivia_demod_slice(olivia_demod_t *d, const float *in, float *energy);

/*
 * Turn the energy of one symbol into soft decisions.
 * energy      : array from olivia_demod_slice
 * freq_offset : shift in BINS (1 bin = 1/4 of the tone spacing), from
 *               -margin*4 to +margin*4. A resolution finer than a tone is
 *               needed: transmitters differ in where they put the first
 *               carrier by half a spacing.
 * reverse     : 1 = reversed tone order. The receiver tries both
 *               orientations, because the spectrum looks IDENTICAL either
 *               way and there is no other way to tell.
 * soft        : cfg.bits values; POSITIVE = bit 0, NEGATIVE = bit 1,
 *               normalised to -1..+1
 */
void olivia_demod_soft(const olivia_demod_t *d, const float *energy,
                       int freq_offset, int reverse, float *soft);

/* Convert a shift in FFT bins to Hz. */
double olivia_demod_offset_hz(const olivia_demod_t *d, int offset_bins);

#endif /* OLIVIA_DEMOD_H */
