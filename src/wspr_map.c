/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - WSPR: the spots on a map (see wspr_map.h)
 */

#include "wspr_map.h"

#include "cfg/cfg_api.h"
#include "ft8_ui/table_view.h"
#include "map_scene.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define ST_MAX   100
#define HEARD_MS (60 * 60 * 1000) /* spots of the last hour, as WSPRnet's map shows by default */

typedef struct {
    char    call[16];     /* "<K1ABC/P>" (type 3) fits */
    char    grid[9];
    int     snr;          /* the last spot here */
    int     power_dbm;
    int64_t heard_ms;     /* monotonic, its last spot here; 0: only heard you */
    int     you_snr;      /* how they heard you */
    int64_t you_ms;       /* when, 0: never */
} station_t;

static station_t st[ST_MAX];
static int       st_n;
static char      sel_call[16];
static char      title[32] = "WSPR";

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int64_t last_ms(const station_t *s) {
    return s->heard_ms > s->you_ms ? s->heard_ms : s->you_ms;
}

/* A station: found, or added (the one longest unheard goes when full). */
static station_t *station(const char *call) {
    for (int i = 0; i < st_n; i++)
        if (strcasecmp(st[i].call, call) == 0) return &st[i];
    station_t *s;
    if (st_n < ST_MAX) {
        s = &st[st_n++];
    } else {
        s = &st[0];
        for (int i = 1; i < st_n; i++)
            if (last_ms(&st[i]) < last_ms(s)) s = &st[i];
    }
    memset(s, 0, sizeof(*s));
    snprintf(s->call, sizeof(s->call), "%s", call);
    return s;
}

static void redraw(bool force);

void wspr_map_spot_cb(void *data) {
    const wspr_spot_t *spot = data;
    /* "<...>": a type 3 hash no call heard here names, nowhere to put it */
    if (!spot->callsign[0] || strcmp(spot->callsign, "<...>") == 0) return;
    station_t *s = station(spot->callsign);
    if (spot->locator[0]) snprintf(s->grid, sizeof(s->grid), "%s", spot->locator);
    s->snr       = (int)lroundf(spot->snr_db);
    s->power_dbm = spot->power_dbm;
    s->heard_ms  = now_ms();
}

void wspr_map_heard_you(const char *call, const char *grid, int snr) {
    station_t *s = station(call);
    if (grid && grid[0]) snprintf(s->grid, sizeof(s->grid), "%s", grid);
    s->you_snr = snr;
    s->you_ms  = now_ms();
}

void wspr_map_clear(void) {
    st_n        = 0;
    sel_call[0] = '\0';
    if (map_scene_shown()) redraw(true);
}

void wspr_map_set_title(const char *t) {
    snprintf(title, sizeof(title), "%s", t);
    if (map_scene_shown()) redraw(false);
}

/* ---- The scene ---- */

static map_scene_station_t sc_st[ST_MAX];
static map_scene_t         scene;
static char                my_call[16], my_grid[9], status[96], status2[96], hint[64];

static const map_legend_item_t legend_here[] = {
    {MAP_SCENE_GREEN, MAP_LEGEND_SQUARE, "heard here (dB)"},
    {MAP_WIDGET_HOME, MAP_LEGEND_SQUARE, "you"},
};
static const map_legend_item_t legend_both[] = {
    {MAP_SCENE_GREEN, MAP_LEGEND_SQUARE, "heard here (dB)"},
    {MAP_SCENE_BLUE, MAP_LEGEND_LINE, "heard you (WSPRnet, dB)"},
    {MAP_WIDGET_HOME, MAP_LEGEND_SQUARE, "you"},
};

