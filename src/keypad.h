/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include <stdint.h>
#include "lvgl/lvgl.h"
#include "events.h"

typedef struct {
    int             fd;
    
    lv_indev_drv_t  indev_drv;
    lv_indev_t      *indev;
    
    int             evdev_state;
    int             evdev_key;
} keypad_t;

keypad_t * keypad_init(char *dev_name);

/* How long a bottom button must be held to count as a hold; 0 = the
 * default (1 s). Apps may shorten it while open. */
void keypad_set_long_time(uint32_t ms);
