/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - Olivia MFSK transmitter
 */

#include "olivia_tx.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "digi_txq.h"
#include "radio.h"
#include "tx_info.h"

#include "olivia.h"
#include "olivia_mod.h"
#include "olivia_resamp.h"

/* Olivia has a constant envelope per tone and runs for long stretches:
 * keep the same thermal cap as the FT8 worker. */
#define MAX_PWR_W        5.0f
#define GAIN_MIN_DB     (-30.0f)
#define GAIN_MAX_DB      0.0f

#define AMPLITUDE        30000.0f
#define CHUNK_SAMPLES    2048       /* 43 ms at 48 kHz */

/* The rate the modulation is defined at. */
#define OLIVIA_NOMINAL_RATE  8000

/* Blocks of zeros before the data. The receiver needs a few blocks to
 * find the synchronisation, so a run-in padded with zeros goes first -
 * fldigi does the same. */
#define LEAD_BLOCKS      4
#define TAIL_BLOCKS      1

#define CENTER_MIN       500
#define CENTER_MAX       2500
#define CENTER_DEFAULT   1500

static const struct {
    int tones;
    int bandwidth;
    const char *name;
} mode_table[OLIVIA_MODE_COUNT] = {
    {  8,  250, "8/250"   },
    {  8,  500, "8/500"   },    /* 62.5 baud: the faster one for good paths */
    { 16,  500, "16/500"  },
    { 32, 1000, "32/1000" },
};

static pthread_t     tx_thread;
static digi_txq_t    txq = DIGI_TXQ_INITIALIZER;
static volatile bool tx_quick_stop = false;   /* no closing blocks */
static uint16_t      center_hz = CENTER_DEFAULT;
static olivia_mode_t cur_mode = OLIVIA_MODE_32_1000;
static olivia_tx_echo_cb_t echo_cb = NULL;

/* --- Mode parameters -------------------------------------------------- */

olivia_mode_t olivia_tx_get_mode(void) {
    return cur_mode;
}

void olivia_tx_set_mode(olivia_mode_t mode) {
    if (mode < 0 || mode >= OLIVIA_MODE_COUNT) return;
    if (digi_txq_running(&txq)) return; /* not while transmitting */
    cur_mode = mode;
}

const char *olivia_tx_mode_name(olivia_mode_t mode) {
    if (mode < 0 || mode >= OLIVIA_MODE_COUNT) return "?";
    return mode_table[mode].name;
}

int olivia_tx_mode_tones(olivia_mode_t mode) {
    if (mode < 0 || mode >= OLIVIA_MODE_COUNT) return 32;
    return mode_table[mode].tones;
}

int olivia_tx_mode_bandwidth(olivia_mode_t mode) {
    if (mode < 0 || mode >= OLIVIA_MODE_COUNT) return 1000;
    return mode_table[mode].bandwidth;
}

uint16_t olivia_tx_get_center(void) {
    return center_hz;
}

void olivia_tx_set_center(uint16_t hz) {
    if (hz < CENTER_MIN) hz = CENTER_MIN;
    if (hz > CENTER_MAX) hz = CENTER_MAX;
    center_hz = hz;
}

void olivia_tx_set_echo_cb(olivia_tx_echo_cb_t cb) {
    echo_cb = cb;
}

/* --- ALC-driven gain correction (same scheme as the FT8 worker) ------ */

static float get_correction(void) {
    static uint8_t msg_id = 0;
    float correction = 0.0f, pwr = 0.0f, alc = 0.0f;

    if (tx_info_refresh(&msg_id, &alc, &pwr, NULL)) {
        float target_pwr = LV_MIN(param_f_get(cfg.pwr()), MAX_PWR_W);
        if (alc > 0.5f) {
            /* above 10.1 the log of a negative: a NaN gain */
            correction = log10f(log10f(11.1f - fminf(alc, 10.0f))) * 20.0f - 0.38f;
        } else if (target_pwr - pwr > 0.5f) {
            correction = log10f(target_pwr / (pwr + 0.01f)) * 10.0f;
        }
    }
    return correction;
}

/* --- Worker thread ---------------------------------------------------- */

