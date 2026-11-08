/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - the band bar between the 3D spectrum and the
 *  waterfall: a frequency scale over the band plan (band_plan.c).
 */

#pragma once

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define BAND_BAR_HEIGHT 26

/* The bar on parent; the segment tag on view (the spectrum). */
lv_obj_t *band_bar_create(lv_obj_t *parent, lv_obj_t *view);

/* Where it goes (screen y), shown or hidden (the 3D view only). */
void band_bar_set_geometry(lv_coord_t y, bool shown);

/* The frequencies at its left and right edges (the spectrum's). */
void band_bar_set_range(int32_t lo_hz, int32_t hi_hz);

/* The band plan changed (region setting, QTH Grid). */
void band_bar_refresh(void);

/* The dial frequency: tuned into another segment, its band and kind show
 * at the top left of the 3D view for a moment ("40m  CW"). */
void band_bar_set_dial(int32_t hz);
