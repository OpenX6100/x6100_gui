/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSPR receive / transmit worker
 */

#include "wspr_worker.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "wspr_demod.h"
#include "wspr_fano.h"
#include "wspr_spec.h"

/* WSPR slot length and the offset at which a transmission starts. */
#define WSPR_SLOT_S      120.0f
#define WSPR_TX_START_S  1.0f

/*
 * How much of the slot to collect before decoding. A transmission runs
 * from second 1 to second 111.6; a little extra covers clock error and
 * lets the time search work at both ends.
 */
/*
 * Collection ends when the latest transmission the time search accepts
 * (start up to 2.4 s late, see WSPR_DT_EARLY_S) has ended: 1 + 2.4 +
 * 110.6 = 114.0 s, plus margin. Ending here rather than at 116 s leaves
 * the decoder 5.5 s before the next slot instead of 4, which the wider
 * search needs on this CPU.
 */
#define WSPR_COLLECT_S   114.5f

/* How early a transmission may start and still be found, in seconds. */
#ifndef WSPR_DT_EARLY_S
#define WSPR_DT_EARLY_S  1.0f
#endif

/* Baseband capacity: one whole slot, so nothing can overrun. */
#define BB_CAPACITY ((long)(WSPR_SLOT_S * WSPR_WORK_RATE))

/* Audio taken from the ring buffer in one go. */
#define AUDIO_BLOCK 512

/* Half width of the frequency search. The WSPR window is 200 Hz wide. */
#define WSPR_SEARCH_HZ 110.0

/*
 * Upper bound on decode attempts per slot. A busy band can show more,
 * but each attempt costs time and the slot has to finish before the
 * next one starts.
 */
#define WSPR_MAX_SPOTS 20

struct wspr_worker_s {
    int    audio_rate;
    double center_hz;

    wspr_worker_cb_t cb;

    pthread_t       thread;
    atomic_bool     stop_req;
    /* Set by wspr_worker_start() once pthread_create() succeeds and
     * cleared by the join, both on the caller's thread: the worker
     * may not have run yet when the window is closed again. */
    bool            started;
    pthread_mutex_t sleep_mux;
    pthread_cond_t  sleep_cv;

    /* Audio buffer, written by wspr_worker_feed, read by the worker. */
    pthread_mutex_t audio_mux;
    float          *audio_buf;
    long            audio_len;
    long            audio_cap;

    /* Worker thread only from here down. Audio arrives at
     * WSPR_DEMOD_RATE already: the DSP resamples for its subscribers. */
    wspr_bb_t       bb;
    float          *bb_re;
    float          *bb_im;
    long            bb_n;

    /* Waterfall spectrum, only built when a callback asked for one. */
    wspr_spec_t     spec;
    bool            spec_ok;

    bool            collecting;
};

/* ---------- helpers ---------------------------------------------------- */

/*
 * Seconds into the current 120 s slot. WSPR slots are aligned to even
 * UTC minutes, so this is simply the Unix time modulo 120.
 */
static float slot_seconds(struct timespec now)
{
    double sec = (double)(now.tv_sec % 120) + now.tv_nsec / 1.0e9;

    return (float)sec;
}

float wspr_worker_slot_progress(void)
{
    struct timespec now;

    clock_gettime(CLOCK_REALTIME, &now);
    return slot_seconds(now);
}

bool wspr_worker_time_is_synced(void)
{
    struct stat attr;

    /* Same marker clock.c watches for its "synchronised with NTP" note. */
    return stat("/var/run/time-sync", &attr) == 0;
}

static void msleep_interruptible(wspr_worker_t *w, long ms)
{
    struct timespec abstime;

    clock_gettime(CLOCK_REALTIME, &abstime);
    abstime.tv_sec += ms / 1000;
    abstime.tv_nsec += (ms % 1000) * 1000000L;
    if (abstime.tv_nsec >= 1000000000L) {
        abstime.tv_sec += 1;
        abstime.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&w->sleep_mux);
    if (!atomic_load(&w->stop_req))
        pthread_cond_timedwait(&w->sleep_cv, &w->sleep_mux, &abstime);
    pthread_mutex_unlock(&w->sleep_mux);
}

/* Duration of the last decode, for the status line. Written by the
 * worker thread only; a torn read of a float is harmless here. */
static volatile float last_decode_s = 0.0f;

float wspr_worker_last_decode_s(void)
{
    return last_decode_s;
}

static void drain_audio(wspr_worker_t *w)
{
    pthread_mutex_lock(&w->audio_mux);
    w->audio_len = 0;
    pthread_mutex_unlock(&w->audio_mux);
}

/* ---------- receive path ---------------------------------------------- */

/* Trampoline from the spectrum module's callback to the worker's. */
static void psd_trampoline(const float *mag_db, int nbins, void *ctx)
{
    wspr_worker_t *w = (wspr_worker_t *)ctx;

    if (w->cb.on_psd)
        w->cb.on_psd(mag_db, nbins, w->cb.ctx);
}

