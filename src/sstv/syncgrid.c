/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Line-sync grid check, see syncgrid.h.
 *
 * The audio goes to complex baseband around 1500 Hz (exactly SR/8: an
 * 8-entry mixer), Blackman low-pass at 750 Hz, one sample in 8 kept
 * (1500 Hz).  For every baseband sample the share of the sync tone in a
 * window of one sync length is computed once; the grids then only average
 * those shares.
 */
#include "syncgrid.h"
#include "linesync.h"
#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#define SG_SR     SSTV_SAMPLE_RATE      /* 12000 */
#define SG_DEC    8
#define SG_FS     ((double)SG_SR / SG_DEC)
#define SG_TAPS   97

/* Restart check thresholds (shares 0..1) */
#define SG_NEW_MIN        0.45f   /* new syncs present                            */
#define SG_NEW_MIN_ALONE  0.55f   /* ... when the old syncs could not be found     */
#define SG_OLD_SEEN       0.35f   /* old syncs found before the VIS                */
#define SG_OLD_GONE       0.35f   /* old syncs gone after the VIS                  */
#define SG_NEW_TOL_S      0.012   /* new grid: transmitters differ a little         */
#define SG_OLD_TOL_S      0.004   /* old grid: extrapolated from its own syncs      */
#define SG_OLD_LOOK_S     3.0     /* audio before the VIS where the old grid is found */
#define SG_NEW_MIN_SLOTS  3       /* new slots left after leaving out the old grid */

static float sg_lp[SG_TAPS];
static pthread_once_t sg_once = PTHREAD_ONCE_INIT;

static void sg_init(void)
{
    const double fc = 750.0 / SG_SR;
    double sum = 0.0;
    for (int k = 0; k < SG_TAPS; ++k) {
        int m = k - SG_TAPS / 2;
        double h = (m == 0) ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
        h *= 0.42 - 0.5 * cos(2.0 * M_PI * k / (SG_TAPS - 1)) + 0.08 * cos(4.0 * M_PI * k / (SG_TAPS - 1));
        sg_lp[k] = (float)h;
        sum += h;
    }
    for (int k = 0; k < SG_TAPS; ++k) sg_lp[k] /= (float)sum;
}

double sstv_syncgrid_first_after_vis(sstv_mode_t mode)
{
    /* Measured on the firmware's own encoder (1KO125 SSTV TX); Scottie:
     * starting sync 9 ms, then sep 1.5 + G + sep 1.5 + B before the sync. */
    switch (mode) {
    case SSTV_MODE_ROBOT8_BW:   return 0.0027;
    case SSTV_MODE_ROBOT36:     return 0.0007;
    case SSTV_MODE_ROBOT72:     return 0.0007;
    case SSTV_MODE_MARTIN_M1:   return 0.0007;
    case SSTV_MODE_MARTIN_M2:   return 0.0007;
    case SSTV_MODE_SCOTTIE_S1:  return 0.2893;
    case SSTV_MODE_SCOTTIE_S2:  return 0.1887;
    case SSTV_MODE_SCOTTIE_DX:  return 0.7039;
    default:                    return -1.0;
    }
}

double sstv_syncgrid_sync_s(sstv_mode_t mode)
{
    switch (mode) {
    case SSTV_MODE_ROBOT8_BW:   return 0.005;
    case SSTV_MODE_MARTIN_M1:
    case SSTV_MODE_MARTIN_M2:   return 0.004862;
    case SSTV_MODE_ROBOT36:
    case SSTV_MODE_ROBOT72:
    case SSTV_MODE_SCOTTIE_S1:
    case SSTV_MODE_SCOTTIE_S2:
    case SSTV_MODE_SCOTTIE_DX:  return 0.009;
    default:                    return 0.0;
    }
}

/* Shares of the sync tone for the baseband samples a .. a+cnt-1 of x
 * (baseband sample j = audio sample j*8).  Returns a malloc'ed array or NULL. */
