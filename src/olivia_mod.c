/*
 * olivia_mod.c - MFSK modulation layer of Olivia
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 * Part of OpenX6100 (github.com/OpenX6100)
 *
 * How it works:
 *
 *  - symbols start every symbol_separ samples, but each symbol lasts
 *    symbol_len = 4 * symbol_separ samples, so consecutive symbols
 *    overlap by 75%. They are summed in a circular buffer.
 *  - the shaping window is given by coefficients in the frequency domain
 *    (4 cosine terms), so the spectrum of a symbol falls off fast and no
 *    energy leaves the nominal band.
 *  - the phase is NOT continuous between symbols: after every symbol a
 *    random +-90 degree shift is added, so that a repeated symbol does not
 *    become a pure tone.
 *  - tone number = gray(symbol), tone 0 is the lowest frequency.
 */

#include "olivia_mod.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Symbol shape described in the frequency domain.
 * shape[t] = c0 - c1*cos(2*pi*t/L) + c2*cos(4*pi*t/L) - c3*cos(6*pi*t/L)
 * The coefficients make the window start and end at zero.
 */
static const double shape_coeff[4] = {
    1.0, 2.1373197349, 1.1207588117, -0.0165609232
};

static int is_pow2(int v)
{
    return v > 0 && (v & (v - 1)) == 0;
}

int olivia_mod_init(olivia_mod_t *mod, int tones, int bandwidth,
                    int sample_rate, double lower_edge)
{
    int i, f;
    double scale;

    memset(mod, 0, sizeof(*mod));

    if (olivia_cfg_init(&mod->cfg, tones, bandwidth) < 0)
        return -1;

    if (sample_rate <= 0 || sample_rate % bandwidth != 0)
        return -1;

    mod->sample_rate = sample_rate;
    /*
     * The shaping window is carrier_separ symbol separations long: 1024
     * samples at 32/1000 and 8000 Hz. The ARRL draft says 512; the
     * reference code computes 4x, and that is what works on air with
     * fldigi. If the two ends disagree, the receiver's analysis window
     * spans neighbouring symbols and the soft decisions turn into noise.
     */
    mod->carrier_separ = 4;
    mod->lower_edge = lower_edge;

    /* Symbol separation chosen so that the rate = bandwidth / tones. */
    mod->symbol_separ = (sample_rate / bandwidth) * tones;
    mod->symbol_len = mod->symbol_separ * mod->carrier_separ;

    if (!is_pow2(mod->symbol_len))
        return -1;

    mod->wrap_mask = mod->symbol_len - 1;

    /*
     * Bin of the first tone. A bin is sample_rate / symbol_len wide and
     * the tones sit carrier_separ bins apart.
     *
     * Tone 0 lies half a tone spacing above the lower band edge, so the
     * tones are centred in the nominal band. This placement was verified
     * on air against fldigi. Half a spacing off (15.625 Hz at 32/1000)
     * each tone's energy splits evenly between two FFT bins and the
     * decoder sees nothing but noise - that mistake did happen.
     */
    mod->first_carrier =
        (int)floor((lower_edge / sample_rate) * mod->symbol_len + 0.5) +
        mod->carrier_separ / 2;

    if (mod->first_carrier < mod->carrier_separ / 2)
        return -1;

    /* The highest tone must stay below half the sample rate. */
    if (mod->first_carrier + tones * mod->carrier_separ >= mod->symbol_len / 2)
        return -1;

    mod->shape = malloc(sizeof(float) * mod->symbol_len);
    mod->cosine = malloc(sizeof(float) * mod->symbol_len);
    mod->tap = malloc(sizeof(float) * mod->symbol_len);

    if (!mod->shape || !mod->cosine || !mod->tap) {
        olivia_mod_free(mod);
        return -1;
    }

    for (i = 0; i < mod->symbol_len; i++)
        mod->cosine[i] = (float)cos((2.0 * M_PI * i) / mod->symbol_len);

    /* Build the window from the sum of cosines. */
    for (i = 0; i < mod->symbol_len; i++)
        mod->shape[i] = (float)shape_coeff[0];

    for (f = 1; f < 4; f++) {
        double ampl = shape_coeff[f];
        int phase = 0;

        if (f & 1)
            ampl = -ampl;

        for (i = 0; i < mod->symbol_len; i++) {
            mod->shape[i] += (float)(ampl * mod->cosine[phase]);
            phase += f;
            if (phase >= mod->symbol_len)
                phase -= mod->symbol_len;
        }
    }

    scale = 1.0 / (2.0 * mod->carrier_separ);
    for (i = 0; i < mod->symbol_len; i++)
        mod->shape[i] *= (float)scale;

    for (i = 0; i < mod->symbol_len; i++)
        mod->tap[i] = 0.0f;

    mod->tap_ptr = 0;
    mod->phase = 0;
    mod->rng = 1;
    mod->phase_differ = 1;
    mod->char_count = 0;

    return 0;
}

