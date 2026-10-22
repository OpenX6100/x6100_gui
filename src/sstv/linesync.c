/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 SSTV - real-time line-sync tracker
 *
 * (c) 2026 Hany El Imam
 *
 * 1. Sync pulses: sliding 2-ms correlation with 1200 Hz (1200 Hz is exactly
 *    SR/10, so the oscillator is a 10-entry table).  score = fraction of the
 *    window energy at 1200 Hz (0..1, independent of signal level).  A pulse
 *    is a run with score above the threshold; its start and length follow
 *    from the run boundaries.  Pulses shorter than 5 ms (as measured; real
 *    4.9-9 ms syncs measure 6-11 ms) are ignored: noise produces about 80
 *    short pulses per second but only ~0.5 per second this long.
 * 2. Search: for every new pulse and every candidate mode, look for an
 *    earlier pulse exactly one line period before.  The line periods of the
 *    supported modes are far apart compared with the timing accuracy (Martin
 *    M1 446.4 ms vs Scottie S1 428.2 ms: 18 ms, tolerance 3 ms), so the
 *    period alone identifies the mode.  The sync length is NOT used: with a
 *    2-ms window it depends on the picture content right after the sync and
 *    on noise, and a hard length class misclassified clean Martin signals.
 *    Evidence is a real chain: a pulse extends the chain of its predecessor
 *    one line (or, if one sync was lost, two lines) earlier.  Random noise
 *    pulses easily pair up by chance, but hardly ever form a chain.  Most
 *    links must be 1-line links, otherwise Robot 72 (300 ms) would form a
 *    perfect chain of 2-line links for Robot 36 (150 ms).
 *    Consecutive confirmations build evidence; the first mode with enough
 *    evidence locks.  When several modes qualify (Robot 36 also confirms
 *    Robot 72 two lines back and Scottie DX seven lines back) the shortest
 *    line period wins.
 * 3. Locked: every pulse that falls on the line grid (any number of lines
 *    later) is a received sync.  The line period is re-estimated by least
 *    squares on the received syncs, so long fades do not lose the phase.
 *    A pulse counts as received only if it is on the grid AND its length is
 *    within 2 ms of the syncs received so far.
 *    LOST needs BOTH max(8 lines, 10 s) without any received sync AND 10 s
 *    without SSTV video energy.  On real recordings the syncs can vanish in
 *    noise for 40 s while the picture (callsign included) is still readable,
 *    so syncs alone must never end a capture.
 * 4. Video-band monitor: fraction of the energy above 300 Hz that lies in
 *    1100-2400 Hz (4th-order Butterworth filters), per 1-s block.  Measured
 *    with this very code: flat SSB noise <= 0.55; 24 real recordings median
 *    0.79; the sync-less but still readable tails of two real recordings
 *    median 0.81, 10th percentile 0.72.  A block counts as video at >= 0.65.
 *    Mistakes go the safe way: in-band QRM after the picture only makes the
 *    capture run to its time limit.
 */
#include "linesync.h"

#include <math.h>
#include <pthread.h>
#include <string.h>

#define LS_SR          SSTV_SAMPLE_RATE
#define LS_ON          0.55        /* score to start a pulse                  */
#define LS_OFF         0.40        /* score to end it                          */
#define LS_MIN_MS      5.0         /* plausible pulse length (as measured)     */
#define LS_MAX_MS      14.0
#define LS_LOCK_CHAIN  5           /* chained syncs needed to lock             */
#define LS_TOL_MS      3.0         /* timing tolerance on one line             */
#define LS_TOL_REL     0.006
#define LS_DRIFT       0.0003      /* extra tolerance per sample of gap        */
#define LS_LEN_TOL     2.0         /* locked: sync length consistency, ms      */
#define LS_LOST_LINES  8
#define LS_LOST_S      10.0
#define LS_VIDEO_RATIO 0.65        /* in-band fraction meaning "video present" */
#define LS_QUIET_S     10.0        /* no video for this long (plus no sync)    */

typedef struct { sstv_mode_t mode; double line_ms; double sync_ms; int lines; } ls_mode_t;
static const ls_mode_t ls_modes[SSTV_LS_NMODES] = {
    { SSTV_MODE_ROBOT8_BW,   66.667,  5.0,   120 },
    { SSTV_MODE_ROBOT36,    150.000,  9.0,   240 },
    { SSTV_MODE_ROBOT72,    300.000,  9.0,   240 },
    { SSTV_MODE_MARTIN_M2,  226.798,  4.862, 256 },
    { SSTV_MODE_MARTIN_M1,  446.446,  4.862, 256 },
    { SSTV_MODE_SCOTTIE_S2, 277.692,  9.0,   256 },
    { SSTV_MODE_SCOTTIE_S1, 428.220,  9.0,   256 },
    { SSTV_MODE_SCOTTIE_DX,1050.300,  9.0,   256 },
};
static const float ls_cos[10] = { 1.0f, 0.809017f, 0.309017f, -0.309017f, -0.809017f,
                                 -1.0f,-0.809017f,-0.309017f,  0.309017f,  0.809017f };