/*
 * Move whatever audio is waiting through the baseband converter,
 * appending to the baseband store.
 */
static void consume_audio(wspr_worker_t *w)
{
    for (;;) {
        float block[AUDIO_BLOCK];
        long got = 0;

        if (atomic_load(&w->stop_req))
            return;

        pthread_mutex_lock(&w->audio_mux);
        if (w->audio_len > 0) {
            got = w->audio_len < AUDIO_BLOCK ? w->audio_len : AUDIO_BLOCK;
            memcpy(block, w->audio_buf, (size_t)got * sizeof(float));
            memmove(w->audio_buf, w->audio_buf + got,
                    (size_t)(w->audio_len - got) * sizeof(float));
            w->audio_len -= got;
        }
        pthread_mutex_unlock(&w->audio_mux);

        if (got == 0)
            return;

        if (!w->collecting)
            continue;               /* window closed, throw the audio away */

        if (w->bb_n < BB_CAPACITY) {
            long before = w->bb_n;

            w->bb_n += wspr_bb_process(&w->bb, block, got,
                                       w->bb_re + w->bb_n,
                                       w->bb_im + w->bb_n,
                                       BB_CAPACITY - w->bb_n);

            /* The display feeds off the same baseband the decoder uses,
             * so the waterfall shows exactly what is being decoded. */
            if (w->spec_ok && w->cb.on_psd && w->bb_n > before) {
                wspr_spec_process(&w->spec, w->bb_re + before,
                                  w->bb_im + before, w->bb_n - before,
                                  psd_trampoline, w);
            }
        }
    }
}

/* Trampoline from the demodulator's callback to the worker's. */
static void spot_trampoline(const wspr_candidate_t *cand,
                            const char *callsign, const char *locator,
                            int power_dbm, int errors, void *ctx)
{
    wspr_worker_t *w = (wspr_worker_t *)ctx;
    wspr_spot_t spot;

    memset(&spot, 0, sizeof(spot));
    snprintf(spot.callsign, sizeof(spot.callsign), "%s", callsign);
    snprintf(spot.locator, sizeof(spot.locator), "%s", locator);
    spot.power_dbm = power_dbm;
    spot.snr_db = cand->snr_db;
    spot.freq_hz = (float)cand->freq_hz;
    spot.errors = errors;

    if (w->cb.on_spot)
        w->cb.on_spot(&spot, w->cb.ctx);
}

static void decode_slot(wspr_worker_t *w)
{
    long skip;

    if (w->bb_n < (long)WSPR_SYMBOLS * WSPR_WORK_SYMBOL)
        return;                     /* not enough audio, nothing to do */

    /*
     * A WSPR transmission starts one second into the slot, and the
     * search spans five symbols (3.4 s) from wherever it is pointed.
     * Starting it WSPR_DT_EARLY_S early covers a clock that is up to a
     * second fast and more than two seconds slow.
     */
    skip = (long)((WSPR_TX_START_S - WSPR_DT_EARLY_S) * WSPR_WORK_RATE);
    if (skip < 0)
        skip = 0;
    if (w->bb_n - skip < (long)WSPR_SYMBOLS * WSPR_WORK_SYMBOL)
        return;

    /*
     * Decode every signal in the window, not just the strongest. The
     * 200 Hz WSPR band normally carries a dozen stations at once, so
     * returning only the best candidate throws most of the band away.
     */
    wspr_demod_decode_all(w->bb_re + skip, w->bb_im + skip, w->bb_n - skip,
                          w->center_hz, WSPR_SEARCH_HZ, WSPR_MAX_SPOTS,
                          spot_trampoline, w);
}

static void restart_collection(wspr_worker_t *w)
{
    w->bb_n = 0;
    wspr_bb_init(&w->bb, w->center_hz);
    if (w->spec_ok)
        wspr_spec_reset(&w->spec);
    drain_audio(w);
    w->collecting = true;
}

/* ---------- thread body ------------------------------------------------ */

