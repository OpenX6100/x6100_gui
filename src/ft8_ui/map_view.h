/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4/FT2: the stations heard on a map
 *
 *  Show: Map (page 2) puts the shared map (map_widget, map_scene) over the
 *  waterfall and the list: stations heard in the last 15 minutes in their
 *  grid squares (by callsign where no grid came), green calling CQ, yellow
 *  calling you, grey worked before, a red path for your QSO and thin grey
 *  lines for the QSOs heard between others. Turning the MFK picks a station
 *  (west to east), pressing it calls that station as pressing its row in
 *  the list does. Everything here runs on the LVGL thread.
 */

#pragma once

#include "table_view.h"

#include "lvgl/lvgl.h"

#include <stdbool.h>

typedef struct {
    lv_obj_t             *waterfall;    /* hidden under the map */
    table_view_press_cb_t on_press;     /* call this station (its last message) */
    void (*on_close)(void);             /* ESC */
    void (*on_vol_change)(int32_t d);   /* VOL turned */
} map_view_actions_t;

/* Every message the list gets (RX, TX, Start QSO): the stations and your
 * QSO follow them, shown or not. */
void map_view_add(const cell_data_t *cell);

/* A new band or mode: no stations. */
void map_view_clear(void);

/* PSK Reporter: `call` at `grid` heard you at UTC seconds `when` (a line
 * from you, brightness by snr, until it's older than the time set here). */
void map_view_heard_you(const char *call, const char *grid, int snr, int64_t when);
void map_view_set_you_minutes(int minutes); /* 0: off, no lines or legend */
void map_view_set_you_up(bool up);         /* false: says it's not connected */
void map_view_forget_you(void);

/* The map over `parent` (the FT8 window), focused for the MFK, or the list
 * back (`table` focused again). False if the map couldn't be made. */
bool map_view_show(lv_obj_t *parent, lv_obj_t *table, bool on, const map_view_actions_t *actions);
bool map_view_shown(void);

/* The MFK back to the map, if it shows (after the keyboard window). */
void map_view_focus(void);

/* Auto -> Close-in -> World; returns the new view's name. */
const char *map_view_next_mode(void);

/* "FT8 20m": the status line's start. */
void map_view_set_title(const char *title);

/* The window closes: timer, buffers, map data. */
void map_view_destroy(void);
