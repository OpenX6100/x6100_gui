/*
 *  SPDX-License-Identifier: GPL-3.0-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - an app's waterfall, as JS8's and XARA's
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The waterfall JS8 and XARA draw, for any app's window: its own receive
 * audio (12 kHz), 15 rows a second released by the clock, colours from the
 * noise floor (settling over ~2 s), each row blended into the ones before
 * (JS8's "Light"), drawn on the display's lower plane by js8_wf.c. While the
 * radio transmits, the rows show the audio going out (from the playback
 * device's monitor), at their place on the air, with a red edge and a "TX"
 * tag, as XARA does.
 *
 * `box` becomes the hole the waterfall shows through: give it its size and
 * place first; its children (finder, marks) are drawn over the waterfall.
 * One at a time (one app window is open). GUI thread. */
typedef struct {
    int  lo_hz, hi_hz;  /* audio across the width */
    /* Narrow spans (WSPR: 375 Hz): each row looks at the last FFT length of
     * audio, not just its own 1/15 s, so the bins are as fine as they say. */
    bool sliding;
} app_wf_cfg_t;

bool app_wf_start(lv_obj_t *box, const app_wf_cfg_t *opt);
/* Puts the lower plane back for the main screen (black there). */
void app_wf_stop(void);
void app_wf_set_range(int lo_hz, int hi_hz);
void app_wf_clear(void);
/* Our audio going out at audio_hz goes on the air where the receiver hears
 * audio_hz + shift_hz (FT8 sends at 1325 Hz and moves the VFO by
 * tx_freq - 1325). Any thread; set it before keying. */
void app_wf_set_tx_shift(int shift_hz);

#ifdef __cplusplus
}
#endif