/* Up to one block of characters from the queue, padded with zeros.
 * Returns how many were taken. */
static int take_chars(uint8_t *chars, int bits) {
    int got = 0;

    while (got < bits) {
        int c = digi_txq_get(&txq);

        if (c < 0) break;
        if (c == '\n') c = '\r';      /* Olivia uses CR */
        if (c > 127)  c = '?';          /* the alphabet is 7-bit */
        chars[got++] = (uint8_t)c;
    }
    for (int i = got; i < bits; i++) chars[i] = 0;
    return got;
}

/* One transmission, until the queue closes. False when it could not
 * start. */
static bool transmit(void) {
    olivia_mod_t    mod;
    olivia_resamp_t res;
    int tones = mode_table[cur_mode].tones;
    int bw    = mode_table[cur_mode].bandwidth;

    if (olivia_mod_init(&mod, tones, bw, OLIVIA_NOMINAL_RATE,
                        (double)center_hz - bw / 2.0) < 0) {
        return false;
    }

    if (olivia_resamp_init(&res, OLIVIA_NOMINAL_RATE, AUDIO_PLAY_RATE) < 0) {
        olivia_mod_free(&mod);
        return false;
    }

    int    bits = mod.cfg.bits;
    int    blk_samples = olivia_mod_block_samples(&mod);
    float *blk = malloc(sizeof(float) * blk_samples);
    int    res_cap = olivia_resamp_max_out(&res, blk_samples);
    float *res_buf = malloc(sizeof(float) * res_cap);

    /* The default 48 kHz mono player is shared. */
    audio_player_t *player = audio_get_player(AUDIO_PLAY_RATE, 1);

    if (!blk || !res_buf || !player) {
        free(blk);
        free(res_buf);
        olivia_resamp_free(&res);
        olivia_mod_free(&mod);
        return false;
    }

    /* A different seed for every transmission, so that a repeated macro
     * does not give an identical spectrum. */
    olivia_mod_seed(&mod, (uint32_t)lv_tick_get() | 1u);

    /* radio_set_pwr() only touches the hardware, so putting the stored
     * setting back afterwards undoes the cap. */
    const float saved_pwr  = param_f_get(cfg.pwr());
    const float target_pwr = LV_MIN(saved_pwr, MAX_PWR_W);

    if (saved_pwr > MAX_PWR_W) {
        radio_set_pwr(MAX_PWR_W);
    }

    float base_gain_offset;
    if (x6100_control_get_base_ver().rev >= 3) {
        base_gain_offset = -9.4f;
    } else {
        base_gain_offset = -16.4f + log10f(target_pwr) * 10.0f;
    }

    float gain_offset      = base_gain_offset + param_f_get(cfg.ft8.output_gain_offset());
    float play_gain_offset = audio_set_play_vol(gain_offset + 6.0f);
    gain_offset           -= play_gain_offset;

    radio_set_modem(true);

    int16_t chunk[CHUNK_SAMPLES];
    size_t  out_n = 0;
    size_t  counter = 0;
    float   prev_gain_offset = gain_offset;

    /*
     * Run-in, then the text a block at a time as it is taken from the
     * queue - text added meanwhile joins this transmission - empty
     * blocks while the operator is still typing, and the tail. Text that
     * arrives during the tail carries on; a stop skips what is left of
     * the run-in.
     */
    enum { PH_LEAD, PH_TEXT, PH_TAIL } ph = PH_LEAD;
    int  lead = LEAD_BLOCKS;
    int  tail = 0;
    bool done = false;

    while (!done) {
        uint8_t chars[OLIVIA_MAX_BITS];
        bool    flush = false;
        int     in_n;

        if (ph == PH_LEAD) {
            memset(chars, 0, sizeof(chars));
            if (--lead <= 0) ph = PH_TEXT;
            if (digi_txq_pending(&txq) == 0 && !digi_txq_held(&txq)) {
                ph = PH_TEXT;           /* stopped during the run-in */
                continue;
            }
        } else if (ph == PH_TEXT) {
            if (take_chars(chars, bits) == 0 && !digi_txq_held(&txq)) {
                ph = PH_TAIL;
                tail = TAIL_BLOCKS;
            }
        }
        if (ph == PH_TAIL) {
            if (tx_quick_stop) {
                tail = 0;               /* closing the window: no tail */
            }
            if (tail == 0) {
                if (!digi_txq_close_if_empty(&txq)) {
                    ph = PH_TEXT;       /* more came during the tail */
                    continue;
                }
                flush = true;
                done = true;
            } else {
                memset(chars, 0, sizeof(chars));
                tail--;
            }
        }

        if (flush) {
            /* The last symbols' windows still in the modulator, and the
             * resampler's filter delay (zeros push it out): played to the
             * end, the signal fades instead of being cut. */
            in_n = olivia_mod_flush(&mod, blk);
            memset(blk + in_n, 0, sizeof(float) * OLIVIA_RESAMP_TAPS);
            in_n += OLIVIA_RESAMP_TAPS;
        } else {
            olivia_mod_block(&mod, chars, blk);
            in_n = blk_samples;
        }
        int n = olivia_resamp_process(&res, blk, in_n, res_buf);

        for (int k = 0; k < n; k++) {
            float v = res_buf[k] * AMPLITUDE;

            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            chunk[out_n++] = (int16_t)v;

            if (out_n == CHUNK_SAMPLES) {
                if (counter > 30) {
                    gain_offset += get_correction() * 0.4f;
                    if (gain_offset > GAIN_MAX_DB) gain_offset = GAIN_MAX_DB;
                    if (gain_offset < GAIN_MIN_DB) gain_offset = GAIN_MIN_DB;
                }
                if (gain_offset == prev_gain_offset) {
                    if (gain_offset != 0.0f) {
                        audio_gain_db(chunk, out_n, gain_offset, chunk);
                    }
                } else {
                    audio_gain_db_transition(chunk, out_n, prev_gain_offset,
                                             gain_offset, chunk);
                    prev_gain_offset = gain_offset;
                }
                audio_player_send(player, chunk, out_n);
                out_n = 0;
                counter++;
            }
        }
    }

    if (out_n > 0) {
        if (gain_offset != 0.0f) {
            audio_gain_db(chunk, out_n, gain_offset, chunk);
        }
        audio_player_send(player, chunk, out_n);
    }

    audio_player_wait(player);
    radio_set_modem(false);
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));
    if (saved_pwr > MAX_PWR_W) {
        radio_set_pwr(saved_pwr);
    }
    audio_player_release(player);

    free(blk);
    free(res_buf);
    olivia_resamp_free(&res);
    olivia_mod_free(&mod);
    return true;
}