static const float ls_sin[10] = { 0.0f, 0.587785f, 0.951057f,  0.951057f,  0.587785f,
                                  0.0f,-0.587785f,-0.951057f, -0.951057f, -0.587785f };

/* 4th-order Butterworth filters as two biquads each (Q = 0.5412 and 1.3066;
 * two identical Q = 0.707 sections would NOT be Butterworth and would lose
 * 6 dB at the corners).  RBJ cookbook, b0 b1 b2 a1 a2 normalised by a0. */
typedef struct { float b0, b1, b2, a1, a2; } ls_bq_t;
static ls_bq_t ls_bq[6];          /* HP 1100 x2, LP 2400 x2, HP 300 x2 */
static pthread_once_t ls_bq_once = PTHREAD_ONCE_INIT;
static void ls_bq_design(ls_bq_t *q, double f0, bool hp, double Q)
{
    double w = 2.0 * M_PI * f0 / LS_SR, c = cos(w), al = sin(w) / (2.0 * Q);
    double a0 = 1.0 + al;
    double b0 = hp ? (1.0 + c) / 2.0 : (1.0 - c) / 2.0;
    double b1 = hp ? -(1.0 + c) : (1.0 - c);
    q->b0 = (float)(b0 / a0); q->b1 = (float)(b1 / a0); q->b2 = (float)(b0 / a0);
    q->a1 = (float)(-2.0 * c / a0); q->a2 = (float)((1.0 - al) / a0);
}
static inline float ls_bq_run(const ls_bq_t *q, float *s, float x)
{
    /* transposed direct form II */
    float y = q->b0 * x + s[0];
    s[0] = q->b1 * x - q->a1 * y + s[1];
    s[1] = q->b2 * x - q->a2 * y;
    return y;
}

static void ls_bq_init(void)
{
    static const double Q[2] = { 0.5411961, 1.3065630 };
    for (int k = 0; k < 2; ++k) {
        ls_bq_design(&ls_bq[0 + k], 1100.0, true,  Q[k]);
        ls_bq_design(&ls_bq[2 + k], 2400.0, false, Q[k]);
        ls_bq_design(&ls_bq[4 + k],  300.0, true,  Q[k]);
    }
}

static int mode_index(sstv_mode_t m)
{
    for (int i = 0; i < SSTV_LS_NMODES; ++i) if (ls_modes[i].mode == m) return i;
    return -1;
}

double sstv_linesync_line_seconds(sstv_mode_t mode)
{
    int i = mode_index(mode);
    return i < 0 ? 0.0 : ls_modes[i].line_ms / 1000.0;
}

double sstv_linesync_picture_seconds(sstv_mode_t mode)
{
    int i = mode_index(mode);
    return i < 0 ? 0.0 : ls_modes[i].line_ms * ls_modes[i].lines / 1000.0;
}

static double line_samples(int mi) { return ls_modes[mi].line_ms * LS_SR / 1000.0; }
static double tol_samples(int mi)
{
    double t = LS_TOL_MS * LS_SR / 1000.0, r = LS_TOL_REL * line_samples(mi);
    return t > r ? t : r;
}

void sstv_linesync_init(sstv_linesync_t *ls, sstv_mode_t forced_mode)
{
    pthread_once(&ls_bq_once, ls_bq_init);
    memset(ls, 0, sizeof(*ls));
    ls->forced = (mode_index(forced_mode) >= 0) ? forced_mode : SSTV_MODE_UNKNOWN;
    ls->mi = -1;
    ls->st.state = SSTV_LS_SEARCH;
    ls->st.mode = SSTV_MODE_UNKNOWN;
}

static void fit_add(sstv_linesync_t *ls, double line, double t)
{
    ls->fx[ls->hfit] = line; ls->fy[ls->hfit] = t;
    ls->hfit = (ls->hfit + 1) % SSTV_LS_FIT;
    if (ls->nfit < SSTV_LS_FIT) ++ls->nfit;
    if (ls->nfit < 6) return;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int k = 0; k < ls->nfit; ++k) {
        sx += ls->fx[k]; sy += ls->fy[k]; sxx += ls->fx[k] * ls->fx[k]; sxy += ls->fx[k] * ls->fy[k];
    }
    double den = ls->nfit * sxx - sx * sx;
    if (fabs(den) < 1e-9) return;
    double p = (ls->nfit * sxy - sx * sy) / den, nom = line_samples(ls->mi);
    if (p < nom * 0.995) p = nom * 0.995;           /* never more than 5000 ppm */
    if (p > nom * 1.005) p = nom * 1.005;
    ls->st.period = p;
    ls->st.clock_ppm = (p / nom - 1.0) * 1e6;
}

