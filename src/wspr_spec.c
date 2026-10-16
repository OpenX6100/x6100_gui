/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  WSPR spectrum for the waterfall display
 */

#include "wspr_spec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Radix-2 FFT                                                         */

static int is_pow2(int n)
{
    return n > 0 && (n & (n - 1)) == 0;
}

int wspr_spec_init(wspr_spec_t *s)
{
    int n = WSPR_SPEC_BINS;
    int i, j, bits = 0;

    if (!s || !is_pow2(n))
        return -1;

    memset(s, 0, sizeof(*s));
    s->n = n;

    while ((1 << bits) < n)
        bits++;

    s->rev = (int *)malloc(sizeof(int) * n);
    s->cos_t = (float *)malloc(sizeof(float) * (n / 2));
    s->sin_t = (float *)malloc(sizeof(float) * (n / 2));
    s->win = (float *)malloc(sizeof(float) * n);

    if (!s->rev || !s->cos_t || !s->sin_t || !s->win) {
        wspr_spec_free(s);
        return -1;
    }

    for (i = 0; i < n; i++) {
        int r = 0;

        for (j = 0; j < bits; j++)
            if (i & (1 << j))
                r |= 1 << (bits - 1 - j);
        s->rev[i] = r;
    }

    for (i = 0; i < n / 2; i++) {
        double a = -2.0 * M_PI * i / n;

        s->cos_t[i] = (float)cos(a);
        s->sin_t[i] = (float)sin(a);
    }

    /*
     * Hann window. Without it a tone between bins spreads across the
     * whole line and the waterfall shows a smear rather than a trace;
     * WSPR tones sit 1.46 Hz apart, so leakage matters more here than
     * the small loss of resolution costs.
     */
    for (i = 0; i < n; i++)
        s->win[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1)));

    s->fill = 0;
    return 0;
}

void wspr_spec_free(wspr_spec_t *s)
{
    if (!s)
        return;

    free(s->rev);
    free(s->cos_t);
    free(s->sin_t);
    free(s->win);

    s->rev = NULL;
    s->cos_t = NULL;
    s->sin_t = NULL;
    s->win = NULL;
    s->n = 0;
    s->fill = 0;
}

void wspr_spec_reset(wspr_spec_t *s)
{
    if (s)
        s->fill = 0;
}

static void fft_run(const wspr_spec_t *s, float *re, float *im)
{
    int n = s->n;
    int i, len;

    for (i = 0; i < n; i++) {
        int r = s->rev[i];

        if (r > i) {
            float t;

            t = re[i]; re[i] = re[r]; re[r] = t;
            t = im[i]; im[i] = im[r]; im[r] = t;
        }
    }

    for (len = 2; len <= n; len <<= 1) {
        int half = len / 2;
        int step = n / len;

        for (i = 0; i < n; i += len) {
            int k, tw = 0;

            for (k = 0; k < half; k++, tw += step) {
                float wr = s->cos_t[tw];
                float wi = s->sin_t[tw];
                float xr = re[i + k + half] * wr - im[i + k + half] * wi;
                float xi = re[i + k + half] * wi + im[i + k + half] * wr;

                re[i + k + half] = re[i + k] - xr;
                im[i + k + half] = im[i + k] - xi;
                re[i + k] += xr;
                im[i + k] += xi;
            }
        }
    }
}

/* ------------------------------------------------------------------ */

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a;
    float y = *(const float *)b;

    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

static void emit_line(wspr_spec_t *s, wspr_spec_cb cb, void *ctx)
{
    float re[WSPR_SPEC_BINS], im[WSPR_SPEC_BINS];
    float db[WSPR_SPEC_BINS], sorted[WSPR_SPEC_BINS];
    float median;
    int n = s->n;
    int i;

    for (i = 0; i < n; i++) {
        re[i] = s->buf_re[i] * s->win[i];
        im[i] = s->buf_im[i] * s->win[i];
    }

    fft_run(s, re, im);

    /*
     * Reorder so the lowest frequency comes first. Bin 0 is the centre
     * of the window, the upper half of the array holds the negative
     * frequencies, so the two halves swap.
     */
    for (i = 0; i < n; i++) {
        int src = (i + n / 2) % n;
        float p = re[src] * re[src] + im[src] * im[src];

        db[i] = 10.0f * log10f(p + 1e-20f);
    }

    /*
     * Reference the line to its own median rather than to full scale.
     * The median of 256 bins is the noise floor as long as signals fill
     * less than half the window, which in a 375 Hz span always holds.
     * The display range then stays correct regardless of AF and RF gain.
     */
    memcpy(sorted, db, sizeof(sorted));
    qsort(sorted, (size_t)n, sizeof(float), cmp_float);
    median = sorted[n / 2];

    for (i = 0; i < n; i++)
        db[i] -= median;

    if (cb)
        cb(db, n, ctx);
}

void wspr_spec_process(wspr_spec_t *s, const float *re, const float *im,
                       long n, wspr_spec_cb cb, void *ctx)
{
    long i;

    if (!s || !s->n || !re || !im)
        return;

    for (i = 0; i < n; i++) {
        s->buf_re[s->fill] = re[i];
        s->buf_im[s->fill] = im[i];
        s->fill++;

        if (s->fill >= s->n) {
            s->fill = 0;
            emit_line(s, cb, ctx);
        }
    }
}
