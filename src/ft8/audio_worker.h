/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 audio / decode worker
 *
 *  Copyright (c) 2026
 */

/*
 *  Owns the FT8 decoder thread and the realtime audio ring buffer that
 *  feeds it. Uses cooperative shutdown via an atomic stop flag plus a
 *  condition variable - never pthread_cancel. When audio_worker_stop()
 *  returns, all DSP state (ftx_worker, firdecim, spgramcf) is in a
 *  consistent state, so ftx_worker_free() is safe.
 *
 *  Business logic hooks are pure callbacks with value-semantic payloads:
 *
 *      on_message   - one FT8/FT4 decode (may fire 0..N times per iteration)
 *      on_psd       - one waterfall PSD frame
 *      on_slot_end  - slot boundary crossed, RX window closed, decoder reset
 *      on_tick      - end-of-iteration hook, caller may perform blocking TX
 *
 *  All callbacks run on the worker thread. Handlers that need to touch LVGL
 *  must scheduler_put() into the UI thread instead of poking LVGL directly.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <complex.h>

#include <ft8lib/constants.h>

#define FTX_CAPTURE_RATE 12000 // (48000 / 3)

#ifdef __cplusplus
extern "C" {
#endif

/* Per-RX-slot metadata exposed to callbacks. Kept minimal on purpose. */
typedef struct {
    bool    odd;
    bool    answer_generated;
    int64_t start_ms;   /* the slot's start, UTC ms since the epoch */
} slot_info_t;

typedef struct audio_worker_s audio_worker_t;

typedef struct {
    void (*on_message)(const char *text, int snr, float freq_hz,
                       float time_sec,
                       const slot_info_t *info, void *ctx);
    void (*on_psd)(const float *psd, uint16_t nfft,
                   float sec_since_slot_start,
                   const slot_info_t *info, void *ctx);
    void (*on_slot_end)(const slot_info_t *info, void *ctx);
    /* End-of-iteration hook. Fires after rx processing and slot-end handling.
     * The caller may perform a blocking TX here; on return, the worker loops
     * back around and the stale audio accumulated during TX will be drained
     * at the next slot boundary. */
    void (*on_tick)(const slot_info_t *info,
                    bool new_slot,
                    float sec_since_slot_start,
                    void *ctx);
    void *ctx;
} audio_worker_cb_t;

/* Create + configure the worker. Does NOT start the thread. */
audio_worker_t *audio_worker_create(int audio_sample_rate,
                                    ftx_protocol_t proto,
                                    int32_t filter_low_hz,
                                    int32_t filter_high_hz,
                                    const audio_worker_cb_t *cb);

/* Start the decode thread. */
int audio_worker_start(audio_worker_t *w);

/* Cooperative shutdown: set stop flag, signal cv, join. No pthread_cancel. */
void audio_worker_stop(audio_worker_t *w);

/* Release all resources. Calls audio_worker_stop() first if still running. */
void audio_worker_destroy(audio_worker_t *w);

/* Push audio samples from the radio audio callback (any thread). */
void audio_worker_feed(audio_worker_t *w, unsigned int n, float *samples);

/* Waterfall FFT size chosen from filter bandwidth at create time. */
uint16_t audio_worker_nfft(const audio_worker_t *w);

#ifdef __cplusplus
}
#endif
