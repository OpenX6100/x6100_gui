/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - the drive level of a digital-mode transmission
 */

#include "tx_level.h"

#include <math.h>

/* Below the setting by less than this (or reading it): there. */
#define NEAR_W 0.05f

void tx_level_start(tx_level_t *l, float target_w) {
    *l          = (tx_level_t){0};
    l->target_w = target_w;
}

static float decide(tx_level_t *l);

float tx_level_block(tx_level_t *l, bool have, float pwr_w, float alc) {
    l->blocks++;
    if (l->blocks <= TX_LEVEL_SETTLE_BLOCKS) return 0.0f;
    /* A decision every half second of blocks, on the readings that came in
     * it. Counting readings instead made the window as long as the radio
     * took to send twelve of them (IU1WXY's find). Without any reading the
     * window waits for one. */
    l->wb++;
    if (have) {
        if (alc < 0.0f) alc = 0.0f;
        if (alc > 10.0f) alc = 10.0f;
        l->n++;
        l->sum_pwr += pwr_w;
        l->sum_alc += alc;
    }
    if (l->wb < TX_LEVEL_WINDOW_BLOCKS || l->n == 0) return 0.0f;
    return decide(l);
}

static float decide(tx_level_t *l) {
    float p = l->sum_pwr / l->n, a = l->sum_alc / l->n;
    l->n = l->wb = 0;
    l->sum_pwr = l->sum_alc = 0.0f;

    float d = 0.0f;
    if (a > TX_LEVEL_ALC_HIGH) {
        /* Overdriven: half the old formula a window (ALC 1: -0.2 dB,
         * 3: -0.6, 5: -1.2, more: the limit). */
        d = (log10f(log10f(11.1f - a)) * 20.0f - 0.38f) * 0.5f;
        if (d > 0.0f) d = 0.0f;
        if (d < -TX_LEVEL_DOWN_MAX_DB) d = -TX_LEVEL_DOWN_MAX_DB;
        l->held = true;
    } else if (l->target_w > 0.0f && p > l->target_w * 1.25f + TX_LEVEL_PWR_STEP_W) {
        d = -10.0f * log10f(p / l->target_w) * 0.5f; /* well over the setting */
        if (d < -TX_LEVEL_DOWN_MAX_DB) d = -TX_LEVEL_DOWN_MAX_DB;
        l->held = true;
    } else if (!l->held) {
        if (a >= TX_LEVEL_ALC_SHOWS || (l->target_w > 0.0f && p >= l->target_w - NEAR_W)) {
            l->held = true; /* there: the ALC holds the power at the setting */
        } else if (l->up_db < TX_LEVEL_UP_MAX_DB) {
            /* The ALC at zero: up, faster while far short (under half the
             * setting), then gently into the ALC's onset. */
            d = l->target_w > 0.0f && p < l->target_w * 0.5f ? TX_LEVEL_UP_FAST_DB : TX_LEVEL_UP_SLOW_DB;
            if (l->up_db + d > TX_LEVEL_UP_MAX_DB) d = TX_LEVEL_UP_MAX_DB - l->up_db;
            l->up_db += d;
        }
    }
    return d;
}
