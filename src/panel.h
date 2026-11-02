/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <unistd.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

lv_obj_t * panel_init(lv_obj_t *parent);

void panel_set_height(lv_coord_t h);

void panel_hide();
void panel_clear();
void panel_update_visibility(bool clear);
void panel_add_text(const char * text);

/* Echo of what we transmit, on its own line and in its own colour, so
 * a QSO reads as a conversation. Used by the RTTY transmitter. */
void panel_add_tx_text(const char * text);
void panel_set_info(const char * text);

#ifdef __cplusplus
}
#endif
