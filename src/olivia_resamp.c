/*
 * olivia_resamp.c - rational resampler
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 * Part of OpenX6100 (github.com/OpenX6100)
 */

#include "olivia_resamp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int gcd_int(int a, int b)
{
    while (b) {
        int t = a % b;

        a = b;
        b = t;
    }
    return a;
}

int olivia_resamp_init(olivia_resamp_t *r, int in_rate, int out_rate)
{
    int g, p, t;
    double cutoff;

    memset(r, 0, sizeof(*r));

    if (in_rate <= 0 || out_rate <= 0)
        return -1;

    r->in_rate = in_rate;
    r->out_rate = out_rate;

    if (in_rate == out_rate) {
        r->phases = 1;
        r->step = 1;
        return 0;               /* pass-through, no tables */
    }

    g = gcd_int(in_rate, out_rate);
    r->phases = out_rate / g;   /* L */
    r->step = in_rate / g;      /* M */

    /*
     * A large L would mean a huge table (8000 -> 48000 is L=6, 11025 ->
     * 8000 would be L=320). Above 1024 refuse, rather than quietly eat
     * the memory.
     */
    if (r->phases > 1024)
        return -1;

    r->kernel = malloc(sizeof(float) * r->phases * OLIVIA_RESAMP_TAPS);
    if (!r->kernel)
        return -1;

    /*
     * Cut-off: half the lower of the two rates, with 10 percent left for
     * the filter slope.
     */
    cutoff = 0.45 * ((in_rate < out_rate) ? 1.0
                                          : (double)out_rate / in_rate);

    for (p = 0; p < r->phases; p++) {
        double frac = (double)p / r->phases;
        double sum = 0.0;

        for (t = 0; t < OLIVIA_RESAMP_TAPS; t++) {
            double x = (double)t - (OLIVIA_RESAMP_TAPS / 2 - 1) - frac;
            double arg = 2.0 * M_PI * cutoff * x;
            double sinc = (fabs(x) < 1e-9) ? (2.0 * cutoff)
                                           : sin(arg) / (M_PI * x);
            /* Blackman window */
            double wpos = (double)t / (OLIVIA_RESAMP_TAPS - 1);
            double w = 0.42 - 0.5 * cos(2.0 * M_PI * wpos) +
                       0.08 * cos(4.0 * M_PI * wpos);
            double v = sinc * w;

            r->kernel[p * OLIVIA_RESAMP_TAPS + t] = (float)v;
            sum += v;
        }

        /* unity DC gain in every phase */
        if (sum != 0.0) {
            for (t = 0; t < OLIVIA_RESAMP_TAPS; t++)
                r->kernel[p * OLIVIA_RESAMP_TAPS + t] /= (float)sum;
        }
    }

    r->hist_len = OLIVIA_RESAMP_TAPS * 2;
    while (r->hist_len & (r->hist_len - 1))
        r->hist_len++;
    r->hist = calloc(r->hist_len, sizeof(float));
    if (!r->hist) {
        free(r->kernel);
        r->kernel = NULL;
        return -1;
    }

    return 0;
}

void olivia_resamp_free(olivia_resamp_t *r)
{
    free(r->kernel);
    free(r->hist);
    r->kernel = NULL;
    r->hist = NULL;
}

void olivia_resamp_reset(olivia_resamp_t *r)
{
    if (r->hist)
        memset(r->hist, 0, sizeof(float) * r->hist_len);
    r->hist_pos = 0;
    r->in_count = 0;
    r->out_pos = 0;
}

int olivia_resamp_max_out(const olivia_resamp_t *r, int n)
{
    if (r->in_rate == r->out_rate)
        return n;
    return (int)((long long)n * r->phases / r->step) + 2;
}

int olivia_resamp_process(olivia_resamp_t *r, const float *in, int n,
                          float *out)
{
    int i, produced = 0;
    int mask;

    if (r->in_rate == r->out_rate) {
        memcpy(out, in, sizeof(float) * n);
        return n;
    }

    mask = r->hist_len - 1;

    for (i = 0; i < n; i++) {
        r->hist[r->hist_pos] = in[i];
        r->hist_pos = (r->hist_pos + 1) & mask;
        r->in_count++;

        /*
         * Output sample k lies at input position k * step / phases.
         * Emit every one that can be computed already, i.e. whose
         * whole neighbourhood is in.
         */
        for (;;) {
            long long num = r->out_pos * r->step;
            long long base = num / r->phases;
            int phase = (int)(num % r->phases);
            int t;
            float acc = 0.0f;
            int idx;

            /* needs samples base - TAPS/2 + 1 .. base + TAPS/2 */
            if (base + OLIVIA_RESAMP_TAPS / 2 >= r->in_count)
                break;

            idx = (int)((r->hist_pos - (r->in_count - base) -
                         (OLIVIA_RESAMP_TAPS / 2 - 1)) & mask);

            for (t = 0; t < OLIVIA_RESAMP_TAPS; t++) {
                acc += r->hist[idx] *
                       r->kernel[phase * OLIVIA_RESAMP_TAPS + t];
                idx = (idx + 1) & mask;
            }

            out[produced++] = acc;
            r->out_pos++;
        }
    }

    return produced;
}
