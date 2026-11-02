/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
/*
 * Xiegu X6100 SSTV - VIS header detector
 *
 * (c) 2026 Hany El Imam
 *
 * Pipeline (per 1-s block, normally every 50 ms):
 *
 *  1. Complex baseband: mix down by 1500 Hz (exactly SR/8, so the mixer is
 *     an 8-entry table), Blackman low-pass, decimate by 8 -> 1500 Hz.
 *
 *  2. Leader gate.  In the two regions that are leader for every admissible
 *     start time ([0.10,0.30] and [0.41,0.61] s) measure which fraction of
 *     the total energy sits in one narrow tone within 1900 +/- 150 Hz.  The
 *     statistic is scale-invariant: noise gives < 0.03, a VIS at -12 dB SNR
 *     (2.5 kHz) ~0.10.  Each leader must pass on its own, at the same
 *     frequency.  The peak also gives the tuning offset (sub-Hz).
 *     Almost all blocks stop here, so the average cost is tiny.
 *
 *  3. Timing.  Locate the leader-2 -> start-bit edge (1900 -> 1200 Hz) on a
 *     0.67-ms grid.  Timing is fixed BEFORE looking at the data bits, so a
 *     shifted frame can never be "explained" by another VIS code.  The
 *     10-ms break 310 ms before the edge is a bonus, not a precondition:
 *     without it the start bit must be clean (START_MIN).
 *
 *  4. Data.  For four bit periods (29.1..30.9 ms) score every symbol with
 *     tone energies at the measured offset, then decode 7 bits + parity by
 *     soft maximum likelihood over the codebook of known VIS codes.  The
 *     stop bit adds to the score but is not required.  Accept only if the
 *     frame score is high enough and the winning code beats every other
 *     code by a clear margin.
 *
 *  Gaps in the signal.  Real recordings showed VIS headers interrupted by
 *  10..180 ms of plain band noise INSERTED into the signal (the rest of the
 *  header arrives later by the same amount - e.g. an audio drop-out at the
 *  transmitting station).  A gap in the leader removed the break from its
 *  nominal place, a gap on the parity/stop bits killed the stop-bit test,
 *  and a gap inside a data bit shifts every following bit.  Hence the
 *  optional break, the soft stop bit and the "slip" hypotheses of step 4:
 *  the data symbols from bit k on may arrive d ms late (one slip per frame).
 */
#include "vis.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

#define VIS_SR        SSTV_SAMPLE_RATE      /* 12000 */
#define VIS_DEC       8
#define VIS_FS        (VIS_SR / VIS_DEC)    /* 1500 Hz complex */
#define VIS_CENTER    1500.0f
#define VIS_TAPS      97
#define VIS_MAXZ      (2 * VIS_SR / VIS_DEC + 8) /* blocks up to 2 s */

#define GATE_MIN      0.035f   /* leader tone energy fraction            */
#define OFF_MAX       150.0f   /* max tuning offset searched, Hz         */
#define LEADER_DF     8.0f     /* max frequency difference of leaders    */
#define EDGE_MIN      0.40f    /* leader tail 1900 / start bit 1200 frac */
#define FRAME_MIN     0.50f    /* mean symbol score                      */
#define CODE_MARGIN   0.30f    /* best vs second code, summed scores     */
#define BIT_INSET     0.05f    /* fraction trimmed at each bit edge      */
#define VIS_LEN_MAX_S 0.937f   /* VIS 3% slow                            */
#define START_MIN     0.70f    /* start bit 1200 fraction without break  */
#define BREAK_BONUS   0.30f    /* edge score bonus when the break is seen */
#define SLIP_MAX_S    0.036f   /* longest gap inside the data bits        */
#define SLIP_STEP_S   0.003f
#define SLIP_PENALTY  0.02f    /* prefer "no slip" when scores are equal  */
#define SLIP_FRAME_MIN    0.75f /* mean symbol score with a slip          */
#define NOBREAK_FRAME_MIN 0.60f /* mean symbol score without the break    */
#define SLIP_GAIN     0.08f    /* slip preferred if this much better      */

