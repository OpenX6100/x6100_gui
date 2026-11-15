/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - FT8 TX player
 *
 *  Copyright (c) 2026
 */

#include "tx_worker.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include <ft8lib/constants.h>

#include "lvgl/lvgl.h"

#include "../cfg/cfg_api.h"
#include "worker.h"
#include "ft2.h"

#define SIGNAL_FREQ_HZ   FT8_TX_AUDIO_HZ
#define MAX_PWR_W        5.0f
#define GAIN_MIN_DB     (-30.0f)
#define GAIN_MAX_DB      0.0f

/* As WSJT-X: the signal starts 0.5 s into the slot. The transmission
 * used to start whenever the slot's last decode and the radio let it,
 * 0 to 1.5 s into the slot, and the far end read DT anywhere from -0.4
 * to +1.6 s. Now, keyed early, the worker waits (silent) until then;
 * keyed late, it leaves out the part of the waveform that is already
 * past, so every remaining symbol is sent where it belongs (WSJT-X's
 * modulator does the same). */
#define TX_START_SEC     0.5f

/* The level is corrected once a block, from the 31st on. The audio goes
 * out in quarter blocks (85 ms at the 6 kHz used), so a swapped message
 * (tx_worker_swap) is on the air within about 0.3 s (the player holds
 * ~0.24 s). */
#define BLOCK_SAMPLES    2048
#define PART_SAMPLES     (BLOCK_SAMPLES / 4)

#define TICK_MS          100

static audio_port_player_t *player;
static const app_ports_t *g_ports = NULL;
static uint32_t sample_rate;

/* The TX thread */
static pthread_t       tick_thread;
static bool            tick_up;
static pthread_mutex_t tick_mux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  tick_cv  = PTHREAD_COND_INITIALIZER;
static bool            tick_quit;
static tx_tick_fn_t    tick_fn;
static void           *tick_ctx;

/* The transmission going out (the TX thread only): the waveform as made,
 * the part that was skipped (a late start) and how much has gone out */
static int16_t        *tx_samples;
static uint32_t        tx_n_samples, tx_skip, tx_pos;


/* ALC-driven gain correction (legacy formula from dialog_ft8.c). */
static float get_correction(void) {
    static uint8_t msg_id = 0;
    float correction = 0.0f;
    float pwr        = 0.0f;
    float alc        = 0.0f;

    if (g_ports->telemetry->tx_info_refresh(&msg_id, &alc, &pwr, NULL)) {
        float target_pwr = LV_MIN(param_f_get(cfg.pwr()), MAX_PWR_W);
        if (alc > 0.5f) {
            correction = log10f(log10f(11.1f - alc)) * 20.0f - 0.38f;
        } else if (target_pwr - pwr > 0.5f) {
            correction = log10f(target_pwr / (pwr + 0.01f)) * 10.0f;
        }
    }
    return correction;
}

/* Seconds since the start of the current slot, by the system clock (the
 * one the slots are counted by: NTP or GPS). */
static float sec_into_slot(void) {
    struct timespec now;
    float           sec;

    clock_gettime(CLOCK_REALTIME, &now);
    ftx_slot_pos(param_i_get(cfg.ft8.protocol()), now, &sec);  /* FT2: 3.75 s slots */
    return sec;
}

/* Keyed: wait for TX_START_SEC, or return how many samples are already
 * late (skipped from the start of the waveform). */
static uint32_t align_to_slot(uint32_t n_samples) {
    float s = sec_into_slot();

    if (s < TX_START_SEC) {
        struct timespec d;
        float           wait = TX_START_SEC - s;

        d.tv_sec  = (time_t)wait;
        d.tv_nsec = (long)((wait - (float)d.tv_sec) * 1.0e9f);
        while (nanosleep(&d, &d) != 0 && errno == EINTR) {
        }
        return 0;
    }
    uint64_t late = (uint64_t)((s - TX_START_SEC) * (float)sample_rate);
    if (late > n_samples) {
        late = n_samples;
    }
    if (late > 0) {
        LV_LOG_USER("FT8/FT4 TX %.2f s late: first %u samples left out", s - TX_START_SEC, (unsigned)late);
    }
    return (uint32_t)late;
}

void tx_worker_construct(const app_ports_t *ports, uint32_t rate) {
    g_ports = ports;
    sample_rate = rate;
    player = g_ports->audio->get_player(rate, 1);
}

void tx_worker_destruct() {
    g_ports->audio->player_release(player);
}

static void correct_level(float *gain_offset) {
    *gain_offset += get_correction() * 0.4f;
    if (*gain_offset > GAIN_MAX_DB) {
        *gain_offset = GAIN_MAX_DB;
    } else if (*gain_offset < GAIN_MIN_DB) {
        *gain_offset = GAIN_MIN_DB;
    }
}

bool tx_worker_swap(const char *text) {
    int16_t *swapped   = NULL;
    uint32_t n_swapped = 0;

    if (!tx_samples) {
        return false;
    }
    if (!ftx_worker_generate_tx_samples(text, SIGNAL_FREQ_HZ, sample_rate, &swapped, &n_swapped)) {
        return false;
    }
    /* Same mode, same length; and everything gone out must be text's too */
    if (n_swapped != tx_n_samples ||
        memcmp(swapped + tx_skip, tx_samples + tx_skip, (tx_pos - tx_skip) * sizeof(int16_t)) != 0) {
        free(swapped);
        LV_LOG_USER("FT8/FT4 TX: too late to swap at %.2f s into the slot, %s goes next time", sec_into_slot(), text);
        return false;
    }
    free(tx_samples);
    tx_samples = swapped;
    LV_LOG_USER("FT8/FT4 TX swapped %.2f s into the slot: %s", sec_into_slot(), text);
    return true;
}

