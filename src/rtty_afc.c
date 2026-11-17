/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - RTTY tuning: snap and AFC
 */

#include "rtty_afc.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <liquid/liquid.h>

#define NFFT      2048
#define NBIN      (NFFT / 2 + 1)
#define AVG_TAU_S 2.0f          /* memory of the averaged spectrum */
#define MIN_SNR   4.0f          /* both tones 6 dB over the band's median */

static float complex *in_buf;
static float complex *out_buf;
static fftplan        plan;
static float          window[NFFT];
static float          avg[NBIN];
static float          fs;
static size_t         fill;
static unsigned       frames;

bool rtty_afc_init(float sample_rate)
{
    rtty_afc_free();
    fs = sample_rate;
    in_buf = malloc(sizeof(float complex) * NFFT);
    out_buf = malloc(sizeof(float complex) * NFFT);
    if (!in_buf || !out_buf) {
        rtty_afc_free();
        return false;
    }
    plan = fft_create_plan(NFFT, in_buf, out_buf, LIQUID_FFT_FORWARD, 0);
    for (int i = 0; i < NFFT; i++) {
        window[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (NFFT - 1));
    }
    rtty_afc_reset();
    return true;
}

void rtty_afc_free(void)
{
    if (plan) {
        fft_destroy_plan(plan);
        plan = NULL;
    }
    free(in_buf);
    free(out_buf);
    in_buf = NULL;
    out_buf = NULL;
}

void rtty_afc_reset(void)
{
    memset(avg, 0, sizeof(avg));
    fill = 0;
    frames = 0;
}

float rtty_afc_seconds(void)
{
    return fs > 0.0f ? (float)frames * NFFT / fs : 0.0f;
}

static void frame_done(void)
{
    float a = (fs > 0.0f) ? (NFFT / fs) / AVG_TAU_S : 1.0f;

    fft_execute(plan);
    if (frames == 0) {
        a = 1.0f;                       /* the first frame as it is */
    } else if (a > 1.0f) {
        a = 1.0f;
    }
    for (int k = 0; k < NBIN; k++) {
        float p = crealf(out_buf[k]) * crealf(out_buf[k]) + cimagf(out_buf[k]) * cimagf(out_buf[k]);

        avg[k] += a * (p - avg[k]);
    }
    frames++;
}

void rtty_afc_feed(const float *samples, size_t n)
{
    if (!plan) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        in_buf[fill] = samples[i] * window[fill];
        if (++fill == NFFT) {
            frame_done();
            fill = 0;
        }
    }
}

/* ---- the search -------------------------------------------------------- */

static int bin_of(float hz, float bw)
{
    int k = (int)lroundf(hz / bw);

    return k < 1 ? 1 : (k > NBIN - 2 ? NBIN - 2 : k);
}

/* The power at hz, between the two nearest bins. */
static float power_at(float hz, float bw)
{
    float x = hz / bw;
    int   k = (int)floorf(x);
    float t = x - (float)k;

    if (k < 0) return avg[0];
    if (k >= NBIN - 1) return avg[NBIN - 1];
    return avg[k] + (avg[k + 1] - avg[k]) * t;
}

/* Where the tone near hz really is: the local maximum within a bin and
 * a half, placed between bins by a parabola through the log powers. */
static float tone_at(float hz, float bw)
{
    int k = bin_of(hz, bw);
    int best = k;

    for (int j = k - 1; j <= k + 1; j++) {
        if (j >= 1 && j < NBIN - 1 && avg[j] > avg[best]) {
            best = j;
        }
    }
    if (best >= 1 && best < NBIN - 1 && avg[best - 1] > 0.0f && avg[best + 1] > 0.0f &&
        avg[best] > 0.0f) {
        float l = logf(avg[best - 1]);
        float c = logf(avg[best]);
        float r = logf(avg[best + 1]);
        float d = l - 2.0f * c + r;
        float off = (d < 0.0f) ? 0.5f * (l - r) / d : 0.0f;

        if (off > 0.5f) off = 0.5f;
        if (off < -0.5f) off = -0.5f;
        return ((float)best + off) * bw;
    }
    return (float)best * bw;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;

    return (x > y) - (x < y);
}

bool rtty_afc_find(float center, float shift, float range, float lo, float hi,
                   float pass_lo, float pass_hi, float *found)
{
    float bw = fs / NFFT;
    float tmp[NBIN];
    int   k0, k1, n = 0;
    float noise, best_score = 0.0f, best_c = center;

    if (!plan || frames < 4 || bw <= 0.0f) {
        return false;                   /* half a second of audio at least */
    }

    /* The band searched, both tones included, inside the receive filter:
     * its median is the noise. */
    {
        float f0 = center - range - shift / 2.0f - 50.0f;
        float f1 = center + range + shift / 2.0f + 50.0f;

        if (f0 < pass_lo + bw) f0 = pass_lo + bw;
        if (f1 > pass_hi - bw) f1 = pass_hi - bw;
        if (f1 <= f0) {
            return false;
        }
        k0 = bin_of(f0, bw);
        k1 = bin_of(f1, bw);
    }
    for (int k = k0; k <= k1; k++) {
        tmp[n++] = avg[k];
    }
    if (n < 8) {
        return false;
    }
    qsort(tmp, (size_t)n, sizeof(float), cmp_float);
    noise = tmp[n / 2];
    if (noise <= 0.0f) {
        return false;
    }

    /* Both tones must be there, about equally strong: a pair scores by
     * its weaker tone, scaled down when the other is much stronger, so a
     * carrier next to one tone of a station does not make a pair. */
    for (float c = center - range; c <= center + range; c += bw / 4.0f) {
        float m, s, lo_p, hi_p, score;

        if (c < lo || c > hi || c - shift / 2.0f < pass_lo + bw || c + shift / 2.0f > pass_hi - bw) {
            continue;                   /* both tones inside the filter */
        }
        m = power_at(c + shift / 2.0f, bw);
        s = power_at(c - shift / 2.0f, bw);
        lo_p = m < s ? m : s;
        hi_p = m < s ? s : m;
        if (lo_p < noise * MIN_SNR || hi_p <= 0.0f) {
            continue;
        }
        score = lo_p * sqrtf(lo_p / hi_p);
        if (score > best_score) {
            best_score = score;
            best_c = c;
        }
    }
    if (best_score <= 0.0f) {
        return false;
    }

    /* The middle of the two tones, each found to a fraction of a bin;
     * if they disagree with the shift, the coarse centre is kept. */
    {
        float fm = tone_at(best_c + shift / 2.0f, bw);
        float fsp = tone_at(best_c - shift / 2.0f, bw);
        float c = (fm + fsp) / 2.0f;

        if (fabsf((fm - fsp) - shift) < 2.5f * bw && fabsf(c - best_c) < 2.0f * bw) {
            best_c = c;
        }
    }
    if (best_c < lo) best_c = lo;
    if (best_c > hi) best_c = hi;
    *found = best_c;
    return true;
}