static float lp[VIS_TAPS];
static pthread_once_t lp_once = PTHREAD_ONCE_INIT;

static void lp_init(void)
{
    const double fc = 750.0 / VIS_SR;
    double sum = 0.0;
    for (int k = 0; k < VIS_TAPS; ++k) {
        int m = k - VIS_TAPS / 2;
        double h = (m == 0) ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
        h *= 0.42 - 0.5 * cos(2.0 * M_PI * k / (VIS_TAPS - 1))
                  + 0.08 * cos(4.0 * M_PI * k / (VIS_TAPS - 1));
        lp[k] = (float)h;
        sum += h;
    }
    for (int k = 0; k < VIS_TAPS; ++k) lp[k] /= (float)sum;
}

/* |DFT|^2 of z[a .. a+len) at baseband frequency f (Hz relative to 1500). */
static float energy(const float complex *z, int a, int len, float f)
{
    const float w = -2.0f * (float)M_PI * f / VIS_FS;
    float complex rot = cexpf(I * w), ph = 1.0f, acc = 0.0f;
    for (int i = 0; i < len; ++i) { acc += z[a + i] * ph; ph *= rot; }
    return crealf(acc) * crealf(acc) + cimagf(acc) * cimagf(acc);
}

static float power(const float complex *z, int a, int len)
{
    float p = 0.0f;
    for (int i = 0; i < len; ++i)
        p += crealf(z[a+i]) * crealf(z[a+i]) + cimagf(z[a+i]) * cimagf(z[a+i]);
    return p;
}

typedef struct { float e1100, e1200, e1300, e1900; } sym_t;

static bool symbol(const float complex *z, int nz, float t0, float dur,
                   float inset, float off, sym_t *s)
{
    int a = (int)lroundf((t0 + dur * inset) * VIS_FS);
    int len = (int)lroundf(dur * (1.0f - 2.0f * inset) * VIS_FS);
    if (a < 0 || len < 4 || a + len > nz) return false;
    s->e1100 = energy(z, a, len, 1100.0f + off - VIS_CENTER);
    s->e1200 = energy(z, a, len, 1200.0f + off - VIS_CENTER);
    s->e1300 = energy(z, a, len, 1300.0f + off - VIS_CENTER);
    s->e1900 = energy(z, a, len, 1900.0f + off - VIS_CENTER);
    return true;
}

/* Share of e among the four VIS tones (0..1). */
static inline float frac(float e, const sym_t *s)
{
    float d = s->e1100 + s->e1200 + s->e1300 + s->e1900;
    return d > 1e-20f ? e / d : 0.0f;
}

static bool leader_gate(const float complex *z, int nz, float *off, float *q)
{
    const int a1 = (int)(0.10f * VIS_FS), a2 = (int)(0.41f * VIS_FS);
    const int l = (int)(0.20f * VIS_FS);
    *q = 0.0f;
    if (a2 + l > nz) return false;
    float p1 = power(z, a1, l) * l, p2 = power(z, a2, l) * l;
    if (p1 + p2 <= 1e-20f) return false;

    /* 2-Hz grid, noncoherent sum (leader phase may jump at the break). */
    float best = -1.0f, bf = 0.0f, em = 0.0f, ep = 0.0f, prev2 = -1.0f, prev1 = -1.0f;
    for (float f = 1900.0f - OFF_MAX; f <= 1900.0f + OFF_MAX + 0.1f; f += 2.0f) {
        float e = energy(z, a1, l, f - VIS_CENTER) + energy(z, a2, l, f - VIS_CENTER);
        if (prev1 > best && prev1 >= prev2 && prev1 >= e) {
            best = prev1; bf = f - 2.0f; em = prev2; ep = e;
        }
        prev2 = prev1; prev1 = e;
    }
    if (best <= 0.0f) return false;
    float den = em - 2.0f * best + ep;
    float delta = (den < 0.0f) ? 0.5f * (em - ep) / den : 0.0f;
    if (delta > 1.0f) delta = 1.0f;
    if (delta < -1.0f) delta = -1.0f;
    float f0 = bf + 2.0f * delta;
    *off = f0 - 1900.0f;
    *q = best / (p1 + p2);
    if (*q < GATE_MIN) return false;

    /* Each leader on its own: narrow tone, same frequency. */
    if (energy(z, a1, l, f0 - VIS_CENTER) / p1 < 0.5f * GATE_MIN) return false;
    if (energy(z, a2, l, f0 - VIS_CENTER) / p2 < 0.5f * GATE_MIN) return false;
    float b1 = 0, f1 = f0, b2 = 0, f2 = f0;
    for (float f = f0 - 20.0f; f <= f0 + 20.1f; f += 1.0f) {
        float e1 = energy(z, a1, l, f - VIS_CENTER), e2 = energy(z, a2, l, f - VIS_CENTER);
        if (e1 > b1) { b1 = e1; f1 = f; }
        if (e2 > b2) { b2 = e2; f2 = f; }
    }
    return fabsf(f1 - f2) <= LEADER_DF;
}

