/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  WSPR demodulator and synchroniser
 */

#include "wspr_demod.h"
#include "wspr_fano.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NFFT WSPR_WORK_SYMBOL                              /* 256      */
#define BIN_HZ ((double)WSPR_WORK_RATE / NFFT)             /* 1.4648   */

/*
 * Frequency sub-steps within one bin. Without them a signal sitting
 * between bins would split its energy across two of them and lose
 * several dB. Four sub-steps leave a worst case error of 1/8 bin.
 */
#define FREQ_SUBSTEPS 4

/* Time sub-steps within one symbol. */
#define TIME_SUBSTEPS 4

/*
 * How many symbol periods of start time to search, from wherever the
 * caller points the search. Five symbols (3.4 s) with the caller
 * starting one second early cover a start time error of -1.0..+2.4 s,
 * about what WSJT-X's wsprd accepts. Stations with a badly set clock are
 * common on WSPR; a two symbol search from half a symbol early
 * (-0.34..+1.0 s) misses many of them.
 */
#ifndef TIME_SEARCH_SYMBOLS
#define TIME_SEARCH_SYMBOLS 5
#endif

/*
 * Decimation filter length. The passband must reach the search window
 * edge (about 60 Hz) and the stopband must start at 315 Hz, because
 * anything above that folds back onto the signal after decimating to
 * 375 Hz. That transition width needs roughly 160 taps with a Hamming
 * window at 12000 Hz.
 */
#define FIR_TAPS 160

/* ------------------------------------------------------------------ */
/* FFT                                                                 */

typedef struct {
    int n;
    int *rev;
    float *cos_t;
    float *sin_t;
} fft_t;

static int fft_init(fft_t *f, int n)
{
    int bits = 0, i, j, t = n;

    while (t > 1) {
        t >>= 1;
        bits++;
    }
    if ((1 << bits) != n)
        return -1;

    f->n = n;
    f->rev = malloc(sizeof(int) * n);
    f->cos_t = malloc(sizeof(float) * (n / 2));
    f->sin_t = malloc(sizeof(float) * (n / 2));
    if (!f->rev || !f->cos_t || !f->sin_t)
        return -1;

    for (i = 0; i < n; i++) {
        int r = 0;

        for (j = 0; j < bits; j++)
            if (i & (1 << j))
                r |= 1 << (bits - 1 - j);
        f->rev[i] = r;
    }
    for (i = 0; i < n / 2; i++) {
        f->cos_t[i] = (float)cos(2.0 * M_PI * i / n);
        f->sin_t[i] = (float)sin(2.0 * M_PI * i / n);
    }
    return 0;
}

static void fft_free(fft_t *f)
{
    free(f->rev);
    free(f->cos_t);
    free(f->sin_t);
    memset(f, 0, sizeof(*f));
}