static float *sg_shares(const float *x, long n, long a, long cnt, double sync_s, float offset_hz)
{
    pthread_once(&sg_once, sg_init);
    const int L = (int)lround(sync_s * SG_FS) < 4 ? 4 : (int)lround(sync_s * SG_FS);
    long nz = cnt + L;
    float complex *z = malloc((size_t)nz * sizeof(*z));
    float *s = malloc((size_t)cnt * sizeof(*s));
    if (!z || !s) {
        free(z);
        free(s);
        return NULL;
    }
    static const float mc[8] = { 1, 0.70710678f, 0, -0.70710678f, -1, -0.70710678f, 0, 0.70710678f };
    static const float ms[8] = { 0, -0.70710678f, -1, -0.70710678f, 0, 0.70710678f, 1, 0.70710678f };
    for (long j = 0; j < nz; ++j) {
        long c = (a + j) * SG_DEC;
        float re = 0.0f, im = 0.0f;
        for (int k = 0; k < SG_TAPS; ++k) {
            long i = c - k + SG_TAPS / 2;
            if (i < 0 || i >= n) continue;
            float v = x[i] * sg_lp[k];
            re += v * mc[i & 7];
            im += v * ms[i & 7];
        }
        z[j] = re + I * im;
    }
    const float w = -2.0f * (float)M_PI * (1200.0f + offset_hz - 1500.0f) / (float)SG_FS;
    const float complex rot = cexpf(I * w);
    for (long j = 0; j < cnt; ++j) {
        float complex acc = 0.0f, ph = 1.0f;
        float p = 0.0f;
        for (int i = 0; i < L; ++i) {
            acc += z[j + i] * ph;
            ph *= rot;
            p += crealf(z[j + i]) * crealf(z[j + i]) + cimagf(z[j + i]) * cimagf(z[j + i]);
        }
        float e = crealf(acc) * crealf(acc) + cimagf(acc) * cimagf(acc);
        s[j] = p > 1e-20f ? e / (L * p) : 0.0f;
    }
    free(z);
    return s;
}

float sstv_syncgrid_measure(const float *x, long n, double first_s, double period_s, double sync_s,
                            double until_s, double tol_s, float offset_hz, int *slots)
{
    if (slots) *slots = 0;
    if (!x || n <= 0 || period_s <= 0.0 || sync_s <= 0.0) return -1.0f;
    const double end_s = fmin(until_s, (double)n / SG_SR);
    const long tol = (long)lround(tol_s * SG_FS);
    long a = (long)floor((first_s - tol_s) * SG_FS);
    if (a < 0) a = 0;
    long b = (long)ceil(end_s * SG_FS);
    if (b <= a) return -1.0f;
    float *s = sg_shares(x, n, a, b - a, sync_s, offset_hz);
    if (!s) return -1.0f;

    float best = -1.0f;
    int best_k = 0;
    for (long d = -tol; d <= tol; ++d) {
        float sum = 0.0f;
        int k = 0;
        for (int i = 0;; ++i) {
            double t = first_s + i * period_s;
            if (t + sync_s > end_s) break;
            long j = (long)lround(t * SG_FS) + d - a;
            if (j < 0) continue;
            if (j >= b - a) break;
            sum += s[j];
            k++;
        }
        if (k && sum / k > best) {
            best = sum / k;
            best_k = k;
        }
    }
    free(s);
    if (slots) *slots = best_k;
    return best;
}

double sstv_syncgrid_find(const float *x, long n, double from_s, double to_s, double period_s, double sync_s,
                          float offset_hz, float *share)
{
    if (share) *share = -1.0f;
    if (!x || n <= 0 || period_s <= 0.0 || sync_s <= 0.0) return -1.0;
    if (from_s < 0.0) from_s = 0.0;
    if (to_s > (double)n / SG_SR) to_s = (double)n / SG_SR;
    if (to_s - from_s < 2.0 * period_s) return -1.0;
    long a = (long)floor(from_s * SG_FS), b = (long)floor((to_s - sync_s) * SG_FS);
    if (b <= a) return -1.0;
    float *s = sg_shares(x, n, a, b - a, sync_s, offset_hz);
    if (!s) return -1.0;

    const double P = period_s * SG_FS;      /* baseband samples per line */
    float best = -1.0f;
    long best_j = -1;
    for (long j0 = 0; j0 < (long)ceil(P) && j0 < b - a; ++j0) {
        float sum = 0.0f;
        int k = 0;
        for (int i = 0;; ++i) {
            long j = j0 + (long)lround(i * P);
            if (j >= b - a) break;
            sum += s[j];
            k++;
        }
        if (k && sum / k > best) {
            best = sum / k;
            best_j = j0;
        }
    }
    free(s);
    if (best_j < 0) return -1.0;
    if (share) *share = best;
    return (double)(a + best_j) / SG_FS;
}

/* Old grid t_k = first_s + k * period_s in [first_s, until_s), leaving out
 * the slots that touch an excluded interval (ex[2*i], ex[2*i+1]): there the
 * new picture or the VIS itself has 1200 Hz of its own. */
static float sg_old_measure(const float *x, long n, double first_s, double period_s, double sync_s,
                            double until_s, double tol_s, float offset_hz, const double *ex, int nex, int *slots)
{
    if (slots) *slots = 0;
    const double end_s = fmin(until_s, (double)n / SG_SR);
    const long tol = (long)lround(tol_s * SG_FS);
    long a = (long)floor((first_s - tol_s) * SG_FS);
    if (a < 0) a = 0;
    long b = (long)ceil(end_s * SG_FS);
    if (b <= a) return -1.0f;
    float *s = sg_shares(x, n, a, b - a, sync_s, offset_hz);
    if (!s) return -1.0f;
    float best = -1.0f;
    int best_k = 0;
    for (long d = -tol; d <= tol; ++d) {
        float sum = 0.0f;
        int k = 0;
        for (int i = 0;; ++i) {
            double t = first_s + i * period_s;
            if (t + sync_s > end_s) break;
            bool skip = false;
            for (int e = 0; e < nex && !skip; ++e)
                skip = t + sync_s + tol_s > ex[2 * e] && t - tol_s < ex[2 * e + 1];
            if (skip) continue;
            long j = (long)lround(t * SG_FS) + d - a;
            if (j < 0) continue;
            if (j >= b - a) break;
            sum += s[j];
            k++;
        }
        if (k && sum / k > best) {
            best = sum / k;
            best_k = k;
        }
    }
    free(s);
    if (slots) *slots = best_k;
    return best;
}

