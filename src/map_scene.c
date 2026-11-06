/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *  Copyright (c) 2026 Randal Warren VE7NHW
 *  Part of OpenX6100 (github.com/OpenX6100)
 *
 *  Xiegu X6100 LVGL GUI - a station map in one call (see map_scene.h)
 */

#include "map_scene.h"

#include "styles.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCENE_MAX   128 /* stations drawn */
#define STACK_PX    2   /* marks this close are on the same spot: one count tag */

static uint32_t        scene_sig;
static int             scene_n;               /* as last drawn */
static bool            scene_shown[SCENE_MAX];
static int             scene_cx[SCENE_MAX], scene_cy[SCENE_MAX];
static js8_map_place_t scene_pl[SCENE_MAX];

static uint32_t hash_str(uint32_t h, const char *s) {
    return map_widget_hash(h, s ? s : "", s ? strlen(s) : 0);
}

void map_scene_distance(const map_scene_t *sc, int i, char *buf, size_t size) {
    js8_map_place_t home;
    buf[0] = '\0';
    if (i < 0 || i >= scene_n || !scene_shown[i] || !js8_map_place(sc->my_call, sc->my_grid, &home)) return;
    double km = js8_map_distance_km(home.lat, home.lon, scene_pl[i].lat, scene_pl[i].lon);
    snprintf(buf, size, "  %.0f %s", sc->miles ? km * 0.621371 : km, sc->miles ? "mi" : "km");
}

