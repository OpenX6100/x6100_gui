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
#include "../dialog.h"
#include "../keyboard.h"
#include "../map_scene.h"
#include "../msg.h"
#include "../events.h"
#include "../styles.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef FT8_MAP_DATA_PATH
#define FT8_MAP_DATA_PATH "/usr/share/x6100/js8/js8_map.bin" /* the JS8 map's data (tools/map_data) */
#endif
#ifndef FT8_CTY_PATH
#define FT8_CTY_PATH "/usr/share/x6100/cty.dat" /* the file the list's countries come from (dxcc.c) */
#endif
#define FT8_CTY_SD_PATH "/mnt/cty.dat" /* a newer list put on the card wins */

#define ST_MAX     100
#define HEARD_MS   (15 * 60 * 1000) /* stations heard in the last 15 min */
#define CALLING_MS (2 * 60 * 1000)  /* yellow for 2 min after their last message to you */
#define QSO_MS     (2 * 60 * 1000)  /* your QSO's red path, 2 min after its last message */
#define LINK_MS    (2 * 60 * 1000)  /* others' QSOs: grey lines for 2 min */
#define REDRAW_MS  1000

typedef struct {
    char        call[13];
    char        grid[9];
    char        to[13];       /* whom their last message was to ("" CQ) */
    int         snr;
    int64_t     heard_ms;     /* monotonic */
    int64_t     to_me_ms;     /* their last message to you, 0 never */
    int64_t     to_ms;
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
static bool       shown;
static lv_obj_t  *table_obj;
static lv_timer_t *timer;
static js8_map_mode_t     mode = JS8_MAP_AUTO;
static map_view_actions_t act;

static const char *const mode_names[3] = {"Auto", "Close-in", "World"};

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
            if (st[i].heard_ms < s->heard_ms) s = &st[i];
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
    if (shown) redraw(false);
}

void map_view_clear(void) {
    st_n        = 0;
    sel_call[0] = '\0';
    qso_call[0] = '\0';
    if (shown) redraw(true);
}

void map_view_set_title(const char *t) {
    snprintf(title, sizeof(title), "%s", t);
    if (shown) redraw(false);
}

/* ---- The scene ---- */

static map_scene_station_t sc_st[ST_MAX];
static station_t          *sc_src[ST_MAX]; /* the station behind each one drawn */
static map_scene_link_t    sc_links[ST_MAX];
static map_scene_t         scene;
static char                my_call[16], my_grid[9], status[96], hint[80];

static const map_legend_item_t legend[] = {
    {MAP_SCENE_GREEN, MAP_LEGEND_SQUARE, "CQ"},
    {MAP_SCENE_YELLOW, MAP_LEGEND_SQUARE, "calling you"},
    {MAP_SCENE_GREY, MAP_LEGEND_SQUARE, "worked"},
    {MAP_SCENE_DIM, MAP_LEGEND_SQUARE, "other"},
    {MAP_SCENE_RED, MAP_LEGEND_LINE, "QSO"},
    {MAP_WIDGET_HOME, MAP_LEGEND_SQUARE, "you"},
};