/* earlier pulse one line period before t, or -1 */
static int find_prev(const sstv_linesync_t *ls, uint64_t t, double period, double tol)
{
    for (int k = 1; k < ls->np; ++k) {
        int idx = (ls->hp - 1 - k + SSTV_LS_HIST) % SSTV_LS_HIST;
        double d = (double)(t - ls->pt[idx]);
        if (d > period + tol) return -1;               /* history is time-ordered */
        if (fabs(d - period) <= tol) return idx;
    }
    return -1;
}

static void lock(sstv_linesync_t *ls, int mi, uint64_t t)
{
    ls->mi = mi;
    ls->st.state = SSTV_LS_LOCKED;
    ls->st.mode = ls_modes[mi].mode;
    ls->st.period = line_samples(mi);
    ls->st.last_sync = t;
    ls->st.lock_sample = t;
    ls->st.hits = 1;
    double lost = LS_LOST_LINES * line_samples(mi), mins = LS_LOST_S * LS_SR;
    ls->st.lost_after = (uint64_t)(lost > mins ? lost : mins);
    /* seed the period fit with the chain that produced the lock */
    double line = 0.0; uint64_t cur = t;
    fit_add(ls, 0.0, (double)t);
    for (int k = 0; k < 8; ++k) {
        int idx = find_prev(ls, cur, line_samples(mi), tol_samples(mi));
        if (idx < 0) break;
        line -= 1.0; cur = ls->pt[idx];
        fit_add(ls, line, (double)cur);
    }
    ls->cur_line = 0.0;      /* line indices are relative to the lock pulse */
    ls->len_avg = ls->pl[(ls->hp - 1 + SSTV_LS_HIST) % SSTV_LS_HIST];
}

static void on_pulse(sstv_linesync_t *ls, uint64_t t, double ms)
{
    if (ms < LS_MIN_MS || ms > LS_MAX_MS) return;
    int me = ls->hp;
    ls->pt[me] = t; ls->pl[me] = (float)ms;
    ls->hp = (ls->hp + 1) % SSTV_LS_HIST;
    if (ls->np < SSTV_LS_HIST) ++ls->np;

    /* chain length per candidate mode (also kept after the lock, for
     * sstv_linesync_classify()) */
    for (int m = 0; m < SSTV_LS_NMODES; ++m) {
        ls->chain[me][m] = 1;
        ls->direct[me][m] = 0;
        if (ls->forced != SSTV_MODE_UNKNOWN && ls_modes[m].mode != ls->forced) continue;
        int prev = find_prev(ls, t, line_samples(m), tol_samples(m)), one = 1;
        if (prev < 0) { prev = find_prev(ls, t, 2.0 * line_samples(m), tol_samples(m)); one = 0; }
        if (prev < 0) continue;
        int c = ls->chain[prev][m] + 1, d = ls->direct[prev][m] + one;
        ls->chain[me][m] = (uint8_t)(c > 250 ? 250 : c);
        ls->direct[me][m] = (uint8_t)(d > 250 ? 250 : d);
        if (one) ++ls->ev_total[m];
    }

    if (ls->st.state == SSTV_LS_SEARCH) {
        int best = -1;
        for (int m = 0; m < SSTV_LS_NMODES; ++m)
            if (ls->chain[me][m] >= LS_LOCK_CHAIN &&
                2 * ls->direct[me][m] >= ls->chain[me][m] &&
                (best < 0 || ls_modes[m].line_ms < ls_modes[best].line_ms))
                best = m;
        if (best >= 0) lock(ls, best, t);
        return;
    }
    if (ls->st.state != SSTV_LS_LOCKED) return;

    /* locked: does this pulse fall on the line grid? */
    double gap = (double)(t - ls->st.last_sync);
    double nl = floor(gap / ls->st.period + 0.5);
    if (nl < 1.0) return;
    double err = gap - nl * ls->st.period;
    if (fabs(err) > tol_samples(ls->mi) + LS_DRIFT * gap) return;
    if (ls->st.hits >= 4 && fabs(ms - ls->len_avg) > LS_LEN_TOL) return;
    ls->len_avg += 0.1 * (ms - ls->len_avg);
    ls->cur_line += nl;
    ls->st.last_sync = t;
    ++ls->st.hits;
    fit_add(ls, ls->cur_line, (double)t);
}

