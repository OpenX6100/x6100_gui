/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY receiver: frame search and decoding in a
 *  block of audio
 *
 *  1. Spectrogram: one-symbol windows every quarter symbol, a 768-point
 *     FFT (bins of half a tone spacing), each bin divided by its own
 *     noise floor.
 *  2. Sync search: at every quarter symbol and half tone, the 13 sync
 *     symbols' power on their tone against the other three. Local peaks,
 *     best first.
 *  3. For each peak: the frame is mixed down and decimated to 1000 Hz
 *     (32 samples a symbol), time and frequency are refined on the sync
 *     symbols, and the four tone energies of the 46 data symbols become
 *     the soft metrics of the tail-biting decoder.
 *  4. A decode counts only if its CRC and the whole source grammar are
 *     valid. Peaks on a frame already decoded are skipped.
 */

#include "jtty_demod.h"

#include <complex.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "jtty_fec.h"

#define HOP         (JTTY_NSPS / 4)         /* spectrogram time step */
#define NFFT        768                     /* half-tone bins */
#define BIN_HZ      ((float)JTTY_RATE / NFFT)

#define DEC         12                      /* 12000 -> 1000 Hz */
#define DRATE       (JTTY_RATE / DEC)
#define DSPS        (JTTY_NSPS / DEC)       /* 32 */
#define NTAPS       121
#define MARGIN      (JTTY_NSPS / 2)         /* refine +-half a symbol */

#define FINE_DF     1.0f                    /* Hz */
#define FINE_NDF    8                       /* +-8 Hz */
#define FINE_NDT    (MARGIN / DEC)          /* +-16 ms in 1 ms steps */

/* ---- FFT: 768 = 3 x 256 -------------------------------------------- */

static float complex tw256[128];
static float complex tw768[NFFT];
static uint8_t       rev256[256];
static float         lpf[NTAPS];
static int           ready;

static void init_tables(void)
{
    if (ready) {
        return;
    }
    for (int k = 0; k < 128; k++) {
        tw256[k] = cexpf(-2.0f * (float)M_PI * I * k / 256);
    }
    for (int k = 0; k < NFFT; k++) {
        tw768[k] = cexpf(-2.0f * (float)M_PI * I * k / NFFT);
    }
    for (int i = 0; i < 256; i++) {
        int r = 0;

        for (int b = 0; b < 8; b++) {
            if (i & (1 << b)) {
                r |= 1 << (7 - b);
            }
        }
        rev256[i] = (uint8_t)r;
    }
    /* Low-pass for the 1000 Hz decimation: 180 Hz cut-off, Blackman. The
     * signal spans +-80 Hz around the centre; what folds onto it from
     * near 1000 Hz is well into the stop band. */
    for (int i = 0; i < NTAPS; i++) {
        double m = i - (NTAPS - 1) / 2.0;
        double fc = 180.0 / JTTY_RATE;
        double h = (m == 0.0) ? 2.0 * fc : sin(2.0 * M_PI * fc * m) / (M_PI * m);
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / (NTAPS - 1)) +
                   0.08 * cos(4.0 * M_PI * i / (NTAPS - 1));

        lpf[i] = (float)(h * w);
    }
    ready = 1;
}