bool tx_worker_run(const char *tx_text, int32_t tx_freq, float base_gain_offset, tx_abort_fn_t abort_check,
                   void *abort_check_ctx) {
    if (!ftx_worker_generate_tx_samples(tx_text, SIGNAL_FREQ_HZ,
                                        sample_rate,
                                        &tx_samples, &tx_n_samples)) {
        return true; /* nothing to send; not an abort */
    }

    if (param_f_get(cfg.pwr()) > MAX_PWR_W) {
        g_ports->radio->set_pwr(MAX_PWR_W);
    }

    float gain_offset      = base_gain_offset + param_f_get(cfg.ft8.output_gain_offset());
    float play_gain_offset = g_ports->audio->set_play_vol(gain_offset + 6.0f);
    gain_offset           -= play_gain_offset;

    uint64_t radio_freq = cparam_i_get(cfg.cur.fg_freq());
    g_ports->radio->set_freq((int32_t)radio_freq + tx_freq - SIGNAL_FREQ_HZ);
    g_ports->radio->set_modem(true);
    LV_LOG_USER("FT8/FT4 TX keyed %.3f s into the slot", sec_into_slot());

    float    prev_gain_offset = gain_offset;
    size_t   counter          = 0;     /* blocks begun */
    int16_t  part_buf[PART_SAMPLES];   /* the waveform stays as made, for tx_worker_swap() */

    tx_skip = tx_pos = align_to_slot(tx_n_samples);
    uint32_t block_end = tx_pos;
    /* Cut mid-symbol: 5 ms fade-in, no click on the air */
    uint32_t ramp      = LV_MIN(sample_rate / 200, tx_n_samples - tx_skip);

    bool aborted = false;
    while (tx_pos < tx_n_samples) {
        if (tx_pos >= block_end) {
            if (counter > 30) {
                correct_level(&gain_offset);
            }
            counter++;
            block_end = tx_pos + BLOCK_SAMPLES;
        }
        if (abort_check && abort_check(abort_check_ctx)) {
            aborted = true;
            break;
        }
        uint32_t part = LV_MIN(PART_SAMPLES, LV_MIN(block_end, tx_n_samples) - tx_pos);

        memcpy(part_buf, tx_samples + tx_pos, part * sizeof(int16_t));
        for (uint32_t i = 0; tx_skip > 0 && i < part && tx_pos + i < tx_skip + ramp; i++) {
            uint32_t k = tx_pos + i - tx_skip;
            part_buf[i] = (int16_t)((int32_t)part_buf[i] * (int32_t)k / (int32_t)ramp);
        }
        if (gain_offset == prev_gain_offset) {
            if (gain_offset != 0.0f) {
                g_ports->audio->gain_db(part_buf, part, gain_offset, part_buf);
            }
        } else {
            g_ports->audio->gain_db_transition(part_buf, part, prev_gain_offset, gain_offset, part_buf);
            prev_gain_offset = gain_offset;
        }
        g_ports->audio->player_send(player, part_buf, part);
        tx_pos += part;
    }
    if (!aborted && counter > 30) {
        correct_level(&gain_offset);    /* after the last block, as before */
    }

    param_f_set(cfg.ft8.output_gain_offset(), gain_offset - base_gain_offset + play_gain_offset);
    g_ports->audio->player_wait(player);
    g_ports->radio->set_modem(false);
    g_ports->radio->set_freq((int32_t)radio_freq);
    free(tx_samples);
    tx_samples = NULL;
    g_ports->audio->set_play_vol(param_f_get(cfg.audio.play_gain_db()));

    return !aborted;
}

/* ---------- the TX thread -------------------------------------------- */

static void *tick_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&tick_mux);
    while (!tick_quit) {
        pthread_mutex_unlock(&tick_mux);

        struct timespec now;
        float           sec;
        int             proto = param_i_get(cfg.ft8.protocol());

        clock_gettime(CLOCK_REALTIME, &now);
        bool odd = ftx_slot_pos(proto, now, &sec);
        tick_fn(odd, sec, tick_ctx);

        /* Next: 100 ms on, or just after the next slot starts */
        clock_gettime(CLOCK_REALTIME, &now);
        ftx_slot_pos(proto, now, &sec);
        long wait_ms = (long)((ftx_slot_time(proto) - sec) * 1000.0f) + 1;

        if (wait_ms > TICK_MS) {
            wait_ms = TICK_MS;
        }
        now.tv_sec  += wait_ms / 1000;
        now.tv_nsec += (wait_ms % 1000) * 1000000L;
        if (now.tv_nsec >= 1000000000L) {
            now.tv_sec  += 1;
            now.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&tick_mux);
        if (!tick_quit) {
            pthread_cond_timedwait(&tick_cv, &tick_mux, &now);
        }
    }
    pthread_mutex_unlock(&tick_mux);
    return NULL;
}

void tx_worker_start(tx_tick_fn_t tick, void *ctx) {
    if (tick_up) {
        return;
    }
    tick_fn   = tick;
    tick_ctx  = ctx;
    tick_quit = false;
    if (pthread_create(&tick_thread, NULL, tick_main, NULL) != 0) {
        LV_LOG_ERROR("FT8/FT4: can't start the TX thread");
        return;
    }
    tick_up = true;
}

void tx_worker_stop(void) {
    if (!tick_up) {
        return;
    }
    pthread_mutex_lock(&tick_mux);
    tick_quit = true;
    pthread_cond_broadcast(&tick_cv);
    pthread_mutex_unlock(&tick_mux);
    pthread_join(tick_thread, NULL);
    tick_up = false;
}