/* The scene from the stations heard in the last 15 min. */
static void build(int64_t now) {
    snprintf(my_call, sizeof(my_call), "%s", PARAM_T_GET(cfg.callsign()));
    snprintf(my_grid, sizeof(my_grid), "%s", PARAM_T_GET(cfg.qth()));
    bool qso_on = qso_call[0] && now - qso_ms < QSO_MS;
    int  n = 0, ncq = 0, ncall = 0, sel = -1;
    for (int i = 0; i < st_n; i++) {
        station_t *s = &st[i];
        if (now - s->heard_ms >= HEARD_MS) continue;
        map_scene_station_t *d = &sc_st[n];
        memset(d, 0, sizeof(*d));
        snprintf(d->call, sizeof(d->call), "%s", s->call);
        snprintf(d->grid, sizeof(d->grid), "%s", s->grid);
        d->snr       = s->snr;
        d->has_snr   = true;
        bool calling = s->to_me_ms && now - s->to_me_ms < CALLING_MS;
        if (calling) {
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
        int age = (int)((now - s->heard_ms) * 8 / HEARD_MS);
        d->opa  = (lv_opa_t)(MAP_WIDGET_SQUARE_OPA * (100 - age * 70 / 8) / 100);
        if (qso_on && strcasecmp(s->call, qso_call) == 0) d->path = MAP_SCENE_RED;
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
    snprintf(status, sizeof(status), "%s   %d heard  %d CQ  %d calling you", title, n, ncq, ncall);
    if (!n) snprintf(hint, sizeof(hint), "No stations heard in the last 15 min");
    else if (sel < 0) snprintf(hint, sizeof(hint), "Turn MFK: pick a station");
    else snprintf(hint, sizeof(hint), "Turn MFK: pick a station    Press MFK: call %s", sel_call);
    int info  = param_i_get(cfg.ui.ft8_info());
    scene     = (map_scene_t){.st      = sc_st,
                              .n       = (unsigned)n,
                              .links   = sc_links,
                              .nlinks  = (unsigned)nl,
                              .sel     = sel,
                              .my_call = my_call,
                              .my_grid = my_grid,
                              .mode    = mode,
                              .miles   = info == TABLE_INFO_MI || info == TABLE_INFO_COUNTRY_MI,
                              .legend  = legend,
                              .nlegend = sizeof(legend) / sizeof(legend[0]),
                              .status  = status,
                              .hint    = hint};
}

static void redraw(bool force) {
    build(now_ms());
    map_scene_draw(&scene, force);
}

static void timer_cb(lv_timer_t *t) {
    (void)t;
    redraw(false);
}

/* ---- Keys ---- */

static void select_step(int dir) {
    int i = map_scene_step(&scene, scene.sel, dir);
    if (i < 0) return;
    snprintf(sel_call, sizeof(sel_call), "%s", sc_st[i].call);
    redraw(false);
}

static void key_cb(lv_event_t *e) {
    uint32_t key = *((uint32_t *)lv_event_get_param(e));
    switch (key) {
    case LV_KEY_RIGHT:
    case LV_KEY_DOWN:
        select_step(1);
        break;
    case LV_KEY_LEFT:
    case LV_KEY_UP:
        select_step(-1);
        break;
    case LV_KEY_ESC:
        /* Closing deletes this focused canvas: the held key mustn't turn
         * into a long press in whatever gets the focus next. */
        if (lv_indev_get_act()) lv_indev_wait_release(lv_indev_get_act());
        if (act.on_close) act.on_close();
        break;
    case KEY_VOL_LEFT_EDIT:
    case KEY_VOL_LEFT_SELECT:
        if (act.on_vol_change) act.on_vol_change(-1);
        break;
    case KEY_VOL_RIGHT_EDIT:
    case KEY_VOL_RIGHT_SELECT:
        if (act.on_vol_change) act.on_vol_change(1);
        break;
    }
}

/* MFK pressed: call the selected station, as pressing its row does. */
static void pressed_cb(lv_event_t *e) {
    (void)e;
    station_t *s = sel_call[0] ? find(sel_call) : NULL;
    if (!s) {
        msg_update_text_fmt("Turn MFK to pick a station first");
        return;
    }
    if (!s->has_cell || !act.on_press) return;
    act.on_press(&s->cell);
}

/* ---- Showing ---- */

static bool make(lv_obj_t *parent) {
    if (map_widget_made()) return true;
    /* The whole inside of the window's frame, as the JS8 map. */
    int                     border = param_i_get(cfg.ui.theme()) == THEME_BLACK ? 0 : 1;
    int                     w = DIALOG_WIDTH - 2 * border, h = DIALOG_HEIGHT - 2 * border;
    const map_widget_geom_t g = {border, border, w, h, h - MAP_SCENE_BOTTOM_H, h - MAP_SCENE_BOTTOM_H};
    if (!map_widget_create(parent, &g, FT8_MAP_DATA_PATH, FT8_CTY_SD_PATH, FT8_CTY_PATH)) return false;
    lv_obj_t *canvas = map_widget_canvas();
    lv_obj_add_event_cb(canvas, key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(canvas, pressed_cb, LV_EVENT_PRESSED, NULL);
    return true;
}

bool map_view_show(lv_obj_t *parent, lv_obj_t *table, bool on, const map_view_actions_t *actions) {
    if (on == shown) return true;
    if (on) {
        if (!make(parent)) {
            msg_update_text_fmt("Map: not enough memory");
            return false;
        }
        act       = *actions;
        table_obj = table;
        shown     = true;
        map_widget_view_reset();
        map_widget_show(true);
        /* The MFK goes to the map while it shows. */
        lv_obj_t *canvas = map_widget_canvas();
        if (table_obj) lv_group_remove_obj(table_obj);
        lv_group_add_obj(keyboard_group, canvas);
        lv_group_focus_obj(canvas);
        lv_group_set_editing(keyboard_group, true);
        redraw(true);
        if (!timer) timer = lv_timer_create(timer_cb, REDRAW_MS, NULL);
        else lv_timer_resume(timer);
    } else {
        shown = false;
        if (timer) lv_timer_pause(timer);
        lv_group_remove_obj(map_widget_canvas());
        map_widget_show(false);
        if (table_obj) {
            lv_group_add_obj(keyboard_group, table_obj);
            lv_group_focus_obj(table_obj);
            lv_group_set_editing(keyboard_group, true);
        }
    }
    return true;
}

bool map_view_shown(void) {
    return shown;
}

const char *map_view_next_mode(void) {
    mode = (js8_map_mode_t)((mode + 1) % 3);
    if (shown) redraw(true);
    return mode_names[mode];
}

void map_view_destroy(void) {
    if (timer) lv_timer_del(timer);
    timer     = NULL;
    shown     = false;
    table_obj = NULL;
    map_widget_free(); /* its objects go with the window */
    st_n        = 0;
    sel_call[0] = '\0';
    qso_call[0] = '\0';
}