/* The status line, top right, on a dark box. */
static void draw_status(const char *text) {
    if (!text || !text[0]) return;
    const map_widget_geom_t *g = map_widget_geom();
    lv_point_t               sz;
    lv_txt_get_size(&sz, text, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int x = g->w - 12 - sz.x, y = 6;
    map_widget_rect(x - 4, y - 2, sz.x + 8, sz.y + 4, 0x000000, LV_OPA_50, 0, 0, 3);
    map_widget_take(x - 4, y - 2, g->w, y + sz.y + 2);
    map_widget_text(x, y, sz.x + 4, &sony_18, 0xFFFFFF, text);
}

/* The hint line, bottom left above the legend. */
static void draw_hint(const char *text, int y0) {
    if (!text || !text[0]) return;
    lv_point_t sz;
    lv_txt_get_size(&sz, text, &sony_18, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int cy = y0 + MAP_SCENE_HINT_H / 2;
    map_widget_rect(4, y0 + 2, sz.x + 8, MAP_SCENE_HINT_H - 3, 0x000000, LV_OPA_50, 0, 0, 0);
    map_widget_take(4, y0 + 2, 12 + sz.x, y0 + MAP_SCENE_HINT_H);
    map_widget_text(8, cy - sz.y / 2, sz.x + 4, &sony_18, 0xFFFFFF, text);
}

void map_scene_draw(const map_scene_t *sc, bool force) {
    if (!map_widget_made()) return;
    const map_widget_geom_t *g = map_widget_geom();
    unsigned                 n = sc->n < SCENE_MAX ? sc->n : SCENE_MAX;

    js8_map_place_t home;
    bool            have_home = js8_map_place(sc->my_call, sc->my_grid, &home);
    char            my_cont[3] = "";
    js8_map_my_continent(sc->my_call, sc->my_grid, my_cont);

    static js8_map_point_t pts[SCENE_MAX];
    unsigned               np = 0;
    for (unsigned i = 0; i < n; i++) {
        scene_shown[i] = js8_map_place(sc->st[i].call, sc->st[i].grid, &scene_pl[i]);
        if (!scene_shown[i]) continue;
        pts[np].lat = scene_pl[i].lat;
        pts[np].lon = scene_pl[i].lon;
        memcpy(pts[np].continent, scene_pl[i].continent, sizeof(pts[np].continent));
        np++;
    }
    scene_n = (int)n;
    bool world;
    bool base_new = map_widget_fit(sc->mode, have_home ? home.lat : 0, have_home ? home.lon : 0, my_cont, pts, np,
                                   &world);

    /* Everything drawn below, hashed: unchanged means nothing to redraw. */
    uint32_t sig = MAP_WIDGET_HASH_START;
    sig          = map_widget_hash(sig, &n, sizeof(n));
    for (unsigned i = 0; i < n; i++) { /* field by field: no padding bytes */
        const map_scene_station_t *s = &sc->st[i];
        sig                          = hash_str(sig, s->call);
        sig                          = hash_str(sig, s->grid);
        sig                          = hash_str(sig, s->tag);
        sig                          = map_widget_hash(sig, &s->color, sizeof(s->color));
        sig                          = map_widget_hash(sig, &s->opa, sizeof(s->opa));
        sig                          = map_widget_hash(sig, &s->snr, sizeof(s->snr));
        sig                          = map_widget_hash(sig, &s->has_snr, sizeof(s->has_snr));
        sig                          = map_widget_hash(sig, &s->cq, sizeof(s->cq));
        sig                          = map_widget_hash(sig, &s->label, sizeof(s->label));
        sig                          = map_widget_hash(sig, &s->path, sizeof(s->path));
        sig                          = map_widget_hash(sig, &s->path_w, sizeof(s->path_w));
    }
    sig = map_widget_hash(sig, &sc->nlinks, sizeof(sc->nlinks));
    for (unsigned i = 0; i < sc->nlinks; i++) {
        sig = map_widget_hash(sig, &sc->links[i].a, sizeof(sc->links[i].a));
        sig = map_widget_hash(sig, &sc->links[i].b, sizeof(sc->links[i].b));
        sig = map_widget_hash(sig, &sc->links[i].color, sizeof(sc->links[i].color));
        sig = map_widget_hash(sig, &sc->links[i].opa, sizeof(sc->links[i].opa));
    }
    sig = map_widget_hash(sig, &sc->sel, sizeof(sc->sel));
    sig = map_widget_hash(sig, &sc->miles, sizeof(sc->miles));
    sig = hash_str(sig, sc->my_call);
    sig = hash_str(sig, sc->my_grid);
    sig = hash_str(sig, sc->status);
    sig = hash_str(sig, sc->hint);
    sig = map_widget_hash(sig, &sc->mode, sizeof(sc->mode));
    if (!force && !base_new && sig == scene_sig) return;
    scene_sig = sig;

    map_widget_begin();

    /* The legend, its right end naming the view, and the hint line above it. */
    int  legend_y = g->h - MAP_SCENE_LEGEND_H - 2;
    char right[48];
    if (!have_home) snprintf(right, sizeof(right), "Set your grid: APP > QTH");
    else if (sc->mode == JS8_MAP_WORLD) snprintf(right, sizeof(right), "World");
    else if (world) snprintf(right, sizeof(right), "World (DX heard)");
    else snprintf(right, sizeof(right), "%s%s", map_widget_continent_name(my_cont),
                  sc->mode == JS8_MAP_CLOSE ? " (close-in)" : "");
    map_widget_legend(sc->legend, sc->nlegend, right, legend_y, MAP_SCENE_LEGEND_H);
    draw_hint(sc->hint, legend_y - MAP_SCENE_HINT_H);
    draw_status(sc->status);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.round_start = 1;
    line.round_end   = 1;
    lv_point_t p[MAP_WIDGET_PATH_N + 1];

    /* Lines between stations, thin, under everything. */
    line.width = 2;
    for (unsigned k = 0; k < sc->nlinks; k++) {
        int a = sc->links[k].a, b = sc->links[k].b;
        if (a < 0 || b < 0 || a >= (int)n || b >= (int)n || !scene_shown[a] || !scene_shown[b]) continue;
        map_widget_path_points(scene_pl[a].lat, scene_pl[a].lon, scene_pl[b].lat, scene_pl[b].lon, p);
        line.color = lv_color_hex(sc->links[k].color);
        line.opa   = sc->links[k].opa ? sc->links[k].opa : 190;
        map_widget_line(p, MAP_WIDGET_PATH_N + 1, &line);
    }

    /* Paths from you; the red ones (your QSO) last, on top. */
    line.opa = LV_OPA_COVER;
    for (int pass = 0; have_home && pass < 2; pass++)
        for (unsigned i = 0; i < n; i++) {
            const map_scene_station_t *s = &sc->st[i];
            if (!scene_shown[i] || !s->path || (s->path == MAP_SCENE_RED) != (pass == 1)) continue;
            map_widget_path_points(home.lat, home.lon, scene_pl[i].lat, scene_pl[i].lon, p);
            line.color = lv_color_hex(s->path);
            line.width = s->path_w ? s->path_w : 3;
            map_widget_line(p, MAP_WIDGET_PATH_N + 1, &line);
        }

    /* The stations. */
    static int side[SCENE_MAX];
    for (unsigned i = 0; i < n; i++) {
        if (!scene_shown[i]) continue;
        const map_scene_station_t *s   = &sc->st[i];
        bool                       sel = (int)i == sc->sel;
        lv_opa_t                   opa = s->opa ? s->opa : MAP_WIDGET_SQUARE_OPA;
        side[i] = map_widget_station_mark(s->grid, &scene_pl[i], s->color, sel ? 200 : opa, s->color, LV_OPA_COVER, 0,
                                          &scene_cx[i], &scene_cy[i]);
    }

    /* You, with your call. */
    if (have_home) {
        int             hx, hy;
        js8_map_place_t me = home;
        me.approx          = strlen(sc->my_grid) < 4;
        map_widget_station_mark(sc->my_grid, &me, MAP_WIDGET_HOME, 220, MAP_WIDGET_HOME, LV_OPA_COVER, 0, &hx, &hy);
        map_widget_take(hx - 6, hy - 6, hx + 6, hy + 6); /* no label over you */
        map_widget_label(hx, hy, sc->my_call, MAP_WIDGET_HOME, 0x000000, 170, 8, true);
    }

    /* Tags reserved before the labels, drawn last: CQ on the top right,
     * the app's tag, else a count where stations share a spot, on the
     * bottom right. */
    static bool      cq_tag[SCENE_MAX], low_tag[SCENE_MAX];
    static lv_area_t cq_box[SCENE_MAX], low_box[SCENE_MAX];
    static char      low_text[SCENE_MAX][8];
    for (unsigned i = 0; i < n; i++) {
        bool sel  = (int)i == sc->sel;
        cq_tag[i] = scene_shown[i] && sc->st[i].cq &&
                    map_widget_badge_box(scene_cx[i], scene_cy[i], sel ? 19 : side[i], "CQ", &cq_box[i]);
        low_tag[i] = false;
        if (!scene_shown[i]) continue;
        if (sc->st[i].tag[0]) {
            snprintf(low_text[i], sizeof(low_text[i]), "%s", sc->st[i].tag);
            low_tag[i] = map_widget_badge_box_at(scene_cx[i], scene_cy[i], side[i], low_text[i], true, &low_box[i]);
            continue;
        }
        bool first = true;
        int  cnt = 0, gside = 0;
        for (unsigned j = 0; j < n && first; j++) {
            if (!scene_shown[j] || abs(scene_cx[j] - scene_cx[i]) > STACK_PX || abs(scene_cy[j] - scene_cy[i]) > STACK_PX)
                continue;
            if (j < i) first = false;
            cnt++;
            gside = LV_MAX(gside, (int)j == sc->sel ? 19 : side[j]);
        }
        if (!first || cnt < 2) continue;
        snprintf(low_text[i], sizeof(low_text[i]), "%d", cnt);
        low_tag[i] = map_widget_badge_box_at(scene_cx[i], scene_cy[i], gside, low_text[i], true, &low_box[i]);
    }

    /* The selected station: red box, its details. */
    if (sc->sel >= 0 && sc->sel < (int)n && scene_shown[sc->sel]) {
        int                        i = sc->sel;
        const map_scene_station_t *s = &sc->st[i];
        map_widget_rect(scene_cx[i] - 9, scene_cy[i] - 9, 19, 19, 0, LV_OPA_TRANSP, MAP_WIDGET_SELECT, 2, 0);
        map_widget_take(scene_cx[i] - 10, scene_cy[i] - 10, scene_cx[i] + 10, scene_cy[i] + 10);
        char text[112];
        int  len = snprintf(text, sizeof(text), "%s  %s", s->call, scene_pl[i].where);
        if (s->has_snr && len < (int)sizeof(text)) len += snprintf(text + len, sizeof(text) - len, "  %+d dB", s->snr);
        if (have_home && len < (int)sizeof(text)) {
            char dist[24];
            map_scene_distance(sc, i, dist, sizeof(dist));
            len += snprintf(text + len, sizeof(text) - len, "%s", dist);
            if (len < (int)sizeof(text))
                snprintf(text + len, sizeof(text) - len, "  az %.0f",
                         js8_map_bearing_deg(home.lat, home.lon, scene_pl[i].lat, scene_pl[i].lon));
        }
        map_widget_label(scene_cx[i], scene_cy[i], text, 0xFFFFFF, 0x6E0000, 225, 13, true);
    }

    /* Calls beside the stations that ask for it, where they fit. */
    for (unsigned i = 0; i < n; i++)
        if (scene_shown[i] && sc->st[i].label && (int)i != sc->sel)
            map_widget_label(scene_cx[i], scene_cy[i], sc->st[i].call, sc->st[i].color, 0x141416, 200, 13, false);

    for (unsigned i = 0; i < n; i++) {
        if (cq_tag[i]) map_widget_badge(&cq_box[i], "CQ");
        if (low_tag[i]) map_widget_badge_ex(&low_box[i], low_text[i], 0xFFFFFF, 0x000000);
    }
    map_widget_end();
}

int map_scene_step(const map_scene_t *sc, int sel, int dir) {
    (void)sc;
    int best = -1;
    /* Order: x, then y, then the index (stations on one spot in list order). */
#define BEFORE(a, b)                                                                                                   \
    (scene_cx[a] != scene_cx[b] ? scene_cx[a] < scene_cx[b]                                                            \
     : scene_cy[a] != scene_cy[b] ? scene_cy[a] < scene_cy[b]                                                          \
                                  : (a) < (b))
    bool from = sel >= 0 && sel < scene_n && scene_shown[sel];
    for (int i = 0; i < scene_n; i++) {
        if (!scene_shown[i] || i == sel) continue;
        if (dir > 0) {
            if (from && !BEFORE(sel, i)) continue;
            if (best < 0 || BEFORE(i, best)) best = i;
        } else {
            if (from && !BEFORE(i, sel)) continue;
            if (best < 0 || BEFORE(best, i)) best = i;
        }
    }
#undef BEFORE
    return best < 0 && from ? sel : best;
}