static void build(int64_t now) {
    snprintf(my_call, sizeof(my_call), "%s", PARAM_T_GET(cfg.callsign()));
    snprintf(my_grid, sizeof(my_grid), "%s", PARAM_T_GET(cfg.qth()));
    int n = 0, nhere = 0, nyou = 0, sel = -1;
    for (int i = 0; i < st_n; i++) {
        station_t *s    = &st[i];
        bool       here = s->heard_ms && now - s->heard_ms < HEARD_MS;
        bool       you  = s->you_ms && now - s->you_ms < HEARD_MS;
        if (!here && !you) continue;
        map_scene_station_t *d = &sc_st[n];
        memset(d, 0, sizeof(*d));
        snprintf(d->call, sizeof(d->call), "%s", s->call);
        snprintf(d->grid, sizeof(d->grid), "%s", s->grid);
        if (here) {
            d->color   = MAP_SCENE_GREEN;
            d->snr     = s->snr;
            d->has_snr = true;
            snprintf(d->tag, sizeof(d->tag), "%+d", s->snr);
            /* Fading over the hour, as the JS8 map's squares do. */
            int age = (int)((now - s->heard_ms) * 8 / HEARD_MS);
            d->opa  = (lv_opa_t)(MAP_WIDGET_SQUARE_OPA * (100 - age * 70 / 8) / 100);
            nhere++;
        } else {
            /* Only they heard you: the tag is how they heard you. */
            d->color = MAP_SCENE_BLUE;
            snprintf(d->tag, sizeof(d->tag), "%+d", s->you_snr);
        }
        int len = here ? snprintf(d->extra, sizeof(d->extra), "%d dBm", s->power_dbm) : 0;
        if (you) {
            snprintf(d->extra + len, sizeof(d->extra) - len, "%shears you %+d", len ? "  " : "", s->you_snr);
            d->path   = MAP_SCENE_BLUE;
            d->path_w = 2;
            nyou++;
        }
        if (sel_call[0] && strcasecmp(s->call, sel_call) == 0) sel = n;
        n++;
    }
    if (sel < 0) sel_call[0] = '\0';
    int    info  = param_i_get(cfg.ui.ft8_info());
    bool   miles = info == TABLE_INFO_MI || info == TABLE_INFO_COUNTRY_MI;
    /* The farthest heard here, and the farthest that heard you. */
    double best[2] = {-1, -1}, km;
    int    dx[2]   = {-1, -1};
    for (int i = 0; i < n; i++) {
        if (!map_scene_km(my_call, my_grid, sc_st[i].call, sc_st[i].grid, &km)) continue;
        for (int k = 0; k < 2; k++)
            if ((k == 0 ? sc_st[i].has_snr : sc_st[i].path != 0) && km > best[k]) {
                best[k] = km;
                dx[k]   = i;
            }
    }
    int len = snprintf(status, sizeof(status), "%s   %d heard here", title, nhere);
    if (nyou) snprintf(status + len, sizeof(status) - len, "  %d heard you", nyou);
    /* "DX VK2XYZ 12504 km", under it "DX heard you: VK3ABC 13204 km" */
    status2[0] = '\0';
    len        = 0;
    if (dx[0] >= 0)
        len = snprintf(status2, sizeof(status2), "DX %s %.0f %s", sc_st[dx[0]].call, miles ? best[0] * 0.621371 : best[0],
                       miles ? "mi" : "km");
    if (dx[1] >= 0 && len < (int)sizeof(status2))
        snprintf(status2 + len, sizeof(status2) - len, "%sDX heard you: %s %.0f %s", len ? "\n" : "", sc_st[dx[1]].call,
                 miles ? best[1] * 0.621371 : best[1], miles ? "mi" : "km");
    snprintf(hint, sizeof(hint), "%s", !n ? "No spots in the last hour" : sel < 0 ? "Turn MFK: pick a station" : "");
    scene    = (map_scene_t){.st      = sc_st,
                             .n       = (unsigned)n,
                             .sel     = sel,
                             .my_call = my_call,
                             .my_grid = my_grid,
                             .mode    = map_scene_mode(),
                             .miles   = miles, /* as the list */
                             .legend  = nyou ? legend_both : legend_here,
                             .nlegend = nyou ? 3 : 2,
                             .status  = status,
                             .status2 = status2,
                             .hint    = hint};
}

static void redraw(bool force) {
    build(now_ms());
    map_scene_draw(&scene, force);
}

/* ---- Keys, showing ---- */

static void select_step(int dir) {
    int i = map_scene_step(&scene, scene.sel, dir);
    if (i < 0) return;
    snprintf(sel_call, sizeof(sel_call), "%s", sc_st[i].call);
    redraw(false);
}

bool wspr_map_show(lv_obj_t *parent, lv_obj_t *list, lv_obj_t *waterfall, bool on, void (*on_close)(void),
                   void (*on_vol_change)(int32_t d)) {
    const map_scene_app_t app = {
        .hide          = waterfall,
        .redraw        = redraw,
        .on_step       = select_step,
        .on_close      = on_close,
        .on_vol_change = on_vol_change,
    };
    return map_scene_show(parent, list, on, &app);
}

bool wspr_map_shown(void) {
    return map_scene_shown();
}

const char *wspr_map_next_mode(void) {
    return map_scene_next_mode();
}

void wspr_map_destroy(void) {
    map_scene_destroy();
    st_n        = 0;
    sel_call[0] = '\0';
}