void sstv_linesync_process(sstv_linesync_t *ls, const float *x, size_t n)
{
    const int W = SSTV_LS_WIN;
    for (size_t i = 0; i < n; ++i) {
        float v = x[i];
        float c = v * ls_cos[ls->ph], s = v * ls_sin[ls->ph], e = v * v;
        ls->ph = (ls->ph + 1) % 10;
        ls->sc += c - ls->bc[ls->rp];
        ls->ss += s - ls->bs[ls->rp];
        ls->se += e - ls->be[ls->rp];
        ls->bc[ls->rp] = c; ls->bs[ls->rp] = s; ls->be[ls->rp] = e;
        ls->rp = (ls->rp + 1) % W;
        uint64_t now = ls->st.samples++;
        {   /* video-band monitor */
            float a = ls_bq_run(&ls_bq[0], ls->bq[0], v);
            a = ls_bq_run(&ls_bq[1], ls->bq[1], a);
            a = ls_bq_run(&ls_bq[2], ls->bq[2], a);
            a = ls_bq_run(&ls_bq[3], ls->bq[3], a);
            float t = ls_bq_run(&ls_bq[4], ls->bq[4], v);
            t = ls_bq_run(&ls_bq[5], ls->bq[5], t);
            ls->e_in += (double)a * a;
            ls->e_all += (double)t * t;
            if (++ls->e_n >= LS_SR) {
                ls->st.band_ratio = ls->e_all > 1e-12 ? (float)(ls->e_in / ls->e_all) : 0.0f;
                if (ls->st.band_ratio >= LS_VIDEO_RATIO) ls->st.last_video = now + 1;
                ls->e_in = ls->e_all = 0.0; ls->e_n = 0;
            }
        }
        if (ls->rp == 0) {                      /* refresh sums: no float drift */
            double a = 0, b = 0, d = 0;
            for (int k = 0; k < W; ++k) { a += ls->bc[k]; b += ls->bs[k]; d += ls->be[k]; }
            ls->sc = a; ls->ss = b; ls->se = d;
        }
        double score = (ls->se > 1e-12 && now + 1 >= (uint64_t)W)
                     ? 2.0 * (ls->sc * ls->sc + ls->ss * ls->ss) / ((double)W * ls->se) : 0.0;
        if (!ls->in_pulse) {
            if (score >= LS_ON) { ls->in_pulse = true; ls->run_start = now; }
        } else if (score < LS_OFF) {
            ls->in_pulse = false;
            /* window [i-W+1, i] fully inside the pulse during the run */
            uint64_t start = ls->run_start + 1 >= (uint64_t)W ? ls->run_start + 1 - W : 0;
            double ms = (double)(now - ls->run_start + W - 1) * 1000.0 / LS_SR;
            on_pulse(ls, start, ms);
        }
    }
    if (ls->st.state == SSTV_LS_LOCKED &&
        ls->st.samples - ls->st.last_sync > ls->st.lost_after &&
        ls->st.samples - ls->st.last_video > (uint64_t)(LS_QUIET_S * LS_SR))
        ls->st.state = SSTV_LS_LOST;
}

sstv_mode_t sstv_linesync_classify(const float *x, long n, sstv_mode_t prefer, long *evidence)
{
    sstv_linesync_t ls;             /* ~2.5 kB on the stack: reentrant */
    if (evidence) *evidence = 0;
    if (!x || n <= 0) return SSTV_MODE_UNKNOWN;
    sstv_linesync_init(&ls, SSTV_MODE_UNKNOWN);
    sstv_linesync_process(&ls, x, (size_t)n);
    /* The mode with most confirmed line intervals wins.  A shorter mode
     * replaces it only when its period is an integer sub-multiple (Robot 36
     * also confirms Robot 72 two lines back and Scottie DX seven lines back)
     * and it has at least half the evidence.  Without the sub-multiple
     * condition a few chance confirmations of a shorter mode won on
     * recordings where half the picture has no usable syncs. */
    int best = -1;
    for (int m = 0; m < SSTV_LS_NMODES; ++m)
        if (best < 0 || ls.ev_total[m] > ls.ev_total[best]) best = m;
    if (best < 0 || ls.ev_total[best] < LS_LOCK_CHAIN) return SSTV_MODE_UNKNOWN;
    int pi = mode_index(prefer);
    if (pi >= 0 && pi != best && 2 * ls.ev_total[pi] >= ls.ev_total[best] && ls.ev_total[pi] >= LS_LOCK_CHAIN)
        best = pi;
    for (int m = 0; m < SSTV_LS_NMODES; ++m) {
        double r = ls_modes[best].line_ms / ls_modes[m].line_ms;
        if (r > 1.5 && fabs(r - floor(r + 0.5)) < 0.01 && 2 * ls.ev_total[m] >= ls.ev_total[best] &&
            ls_modes[m].line_ms < ls_modes[best].line_ms)
            best = m;
    }
    if (evidence) *evidence = ls.ev_total[best];
    return ls_modes[best].mode;
}
