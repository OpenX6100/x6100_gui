/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Kris SP2KMO
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - on-screen list for the digital-mode windows
 *
 *  Instead of pages of buttons, the rarely used actions of a digital-mode
 *  window (its macros, its settings) open as a list over the window: the
 *  MFK moves through it, pressing the MFK picks the item, ESC closes it.
 *  A row with a value (a report, a squelch level) can instead be changed
 *  in place with the MFK, see digi_menu_set_adjust().
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

/* Rows a list can hold (it scrolls): every macro of PSK31 and Olivia (12)
 * and RTTY (16) plus the list's own rows. At 12, macros past the 10th or
 * 9th couldn't be sent, and PSK31's "New macro..." row sent one. */
#define DIGI_MENU_MAX 20

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

/* After digi_menu_open(): start on `row` instead of the first, e.g. a
 * list reopened after a press that changes a value, so the next press
 * changes it again rather than picking row 0. */
void digi_menu_select(int row);

/*
 * Rows whose value the MFK changes in place, as in Settings: pressing
 * the MFK on one of them turns the row orange, and each step of the MFK
 * then calls `adjust` with +1 or -1 (the row's item and the ctx of
 * digi_menu_open()); pressing the MFK again, or ESC, goes back to moving
 * through the list. `adjust` changes the value and shows it with
 * digi_menu_set_texts(). `rows` has bit n set for row n. Call after
 * digi_menu_open(), which clears it.
 */
typedef void (*digi_menu_adjust_cb_t)(int item, int dir, void *ctx);

void digi_menu_set_adjust(uint32_t rows, digi_menu_adjust_cb_t adjust);

/* New texts for the rows of the open list (as many as it has). */
void digi_menu_set_texts(const char *const *items, int n);

bool digi_menu_is_open(void);

/* Closes it without picking anything (the window going away, a button). */
void digi_menu_close(void);
