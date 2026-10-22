/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - PSK31 spectrum for the waterfall display
 */

#include "psk_spec.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MIN_BINS    16

/* ------------------------------------------------------------------ */
/* Radix-2 FFT                                                         */

int psk_spec_init(psk_spec_t *s, float fs)
{
    int n = PSK_SPEC_N;
    int i, j, bits = 0;

    if (!s || fs <= 0.0f)
        return -1;

    memset(s, 0, sizeof(*s));
    s->fs = fs;
    s->df = fs / (float)n;
    s->hop = (int)lroundf(fs / PSK_SPEC_LINE_RATE);
    if (s->hop < 1)
        s->hop = 1;
    if (s->hop > n)
        s->hop = n;

    while ((1 << bits) < n)
        bits++;

    s->rev = (int *)malloc(sizeof(int) * n);
    s->cos_t = (float *)malloc(sizeof(float) * (n / 2));
    s->sin_t = (float *)malloc(sizeof(float) * (n / 2));
    s->win = (float *)malloc(sizeof(float) * n);

    if (!s->rev || !s->cos_t || !s->sin_t || !s->win) {
        psk_spec_free(s);
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

    /* Hann window: keeps a strong station from smearing across the
     * whole line and hiding weak ones beside it. */
    for (i = 0; i < n; i++)
        s->win[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1)));

    psk_spec_set_range(s, 200.0f, 2800.0f);
    return 0;
}

void psk_spec_free(psk_spec_t *s)
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
}

void psk_spec_reset(psk_spec_t *s)
{
    if (!s)
        return;

    memset(s->ring, 0, sizeof(s->ring));
    s->pos = 0;
    s->since = 0;
    s->filled = 0;
}

void psk_spec_set_range(psk_spec_t *s, float lo_hz, float hi_hz)
{
    int lo, hi;

    if (!s || s->df <= 0.0f)
        return;

    if (hi_hz < lo_hz) {
        float t = lo_hz;

        lo_hz = hi_hz;
        hi_hz = t;
    }

    lo = (int)ceilf(lo_hz / s->df);
    hi = (int)floorf(hi_hz / s->df);

    if (lo < 1)
        lo = 1;
    if (hi > PSK_SPEC_MAXBIN - 1)
        hi = PSK_SPEC_MAXBIN - 1;

    if (hi - lo + 1 < MIN_BINS) {
        hi = lo + MIN_BINS - 1;
        if (hi > PSK_SPEC_MAXBIN - 1) {
            hi = PSK_SPEC_MAXBIN - 1;
            lo = hi - MIN_BINS + 1;
        }
    }

    s->lo_bin = lo;
    s->hi_bin = hi;
}

int psk_spec_bins(const psk_spec_t *s)
{
    return s->hi_bin - s->lo_bin + 1;
}

float psk_spec_bin0_hz(const psk_spec_t *s)
{
    return (float)s->lo_bin * s->df;
}

float psk_spec_df(const psk_spec_t *s)
{
    return s->df;
}

float psk_spec_edge_lo_hz(const psk_spec_t *s)
{
    return ((float)s->lo_bin - 0.5f) * s->df;
}

float psk_spec_edge_hi_hz(const psk_spec_t *s)
{
    return ((float)s->hi_bin + 0.5f) * s->df;
}

