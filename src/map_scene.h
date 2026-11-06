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
 *  callsign), you in orange, the selected station in red (its details on
 *  the hint line),
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
    char     extra[24]; /* more for the selected one's details ("23 dBm"), "" none */
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
    const char                *status2;  /* under it ("DX ZL2AB 11300 km", may hold a \n), "" none */
    const char                *hint;     /* bottom left, above the legend, "" none */
} map_scene_t;

/* Draws the scene; skipped when nothing in it changed and the view didn't
 * move (unless `force`). */
void map_scene_draw(const map_scene_t *sc, bool force);

/* The station after (`dir` 1) or before (-1) `sel`, left to right across
 * the map as last drawn (top to bottom where they line up); from none, the
 * first or the last. -1 if none is shown. */
int map_scene_step(const map_scene_t *sc, int sel, int dir);

/* How far a station is from you, km (by grid, else by callsign); false if
 * either place isn't known. For the apps' "DX" (the farthest heard). */
bool map_scene_km(const char *my_call, const char *my_grid, const char *call, const char *grid, double *km);

/* "  1234 km" (or mi) from you to a station's place, "" if either isn't known. */
void map_scene_distance(const map_scene_t *sc, int i, char *buf, size_t size);

/* ---- The map in an app's window ----------------------------------------
 *
 * Show: Map puts the map over the whole inside of the window's frame and
 * gives it the MFK (the app's list gets it back with Show: List). The app
 * keeps its stations and draws them with map_scene_draw() in `redraw`,
 * called once a second while the map shows and whenever it asks (a new
 * station waits for the next second: drawing per message would redraw the
 * whole map dozens of times in a burst). */
typedef struct {
    lv_obj_t *hide;                   /* hidden while the map shows (the app's waterfall), NULL none */
    void (*redraw)(bool force);
    void (*on_step)(int dir);         /* MFK turned: pick the next station, map_scene_step() */
    void (*on_press)(void);           /* MFK pressed, NULL: nothing */
    void (*on_close)(void);           /* ESC: the app closes */
    void (*on_vol_change)(int32_t d); /* VOL turned */
} map_scene_app_t;

/* The map over `parent` (the window) or the list back (`list` focused
 * again). The list and app->hide are hidden under the map, so their
 * updates don't redraw it. False if the map couldn't be made (a message
 * says so). */
bool map_scene_show(lv_obj_t *parent, lv_obj_t *list, bool on, const map_scene_app_t *app);
bool map_scene_shown(void);

/* The MFK back to the map (after a window that took it, e.g. a keyboard). */
void map_scene_focus(void);

/* Auto -> Close-in -> World (the map redrawn); returns the new view's name. */
const char    *map_scene_next_mode(void);
js8_map_mode_t map_scene_mode(void);

/* The window closes: the timer, the buffers, the map data (the objects go
 * with the window). */
void map_scene_destroy(void);
