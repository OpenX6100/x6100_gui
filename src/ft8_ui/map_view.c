/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - FT8/FT4/FT2: the stations heard on a map (see
 *  map_view.h)
 */

#include "map_view.h"

#include "../cfg/cfg_api.h"
#include "../map_scene.h"
#include "../msg.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#define ST_MAX     100
#define HEARD_MS   (15 * 60 * 1000) /* stations heard in the last 15 min */
#define CALLING_MS (2 * 60 * 1000)  /* yellow for 2 min after their last message to you */
#define QSO_MS     (2 * 60 * 1000)  /* your QSO's red path, 2 min after its last message */
#define LINK_MS    (2 * 60 * 1000)  /* others' QSOs: grey lines for 2 min */
#define YOU_OPA_MIN 110              /* the weakest report's line still shows on this screen */

typedef struct {
    char        call[13];
    char        grid[9];
    char        to[13];       /* whom their last message was to ("" CQ) */
    int         snr;
    int64_t     heard_ms;     /* monotonic */
    int64_t     to_me_ms;     /* their last message to you, 0 never */
    int64_t     to_ms;
    int64_t     you_ms;       /* the last report of them hearing you (PSK Reporter), 0 never */
    int         you_snr;
    bool        cq;           /* their last message was a CQ */
    bool        worked;       /* worked before (the list's check, on a CQ) */
    bool        has_cell;
    cell_data_t cell;         /* their last message, to call them with */
} station_t;

static station_t  st[ST_MAX];
static int        st_n;
static char       sel_call[13];
static char       qso_call[13]; /* your QSO */
static int64_t    qso_ms;
static char       title[32] = "FT8";
static map_view_actions_t act;
static int        you_min;      /* minutes PSK Reporter's lines stay, 0: off */
static bool       you_up;       /* PSK Reporter connected */

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* The first word of a message ("JA1XYZ VE7NHW R-12" -> "JA1XYZ"). */
static void first_word(const char *text, char *out, size_t size) {
    size_t n = 0;
    while (*text == ' ') text++;
    while (text[n] && text[n] != ' ' && n + 1 < size) n++;
    memcpy(out, text, n);
    out[n] = '\0';
}

static station_t *find(const char *call) {
    for (int i = 0; i < st_n; i++)
        if (strcasecmp(st[i].call, call) == 0) return &st[i];
    return NULL;
}

/* A station heard: updated, or added (the longest unheard one goes when full). */
static station_t *station(const char *call, int64_t now) {
    station_t *s = find(call);
    if (s) return s;
    if (st_n < ST_MAX) {
        s = &st[st_n++];
    } else {
        s = &st[0];
        for (int i = 1; i < st_n; i++)
            if (MAX(st[i].heard_ms, st[i].you_ms) < MAX(s->heard_ms, s->you_ms)) s = &st[i];
    }
    memset(s, 0, sizeof(*s));
    snprintf(s->call, sizeof(s->call), "%s", call);
    s->heard_ms = now;
    return s;
}

static void redraw(bool force);

void map_view_add(const cell_data_t *cell) {
    int64_t now = now_ms();
    char    word[13];
    switch (cell->cell_type) {
    case CELL_TX_MSG: /* "THEM ME ...": your QSO is with them */
        first_word(cell->text, word, sizeof(word));
        if (word[0] && strcasecmp(word, "CQ") != 0) {
            snprintf(qso_call, sizeof(qso_call), "%s", word);
            qso_ms = now;
        }
        break;
    case CELL_START_QSO: { /* "Start QSO with THEM" */
        const char *w = strrchr(cell->text, ' ');
        if (w && w[1]) {
            snprintf(qso_call, sizeof(qso_call), "%s", w + 1);
            qso_ms = now;
        }
        break;
    }
    case CELL_RX_CQ:
    case CELL_RX_TO_ME:
    case CELL_RX_MSG: {
        const ftx_msg_meta_t *m = &cell->meta;
        if (!m->call_de[0]) break;
        station_t *s = station(m->call_de, now);
        s->heard_ms  = now;
        s->snr       = m->local_snr;
        s->cq        = m->type == FTX_MSG_TYPE_CQ;
        if ((m->type == FTX_MSG_TYPE_CQ || m->type == FTX_MSG_TYPE_GRID) && strlen(m->grid) >= 4)
            snprintf(s->grid, sizeof(s->grid), "%s", m->grid);
        if (s->cq) s->worked = cell->worked_type != SEARCH_WORKED_NO;
        s->to[0] = '\0';
        if (!s->cq) {
            first_word(cell->text, s->to, sizeof(s->to));
            s->to_ms = now;
        }
        if (m->to_me) {
            s->to_me_ms = now;
            if (strcasecmp(qso_call, s->call) == 0) qso_ms = now;
        }
        s->cell     = *cell;
        s->has_cell = true;
        break;
    }
    default:
        return;
    }
}