void olivia_mod_free(olivia_mod_t *mod)
{
    free(mod->shape);
    free(mod->cosine);
    free(mod->tap);
    mod->shape = NULL;
    mod->cosine = NULL;
    mod->tap = NULL;
}

void olivia_mod_set_phase_differ(olivia_mod_t *mod, int on)
{
    mod->phase_differ = on ? 1 : 0;
}

void olivia_mod_seed(olivia_mod_t *mod, uint32_t seed)
{
    mod->rng = seed ? seed : 1;
}

static int rng_bit(olivia_mod_t *mod)
{
    /* xorshift32 - deterministic, so that tests are repeatable */
    uint32_t x = mod->rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    mod->rng = x;
    return (int)(x & 1u);
}

double olivia_mod_tone_freq(const olivia_mod_t *mod, int tone)
{
    int bin = mod->first_carrier + mod->carrier_separ * tone;

    return (double)bin * mod->sample_rate / mod->symbol_len;
}

int olivia_mod_block_samples(const olivia_mod_t *mod)
{
    return OLIVIA_BLOCK_SYMBOLS * mod->symbol_separ;
}

/* Add one shaped symbol to the overlap buffer. */
static void add_symbol(olivia_mod_t *mod, int freq, int phase)
{
    int t;
    int ptr = mod->tap_ptr;

    for (t = 0; t < mod->symbol_len; t++) {
        mod->tap[ptr] += mod->cosine[phase] * mod->shape[t];
        phase += freq;
        phase &= mod->wrap_mask;
        ptr += 1;
        ptr &= mod->wrap_mask;
    }
}

int olivia_mod_symbol(olivia_mod_t *mod, uint8_t symbol, float *out)
{
    int tone = olivia_symbol_to_tone(symbol);
    int freq = mod->first_carrier + mod->carrier_separ * tone;
    int shift;
    int i;

    /*
     * The phase is reckoned at the centre of the symbol, so step back
     * half a window before adding it, and forward half a window plus the
     * symbol separation after.
     */
    shift = mod->symbol_separ / 2 - mod->symbol_len / 2;
    mod->phase = (mod->phase + freq * shift) & mod->wrap_mask;

    add_symbol(mod, freq, mod->phase);

    shift = mod->symbol_separ / 2 + mod->symbol_len / 2;
    mod->phase = (mod->phase + freq * shift) & mod->wrap_mask;

    /* Random +-90 degree shift. */
    if (mod->phase_differ) {
        int quarter = mod->symbol_len / 4;

        if (rng_bit(mod))
            quarter = -quarter;
        mod->phase = (mod->phase + quarter) & mod->wrap_mask;
    }

    /* Take out the finished samples and clear that part of the buffer. */
    for (i = 0; i < mod->symbol_separ; i++) {
        out[i] = mod->tap[mod->tap_ptr];
        mod->tap[mod->tap_ptr] = 0.0f;
        mod->tap_ptr += 1;
        mod->tap_ptr &= mod->wrap_mask;
    }

    return mod->symbol_separ;
}

int olivia_mod_block(olivia_mod_t *mod, const uint8_t *chars, float *out)
{
    uint8_t symbols[OLIVIA_BLOCK_SYMBOLS];
    int t;
    int n = 0;

    olivia_encode_block(&mod->cfg, chars, symbols);

    for (t = 0; t < OLIVIA_BLOCK_SYMBOLS; t++)
        n += olivia_mod_symbol(mod, symbols[t], out + n);

    return n;
}
