/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - Olivia MFSK receiver, application layer
 */

#include "olivia_app.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "dsp.h"
#include "panel.h"

#include "olivia.h"
#include "olivia_rx.h"
#include "olivia_tx.h"        /* mode variant and centre frequency */

/*
 * Tuning margin: +-4 tones, i.e. +-125 Hz at 32/1000. That is enough
 * with the band edges shown in the window. Every hypothesis is tried in
 * both tone orientations, so a wider margin would cost twice over.
 */
#define TUNE_MARGIN      4

/* Statically initialised so it is valid before olivia_app_init(). */
static pthread_mutex_t  mux = PTHREAD_MUTEX_INITIALIZER;
static bool             active = false;

/* The DSP subscription. dsp_audio_set_active() takes the lock the DSP
 * holds while it calls us, so it is never called with mux held: that
 * order could deadlock against olivia_app_put_audio_samples(). */
static uint32_t         dsp_sub = AUDIO_SUB_INVALID;

static olivia_rx_t      rx;
static bool             rx_ok  = false;

static olivia_app_text_cb_t   text_cb   = NULL;
static olivia_app_audio_tap_t audio_tap = NULL;

static uint32_t         stat_blocks = 0;
static float            stat_level = -180.0f;
static float            stat_clip = 0.0f;
static uint32_t         stat_chars = 0;

static int              cfg_tones = 32;
static int              cfg_bw = 1000;
static uint16_t         cfg_center = 1500;


/* --- Received text ---------------------------------------------------- */

/* DSP thread, mux held. */
static void on_text(void *user, const uint8_t *chars, int count) {
    char out[OLIVIA_MAX_BITS + 1];
    int  o = 0;

    (void)user;

    stat_blocks++;

    for (int i = 0; i < count && o < OLIVIA_MAX_BITS; i++) {
        unsigned char c = chars[i] & 0x7f;

        /*
         * Olivia has no idle at the modulation level - the transmitter
         * always sends whole blocks and fills the gaps with zeros
         * (checked on a fldigi recording: it pads with 0x00). Zeros are
         * skipped, so the screen does not fill with rubbish.
         */
        if (c == 0) continue;

        if (c == '\r') {
            out[o++] = '\n';
            continue;
        }
        if (c < 32 && c != '\n') continue;   /* other control characters */
        if (c == 127) continue;

        out[o++] = (char)c;
        stat_chars++;
    }

    if (o > 0) {
        out[o] = '\0';
        if (text_cb)
            text_cb(out);
        else
            panel_add_text(out);
    }
}

/* --- Building and tearing down the chain ------------------------------ */

/* mux held */
static void teardown(void) {
    if (rx_ok) {
        olivia_rx_free(&rx);
        rx_ok = false;
    }
}

/* mux held */
static bool setup(void) {
    teardown();

    cfg_tones  = olivia_tx_mode_tones(olivia_tx_get_mode());
    cfg_bw     = olivia_tx_mode_bandwidth(olivia_tx_get_mode());
    cfg_center = olivia_tx_get_center();

    if (olivia_rx_init(&rx, cfg_tones, cfg_bw, OLIVIA_RX_RATE,
                       (double)cfg_center - cfg_bw / 2.0,
                       TUNE_MARGIN) < 0) {
        return false;
    }

    olivia_rx_set_callback(&rx, on_text, NULL);

    stat_blocks = 0;
    stat_chars = 0;
    stat_level = -180.0f;
    stat_clip = 0.0f;

    rx_ok = true;
    return true;
}

/* --- Public API ------------------------------------------------------- */

void olivia_app_init(void) {
    if (dsp_sub == AUDIO_SUB_INVALID) {
        dsp_sub = dsp_audio_subscribe_float(olivia_app_put_audio_samples,
                                            OLIVIA_RX_RATE);
        dsp_audio_set_active(dsp_sub, false);
    }
}

void olivia_app_set_active(bool on) {
    /* Off: stop the feed first; once this returns no audio block is
     * inside the receiver, which is what lets the caller drop its hooks. */
    if (!on && dsp_sub != AUDIO_SUB_INVALID)
        dsp_audio_set_active(dsp_sub, false);

    pthread_mutex_lock(&mux);
    if (on && !active) {
        active = setup();
    } else if (!on && active) {
        teardown();
        active = false;
    }
    pthread_mutex_unlock(&mux);

    if (on && active && dsp_sub != AUDIO_SUB_INVALID)
        dsp_audio_set_active(dsp_sub, true);
}

bool olivia_app_is_active(void) {
    return active;
}

/*
 * The getters below run on the UI thread while olivia_app_update_settings()
 * may be rebuilding the receiver, so the ones that look inside it take
 * the lock. They never take the DSP's lock, so the order DSP lock -> mux
 * used by the audio thread cannot be inverted.
 */

