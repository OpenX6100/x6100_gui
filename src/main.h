/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "rotary.h"
#include "encoder.h"

#define VERSION "v1.2.0"


extern rotary_t     *vol;
extern encoder_t    *mfk;
extern lv_obj_t     *overlay_scr;
extern lv_obj_t     *primary_scr;
