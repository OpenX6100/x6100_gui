/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JTTY transmitter
 */

#include "jtty_tx.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "tx_info.h"

#include "jtty/jtty_mod.h"
#include "jtty/jtty_pack.h"

#define TX_RATE         JTTY_RATE
#define CHUNK           512                 /* 43 ms */
#define AMPLITUDE       30000.0f
#define MAX_PWR_W       JTTY_TX_MAX_PWR_W
#define GAIN_MIN_DB     (-30.0f)
#define GAIN_MAX_DB     0.0f
#define QUEUE_MAX       8
#define LEAD_IN_MS      250                 /* keyed, silent: as WSJT-X's Tx delay */

typedef struct {
    uint32_t       id;
    float          freq;
    int            frames;
    jtty_payload_t payload[JTTY_MAX_FRAMES];
    char           text[JTTY_MSG_MAX + 1];
} job_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t  cv;
    pthread_t       thread;
    bool            started;
    atomic_bool     quit;
    atomic_bool     abort_req;
    atomic_bool     busy;

    job_t           queue[QUEUE_MAX];
    int             nqueue;
    uint32_t        next_id;
    uint32_t        current;            /* id being sent, 0 none */

    jtty_tx_cb_t    cb;
    void           *ctx;
} S = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};

/* ALC correction, the formula of the other digital modes. */
static float get_correction(float target_pwr)
{
    static uint8_t msg_id = 0;
    float          correction = 0.0f;
    float          pwr = 0.0f;
    float          alc = 0.0f;

    if (tx_info_refresh(&msg_id, &alc, &pwr, NULL)) {
        if (alc > 0.5f) {
            correction = log10f(log10f(11.1f - fminf(alc, 10.0f))) * 20.0f - 0.38f;
        } else if (target_pwr - pwr > 0.5f) {
            correction = log10f(target_pwr / (pwr + 0.01f)) * 10.0f;
        }
    }
    return correction;
}

static void report(const job_t *j, int frame, bool done, bool stopped)
{
    jtty_tx_status_t st;

    if (S.cb == NULL) {
        return;
    }
    memset(&st, 0, sizeof(st));
    st.id = j->id;
    st.freq = j->freq;
    st.frame = frame;
    st.frames = j->frames;
    st.done = done;
    st.stopped = stopped;
    snprintf(st.text, sizeof(st.text), "%s", j->text);
    S.cb(&st, S.ctx);
}

/* Takes the next job off the queue, false if none. */
static bool next_job(job_t *j)
{
    bool have;

    pthread_mutex_lock(&S.lock);
    have = S.nqueue > 0;
    if (have) {
        *j = S.queue[0];
        memmove(S.queue, S.queue + 1, sizeof(job_t) * (size_t)(S.nqueue - 1));
        S.nqueue--;
        S.current = j->id;
    }
    pthread_mutex_unlock(&S.lock);
    return have;
}

/*
 * Sends the jobs in the queue back to back, keyed throughout. Returns
 * when the queue is empty or on abort.
 */
