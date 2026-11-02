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
#include <errno.h>
#include <time.h>

#include <ft8lib/constants.h>

#include "lvgl/lvgl.h"

#include "../cfg/cfg_api.h"
#include "worker.h"

/* FT8 audio tone offset inside the radio passband. */
#define SIGNAL_FREQ_HZ   1325
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

static audio_port_player_t *player;
static const app_ports_t *g_ports = NULL;
static uint32_t sample_rate;


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
    float slot = (param_i_get(cfg.ft8.protocol()) == FTX_PROTOCOL_FT4) ? FT4_SLOT_TIME : FT8_SLOT_TIME;
    float sec;

    clock_gettime(CLOCK_REALTIME, &now);
    sec = (float)(now.tv_sec % 60) + now.tv_nsec / 1.0e9f;
    return fmodf(sec, slot);
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

bool tx_worker_run(const char *tx_text, float base_gain_offset, tx_abort_fn_t abort_check,
                   void *abort_check_ctx) {
    int16_t *samples   = NULL;
    uint32_t n_samples = 0;

    if (!ftx_worker_generate_tx_samples(tx_text, SIGNAL_FREQ_HZ,
                                        sample_rate,
                                        &samples, &n_samples)) {
        return true; /* nothing to send; not an abort */
    }

    if (param_f_get(cfg.pwr()) > MAX_PWR_W) {
        g_ports->radio->set_pwr(MAX_PWR_W);
    }

    float gain_offset      = base_gain_offset + param_f_get(cfg.ft8.output_gain_offset());
    float play_gain_offset = g_ports->audio->set_play_vol(gain_offset + 6.0f);
    gain_offset           -= play_gain_offset;

    uint64_t radio_freq = cparam_i_get(cfg.cur.fg_freq());
    g_ports->radio->set_freq((int32_t)radio_freq + (int32_t)param_i_get(cfg.ft8.tx_freq()) - SIGNAL_FREQ_HZ);
    g_ports->radio->set_modem(true);

    float    prev_gain_offset = gain_offset;
    size_t   counter          = 0;
    int16_t *ptr              = samples;
    size_t   part;
    uint32_t skip             = align_to_slot(n_samples);

    ptr       += skip;
    n_samples -= skip;
    if (skip > 0) {
        /* Cut mid-symbol: 5 ms fade-in, no click on the air */
        uint32_t ramp = LV_MIN(sample_rate / 200, n_samples);

        for (uint32_t i = 0; i < ramp; i++) {
            ptr[i] = (int16_t)((int32_t)ptr[i] * (int32_t)i / (int32_t)ramp);
        }
    }

    bool aborted = false;
    while (true) {
        if (counter > 30) {
            gain_offset += get_correction() * 0.4f;
            if (gain_offset > GAIN_MAX_DB) {
                gain_offset = GAIN_MAX_DB;
            } else if (gain_offset < GAIN_MIN_DB) {
                gain_offset = GAIN_MIN_DB;
            }
        }
        if (n_samples <= 0) {
            break;
        }
        if (abort_check && abort_check(abort_check_ctx)) {
            aborted = true;
            break;
        }
        part = LV_MIN(1024 * 2, n_samples);
        if (gain_offset == prev_gain_offset) {
            if (gain_offset != 0.0f) {
                g_ports->audio->gain_db(ptr, part, gain_offset, ptr);
            }
        } else {
            g_ports->audio->gain_db_transition(ptr, part, prev_gain_offset, gain_offset, ptr);
            prev_gain_offset = gain_offset;
        }
        g_ports->audio->player_send(player, ptr, part);
        n_samples -= part;
        ptr       += part;
        counter++;
    }

    param_f_set(cfg.ft8.output_gain_offset(), gain_offset - base_gain_offset + play_gain_offset);
    g_ports->audio->player_wait(player);
    g_ports->radio->set_modem(false);
    g_ports->radio->set_freq((int32_t)radio_freq);
    free(samples);
    g_ports->audio->set_play_vol(param_f_get(cfg.audio.play_gain_db()));

    return !aborted;
}
