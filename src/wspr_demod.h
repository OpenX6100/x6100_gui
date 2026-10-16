/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  WSPR demodulator and synchroniser
 *
 *  Turns audio into 162 soft values ready for the Fano decoder.
 *
 *  Why decimate to 375 Hz:
 *  a WSPR symbol lasts 8192 samples at 12000 Hz, so a direct FFT would
 *  need 8192 points. Decimating by 32 gives exactly 256 samples per
 *  symbol, and a 256-point FFT at 375 Hz has 1.4648 Hz bins - precisely
 *  the WSPR tone spacing. The four tones therefore land on four
 *  ADJACENT bins with no smearing. Same resolution, 32x less work.
 *
 *  Synchronisation: WSPR has no preamble, but every symbol carries one
 *  bit of a known sync vector (the symbol's low bit). We search for the
 *  time and frequency alignment where energy concentrates in the tones
 *  consistent with that vector.
 */

#ifndef WSPR_DEMOD_H
#define WSPR_DEMOD_H

#include <stdint.h>
#include <stddef.h>

#include "wspr_encode.h"

/* Sample rate the protocol is defined at. */
#define WSPR_DEMOD_RATE 12000

/* Samples per symbol at WSPR_DEMOD_RATE. */
#define WSPR_SYMBOL_SAMPLES 8192

/* Decimation factor and the resulting working rate. */
#define WSPR_DECIM 32
#define WSPR_WORK_RATE (WSPR_DEMOD_RATE / WSPR_DECIM)       /* 375 Hz */
#define WSPR_WORK_SYMBOL (WSPR_SYMBOL_SAMPLES / WSPR_DECIM) /* 256    */

/* Samples spanned by a full 162 symbol transmission at input rate. */
#define WSPR_TX_SAMPLES ((long)WSPR_SYMBOLS * WSPR_SYMBOL_SAMPLES)

/*
 * Streaming mixer and decimator.
 *
 * The worker cannot keep 114 s of audio: at 12000 Hz that is 5.5 MB.
 * Converting to baseband as the audio arrives costs 342 kB instead, and
 * loses nothing, because the +-187 Hz that survive decimation already
 * cover the whole 200 Hz WSPR window.
 */
typedef struct {
    double center_hz;
    float taps[160];
    float hist_re[160];
    float hist_im[160];
    int hist_pos;      /* next write position in the circular history */
    int filled;        /* how many samples of history are valid       */
    int decim_count;   /* inputs since the last output                */
    double phase;      /* mixing phase, radians                       */
    double phase_step;
} wspr_bb_t;

/* Initialise the streaming converter for a given window centre. */
int wspr_bb_init(wspr_bb_t *bb, double center_hz);

/*
 * Feed input samples at WSPR_DEMOD_RATE, receive complex baseband at
 * WSPR_WORK_RATE. Returns the number of complex samples written, never
 * more than cap.
 */
long wspr_bb_process(wspr_bb_t *bb, const float *in, long n,
                     float *out_re, float *out_im, long cap);

typedef struct {
    double freq_hz;       /* detected frequency of the lowest tone       */
    double time_offset_s; /* detected start offset within the buffer     */
    float sync_quality;   /* sync vector match quality, higher is better */
    float snr_db;         /* estimated SNR in a 2500 Hz reference band,
                             the figure WSPR reports and uploads         */
} wspr_candidate_t;

/*
 * Search a buffer of audio for a WSPR signal and produce soft values.
 *
 * audio     : samples at WSPR_DEMOD_RATE, mono
 * n_samples : buffer length; must hold a full transmission plus slack
 *             for the time search
 * center_hz : centre of the search window (typically 1500 Hz)
 * search_hz : half width of the search window in Hz (typically 100)
 * soft162   : output, soft values in CODE order (already deinterleaved),
 *             ready for wspr_decode_message()
 * cand      : optional (may be NULL), details of what was found
 *
 * Returns 0 on success, -1 if no candidate was found or the parameters
 * are invalid.
 */
int wspr_demod_search(const float *audio, long n_samples,
                      double center_hz, double search_hz,
                      float soft162[WSPR_SYMBOLS],
                      wspr_candidate_t *cand);

/*
 * Callback for one decoded signal.
 *
 * The WSPR window is 200 Hz wide and normally carries a dozen or more
 * stations at once, so a search that returns only the strongest one
 * throws most of the band away. This is the interface the radio uses.
 */
typedef void (*wspr_decode_cb)(const wspr_candidate_t *cand,
                               const char *callsign, const char *locator,
                               int power_dbm, int errors, void *ctx);

/*
 * Find and decode every WSPR signal in the window.
 *
 * Works in two stages: first score every frequency bin over all time and
 * fine frequency offsets, keeping the best combination per bin; then
 * decode the strongest candidates in turn, skipping bins too close to
 * one already decoded.
 *
 * max_spots : upper bound on decode attempts, which bounds the run time
 *
 * Returns the number of signals successfully decoded.
 */
int wspr_demod_decode_all(const float *bb_re, const float *bb_im, long bb_n,
                          double center_hz, double search_hz,
                          int max_spots, wspr_decode_cb cb, void *ctx);

/*
 * Same search, but on baseband already produced by wspr_bb_process().
 * This is what the radio uses: the worker converts as audio arrives and
 * only keeps the decimated result.
 *
 * bb_re, bb_im : complex baseband at WSPR_WORK_RATE
 * bb_n         : number of complex samples, must cover a whole
 *                transmission plus two symbols of slack
 *
 * The start time search spans two symbol periods from the beginning of
 * the buffer, so the caller should pass a pointer placed about half a
 * symbol BEFORE the expected start of the transmission. For the radio
 * that means offsetting by (1.0 s - half a symbol) from the slot
 * boundary, since WSPR starts one second in.
 * center_hz    : the centre that was passed to wspr_bb_init(), needed
 *                only to report an absolute frequency
 */
int wspr_demod_search_bb(const float *bb_re, const float *bb_im, long bb_n,
                         double center_hz, double search_hz,
                         float soft162[WSPR_SYMBOLS],
                         wspr_candidate_t *cand);

#endif /* WSPR_DEMOD_H */
