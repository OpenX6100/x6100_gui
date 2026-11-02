/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - JS8 speeds (submodes), C interface.
 *  The numbers come from desktop JS8Call's JS8Submode.cpp; see
 *  docs/T6_PLAN.md.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JS8_SPEED_NORMAL,
    JS8_SPEED_FAST,
    JS8_SPEED_TURBO,
    JS8_SPEED_SLOW,
    JS8_SPEED_ULTRA, /* Ultra (experimental: a Setting puts it on the Speed button) */
    JS8_SPEED_COUNT,
} js8_speed_t;

const char *js8_speed_name(js8_speed_t s);      /* "Normal" */
char        js8_speed_letter(js8_speed_t s);    /* 'N', 'F', 'T', 'S', 'U' */
int         js8_speed_bandwidth_hz(js8_speed_t s);
int         js8_speed_period_s(js8_speed_t s);  /* slot length */
int         js8_speed_rx_threshold_hz(js8_speed_t s); /* "same station" offset window */
int         js8_speed_max_offset_hz(js8_speed_t s);   /* keeps the signal below 3000 Hz */
bool        js8_speed_heartbeats(js8_speed_t s);      /* false in Turbo and Ultra, as on desktop */
int         js8_speed_rx_mask(js8_speed_t s);         /* JS8_SUBMODE_* bit for js8_rx_create */
int         js8_speed_submode(js8_speed_t s);         /* desktop's numbering: 0, 1, 2, 4, 8 */
/* A decode's js8_rx_msg_t.submode (desktop numbering 0/1/2/4/8) as a speed. */
js8_speed_t js8_speed_from_submode(int submode);

#ifdef __cplusplus
}
#endif
