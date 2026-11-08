/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4: take a decoded signal out of the audio
 *
 *  The method is WSJT-X's lib/ft8/subtractft8.f90 (GPL-3.0, K1JT and the
 *  WSJT team): reference cref(t) = exp(j(2 pi f0 t + phi(t))), complex
 *  amplitude cfilt = LPF[audio * conj(cref)], audio -= 2 Re(cref * cfilt).
 *  Done here on a 64-per-symbol grid (the amplitude changes slowly), so it
 *  fits in the radio's time between the last decode and the next TX.
 */

#include "subtract.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GFSK_CONST_K 5.336446f /* as gfsk.c */
#define PER_SYM      64        /* fitting grid: points per symbol */
#define SMOOTH_SYM   2         /* the amplitude's low-pass: ~2 symbols, as WSJT-X's NFILT */

static int            nsps, nsym, decim, nd;
static float         *pulse;   /* 3 symbols of the GFSK frequency pulse */
static float complex *bb;      /* exp(j phi(t)): the message at 0 Hz, one per sample */
static float complex *ref_d;   /* bb on the grid */
static float complex *y_d;     /* the audio on the grid, at 0 Hz, a symbol either side */
static float complex *amp_d;   /* audio * conj(cref) on the grid, then low-passed */
static float         *win;     /* the low-pass, cos^2 */
static int            nwin;

bool ftx_subtract_init(int samples_per_sym, int n_tones, float bt) {
    ftx_subtract_free();
    nsps  = samples_per_sym;
    nsym  = n_tones;
    decim = nsps / PER_SYM;
    if (decim < 1) decim = 1;
    nd    = nsym * nsps / decim;
    nwin  = 2 * SMOOTH_SYM * PER_SYM / 2 + 1;
    pulse = malloc(sizeof(float) * 3 * nsps);
    bb    = malloc(sizeof(float complex) * nsym * nsps);
    ref_d = malloc(sizeof(float complex) * nd);
    y_d   = malloc(sizeof(float complex) * (nd + 2 * PER_SYM + 1));
    amp_d = malloc(sizeof(float complex) * nd * 2);
    win   = malloc(sizeof(float) * nwin);
    if (!pulse || !bb || !ref_d || !y_d || !amp_d || !win) {
        ftx_subtract_free();
        return false;
    }
    for (int i = 0; i < 3 * nsps; i++) {
        float t    = i / (float)nsps - 1.5f;
        float arg1 = GFSK_CONST_K * bt * (t + 0.5f);
        float arg2 = GFSK_CONST_K * bt * (t - 0.5f);
        pulse[i]   = (erff(arg1) - erff(arg2)) / 2;
    }
    for (int i = 0; i < nwin; i++) {
        float c = cosf((float)M_PI * (i - nwin / 2) / nwin);
        win[i]  = c * c;
    }
    return true;
}

void ftx_subtract_free(void) {
    free(pulse);
    free(bb);
    free(ref_d);
    free(y_d);
    free(amp_d);
    free(win);
    pulse = NULL;
    bb    = NULL;
    ref_d = y_d = amp_d = NULL;
    win   = NULL;
}

/* The message's phase at 0 Hz, sample by sample, as gfsk_synth makes it
 * (with its dummy symbols at both ends). */
static void make_bb(const uint8_t *tones) {
    const float peak = 2 * (float)M_PI / nsps;
    const int   n    = nsym * nsps;
    float complex z    = 1;
    float         last = -1, step_v = 0;
    float complex step = 1;
    for (int k = 0; k < n; k++) {
        bb[k]   = z;
        int   i = k + nsps; /* gfsk_synth's dphi index */
        float v = 0;
        for (int s = i / nsps - 2; s <= i / nsps; s++)
            if (s >= 0 && s < nsym) v += tones[s] * pulse[i - s * nsps];
        if (i < 2 * nsps) v += pulse[i + nsps] * tones[0];
        int e = i - nsym * nsps;
        if (e >= 0 && e < 2 * nsps) v += pulse[e] * tones[nsym - 1];
        /* Inside a symbol the step repeats: its phasor too */
        step_v = peak * v;
        if (step_v != last) {
            step = cexpf(I * step_v);
            last = step_v;
        }
        z *= step;
        if ((k & 255) == 255) z /= cabsf(z);
    }
}