/* Every assigned 7-bit VIS code: supported and known-unsupported modes.
 * Restricting ML decoding to real codes is what turns parity into coding gain. */
static const unsigned char vis_codes[] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,40,44,48,51,52,55,56,59,60,63,76,
    93,94,95,96,97,98,99,100,104,105,113,114,115 };

static bool supported(int vis, sstv_info_t *o)
{
    memset(o, 0, sizeof(*o));
    o->vis = vis;
    switch (vis) {
    case 2:  o->mode = SSTV_MODE_ROBOT8_BW;  o->name = "Robot 8";    o->width = 160; o->height = 120; break;
    case 8:  o->mode = SSTV_MODE_ROBOT36;    o->name = "Robot 36";   o->width = 320; o->height = 240; break;
    case 12: o->mode = SSTV_MODE_ROBOT72;    o->name = "Robot 72";   o->width = 320; o->height = 240; break;
    case 40: o->mode = SSTV_MODE_MARTIN_M2;  o->name = "Martin M2";  o->width = 320; o->height = 256; break;
    case 44: o->mode = SSTV_MODE_MARTIN_M1;  o->name = "Martin M1";  o->width = 320; o->height = 256; break;
    case 56: o->mode = SSTV_MODE_SCOTTIE_S2; o->name = "Scottie S2"; o->width = 320; o->height = 256; break;
    case 60: o->mode = SSTV_MODE_SCOTTIE_S1; o->name = "Scottie S1"; o->width = 320; o->height = 256; break;
    case 76: o->mode = SSTV_MODE_SCOTTIE_DX; o->name = "Scottie DX"; o->width = 320; o->height = 256; break;
    default: return false;
    }
    return true;
}

