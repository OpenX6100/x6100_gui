/*
 * olivia_demod.c - MFSK demodulation layer of Olivia
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 * Part of OpenX6100 (github.com/OpenX6100)
 *
 * How it works:
 *
 *  - the input goes through the same window as in the transmitter and an
 *    FFT of length symbol_len; a bin is 1/4 of the tone spacing wide, so
 *    tuning tolerates not hitting a tone exactly,
 *  - FFT slices are computed four times per symbol, which gives the time
 *    synchronisation a resolution of 1/4 symbol,
 *  - the output is SOFT DECISIONS for each of the log2(N) bits, not a hard
 *    choice of the strongest tone. Every tone adds its energy to all the
 *    bits, with a sign depending on that bit of its Gray-decoded number.
 *    That is the essence of Olivia: the FEC layer gets the confidence,
 *    not a finished decision.
 *
 * About the energy: the fourth power of the amplitude is used (|X|^2
 * squared once more), as the reference implementation does. A strong
 * tone then dominates the weak ones and the soft decision is sharper.
 * Measured: without it the sensitivity threshold is about 2 dB worse.
 */

#include "olivia_demod.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const double shape_coeff[4] = {
    1.0, 2.1373197349, 1.1207588117, -0.0165609232
};

static int is_pow2(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

int olivia_demod_init(olivia_demod_t *d, int tones, int bandwidth,
                      int sample_rate, double lower_edge, int margin)
{
    int i, f, n, bits, j;

    memset(d, 0, sizeof(*d));

    if (olivia_cfg_init(&d->cfg, tones, bandwidth) < 0)
        return -1;

    if (sample_rate <= 0 || sample_rate % bandwidth != 0)
        return -1;

    if (margin < 0)
        return -1;

    d->sample_rate = sample_rate;
    /* As in the modulator: the window and the FFT span 4 symbol
     * separations, 1024 samples at 32/1000. */
    d->carrier_separ = 4;
    d->margin = margin;

    d->symbol_separ = (sample_rate / bandwidth) * tones;
    d->symbol_len = d->symbol_separ * d->carrier_separ;
    d->slice_separ = d->symbol_separ / OLIVIA_SLICES_PER_SYMBOL;

    if (!is_pow2(d->symbol_len) || d->slice_separ < 1)
        return -1;

    d->wrap_mask = d->symbol_len - 1;

    /* As in the modulator: tone 0 half a spacing above the lower edge. */
    d->first_carrier =
        (int)floor((lower_edge / sample_rate) * d->symbol_len + 0.5) +
        d->carrier_separ / 2;

    /*
     * Keep the energy of EVERY bin in the tuning window, not only those
     * exactly on the tones, so the frequency hypotheses can step one bin,
     * i.e. a quarter of the tone spacing.
     */
    d->width = (tones + 2 * margin) * d->carrier_separ;
    /*
     * Hypotheses every bin, a quarter of the tone spacing. Half a spacing
     * would halve the cost, but implementations differ by exactly that in
     * where they put the first carrier, and a hypothesis grid that only
     * just reaches it loses sensitivity.
     */
    d->offset_step = 1;   /* every bin = a quarter of the tone spacing */
    d->offsets = (2 * margin * d->carrier_separ) / d->offset_step + 1;

    if (d->first_carrier - margin * d->carrier_separ < 0)
        return -1;
    if (d->first_carrier + (tones + margin) * d->carrier_separ >=
        d->symbol_len / 2)
        return -1;

    n = d->symbol_len;

    d->shape = malloc(sizeof(float) * n);
    d->tap = malloc(sizeof(float) * n);
    d->fft_re = malloc(sizeof(float) * n);
    d->fft_im = malloc(sizeof(float) * n);
    d->cos_tab = malloc(sizeof(float) * (n / 2));
    d->sin_tab = malloc(sizeof(float) * (n / 2));
    d->bit_rev = malloc(sizeof(int) * n);

    if (!d->shape || !d->tap || !d->fft_re || !d->fft_im ||
        !d->cos_tab || !d->sin_tab || !d->bit_rev) {
        olivia_demod_free(d);
        return -1;
    }

    /* The same window as in the transmitter. */
    for (i = 0; i < n; i++)
        d->shape[i] = (float)shape_coeff[0];

    for (f = 1; f < 4; f++) {
        double ampl = shape_coeff[f];

        if (f & 1)
            ampl = -ampl;
        for (i = 0; i < n; i++)
            d->shape[i] += (float)(ampl * cos((2.0 * M_PI * f * i) / n));
    }

    for (i = 0; i < n; i++)
        d->tap[i] = 0.0f;
    d->tap_ptr = 0;

    for (i = 0; i < n / 2; i++) {
        d->cos_tab[i] = (float)cos((2.0 * M_PI * i) / n);
        d->sin_tab[i] = (float)sin((2.0 * M_PI * i) / n);
    }

    bits = 0;
    for (i = n; i > 1; i >>= 1)
        bits++;

    for (i = 0; i < n; i++) {
        int r = 0;

        for (j = 0; j < bits; j++)
            if (i & (1 << j))
                r |= 1 << (bits - 1 - j);
        d->bit_rev[i] = r;
    }

    return 0;
}

void olivia_demod_free(olivia_demod_t *d)
{
    free(d->shape);
    free(d->tap);
    free(d->fft_re);
    free(d->fft_im);
    free(d->cos_tab);
    free(d->sin_tab);
    free(d->bit_rev);
    memset(d, 0, sizeof(*d));
}

double olivia_demod_offset_hz(const olivia_demod_t *d, int offset_bins)
{
    return (double)offset_bins * d->sample_rate / d->symbol_len;
}

int olivia_demod_slice_samples(const olivia_demod_t *d)
{
    return d->slice_separ;
}

/* In-place radix-2 FFT, natural order on output. */
static void fft(olivia_demod_t *d)
{
    int n = d->symbol_len;
    int step, i, j, k;

    for (i = 0; i < n; i++) {
        int r = d->bit_rev[i];

        if (r > i) {
            float t = d->fft_re[i];

            d->fft_re[i] = d->fft_re[r];
            d->fft_re[r] = t;
            t = d->fft_im[i];
            d->fft_im[i] = d->fft_im[r];
            d->fft_im[r] = t;
        }
    }

    for (step = 1; step < n; step <<= 1) {
        int jump = step << 1;
        int tw_step = n / jump;

        for (k = 0; k < step; k++) {
            float wr = d->cos_tab[k * tw_step];
            float wi = -d->sin_tab[k * tw_step];

            for (i = k; i < n; i += jump) {
                j = i + step;
                {
                    float tr = wr * d->fft_re[j] - wi * d->fft_im[j];
                    float ti = wr * d->fft_im[j] + wi * d->fft_re[j];

                    d->fft_re[j] = d->fft_re[i] - tr;
                    d->fft_im[j] = d->fft_im[i] - ti;
                    d->fft_re[i] += tr;
                    d->fft_im[i] += ti;
                }
            }
        }
    }
}

void olivia_demod_slice(olivia_demod_t *d, const float *in, float *energy)
{
    int n = d->symbol_len;
    int i, ptr, idx;

    /* push the new samples in */
    for (i = 0; i < d->slice_separ; i++) {
        d->tap[d->tap_ptr] = in[i];
        d->tap_ptr += 1;
        d->tap_ptr &= d->wrap_mask;
    }

    /* window + FFT */
    ptr = d->tap_ptr;
    for (i = 0; i < n; i++) {
        d->fft_re[i] = d->tap[ptr] * d->shape[i];
        d->fft_im[i] = 0.0f;
        ptr += 1;
        ptr &= d->wrap_mask;
    }

    fft(d);

    /* energy in every bin of the tuning window */
    for (idx = 0; idx < d->width; idx++) {
        int bin = d->first_carrier - d->margin * d->carrier_separ + idx;
        float re = d->fft_re[bin];
        float im = d->fft_im[bin];

        energy[idx] = re * re + im * im;
    }
}

void olivia_demod_soft(const olivia_demod_t *d, const float *energy,
                       int freq_offset, int reverse, float *soft)
{
    int bits = d->cfg.bits;
    int tone, b;
    float total = 0.0f;

    for (b = 0; b < bits; b++)
        soft[b] = 0.0f;

    for (tone = 0; tone < d->cfg.tones; tone++) {
        int phys = reverse ? (d->cfg.tones - 1 - tone) : tone;
        int idx = (d->margin + phys) * d->carrier_separ + freq_offset;
        float e;
        uint8_t symbol;

        if (idx < 0 || idx >= d->width)
            continue;

        e = energy[idx];
        e = e * e;              /* fourth power of the amplitude */
        total += e;

        symbol = olivia_tone_to_symbol((uint8_t)tone);

        for (b = 0; b < bits; b++) {
            if ((symbol >> b) & 1)
                soft[b] -= e;   /* a set bit pulls negative */
            else
                soft[b] += e;
        }
    }

    if (total > 0.0f) {
        for (b = 0; b < bits; b++)
            soft[b] /= total;
    }
}
