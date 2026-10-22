/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - PSK31 spectrum for the waterfall display
 *
 *  Separate from psk_rx.c on purpose: a display feature has no business
 *  reaching into the decoder. It is fed the same audio the decoder gets
 *  (PSK_RX_RATE), through a tap, and costs one 2048 point FFT per line.
 *
 *  2048 points at 12000 Hz give 5.86 Hz bins, so a PSK31 signal (about
 *  60 Hz wide) spans roughly a dozen bins and its two idle sidebands are
 *  resolved. Lines come at a fixed 10.77 per second whatever the rate
 *  (every 1115 samples at 12000 Hz), so a 325 pixel waterfall holds 30
 *  seconds and the window's per-line averaging (snap, squelch) keeps the
 *  time constants it was tuned with.
 *
 *  Output is dB relative to the median of the displayed bins, so the
 *  noise floor sits near zero whatever the AF and RF gain are and the
 *  display limits never need retuning.
 */

#ifndef PSK_SPEC_H
#define PSK_SPEC_H

#include <stdbool.h>

#define PSK_SPEC_N      2048
/* Line rate, lines per second (10.77; the per-line time constants in
 * the window are tuned for it). */
#define PSK_SPEC_LINE_RATE (11025.0f / 1024.0f)
#define PSK_SPEC_MAXBIN (PSK_SPEC_N / 2)

typedef struct {
    float  fs;
    float  df;              /* Hz per bin                           */

    int    lo_bin;          /* first displayed bin, inclusive       */
    int    hi_bin;          /* last displayed bin, inclusive        */

    int   *rev;
    float *cos_t;
    float *sin_t;
    float *win;

    float  ring[PSK_SPEC_N];
    int    pos;             /* next write position in ring          */
    int    hop;             /* samples per line, from fs            */
    int    since;           /* samples since the last line          */
    int    filled;          /* samples collected, saturates at N    */
} psk_spec_t;

/* One completed line: nbins values, lowest frequency first, the first
 * value being bin lo_bin. */
typedef void (*psk_spec_cb)(const float *db, int nbins, void *ctx);

int   psk_spec_init(psk_spec_t *s, float fs);
void  psk_spec_free(psk_spec_t *s);
void  psk_spec_reset(psk_spec_t *s);

/* Choose the displayed span. Clamped to the valid bins and to at least
 * 16 bins. Call only while no audio is being processed. */
void  psk_spec_set_range(psk_spec_t *s, float lo_hz, float hi_hz);

int   psk_spec_bins(const psk_spec_t *s);

/* Centre frequency of the first displayed bin, and the bin spacing. */
float psk_spec_bin0_hz(const psk_spec_t *s);
float psk_spec_df(const psk_spec_t *s);

/* Frequencies of the left and right edges of the displayed span, which
 * is what the waterfall's horizontal axis covers. */
float psk_spec_edge_lo_hz(const psk_spec_t *s);
float psk_spec_edge_hi_hz(const psk_spec_t *s);

/* Feed real audio at fs. The callback fires once per hop samples
 * (PSK_SPEC_LINE_RATE lines per second) once the first full window has
 * been collected. */
void  psk_spec_process(psk_spec_t *s, const float *x, unsigned int n,
                       psk_spec_cb cb, void *ctx);

/*
 * Find a PSK31 signal near hz in an averaged line.
 *
 * lin holds nbins linear power values relative to the noise floor (the
 * callback's dB converted back and averaged). The search slides a
 * 7 bin (38 Hz) window over +-search_hz, which spans both idle
 * sidebands at +-15.6 Hz, takes the position with the most energy and
 * returns the energy centroid around it - the carrier frequency, even
 * though an idle PSK31 signal has a hole exactly there.
 *
 * Returns false, leaving *out_hz alone, when the energy in the best
 * window is less than twice (3 dB) what noise alone would put there,
 * or when the best position is on the edge of the search range - the
 * energy then rises towards something outside it, usually a stronger
 * neighbour, and following it would walk the receiver off its signal.
 * Measured on synthetic signals, with lines averaged at 0.05 per line
 * as the PSK31 window does: at -12 dB in 2.5 kHz the error is 1.9 Hz
 * rms (5.4 Hz worst of 100) for an idle signal and 0.9 Hz rms for
 * random text; at -9 dB, 1.3 and 0.5 Hz. No snaps in 200 noise-only
 * trials.
 */
bool  psk_spec_snap(const float *lin, int nbins, float bin0_hz, float df,
                    float hz, float search_hz, float *out_hz);

/*
 * Signal to noise in the decoder's passband, for the squelch.
 *
 * lin: nbins linear power values of ONE line, relative to that line's
 * median (the callback's dB converted back). Returns the mean power
 * over hz +- half_hz, linear, scaled so that noise alone averages 1.0
 * (0 dB): the median of noise power sits 1.6 dB below its mean (ln 2
 * for an exponential distribution), and that is taken out here. With a
 * station present it is (S+N)/N in that bandwidth. Linear so that the
 * caller can smooth it before converting to dB.
 */
float psk_spec_window_lin(const float *lin, int nbins, float bin0_hz,
                          float df, float hz, float half_hz);

#endif /* PSK_SPEC_H */