bool sstv_vis_detect(const float *x, int n, sstv_info_t *out, sstv_vis_result_t *res)
{
    float complex z[VIS_MAXZ];   /* ~24 kB on the caller's stack: reentrant */
    sstv_vis_result_t r = { .unsupported_vis = -1, .score = -1.0f };
    bool ok = false;
    pthread_once(&lp_once, lp_init);
    if (!x || !out || n <= VIS_TAPS) goto done;

    /* 1. baseband + decimation */
    static const float mc[8] = { 1, 0.70710678f, 0, -0.70710678f, -1, -0.70710678f, 0, 0.70710678f };
    static const float ms[8] = { 0, -0.70710678f, -1, -0.70710678f, 0, 0.70710678f, 1, 0.70710678f };
    int nz = 0;
    for (int o = VIS_TAPS; o + VIS_DEC <= n && nz < VIS_MAXZ; o += VIS_DEC) {
        float re = 0.0f, im = 0.0f;
        for (int k = 0; k < VIS_TAPS; ++k) {
            int i = o - k;
            float v = x[i] * lp[k];
            re += v * mc[i & 7];
            im += v * ms[i & 7];
        }
        z[nz++] = re + I * im;
    }
    const float t_z0 = (float)(VIS_TAPS - VIS_TAPS / 2) / VIS_SR;  /* time of z[0] */

    /* 2. leader gate */
    float off = 0.0f;
    if (!leader_gate(z, nz, &off, &r.leader_q)) goto done;
    r.offset_hz = off;

    /* 3. timing: leader -> start-bit edge */
    const float t_max = (float)nz / VIS_FS + t_z0 - VIS_LEN_MAX_S;
    float best_m = -1.0f, best_t = 0.0f, best_edge = 0.0f;
    bool best_brk = false;
    for (float t = 0.0f; t <= t_max; t += 1.0f / VIS_FS) {
        float tb = t - t_z0;
        sym_t s;
        bool brk = symbol(z, nz, tb + 0.300f, 0.010f, 0.15f, off, &s) &&
                   s.e1200 >= s.e1900;                       /* break */
        float start = tb + 0.610f;
        if (!symbol(z, nz, start - 0.024f, 0.024f, 0.0f, off, &s)) continue;
        float lt = frac(s.e1900, &s);
        if (!symbol(z, nz, start, 0.024f, 0.0f, off, &s)) continue;
        float st = frac(s.e1200, &s);
        if (lt < EDGE_MIN || st < (brk ? EDGE_MIN : START_MIN)) continue;
        float m = lt + st + (brk ? BREAK_BONUS : 0.0f);
        if (m > best_m) { best_m = m; best_t = t; best_edge = lt + st; best_brk = brk; }
    }
    if (best_m < 0.0f) goto done;
    r.near_miss = true;      /* 1KO125: cleared below if the frame is taken */

    /* 4. bit period + slip + soft ML over the VIS codebook */
    static const float periods[] = { 0.0291f, 0.0297f, 0.0303f, 0.0309f };
    const float start = best_t - t_z0 + 0.610f;
    const int n_slip = (int)lroundf(SLIP_MAX_S / SLIP_STEP_S);
    /* Two independent decisions: [0] without slip (the classic decoder),
     * [1] over every slip hypothesis.  [0] is used when it is valid; the
     * slip decoder only rescues frames the classic one rejects. */
    float bests[2] = { -1.0f, -1.0f }, seconds[2] = { -1.0f, -1.0f };
    float best_Ts[2] = { 0.0f, 0.0f }, best_slips[2] = { 0.0f, 0.0f };
    int best_viss[2] = { -1, -1 };
    for (unsigned pi = 0; pi < sizeof(periods) / sizeof(periods[0]); ++pi) {
        const float T = periods[pi];
        sym_t s;
        if (!symbol(z, nz, start, T, BIT_INSET, off, &s)) continue;
        const float fixed0 = 0.5f * best_edge + frac(s.e1200, &s);

        /* Data symbols on time ([0]) and d late ([1]); computed once per d. */
        float f11[2][8], f13[2][8];
        int i;
        for (i = 0; i < 8; ++i) {
            if (!symbol(z, nz, start + (i + 1) * T, T, BIT_INSET, off, &s)) break;
            f11[0][i] = frac(s.e1100, &s);
            f13[0][i] = frac(s.e1300, &s);
        }
        if (i < 8) continue;

        /* slip k: data bits k.. and the stop bit arrive d late
         * (k = 8: only the stop bit).  d = 0 or 3..36 ms. */
        for (int si = 0; si <= n_slip; ++si) {
            const float d = si * SLIP_STEP_S;
            int n_late = 8;
            for (i = 0; i < 8; ++i) {
                if (!si) { f11[1][i] = f11[0][i]; f13[1][i] = f13[0][i]; continue; }
                if (!symbol(z, nz, start + (i + 1) * T + d, T, BIT_INSET, off, &s)) break;
                f11[1][i] = frac(s.e1100, &s);
                f13[1][i] = frac(s.e1300, &s);
            }
            n_late = i;     /* late symbols past the block end: fewer k */
            /* Stop bit: soft (it may be lost in a gap, or lie past the end
             * of the block for a late frame). */
            float fixed = fixed0;
            if (symbol(z, nz, start + 9 * T + d, T, BIT_INSET, off, &s))
                fixed += frac(s.e1200, &s);

            for (int k = (si ? 0 : 8); k <= 8; ++k) {
                if (k < 8 && n_late < 8) continue;
                float g11[8], g13[8];
                for (int j = 0; j < 8; ++j) {
                    g11[j] = f11[j >= k][j];
                    g13[j] = f13[j >= k][j];
                }
                const float pen = si ? SLIP_PENALTY : 0.0f;

                for (unsigned c = 0; c < sizeof(vis_codes); ++c) {
                    int code = vis_codes[c], par = __builtin_popcount(code) & 1;
                    float sc = fixed;
                    for (int b = 0; b < 8; ++b) {
                        int bit = (b < 7) ? (code >> b) & 1 : par;
                        sc += bit ? g11[b] : g13[b];
                    }
                    sc = sc / 11.0f - pen;
                    for (int h = (si ? 1 : 0); h < 2; ++h) {
                        if (sc > bests[h]) {
                            if (code != best_viss[h]) seconds[h] = bests[h];
                            bests[h] = sc; best_viss[h] = code; best_Ts[h] = T; best_slips[h] = d;
                        } else if (code != best_viss[h] && sc > seconds[h]) {
                            seconds[h] = sc;
                        }
                    }
                }
            }
        }
    }
    /* [0]: classic thresholds.  [1]: a slip must be paid for with a much
     * cleaner frame (real headers with a gap score > 0.9, image content
     * that happens to fit a slipped frame ~0.5). */
    const float min_ok[2] = { FRAME_MIN, SLIP_FRAME_MIN };
    bool valid[2];
    for (int v = 0; v < 2; ++v) {
        float need = min_ok[v];
        if (!best_brk && need < NOBREAK_FRAME_MIN) need = NOBREAK_FRAME_MIN;
        valid[v] = best_viss[v] >= 0 && bests[v] >= need &&
                   (bests[v] - seconds[v]) * 11.0f >= CODE_MARGIN;
    }
    /* A slipped frame that fits clearly better than every on-time frame
     * means the on-time decision is reading shifted bits: trust the slip. */
    int h;
    if (valid[1] && best_slips[1] > 0.0f && bests[1] - bests[0] >= SLIP_GAIN) h = 1;
    else if (valid[0]) h = 0;
    else if (valid[1]) h = 1;
    else h = 0;         /* nothing valid: report the classic candidate */
    float best = bests[h], second = seconds[h], best_T = best_Ts[h];
    int best_vis = best_viss[h];
    r.slip_s = best_slips[h];
    r.score = best;
    r.start_s = best_t;
    r.bit_ms = best_T * 1000.0f;
    if (best_vis < 0 || best < FRAME_MIN) goto done;
    if (best < (h ? SLIP_FRAME_MIN : FRAME_MIN)) goto done;
    if (!best_brk && best < NOBREAK_FRAME_MIN) goto done;
    if ((best - second) * 11.0f < CODE_MARGIN) goto done;     /* ambiguous */
    r.near_miss = false;
    if (supported(best_vis, out)) ok = true;
    else r.unsupported_vis = best_vis;

done:
    if (res) *res = r;
    return ok;
}

long sstv_vis_find_end(const float *x, long n, int vis_code, long search_samples)
{
    if (!x || n < VIS_SR) return -1;
    for (long b = 0; b <= search_samples && b + VIS_SR <= n; b += VIS_SR / 20) {
        sstv_info_t info;
        sstv_vis_result_t r;
        bool found = sstv_vis_detect(x + b, VIS_SR, &info, &r);
        int code = found ? info.vis : r.unsupported_vis;
        if (code == vis_code) {
            /* start_s + 0.610 s is the measured leader -> start-bit edge
             * (accurate to ~1 ms); then start bit, 7 data bits, parity and
             * stop bit.  The nominal 300 ms is used for those 10 bits: the
             * bit-period estimate is coarse, and even a 3% slow transmitter
             * moves the end by < 10 ms, far below half a line. */
            double end_s = (double)r.start_s + 0.610 + 0.300 + r.slip_s;
            return b + lround(end_s * VIS_SR);
        }
    }
    return -1;
}