static void *worker_main(void *arg)
{
    wspr_worker_t *w = (wspr_worker_t *)arg;
    float prev_sec = -1.0f;
    bool tx_fired = false;

    restart_collection(w);

    while (!atomic_load(&w->stop_req)) {
        struct timespec now;
        float sec;
        bool new_slot;
        bool tx_window = false;

        clock_gettime(CLOCK_REALTIME, &now);
        sec = slot_seconds(now);

        /* The slot counter wrapping is the slot boundary. */
        new_slot = (prev_sec >= 0.0f && sec < prev_sec);
        prev_sec = sec;

        consume_audio(w);

        if (w->collecting && sec >= WSPR_COLLECT_S) {
            /* Window closed: decode what we have, then idle until the
             * next slot starts. */
            w->collecting = false;
            {
                struct timespec t0, t1;

                clock_gettime(CLOCK_MONOTONIC, &t0);
                decode_slot(w);
                clock_gettime(CLOCK_MONOTONIC, &t1);
                last_decode_s = (float)(t1.tv_sec - t0.tv_sec) +
                                (float)(t1.tv_nsec - t0.tv_nsec) / 1e9f;
            }
            if (w->cb.on_slot_end)
                w->cb.on_slot_end(w->cb.ctx);
        }

        if (new_slot) {
            restart_collection(w);
            tx_fired = false;
        }

        /*
         * Transmissions begin one second into an even minute. The flag
         * is raised once per slot and only within a short window, so a
         * late tick cannot start a transmission halfway through.
         */
        if (!tx_fired && sec >= WSPR_TX_START_S && sec < WSPR_TX_START_S + 1.0f) {
            tx_window = true;
            tx_fired = true;
        }

        if (w->cb.on_tick)
            w->cb.on_tick(tx_window, sec, w->cb.ctx);

        if (!atomic_load(&w->stop_req))
            msleep_interruptible(w, 100);
    }

    return NULL;
}

/* ---------- public API -------------------------------------------------- */

wspr_worker_t *wspr_worker_create(int audio_sample_rate, double center_hz,
                                  const wspr_worker_cb_t *cb)
{
    wspr_worker_t *w;

    /*
     * The decoder works at WSPR_DEMOD_RATE. The DSP hands each
     * subscriber audio at the rate it asks for, so anything else here is
     * a caller error, not something to convert.
     */
    if (audio_sample_rate != WSPR_DEMOD_RATE || center_hz <= 0.0)
        return NULL;

    w = (wspr_worker_t *)calloc(1, sizeof(*w));
    if (!w)
        return NULL;

    w->audio_rate = audio_sample_rate;
    w->center_hz = center_hz;
    if (cb)
        w->cb = *cb;

    pthread_mutex_init(&w->audio_mux, NULL);
    pthread_mutex_init(&w->sleep_mux, NULL);
    pthread_cond_init(&w->sleep_cv, NULL);

    /* Three seconds of audio slack, as the FT8 worker uses. */
    w->audio_cap = (long)audio_sample_rate * 3;
    w->audio_buf = (float *)malloc(sizeof(float) * w->audio_cap);

    w->bb_re = (float *)malloc(sizeof(float) * BB_CAPACITY);
    w->bb_im = (float *)malloc(sizeof(float) * BB_CAPACITY);

    if (!w->audio_buf || !w->bb_re || !w->bb_im) {
        wspr_worker_destroy(w);
        return NULL;
    }

    wspr_bb_init(&w->bb, center_hz);

    /* Only pay for the spectrum if somebody is going to draw it. */
    if (w->cb.on_psd && wspr_spec_init(&w->spec) == 0)
        w->spec_ok = true;

    atomic_store(&w->stop_req, false);

    return w;
}

int wspr_worker_start(wspr_worker_t *w)
{
    if (!w)
        return -1;
    if (w->started)
        return 0;

    atomic_store(&w->stop_req, false);
    if (pthread_create(&w->thread, NULL, worker_main, w) != 0)
        return -1;
    w->started = true;

    return 0;
}

void wspr_worker_stop(wspr_worker_t *w)
{
    if (!w)
        return;

    atomic_store(&w->stop_req, true);
    pthread_mutex_lock(&w->sleep_mux);
    pthread_cond_broadcast(&w->sleep_cv);
    pthread_mutex_unlock(&w->sleep_mux);

    if (w->started) {
        pthread_join(w->thread, NULL);
        w->started = false;
    }
}

void wspr_worker_destroy(wspr_worker_t *w)
{
    if (!w)
        return;

    wspr_worker_stop(w);

    if (w->spec_ok) {
        wspr_spec_free(&w->spec);
        w->spec_ok = false;
    }

    free(w->audio_buf);
    free(w->bb_re);
    free(w->bb_im);

    pthread_mutex_destroy(&w->audio_mux);
    pthread_mutex_destroy(&w->sleep_mux);
    pthread_cond_destroy(&w->sleep_cv);

    free(w);
}

void wspr_worker_feed(wspr_worker_t *w, unsigned int n, float *samples)
{
    if (!w || !samples || n == 0)
        return;

    pthread_mutex_lock(&w->audio_mux);
    if (w->audio_buf) {
        long room = w->audio_cap - w->audio_len;
        long take = (long)n < room ? (long)n : room;

        if (take > 0) {
            memcpy(w->audio_buf + w->audio_len, samples,
                   (size_t)take * sizeof(float));
            w->audio_len += take;
        }
        /* Overflow simply drops samples; that only happens if the
         * worker thread is starved, and losing audio is better than
         * blocking the DSP callback. */
    }
    pthread_mutex_unlock(&w->audio_mux);
}
