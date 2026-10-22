/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Krzysztof Myszkowski SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - on-screen list for the digital-mode windows
 *
 *  Instead of pages of buttons, the rarely used actions of a digital-mode
 *  window (its macros, its settings) open as a list over the window: the
 *  MFK moves through it, pressing the MFK picks the item, ESC closes it.
 */

#pragma once

#include <stdbool.h>

#include "lvgl/lvgl.h"

#define DIGI_MENU_MAX 12

/* `item` picked, 0-based. Called after the list is closed. */
typedef void (*digi_menu_cb_t)(int item, void *ctx);

/*
 * Opens the list over `parent`. `items` are copied. While it is open it
 * has the keyboard group; `back` (the window's list, say) gets it again
 * when it closes, or, with `back` NULL, whatever had the focus before.
 * A second open replaces the first.
 */
void digi_menu_open(lv_obj_t *parent, const char *title, const char *const *items, int n,
                    digi_menu_cb_t cb, void *ctx, lv_obj_t *back);

bool digi_menu_is_open(void);

/* Closes it without picking anything (the window going away, a button). */
void digi_menu_close(void);