static void fft256(float complex *a)
{
    for (int i = 0; i < 256; i++) {
        int j = rev256[i];

        if (j > i) {
            float complex t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }
    for (int len = 2; len <= 256; len <<= 1) {
        int step = 256 / len;

        for (int i = 0; i < 256; i += len) {
            for (int k = 0; k < len / 2; k++) {
                float complex w = tw256[k * step];
                float complex u = a[i + k];
                float complex v = a[i + k + len / 2] * w;

                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

/* Power of bins 0..NFFT/2-1 of one symbol (JTTY_NSPS real samples,
 * zero-padded to 768). */
static void symbol_power(const float *x, float *pw)
{
    float complex b[3][256];

    for (int r = 0; r < 3; r++) {
        for (int m = 0; m < 256; m++) {
            int i = 3 * m + r;

            b[r][m] = (i < JTTY_NSPS) ? x[i] : 0.0f;
        }
        fft256(b[r]);
    }
    for (int k = 0; k < NFFT / 2; k++) {
        float complex v = b[0][k & 255] + tw768[k] * b[1][k & 255] +
                          tw768[(2 * k) % NFFT] * b[2][k & 255];

        pw[k] = crealf(v) * crealf(v) + cimagf(v) * cimagf(v);
    }
}

/* ---- Candidates ------------------------------------------------------- */

typedef struct {
    int   t;        /* spectrogram time step */
    int   f;        /* bin of tone 0 */
    float score;
} peak_t;

static int cmp_peak(const void *a, const void *b)
{
    float d = ((const peak_t *)b)->score - ((const peak_t *)a)->score;

    return (d > 0) - (d < 0);
}

/* k-th smallest of v[0..n) (reorders v). */
static float select_kth(float *v, int n, int k)
{
    int lo = 0;
    int hi = n - 1;

    while (lo < hi) {
        float pivot = v[(lo + hi) / 2];
        int   i = lo;
        int   j = hi;

        while (i <= j) {
            while (v[i] < pivot) i++;
            while (v[j] > pivot) j--;
            if (i <= j) {
                float t = v[i];
                v[i] = v[j];
                v[j] = t;
                i++;
                j--;
            }
        }
        if (k <= j) {
            hi = j;
        } else if (k >= i) {
            lo = i;
        } else {
            break;
        }
    }
    return v[k];
}

void jtty_search_defaults(jtty_search_t *s, int nsamples)
{
    s->fmin = 200.0f;
    s->fmax = 2800.0f;
    s->start_min = 0;
    s->start_max = nsamples - JTTY_FRAME_SAMPLES;
    /* about 20 sync peaks per second of start times, at least 20 */
    s->max_candidates = 20 + (int)(20.0f * (float)(s->start_max - s->start_min) / JTTY_RATE);
    s->list_size = 8;
    s->min_score = 1.2f;
}

/* ---- One candidate ---------------------------------------------------- */

typedef struct {
    float complex *y;       /* decimated, centred on the four tones */
    int            ny;
} baseband_t;

/* Mixes x[from .. from + len) down by fc and decimates to 1000 Hz. False
 * if out of memory. */
static bool mix_down(const float *x, int n, int from, int len, float fc, baseband_t *bb)
{
    const float complex step = cexpf(-2.0f * (float)M_PI * I * fc / JTTY_RATE);
    float complex      *tmp = malloc(sizeof(float complex) * (size_t)(len + NTAPS));
    float complex       osc = cexpf(-2.0f * (float)M_PI * I * fc * (from - NTAPS / 2) / JTTY_RATE);

    if (tmp == NULL) {
        bb->ny = 0;
        return false;
    }

    for (int i = 0; i < len + NTAPS; i++) {
        int   k = from - NTAPS / 2 + i;
        float v = (k >= 0 && k < n) ? x[k] : 0.0f;

        tmp[i] = v * osc;
        osc *= step;
        if ((i & 1023) == 1023) {
            osc /= cabsf(osc);      /* keep |osc| at 1 */
        }
    }
    bb->ny = len / DEC;
    for (int m = 0; m < bb->ny; m++) {
        float complex acc = 0.0f;
        const float complex *p = tmp + m * DEC;

        for (int j = 0; j < NTAPS; j++) {
            acc += p[j] * lpf[j];
        }
        bb->y[m] = acc;
    }
    free(tmp);
    return true;
}

/* Energy of tone k (0..3, offset df Hz) in the 32 samples from y[m0]. */
static float tone_energy(const float complex *y, int m0, const float complex *rot)
{
    float complex acc = 0.0f;

    for (int m = 0; m < DSPS; m++) {
        acc += y[m0 + m] * rot[m];
    }
    return crealf(acc) * crealf(acc) + cimagf(acc) * cimagf(acc);
}

typedef struct {
    int   dt;               /* decimated samples */
    float df;               /* Hz */
    float sync;             /* sync tone energy / all four */
} fine_t;

static void make_rot(float df, float complex rot[JTTY_TONES][DSPS])
{
    for (int k = 0; k < JTTY_TONES; k++) {
        float f = (k - 1.5f) * JTTY_BAUD + df;

        for (int m = 0; m < DSPS; m++) {
            rot[k][m] = cexpf(-2.0f * (float)M_PI * I * f * m / DRATE);
        }
    }
}

/* Sync energy ratio (sync tone / all four over the 13 sync symbols) at
 * one time and frequency offset. */
static float sync_ratio(const baseband_t *bb, int m_frame, int dt,
                        const float complex rot[JTTY_TONES][DSPS])
{
    float on = 0.0f;
    float all = 0.0f;

    for (int j = 0; j < JTTY_NSYNC; j++) {
        int m0 = m_frame + dt + j * DSPS;

        for (int k = 0; k < JTTY_TONES; k++) {
            float e = tone_energy(bb->y, m0, rot[k]);

            all += e;
            if (k == jtty_sync[j]) {
                on += e;
            }
        }
    }
    return (all > 0.0f) ? on / all : 0.0f;
}

/* Coarse grid first (4 Hz, 4 ms), then 1 Hz and 1 ms around the best. */
static fine_t refine(const baseband_t *bb, int m_frame)
{
    fine_t best = {0, 0.0f, -1.0f};
    float complex rot[JTTY_TONES][DSPS];

    for (int idf = -FINE_NDF; idf <= FINE_NDF; idf += 4) {
        make_rot(idf * FINE_DF, rot);
        for (int dt = -FINE_NDT; dt <= FINE_NDT; dt += 4) {
            float r = sync_ratio(bb, m_frame, dt, rot);

            if (r > best.sync) {
                best = (fine_t){dt, idf * FINE_DF, r};
            }
        }
    }
    {
        fine_t c = best;

        for (int idf = -3; idf <= 3; idf++) {
            float df = c.df + idf * FINE_DF;

            if (fabsf(df) > FINE_NDF * FINE_DF + 2.0f) {
                continue;
            }
            make_rot(df, rot);
            for (int dt = c.dt - 3; dt <= c.dt + 3; dt++) {
                float r;

                if (dt < -FINE_NDT || dt > FINE_NDT) {
                    continue;
                }
                r = sync_ratio(bb, m_frame, dt, rot);
                if (r > best.sync) {
                    best = (fine_t){dt, df, r};
                }
            }
        }
    }
    return best;
}

static bool accept_valid(jtty_payload_t p, void *ctx)
{
    return jtty_source_decode(p, (jtty_atom_t *)ctx);
}

/*
 * A CRC-12 alone lets about one random path in 4096 through, and TEXT5
 * accepts almost any grammar word, so a busy band would show garbage
 * every few minutes. A decode must also fit the signal: the energy on
 * the decoded frame's tones (quality) and on the sync tones. Measured on
 * simulated frames from -17 to -11 dB SNR, this keeps 98.6 % of the true
 * decodes and drops all but one false decode in thirty. A frame expected
 * right after the previous one of a message gets a little more slack.
 */
#define Q_MIN           0.50f
#define Q_MIN_DEEP      0.58f       /* not the best path of the list */
#define SYNC_MIN        0.45f
#define Q_MIN_DEEP_NEXT 0.55f

static bool plausible(const jtty_decode_t *d, bool expected)
{
    if (d->quality < Q_MIN) {
        return false;
    }
    if (expected) {
        return d->rank == 0 || d->quality >= Q_MIN_DEEP_NEXT;
    }
    return d->sync >= SYNC_MIN && (d->rank == 0 || d->quality >= Q_MIN_DEEP);
}

/* Decodes the frame around a sync peak. */
static bool decode_at(const float *x, int n, int start, float f0, int list_size,
                      bool expected, jtty_decode_t *d)
{
    const int     from = start - MARGIN;
    const int     len = JTTY_FRAME_SAMPLES + 2 * MARGIN;
    float complex ybuf[(JTTY_FRAME_SAMPLES + 2 * MARGIN) / DEC + 1];
    baseband_t    bb = {ybuf, 0};
    float         fc = f0 + 1.5f * JTTY_BAUD;
    float complex rot[JTTY_TONES][DSPS];
    float         e[JTTY_NSYM][JTTY_TONES];
    float         metric[JTTY_NDATA][JTTY_TONES];
    float         sum_all = 0.0f;
    float         sum_max = 0.0f;
    float         noise;
    float         sig;
    fine_t        fine;
    int           m_frame = MARGIN / DEC;
    jtty_atom_t   atom;

    if (!mix_down(x, n, from, len, fc, &bb)) {
        return false;
    }
    fine = refine(&bb, m_frame);
    if (!expected && fine.sync < SYNC_MIN) {
        return false;               /* no FEC for what plausible() drops */
    }
    make_rot(fine.df, rot);

    for (int s = 0; s < JTTY_NSYM; s++) {
        float mx = 0.0f;

        for (int k = 0; k < JTTY_TONES; k++) {
            e[s][k] = tone_energy(bb.y, m_frame + fine.dt + s * DSPS, rot[k]);
            sum_all += e[s][k];
            if (e[s][k] > mx) {
                mx = e[s][k];
            }
        }
        sum_max += mx;
    }
    /* Noise per tone bin: what is not the strongest tone. */
    noise = (sum_all - sum_max) / (3.0f * JTTY_NSYM);
    if (noise <= 0.0f) {
        return false;
    }
    for (int t = 0; t < JTTY_NDATA; t++) {
        for (int k = 0; k < JTTY_TONES; k++) {
            metric[t][k] = e[JTTY_NSYNC + t][k] / noise;
        }
    }

    if (!jtty_fec_decode(metric, list_size, accept_valid, &atom, &d->payload, &d->rank)) {
        return false;
    }
    /* accept_valid() rendered the accepted path last */
    jtty_source_decode(d->payload, &d->atom);

    /* How much of the frame's energy lies on the tones of the decoded
     * frame: near 1 for a clean signal, about 0.25 for noise. */
    {
        uint8_t tones[JTTY_NSYM];
        float   on = 0.0f;

        jtty_frame_tones(d->payload, tones);
        for (int s = 0; s < JTTY_NSYM; s++) {
            on += e[s][tones[s]];
        }
        d->quality = on / sum_all;
    }

    sig = sum_max / JTTY_NSYM - noise;
    d->snr = 10.0f * log10f(fmaxf(sig, noise * 1e-3f) / noise) - 10.0f * log10f(2500.0f / JTTY_BAUD);
    d->start = start + fine.dt * DEC;
    d->freq = f0 + fine.df;
    d->sync = fine.sync;
    return plausible(d, expected);
}

/* ---- Search ----------------------------------------------------------- */

bool jtty_demod_copy(int64_t a_start, jtty_payload_t a_payload,
                     int64_t b_start, jtty_payload_t b_payload)
{
    int64_t dt = a_start - b_start;

    return a_payload == b_payload && (dt < 0 ? -dt : dt) < 2 * JTTY_NSPS;
}

bool jtty_demod_ghost(int64_t weak_start, float weak_freq, float weak_q, float weak_snr,
                      int64_t strong_start, float strong_freq, float strong_q, float strong_snr)
{
    int64_t dt = weak_start - strong_start;
    float   df = fabsf(weak_freq - strong_freq);

    if (dt < 0) {
        dt = -dt;
    }
    if (dt >= JTTY_FRAME_SAMPLES - JTTY_NSPS) {
        return false;                   /* not at the same time */
    }
    if (df < JTTY_BW_HZ && strong_q > weak_q) {
        return true;
    }
    return df < 2.0f * JTTY_BW_HZ && strong_snr - weak_snr >= 10.0f;
}

/* A refined decode may land on a frame already found. */
static bool already(const jtty_decode_t *out, int nout, const jtty_decode_t *d)
{
    for (int k = 0; k < nout; k++) {
        if (abs(out[k].start - d->start) < JTTY_NSPS / 2 &&
            fabsf(out[k].freq - d->freq) < JTTY_BAUD / 2) {
            return true;
        }
    }
    return false;
}

int jtty_demod_search(const float *x, int n, const jtty_search_t *s,
                      jtty_decode_t *out, int max_out)
{
    int     nt;
    int     f_lo;
    int     f_hi;
    int     nf;
    float  *spec;
    float  *col;
    peak_t *peaks;
    int     npeaks = 0;
    int     cap;
    int     nout = 0;
    int     base;
    const float *x_all = x;
    const int    n_all = n;

    init_tables();
    if (n < JTTY_FRAME_SAMPLES || s->start_max < s->start_min) {
        return 0;
    }

    /* Spectrogram columns: from the earliest start searched to the end
     * of the latest frame. A frame start is a whole number of columns. */
    {
        int t_first = s->start_min > 0 ? s->start_min / HOP : 0;

        x += t_first * HOP;
        n -= t_first * HOP;
        base = t_first * HOP;
    }
    nt = (n - JTTY_NSPS) / HOP + 1;
    if (nt > (s->start_max - base) / HOP + 4 * (JTTY_NSYM - 1) + 1) {
        nt = (s->start_max - base) / HOP + 4 * (JTTY_NSYM - 1) + 1;
    }
    f_lo = (int)floorf(s->fmin / BIN_HZ);
    f_hi = (int)ceilf(s->fmax / BIN_HZ) + 2 * (JTTY_TONES - 1);
    if (f_lo < 1) {
        f_lo = 1;
    }
    if (f_hi > NFFT / 2 - 1) {
        f_hi = NFFT / 2 - 1;
    }
    nf = f_hi - f_lo + 1;

    spec = malloc(sizeof(float) * (size_t)nt * (size_t)nf);
    col = malloc(sizeof(float) * (size_t)(nt > NFFT ? nt : NFFT));
    if (spec == NULL || col == NULL) {
        free(spec);
        free(col);
        return 0;
    }

    for (int t = 0; t < nt; t++) {
        symbol_power(x + t * HOP, col);
        memcpy(spec + (size_t)t * nf, col + f_lo, sizeof(float) * (size_t)nf);
    }

    /* Each bin over its own noise level, so a sloping pass band or a
     * steady carrier does not dominate. The level is the 40th percentile
     * in time, which signals hardly touch; for noise power (exponential)
     * that is 0.51 of the mean, so the result has a noise mean of 1. */
    for (int f = 0; f < nf; f++) {
        float floor_v;

        for (int t = 0; t < nt; t++) {
            col[t] = spec[(size_t)t * nf + f];
        }
        floor_v = select_kth(col, nt, (nt * 2) / 5) / 0.51f;
        if (floor_v <= 0.0f) {
            floor_v = 1e-12f;
        }
        for (int t = 0; t < nt; t++) {
            spec[(size_t)t * nf + f] /= floor_v;
        }
    }

    /* Sync score at every (t, f): power on the sync tone against the
     * mean of the other three, over the 13 sync symbols. */
    {
        int    t_lo = (s->start_min - base + HOP - 1) / HOP;
        int    t_hi = (s->start_max - base) / HOP;
        int    f_top = nf - 1 - 2 * (JTTY_TONES - 1);
        int    ft = f_top + 1;
        int    tt;
        float *score;

        if (t_lo < 0) {
            t_lo = 0;
        }
        if (t_hi > nt - 1 - 4 * (JTTY_NSYM - 1)) {
            t_hi = nt - 1 - 4 * (JTTY_NSYM - 1);
        }
        tt = t_hi - t_lo + 1;
        if (tt <= 0 || ft <= 0) {
            free(spec);
            free(col);
            return 0;
        }
        score = malloc(sizeof(float) * (size_t)tt * (size_t)ft);
        cap = s->max_candidates * 4;
        peaks = malloc(sizeof(peak_t) * (size_t)cap);
        if (score == NULL || peaks == NULL) {
            free(score);
            free(peaks);
            free(spec);
            free(col);
            return 0;
        }

        for (int t = 0; t < tt; t++) {
            for (int f = 0; f < ft; f++) {
                float sc = 0.0f;

                for (int j = 0; j < JTTY_NSYNC; j++) {
                    const float *row = spec + (size_t)(t + t_lo + 4 * j) * nf + f;
                    float        on = row[2 * jtty_sync[j]];
                    float        all = row[0] + row[2] + row[4] + row[6];

                    sc += on - (all - on) / 3.0f;
                }
                score[(size_t)t * ft + f] = sc / JTTY_NSYNC;
            }
        }

        /* local maxima over +-2 time steps and +-1 bin */
        for (int t = 0; t < tt; t++) {
            for (int f = 0; f < ft; f++) {
                float v = score[(size_t)t * ft + f];
                bool  peak = v > s->min_score;

                for (int dt = -2; peak && dt <= 2; dt++) {
                    for (int df = -1; peak && df <= 1; df++) {
                        int t2 = t + dt;
                        int f2 = f + df;

                        if ((dt || df) && t2 >= 0 && t2 < tt && f2 >= 0 && f2 < ft &&
                            score[(size_t)t2 * ft + f2] > v) {
                            peak = false;
                        }
                    }
                }
                if (!peak) {
                    continue;
                }
                if (npeaks < cap) {
                    peaks[npeaks].t = t + t_lo;
                    peaks[npeaks].f = f;
                    peaks[npeaks].score = v;
                    npeaks++;
                } else {
                    /* full: replace the weakest */
                    int w = 0;

                    for (int i = 1; i < npeaks; i++) {
                        if (peaks[i].score < peaks[w].score) {
                            w = i;
                        }
                    }
                    if (v > peaks[w].score) {
                        peaks[w].t = t + t_lo;
                        peaks[w].f = f;
                        peaks[w].score = v;
                    }
                }
            }
        }
        free(score);
    }
    free(spec);
    free(col);

    qsort(peaks, (size_t)npeaks, sizeof(peak_t), cmp_peak);
    if (npeaks > s->max_candidates) {
        npeaks = s->max_candidates;
    }

    for (int i = 0; i < npeaks && nout < max_out; i++) {
        int           start = base + peaks[i].t * HOP;
        float         f0 = (peaks[i].f + f_lo) * BIN_HZ;
        bool          dup = false;
        jtty_decode_t d;

        if (f0 < s->fmin || f0 > s->fmax) {
            continue;
        }
        for (int k = 0; k < nout; k++) {
            if (abs(out[k].start - start) < JTTY_NSPS && fabsf(out[k].freq - f0) < JTTY_BAUD) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (decode_at(x_all, n_all, start, f0, s->list_size, false, &d) && !already(out, nout, &d)) {
            out[nout++] = d;
        }
    }
    free(peaks);

    /* Frames of one message follow each other exactly: after a frame
     * that is not the last, look for the next one where it must be,
     * with the deepest list. This finds frames too weak to stand out in
     * the sync search. */
    for (int i = 0; i < nout && nout < max_out; i++) {
        jtty_decode_t d;
        int           next = out[i].start + JTTY_FRAME_SAMPLES;

        if (out[i].atom.eom || next < s->start_min || next > s->start_max) {
            continue;
        }
        if (jtty_demod_try(x_all, n_all, next, out[i].freq, &d) && !already(out, nout, &d)) {
            out[nout++] = d;
        }
    }

    /* Next to a strong signal a candidate shifted in time or a few tones
     * off can pass every check; two such frames cannot both be real. And
     * a weaker copy of a frame elsewhere in the band is the same frame. */
    for (int i = 0; i < nout; i++) {
        for (int j = 0; j < nout; j++) {
            if (i == j) {
                continue;
            }
            if (jtty_demod_ghost(out[i].start, out[i].freq, out[i].quality, out[i].snr,
                                 out[j].start, out[j].freq, out[j].quality, out[j].snr) ||
                (jtty_demod_copy(out[i].start, out[i].payload, out[j].start, out[j].payload) &&
                 out[j].snr > out[i].snr)) {
                out[i--] = out[--nout];
                break;
            }
        }
    }
    return nout;
}

bool jtty_demod_try(const float *x, int n, int start, float freq, jtty_decode_t *d)
{
    init_tables();
    if (start < 0 || start + JTTY_FRAME_SAMPLES > n) {
        return false;
    }
    return decode_at(x, n, start, freq, 32, true, d);
}