/* How well the message fits the audio sh grid points after its earliest start. */
static float symbol_energy(int sh) {
    float en = 0;
    for (int s = 0; s < nsym; s++) {
        float complex acc = 0;
        for (int k = s * PER_SYM; k < (s + 1) * PER_SYM && k < nd; k++) acc += y_d[sh + k] * conjf(ref_d[k]);
        en += crealf(acc) * crealf(acc) + cimagf(acc) * cimagf(acc);
    }
    return en;
}

void ftx_subtract(float *audio, int n, int rate, const uint8_t *tones, float freq_hz, int start) {
    if (!bb) return;
    make_bb(tones);
    for (int m = 0; m < nd; m++) {
        float complex s = 0;
        for (int q = 0; q < decim; q++) s += bb[m * decim + q];
        ref_d[m] = s / decim;
    }

    /* The audio around the signal, moved down to 0 Hz, on the grid */
    const double        w   = 2 * M_PI * freq_hz / rate;
    const float complex rot = cexpf(-I * (float)w);
    const int           lo  = start - PER_SYM * decim; /* a symbol early */
    const int           ny  = nd + 2 * PER_SYM + 1;
    float complex       ph  = cexpf(-I * (float)fmod(w * lo, 2 * M_PI));
    for (int m = 0; m < ny; m++) {
        float complex s = 0;
        for (int q = 0; q < decim; q++) {
            int t = lo + m * decim + q;
            if (t >= 0 && t < n) s += audio[t] * ph;
            ph *= rot;
        }
        y_d[m] = s / decim;
        ph /= cabsf(ph); /* keep it on the unit circle */
    }

    /* Its start: the shift where each symbol's energy adds up best, every
     * 4th grid point first, then the ones around the best of those */
    int   best    = PER_SYM;
    float best_en = -1;
    for (int sh = 0; sh <= 2 * PER_SYM; sh += 4) {
        float en = symbol_energy(sh);
        if (en > best_en) {
            best_en = en;
            best    = sh;
        }
    }
    int coarse = best;
    for (int sh = coarse - 3; sh <= coarse + 3; sh++) {
        if (sh < 0 || sh > 2 * PER_SYM || sh == coarse) continue;
        float en = symbol_energy(sh);
        if (en > best_en) {
            best_en = en;
            best    = sh;
        }
    }
    const int t0 = lo + best * decim;

    /* Complex amplitude on the grid, low-passed, edges by the weights inside */
    float complex *raw = amp_d + nd;
    ph                 = cexpf(-I * (float)fmod(w * t0, 2 * M_PI));
    for (int m = 0; m < nd; m++) {
        float complex s = 0;
        for (int q = 0; q < decim; q++) {
            int t = t0 + m * decim + q;
            if (t >= 0 && t < n) s += audio[t] * ph * conjf(bb[m * decim + q]);
            ph *= rot;
        }
        raw[m] = s / decim;
        ph /= cabsf(ph);
    }
    for (int m = 0; m < nd; m++) {
        float complex s  = 0;
        float         ws = 0;
        for (int j = 0; j < nwin; j++) {
            int k = m + j - nwin / 2;
            if (k < 0 || k >= nd) continue;
            s += win[j] * raw[k];
            ws += win[j];
        }
        amp_d[m] = s / ws;
    }

    /* Take it out: audio -= 2 Re(cref * amp), amp between grid points */
    const float complex up = cexpf(I * (float)w);
    float complex       c  = cexpf(I * (float)fmod(w * t0, 2 * M_PI));
    for (int k = 0; k < nsym * nsps; k++) {
        int t = t0 + k;
        if (t >= 0 && t < n) {
            float pos = (k + 0.5f) / decim - 0.5f;
            int   m   = (int)floorf(pos);
            float a   = pos - m;
            float complex am;
            if (m < 0) am = amp_d[0];
            else if (m >= nd - 1) am = amp_d[nd - 1];
            else am = amp_d[m] * (1 - a) + amp_d[m + 1] * a;
            audio[t] -= 2 * crealf(c * bb[k] * am);
        }
        c *= up;
        if ((k & 1023) == 0) c /= cabsf(c);
    }
}
