/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - RTTY squelch
 */

#include "rtty_sql.h"

#include <math.h>

#include "cfg/cfg_api.h"
#include "rtty.h"

#define SQL_MAX     90
#define SQL_HYST_DB 2.0f

/*
 * The v0.34 build decoded at 44100 / 4 = 11025 Hz; v1.0 decodes at
 * 48000 / 3 = 16000 Hz. The demodulator integrates over a symbol, which
 * is now 16000/11025 times as many samples, and the bin energies come out
 * 20*log10(16000/11025) = 3.2 dB higher for the same signal and the same
 * noise (measured: +3.1..+3.3 dB at 45.45 and 75 Bd, tone and noise
 * alike). Taking it off keeps the displayed level and the threshold on
 * the scale the operator knows from the old build.
 */
#define OLD_CAPTURE_RATE 11025.0f

static float noise_floor_db = 0.0f;
static bool  floor_primed   = false;
static bool  sql_open       = false;
static float sql_snr_db     = 0.0f;
static float sql_level_db   = 0.0f;

void rtty_sql_reset(void)
{
    /* Forget the old floor so it learns the band again. */
    floor_primed = false;
    sql_open = false;
}

void rtty_sql_feed(float pwr0_db, float pwr1_db)
{
    /* Total energy of both tone bins, independent of which one wins.
     * rtty.c hands over the two bins in dB; a bin with no energy at all
     * comes as -inf, which powf() turns back into 0. */
    float e = powf(10.0f, pwr0_db / 10.0f) + powf(10.0f, pwr1_db / 10.0f);
    float level_db = 10.0f * log10f(e + 1e-12f)
                     - 20.0f * log10f((float)RTTY_CAPTURE_RATE / OLD_CAPTURE_RATE);
    int32_t thr_i;

    if (!floor_primed) {
        noise_floor_db = level_db;
        sql_level_db = level_db;
        floor_primed = true;
    }

    /* Smoothed level: what the operator sees and what the gate uses. */
    sql_level_db += (level_db - sql_level_db) * 0.05f;

    /* Fast falling, slow rising floor, so a steady carrier is not taken
     * for background noise. */
    if (level_db < noise_floor_db)
        noise_floor_db += (level_db - noise_floor_db) * 0.10f;
    else
        noise_floor_db += 0.002f;

    sql_snr_db = sql_level_db - noise_floor_db;

    thr_i = param_i_get(cfg.rtty.squelch());
    if (thr_i <= 0) {
        sql_open = true;
        return;
    }

    if (!sql_open) {
        if (sql_level_db > (float)thr_i)
            sql_open = true;
    } else {
        if (sql_level_db < (float)thr_i - SQL_HYST_DB)
            sql_open = false;
    }
}

bool rtty_squelch_is_open(void)
{
    return sql_open;
}

uint8_t rtty_change_squelch(int16_t df)
{
    int32_t v = param_i_get(cfg.rtty.squelch());

    if (df == 0)
        return (uint8_t)v;

    v += (df > 0) ? 1 : -1;
    if (v < 0)
        v = 0;
    if (v > SQL_MAX)
        v = SQL_MAX;

    param_i_set(cfg.rtty.squelch(), v);
    return (uint8_t)v;
}

float rtty_get_level_db(void)
{
    return sql_level_db;
}

float rtty_get_noise_db(void)
{
    return noise_floor_db;
}

float rtty_get_snr_db(void)
{
    return sql_snr_db;
}