bool sstv_syncgrid_restart_check(const float *x, long n, double vis_end_s, double span_s,
                                 sstv_mode_t new_mode, float new_off, sstv_mode_t old_mode, float old_off,
                                 sstv_syncgrid_verdict_t *v)
{
    sstv_syncgrid_verdict_t r = { .new_share = -1.0f, .old_before = -1.0f, .old_share = -1.0f };
    const double until = vis_end_s + span_s;
    const double vis_start = vis_end_s - 0.910;
    double pn = sstv_linesync_line_seconds(new_mode), sn = sstv_syncgrid_sync_s(new_mode);
    double f0 = sstv_syncgrid_first_after_vis(new_mode);
    double po = sstv_linesync_line_seconds(old_mode), so = sstv_syncgrid_sync_s(old_mode);
    bool new_ok = pn > 0.0 && sn > 0.0 && f0 >= 0.0;

    /* old picture: its grid in the audio before the VIS (0.91 s long) */
    double ph = -1.0;
    if (po > 0.0 && so > 0.0) {
        ph = sstv_syncgrid_find(x, n, vis_start - SG_OLD_LOOK_S, vis_start - 0.02, po, so, old_off, &r.old_before);
        if (ph >= 0.0 && r.old_before < SG_OLD_SEEN) ph = -1.0;
    }

    /* new picture: its syncs from the second line on (the first one sits
     * right after the stop bit, itself 1200 Hz).  Where an old sync would
     * fall too, the slot is left out: a mode with nearly the old line length
     * would otherwise find the old picture's syncs and take them for its own.
     * With fewer than SG_NEW_MIN_SLOTS slots left, all of them are used (the
     * old syncs, gone or not, then decide). */
    if (new_ok) {
        r.new_share = sstv_syncgrid_measure(x, n, vis_end_s + f0 + pn, pn, sn, until, SG_NEW_TOL_S, new_off,
                                            &r.new_slots);
        if (ph >= 0.0) {
            double ex[2 * 64];
            int nex = 0;
            for (double t = ph + ceil((vis_end_s - ph) / po) * po; t < until && nex < 64; t += po) {
                ex[2 * nex] = t - SG_OLD_TOL_S;
                ex[2 * nex + 1] = t + so + SG_OLD_TOL_S;
                nex++;
            }
            int k = 0;
            float m = sg_old_measure(x, n, vis_end_s + f0 + pn, pn, sn, until, SG_NEW_TOL_S, new_off, ex, nex, &k);
            if (k >= SG_NEW_MIN_SLOTS) {
                r.new_share = m;
                r.new_slots = k;
            }
        }
    }

    /* old picture from the start of the VIS on: a real new transmission has
     * no old syncs in its header either, while picture content read as a VIS
     * has them all along.  Left out: the 1200-Hz parts of the header (break,
     * start, stop bit) and the slots of the new grid. */
    if (ph >= 0.0) {
        double ex[2 * 64];
        int nex = 0;
        const double vp[3][2] = { { 0.300, 0.310 }, { 0.610, 0.640 }, { 0.880, 0.922 } };
        for (int i = 0; i < 3; ++i) {
            ex[2 * nex] = vis_start + vp[i][0] - 0.015;
            ex[2 * nex + 1] = vis_start + vp[i][1] + 0.015;
            nex++;
        }
        if (new_ok)
            for (double t = vis_end_s + f0; t < until && nex < 64; t += pn) {
                ex[2 * nex] = t - SG_NEW_TOL_S;
                ex[2 * nex + 1] = t + sn + SG_NEW_TOL_S;
                nex++;
            }
        double from = vis_start + 0.02;
        double first = ph + ceil((from - ph) / po) * po;
        r.old_share = sg_old_measure(x, n, first, po, so, until, SG_OLD_TOL_S, old_off, ex, nex, &r.old_slots);
        r.old_known = r.old_slots > 0;
    }

    if (r.old_known)
        r.confirmed = r.new_share >= SG_NEW_MIN && r.old_share < SG_OLD_GONE;
    else
        r.confirmed = r.new_share >= SG_NEW_MIN_ALONE;
    if (v) *v = r;
    return r.confirmed;
}
