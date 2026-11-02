/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - digital-mode TX player
 *
 *  Copyright (c) 2026
 *  tx_player_stream() and the learned-level floor after Giovanni Ricciardi
 *  IU1WXY's tx_player.c (IU1WXY firmware, LGPL-2.1-or-later)
 */

#include "tx_player.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "lvgl/lvgl.h"

#include "audio.h"
#include "cfg/cfg_api.h"
#include "radio.h"
#include "tx_info.h"
#include "tx_level.h"

#include <aether_radio/x6100_control/control.h>

#define GAIN_MIN_DB (-30.0f)
#define GAIN_MAX_DB 0.0f

/* The learned gain offset is kept no lower than this: an overdriven or long
 * transmission could push it to the floor, and the ramp up (8 dB a
 * transmission at most, tx_level.c) then took many transmissions to bring
 * the power back. 5 W needs about -36 dB in all (measured by IU1WXY and by
 * the Murus team on rev 3 bases, i.e. about -27 dB learned). From IU1WXY's
 * tx_player.c. */
#define LEARNED_MIN_DB (-42.0f)

static float clamp_gain(float g) {
    return g > GAIN_MAX_DB ? GAIN_MAX_DB : g < GAIN_MIN_DB ? GAIN_MIN_DB : g;
}

float tx_player_base_gain_offset(void) {
    float target_pwr = LV_MIN(param_f_get(cfg.pwr()), TX_PLAYER_MAX_PWR_W);
    if (x6100_control_get_base_ver().rev >= 3) {
        // patched firmware has a true power control
        return -9.4f;
    }
    return -16.4f + log10f(target_pwr) * 10.0f;
}

/* tx_player_play()'s samples, handed out as a stream. */
typedef struct {
    int16_t *next;
    uint32_t left;
} samples_t;

static size_t take_samples(int16_t *buf, size_t n, void *ctx) {
    samples_t *s = ctx;

    if (n > s->left) {
        n = s->left;
    }
    memcpy(buf, s->next, n * sizeof(int16_t));
    s->next += n;
    s->left -= n;
    return n;
}

bool tx_player_play(int16_t      *samples,
                    uint32_t      n_samples,
                    int32_t       tx_offset_hz,
                    float         base_gain_offset,
                    tx_abort_fn_t abort_check,
                    void         *abort_check_ctx) {
    samples_t s = {.next = samples, .left = n_samples};

    return tx_player_stream(take_samples, &s, tx_offset_hz, base_gain_offset, abort_check, abort_check_ctx);
}

bool tx_player_stream(tx_fill_fn_t  fill,
                      void         *fill_ctx,
                      int32_t       tx_offset_hz,
                      float         base_gain_offset,
                      tx_abort_fn_t abort_check,
                      void         *abort_check_ctx) {
    if (param_f_get(cfg.pwr()) > TX_PLAYER_MAX_PWR_W) {
        radio_set_pwr(TX_PLAYER_MAX_PWR_W);
    }

    float gain_offset      = base_gain_offset + LV_MAX(param_f_get(cfg.ft8.output_gain_offset()), LEARNED_MIN_DB);
    float play_gain_offset = audio_set_play_vol(gain_offset + 6.0f);
    gain_offset           -= play_gain_offset;

    uint64_t radio_freq = cparam_i_get(cfg.cur.fg_freq());
    radio_set_freq((int32_t)radio_freq + tx_offset_hz - TX_PLAYER_AUDIO_HZ);
    radio_set_modem(true);

    /* The drive from the ALC and power readback (tx_level.c): ramped up
     * while the ALC reads zero, held once it shows; kept for the next. */
    static uint8_t msg_id = 0;
    tx_level_t     level;
    tx_level_start(&level, LV_MIN(param_f_get(cfg.pwr()), TX_PLAYER_MAX_PWR_W));
    tx_info_refresh(&msg_id, NULL, NULL, NULL); /* readings from before this one don't count */

    float   prev_gain_offset = gain_offset;
    int16_t block[TX_PLAYER_BLOCK];

    bool aborted = false;
    while (true) {
        float pwr = 0.0f, alc = 0.0f;
        bool  have = tx_info_refresh(&msg_id, &alc, &pwr, NULL);
        gain_offset = clamp_gain(gain_offset + tx_level_block(&level, have, pwr, alc));
        size_t part = fill(block, TX_PLAYER_BLOCK, fill_ctx);
        if (part == 0) {
            break;
        }
        if (abort_check && abort_check(abort_check_ctx)) {
            aborted = true;
            break;
        }
        if (gain_offset == prev_gain_offset) {
            if (gain_offset != 0.0f) {
                audio_gain_db(block, part, gain_offset, block);
            }
        } else {
            audio_gain_db_transition(block, part, prev_gain_offset, gain_offset, block);
            prev_gain_offset = gain_offset;
        }
        audio_play(block, part); /* R1CBU 1.0: its default player, AUDIO_PLAY_RATE */
    }

    /* The learned gain offset is shared by every mode: same audio path. */
    param_f_set(cfg.ft8.output_gain_offset(),
                LV_MAX(gain_offset - base_gain_offset + play_gain_offset, LEARNED_MIN_DB));
    audio_play_wait();
    radio_set_modem(false);
    radio_set_freq((int32_t)radio_freq);
    audio_set_play_vol(param_f_get(cfg.audio.play_gain_db()));

    return !aborted;
}