static void transmit(audio_player_t *player)
{
    float target_pwr = fminf(param_f_get(cfg.pwr()), MAX_PWR_W);
    float saved_pwr = param_f_get(cfg.pwr());
    float base_gain_offset;
    float gain_offset;
    float play_gain_offset;
    float prev_gain_offset;
    float stored;
    int   counter = 0;
    job_t j;
    bool  go;

    /* ESC may have emptied the queue since the worker woke up: then the
     * transmitter is not keyed at all (no 250 ms of keyed silence, no
     * relay click). */
    pthread_mutex_lock(&S.lock);
    go = S.nqueue > 0 && !atomic_load(&S.abort_req);
    pthread_mutex_unlock(&S.lock);
    if (!go) {
        return;
    }

    radio_set_pwr(target_pwr);
    if (x6100_control_get_base_ver().rev >= 3) {
        base_gain_offset = -9.4f;          /* true power control */
    } else {
        base_gain_offset = -16.4f + log10f(target_pwr) * 10.0f;
    }
    /* start from the drive level FT8 learned */
    stored = param_f_get(cfg.ft8.output_gain_offset());
    if (!isfinite(stored)) {
        stored = 0.0f;
    }
    gain_offset = base_gain_offset + stored;
    play_gain_offset = audio_set_play_vol(gain_offset + 6.0f);
    gain_offset -= play_gain_offset;
    prev_gain_offset = gain_offset;

    radio_set_modem(true);

    /* Silence first: the transmitter switches over and the sound server
     * fills its buffer before the first sync symbol, which would
     * otherwise be clipped or broken up. */
    {
        int16_t silence[CHUNK];

        memset(silence, 0, sizeof(silence));
        for (int left = TX_RATE * LEAD_IN_MS / 1000; left > 0 && !atomic_load(&S.abort_req);
             left -= CHUNK) {
            audio_player_send(player, silence, (size_t)(left < CHUNK ? left : CHUNK));
        }
    }

    while (!atomic_load(&S.abort_req) && next_job(&j)) {
        uint8_t *tones = malloc((size_t)j.frames * JTTY_NSYM);
        int      nsym = j.frames * JTTY_NSYM;
        float   *wave;
        int      n;
        int      frame = -1;
        int16_t  chunk[CHUNK];
        bool     stopped = false;

        wave = malloc(sizeof(float) * (size_t)jtty_mod_length(nsym));
        if (tones == NULL || wave == NULL) {
            free(tones);
            free(wave);
            report(&j, 0, true, true);
            continue;
        }
        for (int f = 0; f < j.frames; f++) {
            jtty_frame_tones(j.payload[f], tones + f * JTTY_NSYM);
        }
        n = jtty_mod_wave(tones, nsym, j.freq, TX_RATE, wave);
        free(tones);

        for (int pos = 0; pos < n; pos += CHUNK) {
            int part = (n - pos < CHUNK) ? n - pos : CHUNK;
            int now = (pos - JTTY_MOD_RAMP) / JTTY_FRAME_SAMPLES;

            if (atomic_load(&S.abort_req)) {
                stopped = true;
                break;
            }
            if (now != frame && now >= 0 && now < j.frames) {
                frame = now;
                report(&j, frame, false, false);
            }
            for (int i = 0; i < part; i++) {
                chunk[i] = (int16_t)lrintf(wave[pos + i] * AMPLITUDE);
            }
            if (counter > 30) {
                gain_offset += get_correction(target_pwr) * 0.4f;
                gain_offset = fminf(fmaxf(gain_offset, GAIN_MIN_DB), GAIN_MAX_DB);
            }
            if (gain_offset == prev_gain_offset) {
                if (gain_offset != 0.0f) {
                    audio_gain_db(chunk, (size_t)part, gain_offset, chunk);
                }
            } else {
                audio_gain_db_transition(chunk, (size_t)part, prev_gain_offset, gain_offset, chunk);
                prev_gain_offset = gain_offset;
            }
            audio_player_send(player, chunk, (size_t)part);
            counter++;
        }
        free(wave);
        report(&j, stopped ? (frame > 0 ? frame : 0) : j.frames - 1, true, stopped);
    }
    pthread_mutex_lock(&S.lock);
    S.current = 0;
    pthread_mutex_unlock(&S.lock);

    audio_player_wait(player);
    radio_set_modem(false);
    /* FT8's learned level is read, never written: what JTTY found is not
     * FT8's to keep (a shared transmit level is being discussed, #303). */
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    radio_set_pwr(saved_pwr);
}

