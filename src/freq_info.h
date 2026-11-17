/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t * freq_info_init(lv_obj_t * parent);

/* Bluetooth's icons (bt_icons_17) right of the mode, for while a window
 * hides the status bar; highlighted when `connected`. NULL hides them. */
void freq_info_bt(const char *text, bool connected);

// const char* info_params_mode_label_get();
// const char* info_params_agc();
// const char* info_params_vfo_label_get();

// void info_lock_mode(bool lock);

#ifdef __cplusplus
}
#endif
