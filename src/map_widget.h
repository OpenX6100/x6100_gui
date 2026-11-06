/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - the shared map
 *
 *  The map the JS8 app shows (Show Map), for any app: GridTracker's look,
 *  the base map drawn by the radio for the view (src/js8/map_render),
 *  stations in their grid squares or placed by callsign (src/js8/js8_map),
 *  curved paths, labels that keep off each other, "CQ" and count tags, a
 *  legend. The app decides what to show and draws it between
 *  map_widget_begin() and map_widget_end() with the calls below.
 *
 *  One map at a time (one app window is open at a time): the module keeps
 *  its state, as table_view does. Made in the app's window, it goes with
 *  it; map_widget_free() releases the buffers and the map data.
 */

#pragma once

#include "js8/js8_map.h"

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#define MAP_WIDGET_OCEAN      0x232227 /* GridTracker Dark Gray's water */
#define MAP_WIDGET_HOME       0xFFA600 /* GridTracker's QTH orange */
#define MAP_WIDGET_SELECT     0xFF3030 /* the selected station */
#define MAP_WIDGET_CQ_BG      0x00E000 /* the "CQ" tag: black on the heard green */
#define MAP_WIDGET_SQUARE_OPA 136      /* GridTracker's grid alpha */
#define MAP_WIDGET_PATH_N     48       /* points along a curved path */

/* Where the map goes in `parent` (x, y, w, h), how high the part the
 * stations are fitted into is (the rest, below, is for the legend and
 * whatever the app puts there), and how far down labels may go. */
typedef struct {
    int x, y, w, h;
    int fit_h;
    int label_bottom;
} map_widget_geom_t;

/* Makes the map in `parent` (hidden), once; true if it is there. Reads the
 * map data and, if not yet loaded, the country list (cty.dat: `cty_sd`
 * first, a newer one put on the card, then `cty`). The rounded or cut
 * corners of the parent's frame image are kept over the map. */
bool map_widget_create(lv_obj_t *parent, const map_widget_geom_t *g, const char *data_path, const char *cty_sd,
                       const char *cty);
bool map_widget_made(void);

/* After the parent window was deleted (its objects went with it): the
 * buffers and the map data. */
void map_widget_free(void);

lv_obj_t *map_widget_box(void);    /* for the app's own labels on the map */
lv_obj_t *map_widget_canvas(void);
const map_widget_geom_t *map_widget_geom(void);

void map_widget_show(bool on);
void map_widget_view_reset(void);  /* the base map redrawn at the next fit */

/* The view for these stations (js8_map_choose_view() in the fitted part,
 * drawn over the whole map); the base map redrawn only if it moved. True
 * if it was (`world` set to whether it is a world view). */
bool map_widget_fit(js8_map_mode_t mode, double my_lat, double my_lon, const char *my_continent,
                    const js8_map_point_t *pts, unsigned n, bool *world);
const js8_map_view_t *map_widget_view(void);
bool map_widget_world(void);

/* FNV-1a over what is drawn: the app keeps the last value and skips the
 * redraw when nothing changed. */
uint32_t map_widget_hash(uint32_t h, const void *p, size_t n);
#define MAP_WIDGET_HASH_START 2166136261u

/* A redraw: the base map copied in, no label places taken ... the
 * frame's corners put back, shown. */
void map_widget_begin(void);
void map_widget_end(void);

void map_widget_rect_ex(int x, int y, int w, int h, uint32_t fill, lv_opa_t fill_opa, uint32_t border,
                        lv_opa_t border_opa, int bw, lv_coord_t radius);
void map_widget_rect(int x, int y, int w, int h, uint32_t fill, lv_opa_t fill_opa, uint32_t border, int bw,
                     lv_coord_t radius);
void map_widget_text(int x, int y, int w, const lv_font_t *font, uint32_t color, const char *text);

/* Places labels and tags keep off (the legend, the app's own labels). */
bool map_widget_overlaps(const lv_area_t *a);
void map_widget_take(lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2);

/* A label on a dark box beside (x, y): right of it, else left, above or
 * below, wherever it covers nothing taken. A forced one takes the first
 * place that fits on the map; any other is left out if none is free. */
void map_widget_label(int x, int y, const char *text, uint32_t fg, uint32_t bg, lv_opa_t bg_opa, int dx, bool force);

/* A station's square in its grid square (6-20 px, `grow` more or less), or
 * a hollow box where it is placed by callsign. Its centre in *cx, *cy;
 * returns its side. */
int map_widget_station_mark(const char *grid, const js8_map_place_t *pl, uint32_t color, lv_opa_t opa,
                            uint32_t border, lv_opa_t line_opa, int grow, int *cx, int *cy);

/* The curved (great-circle) path from 1 to 2, as screen points (MAP_WIDGET_PATH_N
 * + 1 of them, clamped to +-4000). */
void map_widget_path_points(double lat1, double lon1, double lat2, double lon2, lv_point_t *p);
void map_widget_line(const lv_point_t *p, int n, const lv_draw_line_dsc_t *line);

/* A small tag ("CQ", a count) pinned on a mark's top right (or, `below`,
 * bottom right) corner: reserved with map_widget_badge_box*() before the
 * labels are placed (false if there's no room), drawn last. */
bool map_widget_badge_box_at(int x, int y, int side, const char *text, bool below, lv_area_t *box);
bool map_widget_badge_box(int x, int y, int side, const char *text, lv_area_t *box);
void map_widget_badge_ex(const lv_area_t *box, const char *text, uint32_t bg, uint32_t fg);
void map_widget_badge(const lv_area_t *box, const char *text);

/* The legend: see-through, a dark box only behind the items on the left
 * and behind `right` (the view, or a hint) on the right; its row starts at
 * y0, `h` high. */
typedef enum {
    MAP_LEGEND_SQUARE,
    MAP_LEGEND_LINE,
    MAP_LEGEND_HOLLOW,
} map_legend_kind_t;

typedef struct {
    uint32_t          color;
    map_legend_kind_t kind;
    const char       *text;
} map_legend_item_t;

void map_widget_legend(const map_legend_item_t *items, unsigned n, const char *right, int y0, int h);

/* "North America" ... "Your continent" for a continent code. */
const char *map_widget_continent_name(const char *code);