void map_view_heard_you(const char *call, const char *grid, int snr, int64_t when) {
    int64_t now   = now_ms();
    int64_t age_s = MAX((int64_t)time(NULL) - when, 0);
    bool    known = find(call) != NULL;
    station_t *s  = station(call, now);
    if (!known) s->heard_ms = 0; /* not heard here */
    s->you_ms  = MAX(now - age_s * 1000, 1);
    s->you_snr = snr;
    snprintf(s->grid, sizeof(s->grid), "%s", grid);
}

void map_view_set_you_minutes(int minutes) {
    you_min = minutes;
    if (map_scene_shown()) redraw(false);
}

void map_view_set_you_up(bool up) {
    you_up = up;
    if (map_scene_shown()) redraw(false);
}

void map_view_forget_you(void) {
    for (int i = 0; i < st_n; i++) st[i].you_ms = 0;
    if (map_scene_shown()) redraw(true);
}

void map_view_clear(void) {
    st_n        = 0;
    sel_call[0] = '\0';
    qso_call[0] = '\0';
    if (map_scene_shown()) redraw(true);
}

void map_view_set_title(const char *t) {
    snprintf(title, sizeof(title), "%s", t);
    if (map_scene_shown()) redraw(false);
}

/* ---- The scene ---- */

static map_scene_station_t sc_st[ST_MAX];
static station_t          *sc_src[ST_MAX]; /* the station behind each one drawn */
static map_scene_link_t    sc_links[ST_MAX];
static map_scene_t         scene;
static char                my_call[16], my_grid[9], status[96], status2[48], status3[32], hint[80];

static const map_legend_item_t legend[] = {
    {MAP_SCENE_GREEN, MAP_LEGEND_SQUARE, "CQ"},
    {MAP_SCENE_YELLOW, MAP_LEGEND_SQUARE, "calling you"},
    {MAP_SCENE_GREY, MAP_LEGEND_SQUARE, "worked"},
    {MAP_SCENE_DIM, MAP_LEGEND_SQUARE, "other"},
    {MAP_SCENE_RED, MAP_LEGEND_LINE, "QSO"},
    {MAP_WIDGET_HOME, MAP_LEGEND_SQUARE, "you"},
    {MAP_SCENE_BLUE, MAP_LEGEND_LINE, "heard you"}, /* with PSK Reporter on and connected */
};

