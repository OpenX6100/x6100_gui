/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSPR: the spots on a map
 *
 *  Show: Map (page 2) puts the shared map (map_scene) over the waterfall
 *  and the list: the stations decoded here in the last hour in their grid
 *  squares, green, each with its SNR on a tag, fading over the hour.
 *  Turning the MFK picks one: call, grid, SNR, its power, distance and
 *  beam heading.
 *
 *  Stations that heard you (WSPRnet's spots of your signal, fetched from
 *  wspr.live by wspr_net.c) go in with wspr_map_heard_you(): a blue path from
 *  you, blue squares where only they heard you, "heard you" in the legend
 *  once there are any. Everything here runs on the LVGL thread.
 */

#pragma once

#include "wspr_worker.h"

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

/* A spot decoded here (scheduler_put() it from the worker thread: `spot`
 * is a wspr_spot_t). */
void wspr_map_spot_cb(void *spot);

/* A station that heard you, `snr` dB (from WSPRnet). */
void wspr_map_heard_you(const char *call, const char *grid, int snr);

/* A new band, or Clear: no spots. */
void wspr_map_clear(void);

/* "WSPR 20m": the status line's start. */
void wspr_map_set_title(const char *title);

/* The map over `parent` (the WSPR window) or the list back (`list`
 * focused again); `waterfall` is hidden under it. False if the map
 * couldn't be made. */
bool wspr_map_show(lv_obj_t *parent, lv_obj_t *list, lv_obj_t *waterfall, bool on, void (*on_close)(void),
                   void (*on_vol_change)(int32_t d));
bool wspr_map_shown(void);

/* Auto -> Close-in -> World; returns the new view's name. */
const char *wspr_map_next_mode(void);

/* The window closes. */
void wspr_map_destroy(void);
