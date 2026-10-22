/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSPR receive / transmit worker
 *
 *  Owns the WSPR decoder thread and the realtime audio ring buffer that
 *  feeds it. Modelled on ft8/audio_worker.c: cooperative shutdown via an
 *  atomic stop flag plus a condition variable, never pthread_cancel.
 *
 *  Two things differ from the FT8 worker.
 *
 *  The slot is 120 s rather than 15 s, and a transmission starts one
 *  second into an even UTC minute rather than at the slot boundary.
 *
 *  Audio is not retained. 114 s at 12000 Hz would be 5.5 MB, which this
 *  radio cannot spare, so incoming audio is converted to 375 Hz complex
 *  baseband as it arrives and only that is kept - 334 kB. Nothing is
 *  lost: the +-187 Hz that survive decimation already cover the whole
 *  200 Hz WSPR window.
 *
 *  All callbacks run on the worker thread. Handlers that touch LVGL
 *  must scheduler_put() into the UI thread rather than poking LVGL.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One decoded WSPR spot. */
typedef struct {
    char  callsign[8];
    char  locator[8];
    int   power_dbm;
    float snr_db;        /* in the 2500 Hz reference band  */
    float freq_hz;       /* audio frequency of the signal  */
    int   errors;        /* bit mismatches, lower is better */
} wspr_spot_t;

typedef struct wspr_worker_s wspr_worker_t;

typedef struct {
    /* One decoded spot. May fire zero or more times per slot. */
    void (*on_spot)(const wspr_spot_t *spot, void *ctx);

    /* Receive window closed and decoding finished. */
    void (*on_slot_end)(void *ctx);

    /*
     * One line of spectrum for a waterfall display, ordered by
     * ascending frequency and expressed in dB above the noise floor of
     * that same line. Fires about every 0.68 s while the receive window
     * is open, which is one line per WSPR symbol period.
     *
     * Optional: leave it NULL and no spectrum is computed at all, so a
     * caller with nothing to draw on pays nothing.
     */
    void (*on_psd)(const float *mag_db, int nbins, void *ctx);

    /*
     * Fired once per loop iteration. The caller may perform a blocking
     * transmission here; audio piling up meanwhile is discarded at the
     * next slot boundary.
     *
     * tx_window is true for the brief moment when a transmission should
     * start, i.e. one second into an even minute.
     */
    void (*on_tick)(bool tx_window, float sec_into_slot, void *ctx);

    void *ctx;
} wspr_worker_cb_t;

/*
 * Create and configure. Does NOT start the thread.
 *
 * audio_sample_rate : rate of the samples passed to wspr_worker_feed();
 *                     must be WSPR_DEMOD_RATE (12000), which is what the
 *                     DSP subscription delivers - anything else fails
 * center_hz         : centre of the 200 Hz WSPR window, normally 1500
 */
wspr_worker_t *wspr_worker_create(int audio_sample_rate, double center_hz,
                                  const wspr_worker_cb_t *cb);

int  wspr_worker_start(wspr_worker_t *w);
void wspr_worker_stop(wspr_worker_t *w);
void wspr_worker_destroy(wspr_worker_t *w);

/* Push audio from the radio audio callback (any thread). */
void wspr_worker_feed(wspr_worker_t *w, unsigned int n, float *samples);

/*
 * True when the system clock has been synchronised by NTP. WSPR must
 * not transmit without it: a transmission in the wrong window is just
 * interference, and a spot with a wrong timestamp pollutes the global
 * database.
 */
bool wspr_worker_time_is_synced(void);

/* Seconds into the current 120 s slot, for a progress display. */
float wspr_worker_slot_progress(void);

/*
 * Seconds the last slot took to decode. The decoder has from the end of
 * collection (114.5 s) to the next slot (120 s); shown on the status
 * line so a slow decode is visible on the radio.
 */
float wspr_worker_last_decode_s(void);

#ifdef __cplusplus
}
#endif