bool olivia_app_reversed(void) {
    bool r;

    pthread_mutex_lock(&mux);
    r = rx_ok && olivia_rx_reversed(&rx);
    pthread_mutex_unlock(&mux);
    return r;
}

void olivia_app_update_settings(void) {
    pthread_mutex_lock(&mux);
    if (active) {
        /*
         * A new frequency or variant invalidates the synchronisation -
         * building the chain again is cheap and surer than trying to
         * recompute the state.
         */
        if (!setup())
            active = false;
    }
    pthread_mutex_unlock(&mux);
}

void olivia_app_set_text_cb(olivia_app_text_cb_t cb) {
    pthread_mutex_lock(&mux);
    text_cb = cb;
    pthread_mutex_unlock(&mux);
}

void olivia_app_set_audio_tap(olivia_app_audio_tap_t tap) {
    pthread_mutex_lock(&mux);
    audio_tap = tap;
    pthread_mutex_unlock(&mux);
}

/* DSP thread. */
void olivia_app_put_audio_samples(size_t n, float *samples) {
    pthread_mutex_lock(&mux);
    if (!active || !rx_ok || n == 0) {
        pthread_mutex_unlock(&mux);
        return;
    }

    if (audio_tap)
        audio_tap((unsigned int)n, samples);

    /*
     * Level and clipping. Olivia sends one of many tones at a time but
     * the receive audio carries everything else in the passband too; a
     * clipped input turns into intermodulation and the tone structure
     * disappears. Shown so the operator can turn the RF/AF gain down.
     */
    {
        double sum = 0.0;
        size_t clipped = 0;

        for (size_t i = 0; i < n; i++) {
            float v = samples[i];

            sum += (double)v * v;
            if (v > 0.98f || v < -0.98f)
                clipped++;
        }

        float pct = 100.0f * (float)clipped / (float)n;
        stat_clip += (pct - stat_clip) * 0.1f;

        float rms = (float)sqrt(sum / (double)n);
        float db = (rms > 1e-9f) ? 20.0f * log10f(rms) : -180.0f;
        stat_level += (db - stat_level) * 0.1f;
    }

    /* olivia_rx_process keeps the remainder itself, so any length is
     * fine. */
    olivia_rx_process(&rx, samples, (int)n);

    pthread_mutex_unlock(&mux);
}

bool olivia_app_locked(void) {
    bool r;

    pthread_mutex_lock(&mux);
    r = rx_ok && olivia_rx_locked(&rx);
    pthread_mutex_unlock(&mux);
    return r;
}

float olivia_app_freq_error(void) {
    float r = 0.0f;

    pthread_mutex_lock(&mux);
    if (rx_ok)
        r = (float)olivia_rx_freq_error(&rx);
    pthread_mutex_unlock(&mux);
    return r;
}

uint32_t olivia_app_blocks(void) {
    return stat_blocks;
}

uint32_t olivia_app_chars(void) {
    return stat_chars;
}

float olivia_app_level_db(void) {
    return stat_level;
}

uint8_t olivia_app_clip_pct(void) {
    float v = stat_clip;

    if (v < 0.0f) v = 0.0f;
    if (v > 99.0f) v = 99.0f;
    return (uint8_t)(v + 0.5f);
}

float olivia_app_best_score(void) {
    float r = 0.0f;

    pthread_mutex_lock(&mux);
    if (rx_ok)
        r = olivia_rx_best_score(&rx);
    pthread_mutex_unlock(&mux);
    return r;
}

float olivia_app_best_offset_hz(void) {
    int   off = 0;
    float r = 0.0f;

    pthread_mutex_lock(&mux);
    if (rx_ok) {
        olivia_rx_best_hypothesis(&rx, &off);
        /* The offset is in FFT bins, a quarter of the tone spacing each. */
        r = (float)olivia_demod_offset_hz(&rx.demod, off);
    }
    pthread_mutex_unlock(&mux);
    return r;
}

float olivia_app_raw_score(void) {
    float r = 0.0f;

    pthread_mutex_lock(&mux);
    if (rx_ok)
        r = olivia_rx_score(&rx);
    pthread_mutex_unlock(&mux);
    return r;
}

uint8_t olivia_app_quality(void) {
    float s;
    bool  ok;

    pthread_mutex_lock(&mux);
    ok = rx_ok && olivia_rx_locked(&rx);
    s = ok ? olivia_rx_score(&rx) : 0.0f;
    pthread_mutex_unlock(&mux);

    if (!ok) return 0;

    /*
     * FHT figure on a 0..100 scale. Below 3 decoding is unreliable, at
     * about 10 it is very sure - hence this mapping range.
     */
    if (s <= 3.0f)  return 0;
    if (s >= 12.0f) return 100;
    return (uint8_t)((s - 3.0f) * 100.0f / 9.0f + 0.5f);
}
