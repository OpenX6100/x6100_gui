/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - a station map in one call, on the shared map
 *
 *  For apps whose map is a list of stations (FT8/FT4/FT2, WSPR): the app
 *  hands over its stations (colour, tags, a path from you), lines between
 *  stations, the selected one and the legend; this draws them the way the
 *  JS8 map does: squares in their grid squares (hollow where placed by
 *  callsign), you in orange, the selected station in red with its details,
 *  "CQ" tags, a count tag where stations share a square, a status line at
 *  the top right, a hint line and the legend at the bottom.
 *
 *  The map itself is made with map_widget_create(); the part stations are
 *  fitted into ends MAP_SCENE_BOTTOM_H above its bottom edge.
 */

#pragma once

#include "map_widget.h"

#define MAP_SCENE_LEGEND_H 26
#define MAP_SCENE_HINT_H   26
#define MAP_SCENE_BOTTOM_H (MAP_SCENE_LEGEND_H + MAP_SCENE_HINT_H + 4) /* legend, hint line, a margin */

#define MAP_SCENE_GREEN   0x00FF00 /* GridTracker's CQ green */
#define MAP_SCENE_DIM     0x008C00 /* heard, not calling CQ */
#define MAP_SCENE_YELLOW  0xFFFF00 /* calling you */
#define MAP_SCENE_GREY    0x9A9A9A /* worked before */
#define MAP_SCENE_BLUE    0x42A5F5 /* heard you (the JS8 map's paths) */
#define MAP_SCENE_RED     0xFF3030 /* your QSO */
#define MAP_SCENE_LINK    0xC8C8C8 /* other stations' QSOs */

typedef struct {
    char     call[16];
    char     grid[9];   /* "" if not known: placed by callsign */
    uint32_t color;     /* its square */
    lv_opa_t opa;       /* 0: the map's usual see-through square */
    int      snr;
    bool     has_snr;
    bool     cq;        /* the "CQ" tag */
    bool     label;     /* its call beside it, in its colour */
    char     tag[8];    /* a small tag on its bottom right ("-12"), "" none */
    uint32_t path;      /* a path from you in this colour, 0 none */
    uint8_t  path_w;    /* its width, 0: 3 px */
} map_scene_station_t;

/* A thin line between two stations (indexes into the list). */
typedef struct {
    int      a, b;
    uint32_t color;
    lv_opa_t opa;
} map_scene_link_t;

typedef struct {
    const map_scene_station_t *st;
    unsigned                   n;
    const map_scene_link_t    *links;
    unsigned                   nlinks;
    int                        sel;      /* the selected station, -1 none */
    const char                *my_call;
    const char                *my_grid;
    js8_map_mode_t             mode;
    bool                       miles;
    const map_legend_item_t   *legend;
    unsigned                   nlegend;
    const char                *status;   /* top right, "" none */
    const char                *hint;     /* bottom left, above the legend, "" none */
} map_scene_t;

/* Draws the scene; skipped when nothing in it changed and the view didn't
 * move (unless `force`). */
void map_scene_draw(const map_scene_t *sc, bool force);

/* The station after (`dir` 1) or before (-1) `sel`, left to right across
 * the map as last drawn (top to bottom where they line up); from none, the
 * first or the last. -1 if none is shown. */
int map_scene_step(const map_scene_t *sc, int sel, int dir);

/* "  1234 km" (or mi) from you to a station's place, "" if either isn't known. */
void map_scene_distance(const map_scene_t *sc, int i, char *buf, size_t size);
