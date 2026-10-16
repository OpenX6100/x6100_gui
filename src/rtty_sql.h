/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *
 *  Xiegu X6100 LVGL GUI - RTTY squelch
 *
 *  The RTTY bit decision only compares which of the two tones is
 *  stronger, so with no signal it still flips on noise and the framing
 *  now and then lines up by chance, printing junk. This gate also
 *  requires the band to contain something: characters pass only while
 *  the energy in the two tone bins is above a level set by the operator.
 *
 *  The threshold is an ABSOLUTE level, not an offset from a tracked
 *  noise floor. A tracked floor made the gate move by itself: a strong
 *  station lifts the floor estimate and the effective threshold drifts
 *  with it. A fixed level behaves like the squelch on a conventional
 *  radio - set once for the band and it stays put. The floor is still
 *  estimated, but only to show where the noise sits.
 *
 *  Stored in the settings database as "rtty_squelch" (0..90, 0 = off),
 *  the same key the v0.34 build used.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decoder side, called from rtty.c. */
void    rtty_sql_reset(void);                       /* RX (re)start        */
void    rtty_sql_feed(float pwr0_db, float pwr1_db); /* one per demod step */
bool    rtty_squelch_is_open(void);

/* Operator side: step the threshold by the sign of df (0 just reads it). */
uint8_t rtty_change_squelch(int16_t df);

/* Live readings for the SQL button and the MFK message. */
float   rtty_get_level_db(void);    /* smoothed absolute level          */
float   rtty_get_noise_db(void);    /* tracked floor, for display only  */
float   rtty_get_snr_db(void);      /* level above the floor            */

#ifdef __cplusplus
}
#endif
