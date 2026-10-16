/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  WSPR modulator
 */

#include "wspr_mod.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int wspr_mod_init(wspr_mod_t *mod, int sample_rate, double base_freq_hz)
{
    double highest_tone;

    if (sample_rate <= 0 || base_freq_hz <= 0.0)
        return -1;

    highest_tone = base_freq_hz + 3.0 * WSPR_TONE_SPACING_HZ;
    if (highest_tone >= sample_rate / 2.0)
        return -1;                  /* beyond Nyquist */

    memset(mod, 0, sizeof(*mod));
    mod->sample_rate = sample_rate;
    mod->base_freq_hz = base_freq_hz;
    mod->phase = 0.0;
    return 0;
}

long wspr_mod_total_samples(const wspr_mod_t *mod)
{
    return (long)llround(WSPR_TX_DURATION_S * mod->sample_rate);
}

long wspr_mod_max_symbol_samples(const wspr_mod_t *mod)
{
    /* One extra sample of headroom covers boundary rounding. */
    return (long)(WSPR_SYMBOL_PERIOD_S * mod->sample_rate) + 2;
}

long wspr_mod_symbol(wspr_mod_t *mod, int k, uint8_t symbol, float *out)
{
    double sr = (double)mod->sample_rate;
    double freq, w;
    long start, end, n, i;

    if (!mod || !out || k < 0 || k >= WSPR_SYMBOLS)
        return 0;

    freq = mod->base_freq_hz + (double)(symbol & 3) * WSPR_TONE_SPACING_HZ;
    w = 2.0 * M_PI * freq / sr;

    /* Boundaries from the absolute index, so rounding cannot accumulate
     * across 162 symbols. */
    start = (long)llround((double)k * WSPR_SYMBOL_PERIOD_S * sr);
    end = (long)llround((double)(k + 1) * WSPR_SYMBOL_PERIOD_S * sr);
    n = end - start;

    mod->phase = fmod(mod->phase, 2.0 * M_PI);

    for (i = 0; i < n; i++) {
        out[i] = (float)sin(mod->phase);
        mod->phase += w;
    }

    return n;
}

long wspr_mod_generate(wspr_mod_t *mod, const uint8_t symbols[WSPR_SYMBOLS],
                       float *out)
{
    long written = 0;
    int k;

    mod->phase = 0.0;

    for (k = 0; k < WSPR_SYMBOLS; k++)
        written += wspr_mod_symbol(mod, k, symbols[k], out + written);

    return written;
}