static void fft_run(const fft_t *f, float *re, float *im)
{
    int n = f->n, step, i, k;

    for (i = 0; i < n; i++) {
        int r = f->rev[i];

        if (r > i) {
            float tmp = re[i]; re[i] = re[r]; re[r] = tmp;
            tmp = im[i]; im[i] = im[r]; im[r] = tmp;
        }
    }

    for (step = 1; step < n; step <<= 1) {
        int jump = step << 1;
        int tw = n / jump;

        for (k = 0; k < step; k++) {
            float wr = f->cos_t[k * tw];
            float wi = -f->sin_t[k * tw];

            for (i = k; i < n; i += jump) {
                int j = i + step;
                float tr = wr * re[j] - wi * im[j];
                float ti = wr * im[j] + wi * re[j];

                re[j] = re[i] - tr;
                im[j] = im[i] - ti;
                re[i] += tr;
                im[i] += ti;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Mixing and decimation                                               */

static void design_lowpass(float *taps, int n, double cutoff_hz,
                           double sample_rate)
{
    double fc = cutoff_hz / sample_rate;
    int m = n - 1;
    double sum = 0.0;
    int i;

    for (i = 0; i < n; i++) {
        double k = (double)i - m / 2.0;
        double sinc = (fabs(k) < 1e-9) ? (2.0 * fc)
                                       : sin(2.0 * M_PI * fc * k) / (M_PI * k);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / m);

        taps[i] = (float)(sinc * w);
        sum += taps[i];
    }
    if (sum != 0.0)
        for (i = 0; i < n; i++)
            taps[i] = (float)(taps[i] / sum);
}

int wspr_bb_init(wspr_bb_t *bb, double center_hz)
{
    if (!bb || center_hz <= 0.0)
        return -1;

    memset(bb, 0, sizeof(*bb));
    bb->center_hz = center_hz;
    bb->phase = 0.0;
    bb->phase_step = -2.0 * M_PI * center_hz / WSPR_DEMOD_RATE;

    /* Stopband must start at 315 Hz: anything above that folds onto the
     * signal once we decimate to 375 Hz. The passband only has to reach
     * the edge of the search window. */
    design_lowpass(bb->taps, FIR_TAPS, 140.0, WSPR_DEMOD_RATE);
    return 0;
}

long wspr_bb_process(wspr_bb_t *bb, const float *in, long n,
                     float *out_re, float *out_im, long cap)
{
    long i, out_n = 0;

    if (!bb || !in || !out_re || !out_im)
        return 0;

    for (i = 0; i < n && out_n < cap; i++) {
        float c = (float)cos(bb->phase);
        float s = (float)sin(bb->phase);

        bb->hist_re[bb->hist_pos] = in[i] * c;
        bb->hist_im[bb->hist_pos] = in[i] * s;
        bb->hist_pos = (bb->hist_pos + 1) % FIR_TAPS;
        if (bb->filled < FIR_TAPS)
            bb->filled++;

        bb->phase += bb->phase_step;
        if (bb->phase < -2.0 * M_PI)
            bb->phase += 2.0 * M_PI;

        if (++bb->decim_count >= WSPR_DECIM) {
            bb->decim_count = 0;

            if (bb->filled >= FIR_TAPS) {
                double acc_re = 0.0, acc_im = 0.0;
                int j, idx = bb->hist_pos;

                for (j = 0; j < FIR_TAPS; j++) {
                    acc_re += bb->hist_re[idx] * bb->taps[j];
                    acc_im += bb->hist_im[idx] * bb->taps[j];
                    idx = (idx + 1) % FIR_TAPS;
                }
                out_re[out_n] = (float)acc_re;
                out_im[out_n] = (float)acc_im;
                out_n++;
            }
        }
    }

    return out_n;
}

/* ------------------------------------------------------------------ */

/*
 * Sync vector match for a given base bin.
 *
 * A symbol is sync + 2 * data, so:
 *   sync = 0 -> tone 0 or 2
 *   sync = 1 -> tone 1 or 3
 * We add the energy in tones consistent with the known sync vector and
 * subtract the energy in inconsistent ones.
 *
 * The result is normalised by the energy of the WHOLE window rather
 * than by the four bins. That matters: with local normalisation a two
 * bin offset produces a spurious perfect score, because the bins then
 * treated as "wrong" land on tones that never occur, so the error term
 * is zero even though only half the energy is captured. That bug was
 * real and showed up as a signal exactly on 1500 Hz failing to decode
 * while one detuned by 10 Hz decoded fine.
 */
static float sync_metric(const float *mag, int n_bins, int b,
                         float energy_ref)
{
    float good = 0.0f, bad = 0.0f;
    int k;

    if (b < 0 || b + 3 >= n_bins)
        return -1e30f;

    for (k = 0; k < WSPR_SYMBOLS; k++) {
        const float *m = mag + (size_t)k * n_bins;
        int s = wspr_sync_vector[k];

        good += m[b + s] + m[b + s + 2];
        bad  += m[b + (1 - s)] + m[b + (3 - s)];
    }

    if (energy_ref < 1e-12f)
        return -1e30f;

    return (good - bad) / energy_ref;
}

/*
 * Per symbol magnitude spectra for one time and fine frequency offset.
 */
static void analyze(const fft_t *fft,
                    const float *bb_re, const float *bb_im, long bb_n,
                    long time_offset, double freq_shift_hz,
                    int bin_lo, int n_bins, float *mag)
{
    float re[NFFT], im[NFFT];
    int k, i;

    for (k = 0; k < WSPR_SYMBOLS; k++) {
        long base = time_offset + (long)k * WSPR_WORK_SYMBOL;

        for (i = 0; i < NFFT; i++) {
            long idx = base + i;
            float sr = 0.0f, si = 0.0f;
            double ph, c, s;

            if (idx >= 0 && idx < bb_n) {
                sr = bb_re[idx];
                si = bb_im[idx];
            }

            /* Fine frequency shift, phase relative to symbol start.
             * A constant phase step between symbols does not matter
             * because detection is non coherent. */
            ph = -2.0 * M_PI * freq_shift_hz * i / WSPR_WORK_RATE;
            c = cos(ph);
            s = sin(ph);

            re[i] = (float)(sr * c - si * s);
            im[i] = (float)(sr * s + si * c);
        }

        fft_run(fft, re, im);

        for (i = 0; i < n_bins; i++) {
            int b = bin_lo + i;
            float rr, ii;

            /* Negative frequencies live in the upper half of the FFT. */
            if (b < 0)
                b += NFFT;
            if (b >= NFFT)
                b -= NFFT;

            rr = re[b];
            ii = im[b];
            mag[(size_t)k * n_bins + i] = sqrtf(rr * rr + ii * ii);
        }
    }
}

/* ------------------------------------------------------------------ */


/*
 * Turn one symbol's magnitudes into soft values for the data bit.
 *
 *   sync = 0: tone 0 (data 0) against tone 2 (data 1)
 *   sync = 1: tone 1 (data 0) against tone 3 (data 1)
 *
 * The difference is divided by the total energy of the four bins in that
 * symbol. Without this per symbol normalisation a single strong burst of
 * interference would dominate the whole block, because the Fano decoder
 * scales its metric by the mean over all 162 values.
 */
static void soft_from_mag(const float *mag, int n_bins, int base_bin,
                          float soft162[WSPR_SYMBOLS])
{
    float interleaved[WSPR_SYMBOLS];
    int k;

    for (k = 0; k < WSPR_SYMBOLS; k++) {
        const float *m = mag + (size_t)k * n_bins;
        int s = wspr_sync_vector[k];
        float tot = m[base_bin] + m[base_bin + 1] +
                    m[base_bin + 2] + m[base_bin + 3] + 1e-9f;

        interleaved[k] = (m[base_bin + s + 2] - m[base_bin + s]) / tot;
    }

    wspr_deinterleave_soft(interleaved, soft162);
}

/* Estimated SNR in the 2500 Hz reference band that WSPR reports. */
static float estimate_snr(const float *mag, int n_bins, int base_bin)
{
    float sig = 0.0f, noise = 1e-12f;
    float *sorted;
    int k, j;

    for (k = 0; k < WSPR_SYMBOLS; k++) {
        const float *m = mag + (size_t)k * n_bins;
        int s = wspr_sync_vector[k];

        sig += m[base_bin + s] * m[base_bin + s] +
               m[base_bin + s + 2] * m[base_bin + s + 2];
    }
    sig /= WSPR_SYMBOLS;

    sorted = malloc(sizeof(float) * n_bins);
    if (sorted) {
        for (j = 0; j < n_bins; j++) {
            float acc = 0.0f;

            for (k = 0; k < WSPR_SYMBOLS; k++) {
                float v = mag[(size_t)k * n_bins + j];

                acc += v * v;
            }
            sorted[j] = acc / WSPR_SYMBOLS;
        }
        /* Median bin as the noise floor: robust against another WSPR
         * signal sitting a few Hz away. */
        for (j = 0; j < n_bins; j++) {
            int q;

            for (q = j + 1; q < n_bins; q++)
                if (sorted[q] < sorted[j]) {
                    float t = sorted[j];

                    sorted[j] = sorted[q];
                    sorted[q] = t;
                }
        }
        noise = sorted[n_bins / 2];
        if (noise < 1e-12f)
            noise = 1e-12f;
        free(sorted);
    }

    /* 32.3 dB converts the 1.4648 Hz analysis bandwidth to 2500 Hz. */
    return (float)(10.0 * log10((sig - noise > 0 ? sig - noise : 1e-12) /
                                noise) - 32.3);
}

int wspr_demod_decode_all(const float *bb_re, const float *bb_im, long bb_n,
                          double center_hz, double search_hz,
                          int max_spots, wspr_decode_cb cb, void *ctx)
{
    typedef struct {
        float metric;
        int ti;
        int fi;
    } bin_best_t;

    fft_t fft;
    float *mag = NULL;
    bin_best_t *best = NULL;
    int *order = NULL;
    char *taken = NULL;
    int n_bins, bin_lo;
    int ti, fi, b, i, j;
    int found = 0;
    int attempts = 0;
    int last_ti = -1, last_fi = -1;

    if (!bb_re || !bb_im || !cb)
        return 0;
    if (bb_n < (long)WSPR_SYMBOLS * WSPR_WORK_SYMBOL)
        return 0;
    if (max_spots <= 0)
        max_spots = 16;

    if (fft_init(&fft, NFFT) != 0)
        return 0;

    n_bins = (int)(2.0 * search_hz / BIN_HZ) + 8;
    if (n_bins > NFFT)
        n_bins = NFFT;
    bin_lo = -n_bins / 2;

    mag = malloc(sizeof(float) * WSPR_SYMBOLS * n_bins);
    best = malloc(sizeof(bin_best_t) * n_bins);
    order = malloc(sizeof(int) * n_bins);
    taken = calloc(n_bins, 1);
    if (!mag || !best || !order || !taken)
        goto done;

    for (b = 0; b < n_bins; b++) {
        best[b].metric = -1e30f;
        best[b].ti = 0;
        best[b].fi = 0;
    }

    /* Stage one: score every bin over all time and frequency offsets. */
    for (ti = 0; ti < TIME_SUBSTEPS * TIME_SEARCH_SYMBOLS; ti++) {
        long toff = (long)ti * WSPR_WORK_SYMBOL / TIME_SUBSTEPS;

        if (toff + (long)WSPR_SYMBOLS * WSPR_WORK_SYMBOL > bb_n)
            break;

        for (fi = 0; fi < FREQ_SUBSTEPS; fi++) {
            double fshift = fi * BIN_HZ / FREQ_SUBSTEPS;
            float energy_ref;
            double e = 0.0;
            long q, total;

            analyze(&fft, bb_re, bb_im, bb_n, toff, fshift,
                    bin_lo, n_bins, mag);

            total = (long)WSPR_SYMBOLS * n_bins;
            for (q = 0; q < total; q++)
                e += mag[q];
            energy_ref = (float)(e / n_bins);

            for (b = 0; b + 3 < n_bins; b++) {
                float m = sync_metric(mag, n_bins, b, energy_ref);

                if (m > best[b].metric) {
                    best[b].metric = m;
                    best[b].ti = ti;
                    best[b].fi = fi;
                }
            }
        }
    }

    /* Rank bins by how well they matched the sync vector. */
    for (b = 0; b < n_bins; b++)
        order[b] = b;
    for (i = 0; i < n_bins; i++)
        for (j = i + 1; j < n_bins; j++)
            if (best[order[j]].metric > best[order[i]].metric) {
                int t = order[i];

                order[i] = order[j];
                order[j] = t;
            }

    /*
     * Stage two: decode the strongest candidates in turn.
     *
     * The limit counts ATTEMPTS, not successes. Bins holding only noise
     * still cost a full Fano search before the decoder gives up, and on
     * a quiet band nearly every bin is noise, so limiting successes
     * would let the loop grind through the whole window.
     */
    for (i = 0; i < n_bins && attempts < max_spots; i++) {
        int cand_bin = order[i];
        float soft[WSPR_SYMBOLS];
        char callsign[8], locator[8];
        int power = 0, errors = 0;
        wspr_candidate_t cand;
        long toff;
        double fshift;
        int skip = 0;

        if (best[cand_bin].metric <= -1e29f)
            continue;
        if (cand_bin + 3 >= n_bins)
            continue;

        /*
         * A signal occupies four bins, and a neighbouring bin will score
         * almost as well, so skip anything overlapping one already
         * decoded. Otherwise the same station is reported repeatedly.
         */
        for (j = -3; j <= 3 && !skip; j++) {
            int q = cand_bin + j;

            if (q >= 0 && q < n_bins && taken[q])
                skip = 1;
        }
        if (skip)
            continue;

        toff = (long)best[cand_bin].ti * WSPR_WORK_SYMBOL / TIME_SUBSTEPS;
        fshift = best[cand_bin].fi * BIN_HZ / FREQ_SUBSTEPS;

        /*
         * Candidates are ranked by score, but several often share the
         * same time and frequency offset, so recomputing the spectra
         * only when the combination actually changes saves most of the
         * work in stage two.
         */
        if (best[cand_bin].ti != last_ti || best[cand_bin].fi != last_fi) {
            analyze(&fft, bb_re, bb_im, bb_n, toff, fshift,
                    bin_lo, n_bins, mag);
            last_ti = best[cand_bin].ti;
            last_fi = best[cand_bin].fi;
        }

        soft_from_mag(mag, n_bins, cand_bin, soft);
        attempts++;

        {
            uint8_t bits[WSPR_INFO_BITS];
            int bad;

            /*
             * Cap the decoder's effort. A real signal at the working
             * point needs well under 20000 nodes; anything beyond that
             * is almost certainly noise, and without a cap each empty
             * bin would burn the full 200000 node budget.
             */
            if (wspr_fano_decode(soft, bits, 30000, NULL) != 0)
                continue;

            bad = wspr_verify(soft, bits);
            if (bad > WSPR_VERIFY_MAX_ERRORS)
                continue;

            if (wspr_unpack(bits, callsign, locator, &power) != 0)
                continue;

            errors = bad;
        }

        memset(&cand, 0, sizeof(cand));
        cand.freq_hz = center_hz + (bin_lo + cand_bin) * BIN_HZ + fshift;
        cand.time_offset_s = (double)best[cand_bin].ti * WSPR_WORK_SYMBOL /
                             TIME_SUBSTEPS / WSPR_WORK_RATE;
        cand.sync_quality = best[cand_bin].metric;
        cand.snr_db = estimate_snr(mag, n_bins, cand_bin);

        for (j = 0; j <= 3; j++)
            if (cand_bin + j < n_bins)
                taken[cand_bin + j] = 1;

        cb(&cand, callsign, locator, power, errors, ctx);
        found++;
    }

done:
    free(mag);
    free(best);
    free(order);
    free(taken);
    fft_free(&fft);
    return found;
}

int wspr_demod_search_bb(const float *bb_re, const float *bb_im, long bb_n,
                         double center_hz, double search_hz,
                         float soft162[WSPR_SYMBOLS],
                         wspr_candidate_t *cand)
{
    fft_t fft;
    float *mag = NULL, *best_mag = NULL;
    int n_bins, bin_lo;
    float best_metric = -1e30f;
    int best_bin = -1, best_t = 0, best_f = 0;
    int ti, fi, b;
    int rc = -1;

    if (!bb_re || !bb_im || !soft162)
        return -1;
    if (bb_n < (long)WSPR_SYMBOLS * WSPR_WORK_SYMBOL)
        return -1;
    if (search_hz < 0.0)
        return -1;

    if (fft_init(&fft, NFFT) != 0)
        return -1;

    n_bins = (int)(2.0 * search_hz / BIN_HZ) + 8;
    if (n_bins > NFFT)
        n_bins = NFFT;
    bin_lo = -n_bins / 2;

    mag = malloc(sizeof(float) * WSPR_SYMBOLS * n_bins);
    best_mag = malloc(sizeof(float) * WSPR_SYMBOLS * n_bins);
    if (!mag || !best_mag)
        goto done;

    for (ti = 0; ti < TIME_SUBSTEPS * TIME_SEARCH_SYMBOLS; ti++) {
        long toff = (long)ti * WSPR_WORK_SYMBOL / TIME_SUBSTEPS;

        /* Do not run off the end of the stored baseband. */
        if (toff + (long)WSPR_SYMBOLS * WSPR_WORK_SYMBOL > bb_n)
            break;

        for (fi = 0; fi < FREQ_SUBSTEPS; fi++) {
            double fshift = fi * BIN_HZ / FREQ_SUBSTEPS;
            float energy_ref;
            double e = 0.0;
            long q, total;

            analyze(&fft, bb_re, bb_im, bb_n, toff, fshift,
                    bin_lo, n_bins, mag);

            total = (long)WSPR_SYMBOLS * n_bins;
            for (q = 0; q < total; q++)
                e += mag[q];
            energy_ref = (float)(e / n_bins);

            for (b = 0; b + 3 < n_bins; b++) {
                float q2 = sync_metric(mag, n_bins, b, energy_ref);

                if (q2 > best_metric) {
                    best_metric = q2;
                    best_bin = b;
                    best_t = ti;
                    best_f = fi;
                    memcpy(best_mag, mag,
                           sizeof(float) * WSPR_SYMBOLS * n_bins);
                }
            }
        }
    }

    if (best_bin < 0)
        goto done;

    /*
     * Soft values. For each symbol the data bit is decided between the
     * two tones consistent with the known sync bit:
     *   sync = 0: tone 0 (data 0) against tone 2 (data 1)
     *   sync = 1: tone 1 (data 0) against tone 3 (data 1)
     *
     * The difference is divided by the total energy of the four bins in
     * that symbol. Without this per symbol normalisation a single
     * strong interference burst would dominate the whole block, because
     * the Fano decoder scales its metric by the mean over all 162
     * values.
     */
    {
        float interleaved[WSPR_SYMBOLS];
        int k;

        for (k = 0; k < WSPR_SYMBOLS; k++) {
            const float *m = best_mag + (size_t)k * n_bins;
            int s = wspr_sync_vector[k];
            float tot = m[best_bin] + m[best_bin + 1] +
                        m[best_bin + 2] + m[best_bin + 3] + 1e-9f;

            interleaved[k] = (m[best_bin + s + 2] - m[best_bin + s]) / tot;
        }

        wspr_deinterleave_soft(interleaved, soft162);
    }

    if (cand) {
        /*
         * SNR estimate in the 2500 Hz reference band that WSPR reports.
         *
         * Signal power is what sits in the four tone bins; the noise
         * floor is taken as the median bin of the window, which is
         * robust against another WSPR signal sitting a few Hz away.
         * The 32.3 dB term converts from the 1.4648 Hz analysis
         * bandwidth to the 2500 Hz reference.
         */
        float sig = 0.0f, noise;
        float *sorted = malloc(sizeof(float) * n_bins);
        int k, j;

        for (k = 0; k < WSPR_SYMBOLS; k++) {
            const float *m = best_mag + (size_t)k * n_bins;
            int s = wspr_sync_vector[k];

            sig += m[best_bin + s] * m[best_bin + s] +
                   m[best_bin + s + 2] * m[best_bin + s + 2];
        }
        sig /= WSPR_SYMBOLS;

        noise = 1e-12f;
        if (sorted) {
            for (j = 0; j < n_bins; j++) {
                float acc = 0.0f;

                for (k = 0; k < WSPR_SYMBOLS; k++) {
                    float v = best_mag[(size_t)k * n_bins + j];

                    acc += v * v;
                }
                sorted[j] = acc / WSPR_SYMBOLS;
            }
            for (j = 0; j < n_bins; j++) {
                int q;

                for (q = j + 1; q < n_bins; q++)
                    if (sorted[q] < sorted[j]) {
                        float t = sorted[j];

                        sorted[j] = sorted[q];
                        sorted[q] = t;
                    }
            }
            noise = sorted[n_bins / 2];
            if (noise < 1e-12f)
                noise = 1e-12f;
            free(sorted);
        }

        cand->snr_db = (float)(10.0 * log10((sig - noise > 0 ? sig - noise
                                                             : 1e-12) / noise)
                               - 32.3);

        cand->freq_hz = center_hz + (bin_lo + best_bin) * BIN_HZ +
                        best_f * BIN_HZ / FREQ_SUBSTEPS;
        cand->time_offset_s = (double)best_t * WSPR_WORK_SYMBOL /
                              TIME_SUBSTEPS / WSPR_WORK_RATE;
        cand->sync_quality = best_metric;
    }

    rc = 0;

done:
    free(mag);
    free(best_mag);
    fft_free(&fft);
    return rc;
}

/*
 * Convenience wrapper for offline use and tests: converts the whole
 * audio buffer to baseband in one go, then searches it. The radio uses
 * wspr_bb_process() incrementally instead, to avoid holding the audio.
 */
int wspr_demod_search(const float *audio, long n_samples,
                      double center_hz, double search_hz,
                      float soft162[WSPR_SYMBOLS],
                      wspr_candidate_t *cand)
{
    wspr_bb_t bb;
    float *bb_re = NULL, *bb_im = NULL;
    long bb_cap, bb_n;
    int rc = -1;

    if (!audio || n_samples < WSPR_TX_SAMPLES || center_hz <= 0.0)
        return -1;

    bb_cap = n_samples / WSPR_DECIM + 1;
    bb_re = malloc(sizeof(float) * bb_cap);
    bb_im = malloc(sizeof(float) * bb_cap);
    if (!bb_re || !bb_im)
        goto done;

    if (wspr_bb_init(&bb, center_hz) != 0)
        goto done;

    bb_n = wspr_bb_process(&bb, audio, n_samples, bb_re, bb_im, bb_cap);

    rc = wspr_demod_search_bb(bb_re, bb_im, bb_n, center_hz, search_hz,
                              soft162, cand);

done:
    free(bb_re);
    free(bb_im);
    return rc;
}
