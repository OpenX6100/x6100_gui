/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI - JS8 map (C API)
 *
 *  Where stations go on the Show Map view and which part of the world it
 *  shows (docs/MAP_PLAN.md). Everything here is maths and lookups: no LVGL.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AD1C's country file (cty.dat). Loaded once; the lookups below fall back
 * to "unknown" without it. False if the file can't be read or parsed. */
bool js8_map_load_countries(const char *path);
bool js8_map_countries_loaded(void);

/* Where a station goes: its grid if it has one (the square's centre), else
 * its callsign's region or country (`approx` set: drawn hollow). */
typedef struct {
    double lat, lon;
    bool   approx;        /* placed by callsign, not grid */
    char   continent[3];  /* "NA" ... "" unknown */
    char   where[40];     /* the grid, else "British Columbia", "Canada" ... */
} js8_map_place_t;

bool js8_map_place(const char *call, const char *grid, js8_map_place_t *out);

/* Your continent: from your callsign, else the country nearest your grid.
 * False (empty out) if neither is known. */
bool js8_map_my_continent(const char *call, const char *grid, char out[3]);

/* The view button: Auto switches between your continent and the world. */
typedef enum {
    JS8_MAP_AUTO,
    JS8_MAP_CLOSE,
    JS8_MAP_WORLD,
} js8_map_mode_t;

/* A view: the centre, the scale and the size in pixels. */
typedef struct {
    double lon_c, merc_c, px_deg;
    int    width, height;
} js8_map_view_t;

/* One station for choosing the view. */
typedef struct {
    double lat, lon;
    char   continent[3];
} js8_map_point_t;

/* The view for the stations shown:
 *   Auto  - close-in while every station is on `my_continent` (or it's
 *           unknown), else fitted to them all ("world");
 *   Close - fitted to you and the stations on your continent only;
 *   World - the whole inhabited world (-56 to +75 degrees), centred on
 *           your longitude; the world repeats at the sides.
 * Fitted views keep a margin and are never closer in than about 40 x 18
 * degrees. Returns true when the result is a world view (for the legend). */
bool js8_map_choose_view(js8_map_mode_t mode, double my_lat, double my_lon, const char *my_continent,
                         const js8_map_point_t *pts, unsigned n, int width, int height, js8_map_view_t *out);

/* Screen position (pixels, (0,0) top left) of a point in a view. */
void js8_map_project(const js8_map_view_t *v, double lat, double lon, float *x, float *y);

/* The screen rectangle of a locator's area (its 4 characters for a
 * station's square): x0 < x1, y0 < y1. False if `grid` isn't a locator. */
bool js8_map_grid_rect(const js8_map_view_t *v, const char *grid, float *x0, float *y0, float *x1, float *y1);

/* The curved path from 1 to 2 on the screen: `n` + 1 points into xs/ys
 * (both `n` + 1 long). */
void js8_map_path(const js8_map_view_t *v, double lat1, double lon1, double lat2, double lon2, int n, float *xs,
                  float *ys);

/* Great-circle distance, km, and the bearing from 1 to 2, degrees 0-359
 * (the long path is the other way round: +180). */
double js8_map_distance_km(double lat1, double lon1, double lat2, double lon2);
double js8_map_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/* ---- Worked before (the radio's QSO log, all bands and modes) ------------ */

/* Read the grids and countries you've worked from the radio's QSO log
 * (qso_log.db, opened read-only). Needs the country file for countries.
 * False if the log can't be read: then nothing counts as new. */
bool js8_map_load_worked(const char *db_path);
/* A QSO just logged. */
void js8_map_worked_add(const char *call, const char *grid);
/* How many grids / countries worked (0 before a load). */
unsigned js8_map_worked_grids(void);
unsigned js8_map_worked_countries(void);

typedef enum {
    JS8_MAP_NEW_NONE, /* worked before, or the log isn't known */
    JS8_MAP_NEW_GRID, /* a grid square never worked; `what` = "FN31" */
    JS8_MAP_NEW_DXCC, /* a country never worked; `what` = "Japan" */
} js8_map_new_t;

/* Would a QSO with this station be a new country or grid square? A new
 * country wins over a new grid. */
js8_map_new_t js8_map_new_kind(const char *call, const char *grid, char *what, unsigned size);

/* ---- The base map (Natural Earth, tools/map_data) ------------------------ */

typedef struct js8_map_data js8_map_data_t;

/* The base-map data file; NULL if it can't be read or isn't one. */
js8_map_data_t *js8_map_data_load(const char *path);
void            js8_map_data_free(js8_map_data_t *d);

/* Draw the base map for view `v` (land, lakes, borders, state lines,
 * Maidenhead field lines, GridTracker's Dark Gray colours) into `argb`:
 * v->width x v->height pixels of 0xAARRGGBB (lv_color_t at 32-bit colour
 * depth), `stride` pixels per row. Takes tens of ms: not on the LVGL
 * thread. False without data. */
bool js8_map_render_base(const js8_map_data_t *d, const js8_map_view_t *v, uint32_t *argb, int stride);

#ifdef __cplusplus
}
#endif