/* The scene from the stations heard in the last 15 min. */
static void build(int64_t now) {
    snprintf(my_call, sizeof(my_call), "%s", PARAM_T_GET(cfg.callsign()));
    snprintf(my_grid, sizeof(my_grid), "%s", PARAM_T_GET(cfg.qth()));
    bool qso_on = qso_call[0] && now - qso_ms < QSO_MS;
    int  n = 0, nhere = 0, ncq = 0, ncall = 0, nyou = 0, sel = -1;
    for (int i = 0; i < st_n; i++) {
        station_t *s = &st[i];
        bool here = s->heard_ms && now - s->heard_ms < HEARD_MS;
        bool you  = you_min && s->you_ms && now - s->you_ms < you_min * 60000LL;
        if (!here && !you) continue;
        map_scene_station_t *d = &sc_st[n];
        memset(d, 0, sizeof(*d));
        snprintf(d->call, sizeof(d->call), "%s", s->call);
        snprintf(d->grid, sizeof(d->grid), "%s", s->grid);
        d->snr       = s->snr;
        d->has_snr   = here;
        nhere += here;
        bool calling = here && s->to_me_ms && now - s->to_me_ms < CALLING_MS;
        /* Brightness by how well they hear you, as GridTracker's */
        lv_opa_t you_opa = (lv_opa_t)LV_CLAMP(YOU_OPA_MIN, (s->you_snr + 25) * 9, 255);
        if (!here) {
            d->color = MAP_SCENE_BLUE;
            d->opa   = you_opa;
        } else if (calling) {
            d->color = MAP_SCENE_YELLOW;
            d->label = true;
            ncall++;
        } else if (s->cq && s->worked) {
            d->color = MAP_SCENE_GREY;
        } else if (s->cq) {
            d->color = MAP_SCENE_GREEN;
            d->cq    = true;
            ncq++;
        } else {
            d->color = MAP_SCENE_DIM;
        }
        /* Fading over the 15 min, as the JS8 map's squares do. */
        if (here) {
            int age = (int)((now - s->heard_ms) * 8 / HEARD_MS);
            d->opa  = (lv_opa_t)(MAP_WIDGET_SQUARE_OPA * (100 - age * 70 / 8) / 100);
        }
        if (you) {
            d->path     = MAP_SCENE_BLUE;
            d->path_w   = 2;
            d->path_opa = you_opa;
            snprintf(d->extra, sizeof(d->extra), "hears you %+d", s->you_snr);
            nyou++;
        }
        if (qso_on && strcasecmp(s->call, qso_call) == 0) { /* your QSO's line, not the PSK Reporter one */
            d->path     = MAP_SCENE_RED;
            d->path_w   = 0;
            d->path_opa = 0;
        }
        if (sel_call[0] && strcasecmp(s->call, sel_call) == 0) sel = n;
        sc_src[n++] = s;
    }
    /* Others' QSOs: a grey line while both ends are on the map. */
    int nl = 0;
    for (int i = 0; i < n; i++) {
        station_t *s = sc_src[i];
        if (!s->to[0] || now - s->to_ms >= LINK_MS || strcasecmp(s->to, my_call) == 0) continue;
        for (int j = 0; j < n; j++)
            if (j != i && strcasecmp(sc_src[j]->call, s->to) == 0) {
                sc_links[nl++] = (map_scene_link_t){i, j, MAP_SCENE_LINK, 150};
                break;
            }
    }
    if (sel < 0) sel_call[0] = '\0';
    int    info  = param_i_get(cfg.ui.ft8_info());
    bool   miles = info == TABLE_INFO_MI || info == TABLE_INFO_COUNTRY_MI;
    snprintf(status, sizeof(status), "%s   %d heard  %d CQ  %d calling you", title, nhere, ncq, ncall);
    status3[0] = '\0';
    if (you_min && you_up) snprintf(status3, sizeof(status3), "%d heard you  (%d min)", nyou, you_min);
    else if (you_min) snprintf(status3, sizeof(status3), "Not connected to PSK Reporter");
    double best  = -1, km;
    int    dx    = -1;
    for (int i = 0; i < n; i++) /* the farthest heard: "DX ZL2AB 11300 km" */
        if (sc_st[i].has_snr && map_scene_km(my_call, my_grid, sc_st[i].call, sc_st[i].grid, &km) && km > best) {
            best = km;
            dx   = i;
        }
    status2[0] = '\0';
    if (dx >= 0)
        snprintf(status2, sizeof(status2), "DX %s %.0f %s", sc_st[dx].call, miles ? best * 0.621371 : best,
                 miles ? "mi" : "km");
    if (!n) snprintf(hint, sizeof(hint), "No stations heard in the last 15 min");
    else if (sel < 0) snprintf(hint, sizeof(hint), "Turn MFK: pick a station");
    else if (sc_st[sel].has_snr) snprintf(hint, sizeof(hint), "Press MFK: call");
    else hint[0] = '\0'; /* only they heard you: nothing decoded to answer */
    scene     = (map_scene_t){.st      = sc_st,
                              .n       = (unsigned)n,
                              .links   = sc_links,
                              .nlinks  = (unsigned)nl,
                              .sel     = sel,
                              .my_call = my_call,
                              .my_grid = my_grid,
                              .mode    = map_scene_mode(),
                              .miles   = miles,
                              .legend  = legend,
                              .nlegend = sizeof(legend) / sizeof(legend[0]) - (you_min && (you_up || nyou) ? 0 : 1),
                              .status  = status,
                              .status2 = status2,
                              .status3 = status3,
                              .status3_color = MAP_SCENE_BLUE,
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

/* MFK pressed: call the selected station, as pressing its row does. */
static void press(void) {
    station_t *s = sel_call[0] ? find(sel_call) : NULL;
    if (!s) {
        msg_update_text_fmt("Turn MFK to pick a station first");
        return;
    }
    if (s->has_cell && act.on_press) act.on_press(&s->cell);
}

bool map_view_show(lv_obj_t *parent, lv_obj_t *table, bool on, const map_view_actions_t *actions) {
    if (on) act = *actions;
    const map_scene_app_t app = {
        .hide          = act.waterfall,
        .redraw        = redraw,
        .on_step       = select_step,
        .on_press      = press,
        .on_close      = act.on_close,
        .on_vol_change = act.on_vol_change,
    };
    return map_scene_show(parent, table, on, &app);
}

bool map_view_shown(void) {
    return map_scene_shown();
}

void map_view_focus(void) {
    map_scene_focus();
}

const char *map_view_next_mode(void) {
    return map_scene_next_mode();
}

void map_view_destroy(void) {
    you_up = false;
    map_scene_destroy();
    st_n        = 0;
    sel_call[0] = '\0';
    qso_call[0] = '\0';
}
