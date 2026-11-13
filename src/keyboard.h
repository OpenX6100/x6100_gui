/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  Copyright (c) 2022-2023 Belousov Oleg aka R1CBU
 */

#pragma once

#include "lvgl/lvgl.h"

#define KEYBOARD_F1         (0xFFBE)
#define KEYBOARD_F2         (0xFFBF)
#define KEYBOARD_F3         (0xFFC0)
#define KEYBOARD_F4         (0xFFC1)
#define KEYBOARD_F5         (0xFFC2)
#define KEYBOARD_F6         (0xFFC3)
#define KEYBOARD_F7         (0xFFC4)
#define KEYBOARD_F8         (0xFFC5)
#define KEYBOARD_F9         (0xFFC6)
#define KEYBOARD_F10        (0xFFC7)
#define KEYBOARD_F11        (0xFFC8)
#define KEYBOARD_F12        (0xFFC9)

#define KEYBOARD_SCRL_LOCK  (0xFF14)
#define KEYBOARD_PRINT      (0xFF61)
#define KEYBOARD_PRINT_SCR  (0xFD1D)

#define KEYBOARD_PGUP       (0xFF55)
#define KEYBOARD_PGDN       (0xFF56)

extern lv_group_t *keyboard_group;

void keyboard_init();
bool keyboard_ready();

/* A Bluetooth keyboard is connected (looked for when an input device comes
 * or goes). */
bool keyboard_bt_connected();