static void *worker(void *arg)
{
    audio_player_t *player = audio_get_player(TX_RATE, 1);

    (void)arg;
    while (!atomic_load(&S.quit)) {
        pthread_mutex_lock(&S.lock);
        while (!atomic_load(&S.quit) && S.nqueue == 0) {
            pthread_cond_wait(&S.cv, &S.lock);
        }
        /* cleared under the lock, while there is still something to send:
         * an ESC after this point stays set and is seen by transmit() */
        atomic_store(&S.abort_req, false);
        atomic_store(&S.busy, true);
        pthread_mutex_unlock(&S.lock);
        if (atomic_load(&S.quit)) {
            atomic_store(&S.busy, false);
            break;
        }
        if (player == NULL) {
            player = audio_get_player(TX_RATE, 1);  /* try again: the sound server may be back */
        }
        if (player != NULL) {
            transmit(player);
        } else {
            /* Nothing can be sent: drop the queue (the window marks the
             * messages "not sent"), or this loop would spin with work
             * that never leaves the queue. */
            pthread_mutex_lock(&S.lock);
            S.nqueue = 0;
            pthread_mutex_unlock(&S.lock);
        }
        atomic_store(&S.busy, false);
        if (S.cb != NULL) {
            jtty_tx_status_t st;

            memset(&st, 0, sizeof(st));
            st.idle = true;
            S.cb(&st, S.ctx);
        }
    }
    if (player != NULL) {
        audio_player_release(player);
    }
    return NULL;
}

bool jtty_tx_start(jtty_tx_cb_t cb, void *ctx)
{
    if (S.started) {
        return true;
    }
    S.cb = cb;
    S.ctx = ctx;
    S.nqueue = 0;
    atomic_store(&S.quit, false);
    atomic_store(&S.abort_req, false);
    atomic_store(&S.busy, false);
    if (pthread_create(&S.thread, NULL, worker, NULL) != 0) {
        return false;
    }
    S.started = true;
    return true;
}

void jtty_tx_stop(void)
{
    if (!S.started) {
        return;
    }
    pthread_mutex_lock(&S.lock);
    S.nqueue = 0;
    atomic_store(&S.abort_req, true);
    atomic_store(&S.quit, true);
    pthread_cond_signal(&S.cv);
    pthread_mutex_unlock(&S.lock);
    pthread_join(S.thread, NULL);
    S.started = false;
    S.cb = NULL;
}

static uint32_t enqueue(job_t *j)
{
    uint32_t id = 0;

    pthread_mutex_lock(&S.lock);
    if (S.started && S.nqueue < QUEUE_MAX) {
        j->id = id = ++S.next_id;
        S.queue[S.nqueue++] = *j;
        pthread_cond_signal(&S.cv);
    }
    pthread_mutex_unlock(&S.lock);
    return id;
}

uint32_t jtty_tx_queue(const char *text, float freq, char *norm, int norm_size)
{
    job_t j;

    memset(&j, 0, sizeof(j));
    j.freq = freq;
    j.frames = jtty_pack_text(text, j.payload, j.text, sizeof(j.text));
    if (norm != NULL && norm_size > 0) {
        snprintf(norm, (size_t)norm_size, "%s", j.text);
    }
    if (j.frames == 0) {
        return 0;
    }
    return enqueue(&j);
}

uint32_t jtty_tx_queue_frames(const jtty_payload_t *payload, int frames, const char *text,
                              float freq)
{
    job_t j;

    if (frames < 1 || frames > JTTY_MAX_FRAMES) {
        return 0;
    }
    memset(&j, 0, sizeof(j));
    j.freq = freq;
    j.frames = frames;
    memcpy(j.payload, payload, sizeof(jtty_payload_t) * (size_t)frames);
    snprintf(j.text, sizeof(j.text), "%s", text);
    return enqueue(&j);
}

void jtty_tx_abort(void)
{
    pthread_mutex_lock(&S.lock);
    S.nqueue = 0;
    atomic_store(&S.abort_req, true);
    pthread_mutex_unlock(&S.lock);
}

bool jtty_tx_pending(uint32_t id)
{
    bool p = false;

    pthread_mutex_lock(&S.lock);
    p = S.current == id;
    for (int i = 0; i < S.nqueue && !p; i++) {
        p = S.queue[i].id == id;
    }
    pthread_mutex_unlock(&S.lock);
    return p;
}

bool jtty_tx_busy(void)
{
    bool b;

    pthread_mutex_lock(&S.lock);
    b = atomic_load(&S.busy) || S.nqueue > 0;
    pthread_mutex_unlock(&S.lock);
    return b;
}