static void fft_run(const psk_spec_t *s, float *re, float *im)
{
    int n = PSK_SPEC_N;
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

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a;
    float y = *(const float *)b;

    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

static float median_of(const float *v, int n)
{
    float tmp[PSK_SPEC_MAXBIN];

    if (n <= 0)
        return 0.0f;
    if (n > PSK_SPEC_MAXBIN)
        n = PSK_SPEC_MAXBIN;

    memcpy(tmp, v, sizeof(float) * (size_t)n);
    qsort(tmp, (size_t)n, sizeof(float), cmp_float);
    return tmp[n / 2];
}

static void emit_line(psk_spec_t *s, psk_spec_cb cb, void *ctx)
{
    static float re[PSK_SPEC_N], im[PSK_SPEC_N];
    static float db[PSK_SPEC_MAXBIN];
    int n = PSK_SPEC_N;
    int nb = psk_spec_bins(s);
    int i, src;
    float median;

    /* Oldest sample first: pos points at the oldest one in the ring. */
    src = s->pos;
    for (i = 0; i < n; i++) {
        re[i] = s->ring[src] * s->win[i];
        im[i] = 0.0f;
        if (++src >= n)
            src = 0;
    }

    fft_run(s, re, im);

    for (i = 0; i < nb; i++) {
        int b = s->lo_bin + i;
        float p = re[b] * re[b] + im[b] * im[b];

        db[i] = 10.0f * log10f(p + 1e-20f);
    }

    /* The median of the displayed span is the noise floor as long as
     * signals fill less than half of it, which holds even on a busy
     * PSK31 segment: a dozen stations take well under 1 kHz. */
    median = median_of(db, nb);
    for (i = 0; i < nb; i++)
        db[i] -= median;

    if (cb)
        cb(db, nb, ctx);
}

void psk_spec_process(psk_spec_t *s, const float *x, unsigned int n,
                      psk_spec_cb cb, void *ctx)
{
    unsigned int i;

    if (!s || !s->rev || !x)
        return;

    for (i = 0; i < n; i++) {
        s->ring[s->pos] = x[i];
        if (++s->pos >= PSK_SPEC_N)
            s->pos = 0;

        if (s->filled < PSK_SPEC_N)
            s->filled++;

        if (++s->since >= s->hop) {
            s->since = 0;
            if (s->filled >= PSK_SPEC_N)
                emit_line(s, cb, ctx);
        }
    }
}

/* ------------------------------------------------------------------ */

#define SNAP_HALF       3       /* sliding window: 2*3+1 = 7 bins     */
#define SNAP_CENTROID   4       /* centroid over 2*4+1 = 9 bins       */
#define SNAP_MIN_RATIO  2.0f    /* window energy 3 dB above noise     */

bool psk_spec_snap(const float *lin, int nbins, float bin0_hz, float df,
                   float hz, float search_hz, float *out_hz)
{
    float noise, best = -1.0f, sum_w = 0.0f, sum_wf = 0.0f;
    int c_lo, c_hi, c, k, best_c = -1;

    if (!lin || nbins < 2 * SNAP_CENTROID + 1 || df <= 0.0f || !out_hz)
        return false;

    noise = median_of(lin, nbins);
    if (noise <= 0.0f)
        noise = 1e-12f;

    c_lo = (int)floorf((hz - search_hz - bin0_hz) / df);
    c_hi = (int)ceilf((hz + search_hz - bin0_hz) / df);

    if (c_lo < SNAP_HALF)
        c_lo = SNAP_HALF;
    if (c_hi > nbins - 1 - SNAP_HALF)
        c_hi = nbins - 1 - SNAP_HALF;

    for (c = c_lo; c <= c_hi; c++) {
        float sum = 0.0f;

        for (k = -SNAP_HALF; k <= SNAP_HALF; k++)
            sum += lin[c + k];

        if (sum > best) {
            best = sum;
            best_c = c;
        }
    }

    if (best_c < 0)
        return false;

    /*
     * A maximum on the edge of the search range means the energy is
     * rising towards something outside it - usually a stronger station
     * next door. Snapping there would drag the receiver off the signal
     * it is on, and repeated calls would walk it all the way over.
     */
    if (c_hi > c_lo && (best_c == c_lo || best_c == c_hi))
        return false;

    if (best < noise * (float)(2 * SNAP_HALF + 1) * SNAP_MIN_RATIO)
        return false;

    for (k = -SNAP_CENTROID; k <= SNAP_CENTROID; k++) {
        int b = best_c + k;
        float w;

        if (b < 0 || b >= nbins)
            continue;

        w = lin[b] - noise;
        if (w <= 0.0f)
            continue;

        sum_w += w;
        sum_wf += w * (float)b;
    }

    if (sum_w <= 0.0f)
        return false;

    *out_hz = bin0_hz + (sum_wf / sum_w) * df;
    return true;
}

/* ------------------------------------------------------------------ */

float psk_spec_window_lin(const float *lin, int nbins, float bin0_hz,
                          float df, float hz, float half_hz)
{
    int lo, hi, b, n = 0;
    float sum = 0.0f;

    if (!lin || nbins <= 0 || df <= 0.0f)
        return 0.0f;

    lo = (int)ceilf((hz - half_hz - bin0_hz) / df);
    hi = (int)floorf((hz + half_hz - bin0_hz) / df);
    if (lo < 0)
        lo = 0;
    if (hi > nbins - 1)
        hi = nbins - 1;

    for (b = lo; b <= hi; b++) {
        sum += lin[b];
        n++;
    }
    if (n == 0)
        return 0.0f;

    /* mean of noise = median / ln 2 */
    return (sum / (float)n) * 0.6931472f;
}
