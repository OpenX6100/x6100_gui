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

lv_obj_t *band_bar_create(lv_obj_t *parent);

/* Where it goes (screen y), shown or hidden (the 3D view only). */
void band_bar_set_geometry(lv_coord_t y, bool shown);

/* The frequencies at its left and right edges (the spectrum's). */
void band_bar_set_range(int32_t lo_hz, int32_t hi_hz);

/* The band plan changed (region setting, QTH Grid). */
void band_bar_refresh(void);