/* One transmission after another while text keeps coming in as one
 * ends (digi_txq_finish); text that cannot go out is dropped. */
static void *tx_thread_fn(void *arg) {
    (void)arg;

    while (transmit()) {
        if (!digi_txq_finish(&txq)) return NULL;
    }
    digi_txq_abort(&txq);
    digi_txq_finish(&txq);
    return NULL;
}

/* --- Public API ------------------------------------------------------- */

bool olivia_tx_send(const char *text) {
    if (!text || !*text) return false;

    switch (digi_txq_put(&txq, text)) {
        case DIGI_TXQ_ADDED:
            break;

        case DIGI_TXQ_STARTED:
            tx_quick_stop = false;      /* a new transmission; not in transmit(),
                                           where a stop made meanwhile would be lost */
            if (pthread_create(&tx_thread, NULL, tx_thread_fn, NULL) != 0) {
                digi_txq_abort(&txq);
                digi_txq_finish(&txq);
                return false;
            }
            pthread_detach(tx_thread);
            break;

        default:                        /* no room */
            return false;
    }
    if (echo_cb) {
        echo_cb(text);
    }
    return true;
}

void olivia_tx_stop(void) {
    digi_txq_abort(&txq);
}

void olivia_tx_stop_quick(void) {
    if (digi_txq_running(&txq)) {
        tx_quick_stop = true;
        digi_txq_abort(&txq);
    }
}

bool olivia_tx_is_on(void) {
    return digi_txq_running(&txq);
}

bool olivia_tx_hold(bool on) {
    return digi_txq_hold(&txq, on);
}
